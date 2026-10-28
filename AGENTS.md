# AGENTS.md — Shared instructions for AI coding agents

Guardian Beacon is a child-safety tracker: **ESP32 firmware** (ESP-IDF, C) driving an **AI-Thinker A9G** (GPS + GSM), a **Firebase Realtime Database**, and a **static web dashboard** (vanilla ES modules + Leaflet). Read `docs/PROJECT_CONTEXT.md` and `docs/ARCHITECTURE.md` before non-trivial changes; check `docs/TASKS.md` for open work and known issues.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/` | ESP-IDF project (open this folder as the IDF project, not the repo root) |
| `firmware/src/` | `main.c` (SOS state machine, safety + modem tasks), `cloud_sync.c` (Wi-Fi, HTTPS, Firebase), `device_state.c` (shared state), `a9g_module.c` (AT-command driver) |
| `firmware/include/` | Headers; `pins_config.h` (public config); `pins_config.local.h` (**private, git-ignored**) |
| `firmware/certs/` | Extra TLS root certificate added to the ESP-IDF bundle |
| `website/` | Dashboard: `index.html`, `css/style.css`, `js/` (no build step, no npm dependencies) |
| `tools/check-device.mjs` | Node CLI: signs in as a guardian and reports device status from Firebase |
| `docs/` | Project context, architecture, tasks, changelog |
| `USER_GUIDE.md` | Hardware wiring, Firebase setup, end-to-end test checklist |

## Commands

```powershell
# Firmware (Windows, ESP-IDF v6.0.1 installed via EIM)
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
cd firmware
idf.py build
idf.py -p COM4 flash monitor     # COM port varies; Ctrl+] exits the monitor
```

```bash
# Website: serve the folder over HTTP (ES modules fail from file://)
npx --yes serve website            # or VS Code Live Server, or: python -m http.server -d website 8000
# Simulated data without Firebase: open http://localhost:<port>/?demo

# Device status from Firebase (prompts for the guardian password)
node tools/check-device.mjs <guardian-email> [--watch]

# JS syntax check (no test framework is set up)
node --check --input-type=module < website/js/app.js
```

There is **no automated test suite** in the repository. Verify firmware by building; verify behaviour on hardware via the serial log and `tools/check-device.mjs`; verify the website by serving it (use `?demo` when Firebase access is not available).

## Rules

1. **Safety first.** The SOS path (`safety_task` in `main.c`) must never wait on the network. Network I/O happens only in `cloud_task`; other tasks post to its queue or update `device_state`. Keep SOS able to pre-empt background modem commands (`a9g_preempt_background`).
2. **Subsystems report independently.** A missing GPS fix, SIM, or modem must never stop the heartbeat. Report unknown values as `null`/omitted, never as fake defaults.
3. **Keep the data contract in sync.** Any change to a Firebase field touches the firmware writer (`cloud_sync.c`), the website reader (`liveDataService.js`), and the schema in `docs/ARCHITECTURE.md`.
4. **Timestamps are Firebase server time** (`{".sv":"timestamp"}`). The website compares them with `serverNow()`; do not use device or browser clocks for freshness.
5. **Never disable TLS verification.** If HTTPS fails with "No matching trusted root certificate", update `firmware/certs/firebase_extra_roots.pem` (see `docs/ARCHITECTURE.md`).
6. **No secrets in tracked files.** Wi-Fi credentials and phone numbers go only in `firmware/include/pins_config.local.h`; private website values in `website/js/config.local.js`. Never print, copy, or commit their contents. The Firebase *web* config in `website/js/config.js` is public by design; service-account/Admin credentials must never appear anywhere.
7. **Do not change dependencies or rewrite working code without approval.** Prefer small, targeted edits.
8. **Do not claim hardware behaviour is verified** unless it was observed on the device (serial log or Firebase). Mark unverified work as such in `docs/TASKS.md`.
9. **Update docs with code**: add an entry to `docs/CHANGELOG.md`; move items in `docs/TASKS.md`; bump `FIRMWARE_VERSION` in `pins_config.h` for firmware behaviour changes.

## Code style

- **Firmware (C, ESP-IDF):** match the file you edit. `main.c`, `cloud_sync.c`, and `device_state.c` use 4-space Allman braces (`main.c` additionally uses a very vertical one-argument-per-line style); `a9g_module.c` uses 2-space, same-line braces. Tunables belong in `pins_config.h` as `#define`s (wrap user-overridable ones in `#ifndef`). Use `ESP_LOG*` with the file's `TAG`.
- **Website (JS):** ES modules, no framework, no bundler. `const $ = (id) => document.getElementById(id)`. Services emit plain objects through listener arrays; controllers own DOM rendering and update the DOM only when values change.
- **CSS:** use the design tokens in `:root` / `[data-theme="dark"]` of `style.css`; colour carries meaning (accent = interactive, green = online, amber = delayed, red = offline/SOS). No new fonts or UI libraries without approval.
- Comments explain *why*, not *what*. Keep the existing comment density.

## Hardware facts agents get wrong

- The A9G is **2G (GSM) only**. SIMs without a 2G network (e.g. Jio in India) cannot register, call, or SMS.
- The A9G checks the SIM at power-up; re-seating a SIM requires power-cycling the A9G, not just resetting the ESP32.
- SOS requires the button (GPIO 4 → GND) to be held for `SOS_LONG_PRESS_MS` (default 1000 ms).
- The board has 4 MB flash; the build uses the "single factory app (large)" partition table.
