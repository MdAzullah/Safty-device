#pragma once

#include <stdbool.h>
#include <stdint.h>

// =========================================================
// Shared device state
// Each subsystem (modem, GPS, cellular, battery, Wi-Fi) updates only its
// own fields, so a failure in one never blocks the others from being
// reported. All access is mutex protected; readers take a snapshot copy.
// =========================================================

typedef struct
{
    // A9G modem link
    bool modem_responding;
    bool gps_enabled;

    // GPS
    bool has_location;          // a fix has been received since boot
    double latitude;            // last known position
    double longitude;
    int64_t fix_time_ms;        // uptime when the last fix was received
    uint32_t fix_seq;           // increments on every new fix
    double speed_kmh;           // from NMEA, or derived from consecutive fixes
    bool speed_valid;
    double course_deg;          // -1 unknown
    int satellites;             // satellites used, -1 unknown
    double hdop;                // horizontal dilution of precision, -1 unknown

    // Cellular
    bool sim_ready;
    char sim_status[24];        // modem's SIM answer, e.g. "READY", "SIM PIN"
    bool gsm_registered;
    int gsm_csq;                // 0-31, -1 unknown

    // Supply / battery
    int battery_pct;            // -1 unknown
    int battery_mv;             // -1 unknown

    // Network
    bool wifi_connected;
    int wifi_rssi;              // dBm, 0 unknown

    // Cloud
    int64_t last_cloud_ok_ms;   // uptime of last successful upload, 0 never

    // Emergency
    bool sos_active;

    // Diagnostics
    uint32_t boot_count;
    const char *reset_reason;
} device_state_t;

// Optional extra data accompanying a fix; negative values mean unknown.
typedef struct
{
    double speed_kmh;
    double course_deg;
    int satellites;
    double hdop;
} gps_fix_extra_t;

void device_state_init(void);

// Copy the full state atomically.
void device_state_snapshot(device_state_t *out);

// Record a new GPS fix; updates position, fix time/sequence, speed and
// quality. `extra` may be NULL. Returns false when the fix is rejected as
// an implausible jump from the previous fresh fix.
bool device_state_record_fix(double lat, double lon, const gps_fix_extra_t *extra);

// Satellites/HDOP while searching (no fix).
void device_state_set_gps_quality(int satellites, double hdop);

// True when a fix exists and is younger than GPS_FIX_STALE_MS.
bool device_state_fix_is_fresh(const device_state_t *state, int64_t now_ms);

void device_state_set_modem(bool responding);
void device_state_set_gps_enabled(bool enabled);
void device_state_set_cellular(bool sim_ready, const char *sim_status, bool registered, int csq);
void device_state_set_battery(int pct, int mv);
void device_state_set_wifi(bool connected, int rssi);
void device_state_mark_cloud_ok(void);
void device_state_set_sos_active(bool active);
void device_state_set_boot_info(uint32_t boot_count, const char *reset_reason);

int64_t device_uptime_ms(void);
