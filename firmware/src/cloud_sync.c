#include "cloud_sync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "device_state.h"
#include "pins_config.h"

static const char *TAG = "CLOUD";

#define CLOUD_TASK_STACK 8192
#define CLOUD_TASK_PRIORITY 4
#define CLOUD_QUEUE_LEN 4

// =========================================================
// SOS MESSAGES
// =========================================================

typedef enum
{
    SOS_MSG_TRIGGER = 0,
    SOS_MSG_UPDATE
} sos_msg_type_t;

typedef struct
{
    sos_msg_type_t type;
    sos_location_source_t source;
    double lat;
    double lon;
    bool sms_sent;
    bool call_started;
    char event_id[12];
} sos_msg_t;

typedef struct
{
    bool pending;
    sos_msg_t msg;
} sos_pending_t;

static QueueHandle_t s_queue = NULL;

// =========================================================
// WI-FI STATE
// associated: linked to the access point; connected: has an IP address.
// Reconnects are scheduled by the cloud task with exponential back-off and
// never interrupt an attempt or DHCP that is still in progress.
// =========================================================

static bool s_wifi_configured = false;
static volatile bool s_wifi_associated = false;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_connecting = false;
static volatile int64_t s_wifi_assoc_ms = 0;
static volatile int64_t s_wifi_next_attempt_ms = 0;
static volatile uint32_t s_wifi_backoff_ms = WIFI_RECONNECT_MIN_BACKOFF_MS;
static volatile uint32_t s_wifi_reconnects = 0;

static esp_http_client_handle_t s_http = NULL;

// Upload statistics, reported in /live for diagnostics
static uint32_t s_uploads_ok = 0;
static uint32_t s_uploads_failed = 0;

// =========================================================
// JSON BUILDER
// Minimal bounded writer; output is truncated-safe and flagged on overflow.
// Supports one level of nested objects.
// =========================================================

typedef struct
{
    char *buf;
    size_t size;
    size_t len;
    bool first;
    bool ok;
} json_t;

static void json_raw(json_t *j, const char *fmt, ...)
{
    if (!j->ok)
        return;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, args);
    va_end(args);

    if (n < 0 || (size_t)n >= j->size - j->len)
    {
        j->ok = false;
        return;
    }
    j->len += (size_t)n;
}

static void json_begin(json_t *j, char *buf, size_t size)
{
    j->buf = buf;
    j->size = size;
    j->len = 0;
    j->first = true;
    j->ok = true;
    json_raw(j, "{");
}

static void json_key(json_t *j, const char *key)
{
    json_raw(j, "%s\"%s\":", j->first ? "" : ",", key);
    j->first = false;
}

static void json_obj_begin(json_t *j, const char *key)
{
    json_key(j, key);
    json_raw(j, "{");
    j->first = true;
}

static void json_obj_end(json_t *j)
{
    json_raw(j, "}");
    j->first = false;
}

static void json_str(json_t *j, const char *key, const char *value)
{
    json_key(j, key);
    json_raw(j, "\"");
    for (const char *p = value; p && *p; ++p)
    {
        if (*p == '"' || *p == '\\')
            json_raw(j, "\\%c", *p);
        else if ((unsigned char)*p >= 0x20)
            json_raw(j, "%c", *p);
    }
    json_raw(j, "\"");
}

static void json_int(json_t *j, const char *key, long long value)
{
    json_key(j, key);
    json_raw(j, "%lld", value);
}

static void json_num(json_t *j, const char *key, double value, int decimals)
{
    json_key(j, key);
    json_raw(j, "%.*f", decimals, value);
}

static void json_bool(json_t *j, const char *key, bool value)
{
    json_key(j, key);
    json_raw(j, value ? "true" : "false");
}

static void json_null(json_t *j, const char *key)
{
    json_key(j, key);
    json_raw(j, "null");
}

static void json_server_time(json_t *j, const char *key)
{
    json_key(j, key);
    json_raw(j, "{\".sv\":\"timestamp\"}");
}

static bool json_end(json_t *j)
{
    json_raw(j, "}");
    return j->ok;
}

// =========================================================
// HELPERS
// =========================================================

