#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

// =========================================================
// A9G Module driver
// ESP32 UART2 <-> AI-Thinker A9G
// =========================================================

bool a9g_init(void);

bool a9g_enable_gps(void);

// Normal GPS request
bool a9g_try_get_gps_location(double *lat, double *lon);

// GPS request with a custom timeout.
// Used by SOS so the 10-second deadline is respected.
bool a9g_try_get_gps_location_timeout(double *lat,
                                      double *lon,
                                      uint32_t timeout_ms);

bool a9g_send_sms(const char *number, const char *message);

bool a9g_make_call(const char *number);

void a9g_hangup(void);

// =========================================================
// GPRS & CLOUD DATA TRANSMISSION
// =========================================================

// Connect to GPRS cellular data network
bool a9g_gprs_connect(const char *apn);

// Send HTTP PUT request with JSON payload (e.g. Firebase live.json / sos_alert.json)
bool a9g_http_put(const char *url, const char *json_body);

// Send HTTP POST request with JSON payload (e.g. Firebase history.json)
bool a9g_http_post(const char *url, const char *json_body);

// Query battery percentage (0-100%) and voltage in mV via AT+CBC
bool a9g_get_battery(int *pct, int *voltage_mv);

// Query cell signal strength and return descriptive string ("Good", "Weak", etc.)
bool a9g_get_signal(char *signal_out, size_t max_len, int *rssi_raw);

// Thread safety lock for A9G UART bus
void a9g_uart_lock(void);
void a9g_uart_unlock(void);

bool a9g_send_at(const char *command,
                 char *response_buf,
                 size_t buf_size,
                 uint32_t timeout_ms);
