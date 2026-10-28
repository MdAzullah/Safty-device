# Guardian Beacon User Guide

This guide explains how to assemble and configure the ESP32/A9G tracker, connect it to Firebase, run the website, display the child and guardian locations, and verify the end-to-end flow.

> **Safety notice:** This project is a prototype. GPS, mobile coverage, Firebase access, browser permissions, and power can all fail. Do not rely on it as the only emergency or child-safety measure until the complete system has been professionally reviewed and tested under realistic conditions.

## 1. How the system works

1. The ESP32 reads GPS coordinates from the A9G module over UART.
2. The ESP32 sends current location and metrics to Firebase over its configured Wi-Fi connection using HTTPS.
3. The website reads the Firebase device data and shows the child's location, recent history, and SOS alerts.
4. A guardian can choose **My Location** in the website. The browser asks for location permission and shows a parent marker. If signed into Firebase, the browser also saves this location to the guardian's own UID path.
5. When the hardware SOS button is pressed, firmware attempts fresh GPS, sends an SMS/call through A9G, and publishes the SOS state when Wi-Fi is available.

The A9G cellular data HTTP path is not used for Firebase writes in the current firmware. Cellular-only cloud reporting is not available until that transport is implemented and validated.

## 2. Hardware and wiring

### Required equipment

- ESP32 DevKit board supported by this project's build configuration.
- AI-Thinker A9G module/board with GPS and GSM antenna(s), SIM card with service, and an appropriate stable power supply.
- SOS push button, status LED and series resistor, buzzer (and transistor/driver if required by its current draw), jumper wires, and USB cable.
- A 2.4 GHz Wi-Fi network with internet access for firmware cloud uploads.

### UART and GPIO mapping

Firmware configuration is in `firmware/include/pins_config.h`:

| Signal | ESP32 GPIO | Connection |
|---|---:|---|
| ESP32 UART2 TX | 17 | Connect to A9G UART RX |
| ESP32 UART2 RX | 16 | Connect to A9G UART TX |
| SOS button | 4 | Button between GPIO 4 and GND; input uses internal pull-up |
| Status LED | 2 | LED through a suitable series resistor, or compatible board LED |
| Buzzer | 5 | Use a driver if buzzer current exceeds safe GPIO output |
| Ground | GND | ESP32 and A9G grounds must be common |

**Power and logic-level precautions:** verify the exact A9G carrier-board supply and UART logic-level requirements from its documentation before connecting it. The radio has high current bursts; do not power it from the ESP32 3.3 V pin unless the module/carrier documentation explicitly permits it. Use a properly rated supply and compatible level shifting where required. Disconnect power before changing wiring. Incorrect voltage or reversed UART wiring can damage hardware.

### SIM/GPS checks

- Insert an activated SIM with the required SMS/call service; disable SIM PIN or configure firmware to handle it.
- Attach the GSM antenna and GPS antenna as required by the exact A9G board.
- Test outdoors with a clear view of the sky for GPS. Initial fixes may take several minutes.
- Confirm the configured emergency number is correct and authorized before testing calls/SMS. Firmware contact values belong only in the ignored `pins_config.local.h`; do not commit private phone numbers to a public repository.

## 3. Configure firmware

1. Open the `firmware/` directory as the ESP-IDF project in VS Code, not the repository root. The ESP-IDF project `CMakeLists.txt` is inside `firmware/`.
2. Select the matching board target (the provided PlatformIO environment is `esp32doit-devkit-v1`; verify this matches your physical ESP32).
3. In `firmware/include/pins_config.h`, check the public hardware pins, `DEVICE_ID`, and `CLOUD_API_BASE_URL`. Add private contact/Wi-Fi overrides to ignored `firmware/include/pins_config.local.h`. The optional ignored `website/js/config.local.js` can hold your private website click-to-call number.
4. Set `WIFI_SSID` and `WIFI_PASS` in `firmware/include/pins_config.local.h` to the 2.4 GHz Wi-Fi credentials. The tracked defaults are intentionally blank. Do not share or commit real credentials.
  The local override should define `PARENT_NUMBER`, `PARENT_NUMBER_2` (or leave it empty), `WIFI_SSID`, and `WIFI_PASS`. The checked-in `PARENT_NUMBER` is a dummy value and cannot receive emergency calls or SMS.