static const char *csq_label(int csq)
{
    if (csq < 0 || csq == 99)
        return "Unknown";
    if (csq >= 20)
        return "Excellent";
    if (csq >= 14)
        return "Good";
    if (csq >= 8)
        return "Fair";
    return "Weak";
}

static const char *cellular_label(const device_state_t *s)
{
    if (!s->modem_responding)
        return "Modem offline";
    if (!s->sim_ready)
    {
        if (strncmp(s->sim_status, "SIM PIN", 7) == 0 || strncmp(s->sim_status, "SIM PUK", 7) == 0)
            return "SIM locked";
        if (strcmp(s->sim_status, "NOT INSERTED") == 0 || s->sim_status[0] == '\0')
            return "No SIM";
        return "SIM error";
    }
    if (!s->gsm_registered)
        return "Searching";
    return csq_label(s->gsm_csq);
}

static const char *sos_source_name(sos_location_source_t source)
{
    switch (source)
    {
        case SOS_LOCATION_CURRENT:
            return "current";
        case SOS_LOCATION_LAST_KNOWN:
            return "last_known";
        default:
            return "none";
    }
}

// =========================================================
// TIME (SNTP)
// Real time is needed to create chronologically ordered history keys on
// the device; record timestamps themselves always use Firebase server time.
// =========================================================

static bool time_is_synced(void)
{
    return time(NULL) > 1700000000; // after Nov 2023
}

static int64_t epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Firebase-compatible push ID: 8 time characters + 12 random, so device keys
// sort in time order alongside keys created by POST.
static void make_push_id(char out[21], int64_t now_ms)
{
    static const char chars[] =
        "-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";

    for (int i = 7; i >= 0; i--)
    {
        out[i] = chars[now_ms % 64];
        now_ms /= 64;
    }
    for (int i = 8; i < 20; i++)
    {
        out[i] = chars[esp_random() % 64];
    }
    out[20] = '\0';
}

// =========================================================
// WI-FI
// =========================================================

static void schedule_reconnect(void)
{
    s_wifi_next_attempt_ms = device_uptime_ms() + s_wifi_backoff_ms;
    s_wifi_backoff_ms = (s_wifi_backoff_ms * 2 > WIFI_RECONNECT_MAX_BACKOFF_MS)
                            ? WIFI_RECONNECT_MAX_BACKOFF_MS
                            : s_wifi_backoff_ms * 2;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        s_wifi_connecting = true;
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        s_wifi_connecting = false;
        s_wifi_associated = true;
        s_wifi_assoc_ms = device_uptime_ms();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        // Common reasons: 15/204 wrong password, 201 SSID not found (or 5 GHz only).
        ESP_LOGW(TAG, "Wi-Fi %s (reason %d). Retrying in %lu ms.",
                 s_wifi_connected ? "disconnected" : "connect failed", event->reason,
                 (unsigned long)s_wifi_backoff_ms);
        s_wifi_connecting = false;
        s_wifi_associated = false;
        s_wifi_connected = false;
        device_state_set_wifi(false, 0);
        schedule_reconnect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wifi_backoff_ms = WIFI_RECONNECT_MIN_BACKOFF_MS;
        device_state_set_wifi(true, 0);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP)
    {
        ESP_LOGW(TAG, "Wi-Fi lost its IP address.");
        s_wifi_connected = false;
        s_wifi_assoc_ms = device_uptime_ms(); // DHCP grace period starts now
        device_state_set_wifi(false, 0);
    }
}

