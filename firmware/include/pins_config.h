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

// Cellular GPRS APN (Common defaults: "airtelgprs.com", "jionet", "www",
// "internet", "vi")
#define GPRS_APN "internet"

// Firebase Realtime Database Base URL
#define CLOUD_API_BASE_URL                                                     \
  "https://"                                                                   \
  "safety-device-77b03-default-rtdb.asia-southeast1.firebasedatabase.app"

// =========================================================
// WI-FI FALLBACK CONFIGURATION (Used if A9G GPRS is Offline)
// =========================================================

#define WIFI_FALLBACK_ENABLED 1
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// =========================================================
// TIMING
// =========================================================

// Normal live GPS & Cloud Telemetry update interval
#define TELEMETRY_PERIODIC_UPDATE_MS 15000

// Button debounce
#define SOS_DEBOUNCE_MS 4000

// Normal live GPS update
#define GPS_PERIODIC_UPDATE_MS 15000

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
