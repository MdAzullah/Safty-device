# Project context

## Purpose

Guardian Beacon is a wearable/portable child-safety tracker built as a mini project. A guardian sees the child's live location, path, and device health on a web dashboard, and receives an SOS alert (website banner, SMS, and phone call) when the child presses a button on the device.

**Status: working prototype.** It is not certified emergency equipment and must not be the only safety measure.

## Users

- **Guardian** — signs in to the dashboard (Firebase Email/Password account), watches location and health, acknowledges SOS alerts, receives SMS/calls.
- **Child** — carries the device; only interaction is the SOS button.
- **Developer** — configures firmware and Firebase, flashes the board.

## Hardware

| Part | Role |
|---|---|
| ESP32 DevKit (DOIT DevKit V1, ESP32 rev 3.1, 4 MB flash) | Main controller, Wi-Fi uplink, HTTPS client |
| AI-Thinker A9G board | GPS, 2G GSM (SMS, voice call); AT commands over UART2 at 9600 baud |
| Push button (GPIO 4 → GND), LED (GPIO 2), buzzer (GPIO 5) | SOS input and local feedback |
| SIM card | Must be on an operator with a **2G** network (Airtel works; Jio does not) |

Full wiring and power notes: `USER_GUIDE.md` §2.

## Technology

| Area | Technology | Version / notes |
|---|---|---|
| Firmware | C, ESP-IDF, FreeRTOS | ESP-IDF **v6.0.1** (installed at `C:\esp\v6.0.1\esp-idf`); `platformio.ini` exists but builds are done with `idf.py` |
| Cloud | Firebase Realtime Database (REST from device, Web SDK from browser), Firebase Authentication (Email/Password) | Project ID `safety-device-77b03`, region asia-southeast1 |
| Website | HTML, CSS, vanilla JS ES modules, no build step | Leaflet 1.9.4, Firebase JS SDK 10.12.2 (compat), IBM Plex fonts — all from CDNs |
| Map tiles | OpenStreetMap standard tiles | Subject to the OSM tile usage policy |
| Tooling | Node.js (≥ 18) for `tools/check-device.mjs` | Verified with Node 24 |

## Key design decisions

- **Cloud uploads use ESP32 Wi-Fi only.** The A9G's own HTTP stack is not used (no verified PUT, and holding its UART would delay SOS). Cellular data upload is future work.
- **Heartbeat-based presence.** The device writes `/live` every 5 s; the website derives Online / Delayed / Offline from the server timestamp age. Firebase `onDisconnect` is not available to a REST client.
- **Independent subsystems.** Modem, GPS, SIM, battery, and Wi-Fi are polled and reported separately so one failure never hides the others.
- **SOS never waits on the network.** The alert is queued to the cloud task and retried until delivered; the SMS/call sequence runs in parallel.
- **Server timestamps everywhere** to avoid device/browser clock problems.

## Constraints

- Device writes to Firebase are currently **unauthenticated** (development rules). See `docs/ARCHITECTURE.md` → Security.
- Private values (Wi-Fi, phone numbers) live only in git-ignored local files.
- Firebase free (Spark) plan assumed; no Cloud Functions.

## Unknowns

- Whether the A9G on this board provides an NMEA stream (`AT+GPSRD`) or the firmware has fallen back to `AT+LOCATION` (check the boot log for `GPS: using NMEA stream` or `using AT+LOCATION instead`).
- Battery chemistry and wiring: the battery percentage is whatever the A9G's `AT+CBC?` reports; how that maps to the real supply is unverified.
- Production hosting target for the website (Firebase Hosting is suggested, not set up).
- Whether Firebase Authentication allows self sign-up for this project (affects security; see `docs/TASKS.md`).