static void wifi_init(void)
{
    if (WIFI_SSID[0] == '\0')
    {
        ESP_LOGE(TAG, "WIFI_SSID is empty. Set it in pins_config.local.h; cloud uploads are disabled.");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            // Open networks have an empty password; otherwise require WPA2+.
            .threshold.authmode = (WIFI_PASS[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_POWER_SAVE ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));

    // Starts syncing as soon as the network is up; retries automatically.
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    esp_netif_sntp_init(&sntp_config);

    s_wifi_configured = true;
    ESP_LOGI(TAG, "Wi-Fi station started for SSID \"%s\" (power save %s)",
             WIFI_SSID, WIFI_POWER_SAVE ? "on" : "off");
}

// Called every loop of the cloud task while there is no IP address.
static void wifi_supervise(int64_t now)
{
    if (s_wifi_associated)
    {
        // Linked but DHCP never completed: drop and retry.
        if (now - s_wifi_assoc_ms > WIFI_DHCP_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "No IP address after %d s; reconnecting.", WIFI_DHCP_TIMEOUT_MS / 1000);
            s_wifi_assoc_ms = now;
            esp_wifi_disconnect(); // raises DISCONNECTED -> back-off -> retry
        }
        return;
    }

    if (!s_wifi_connecting && now >= s_wifi_next_attempt_ms)
    {
        s_wifi_connecting = true;
        s_wifi_reconnects++;
        if (esp_wifi_connect() != ESP_OK)
        {
            s_wifi_connecting = false;
            schedule_reconnect();
        }
    }
}

static void wifi_refresh_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        device_state_set_wifi(true, ap.rssi);
    }
}

// =========================================================
// HTTPS (persistent connection, reused between requests)
// =========================================================

static void http_reset(void)
{
    if (s_http != NULL)
    {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
}

static const char *method_name(esp_http_client_method_t method)
{
    switch (method)
    {
        case HTTP_METHOD_PUT:
            return "PUT";
        case HTTP_METHOD_POST:
            return "POST";
        case HTTP_METHOD_PATCH:
            return "PATCH";
        default:
            return "REQ";
    }
}

// One attempt. Returns HTTP status, or -1 on a transport error.
static int http_perform(esp_http_client_method_t method, const char *url,
                        const char *body, int64_t *elapsed_ms)
{
    if (s_http == NULL)
    {
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = CLOUD_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_http = esp_http_client_init(&config);
        if (s_http == NULL)
            return -1;
    }
    else
    {
        esp_http_client_set_url(s_http, url);
    }

    esp_http_client_set_method(s_http, method);
    esp_http_client_set_header(s_http, "Content-Type", "application/json");
    esp_http_client_set_post_field(s_http, body, (int)strlen(body));

    int64_t started = device_uptime_ms();
    esp_err_t err = esp_http_client_perform(s_http);
    *elapsed_ms = device_uptime_ms() - started;

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s transport error: %s (%lld ms)",
                 method_name(method), esp_err_to_name(err), *elapsed_ms);
        http_reset();
        return -1;
    }

    return esp_http_client_get_status_code(s_http);
}

// `node` is relative to devices/<id>; NULL addresses the device root.
// Returns true on HTTP 2xx. A transport error (typically a keep-alive
// connection the server already closed) is retried once on a new connection.
static bool cloud_request(esp_http_client_method_t method,
                          const char *node,
                          const char *body)
{
    if (!s_wifi_connected)
        return false;

    char url[256];
    // print=silent: Firebase answers 204 without echoing the body.
    int n = snprintf(url, sizeof(url), "%s/devices/%s%s%s.json?print=silent",
                     CLOUD_API_BASE_URL, DEVICE_ID, node ? "/" : "", node ? node : "");
    if (n < 0 || (size_t)n >= sizeof(url))
    {
        ESP_LOGE(TAG, "URL too long for node %s", node ? node : "(root)");
        return false;
    }

    const char *label = node ? node : "device";
    int64_t elapsed = 0;
    int status = http_perform(method, url, body, &elapsed);

    if (status < 0 && s_wifi_connected)
    {
        status = http_perform(method, url, body, &elapsed);
    }

    if (status >= 200 && status < 300)
    {
        ESP_LOGI(TAG, "%s %s -> %d (%lld ms)", method_name(method), label, status, elapsed);
        s_uploads_ok++;
        device_state_mark_cloud_ok();
        return true;
    }

    s_uploads_failed++;

    if (status == 401 || status == 403)
    {
        ESP_LOGE(TAG, "%s %s -> %d: Firebase rules deny device writes to devices/%s",
                 method_name(method), label, status, DEVICE_ID);
    }
    else if (status > 0)
    {
        ESP_LOGW(TAG, "%s %s -> HTTP %d", method_name(method), label, status);
    }
    return false;
}

// =========================================================
// PAYLOADS
// =========================================================

