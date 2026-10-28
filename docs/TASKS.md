# Tasks

Status legend: **Verified** = observed on the real device/Firebase/browser; **Built** = compiles/tests pass but not observed on hardware; **Open** = not done.
Last reviewed: 2026-10-28 (after user hardware test: SOS button, SMS/call, GPS working).

## Known issues (open)

| # | Issue | Evidence / notes |
|---|---|---|
| K2 | **Security: any signed-in user can read device data, and device writes are open.** | Rules are the development rules from `USER_GUIDE.md`. Unknown whether Firebase Auth self sign-up is enabled. See S1–S3. |
| K3 | **Credentials were shared in plain text** during development (guardian password, Wi-Fi password). | Rotate both. |
| K4 | `history` grows without limit. | No retention mechanism; the dashboard reads only the last 50. |
| K5 | TLS root "GlobalSign Root CA" (added manually) **expires 2028-01-28**. | Replace/extend `firmware/certs/firebase_extra_roots.pem` before then. |
| K6 | All work since commit `5da2126` is **uncommitted**. | See `docs/CHANGELOG.md` → Unreleased. |
| K7 | An old hand-entered sample record (New Delhi, 2025-02-28) may remain in `history`. | Delete it in the Firebase console if it appears on the trail. |

## Done

| Item | Status |
|---|---|
| Wi-Fi + HTTPS uploads to Firebase (after adding the GlobalSign root) | Verified (`PUT live -> 204`) |
| Heartbeat independent of GPS/SIM/modem; Offline shown when device powers off | Verified (fw 1.1: ~45 s) |
| Firmware 1.2.0 running on the device with diagnostics (`diag`), `simStatus` | Verified (Firebase) |
| SIM detection fix (`+CPIN:READY` without space); Airtel SIM READY and registered, CSQ 21 | Verified (Firebase) |
| GPS fix with speed reported to the dashboard; history points written | Verified (Firebase) |
| Website sign-in, live data display, auth-gated listeners | Verified by user |
| Website redesign (light/dark, phone layout, SOS banner) | Verified in headless browser (`?demo`) |
| 5 s heartbeat; Delayed at 9 s / Offline at 15 s with exact timers | Built (logic unit-checked in Node; not timed on hardware) |
| SOS button (1 s long press) triggers the SOS sequence on hardware | Verified (user test, 2026-10-28) |
| SOS UART pre-emption, immediate SOS completion update | Built (not individually checked) |
| GPS location on hardware | Verified (user test, 2026-10-28) |
| NMEA speed/satellites/HDOP with `AT+LOCATION` fallback | Built (which path is active on this A9G is unknown; check boot log) |
| Upload retry, Wi-Fi back-off/DHCP recovery, power save off | Built |
| Task watchdog, stall restart, combined live+history PATCH | Built |
| SMS and voice call to `PARENT_NUMBER` (Airtel SIM) | Verified (user test, 2026-10-28) |

## Backlog

### Security (do first)
- **S1** Disable Firebase Auth sign-up; restrict reads to an allow-list (e.g. `devices/beacon-01/guardians/<uid>: true`).
- **S2** Device authentication: dedicated Firebase account, refresh token in NVS, `?auth=` on requests; rules allow writes only from that UID.
- **S3** Rule validation (types, coordinate ranges, server timestamps, append-only history, guardians may change only `sos_alert/active` and `acknowledgedAt`).
- **S4** Wi-Fi provisioning (BLE/SoftAP) instead of compiled credentials; flash encryption + Secure Boot v2 on the final board (irreversible eFuses).
- **S5** Host on Firebase Hosting (HTTPS); restrict the API key by HTTP referrer; App Check.
- **S6** Add GTS Root R1–R4 alongside GlobalSign; alert on certificate failures.
- **S8** History retention (scheduled cleanup or device-side cap); "clear my location" for guardians.

### Reliability
- **R2** Buffer GPS points while offline and upload in a batch after reconnect.
- **R3** Cellular data fallback (A9G as PPP modem via `esp_modem`), needs SIM/APN testing.
- **R5** Persist pending SOS across reboot; let guardian acknowledgement reach the device; hang up / keep button responsive during the 25 s call hold.
- **R6** Dedicated A9G power supply; ESP32 ADC battery measurement; low-battery alert; deep sleep when stationary.
- **R7** OTA updates (two-slot partition table, signed images, rollback).
- **R8** Multiple saved Wi-Fi networks; setup mode.
- **R9** Push notifications (FCM) for SOS/offline; installable PWA.
- **R10** Automated tests: host-side unit tests for NMEA/JSON/SIM parsers; a repo test script for `liveDataService.js`; 24 h soak test; scripted fault checklist.

### Housekeeping
- Remove or archive legacy `website/prompt.txt` and `website/data/mock-locations.json` (not used by the app).
- Decide whether `firmware/platformio.ini` is still supported (builds are done with ESP-IDF directly; PlatformIO build is unverified).
