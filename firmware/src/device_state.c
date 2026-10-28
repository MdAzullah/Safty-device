#include "device_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pins_config.h"

static const char *TAG = "STATE";

static SemaphoreHandle_t s_lock = NULL;
static device_state_t s_state;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

int64_t device_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void device_state_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.gsm_csq = -1;
    s_state.battery_pct = -1;
    s_state.battery_mv = -1;
    s_state.course_deg = -1.0;
    s_state.satellites = -1;
    s_state.hdop = -1.0;
    s_state.reset_reason = "unknown";
}

void device_state_snapshot(device_state_t *out)
{
    if (out == NULL)
        return;

    lock();
    *out = s_state;
    unlock();
}

// Great-circle distance in metres.
static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 6371000.0;
    const double to_rad = M_PI / 180.0;

    double dlat = (lat2 - lat1) * to_rad;
    double dlon = (lon2 - lon1) * to_rad;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * to_rad) * cos(lat2 * to_rad) *
               sin(dlon / 2) * sin(dlon / 2);

    return 2.0 * r * atan2(sqrt(a), sqrt(1.0 - a));
}

bool device_state_record_fix(double lat, double lon, const gps_fix_extra_t *extra)
{
    int64_t now = device_uptime_ms();

    lock();

    double derived_kmh = -1.0;

    if (s_state.has_location)
    {
        int64_t dt_ms = now - s_state.fix_time_ms;

        // Only compare with a recent fix; a long gap says nothing about
        // current speed and legitimately allows a large jump.
        if (dt_ms >= 1000 && dt_ms <= GPS_FIX_STALE_MS)
        {
            double dist = haversine_m(s_state.latitude, s_state.longitude, lat, lon);
            double kmh = (dist / ((double)dt_ms / 1000.0)) * 3.6;

            if (kmh > GPS_MAX_PLAUSIBLE_KMH)
            {
                unlock();
                ESP_LOGW(TAG, "GPS fix rejected: %.0f m jump in %lld ms", dist, dt_ms);
                return false;
            }

            derived_kmh = (dist < GPS_JITTER_DISTANCE_M) ? 0.0 : kmh;
        }
    }

    // Receiver-reported speed is preferred over the derived estimate.
    if (extra != NULL && extra->speed_kmh >= 0.0)
    {
        s_state.speed_kmh = extra->speed_kmh;
        s_state.speed_valid = true;
    }
    else if (derived_kmh >= 0.0)
    {
        s_state.speed_kmh = derived_kmh;
        s_state.speed_valid = true;
    }
    else
    {
        s_state.speed_valid = false;
    }

    s_state.course_deg = (extra != NULL) ? extra->course_deg : -1.0;
    s_state.satellites = (extra != NULL) ? extra->satellites : -1;
    s_state.hdop = (extra != NULL) ? extra->hdop : -1.0;

    s_state.latitude = lat;
    s_state.longitude = lon;
    s_state.fix_time_ms = now;
    s_state.has_location = true;
    uint32_t seq = ++s_state.fix_seq;

    unlock();

    ESP_LOGI(TAG, "GPS fix #%lu: %.7f, %.7f", (unsigned long)seq, lat, lon);
    return true;
}

void device_state_set_gps_quality(int satellites, double hdop)
{
    lock();
    s_state.satellites = satellites;
    s_state.hdop = hdop;
    unlock();
}

void device_state_set_boot_info(uint32_t boot_count, const char *reset_reason)
{
    lock();
    s_state.boot_count = boot_count;
    s_state.reset_reason = reset_reason;
    unlock();
}

bool device_state_fix_is_fresh(const device_state_t *state, int64_t now_ms)
{
    return state != NULL &&
           state->has_location &&
           (now_ms - state->fix_time_ms) <= GPS_FIX_STALE_MS;
}

void device_state_set_modem(bool responding)
{
    lock();
    s_state.modem_responding = responding;
    if (!responding)
    {
        // Values reported by an unresponsive modem are no longer current.
        s_state.gps_enabled = false;
        s_state.sim_ready = false;
        s_state.gsm_registered = false;
        s_state.gsm_csq = -1;
    }
    unlock();
}

void device_state_set_gps_enabled(bool enabled)
{
    lock();
    s_state.gps_enabled = enabled;
    unlock();
}

void device_state_set_cellular(bool sim_ready, const char *sim_status, bool registered, int csq)
{
    lock();
    s_state.sim_ready = sim_ready;
    snprintf(s_state.sim_status, sizeof(s_state.sim_status), "%s", sim_status ? sim_status : "");
    s_state.gsm_registered = registered;
    s_state.gsm_csq = csq;
    unlock();
}

void device_state_set_battery(int pct, int mv)
{
    lock();
    s_state.battery_pct = pct;
    s_state.battery_mv = mv;
    unlock();
}

void device_state_set_wifi(bool connected, int rssi)
{
    lock();
    s_state.wifi_connected = connected;
    s_state.wifi_rssi = connected ? rssi : 0;
    unlock();
}

void device_state_mark_cloud_ok(void)
{
    int64_t now = device_uptime_ms();
    lock();
    s_state.last_cloud_ok_ms = now;
    unlock();
}

void device_state_set_sos_active(bool active)
{
    lock();
    s_state.sos_active = active;
    unlock();
}