static void add_live_fields(json_t *j, const device_state_t *s, int64_t now)
{
    bool fix_fresh = device_state_fix_is_fresh(s, now);

    json_str(j, "status", "Online");
    json_str(j, "deviceName", DEVICE_NAME);
    json_str(j, "fw", FIRMWARE_VERSION);
    json_int(j, "uptimeSec", now / 1000);
    json_int(j, "heartbeatSec", CLOUD_HEARTBEAT_MS / 1000);

    // Network
    json_str(j, "network", "Wi-Fi");
    json_int(j, "wifiRssi", s->wifi_rssi);

    // GPS — last known location is always reported; gpsFix says if it is current
    json_bool(j, "gpsEnabled", s->gps_enabled);
    json_bool(j, "gpsFix", fix_fresh);
    if (s->has_location)
    {
        json_num(j, "lat", s->latitude, 7);
        json_num(j, "lng", s->longitude, 7);
        json_int(j, "fixAgeSec", (now - s->fix_time_ms) / 1000);
    }
    if (fix_fresh && s->speed_valid)
        json_num(j, "speed", s->speed_kmh, 1);
    else
        json_null(j, "speed");
    if (fix_fresh && s->course_deg >= 0.0)
        json_num(j, "course", s->course_deg, 0);
    if (s->satellites >= 0)
        json_int(j, "satellites", s->satellites);
    if (s->hdop > 0.0)
        json_num(j, "hdop", s->hdop, 1);

    // Supply
    if (s->battery_pct >= 0)
        json_int(j, "battery", s->battery_pct);
    else
        json_null(j, "battery");
    if (s->battery_mv > 0)
        json_int(j, "batteryMv", s->battery_mv);

    // Cellular
    json_bool(j, "modemOk", s->modem_responding);
    json_bool(j, "simReady", s->sim_ready);
    if (s->sim_status[0] != '\0')
        json_str(j, "simStatus", s->sim_status);
    json_bool(j, "gsmRegistered", s->gsm_registered);
    if (s->gsm_csq >= 0 && s->gsm_csq != 99)
        json_int(j, "gsmCsq", s->gsm_csq);
    json_str(j, "signal", cellular_label(s));

    json_bool(j, "sosActive", s->sos_active);

    // Diagnostics
    json_obj_begin(j, "diag");
    json_str(j, "resetReason", s->reset_reason ? s->reset_reason : "unknown");
    json_int(j, "bootCount", s->boot_count);
    json_int(j, "heapFree", esp_get_free_heap_size());
    json_int(j, "heapMin", esp_get_minimum_free_heap_size());
    json_int(j, "uploadsOk", s_uploads_ok);
    json_int(j, "uploadsFailed", s_uploads_failed);
    json_int(j, "wifiReconnects", s_wifi_reconnects);
    json_bool(j, "timeSynced", time_is_synced());
    json_obj_end(j);

    json_server_time(j, "timestamp");
}

static void add_history_fields(json_t *j, const device_state_t *s)
{
    json_num(j, "lat", s->latitude, 7);
    json_num(j, "lng", s->longitude, 7);
    if (s->speed_valid)
        json_num(j, "speed", s->speed_kmh, 1);
    if (s->hdop > 0.0)
        json_num(j, "hdop", s->hdop, 1);
    if (s->battery_pct >= 0)
        json_int(j, "battery", s->battery_pct);
    json_server_time(j, "timestamp");
}

// True when the current fix should be appended to /history.
static bool fix_belongs_in_history(const device_state_t *s, int64_t now)
{
    return device_state_fix_is_fresh(s, now) &&
           (s->hdop <= 0.0 || s->hdop <= GPS_MAX_HDOP_FOR_TRAIL);
}

