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

// Consecutive commands that received no bytes at all (modem silent).
static volatile uint32_t s_silent_commands = 0;

// Background polling task; its commands are cut short while SOS needs the
// modem (see a9g_preempt_background).
static TaskHandle_t s_background_task = NULL;
static volatile bool s_preempt_background = false;
static bool s_last_read_aborted = false;

// Raw AT traffic is very chatty with periodic polling; enable when debugging.
#if A9G_VERBOSE_AT
#define A9G_LOG_AT(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#else
#define A9G_LOG_AT(tag, ...) ESP_LOGD(tag, __VA_ARGS__)
#endif

void a9g_uart_lock(void) {
  if (s_uart_mutex == NULL) {
    s_uart_mutex = xSemaphoreCreateMutex();
  }
  if (s_uart_mutex != NULL) {
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
  }
}

void a9g_uart_unlock(void) {
  if (s_uart_mutex != NULL) {
    xSemaphoreGive(s_uart_mutex);
  }
}

// =========================================================
// TIME
// =========================================================

static int64_t now_us(void) { return esp_timer_get_time(); }

// =========================================================
// SOS PRIORITY
// =========================================================

void a9g_set_background_task(void) {
  s_background_task = xTaskGetCurrentTaskHandle();
}

void a9g_preempt_background(bool preempt) { s_preempt_background = preempt; }

static bool should_abort(void) {
  return s_preempt_background && s_background_task != NULL &&
         xTaskGetCurrentTaskHandle() == s_background_task;
}

// =========================================================
// READ A9G RESPONSE
// =========================================================

static int read_response(char *buf, size_t buf_size, uint32_t timeout_ms) {
  if (buf == NULL || buf_size == 0)
    return 0;

  memset(buf, 0, buf_size);

  size_t total = 0;

  int64_t start = now_us();
  int64_t last_byte = start;

  // Generous idle timeout (4.0s) so cellular SMS / Network delays do not get
  // aborted early
  const int64_t idle_timeout_us = 4000 * 1000;

  s_last_read_aborted = false;

  while ((now_us() - start) < ((int64_t)timeout_ms * 1000)) {
    if (should_abort()) {
      s_last_read_aborted = true;
      break;
    }

    uint8_t byte;

    int len = uart_read_bytes(A9G_UART_PORT, &byte, 1, pdMS_TO_TICKS(50));

    if (len > 0) {
      if (total < buf_size - 1) {
        buf[total++] = (char)byte;
        buf[total] = '\0';
      }

      last_byte = now_us();

      // Normal command completion keywords
      if (strstr(buf, "\r\nOK\r\n") || strstr(buf, "\nOK\r\n") ||
          strstr(buf, "\nOK\n") || strstr(buf, "\r\nERROR\r\n") ||
          strstr(buf, "\nERROR\n") || strstr(buf, "+CME ERROR") ||
          strstr(buf, "+CMS ERROR") || strstr(buf, "+CMGS:") ||
          strstr(buf, "GPS NOT FIX")) {
        // Brief pause to read any remaining trailer bytes
        vTaskDelay(pdMS_TO_TICKS(20));
        while (uart_read_bytes(A9G_UART_PORT, &byte, 1, pdMS_TO_TICKS(10)) >
               0) {
          if (total < buf_size - 1) {
            buf[total++] = (char)byte;
            buf[total] = '\0';
          }
        }
        break;
      }
    } else {
      if (total > 0 && (now_us() - last_byte) > idle_timeout_us) {
        break;
      }
    }
  }

  return (int)total;
}

// =========================================================
// RESPONSE HELPERS
// =========================================================

