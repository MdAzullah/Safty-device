#pragma once

#include <stdbool.h>

// =========================================================
// Cloud sync — ESP32 Wi-Fi + Firebase Realtime Database (HTTPS REST)
//
// Runs in its own task. Other tasks never perform network I/O; they update
// device_state or post SOS events here, which never block. The heartbeat to
// /live is sent on a fixed interval whatever the GPS/SIM/modem state is.
// =========================================================

typedef enum
{
    SOS_LOCATION_NONE = 0,
    SOS_LOCATION_LAST_KNOWN,
    SOS_LOCATION_CURRENT
} sos_location_source_t;

// Initialize Wi-Fi and start the cloud task. Call once from app_main.
void cloud_sync_start(void);

// SOS pressed: publish an active alert as soon as possible (last known
// location if any). Non-blocking; retried until delivered.
void cloud_sync_sos_triggered(void);

// SOS sequence finished: attach the alert location and SMS/call results.
// Does not re-activate an alert the guardian already acknowledged.
void cloud_sync_sos_update(sos_location_source_t source,
                           double lat,
                           double lon,
                           bool sms_sent,
                           bool call_started);

// True when the last successful upload is recent.
bool cloud_sync_is_healthy(void);
