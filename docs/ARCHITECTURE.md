# Architecture

```
 SOS button ─┐                          ┌──────────────── Firebase ────────────────┐
 LED/buzzer ─┤   ESP32 (firmware 1.2)   │  Realtime Database   Authentication       │
             │ ┌──────────────────────┐ │  devices/<id>/live    (Email/Password)    │
 A9G (UART2)─┼─┤ modem_task  (prio 3) │ │  devices/<id>/history                     │
  GPS, GSM   │ │ safety_task (prio 10)│ │  devices/<id>/sos_alert                   │
             │ │ cloud_task  (prio 4) ├─┼─► HTTPS REST (Wi-Fi) ─►                    │
             │ └──────────────────────┘ │  guardians/<uid>/location                 │
             └─ SMS / voice call (2G)   └───────────────────────▲──────────────────┘
                                                                 │ Web SDK (signed in)
                                              Website (Leaflet dashboard, browser)
```

## Firmware (`firmware/`)

### Tasks

| Task | File | Priority | Responsibility |
|---|---|---|---|
| `safety_task` | `src/main.c` | 10 | Polls the SOS button every 50 ms (long-press detection), runs the SOS state machine, drives the status LED. Never does network I/O. |
| `cloud_task` | `src/cloud_sync.c` | 4 | Wi-Fi supervision, SNTP, persistent HTTPS connection to Firebase, heartbeat, history, SOS delivery/retry, stall restart. Subscribed to the task watchdog. |
| `modem_task` | `src/main.c` | 3 | A9G bring-up and recovery; polls GPS every 5 s, cellular every 30 s, battery every 60 s (one subsystem per pass). Subscribed to the task watchdog. Yields the UART during SOS. |

Shared state lives in `device_state.c` (mutex-protected; readers take snapshots). The A9G UART is protected by a mutex in `a9g_module.c`; commands issued by the background task abort early while SOS has pre-empted it.

### SOS sequence (`main.c`)

1. Button held LOW for `SOS_LONG_PRESS_MS` (1000 ms) → alert queued to the cloud (`sos_alert` PUT, `active: true`), two beeps, LED on.
2. Up to 10 s (`SOS_GPS_FIRST_WINDOW_MS`) trying for a fresh fix via `AT+LOCATION=2`.
3. SMS to `PARENT_NUMBER` (and `PARENT_NUMBER_2`) with the current or last known location, or no SMS if no location exists.
4. Voice call to `PARENT_NUMBER` (`ATD<number>;`), then a 25 s hold with LED blinking. The firmware does not hang up.
5. `sos_alert` PATCHed with `stage: "completed"`, location source, `smsSent`, `callStarted`.

### Timing constants (`include/pins_config.h`)

| Constant | Value | Purpose |
|---|---|---|
| `CLOUD_HEARTBEAT_MS` | 5000 | `/live` write interval |
| `CLOUD_HISTORY_MIN_INTERVAL_MS` | 15000 | Minimum spacing of history points |
| `GPS_PERIODIC_UPDATE_MS` / `GPS_NMEA_LISTEN_MS` | 5000 / 2200 | GPS poll interval / NMEA listen window |
| `GPS_FIX_STALE_MS` | 30000 | A fix older than this is "searching" |
| `CLOUD_HTTP_TIMEOUT_MS` | 8000 | HTTPS timeout (one immediate retry on transport error) |
| `WIFI_RECONNECT_MIN/MAX_BACKOFF_MS` | 1000 / 30000 | Reconnect back-off |
| `TASK_WDT_TIMEOUT_MS` | 60000 | Task watchdog (panic → reboot) |
| `CLOUD_STALL_RESTART_MS` | 600000 | Reboot if Wi-Fi is up but nothing uploads |

### GPS

Primary: NMEA stream (`AT+GPSRD=1` for 2.2 s, then `=0`), parsing checksummed RMC/GGA for position, speed, course, satellites, HDOP. After 3 empty reads the firmware falls back to `AT+LOCATION=2` (position only; speed derived from consecutive fixes). Fixes implying > 250 km/h are rejected; fixes with HDOP > 5 are shown live but not added to history.

### Networking and TLS

- ESP-IDF `esp_http_client` with the certificate bundle, reused keep-alive connection, `?print=silent` (204 responses).
- Firebase's chain ends at **GlobalSign Root CA**, which ESP-IDF v6 no longer ships. It is appended from `firmware/certs/firebase_extra_roots.pem` via `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`. **That root expires 2028-01-28.**
- SNTP (`pool.ntp.org`) provides wall-clock time used only to generate chronologically ordered history keys; record timestamps are always server time.
- Wi-Fi power save is off by default (`WIFI_POWER_SAVE 0`) for latency.

### Build configuration