static bool response_has_ok(const char *response) {
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

// Last SIM state reported by the modem, e.g. "READY", "SIM PIN",
// "NOT INSERTED", "SIM FAILURE", "NO RESPONSE".
static char s_sim_status[24] = "UNKNOWN";

// Parses an AT+CPIN? reply into s_sim_status. A9G firmware answers either
// "+CPIN: READY" or "+CPIN:READY", and reports a missing/failed SIM as
// "+CME ERROR: <n>".
static bool sim_is_ready(const char *response) {
  if (response == NULL || response[0] == '\0') {
    snprintf(s_sim_status, sizeof(s_sim_status), "NO RESPONSE");
    return false;
  }

  const char *cpin = strstr(response, "+CPIN:");
  if (cpin != NULL) {
    cpin += 6;
    while (*cpin == ' ')
      cpin++;

    size_t n = 0;
    while (cpin[n] != '\0' && cpin[n] != '\r' && cpin[n] != '\n' &&
           n < sizeof(s_sim_status) - 1) {
      s_sim_status[n] = cpin[n];
      n++;
    }
    s_sim_status[n] = '\0';

    return strncmp(s_sim_status, "READY", 5) == 0;
  }

  const char *cme = strstr(response, "+CME ERROR:");
  if (cme != NULL) {
    int code = atoi(cme + 11);
    switch (code) {
    case 10:
      snprintf(s_sim_status, sizeof(s_sim_status), "NOT INSERTED");
      break;
    case 13:
      snprintf(s_sim_status, sizeof(s_sim_status), "SIM FAILURE");
      break;
    case 14:
      snprintf(s_sim_status, sizeof(s_sim_status), "SIM BUSY");
      break;
    case 15:
      snprintf(s_sim_status, sizeof(s_sim_status), "SIM WRONG");
      break;
    default:
      snprintf(s_sim_status, sizeof(s_sim_status), "CME ERROR %d", code);
      break;
    }
    return false;
  }

  snprintf(s_sim_status, sizeof(s_sim_status), "UNRECOGNIZED");
  return false;
}

const char *a9g_sim_status(void) { return s_sim_status; }

static bool network_is_registered(const char *response) {
  if (response == NULL)
    return false;

  const char *creg = strstr(response, "+CREG:");

  if (creg == NULL)
    return false;

  int n = 0;
  int stat = 0;

  if (sscanf(creg, "+CREG: %d,%d", &n, &stat) != 2 &&
      sscanf(creg, "+CREG:%d,%d", &n, &stat) != 2) {
    return false;
  }

  return (stat == 1 || stat == 5);
}

// =========================================================
// SEND AT COMMAND (THREAD SAFE)
// =========================================================

bool a9g_send_at(const char *command, char *response_buf, size_t buf_size,
                 uint32_t timeout_ms) {
  if (command == NULL || response_buf == NULL || buf_size == 0) {
    return false;
  }

  a9g_uart_lock();

  // Remove stale UART data
  uart_flush_input(A9G_UART_PORT);

  A9G_LOG_AT(TAG, "TX: %s", command);

  uart_write_bytes(A9G_UART_PORT, command, strlen(command));

  uart_write_bytes(A9G_UART_PORT, "\r\n", 2);

  int len = read_response(response_buf, buf_size, timeout_ms);

  A9G_LOG_AT(TAG, "RX: %s", response_buf);

  // A command cut short for SOS says nothing about modem health.
  if (len > 0) {
    s_silent_commands = 0;
  } else if (!s_last_read_aborted) {
    s_silent_commands++;
  }

  a9g_uart_unlock();

  return len > 0;
}

uint32_t a9g_silent_command_count(void) { return s_silent_commands; }

// =========================================================
// INITIALIZE A9G
// Installs the UART once and verifies the AT link. SIM, network and SMS
// are configured separately so a missing SIM never blocks GPS or Wi-Fi.
// Safe to call repeatedly to recover a modem that stopped responding.
// =========================================================

bool a9g_init(void) {
  if (!uart_is_driver_installed(A9G_UART_PORT)) {
    uart_config_t uart_config = {.baud_rate = A9G_BAUD_RATE,
                                 .data_bits = UART_DATA_8_BITS,
                                 .parity = UART_PARITY_DISABLE,
                                 .stop_bits = UART_STOP_BITS_1,
                                 .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                 .source_clk = UART_SCLK_DEFAULT};

    ESP_ERROR_CHECK(uart_driver_install(A9G_UART_PORT, A9G_UART_BUF_SIZE,
                                        A9G_UART_BUF_SIZE, 0, NULL, 0));

    ESP_ERROR_CHECK(uart_param_config(A9G_UART_PORT, &uart_config));

    ESP_ERROR_CHECK(uart_set_pin(A9G_UART_PORT, A9G_TX_PIN, A9G_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    vTaskDelay(pdMS_TO_TICKS(BOOT_SETTLE_DELAY_MS));
  }

  char resp[256];

  for (int retry = 0; retry < 5; retry++) {
    a9g_send_at("ATE0", resp, sizeof(resp), 1500);

    if (a9g_send_at("AT", resp, sizeof(resp), 2000) && response_has_ok(resp)) {
      ESP_LOGI(TAG, "A9G UART communication OK.");

      return true;
    }

    ESP_LOGW(TAG, "A9G AT response not received. Retry %d/5", retry + 1);

    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  ESP_LOGE(
      TAG,
      "A9G communication failed. Check power, TX/RX wiring and baud rate.");

  return false;
}

// =========================================================
// CELLULAR STATUS (single, quick queries)
// =========================================================

bool a9g_check_sim(void) {
  char resp[128];

  if (!a9g_send_at("AT+CPIN?", resp, sizeof(resp), 2000)) {
    resp[0] = '\0';
  }

  bool ready = sim_is_ready(resp);

  if (!ready) {
    // Raw reply helps diagnose formats this parser does not know.
    for (char *p = resp; *p; ++p) {
      if (*p == '\r' || *p == '\n')
        *p = ' ';
    }
    ESP_LOGW(TAG, "SIM not ready (%s). Modem reply: %s", s_sim_status, resp);
  }

  return ready;
}

bool a9g_check_registration(void) {
  char resp[128];

  return a9g_send_at("AT+CREG?", resp, sizeof(resp), 2000) &&
         network_is_registered(resp);
}

bool a9g_setup_sms(void) {
  char resp[128];

  if (!a9g_send_at("AT+CMGF=1", resp, sizeof(resp), 2000) ||
      !response_has_ok(resp)) {
    ESP_LOGW(TAG, "Could not enable SMS text mode.");

    return false;
  }

  if (!a9g_send_at("AT+CSCS=\"GSM\"", resp, sizeof(resp), 2000) ||
      !response_has_ok(resp)) {
    ESP_LOGW(TAG, "Could not set GSM character set.");
  }

  ESP_LOGI(TAG, "A9G SMS service ready.");
  return true;
}

// =========================================================
// ENABLE GPS
// =========================================================

bool a9g_enable_gps(void) {
  char resp[128];

  bool ok = a9g_send_at("AT+GPS=1", resp, sizeof(resp), 3000) &&
            response_has_ok(resp);

  if (ok) {
    ESP_LOGI(TAG, "GPS enabled.");
  } else {
    ESP_LOGE(TAG, "Failed to enable GPS.");
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

static bool parse_location(const char *response, double *lat, double *lon) {
  if (response == NULL || lat == NULL || lon == NULL) {
    return false;
  }

  // -----------------------------------------------------
  // First try +LOCATION: latitude,longitude
  // -----------------------------------------------------

  const char *location_tag = strstr(response, "+LOCATION:");

  if (location_tag != NULL) {
    double parsed_lat = 0.0;
    double parsed_lon = 0.0;

    if (sscanf(location_tag, "+LOCATION: %lf , %lf", &parsed_lat,
               &parsed_lon) == 2 ||
        sscanf(location_tag, "+LOCATION:%lf,%lf", &parsed_lat, &parsed_lon) ==
            2) {
      if (parsed_lat >= -90.0 && parsed_lat <= 90.0 && parsed_lon >= -180.0 &&
          parsed_lon <= 180.0 && !(parsed_lat == 0.0 && parsed_lon == 0.0)) {
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

  strncpy(copy, response, sizeof(copy) - 1);

  copy[sizeof(copy) - 1] = '\0';

  char *saveptr = NULL;

  char *line = strtok_r(copy, "\r\n", &saveptr);

  while (line != NULL) {
    double parsed_lat = 0.0;
    double parsed_lon = 0.0;

    if (sscanf(line, " %lf , %lf", &parsed_lat, &parsed_lon) == 2) {
      if (parsed_lat >= -90.0 && parsed_lat <= 90.0 && parsed_lon >= -180.0 &&
          parsed_lon <= 180.0 && !(parsed_lat == 0.0 && parsed_lon == 0.0)) {
        *lat = parsed_lat;
        *lon = parsed_lon;

        return true;
      }
    }

    line = strtok_r(NULL, "\r\n", &saveptr);
  }

  return false;
}

// =========================================================
// GPS REQUEST WITH TIMEOUT
// =========================================================

a9g_gps_result_t a9g_read_gps(double *lat, double *lon, uint32_t timeout_ms) {
  if (lat == NULL || lon == NULL)
    return A9G_GPS_NO_RESPONSE;

  char resp[256];

  bool received = a9g_send_at("AT+LOCATION=2", resp, sizeof(resp), timeout_ms);

  if (!received) {
    ESP_LOGW(TAG, "GPS command produced no response.");

    return A9G_GPS_NO_RESPONSE;
  }

  if (parse_location(resp, lat, lon)) {
    return A9G_GPS_FIX;
  }

  return A9G_GPS_NO_FIX;
}

bool a9g_try_get_gps_location_timeout(double *lat, double *lon,
                                      uint32_t timeout_ms) {
  return a9g_read_gps(lat, lon, timeout_ms) == A9G_GPS_FIX;
}

// =========================================================
// NORMAL GPS REQUEST
// =========================================================

bool a9g_try_get_gps_location(double *lat, double *lon) {
  return a9g_try_get_gps_location_timeout(lat, lon, AT_DEFAULT_TIMEOUT_MS);
}

// =========================================================
// WAIT FOR SMS PROMPT
// =========================================================

static bool wait_for_sms_prompt(uint32_t timeout_ms) {
  int64_t start = now_us();

  while ((now_us() - start) < ((int64_t)timeout_ms * 1000)) {
    uint8_t byte;

    if (uart_read_bytes(A9G_UART_PORT, &byte, 1, pdMS_TO_TICKS(50)) > 0) {
      if (byte == '>') {
        return true;
      }
    }
  }

  return false;
}

// =========================================================
// SEND SMS
// =========================================================

bool a9g_send_sms(const char *number, const char *message) {
  if (number == NULL || message == NULL) {
    return false;
  }

  char resp[256];

  // -----------------------------------------------------
  // SMS TEXT MODE
  // -----------------------------------------------------

  if (!a9g_send_at("AT+CMGF=1", resp, sizeof(resp), 3000) ||
      !response_has_ok(resp)) {
    ESP_LOGE(TAG, "SMS: failed to enter text mode.");

    return false;
  }

  a9g_uart_lock();

  uart_flush_input(A9G_UART_PORT);

  // -----------------------------------------------------
  // CMGS
  // -----------------------------------------------------

  char command[64];

  snprintf(command, sizeof(command), "AT+CMGS=\"%s\"", number);

  ESP_LOGI(TAG, "SMS TX: %s", command);

  uart_write_bytes(A9G_UART_PORT, command, strlen(command));

  uart_write_bytes(A9G_UART_PORT, "\r\n", 2);

  // -----------------------------------------------------
  // WAIT FOR >
  // -----------------------------------------------------

  if (!wait_for_sms_prompt(5000)) {
    ESP_LOGE(TAG, "SMS failed: no '>' prompt.");

    a9g_uart_unlock();
    return false;
  }

  ESP_LOGI(TAG, "SMS prompt received.");

  // -----------------------------------------------------
  // SEND MESSAGE
  // -----------------------------------------------------

  uart_write_bytes(A9G_UART_PORT, message, strlen(message));

  // CTRL+Z
  uint8_t ctrl_z = 0x1A;

  uart_write_bytes(A9G_UART_PORT, (const char *)&ctrl_z, 1);

  // -----------------------------------------------------
  // WAIT FOR RESULT
  // -----------------------------------------------------

  int len = read_response(resp, sizeof(resp), SMS_SEND_TIMEOUT_MS);

  a9g_uart_unlock();

  ESP_LOGI(TAG, "SMS response: %s", resp);

  if (len <= 0) {
    ESP_LOGE(TAG, "SMS failed: no response.");

    return false;
  }

  if (strstr(resp, "+CMGS:") != NULL && strstr(resp, "ERROR") == NULL) {
    ESP_LOGI(TAG, "SMS SENT SUCCESSFULLY.");

    return true;
  }

  if (strstr(resp, "OK") != NULL && strstr(resp, "ERROR") == NULL) {
    ESP_LOGI(TAG, "SMS SENT.");

    return true;
  }

  ESP_LOGE(TAG, "SMS sending failed.");

  return false;
}

// =========================================================
// MAKE CALL
// =========================================================

bool a9g_make_call(const char *number) {
  if (number == NULL)
    return false;

  char command[64];
  char resp[128];

  snprintf(command, sizeof(command), "ATD%s;", number);

  ESP_LOGW(TAG, "Calling guardian: %s", number);

  bool got_response = a9g_send_at(command, resp, sizeof(resp), 5000);

  if (!got_response) {
    ESP_LOGE(TAG, "Call: no response from A9G.");

    return false;
  }

  // ERROR means command rejected.
  if (strstr(resp, "ERROR") != NULL || strstr(resp, "+CME ERROR") != NULL) {
    ESP_LOGE(TAG, "Call command rejected.");

    return false;
  }

  if (strstr(resp, "OK") != NULL) {
    ESP_LOGI(TAG, "Emergency call initiated.");

    return true;
  }

  ESP_LOGW(TAG, "Unexpected call response: %s", resp);

  return false;
}

// =========================================================
// HANG UP
// =========================================================

void a9g_hangup(void) {
  char resp[64];

  a9g_send_at("ATH", resp, sizeof(resp), 3000);
}

// =========================================================
// BATTERY QUERY (AT+CBC)
// =========================================================

bool a9g_get_battery(int *pct, int *voltage_mv) {
  if (pct == NULL)
    return false;

  char resp[128];
  if (!a9g_send_at("AT+CBC?", resp, sizeof(resp), 2000)) {
    return false;
  }

  // Response format: +CBC: <bcs>,<bcl>,<voltage> e.g. +CBC: 0, 78, 4120
  const char *tag = strstr(resp, "+CBC:");
  if (tag == NULL) {
    return false;
  }

  int bcs = 0, bcl = -1, mv = -1;
  if (sscanf(tag, "+CBC: %d , %d , %d", &bcs, &bcl, &mv) < 2 &&
      sscanf(tag, "+CBC:%d,%d,%d", &bcs, &bcl, &mv) < 2) {
    return false;
  }

  if (bcl < 0 || bcl > 100) {
    return false;
  }

  *pct = bcl;
  if (voltage_mv)
    *voltage_mv = (mv > 0) ? mv : -1;
  return true;
}

// =========================================================
// SIGNAL QUALITY QUERY (AT+CSQ)
// =========================================================

bool a9g_get_csq(int *csq) {
  if (csq == NULL)
    return false;

  char resp[128];
  if (!a9g_send_at("AT+CSQ", resp, sizeof(resp), 2000)) {
    return false;
  }

  const char *tag = strstr(resp, "+CSQ:");
  int rssi = 99;
  int ber = 99;
  if (tag == NULL || (sscanf(tag, "+CSQ: %d , %d", &rssi, &ber) < 1 &&
                      sscanf(tag, "+CSQ:%d,%d", &rssi, &ber) < 1)) {
    return false;
  }

  // 99 = not known or not detectable
  *csq = (rssi >= 0 && rssi <= 31) ? rssi : -1;
  return true;
}

// =========================================================
// NMEA (AT+GPSRD)
// Receiver-reported fix status, speed, course, satellites and HDOP.
// The stream is enabled only for a short listen window, under the UART
// lock, then switched off again so AT command parsing is unaffected.
// =========================================================

#define NMEA_MAX_FIELDS 20

// Validates "*hh" checksum of a sentence starting at '$'.
static bool nmea_checksum_ok(const char *sentence) {
  const char *star = strchr(sentence, '*');
  if (star == NULL || star[1] == '\0' || star[2] == '\0')
    return false;

  uint8_t sum = 0;
  for (const char *p = sentence + 1; p < star; ++p)
    sum ^= (uint8_t)*p;

  char hex[3] = {star[1], star[2], '\0'};
  return sum == (uint8_t)strtol(hex, NULL, 16);
}

// Splits on ',' keeping empty fields. Modifies `s`. Returns field count.
static int nmea_split(char *s, char *fields[], int max_fields) {
  int count = 0;
  char *p = s;

  while (count < max_fields) {
    fields[count++] = p;
    char *comma = strchr(p, ',');
    if (comma == NULL)
      break;
    *comma = '\0';
    p = comma + 1;
  }

  // Strip the checksum from the last field
  char *star = strchr(fields[count - 1], '*');
  if (star != NULL)
    *star = '\0';

  return count;
}

// "ddmm.mmmm" + hemisphere -> signed decimal degrees
static bool nmea_coord(const char *value, const char *hemi, double *out) {
  if (value == NULL || hemi == NULL || value[0] == '\0' || hemi[0] == '\0')
    return false;

  double raw = atof(value);
  int degrees = (int)(raw / 100.0);
  double result = degrees + (raw - degrees * 100.0) / 60.0;

  if (hemi[0] == 'S' || hemi[0] == 'W')
    result = -result;

  *out = result;
  return true;
}

static void nmea_parse_sentence(char *sentence, a9g_nmea_t *out) {
  if (!nmea_checksum_ok(sentence))
    return;

  char *f[NMEA_MAX_FIELDS];
  int n = nmea_split(sentence, f, NMEA_MAX_FIELDS);
  if (n < 1 || strlen(f[0]) < 6)
    return;

  // Talker-independent type: $GPRMC, $GNRMC, $BDGGA ...
  const char *type = f[0] + 3;
  out->sentences++;

  if (strcmp(type, "RMC") == 0 && n >= 9) {
    // 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W, 7 knots, 8 course
    double lat = 0.0;
    double lon = 0.0;

    if (f[2][0] == 'A' && nmea_coord(f[3], f[4], &lat) &&
        nmea_coord(f[5], f[6], &lon) && !(lat == 0.0 && lon == 0.0)) {
      out->fix = true;
      out->lat = lat;
      out->lon = lon;

      if (f[7][0] != '\0')
        out->speed_kmh = atof(f[7]) * 1.852;
      if (f[8][0] != '\0')
        out->course_deg = atof(f[8]);
    }
  } else if (strcmp(type, "GGA") == 0 && n >= 9) {
    // 6 quality, 7 satellites, 8 HDOP
    if (f[7][0] != '\0')
      out->satellites = atoi(f[7]);
    if (f[8][0] != '\0')
      out->hdop = atof(f[8]);

    double lat = 0.0;
    double lon = 0.0;

    if (!out->fix && atoi(f[6]) > 0 && nmea_coord(f[2], f[3], &lat) &&
        nmea_coord(f[4], f[5], &lon) && !(lat == 0.0 && lon == 0.0)) {
      out->fix = true;
      out->lat = lat;
      out->lon = lon;
    }
  }
}

// A line may carry a "+GPSRD:" prefix or several sentences.
static void nmea_parse_line(char *line, a9g_nmea_t *out) {
  char *start = strchr(line, '$');

  while (start != NULL) {
    char *next = strchr(start + 1, '$');
    if (next != NULL)
      *next = '\0';

    // Parsing only modifies this sentence's own bytes.
    nmea_parse_sentence(start, out);

    if (next == NULL)
      break;
    *next = '$';
    start = next;
  }
}

static void uart_write_cmd(const char *command) {
  uart_write_bytes(A9G_UART_PORT, command, strlen(command));
  uart_write_bytes(A9G_UART_PORT, "\r\n", 2);
}

bool a9g_read_nmea(a9g_nmea_t *out, uint32_t listen_ms) {
  if (out == NULL)
    return false;

  memset(out, 0, sizeof(*out));
  out->speed_kmh = -1.0;
  out->course_deg = -1.0;
  out->satellites = -1;
  out->hdop = -1.0;

  char line[160];
  size_t used = 0;
  size_t bytes = 0;

  a9g_uart_lock();

  uart_flush_input(A9G_UART_PORT);
  uart_write_cmd("AT+GPSRD=1");

  int64_t end = now_us() + (int64_t)listen_ms * 1000;

  while (now_us() < end && !should_abort()) {
    uint8_t b;
    if (uart_read_bytes(A9G_UART_PORT, &b, 1, pdMS_TO_TICKS(20)) <= 0)
      continue;

    bytes++;

    if (b == '\r' || b == '\n') {
      if (used > 0) {
        line[used] = '\0';
        nmea_parse_line(line, out);
        used = 0;
      }
    } else if (used < sizeof(line) - 1) {
      line[used++] = (char)b;
    }
  }

  // Stop the stream and drain its tail before releasing the UART.
  uart_write_cmd("AT+GPSRD=0");
  uint8_t discard;
  int64_t drain_end = now_us() + 300 * 1000;
  while (now_us() < drain_end &&
         uart_read_bytes(A9G_UART_PORT, &discard, 1, pdMS_TO_TICKS(30)) > 0) {
  }
  uart_flush_input(A9G_UART_PORT);

  if (bytes > 0) {
    s_silent_commands = 0;
  } else if (!should_abort()) {
    s_silent_commands++;
  }

  a9g_uart_unlock();

  A9G_LOG_AT(TAG, "NMEA: %u sentences, fix=%d, sats=%d, hdop=%.1f",
             (unsigned)out->sentences, out->fix, out->satellites, out->hdop);

  return out->sentences > 0;
}
