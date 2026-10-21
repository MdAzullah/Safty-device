/* =========================================================
   Guardian Beacon - ESP32 + AI-Thinker A9G
   ESP-IDF Safety Firmware

   BEHAVIOR
   ---------------------------------------------------------
   NORMAL:
     - GPS enabled continuously.
     - GPS checked immediately after boot.
     - GPS checked every 30 seconds.
     - Every valid GPS fix becomes the LAST LIVE LOCATION.

   SOS:
     - Start a 10-second fresh GPS acquisition window.
     - If a fresh GPS fix is obtained within 10 seconds:
         -> SMS current location
         -> CALL guardian

     - If no fresh GPS fix within 10 seconds BUT a previous
       live location exists:
         -> SMS last live seen location
         -> CALL guardian

     - If no fresh GPS fix AND no previous location exists:
         -> NO location SMS
         -> CALL guardian immediately

   ========================================================= */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "a9g_module.h"
#include "pins_config.h"

static const char *TAG = "SOS";

// =========================================================
// GPS LOCATION STRUCTURE
// =========================================================

typedef struct
{
    double latitude;
    double longitude;

    bool valid;

    int64_t fix_time_ms;

} gps_location_t;

// =========================================================
// SOS STATES
// =========================================================

typedef enum
{
    SOS_STATE_NORMAL = 0,

    SOS_STATE_GPS_WAIT,

    SOS_STATE_SEND_CURRENT,

    SOS_STATE_SEND_LAST,

    SOS_STATE_CALL

} sos_state_t;

// =========================================================
// GLOBAL STATE
// =========================================================

static gps_location_t last_live_location = {0};

static gps_location_t sos_current_location = {0};

static sos_state_t sos_state =
    SOS_STATE_NORMAL;

static int64_t sos_started_ms = 0;

static int64_t last_sos_gps_attempt_ms = 0;

static int64_t last_periodic_gps_ms = 0;

static bool a9g_ready = false;

static bool sos_sms_ok = false;

// =========================================================
// TIME
// =========================================================

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// =========================================================
// BUZZER
// =========================================================

static void beep(
    int times,
    int duration_ms)
{
    for (int i = 0; i < times; i++)
    {
        gpio_set_level(
            BUZZER_PIN,
            1
        );

        vTaskDelay(
            pdMS_TO_TICKS(
                duration_ms
            )
        );

        gpio_set_level(
            BUZZER_PIN,
            0
        );

        vTaskDelay(
            pdMS_TO_TICKS(
                duration_ms
            )
        );
    }
}

// =========================================================
// SAVE LAST LIVE LOCATION
// =========================================================

static void save_last_live_location(
    double lat,
    double lon,
    int64_t fix_time_ms)
{
    last_live_location.latitude =
        lat;

    last_live_location.longitude =
        lon;

    last_live_location.valid =
        true;

    last_live_location.fix_time_ms =
        fix_time_ms;

    ESP_LOGI(
        TAG,
        "LAST LIVE LOCATION UPDATED:"
    );

    ESP_LOGI(
        TAG,
        "Latitude : %.7f",
        lat
    );

    ESP_LOGI(
        TAG,
        "Longitude: %.7f",
        lon
    );
}

// =========================================================
// REQUEST GPS
// =========================================================
//
// IMPORTANT:
// This function updates last_live_location ONLY when
// a VALID GPS response is received.
//
// =========================================================

static bool request_gps_and_save(
    gps_location_t *output,
    uint32_t timeout_ms)
{
    if (!a9g_ready)
        return false;

    double lat = 0.0;
    double lon = 0.0;

    if (!a9g_try_get_gps_location_timeout(
            &lat,
            &lon,
            timeout_ms))
    {
        return false;
    }

    int64_t fix_time =
        now_ms();

    // Save as latest live location
    save_last_live_location(
        lat,
        lon,
        fix_time
    );

    // Optional output structure
    if (output != NULL)
    {
        output->latitude =
            lat;

        output->longitude =
            lon;

        output->valid =
            true;

        output->fix_time_ms =
            fix_time;
    }

    return true;
}

// =========================================================
// ESP32 WI-FI FALLBACK & HYBRID NETWORK MANAGER
// =========================================================

static bool s_wifi_connected = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected. Auto-reconnecting in background...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
    }
}

