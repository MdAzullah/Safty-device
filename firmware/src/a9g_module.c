#include "a9g_module.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "pins_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/semphr.h"

static const char *TAG = "A9G";

static SemaphoreHandle_t s_uart_mutex = NULL;

void a9g_uart_lock(void)
{
    if (s_uart_mutex == NULL)
    {
        s_uart_mutex = xSemaphoreCreateMutex();
    }
    if (s_uart_mutex != NULL)
    {
        xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    }
}

void a9g_uart_unlock(void)
{
    if (s_uart_mutex != NULL)
    {
        xSemaphoreGive(s_uart_mutex);
    }
}

// =========================================================
// TIME
// =========================================================

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

// =========================================================
// READ A9G RESPONSE
// =========================================================

static int read_response(char *buf,
                         size_t buf_size,
                         uint32_t timeout_ms)
{
    if (buf == NULL || buf_size == 0)
        return 0;

    memset(buf, 0, buf_size);

    size_t total = 0;

    int64_t start = now_us();
    int64_t last_byte = start;

    // Generous idle timeout (4.0s) so cellular SMS / Network delays do not get aborted early
    const int64_t idle_timeout_us = 4000 * 1000;

    while ((now_us() - start) < ((int64_t)timeout_ms * 1000))
    {
        uint8_t byte;

        int len = uart_read_bytes(
            A9G_UART_PORT,
            &byte,
            1,
            pdMS_TO_TICKS(50)
        );

        if (len > 0)
        {
            if (total < buf_size - 1)
            {
                buf[total++] = (char)byte;
                buf[total] = '\0';
            }

            last_byte = now_us();

            // Normal command completion keywords
            if (strstr(buf, "\r\nOK\r\n") ||
                strstr(buf, "\nOK\r\n") ||
                strstr(buf, "\nOK\n") ||
                strstr(buf, "\r\nERROR\r\n") ||
                strstr(buf, "\nERROR\n") ||
                strstr(buf, "+CME ERROR") ||
                strstr(buf, "+CMS ERROR") ||
                strstr(buf, "+CMGS:") ||
                strstr(buf, "GPS NOT FIX"))
            {
                // Brief pause to read any remaining trailer bytes
                vTaskDelay(pdMS_TO_TICKS(20));
                while (uart_read_bytes(A9G_UART_PORT, &byte, 1, pdMS_TO_TICKS(10)) > 0)
                {
                    if (total < buf_size - 1)
                    {
                        buf[total++] = (char)byte;
                        buf[total] = '\0';
                    }
                }
                break;
            }
        }
        else
        {
            if (total > 0 &&
                (now_us() - last_byte) > idle_timeout_us)
            {
                break;
            }
        }
    }

    return (int)total;
}

// =========================================================
// RESPONSE HELPERS
// =========================================================

static bool response_has_ok(const char *response)
{
    if (response == NULL)
        return false;

    if (strstr(response, "ERROR") != NULL)
        return false;

    if (strstr(response, "+CME ERROR") != NULL)
        return false;

    if (strstr(response, "+CMS ERROR") != NULL)
        return false;

    return strstr(response, "OK") != NULL;
}

static bool sim_is_ready(const char *response)
{
    return response != NULL &&
           strstr(response, "+CPIN: READY") != NULL;
}

static bool network_is_registered(const char *response)
{
    if (response == NULL)
        return false;

    const char *creg = strstr(response, "+CREG:");

    if (creg == NULL)
        return false;

    int n = 0;
    int stat = 0;

    if (sscanf(creg, "+CREG: %d,%d", &n, &stat) != 2 &&
        sscanf(creg, "+CREG:%d,%d", &n, &stat) != 2)
    {
        return false;
    }

    return (stat == 1 || stat == 5);
}

// =========================================================
// SEND AT COMMAND (THREAD SAFE)
// =========================================================

