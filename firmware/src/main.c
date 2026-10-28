/* =========================================================
   Guardian Beacon - ESP32 + AI-Thinker A9G
   ESP-IDF Safety Firmware

   ARCHITECTURE
   ---------------------------------------------------------
   Independent tasks, so one failing subsystem never stops
   the others from reporting:

     safety_task  (prio 10) SOS button (long press), SOS sequence, status LED
     cloud_task   (prio 4)  Wi-Fi + Firebase heartbeat/history/SOS
     modem_task   (prio 3)  A9G supervision, GPS, SIM/signal, battery

   The device heartbeat reaches the website every
   CLOUD_HEARTBEAT_MS while Wi-Fi is up, with or without
   GPS fix, SIM card, or a responding modem.

   SOS:
     - Alert is published to the website immediately.
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

     - Website alert is updated with the final location and
       SMS/call results.

   STATUS LED (outside SOS):
     short blink every 3 s  -> cloud uploads OK
     1 Hz blink             -> no Wi-Fi or uploads failing

   ========================================================= */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "a9g_module.h"
#include "cloud_sync.h"
#include "device_state.h"
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

static gps_location_t sos_current_location = {0};

// Written by safety_task, read by modem_task.
static volatile sos_state_t sos_state =
    SOS_STATE_NORMAL;

static int64_t sos_started_ms = 0;

static int64_t last_sos_gps_attempt_ms = 0;

// Written by modem_task, read by safety_task.
static volatile bool a9g_ready = false;

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
// LAST KNOWN LOCATION
// =========================================================

static gps_location_t last_known_location(void)
{
    device_state_t state;
    device_state_snapshot(&state);

    gps_location_t location = {
        .latitude = state.latitude,
        .longitude = state.longitude,
        .valid = state.has_location,
        .fix_time_ms = state.fix_time_ms
    };

    return location;
}