5. Keep the firmware device ID and database URL aligned with `website/js/config.js` and Firebase. Current configured device ID: `beacon-01`.
6. Build and flash through the ESP-IDF VS Code extension or a properly configured ESP-IDF environment. Connect the board via USB and select the correct serial port. Open the serial monitor at the configured monitor baud rate (typically 115200; check your ESP-IDF settings).
7. On startup, check logs for A9G UART, GPS enable, and Wi-Fi connection. SIM readiness and GSM registration are needed for SMS/calls, but do not block GPS or Wi-Fi cloud uploads. A GPS antenna fix and cloud write are separate: the device can have modem ready but not yet have a GPS fix, or have GPS but no Wi-Fi internet.

The firmware (1.2+) runs three independent tasks: a safety task (SOS button and sequence), a cloud task (Wi-Fi and Firebase), and a modem task (A9G, GPS, SIM/signal, battery). A heartbeat is written to `/live` every 5 seconds whenever Wi-Fi is up, even with no GPS fix, no SIM card, or an A9G that is not responding. Each value is reported separately, so one failing subsystem never stops the others from reaching the dashboard. Cloud uploads require Wi-Fi; the A9G provides GPS/SMS/call only.

Reliability features in 1.2:

- **SOS needs a 1-second press.** The button must be held continuously for `SOS_LONG_PRESS_MS` (default 1000 ms) before SOS starts; taps and electrical noise are ignored. Two beeps confirm the press was accepted. Override `SOS_LONG_PRESS_MS` in `pins_config.local.h` if needed.
- **SOS gets the modem immediately**: any background GPS/SIM query in progress is cut short.
- **GPS quality**: speed, heading, satellites and HDOP come from the A9G's NMEA stream (`AT+GPSRD`). If your A9G firmware does not provide NMEA, the log says `using AT+LOCATION instead` and speed is estimated from successive fixes. Fixes with poor accuracy (HDOP > 5) or impossible jumps are kept out of the history trail.
- **Uploads**: a failed request is retried once on a fresh connection; a new GPS point and the heartbeat are sent in one request once the clock is synchronized over NTP.
- **Wi-Fi**: reconnects with back-off (1 s up to 30 s), never interrupts a connection in progress, and recovers when DHCP stalls. Power saving is off by default for low latency (`WIFI_POWER_SAVE`).
- **Self-healing**: the cloud and modem tasks are watched by the task watchdog (60 s). The device also restarts if Wi-Fi is up but nothing has uploaded for 10 minutes. A9G that stops answering is re-initialized.
- **Diagnostics** in `/live/diag`: last reset reason, boot count, free memory, upload counters, Wi-Fi reconnects and clock sync. The website shows them under **Diagnostics**.

Expected serial log lines when everything works:

```text
I (…) CLOUD: Wi-Fi connected. IP: 192.168.x.x
I (…) CLOUD: PUT live -> 204 (150 ms)          every 5 s
I (…) SOS: Cellular: SIM NOT detected (SMS/call unavailable), network not registered, CSQ …
W (…) SOS: GPS searching for satellites (no fix yet). Antenna needs open sky.
I (…) STATE: GPS fix #1: 28.6162123, 77.2079612
I (…) CLOUD: POST history -> 204 (300 ms)
```

`PUT live -> 401` means the Realtime Database rules deny device writes. `Wi-Fi connect failed (reason 15/204)` usually means a wrong password, and `reason 201` means the SSID was not found (the ESP32 supports only 2.4 GHz). Set `A9G_VERBOSE_AT 1` in `pins_config.local.h` to log every AT command.

Status LED outside an SOS: a short blink every 3 seconds means cloud uploads are working; a 1 Hz blink means there is no Wi-Fi or uploads are failing.

