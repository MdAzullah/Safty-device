#pragma once

#include <driver/gpio.h>
#include <driver/uart.h>

// Optional local overrides. Keep pins_config.local.h out of version control.
#if defined(__has_include)
#if __has_include("pins_config.local.h")
#include "pins_config.local.h"
#endif
#endif

// =========================================================
// A9G UART
// =========================================================

#define A9G_UART_PORT UART_NUM_2

#define A9G_TX_PIN GPIO_NUM_17
#define A9G_RX_PIN GPIO_NUM_16

// CONFIRMED WITH YOUR HARDWARE TEST
#define A9G_BAUD_RATE 9600

#define A9G_UART_BUF_SIZE 1024

// 1 = log every AT command/response at INFO level (debugging)
#ifndef A9G_VERBOSE_AT
#define A9G_VERBOSE_AT 0
#endif

// =========================================================
// GPIO
// =========================================================

#define SOS_BUTTON_PIN GPIO_NUM_4
#define STATUS_LED_PIN GPIO_NUM_2
#define BUZZER_PIN GPIO_NUM_5

// =========================================================
// EMERGENCY CONTACT
// =========================================================

#ifndef PARENT_NUMBER
#define PARENT_NUMBER "0000000000" // Dummy value; set locally in pins_config.local.h
#endif

#ifndef PARENT_NUMBER_2
#define PARENT_NUMBER_2 ""
#endif

// =========================================================
// CLOUD BACKEND & CELLULAR GPRS CONFIGURATION
// =========================================================

// Unique Device ID corresponding to the website
#define DEVICE_ID "beacon-01"
#define DEVICE_NAME "Guardian Beacon"

// Firebase Realtime Database Base URL
#define CLOUD_API_BASE_URL                                                     \
  "https://"                                                                   \
  "safety-device-77b03-default-rtdb.asia-southeast1.firebasedatabase.app"

// =========================================================
// WI-FI CONFIGURATION (all cloud uploads use ESP32 Wi-Fi)
// =========================================================

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// 1 = Wi-Fi modem sleep (saves power, adds latency). 0 = lowest latency,
// recommended while the device runs from USB/mains power.
#ifndef WIFI_POWER_SAVE
#define WIFI_POWER_SAVE 0
#endif

// NTP server used for real timestamps (history keys)
#define SNTP_SERVER "pool.ntp.org"

// =========================================================
// FIRMWARE IDENTITY
// =========================================================

#define FIRMWARE_VERSION "1.2.0"

// =========================================================
// TIMING
// =========================================================

// Device heartbeat to /live. Sent whenever Wi-Fi is up, independent of
// GPS fix, SIM, or modem state. The website marks the device Stale after
// ~2 missed heartbeats and Offline after ~3 (see website config.js).
#define CLOUD_HEARTBEAT_MS 5000

// Minimum spacing between /history entries (only written for new GPS fixes)
#define CLOUD_HISTORY_MIN_INTERVAL_MS 15000

// Retry interval for an SOS alert that could not be delivered
#define CLOUD_SOS_RETRY_MS 3000

// HTTPS request timeout
#define CLOUD_HTTP_TIMEOUT_MS 8000

// Restart the device when Wi-Fi is up but nothing has uploaded for this long
#define CLOUD_STALL_RESTART_MS 600000

// Wi-Fi reconnect back-off after a disconnect (doubles up to the maximum)
#define WIFI_RECONNECT_MIN_BACKOFF_MS 1000
#define WIFI_RECONNECT_MAX_BACKOFF_MS 30000

// Associated to the access point but no IP address after this long -> retry
#define WIFI_DHCP_TIMEOUT_MS 15000

// Task watchdog for the cloud and modem tasks (device restarts if one hangs)
#define TASK_WDT_TIMEOUT_MS 60000

// SOS button: must be held LOW continuously this long to trigger.
// Short taps and electrical noise are ignored.
#ifndef SOS_LONG_PRESS_MS
#define SOS_LONG_PRESS_MS 1000
#endif

// Cool-down after an SOS sequence before the button is accepted again
#define SOS_DEBOUNCE_MS 4000

// Normal live GPS update
#define GPS_PERIODIC_UPDATE_MS 5000

// How long to listen to the A9G NMEA stream per GPS update
#define GPS_NMEA_LISTEN_MS 2200

// Fall back to AT+LOCATION after this many NMEA reads with no sentences
#define GPS_NMEA_MAX_MISSES 3

// A fix older than this is reported as "searching" (last known location kept)
#define GPS_FIX_STALE_MS 30000

// Movement below this between fixes is treated as GPS jitter (speed 0)
#define GPS_JITTER_DISTANCE_M 8.0

// Fixes with worse HDOP are shown live but not added to the history trail
#define GPS_MAX_HDOP_FOR_TRAIL 5.0

// A fix implying a faster jump from the previous fresh fix is rejected
#define GPS_MAX_PLAUSIBLE_KMH 250.0

// Cellular status (SIM, registration, signal) refresh
#define MODEM_STATUS_POLL_MS 30000

// Battery/supply voltage refresh
#define BATTERY_POLL_MS 60000

// Re-initialize the A9G after this many consecutive unanswered commands
#define MODEM_MAX_SILENT_COMMANDS 3

// Delay between A9G re-initialization attempts while it is not responding
#define MODEM_REINIT_INTERVAL_MS 10000

// Maximum time allowed to find fresh GPS after SOS
#define SOS_GPS_FIRST_WINDOW_MS 10000

// Time between SOS GPS attempts
#define GPS_POLL_INTERVAL_MS 2000

// Normal GPS AT command timeout
#define AT_DEFAULT_TIMEOUT_MS 3000

// SMS maximum timeout
#define SMS_SEND_TIMEOUT_MS 20000

// Allow A9G to boot
#define BOOT_SETTLE_DELAY_MS 3000