ESP-IDF v6.0.1, target `esp32`, 4 MB flash, partition table `partitions_singleapp_large.csv` (1.5 MB app; build ≈ 1.0 MB). Both `sdkconfig` and `sdkconfig.esp32doit-devkit-v1` carry the same changes. Component dependencies are listed in `src/CMakeLists.txt`.

## Firebase data model

All device paths are under `devices/<DEVICE_ID>/` (`DEVICE_ID = "beacon-01"`, matching `device.id` in `website/js/config.js`).

### `live` — replaced every 5 s (PUT, or multi-path PATCH at the device root together with a history point)

| Field | Type | Notes |
|---|---|---|
| `status` | string | Always `"Online"` when written; the website computes freshness itself |
| `timestamp` | number | Server time (ms) |
| `deviceName`, `fw`, `uptimeSec`, `heartbeatSec` | string/number | Identity and heartbeat interval |
| `network`, `wifiRssi` | string, number | `"Wi-Fi"`, dBm |
| `gpsEnabled`, `gpsFix` | bool | `gpsFix` = fix ≤ 30 s old |
| `lat`, `lng`, `fixAgeSec` | number | Last known position; omitted before the first fix |
| `speed` | number \| null | km/h; null without a current fix |
| `course`, `satellites`, `hdop` | number | Present only when known (NMEA) |
| `battery` | number \| null | % from A9G `AT+CBC?`; `batteryMv` when reported |
| `modemOk`, `simReady`, `simStatus`, `gsmRegistered`, `gsmCsq`, `signal` | mixed | `simStatus` is the A9G's answer (`READY`, `SIM PIN`, `NOT INSERTED`, …); `signal` is a label (`No SIM`, `SIM locked`, `Good`, …) |
| `sosActive` | bool | SOS sequence running on the device |
| `diag` | object | `resetReason`, `bootCount`, `heapFree`, `heapMin`, `uploadsOk`, `uploadsFailed`, `wifiReconnects`, `timeSynced` |

### `history/<pushId>` — appended for new fixes (≥ 15 s apart, HDOP ≤ 5)

`lat`, `lng`, `timestamp` (server), optional `speed`, `hdop`, `battery`. Keys are Firebase push IDs (server-generated by POST, or device-generated from SNTP time); both sort chronologically. **No retention: grows indefinitely.**

### `sos_alert` — one current alert

- Trigger (PUT): `active: true`, `eventId`, `deviceName`, `stage: "triggered"`, `locationSource` (`last_known`/`none`), optional `lat`/`lng`, `timestamp`.
- Update (PATCH, never changes `active`): `stage: "completed"`, `locationSource` (`current`/`last_known`/`none`), `smsSent`, `callStarted`, `updatedAt`.
- Acknowledge (website): `active: false`, `acknowledgedAt`.

### `guardians/<uid>/location`

Written by the website after the guardian clicks "Show my location" and grants permission: `lat`, `lng`, `updatedAt`, `uid`.

## Website (`website/`)

| Module | Responsibility |
|---|---|
| `js/config.js` | Device ID, Firebase web config (public), freshness thresholds, map settings; `?demo` switches to simulation; optionally imports git-ignored `config.local.js` |
| `js/services/liveDataService.js` | Auth-gated Firebase listeners (`live` value, `history` child_added/removed on the last 50, `sos_alert` value), server-time offset, exact Online → Delayed → Offline timers, REST fallback when the SDK fails to load |
| `js/demoData.js` | In-memory simulated device |
| `js/app.js` | Wiring: auth UI, status chip/notice, actions, theme, toasts |
| `js/modules/mapController.js` | Leaflet map, device/guardian markers, incremental history trail |
| `js/modules/sidebarController.js` | Panel rendering (diffed DOM updates), diagnostics |
| `js/modules/sosController.js` | SOS banner, tab title, chime |

**Freshness:** with heartbeat `h` seconds (from `heartbeatSec`, default 10 for older firmware): Delayed after `max(staleAfterMs, 1.8h)`, Offline after `max(offlineTimeoutMs, 3h)` — 9 s / 15 s for firmware 1.2. Re-evaluated at the exact threshold, on tab focus/visibility, and when the browser comes back online.

## Security model (current)

| Area | Current state | Risk |
|---|---|---|
| Device → Firebase writes | Unauthenticated; rules allow `.write: true` on `devices/beacon-01` | Anyone with the database URL can forge location/SOS |
| Guardian reads | Rules require `auth != null` | Any authenticated user can read, including self-registered accounts if sign-up is enabled |
| Guardian location | Rules restrict `guardians/<uid>` to that uid | OK |
| Firebase web config | In `website/js/config.js` (public by design) | Restrict the API key to the hosting domain for production |
| Device secrets | `pins_config.local.h` compiled into flash; no flash encryption | Readable from a stolen board |
| TLS | Certificate-validated HTTPS | OK; root expiry 2028 |

The development rules are in `USER_GUIDE.md` §4. Planned hardening is in `docs/TASKS.md`.