static void wifi_init_fallback(void)
{
#if WIFI_FALLBACK_ENABLED
    if (WIFI_SSID[0] == '\0' || WIFI_PASS[0] == '\0')
    {
        ESP_LOGW(TAG, "Wi-Fi fallback is enabled but credentials are empty; skipping Wi-Fi initialization.");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "ESP32 Fallback Wi-Fi station initialized for SSID: %s", WIFI_SSID);
#endif
}

static bool esp_wifi_http_send(const char *url, const char *json_body, esp_http_client_method_t method)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 6000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return false;

    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    bool success = false;
    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code >= 200 && status_code < 300)
        {
            ESP_LOGI(TAG, "Wi-Fi HTTPS Cloud Sync OK (Status %d)", status_code);
            success = true;
        }
        else
        {
            ESP_LOGW(TAG, "Wi-Fi HTTPS Cloud Sync HTTP Status: %d", status_code);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Wi-Fi HTTPS request error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

static bool cloud_send_payload(const char *url, const char *json_body, bool is_post, const char **out_medium)
{
    // Use the ESP32 HTTP client for both methods. The A9G command path
    // does not provide a verified PUT implementation, and UART HTTP could
    // block SOS GPS/SMS/call operations while holding the modem lock.
    if (s_wifi_connected)
    {
        bool wifi_ok = esp_wifi_http_send(url, json_body, is_post ? HTTP_METHOD_POST : HTTP_METHOD_PUT);
        if (wifi_ok)
        {
            if (out_medium) *out_medium = "Wi-Fi (ESP32)";
            return true;
        }
    }

    if (out_medium) *out_medium = "Offline";
    return false;
}

// =========================================================
// CLOUD TELEMETRY SYNC (WEBSITE INTEGRATION)
// =========================================================

static void sync_telemetry_to_cloud(double lat, double lon, double speed)
{
    int battery_pct = 100;
    int battery_mv = 4000;
    if (a9g_ready)
    {
        a9g_get_battery(&battery_pct, &battery_mv);
    }

    char signal_str[32] = "Good";
    int rssi = 20;
    if (a9g_ready)
    {
        a9g_get_signal(signal_str, sizeof(signal_str), &rssi);
    }

    const char *network_mode = "ESP32 Wi-Fi";

    // 1. Prepare Live Telemetry JSON
    char live_url[256];
    snprintf(live_url, sizeof(live_url), "%s/devices/%s/live.json", CLOUD_API_BASE_URL, DEVICE_ID);

    char live_json[320];
    snprintf(
        live_json,
        sizeof(live_json),
        "{\"lat\":%.7f,\"lng\":%.7f,\"speed\":%.1f,\"battery\":%d,\"signal\":\"%s\",\"network\":\"%s\",\"status\":\"Online\",\"timestamp\":{\".sv\":\"timestamp\"}}",
        lat, lon, speed, battery_pct, signal_str, network_mode
    );

    ESP_LOGI(TAG, "Syncing Live Telemetry to Website: %s", live_json);
    cloud_send_payload(live_url, live_json, false, NULL);

    // 2. Append to History Trail JSON
    char history_url[256];
    snprintf(history_url, sizeof(history_url), "%s/devices/%s/history.json", CLOUD_API_BASE_URL, DEVICE_ID);

    char history_json[320];
    snprintf(
        history_json,
        sizeof(history_json),
        "{\"lat\":%.7f,\"lng\":%.7f,\"speed\":%.1f,\"battery\":%d,\"signal\":\"%s\",\"network\":\"%s\",\"timestamp\":{\".sv\":\"timestamp\"}}",
        lat, lon, speed, battery_pct, signal_str, network_mode
    );

    cloud_send_payload(history_url, history_json, true, NULL);
}

static void sync_sos_to_cloud(bool active, double lat, double lon)
{
    char sos_url[256];
    snprintf(sos_url, sizeof(sos_url), "%s/devices/%s/sos_alert.json", CLOUD_API_BASE_URL, DEVICE_ID);

    char sos_json[256];
    if (active)
    {
        if (lat == 0.0 && lon == 0.0)
        {
            snprintf(sos_json, sizeof(sos_json),
                     "{\"active\":true,\"deviceName\":\"%s\",\"timestamp\":{\".sv\":\"timestamp\"}}",
                     DEVICE_NAME);
        }
        else
        {
            snprintf(
                sos_json,
                sizeof(sos_json),
                "{\"active\":true,\"deviceName\":\"%s\",\"lat\":%.7f,\"lng\":%.7f,\"timestamp\":{\".sv\":\"timestamp\"}}",
                DEVICE_NAME, lat, lon
            );
        }
    }
    else
    {
        snprintf(
            sos_json,
            sizeof(sos_json),
            "{\"active\":false,\"deviceName\":\"%s\",\"timestamp\":{\".sv\":\"timestamp\"}}",
            DEVICE_NAME
        );
    }

    ESP_LOGW(TAG, "Broadcasting SOS alert to Website: %s", sos_json);
    cloud_send_payload(sos_url, sos_json, false, NULL);
}

// =========================================================
// PERIODIC GPS & CLOUD UPDATE
// =========================================================

static void handle_periodic_gps_update(void)
{
    if (!a9g_ready)
        return;

    // Never perform periodic GPS acquisition while
    // an SOS sequence is running.
    if (sos_state != SOS_STATE_NORMAL)
        return;

    int64_t current_ms =
        now_ms();

    bool should_update =
        (last_periodic_gps_ms == 0) ||
        ((current_ms -
          last_periodic_gps_ms) >=
         TELEMETRY_PERIODIC_UPDATE_MS);

    if (!should_update)
        return;

    last_periodic_gps_ms =
        current_ms;

    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "PERIODIC LIVE GPS & CLOUD SYNC"
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    gps_location_t current_fix = {0};

    if (request_gps_and_save(
            &current_fix,
            AT_DEFAULT_TIMEOUT_MS))
    {
        ESP_LOGI(
            TAG,
            "Live GPS location updated successfully."
        );

        // Upload live data to the website cloud
        sync_telemetry_to_cloud(current_fix.latitude, current_fix.longitude, 0.0);
    }
    else
    {
        ESP_LOGW(
            TAG,
            "No new GPS fix. Keeping previous last live location."
        );

        // Do not refresh cloud timestamps with an old fix: the dashboard
        // watchdog must be able to distinguish a stale GPS fix from a live one.
    }
}


// =========================================================
// CURRENT LOCATION SMS
// =========================================================

static void build_current_location_message(
    char *out,
    size_t out_size,
    const gps_location_t *location)
{
    snprintf(
        out,
        out_size,

        "SOS EMERGENCY\r\n"
        "\r\n"
        "Emergency SOS has been activated.\r\n"
        "\r\n"
        "CURRENT LIVE LOCATION:\r\n"
        "https://maps.google.com/?q=%.7f,%.7f\r\n"
        "\r\n"
        "Latitude: %.7f\r\n"
        "Longitude: %.7f\r\n"
        "\r\n"
        "Location Status: CURRENT LIVE GPS LOCATION",

        location->latitude,
        location->longitude,

        location->latitude,
        location->longitude
    );
}

// =========================================================
// LAST LIVE LOCATION SMS
// =========================================================

static void build_last_location_message(
    char *out,
    size_t out_size)
{
    int64_t age_ms =
        now_ms() -
        last_live_location.fix_time_ms;

    int64_t age_seconds =
        age_ms / 1000;

    snprintf(
        out,
        out_size,

        "SOS EMERGENCY\r\n"
        "\r\n"
        "Emergency SOS has been activated.\r\n"
        "\r\n"
        "CURRENT LIVE LOCATION: NOT AVAILABLE\r\n"
        "\r\n"
        "The device could not obtain a new GPS location "
        "within 10 seconds.\r\n"
        "\r\n"
        "LAST LIVE SEEN LOCATION:\r\n"
        "https://maps.google.com/?q=%.7f,%.7f\r\n"
        "\r\n"
        "Latitude: %.7f\r\n"
        "Longitude: %.7f\r\n"
        "\r\n"
        "Location was last successfully updated "
        "%lld seconds ago.\r\n"
        "\r\n"
        "Location Status: LAST LIVE SEEN LOCATION",

        last_live_location.latitude,
        last_live_location.longitude,

        last_live_location.latitude,
        last_live_location.longitude,

        age_seconds
    );
}

// =========================================================
// SEND SMS TO GUARDIANS
// =========================================================

static bool send_emergency_sms(
    const char *message)
{
    bool primary_ok =
        a9g_send_sms(
            PARENT_NUMBER,
            message
        );

    bool overall_ok =
        primary_ok;

    // Optional second guardian
    if (strlen(PARENT_NUMBER_2) > 0)
    {
        bool second_ok =
            a9g_send_sms(
                PARENT_NUMBER_2,
                message
            );

        overall_ok =
            overall_ok &&
            second_ok;
    }

    return overall_ok;
}

// =========================================================
// START SOS
// =========================================================

static void start_sos_sequence(void)
{
    ESP_LOGW(
        TAG,
        "======================================"
    );

    ESP_LOGW(
        TAG,
        "SOS BUTTON PRESSED"
    );

    ESP_LOGW(
        TAG,
        "Starting 10-second LIVE GPS window."
    );

    ESP_LOGW(
        TAG,
        "======================================"
    );

    gpio_set_level(
        STATUS_LED_PIN,
        1
    );

    beep(
        2,
        80
    );

    if (!a9g_ready)
    {
        ESP_LOGE(
            TAG,
            "A9G is not ready."
        );

        ESP_LOGW(
            TAG,
            "Calling guardian anyway."
        );

        a9g_make_call(
            PARENT_NUMBER
        );

        sync_sos_to_cloud(true, 0.0, 0.0);

        gpio_set_level(
            STATUS_LED_PIN,
            0
        );

        return;
    }

    sos_current_location.valid =
        false;

    sos_sms_ok =
        false;

    sos_state =
        SOS_STATE_GPS_WAIT;

    sos_started_ms =
        now_ms();

    last_sos_gps_attempt_ms =
        0;

    // SOS cloud update is sent after GPS acquisition so that no network
    // operation can consume the bounded fresh-location acquisition window.
}


// =========================================================
// SOS STATE MACHINE
// =========================================================

static void handle_sos_state_machine(void)
{
    if (sos_state ==
        SOS_STATE_NORMAL)
    {
        return;
    }

    int64_t current_ms =
        now_ms();

    switch (sos_state)
    {
        // =================================================
        // WAIT FOR FRESH GPS
        // =================================================

        case SOS_STATE_GPS_WAIT:
        {
            int64_t elapsed =
                current_ms -
                sos_started_ms;

            // ---------------------------------------------
            // 10-second deadline reached
            // ---------------------------------------------

            if (elapsed >=
                SOS_GPS_FIRST_WINDOW_MS)
            {
                ESP_LOGW(
                    TAG,
                    "NO CURRENT GPS FIX WITHIN 10 SECONDS."
                );

                if (last_live_location.valid)
                {
                    ESP_LOGW(
                        TAG,
                        "Using LAST LIVE SEEN LOCATION."
                    );

                    sos_state =
                        SOS_STATE_SEND_LAST;
                }
                else
                {
                    ESP_LOGW(
                        TAG,
                        "NO LAST LIVE LOCATION EXISTS."
                    );

                    ESP_LOGW(
                        TAG,
                        "CALLING GUARDIAN WITHOUT SMS."
                    );

                    sos_state =
                        SOS_STATE_CALL;
                }

                break;
            }

            // ---------------------------------------------
            // GPS polling interval
            // ---------------------------------------------

            bool should_try =
                (last_sos_gps_attempt_ms == 0) ||
                ((current_ms -
                  last_sos_gps_attempt_ms) >=
                 GPS_POLL_INTERVAL_MS);

            if (!should_try)
                break;

            last_sos_gps_attempt_ms =
                current_ms;

            // ---------------------------------------------
            // Calculate EXACT remaining time
            // ---------------------------------------------

            current_ms =
                now_ms();

            elapsed =
                current_ms -
                sos_started_ms;

            int64_t remaining_ms =
                SOS_GPS_FIRST_WINDOW_MS -
                elapsed;

            if (remaining_ms <= 0)
            {
                break;
            }

            // Never allow the GPS command itself to run
            // beyond the 10-second SOS window.
            uint32_t gps_timeout =
                (remaining_ms >
                 3000)
                    ? 3000
                    : (uint32_t)remaining_ms;

            ESP_LOGI(
                TAG,
                "SOS GPS attempt. %lld ms remaining.",
                remaining_ms
            );

            if (request_gps_and_save(
                    &sos_current_location,
                    gps_timeout))
            {
                // IMPORTANT:
                // Check the deadline again AFTER the GPS
                // command returns.
                int64_t finished_ms =
                    now_ms();

                if ((finished_ms -
                     sos_started_ms) <=
                    SOS_GPS_FIRST_WINDOW_MS)
                {
                    ESP_LOGI(
                        TAG,
                        "CURRENT GPS LOCATION FOUND "
                        "WITHIN 10 SECONDS."
                    );

                    sos_state =
                        SOS_STATE_SEND_CURRENT;
                }
                else
                {
                    ESP_LOGW(
                        TAG,
                        "GPS response arrived AFTER "
                        "10-second deadline."
                    );

                    if (last_live_location.valid)
                    {
                        sos_state =
                            SOS_STATE_SEND_LAST;
                    }
                    else
                    {
                        sos_state =
                            SOS_STATE_CALL;
                    }
                }
            }

            break;
        }

        // =================================================
        // CURRENT LOCATION SMS
        // =================================================

        case SOS_STATE_SEND_CURRENT:
        {
            char message[512];

            build_current_location_message(
                message,
                sizeof(message),
                &sos_current_location
            );

            ESP_LOGI(
                TAG,
                "Sending CURRENT GPS location SMS."
            );

            sos_sms_ok =
                send_emergency_sms(
                    message
                );

            // Call ALWAYS follows
            sos_state =
                SOS_STATE_CALL;

            break;
        }

        // =================================================
        // LAST LIVE LOCATION SMS
        // =================================================

        case SOS_STATE_SEND_LAST:
        {
            char message[512];

            build_last_location_message(
                message,
                sizeof(message)
            );

            ESP_LOGI(
                TAG,
                "Sending LAST LIVE SEEN location SMS."
            );

            sos_sms_ok =
                send_emergency_sms(
                    message
                );

            // Call ALWAYS follows
            sos_state =
                SOS_STATE_CALL;

            break;
        }

        // =================================================
        // EMERGENCY CALL
        // =================================================

        case SOS_STATE_CALL:
        {
            ESP_LOGW(
                TAG,
                "======================================"
            );

            ESP_LOGW(
                TAG,
                "STARTING EMERGENCY CALL"
            );

            ESP_LOGW(
                TAG,
                "======================================"
            );

            bool call_ok =
                a9g_make_call(
                    PARENT_NUMBER
                );

            // Begin the emergency call before cloud sync can wait on Wi-Fi.
            if (sos_current_location.valid)
            {
                sync_sos_to_cloud(true, sos_current_location.latitude, sos_current_location.longitude);
            }
            else if (last_live_location.valid)
            {
                sync_sos_to_cloud(true, last_live_location.latitude, last_live_location.longitude);
            }
            else
            {
                sync_sos_to_cloud(true, 0.0, 0.0);
            }

            if (call_ok)
            {
                ESP_LOGI(
                    TAG,
                    "EMERGENCY CALL STARTED."
                );
            }
            else
            {
                ESP_LOGE(
                    TAG,
                    "EMERGENCY CALL FAILED."
                );
            }

            // ---------------------------------------------
            // Sustain call duration (allow call to connect/ring)
            // ---------------------------------------------
            ESP_LOGI(TAG, "Call in progress... sustaining emergency mode for 25 seconds.");
            for (int s = 0; s < 25; s++)
            {
                gpio_set_level(STATUS_LED_PIN, (s % 2 == 0) ? 1 : 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            gpio_set_level(
                STATUS_LED_PIN,
                0
            );

            ESP_LOGW(
                TAG,
                "======================================"
            );

            ESP_LOGW(
                TAG,
                "SOS SEQUENCE COMPLETE"
            );

            ESP_LOGW(
                TAG,
                "======================================"
            );

            // Return to normal operation
            sos_state =
                SOS_STATE_NORMAL;

            sos_started_ms =
                0;

            last_sos_gps_attempt_ms =
                0;

            break;
        }

        default:
        {
            sos_state =
                SOS_STATE_NORMAL;

            break;
        }
    }
}

// =========================================================
// SOS BUTTON
// =========================================================

static void handle_sos_button(void)
{
    static int64_t last_press_ms = 0;

    // Active LOW
    if (gpio_get_level(
            SOS_BUTTON_PIN) != 0)
    {
        return;
    }

    int64_t current_ms =
        now_ms();

    // Debounce
    if ((current_ms -
         last_press_ms) <=
        SOS_DEBOUNCE_MS)
    {
        return;
    }

    last_press_ms =
        current_ms;

    if (sos_state ==
        SOS_STATE_NORMAL)
    {
        start_sos_sequence();
    }
    else
    {
        ESP_LOGW(
            TAG,
            "SOS already running. Button ignored."
        );
    }
}

// =========================================================
// GPIO SETUP
// =========================================================

static void gpio_setup(void)
{
    // ---------------------------------------------
    // SOS button
    // ---------------------------------------------

    gpio_config_t button_cfg =
    {
        .pin_bit_mask =
            1ULL << SOS_BUTTON_PIN,

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_ENABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(
        gpio_config(
            &button_cfg
        )
    );

    // ---------------------------------------------
    // LED + buzzer
    // ---------------------------------------------

    gpio_config_t output_cfg =
    {
        .pin_bit_mask =
            (1ULL << STATUS_LED_PIN) |
            (1ULL << BUZZER_PIN),

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(
        gpio_config(
            &output_cfg
        )
    );

    gpio_set_level(
        STATUS_LED_PIN,
        0
    );

    gpio_set_level(
        BUZZER_PIN,
        0
    );
}

// =========================================================
// MAIN HIGH-PRIORITY SAFETY TASK
// Handles SOS button and emergency state machine
// =========================================================

static void safety_task(
    void *arg)
{
    (void)arg;

    while (1)
    {
        // SOS button
        handle_sos_button();

        // SOS state machine
        handle_sos_state_machine();

        // Fast loop for instantaneous button/SOS timing (<50ms)
        vTaskDelay(
            pdMS_TO_TICKS(50)
        );
    }
}

// =========================================================
// BACKGROUND TELEMETRY TASK
// Handles periodic GPS updates and Cloud Sync
// =========================================================

static void telemetry_task(
    void *arg)
{
    (void)arg;

    // Initial settle delay before first background telemetry check
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1)
    {
        // Yield if an SOS emergency is ongoing
        if (sos_state == SOS_STATE_NORMAL && a9g_ready)
        {
            handle_periodic_gps_update();
        }

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}

// =========================================================
// APP MAIN
// =========================================================

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "======================================"
    );

    ESP_LOGI(
        TAG,
        "Guardian Beacon Booting..."
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    gpio_setup();

    // ---------------------------------------------
    // Initialize Wi-Fi Fallback
    // ---------------------------------------------
    wifi_init_fallback();

    // ---------------------------------------------
    // Initialize A9G
    // ---------------------------------------------

    if (!a9g_init())
    {
        ESP_LOGE(
            TAG,
            "A9G initialization failed."
        );

        ESP_LOGE(
            TAG,
            "GPS/SMS/CALL functions may not work."
        );
    }
    else
    {
        a9g_ready =
            true;

        // -----------------------------------------
        // Enable GPS
        // -----------------------------------------

        if (a9g_enable_gps())
        {
            ESP_LOGI(
                TAG,
                "GPS enabled successfully."
            );
        }
        else
        {
            ESP_LOGE(
                TAG,
                "GPS could not be enabled."
            );
        }

            ESP_LOGI(TAG, "Firebase cloud sync uses ESP32 Wi-Fi; configure its credentials for uploads.");

        ESP_LOGI(
            TAG,
            "System ready."
        );

        ESP_LOGI(
            TAG,
            "GPS will be checked periodically."
        );

        ESP_LOGI(
            TAG,
            "Press SOS to start emergency sequence."
        );

        beep(
            1,
            100
        );
    }

    // ---------------------------------------------
    // 1. High-Priority Safety Task (Priority 10)
    // ---------------------------------------------
    xTaskCreate(
        safety_task,
        "safety_task",
        4096,
        NULL,
        10,
        NULL
    );

    // ---------------------------------------------
    // 2. Background Telemetry Task (Priority 2)
    // ---------------------------------------------
    xTaskCreate(
        telemetry_task,
        "telemetry_task",
        4096,
        NULL,
        2,
        NULL
    );
}