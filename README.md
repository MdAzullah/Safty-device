# Guardian Beacon — ESP32/A9G Safety Tracker

A child-safety tracker. An **ESP32** with an **AI-Thinker A9G** (GPS + 2G GSM) reports location and device health to **Firebase Realtime Database** over Wi-Fi. A **web dashboard** shows live position, recent path, device health, and SOS alerts. Holding the SOS button sends a website alert, an SMS with the location, and a phone call to the guardian.

> **Prototype.** Not certified emergency equipment. GPS, mobile coverage, Wi-Fi, Firebase, and power can all fail. Read the security notes before real use.

## Features

- Heartbeat every 5 s with Wi-Fi, modem, SIM/cellular, battery, and GPS reported independently (one failing subsystem does not hide the others).
- GPS position, speed, satellites, and accuracy; recent path (last 50 points) on the map.
- SOS: 1 s long press → immediate website alert → SMS with location → voice call; result reported back to the dashboard.
- Dashboard: Online / Delayed / Offline status from server time, sign-in with Firebase Email/Password, light/dark themes, phone layout, diagnostics, guardian's own location, share/copy location, demo mode.
- Self-healing firmware: modem recovery, Wi-Fi back-off, upload retry, task watchdog.

What is verified on hardware and what is not: [`docs/TASKS.md`](docs/TASKS.md).

## Repository layout

```
firmware/            ESP-IDF project (ESP32 + A9G)
  src/               main.c, cloud_sync.c, device_state.c, a9g_module.c
  include/           pins_config.h (public config), pins_config.local.h (private, git-ignored)
  certs/             extra TLS root certificate for Firebase
website/             static dashboard (HTML/CSS/ES modules, no build step)
tools/               check-device.mjs (device status from Firebase)
docs/                PROJECT_CONTEXT, ARCHITECTURE, TASKS, CHANGELOG
USER_GUIDE.md        wiring, Firebase setup and rules, end-to-end test checklist
AGENTS.md, CLAUDE.md instructions for AI coding agents
```

## Requirements

| For | Needs |
|---|---|
| Firmware | ESP-IDF **v6.0.1** (EIM installer on Windows), USB driver for CP210x, ESP32 DevKit (4 MB flash), A9G board with GPS + GSM antennas, push button, LED, buzzer |
| SIM | Operator with a **2G** network (Airtel works; Jio does not), SIM PIN disabled |
| Cloud | Firebase project with Realtime Database and Email/Password Authentication |
| Website | Any modern browser; a local HTTP server (VS Code Live Server, `npx serve`, or Python) |
| Tools | Node.js 18+ for `tools/check-device.mjs` |

## Setup

### 1. Firebase
Create a Realtime Database, register a Web App, enable **Email/Password** sign-in, and create a guardian user. Publish database rules (development example in [`USER_GUIDE.md`](USER_GUIDE.md) §4). Data paths and formats: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#firebase-data-model).

### 2. Firmware
1. Wire the hardware ([`USER_GUIDE.md`](USER_GUIDE.md) §2): A9G TX/RX ↔ GPIO 16/17, SOS button GPIO 4 → GND, LED GPIO 2, buzzer GPIO 5, common GND.
2. Create `firmware/include/pins_config.local.h` (git-ignored):
   ```c
   #pragma once
   #define PARENT_NUMBER   "+91XXXXXXXXXX"   // country code + full number
   #define PARENT_NUMBER_2 ""                // optional second guardian
   #define WIFI_SSID       "your-2.4GHz-ssid"
   #define WIFI_PASS       "your-password"
   ```
3. Check `DEVICE_ID` and `CLOUD_API_BASE_URL` in `firmware/include/pins_config.h` match the website config and your database.
4. Build and flash (PowerShell):
   ```powershell
   . "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
   cd firmware
   idf.py build
   idf.py -p COM4 flash monitor
   ```
   Expected log: `Wi-Fi connected`, `Certificate validated`, then `PUT live -> 204` every 5 s.

