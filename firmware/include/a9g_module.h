#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

// =========================================================
// A9G Module driver
// ESP32 UART2 <-> AI-Thinker A9G
// =========================================================

typedef enum
{
    A9G_GPS_FIX = 0,       // valid coordinates returned
    A9G_GPS_NO_FIX,        // modem answered, no fix yet
    A9G_GPS_NO_RESPONSE    // modem did not answer
} a9g_gps_result_t;

// Install UART (first call only) and verify the AT link.
// Does not require a SIM. Safe to call again to recover the modem.
bool a9g_init(void);

bool a9g_enable_gps(void);

// Single quick status queries (no retries)
bool a9g_check_sim(void);

// SIM state from the last a9g_check_sim(): "READY", "SIM PIN",
// "NOT INSERTED", "SIM FAILURE", "NO RESPONSE", ...
const char *a9g_sim_status(void);
bool a9g_check_registration(void);

// SMS text mode + GSM character set. Requires a ready SIM.
bool a9g_setup_sms(void);

// GPS request distinguishing "no fix" from "modem silent"
a9g_gps_result_t a9g_read_gps(double *lat, double *lon, uint32_t timeout_ms);

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

// Battery percentage (0-100) and voltage in mV via AT+CBC.
// Returns false when the modem does not report it (no fake defaults).
bool a9g_get_battery(int *pct, int *voltage_mv);

// Raw cell signal quality 0-31 via AT+CSQ; -1 when not detectable.
bool a9g_get_csq(int *csq);

// Consecutive AT commands that received no response at all.
uint32_t a9g_silent_command_count(void);

// Result of one NMEA listen window; negative values mean unknown.
typedef struct
{
    unsigned sentences;     // valid (checksummed) sentences received
    bool fix;
    double lat;
    double lon;
    double speed_kmh;
    double course_deg;
    int satellites;
    double hdop;
} a9g_nmea_t;

// Stream NMEA (AT+GPSRD) for listen_ms and parse RMC/GGA.
// Returns false when no sentence was received (feature unsupported or modem
// silent), so the caller can fall back to AT+LOCATION.
bool a9g_read_nmea(a9g_nmea_t *out, uint32_t listen_ms);

// Register the calling task as the background poller. While preemption is
// on, its commands return early so the SOS sequence gets the UART at once.
void a9g_set_background_task(void);
void a9g_preempt_background(bool preempt);

// Thread safety lock for A9G UART bus
void a9g_uart_lock(void);
void a9g_uart_unlock(void);

bool a9g_send_at(const char *command,
                 char *response_buf,
                 size_t buf_size,
                 uint32_t timeout_ms);