// =========================================================
// REQUEST GPS
// =========================================================
//
// IMPORTANT:
// The shared last known location is updated ONLY when
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

    if (!device_state_record_fix(
            lat,
            lon,
            NULL))
    {
        return false;
    }

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
            now_ms();
    }

    return true;
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
    gps_location_t last_live_location =
        last_known_location();

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

    // Cut short any background modem command so SOS gets the UART now.
    a9g_preempt_background(true);

    // Website alert first: queued for the cloud task, never blocks.
    device_state_set_sos_active(true);

    cloud_sync_sos_triggered();

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

        bool call_ok =
            a9g_make_call(
                PARENT_NUMBER
            );

        gps_location_t last_live_location =
            last_known_location();

        cloud_sync_sos_update(
            last_live_location.valid ? SOS_LOCATION_LAST_KNOWN : SOS_LOCATION_NONE,
            last_live_location.latitude,
            last_live_location.longitude,
            false,
            call_ok
        );

        device_state_set_sos_active(false);

        a9g_preempt_background(false);

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

    gps_location_t last_live_location =
        last_known_location();

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

            // Attach the final location and results to the website alert.
            if (sos_current_location.valid)
            {
                cloud_sync_sos_update(
                    SOS_LOCATION_CURRENT,
                    sos_current_location.latitude,
                    sos_current_location.longitude,
                    sos_sms_ok,
                    call_ok
                );
            }
            else if (last_live_location.valid)
            {
                cloud_sync_sos_update(
                    SOS_LOCATION_LAST_KNOWN,
                    last_live_location.latitude,
                    last_live_location.longitude,
                    sos_sms_ok,
                    call_ok
                );
            }
            else
            {
                cloud_sync_sos_update(
                    SOS_LOCATION_NONE,
                    0.0,
                    0.0,
                    sos_sms_ok,
                    call_ok
                );
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
            device_state_set_sos_active(false);

            a9g_preempt_background(false);

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

// The button must stay LOW (active) continuously for SOS_LONG_PRESS_MS.
// Any HIGH sample restarts the timer, so short taps, contact bounce and
// noise spikes never trigger an emergency. One trigger per press.

static void handle_sos_button(void)
{
    static int64_t pressed_since_ms = 0;
    static bool press_handled = false;
    static int64_t last_trigger_ms = 0;

    int64_t current_ms =
        now_ms();

    // Active LOW
    if (gpio_get_level(
            SOS_BUTTON_PIN) != 0)
    {
        pressed_since_ms = 0;
        press_handled = false;
        return;
    }

    if (pressed_since_ms == 0)
    {
        pressed_since_ms =
            current_ms;
        return;
    }

    // Minimum of 80 ms even if the long press is configured shorter
    int64_t hold_required =
        (SOS_LONG_PRESS_MS > 80) ? SOS_LONG_PRESS_MS : 80;

    if (press_handled ||
        (current_ms - pressed_since_ms) < hold_required)
    {
        return;
    }

    press_handled =
        true;

    if (sos_state != SOS_STATE_NORMAL)
    {
        ESP_LOGW(
            TAG,
            "SOS already running. Button ignored."
        );

        return;
    }

    if (last_trigger_ms != 0 &&
        (current_ms - last_trigger_ms) <= SOS_DEBOUNCE_MS)
    {
        return;
    }

    last_trigger_ms =
        current_ms;

    start_sos_sequence();
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
// STATUS LED (only outside an SOS sequence)
// =========================================================

static void update_status_led(void)
{
    int64_t t = now_ms();

    bool on = cloud_sync_is_healthy()
                  ? (t % 3000) < 80
                  : (t % 1000) < 500;

    gpio_set_level(
        STATUS_LED_PIN,
        on ? 1 : 0
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

        if (sos_state == SOS_STATE_NORMAL)
        {
            update_status_led();
        }

        // Fast loop for instantaneous button/SOS timing (<50ms)
        vTaskDelay(
            pdMS_TO_TICKS(50)
        );
    }
}

// =========================================================
// MODEM TASK
// Supervises the A9G and polls each subsystem on its own
// schedule. A failure in one (no GPS fix, no SIM) never
// stops the others; a silent modem is re-initialized.
// =========================================================

static void modem_bring_up(void)
{
    a9g_ready =
        true;

    device_state_set_modem(true);

    bool gps_ok =
        a9g_enable_gps();

    device_state_set_gps_enabled(gps_ok);

    ESP_LOGI(
        TAG,
        "A9G ready. GPS %s.",
        gps_ok ? "enabled" : "enable failed (will retry)"
    );
}

static void poll_gps(void)
{
    static a9g_gps_result_t last_result = A9G_GPS_FIX;
    static int64_t last_searching_log_ms = 0;

    device_state_t state;
    device_state_snapshot(&state);

    if (!state.gps_enabled)
    {
        device_state_set_gps_enabled(a9g_enable_gps());
    }

    // Prefer the NMEA stream (receiver speed, course, satellites, HDOP);
    // fall back to AT+LOCATION if this A9G firmware does not provide it.
    static bool nmea_available = true;
    static int nmea_misses = 0;

    a9g_gps_result_t result = A9G_GPS_NO_RESPONSE;

    if (nmea_available)
    {
        a9g_nmea_t nmea;

        if (a9g_read_nmea(&nmea, GPS_NMEA_LISTEN_MS))
        {
            if (nmea_misses >= 0)
            {
                ESP_LOGI(TAG, "GPS: using NMEA stream (speed, satellites, accuracy).");
                nmea_misses = -1; // logged once
            }

            if (nmea.fix)
            {
                gps_fix_extra_t extra = {
                    .speed_kmh = nmea.speed_kmh,
                    .course_deg = nmea.course_deg,
                    .satellites = nmea.satellites,
                    .hdop = nmea.hdop
                };

                device_state_record_fix(nmea.lat, nmea.lon, &extra);
                result = A9G_GPS_FIX;
            }
            else
            {
                device_state_set_gps_quality(nmea.satellites, nmea.hdop);
                result = A9G_GPS_NO_FIX;
            }
        }
        else if (sos_state != SOS_STATE_NORMAL)
        {
            return; // read was cut short for SOS; not an NMEA failure
        }
        else if (nmea_misses >= 0 && ++nmea_misses >= GPS_NMEA_MAX_MISSES)
        {
            nmea_available = false;

            ESP_LOGW(TAG, "GPS: no NMEA from A9G; using AT+LOCATION instead.");
        }
    }

    if (!nmea_available || result == A9G_GPS_NO_RESPONSE)
    {
        double lat = 0.0;
        double lon = 0.0;

        result =
            a9g_read_gps(
                &lat,
                &lon,
                AT_DEFAULT_TIMEOUT_MS
            );

        if (result == A9G_GPS_FIX)
        {
            device_state_record_fix(lat, lon, NULL);
        }
    }

    if (result == A9G_GPS_NO_FIX)
    {
        int64_t current_ms = now_ms();

        if (last_result != A9G_GPS_NO_FIX ||
            (current_ms - last_searching_log_ms) >= 60000)
        {
            last_searching_log_ms = current_ms;

            ESP_LOGW(
                TAG,
                "GPS searching for satellites (no fix yet). Antenna needs open sky."
            );
        }
    }

    last_result = result;
}

static void poll_cellular(void)
{
    static bool last_sim = false;
    static bool last_registered = false;
    static bool first_poll = true;
    static bool sms_configured = false;

    bool sim =
        a9g_check_sim();

    bool registered =
        sim && a9g_check_registration();

    int csq = -1;

    if (!a9g_get_csq(&csq))
    {
        csq = -1;
    }

    if (sim && !sms_configured)
    {
        sms_configured =
            a9g_setup_sms();
    }
    else if (!sim)
    {
        sms_configured =
            false;
    }

    if (first_poll || sim != last_sim || registered != last_registered)
    {
        ESP_LOGI(
            TAG,
            "Cellular: SIM %s (%s), network %s, CSQ %d",
            sim ? "ready" : "NOT ready - SMS/call unavailable",
            a9g_sim_status(),
            registered ? "registered" : "not registered",
            csq
        );
    }

    first_poll = false;
    last_sim = sim;
    last_registered = registered;

    device_state_set_cellular(sim, a9g_sim_status(), registered, csq);
}

static void poll_battery(void)
{
    int pct = -1;
    int mv = -1;

    if (!a9g_get_battery(&pct, &mv))
    {
        pct = -1;
        mv = -1;
    }

    device_state_set_battery(pct, mv);
}

static void modem_task(
    void *arg)
{
    (void)arg;

    int64_t last_gps_ms = 0;
    int64_t last_cellular_ms = 0;
    int64_t last_battery_ms = 0;
    bool announced = false;

    a9g_set_background_task();

    esp_task_wdt_add(NULL);

    while (1)
    {
        esp_task_wdt_reset();

        // -----------------------------------------
        // Bring up / recover the modem
        // -----------------------------------------

        if (!a9g_ready)
        {
            if (!a9g_init())
            {
                device_state_set_modem(false);

                ESP_LOGE(
                    TAG,
                    "A9G not responding. GPS/SMS/CALL unavailable; retrying in %d s.",
                    MODEM_REINIT_INTERVAL_MS / 1000
                );

                esp_task_wdt_reset();

                vTaskDelay(
                    pdMS_TO_TICKS(MODEM_REINIT_INTERVAL_MS)
                );

                continue;
            }

            modem_bring_up();

            last_gps_ms = 0;
            last_cellular_ms = 0;
            last_battery_ms = 0;

            if (!announced)
            {
                announced = true;

                ESP_LOGI(
                    TAG,
                    "System ready. Press SOS to start emergency sequence."
                );

                beep(
                    1,
                    100
                );
            }
        }

        // -----------------------------------------
        // Leave the UART to the SOS sequence
        // -----------------------------------------

        if (sos_state != SOS_STATE_NORMAL)
        {
            vTaskDelay(
                pdMS_TO_TICKS(200)
            );

            continue;
        }

        int64_t current_ms =
            now_ms();

        // One subsystem per pass keeps each UART hold short.
        if (last_gps_ms == 0 ||
            (current_ms - last_gps_ms) >= GPS_PERIODIC_UPDATE_MS)
        {
            last_gps_ms = current_ms;
            poll_gps();
        }
        else if (last_cellular_ms == 0 ||
                 (current_ms - last_cellular_ms) >= MODEM_STATUS_POLL_MS)
        {
            last_cellular_ms = current_ms;
            poll_cellular();
        }
        else if (last_battery_ms == 0 ||
                 (current_ms - last_battery_ms) >= BATTERY_POLL_MS)
        {
            last_battery_ms = current_ms;
            poll_battery();
        }

        // -----------------------------------------
        // Detect a modem that stopped answering
        // -----------------------------------------

        if (a9g_silent_command_count() >= MODEM_MAX_SILENT_COMMANDS)
        {
            ESP_LOGE(
                TAG,
                "A9G stopped responding. Re-initializing."
            );

            a9g_ready =
                false;

            device_state_set_modem(false);

            continue;
        }

        vTaskDelay(
            pdMS_TO_TICKS(250)
        );
    }
}

// =========================================================
// NVS + BOOT DIAGNOSTICS
// =========================================================

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

static void init_nvs_and_boot_info(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    uint32_t boot_count = 0;
    nvs_handle_t nvs;

    if (nvs_open("beacon", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_get_u32(nvs, "boot_count", &boot_count);
        boot_count++;
        nvs_set_u32(nvs, "boot_count", boot_count);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    const char *reason =
        reset_reason_name(esp_reset_reason());

    device_state_set_boot_info(boot_count, reason);

    ESP_LOGI(
        TAG,
        "Boot #%lu, reset reason: %s",
        (unsigned long)boot_count,
        reason
    );
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
        "Guardian Beacon %s Booting... (device %s)",
        FIRMWARE_VERSION,
        DEVICE_ID
    );

    ESP_LOGI(
        TAG,
        "======================================"
    );

    gpio_setup();

    device_state_init();

    init_nvs_and_boot_info();

    // Cloud and modem tasks subscribe to the task watchdog; a hang longer
    // than TASK_WDT_TIMEOUT_MS restarts the device.
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = TASK_WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };

    ESP_ERROR_CHECK(
        esp_task_wdt_reconfigure(&wdt_config)
    );

    // ---------------------------------------------
    // 1. High-Priority Safety Task (Priority 10)
    // Started first so SOS works even while the
    // modem or Wi-Fi are still coming up.
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
    // 2. Wi-Fi + Cloud Task (Priority 4)
    // ---------------------------------------------
    cloud_sync_start();

    // ---------------------------------------------
    // 3. Modem / Sensor Task (Priority 3)
    // ---------------------------------------------
    xTaskCreate(
        modem_task,
        "modem_task",
        4096,
        NULL,
        3,
        NULL
    );
}