// Heartbeat, optionally with a history entry. With synced time both go in a
// single multi-path PATCH; otherwise live is PUT and history POSTed.
// Returns true when the live record was written; *history_written reports
// the history part.
static bool send_update(const device_state_t *s, bool with_history, bool *history_written)
{
    static char body[1536];
    int64_t now = device_uptime_ms();
    json_t j;

    *history_written = false;

    if (with_history && time_is_synced())
    {
        char key[32] = "history/";
        make_push_id(key + 8, epoch_ms());

        json_begin(&j, body, sizeof(body));
        json_obj_begin(&j, "live");
        add_live_fields(&j, s, now);
        json_obj_end(&j);
        json_obj_begin(&j, key);
        add_history_fields(&j, s);
        json_obj_end(&j);

        if (!json_end(&j))
        {
            ESP_LOGE(TAG, "update payload overflow");
            return false;
        }

        bool ok = cloud_request(HTTP_METHOD_PATCH, NULL, body);
        *history_written = ok;
        return ok;
    }

    json_begin(&j, body, sizeof(body));
    add_live_fields(&j, s, now);
    if (!json_end(&j))
    {
        ESP_LOGE(TAG, "live payload overflow");
        return false;
    }

    bool live_ok = cloud_request(HTTP_METHOD_PUT, "live", body);

    if (with_history)
    {
        json_begin(&j, body, sizeof(body));
        add_history_fields(&j, s);
        if (json_end(&j))
        {
            *history_written = cloud_request(HTTP_METHOD_POST, "history", body);
        }
    }

    return live_ok;
}

static bool send_sos(const sos_msg_t *msg)
{
    char body[384];
    json_t j;
    json_begin(&j, body, sizeof(body));

    bool has_location = msg->source != SOS_LOCATION_NONE;

    if (msg->type == SOS_MSG_TRIGGER)
    {
        // Full replace: a new alert.
        json_bool(&j, "active", true);
        json_str(&j, "eventId", msg->event_id);
        json_str(&j, "deviceName", DEVICE_NAME);
        json_str(&j, "stage", "triggered");
        json_str(&j, "locationSource", sos_source_name(msg->source));
        if (has_location)
        {
            json_num(&j, "lat", msg->lat, 7);
            json_num(&j, "lng", msg->lon, 7);
        }
        json_server_time(&j, "timestamp");
    }
    else
    {
        // Partial update: never touches `active`, so a guardian's
        // acknowledgement is preserved.
        json_str(&j, "stage", "completed");
        json_str(&j, "locationSource", sos_source_name(msg->source));
        if (has_location)
        {
            json_num(&j, "lat", msg->lat, 7);
            json_num(&j, "lng", msg->lon, 7);
        }
        json_bool(&j, "smsSent", msg->sms_sent);
        json_bool(&j, "callStarted", msg->call_started);
        json_server_time(&j, "updatedAt");
    }

    if (!json_end(&j))
        return false;

    ESP_LOGW(TAG, "SOS %s: %s", msg->type == SOS_MSG_TRIGGER ? "alert" : "update", body);
    return cloud_request(msg->type == SOS_MSG_TRIGGER ? HTTP_METHOD_PUT : HTTP_METHOD_PATCH,
                         "sos_alert", body);
}

// =========================================================
// CLOUD TASK
// =========================================================

