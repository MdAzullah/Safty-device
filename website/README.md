# Guardian Beacon — Family Safety Dashboard

For complete ESP32/A9G wiring, Firebase configuration, security, testing, and troubleshooting instructions, see the project [USER_GUIDE.md](../USER_GUIDE.md).

The dashboard displays ESP32/A9G child-device telemetry, location history, and SOS alerts from Firebase Realtime Database. It can also show a guardian's browser location after the guardian grants permission. Browser location is shown locally immediately; it is synced to Firebase only if the guardian is signed in.

## Run locally

Serve this directory using a local HTTP server (ES modules do not work reliably from `file://`). For example, use VS Code Live Server or Python's HTTP server, then open `index.html` at `http://localhost:8000`. Browser geolocation works on HTTPS sites and localhost; it does not work on arbitrary HTTP origins.

Set `USE_LIVE_DATA: false` in `js/config.js` for the in-memory simulated demo, or `true` for Firebase. `data/mock-locations.json` is a legacy sample and is not the active demo source.

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

- `devices/beacon-01/live`: object replaced with `PUT`; fields are `lat`, `lng`, `speed`, `battery`, `signal`, `network`, `status`, and `timestamp`.
- `devices/beacon-01/history`: a telemetry object appended with `POST`; each entry has coordinates, metrics, and timestamp. Apply retention/pruning so the history does not grow forever. The dashboard currently limits displayed points to 50.
- `devices/beacon-01/sos_alert`: object replaced with `PUT`; fields include `active`, `deviceName`, optional `lat`/`lng`, and `timestamp`. The acknowledge button writes `active: false`.
- `guardians/<Firebase Auth UID>/location`: guardian coordinates, `updatedAt`, and `uid`. Written after the guardian clicks **My Location**, grants browser location permission, and is signed in. The same authenticated user can see the synced marker on their other logged-in sessions.

Firmware timestamps use Firebase's server timestamp placeholder (`{ ".sv": "timestamp" }`), resolved by Realtime Database to epoch milliseconds. The website expects epoch milliseconds; it marks the device offline if there is no valid timestamp or it is older than 45 seconds. GPS fixes are sent every 15 seconds when valid; a stale GPS fix is not resent as a fresh update.

### Cellular/Wi-Fi transport notes

The firmware sends all Firebase writes through the ESP32 Wi-Fi HTTP client, using HTTPS with the ESP-IDF certificate bundle and real PUT/POST. Configure Wi-Fi credentials and real emergency contacts in the ignored local override `firmware/include/pins_config.local.h`; keep the public `pins_config.h` free of private values. Never commit personal credentials. Cloud writes will not work until valid Wi-Fi credentials and an internet connection are configured.

The A9G AT-command HTTP flow is not used for cloud writes because the project does not yet have a verified PUT implementation, and holding the modem UART during HTTP could delay SOS GPS/SMS/call operations. The A9G continues to provide GPS, SMS and calling. For cellular-only cloud sync, implement and verify the exact modem HTTP methods and response handling against the specific A9G firmware/manual, ideally using a separate nonblocking/queued transport that does not block emergency handling. Do not disable TLS certificate validation.

## Parent location on the map

Click **My Location** to request the browser's current position. Browser permission is required. The map shows the parent marker and fits the view to include both parent and child. When signed in, the location is additionally saved under that account's Firebase UID and appears in other sessions for the same account. The current implementation is a one-shot location request; click again to refresh. No location is requested in the background.

Location is sensitive personal information. Share it only with informed consent, use HTTPS, enforce authenticated rules, and consider allowing the guardian to clear the saved location.

## Current implemented features and limitations

- ESP32/A9G GPS, SOS button flow, SMS/call attempts, and cloud telemetry serialization.
- Live Firebase REST polling/SSE and in-memory demo mode.
- Child live marker, history trail, status panel, and SOS banner.
- Explicit browser parent-location request, map marker, and optional authenticated Firebase sync.
- Email/password Firebase sign-in controls.

Not yet implemented: continuous parent location tracking, background tracking, push notifications, geofencing, multi-device selection, or verified cellular PUT/TLS. Hardware transport and Firebase authorization must be tested on the actual board/database before relying on emergency alerts.