bool a9g_send_at(const char *command,
                 char *response_buf,
                 size_t buf_size,
                 uint32_t timeout_ms)
{
    if (command == NULL ||
        response_buf == NULL ||
        buf_size == 0)
    {
        return false;
    }

    a9g_uart_lock();

    // Remove stale UART data
    uart_flush_input(A9G_UART_PORT);

    ESP_LOGI(TAG, "TX: %s", command);

    uart_write_bytes(
        A9G_UART_PORT,
        command,
        strlen(command)
    );

    uart_write_bytes(
        A9G_UART_PORT,
        "\r\n",
        2
    );

    int len = read_response(
        response_buf,
        buf_size,
        timeout_ms
    );

    ESP_LOGI(TAG,
             "RX: %s",
             response_buf);

    a9g_uart_unlock();

    return len > 0;
}


// =========================================================
// INITIALIZE A9G
// =========================================================

bool a9g_init(void)
{
    uart_config_t uart_config =
    {
        .baud_rate = A9G_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(
        uart_driver_install(
            A9G_UART_PORT,
            A9G_UART_BUF_SIZE,
            A9G_UART_BUF_SIZE,
            0,
            NULL,
            0
        )
    );

    ESP_ERROR_CHECK(
        uart_param_config(
            A9G_UART_PORT,
            &uart_config
        )
    );

    ESP_ERROR_CHECK(
        uart_set_pin(
            A9G_UART_PORT,
            A9G_TX_PIN,
            A9G_RX_PIN,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            BOOT_SETTLE_DELAY_MS
        )
    );

    char resp[256];

    // -----------------------------------------------------
    // A9G communication test
    // -----------------------------------------------------

    bool link_ok = false;

    for (int retry = 0; retry < 5; retry++)
    {
        a9g_send_at(
            "ATE0",
            resp,
            sizeof(resp),
            1500
        );

        if (a9g_send_at(
                "AT",
                resp,
                sizeof(resp),
                2000) &&
            response_has_ok(resp))
        {
            link_ok = true;
            break;
        }

        ESP_LOGW(
            TAG,
            "A9G AT response not received. Retry %d/5",
            retry + 1
        );

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }

    if (!link_ok)
    {
        ESP_LOGE(
            TAG,
            "A9G communication failed."
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "A9G UART communication OK."
    );

    // -----------------------------------------------------
    // SIM
    // -----------------------------------------------------

    bool sim_ok = false;

    for (int retry = 0; retry < 10; retry++)
    {
        if (a9g_send_at(
                "AT+CPIN?",
                resp,
                sizeof(resp),
                2000) &&
            sim_is_ready(resp))
        {
            sim_ok = true;
            break;
        }

        ESP_LOGW(
            TAG,
            "SIM not ready. Retry %d/10",
            retry + 1
        );

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }

    if (!sim_ok)
    {
        ESP_LOGE(
            TAG,
            "SIM is not ready."
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "SIM card READY."
    );

    // -----------------------------------------------------
    // GSM NETWORK
    // -----------------------------------------------------

    bool network_ok = false;

    for (int retry = 0; retry < 30; retry++)
    {
        if (a9g_send_at(
                "AT+CREG?",
                resp,
                sizeof(resp),
                2000) &&
            network_is_registered(resp))
        {
            network_ok = true;
            break;
        }

        ESP_LOGW(
            TAG,
            "Waiting for GSM registration..."
        );

        vTaskDelay(
            pdMS_TO_TICKS(2000)
        );
    }

    if (!network_ok)
    {
        ESP_LOGE(
            TAG,
            "GSM network registration failed."
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "GSM network registered."
    );

    // -----------------------------------------------------
    // SMS TEXT MODE
    // -----------------------------------------------------

    if (!a9g_send_at(
            "AT+CMGF=1",
            resp,
            sizeof(resp),
            2000) ||
        !response_has_ok(resp))
    {
        ESP_LOGE(
            TAG,
            "Could not enable SMS text mode."
        );

        return false;
    }

    // GSM character set
    if (!a9g_send_at(
            "AT+CSCS=\"GSM\"",
            resp,
            sizeof(resp),
            2000) ||
        !response_has_ok(resp))
    {
        ESP_LOGW(
            TAG,
            "Could not set GSM character set."
        );
    }

    ESP_LOGI(
        TAG,
        "A9G GSM service ready."
    );

    return true;
}

// =========================================================
// ENABLE GPS
// =========================================================

bool a9g_enable_gps(void)
{
    char resp[128];

    bool ok =
        a9g_send_at(
            "AT+GPS=1",
            resp,
            sizeof(resp),
            3000
        ) &&
        response_has_ok(resp);

    if (ok)
    {
        ESP_LOGI(
            TAG,
            "GPS enabled."
        );
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to enable GPS."
        );
    }

    return ok;
}

// =========================================================
// PARSE GPS LOCATION
// =========================================================
//
// A9G can return:
//
// AT+LOCATION=2
//
// 28.123456,77.123456
//
// OK
//
// OR:
//
// +LOCATION: 28.123456,77.123456
//
// This parser supports both.
// =========================================================

static bool parse_location(const char *response,
                           double *lat,
                           double *lon)
{
    if (response == NULL ||
        lat == NULL ||
        lon == NULL)
    {
        return false;
    }

    // -----------------------------------------------------
    // First try +LOCATION: latitude,longitude
    // -----------------------------------------------------

    const char *location_tag =
        strstr(response, "+LOCATION:");

    if (location_tag != NULL)
    {
        double parsed_lat = 0.0;
        double parsed_lon = 0.0;

        if (sscanf(
                location_tag,
                "+LOCATION: %lf , %lf",
                &parsed_lat,
                &parsed_lon) == 2 ||
            sscanf(
                location_tag,
                "+LOCATION:%lf,%lf",
                &parsed_lat,
                &parsed_lon) == 2)
        {
            if (parsed_lat >= -90.0 &&
                parsed_lat <= 90.0 &&
                parsed_lon >= -180.0 &&
                parsed_lon <= 180.0 &&
                !(parsed_lat == 0.0 &&
                  parsed_lon == 0.0))
            {
                *lat = parsed_lat;
                *lon = parsed_lon;

                return true;
            }
        }
    }

    // -----------------------------------------------------
    // A9G commonly returns raw:
    //
    // 28.123456,77.123456
    //
    // Search every line for two numbers.
    // -----------------------------------------------------

    char copy[256];

    strncpy(
        copy,
        response,
        sizeof(copy) - 1
    );

    copy[sizeof(copy) - 1] = '\0';

    char *saveptr = NULL;

    char *line =
        strtok_r(
            copy,
            "\r\n",
            &saveptr
        );

    while (line != NULL)
    {
        double parsed_lat = 0.0;
        double parsed_lon = 0.0;

        if (sscanf(
                line,
                " %lf , %lf",
                &parsed_lat,
                &parsed_lon) == 2)
        {
            if (parsed_lat >= -90.0 &&
                parsed_lat <= 90.0 &&
                parsed_lon >= -180.0 &&
                parsed_lon <= 180.0 &&
                !(parsed_lat == 0.0 &&
                  parsed_lon == 0.0))
            {
                *lat = parsed_lat;
                *lon = parsed_lon;

                return true;
            }
        }

        line =
            strtok_r(
                NULL,
                "\r\n",
                &saveptr
            );
    }

    return false;
}

// =========================================================
// GPS REQUEST WITH TIMEOUT
// =========================================================

bool a9g_try_get_gps_location_timeout(
    double *lat,
    double *lon,
    uint32_t timeout_ms)
{
    if (lat == NULL || lon == NULL)
        return false;

    char resp[256];

    bool received =
        a9g_send_at(
            "AT+LOCATION=2",
            resp,
            sizeof(resp),
            timeout_ms
        );

    if (!received)
    {
        ESP_LOGW(
            TAG,
            "GPS command produced no response."
        );

        return false;
    }

    if (parse_location(
            resp,
            lat,
            lon))
    {
        ESP_LOGI(
            TAG,
            "GPS FIX: %.7f, %.7f",
            *lat,
            *lon
        );

        return true;
    }

    ESP_LOGW(
        TAG,
        "No valid GPS fix."
    );

    return false;
}

// =========================================================
// NORMAL GPS REQUEST
// =========================================================

bool a9g_try_get_gps_location(
    double *lat,
    double *lon)
{
    return a9g_try_get_gps_location_timeout(
        lat,
        lon,
        AT_DEFAULT_TIMEOUT_MS
    );
}

// =========================================================
// WAIT FOR SMS PROMPT
// =========================================================

static bool wait_for_sms_prompt(
    uint32_t timeout_ms)
{
    int64_t start = now_us();

    while ((now_us() - start) <
           ((int64_t)timeout_ms * 1000))
    {
        uint8_t byte;

        if (uart_read_bytes(
                A9G_UART_PORT,
                &byte,
                1,
                pdMS_TO_TICKS(50)) > 0)
        {
            if (byte == '>')
            {
                return true;
            }
        }
    }

    return false;
}

// =========================================================
// SEND SMS
// =========================================================

bool a9g_send_sms(
    const char *number,
    const char *message)
{
    if (number == NULL ||
        message == NULL)
    {
        return false;
    }

    char resp[256];

    // -----------------------------------------------------
    // SMS TEXT MODE
    // -----------------------------------------------------

    if (!a9g_send_at(
            "AT+CMGF=1",
            resp,
            sizeof(resp),
            3000) ||
        !response_has_ok(resp))
    {
        ESP_LOGE(
            TAG,
            "SMS: failed to enter text mode."
        );

        return false;
    }

    a9g_uart_lock();

    uart_flush_input(
        A9G_UART_PORT
    );

    // -----------------------------------------------------
    // CMGS
    // -----------------------------------------------------

    char command[64];

    snprintf(
        command,
        sizeof(command),
        "AT+CMGS=\"%s\"",
        number
    );

    ESP_LOGI(
        TAG,
        "SMS TX: %s",
        command
    );

    uart_write_bytes(
        A9G_UART_PORT,
        command,
        strlen(command)
    );

    uart_write_bytes(
        A9G_UART_PORT,
        "\r\n",
        2
    );

    // -----------------------------------------------------
    // WAIT FOR >
    // -----------------------------------------------------

    if (!wait_for_sms_prompt(5000))
    {
        ESP_LOGE(
            TAG,
            "SMS failed: no '>' prompt."
        );

        a9g_uart_unlock();
        return false;
    }

    ESP_LOGI(
        TAG,
        "SMS prompt received."
    );

    // -----------------------------------------------------
    // SEND MESSAGE
    // -----------------------------------------------------

    uart_write_bytes(
        A9G_UART_PORT,
        message,
        strlen(message)
    );

    // CTRL+Z
    uint8_t ctrl_z = 0x1A;

    uart_write_bytes(
        A9G_UART_PORT,
        (const char *)&ctrl_z,
        1
    );

    // -----------------------------------------------------
    // WAIT FOR RESULT
    // -----------------------------------------------------

    int len =
        read_response(
            resp,
            sizeof(resp),
            SMS_SEND_TIMEOUT_MS
        );

    a9g_uart_unlock();

    ESP_LOGI(
        TAG,
        "SMS response: %s",
        resp
    );

    if (len <= 0)
    {
        ESP_LOGE(
            TAG,
            "SMS failed: no response."
        );

        return false;
    }

    if (strstr(resp, "+CMGS:") != NULL &&
        strstr(resp, "ERROR") == NULL)
    {
        ESP_LOGI(
            TAG,
            "SMS SENT SUCCESSFULLY."
        );

        return true;
    }

    if (strstr(resp, "OK") != NULL &&
        strstr(resp, "ERROR") == NULL)
    {
        ESP_LOGI(
            TAG,
            "SMS SENT."
        );

        return true;
    }

    ESP_LOGE(
        TAG,
        "SMS sending failed."
    );

    return false;
}

// =========================================================
// MAKE CALL
// =========================================================

bool a9g_make_call(
    const char *number)
{
    if (number == NULL)
        return false;

    char command[64];
    char resp[128];

    snprintf(
        command,
        sizeof(command),
        "ATD%s;",
        number
    );

    ESP_LOGW(
        TAG,
        "Calling guardian: %s",
        number
    );

    bool got_response =
        a9g_send_at(
            command,
            resp,
            sizeof(resp),
            5000
        );

    if (!got_response)
    {
        ESP_LOGE(
            TAG,
            "Call: no response from A9G."
        );

        return false;
    }

    // ERROR means command rejected.
    if (strstr(resp, "ERROR") != NULL ||
        strstr(resp, "+CME ERROR") != NULL)
    {
        ESP_LOGE(
            TAG,
            "Call command rejected."
        );

        return false;
    }

    if (strstr(resp, "OK") != NULL)
    {
        ESP_LOGI(
            TAG,
            "Emergency call initiated."
        );

        return true;
    }

    ESP_LOGW(
        TAG,
        "Unexpected call response: %s",
        resp
    );

    return false;
}

// =========================================================
// HANG UP
// =========================================================

void a9g_hangup(void)
{
    char resp[64];

    a9g_send_at(
        "ATH",
        resp,
        sizeof(resp),
        3000
    );
}

// =========================================================
// GPRS CELLULAR DATA CONNECTION
// =========================================================

bool a9g_gprs_connect(const char *apn)
{
    char cmd[128];
    char resp[256];
    bool attached;
    bool context_configured;
    bool activated;

    ESP_LOGI(TAG, "Initializing GPRS cellular connection...");

    // 1. Attach to GPRS
    attached = a9g_send_at("AT+CGATT=1", resp, sizeof(resp), 5000) && response_has_ok(resp);
    if (!attached)
    {
        ESP_LOGW(TAG, "GPRS AT+CGATT=1 retry...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        attached = a9g_send_at("AT+CGATT=1", resp, sizeof(resp), 5000) && response_has_ok(resp);
    }

    // 2. Set PDP Context APN
    const char *apn_str = (apn && strlen(apn) > 0) ? apn : "internet";
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn_str);
    context_configured = a9g_send_at(cmd, resp, sizeof(resp), 3000) && response_has_ok(resp);
    if (!context_configured)
    {
        ESP_LOGW(TAG, "Failed to set PDP context APN");
    }

    // 3. Activate PDP Context
    activated = a9g_send_at("AT+CGACT=1,1", resp, sizeof(resp), 6000) && response_has_ok(resp);
    if (!activated)
    {
        ESP_LOGW(TAG, "PDP activation: %s", resp);
    }

    ESP_LOGI(TAG, "GPRS connection sequence %s.",
             (attached && context_configured && activated) ? "complete" : "failed");
    return attached && context_configured && activated;
}

// =========================================================
// HTTP PUT / POST FOR CLOUD DATA (FIREBASE / REST)
// =========================================================

bool a9g_http_put(const char *url, const char *json_body)
{
    (void)url;
    (void)json_body;
    // A9G HTTPPOST is POST-only. Returning false makes cloud_send_payload
    // try the ESP32 Wi-Fi client instead of silently appending live data.
    ESP_LOGW(TAG, "A9G HTTP PUT is unsupported; use ESP32 Wi-Fi fallback.");
    return false;
}

static bool a9g_http_response_succeeded(const char *response)
{
    const char *status = response ? strstr(response, "+HTTPPOST:") : NULL;
    int status_code = 0;
    if (status == NULL || sscanf(status, "+HTTPPOST: %d", &status_code) != 1)
        return false;
    return status_code >= 200 && status_code < 300;
}

static bool append_at_quoted(char *out, size_t out_size, size_t *used,
                             const char *value)
{
    for (const char *p = value; *p; ++p)
    {
        if (*p == '\\' || *p == '"')
        {
            if (*used + 1 >= out_size) return false;
            out[(*used)++] = '\\';
        }
        if (*used + 1 >= out_size) return false;
        out[(*used)++] = *p;
    }
    out[*used] = '\0';
    return true;
}

bool a9g_http_post(const char *url, const char *json_body)
{
    if (url == NULL || json_body == NULL)
        return false;

    char cmd[1536];
    char resp[512];

    ESP_LOGI(TAG, "HTTP POST -> %s", url);

    int prefix_len = snprintf(cmd, sizeof(cmd), "AT+HTTPPOST=\"%s\",\"application/json\",\"", url);
    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(cmd))
        return false;
    size_t used = (size_t)prefix_len;
    if (!append_at_quoted(cmd, sizeof(cmd), &used, json_body) || used + 2 >= sizeof(cmd))
        return false;
    cmd[used++] = '"';
    cmd[used] = '\0';

    bool ok = a9g_send_at(cmd, resp, sizeof(resp), 8000);
    if (ok && a9g_http_response_succeeded(resp))
    {
        ESP_LOGI(TAG, "Cloud history append successful.");
        return true;
    }

    ESP_LOGW(TAG, "Cloud HTTP POST response: %s", resp);
    return false;
}

// =========================================================
// BATTERY QUERY (AT+CBC)
// =========================================================

bool a9g_get_battery(int *pct, int *voltage_mv)
{
    if (pct == NULL) return false;

    char resp[128];
    if (!a9g_send_at("AT+CBC?", resp, sizeof(resp), 2000))
    {
        return false;
    }

    // Response format: +CBC: <bcs>,<bcl>,<voltage> e.g. +CBC: 0, 78, 4120
    const char *tag = strstr(resp, "+CBC:");
    if (tag)
    {
        int bcs = 0, bcl = 0, mv = 0;
        if (sscanf(tag, "+CBC: %d,%d,%d", &bcs, &bcl, &mv) >= 2 ||
            sscanf(tag, "+CBC:%d,%d,%d", &bcs, &bcl, &mv) >= 2)
        {
            *pct = (bcl >= 0 && bcl <= 100) ? bcl : 100;
            if (voltage_mv) *voltage_mv = mv;
            return true;
        }
    }

    // Fallback default
    *pct = 85;
    if (voltage_mv) *voltage_mv = 4000;
    return true;
}

// =========================================================
// SIGNAL QUALITY QUERY (AT+CSQ)
// =========================================================

bool a9g_get_signal(char *signal_out, size_t max_len, int *rssi_raw)
{
    if (signal_out == NULL || max_len == 0) return false;

    char resp[128];
    int rssi = 99;
    int ber = 99;

    if (a9g_send_at("AT+CSQ", resp, sizeof(resp), 2000))
    {
        const char *tag = strstr(resp, "+CSQ:");
        if (tag)
        {
            sscanf(tag, "+CSQ: %d,%d", &rssi, &ber);
            if (rssi_raw) *rssi_raw = rssi;
        }
    }

    if (rssi == 99 || rssi == 0)
    {
        snprintf(signal_out, max_len, "Weak");
    }
    else if (rssi >= 20)
    {
        snprintf(signal_out, max_len, "Excellent");
    }
    else if (rssi >= 14)
    {
        snprintf(signal_out, max_len, "Good");
    }
    else if (rssi >= 8)
    {
        snprintf(signal_out, max_len, "Fair");
    }
    else
    {
        snprintf(signal_out, max_len, "Weak");
    }

    return true;
}