static void cloud_task(void *arg)
{
    (void)arg;

    esp_task_wdt_add(NULL);

    sos_pending_t sos_trigger = {0};
    sos_pending_t sos_update = {0};

    int64_t last_heartbeat_ms = 0;
    int64_t last_history_ms = 0;
    int64_t last_sos_attempt_ms = 0;
    uint32_t last_history_seq = 0;
    bool was_connected = false;
    bool time_logged = false;

    while (1)
    {
        esp_task_wdt_reset();

        sos_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (msg.type == SOS_MSG_TRIGGER)
            {
                // A new alert supersedes any undelivered update.
                sos_trigger.pending = true;
                sos_trigger.msg = msg;
                sos_update.pending = false;
            }
            else
            {
                sos_update.pending = true;
                sos_update.msg = msg;
            }
            last_sos_attempt_ms = 0;
        }

        if (!s_wifi_configured)
            continue;

        int64_t now = device_uptime_ms();

        if (!s_wifi_connected)
        {
            if (was_connected)
            {
                http_reset();
                was_connected = false;
            }
            wifi_supervise(now);
            continue;
        }

        if (!was_connected)
        {
            was_connected = true;
            last_heartbeat_ms = 0; // report immediately after (re)connecting
        }

        if (!time_logged && time_is_synced())
        {
            time_logged = true;
            ESP_LOGI(TAG, "Clock synchronized via SNTP.");
        }

        // 1. SOS has priority and is retried until delivered.
        if ((sos_trigger.pending || sos_update.pending) &&
            (last_sos_attempt_ms == 0 || now - last_sos_attempt_ms >= CLOUD_SOS_RETRY_MS))
        {
            last_sos_attempt_ms = now;
            if (sos_trigger.pending && send_sos(&sos_trigger.msg))
            {
                sos_trigger.pending = false;
                last_heartbeat_ms = 0; // reflect sosActive in /live too
            }
            // The update goes straight after its trigger is delivered.
            if (!sos_trigger.pending && sos_update.pending && send_sos(&sos_update.msg))
            {
                sos_update.pending = false;
            }
            now = device_uptime_ms();
        }

        // 2. Heartbeat (independent of GPS, SIM and modem), carrying a
        //    history entry when there is a new trail-quality fix.
        if (last_heartbeat_ms == 0 || now - last_heartbeat_ms >= CLOUD_HEARTBEAT_MS)
        {
            last_heartbeat_ms = now;
            wifi_refresh_rssi();

            device_state_t s;
            device_state_snapshot(&s);

            bool history_due = s.has_location &&
                               s.fix_seq != last_history_seq &&
                               (last_history_ms == 0 ||
                                now - last_history_ms >= CLOUD_HISTORY_MIN_INTERVAL_MS);
            bool with_history = history_due && fix_belongs_in_history(&s, now);

            if (history_due && !with_history)
            {
                last_history_seq = s.fix_seq; // stale or low quality: skip it
            }

            bool history_written = false;
            send_update(&s, with_history, &history_written);

            if (history_written)
            {
                last_history_seq = s.fix_seq;
                last_history_ms = now;
            }
        }

        // 3. Self-healing: Wi-Fi up but nothing uploads for a long time.
        device_state_t health;
        device_state_snapshot(&health);
        int64_t since_ok = now - (health.last_cloud_ok_ms > 0 ? health.last_cloud_ok_ms : 0);
        if (now > CLOUD_STALL_RESTART_MS && since_ok > CLOUD_STALL_RESTART_MS)
        {
            ESP_LOGE(TAG, "No successful upload for %d min with Wi-Fi up. Restarting.",
                     CLOUD_STALL_RESTART_MS / 60000);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}

// =========================================================
// PUBLIC API
// =========================================================

void cloud_sync_start(void)
{
    s_queue = xQueueCreate(CLOUD_QUEUE_LEN, sizeof(sos_msg_t));

    wifi_init();

    xTaskCreate(cloud_task, "cloud_task", CLOUD_TASK_STACK, NULL, CLOUD_TASK_PRIORITY, NULL);
}

void cloud_sync_sos_triggered(void)
{
    if (s_queue == NULL)
        return;

    device_state_t s;
    device_state_snapshot(&s);

    sos_msg_t msg = {
        .type = SOS_MSG_TRIGGER,
        .source = s.has_location ? SOS_LOCATION_LAST_KNOWN : SOS_LOCATION_NONE,
        .lat = s.latitude,
        .lon = s.longitude,
    };
    snprintf(msg.event_id, sizeof(msg.event_id), "%08lx", (unsigned long)esp_random());

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "SOS queue full; alert not queued");
    }
}

void cloud_sync_sos_update(sos_location_source_t source,
                           double lat,
                           double lon,
                           bool sms_sent,
                           bool call_started)
{
    if (s_queue == NULL)
        return;

    sos_msg_t msg = {
        .type = SOS_MSG_UPDATE,
        .source = source,
        .lat = lat,
        .lon = lon,
        .sms_sent = sms_sent,
        .call_started = call_started,
    };

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "SOS queue full; update not queued");
    }
}

bool cloud_sync_is_healthy(void)
{
    device_state_t s;
    device_state_snapshot(&s);
    return s.wifi_connected &&
           s.last_cloud_ok_ms > 0 &&
           (device_uptime_ms() - s.last_cloud_ok_ms) <= (3 * CLOUD_HEARTBEAT_MS + CLOUD_HTTP_TIMEOUT_MS);
}
