# Guardian Beacon — ESP32/A9G Safety Tracker

Guardian Beacon combines ESP32 firmware and a web dashboard for child-device tracking, location history, and SOS notifications. The website also supports showing a guardian's own location on the map after explicit browser permission.

## Project layout

- `firmware/`: ESP-IDF project for ESP32 and AI-Thinker A9G, including GPS, SMS/call SOS flow, and cloud telemetry.
- `website/`: Leaflet-based dashboard, Firebase Realtime Database adapter, demo mode, and parent-location feature.

## User guide

For the full hardware wiring, ESP32 configuration, Firebase setup and rules, website launch steps, parent location behavior, end-to-end tests, and troubleshooting, see [USER_GUIDE.md](USER_GUIDE.md).

## GitHub repository hygiene

Generated firmware builds, local credentials, dependency folders, and temporary files are excluded by `.gitignore`. Keep `firmware/sdkconfig` files tracked because they capture ESP-IDF build configuration. Before committing, check `git status` and ensure no passwords, private contact data, database secrets, or generated build output is staged.

## Data connection

The firmware and website are configured for Firebase Realtime Database project `safety-device-77b03`, device ID `beacon-01`, and these paths:

- `devices/beacon-01/live` — current telemetry (PUT/replacement).
- `devices/beacon-01/history` — appended telemetry records (POST).
- `devices/beacon-01/sos_alert` — active/acknowledged SOS state (PUT/replacement).
- `guardians/<Firebase Auth UID>/location` — optional guardian location, only when a signed-in user clicks **My Location** and grants permission.

Firmware writes Firebase server timestamps, resolved to epoch milliseconds, which matches the website's time and offline checks. See [website/README.md](website/README.md) for Firebase setup, rules guidance, location consent/privacy, and important modem transport limitations.

## Before relying on this for safety

- Configure the Firebase Web App values in `website/js/config.js` and enable Firebase Email/Password Authentication.
- Configure phone and Wi-Fi values in the ignored local override `firmware/include/pins_config.local.h`; current Firebase cloud uploads require ESP32 Wi-Fi.
- Firebase device writes are unauthenticated in the current firmware; the example development rule is not production secure. Use a trusted backend or proper device authentication before deployment.
- Test actual GPS, Wi-Fi cloud access, SMS/call, database rules, timestamps and dashboard display on the physical board and network. A9G cellular cloud writes are intentionally disabled until its HTTP methods are verified and can run without delaying emergency handling.

The dashboard is a monitoring prototype, not a substitute for tested emergency-response equipment.
