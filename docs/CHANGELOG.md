# Changelog

Format: newest first. Firmware versions follow `FIRMWARE_VERSION` in `firmware/include/pins_config.h`. Verification status per item is tracked in `docs/TASKS.md`.

## Unreleased (uncommitted since `5da2126`)

### Firmware 1.2.0 — 2026-10-28
- SOS requires a continuous 1 s press (`SOS_LONG_PRESS_MS`); noise and taps are ignored.
- SOS pre-empts background A9G commands; the SOS completion update is sent immediately after the alert.
- Heartbeat every 5 s (`heartbeatSec` reported); combined live + history multi-path PATCH once SNTP time is synced.
- One immediate retry on HTTPS transport errors; Wi-Fi reconnect back-off (1–30 s), DHCP stall recovery, lost-IP handling; Wi-Fi power save off by default.
- GPS via NMEA (`AT+GPSRD`) with real speed, course, satellites, HDOP; automatic fallback to `AT+LOCATION`; rejects implausible jumps; low-accuracy fixes excluded from history.
- Task watchdog (60 s) on cloud and modem tasks; restart after 10 min without a successful upload.
- `diag` object in `/live` (reset reason, boot count, heap, upload counters, Wi-Fi reconnects, clock sync); boot count in NVS.
- SIM detection accepts `+CPIN:READY` and `+CPIN: READY`; `simStatus` reports the modem's SIM answer; `signal` distinguishes "No SIM", "SIM locked", "SIM error".
- Flash size set to 4 MB.

### Firmware 1.1.0 — 2026-10-28
- Split into `safety_task`, `cloud_task`, `modem_task`; new `cloud_sync.c` and `device_state.c`.
- Heartbeat to `/live` independent of GPS fix, SIM, and modem; unknown values reported as null.
- SOS alert published immediately on press and updated with location and SMS/call results.
- A9G recovery when it stops responding; one-shot SIM/registration/SMS setup.
- Removed unused A9G GPRS/HTTP code; no fake battery/signal defaults.
- Large single-app partition (1.5 MB); added GlobalSign Root CA to the TLS bundle (fixes `No matching trusted root certificate`).

### Website — 2026-10-28
- Fixed truncated Firebase API key (sign-in previously impossible).
- Listeners attached after sign-in and re-attached on every sign-in; permission errors shown in the UI.
- Freshness from Firebase server time with Online / Delayed / Offline at exact thresholds; re-checks on tab focus/visibility/online.
- History via `child_added`/`child_removed`; diffed DOM updates; deferred script loading; `?demo` mode.
- Redesign: design tokens for light/dark, IBM Plex type, SVG icons, sign-in card, status chip, panel sections (location, health, actions, history, diagnostics), SOS banner with chime, responsive phone layout, reduced-motion support.
- OpenStreetMap tiles with CSS colour grading (CARTO tiles require an API key).

### Tools and docs — 2026-10-28
- `tools/check-device.mjs` device status checker.
- `AGENTS.md`, `CLAUDE.md`, `docs/` (context, architecture, tasks, changelog); README rewritten; USER_GUIDE and website README updated.

## `5da2126` — Publish privacy-sanitized project snapshot
Initial committed version: ESP32/A9G firmware (GPS, SOS SMS/call, Wi-Fi uploads only with a GPS fix) and Leaflet dashboard with Firebase and demo modes.