The build uses the "single factory app (large)" partition table (1.5 MB app). Flash with `idf.py flash` (or the extension's Flash command) so the new partition table is written together with the app.

## 4. Configure Firebase

### Create project and database

1. Create a Firebase project in the Firebase Console.
2. Create a **Realtime Database** and note its full regional database URL (for example, the `...firebasedatabase.app` URL).
3. Copy the exact URL into both `firmware/include/pins_config.h` (`CLOUD_API_BASE_URL`, without a trailing slash) and `website/js/config.js` (`backend.firebase.databaseURL`).
4. In **Project settings → Your apps**, register a Web App and copy its Web App configuration.
5. Enable **Authentication → Sign-in method → Email/Password** and create guardian users under **Authentication → Users**.
6. Under **Authentication → Settings → Authorized domains**, make sure your deployed website domain is allowed. Localhost is typically available for development.

### Set website configuration

In `website/js/config.js`:

- Set `USE_LIVE_DATA: true`.
- Fill the Web App `apiKey`, `authDomain`, `projectId`, `messagingSenderId`, and `appId` values from Firebase.
- Keep `databaseURL` exactly equal to the firmware database URL.
- Keep `device.id` equal to firmware `DEVICE_ID` (`beacon-01` unless changed on both sides).

The Firebase Web App API key identifies the public Web App; it is not an Admin credential. Never put service-account credentials, Firebase Admin credentials, or privileged secrets in website code or firmware.

### Database paths used by the app

```text
/devices/beacon-01/live       Latest telemetry object, replaced with PUT
/devices/beacon-01/history    Telemetry entries, appended with POST
/devices/beacon-01/sos_alert  SOS state object, replaced with PUT
/guardians/<auth-uid>/location Guardian browser location (only after consent)
```

Live telemetry is described field by field in [website/README.md](website/README.md#database-schema-and-update-behavior). In short: a heartbeat with `status`, `timestamp`, Wi-Fi, modem, SIM/cellular, battery and GPS state, plus the last known `lat`/`lng` with `gpsFix` saying whether it is current. History holds only real new GPS fixes. SOS is written the moment the button is pressed and updated with the final location and SMS/call results. Firebase server timestamps resolve to epoch milliseconds; the website corrects for the browser clock and marks the device Delayed after about 9 seconds and Offline after 15 seconds without a heartbeat.

### Rules and important security trade-off

The firmware currently writes device records without Firebase authentication. A development test may permit those writes at the exact device node, but a public `.write: true` rule allows anyone to forge coordinates/history/SOS. **Do not use a public write rule for real deployment.** Use a trusted server/gateway that authenticates the device or implement a robust device identity and credential scheme, then grant only the required paths. Never embed a Firebase Admin key in the ESP32.

The signed-in guardian's location is stored at `/guardians/<UID>/location`, and the website writes that value using the Firebase Web SDK. Rules should restrict a guardian to their own UID. Example of a *development starting point only* (the device write is deliberately insecure and must be replaced before production):

```json
{
  "rules": {
    "devices": {
      "beacon-01": {
        ".read": "auth != null",
        ".write": true
      }
    },
    "guardians": {
      "$uid": {
        ".read": "auth != null && auth.uid === $uid",
        ".write": "auth != null && auth.uid === $uid",
        "location": {
          ".validate": "newData.hasChildren(['lat', 'lng', 'updatedAt', 'uid']) && newData.child('lat').isNumber() && newData.child('lng').isNumber() && newData.child('lat').val() >= -90 && newData.child('lat').val() <= 90 && newData.child('lng').val() >= -180 && newData.child('lng').val() <= 180 && newData.child('uid').val() === auth.uid"
        }
      }
    }
  }
}
```

Test rules with Firebase Rules Playground or the Emulator before deploying. Configure a history retention process: POST creates additional records, and unlimited history increases database size and read cost. Website rendering is capped to the most recent 50 points.

## 5. Run the website

1. Serve the `website/` folder over HTTP, for example using VS Code Live Server or a local development server. Do not double-click the HTML file; ES modules and Firebase requests require a web origin.
2. Open the served site in a modern browser. Open Developer Tools → Console/Network if the map, SDK scripts, or Firebase data do not load.
3. For real device data, ensure `USE_LIVE_DATA: true`, Web App config is valid, and the guardian signs in using the account created in Firebase Authentication.
4. The map uses OpenStreetMap tiles and requires internet access. Keep the provided attribution visible and follow the OpenStreetMap tile usage policy.
5. The browser geolocation API requires HTTPS in production (localhost is allowed for development).

### Optional Firebase Hosting deployment

Use the Firebase CLI on your development computer (not on the ESP32). From the repository, initialize Hosting with the `website/` folder as the public directory, configure it as a single-page app only if you later add routes, then deploy. Follow Firebase's current CLI prompts and check in the Hosting config, but do not include service-account credentials. The final hosting URL must be listed in Firebase Authentication's authorized domains.

The parent location request is one-shot. Click **My Location**, grant permission, and the marker is shown on that browser. With Firebase sign-in and correct rules, the location is also written under that account's UID and seen in its other signed-in sessions. The site does not track the parent continuously or in the background.

## 6. End-to-end test checklist

Perform tests in a safe place with an authorized SIM/contact:

1. **Website only:** load the map and confirm map tiles render. Click **My Location**, allow permission, and verify the parent marker appears.
2. **Firebase auth:** sign in; verify status changes to signed in. Confirm the UID-specific guardian location is readable/writable under database rules. Sign out and verify the parent listener is detached.
3. **Firmware boot:** check serial logs for successful modem communication, SIM readiness, network registration, Wi-Fi IP address, and `PUT live -> 204` (or `PATCH device -> 204` with a new GPS point) every 5 seconds. Without a SIM or GPS fix the website must still show the device Online with "No SIM" / "Searching".
4. **GPS:** take the device outdoors, wait for a valid fix, and check logged latitude/longitude are plausible for the device's physical location.
5. **Cloud live record:** confirm `/devices/beacon-01/live` changes in Firebase. Website should show matching coordinates and recent time and show Online while fresh timestamps arrive.
6. **History:** confirm new children appear under `/devices/beacon-01/history` and the website trail/recent-location list updates.
7. **SOS:** with consent, hold the physical button for 1 second (a short tap must not trigger) and verify location attempt, SMS/call behavior, Firebase SOS state, website banner, and acknowledge reset. Test separately with GPS unavailable and network unavailable.
8. **Security:** test rules as signed-out, wrong guardian, correct guardian, and device. Do not leave test-mode rules enabled.

Never use only the website's **Simulate SOS** button to claim hardware SMS/call works; that button only creates a browser-side demo event.

## 7. Troubleshooting

| Symptom | Checks |
|---|---|
| Map area blank | Confirm internet access to Leaflet CDN and OpenStreetMap tiles; inspect console/network; verify map has dimensions and reload after CSS changes. |
| Map appears but no child marker at actual location | Verify the firmware has a GPS fix and `/devices/beacon-01/live` contains numeric `lat`/`lng`; confirm device ID matches website. |
| Website reports Offline | Check `timestamp` is a Firebase-resolved epoch-millisecond number, latest live data is updating, Firebase read rules permit the signed-in account, and local clock is correct. |
| Firebase permission denied | Sign in; check Email/Password is enabled, Web App config is complete, database URL is correct, and rules allow this account/device path. |
| No cloud records | Confirm Wi-Fi credentials, 2.4 GHz access, router internet, serial Wi-Fi IP log, Firebase URL, TLS certificate bundle/build config, and database rules. Current cloud upload does not use A9G cellular data. |
| Serial log shows `No matching trusted root certificate found` and `PUT live failed: ESP_ERR_HTTP_CONNECT` | Firebase's chain ends at "GlobalSign Root CA", which ESP-IDF v6 no longer ships. The root is added from `firmware/certs/firebase_extra_roots.pem` (`CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`); keep that setting enabled, and update the file if Google changes its chain or before the root expires (Jan 2028). |
| GPS stays unavailable | Test outdoors, check antenna/connection and A9G power, wait for cold-start fix, inspect A9G UART logs. |
| SMS/call fails | Verify SIM service, antenna, balance/plan, number format, country code, network registration, and power supply stability. |
| Parent marker not saved to another session | Use HTTPS or localhost, allow location permission, sign into the same Firebase user on both sessions, and inspect `/guardians/<UID>/location` rules/data. |

## 8. Privacy and maintenance

- Parent location is personal data. Request it only with consent, minimize retention, limit account access, and provide a way to clear it if used beyond testing.
- Keep Wi-Fi credentials and personal contact numbers out of public source control.
- Monitor Firebase usage and prune history. Rotate credentials if they are exposed.
- Keep physical emergency alternatives available; web notifications and cloud connectivity can fail.
- Re-test after changing hardware, firmware, database rules, Wi-Fi, or browser hosting.