### 3. Website
1. In `website/js/config.js`, set the Firebase web config (`backend.firebase`) and `device.id`. Optional private values (device phone number for "Call device") go in git-ignored `website/js/config.local.js`:
   ```js
   export default { device: { phone: "+91XXXXXXXXXX" } };
   ```
   Set `LOAD_LOCAL_CONFIG: false` in `config.js` if you do not create that file.
2. Serve the `website/` folder over HTTP and open it, e.g. `npx --yes serve website`. Opening `index.html` directly from disk does not work.
3. Sign in with the guardian account. Append `?demo` to the URL to preview with simulated data.

## Configuration reference

| Setting | File | Default |
|---|---|---|
| Device ID | `pins_config.h` `DEVICE_ID`, `config.js` `device.id` | `beacon-01` |
| Heartbeat interval | `pins_config.h` `CLOUD_HEARTBEAT_MS` | 5000 ms |
| SOS hold time | `SOS_LONG_PRESS_MS` (override in `pins_config.local.h`) | 1000 ms |
| Wi-Fi power save | `WIFI_POWER_SAVE` | 0 (off) |
| Raw AT logging | `A9G_VERBOSE_AT` | 0 |
| Delayed / Offline thresholds | `config.js` `staleAfterMs` / `offlineTimeoutMs` | 9 s / 15 s (scaled up for slower heartbeats) |
| Theme | `config.js` `map.defaultTheme` | `system` |

## Checking the device

```bash
node tools/check-device.mjs your-guardian@example.com --watch
```
Signs in like the website and prints Online/Offline, firmware, Wi-Fi, modem, GPS, location, battery, cellular, history count, and SOS state. You can also watch **Realtime Database → Data → devices/beacon-01/live** in the Firebase console.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Website sign-in fails | Account missing in Authentication → Users, wrong password, or an invalid `apiKey` |
| "Permission denied reading devices/…" | Database rules do not allow `auth != null` reads |
| Device never appears online | Serial log: Wi-Fi reason 15/204 = wrong password, 201 = SSID not found (2.4 GHz only); `-> 401` = rules block device writes |
| `No matching trusted root certificate found` | Keep `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE` and `firmware/certs/firebase_extra_roots.pem` |
| Cellular shows "No SIM" / "SIM locked" / "SIM error" | Re-seat SIM and **power-cycle the A9G**; disable SIM PIN; Diagnostics → "SIM (modem reply)" shows the modem's answer |
| SIM ready but "Searching network" | No 2G coverage or a 4G-only SIM (Jio) |
| SOS does nothing | Hold the button ≥ 1 s; button must connect GPIO 4 to GND (use diagonal legs on 4-leg switches); log should show `SOS BUTTON PRESSED` |
| GPS "Searching" | Needs open sky; first fix can take minutes |
| `could not open port 'COM4'` | Close the serial monitor using the port |

More: [`USER_GUIDE.md`](USER_GUIDE.md) §7.

## Security

Current development rules let the device write **without authentication** and let **any signed-in user** read device data. Before real use: disable Firebase sign-up, restrict reads to listed guardians, add device authentication and rule validation (plan in [`docs/TASKS.md`](docs/TASKS.md)). Never commit `pins_config.local.h`, `config.local.js`, or any Firebase Admin/service-account credential. Map tiles come from OpenStreetMap; follow its tile usage policy for public deployments.

## Documentation

- [`docs/PROJECT_CONTEXT.md`](docs/PROJECT_CONTEXT.md) — purpose, hardware, technology, decisions, unknowns
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — firmware tasks, SOS flow, Firebase schema, website modules, security model
- [`docs/TASKS.md`](docs/TASKS.md) — verified/unverified work, known issues, backlog
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md) — changes by version
- [`USER_GUIDE.md`](USER_GUIDE.md) — hardware, Firebase, end-to-end tests
- [`website/README.md`](website/README.md) — dashboard-specific notes
