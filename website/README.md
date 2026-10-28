# Guardian Beacon — Family Safety Dashboard

For complete ESP32/A9G wiring, Firebase configuration, security, testing, and troubleshooting instructions, see the project [USER_GUIDE.md](../USER_GUIDE.md).

The dashboard displays ESP32/A9G child-device telemetry, location history, and SOS alerts from Firebase Realtime Database. It can also show a guardian's browser location after the guardian grants permission. Browser location is shown locally immediately; it is synced to Firebase only if the guardian is signed in.

## Run locally

Serve this directory using a local HTTP server (ES modules do not work reliably from `file://`). For example, use VS Code Live Server or Python's HTTP server, then open `index.html` at `http://localhost:8000`. Browser geolocation works on HTTPS sites and localhost; it does not work on arbitrary HTTP origins.

Set `USE_LIVE_DATA: false` in `js/config.js` for the in-memory simulated demo, or `true` for Firebase. Adding `?demo` to the URL also starts the simulation without editing the config. `data/mock-locations.json` is a legacy sample and is not the active demo source.

## Firebase setup

1. Create a Firebase project and a Realtime Database. Copy its exact regional database URL.
2. Register a Web App in **Project settings → Your apps**. Copy the Web App config into `backend.firebase` in `js/config.js`, including `apiKey`, `authDomain`, `projectId`, `messagingSenderId`, and `appId`. The `databaseURL` must match firmware's `CLOUD_API_BASE_URL`.
3. Enable **Authentication → Sign-in method → Email/Password**. Create guardian accounts under Authentication → Users. The page includes sign-in/sign-out controls.
4. Host the website on HTTPS for production (Firebase Hosting is suitable). The Web App config is not an Admin credential; never put a Firebase service-account key or Admin SDK credential into the browser or firmware.
5. Set Realtime Database rules before using real data. Start with the template below, replace policy as needed, and do not leave public writes enabled in a deployed safety system.

### Example development rules

This example demonstrates authenticated guardian reads and per-user guardian-location writes. The firmware currently writes device telemetry without Firebase authentication, so the device rule is intentionally permissive for initial testing only. It is **not safe for production**: anyone who can access the database could forge a device location, history, or SOS.

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

For production, route device writes through a trusted backend or implement a proper device identity/authentication scheme, then remove `.write: true`. Do not use a shared secret embedded in public firmware as a long-term security boundary. Test rules in Firebase Rules Playground / Emulator before deploying.

## Database schema and update behavior

The firmware and website use device ID `beacon-01` and these paths:

- `devices/beacon-01/live`: heartbeat replaced every 5 seconds while the device has Wi-Fi.
- `devices/beacon-01/history`: new GPS fixes (last 50 shown on the map; no retention yet).
- `devices/beacon-01/sos_alert`: current SOS alert; the Acknowledge button writes `active: false` and `acknowledgedAt`, which closes the banner in every signed-in session.
- `guardians/<Firebase Auth UID>/location`: written after the guardian clicks **Show my location** on the map and grants permission.

Every field, its type and writer are documented in [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md#firebase-data-model). Keep that file, `firmware/src/cloud_sync.c` and `js/services/liveDataService.js` in sync.

Firmware timestamps use Firebase's server timestamp placeholder (`{ ".sv": "timestamp" }`), resolved by Realtime Database to epoch milliseconds. The website expects epoch milliseconds and corrects for the browser clock using Firebase's `.info/serverTimeOffset`; it marks the device Delayed when the last heartbeat is older than `staleAfterMs` (9 s) and Offline after `offlineTimeoutMs` (15 s); both scale up automatically for firmware with a slower `heartbeatSec`. Status changes at the exact threshold and is re-checked when the tab becomes visible again. GPS is polled every 5 seconds; `history` receives at most one entry per 15 seconds and only for new fixes.

Device listeners are attached after sign-in and re-attached on every sign-in, so data appears without reloading. If the rules deny a read, the Live Status card shows the exact path that was refused.

### Cellular/Wi-Fi transport notes

The firmware sends all Firebase writes through the ESP32 Wi-Fi HTTP client, using HTTPS with the ESP-IDF certificate bundle and real PUT/POST. Configure Wi-Fi credentials and real emergency contacts in the ignored local override `firmware/include/pins_config.local.h`; keep the public `pins_config.h` free of private values. Never commit personal credentials. Cloud writes will not work until valid Wi-Fi credentials and an internet connection are configured.

The A9G AT-command HTTP flow is not used for cloud writes because the project does not yet have a verified PUT implementation, and holding the modem UART during HTTP could delay SOS GPS/SMS/call operations. The A9G continues to provide GPS, SMS and calling. For cellular-only cloud sync, implement and verify the exact modem HTTP methods and response handling against the specific A9G firmware/manual, ideally using a separate nonblocking/queued transport that does not block emergency handling. Do not disable TLS certificate validation.

## Parent location on the map

Click the person icon (**Show my location**) on the map to request the browser's current position. Browser permission is required. The map shows the parent marker and fits the view to include both parent and child. When signed in, the location is additionally saved under that account's Firebase UID and appears in other sessions for the same account. The current implementation is a one-shot location request; click again to refresh. No location is requested in the background.

Location is sensitive personal information. Share it only with informed consent, use HTTPS, enforce authenticated rules, and consider allowing the guardian to clear the saved location.

## Status and limitations

What is implemented, what has been verified on hardware, known issues, and planned work are tracked in [`docs/TASKS.md`](../docs/TASKS.md). Not implemented: continuous/background guardian location, push notifications, geofencing, multi-device selection, and cellular data upload.
