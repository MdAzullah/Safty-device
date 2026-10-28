#!/usr/bin/env node
/* =========================================================
   Guardian Beacon — device connection check
   Signs in with a guardian account (same as the website), then reads the
   device's Firebase data and reports whether it is online and what it sends.

   Usage (Node 18+), from the repository root:
     node tools/check-device.mjs guardian@example.com
     node tools/check-device.mjs guardian@example.com --watch   (refresh every 10 s)

   The password is prompted for (hidden), or read from GUARDIAN_PASSWORD.
   ========================================================= */

import readline from "node:readline";
import { APP_CONFIG } from "../website/js/config.js";

const API_KEY = APP_CONFIG.backend.firebase.apiKey;
const DB_URL = APP_CONFIG.backend.firebase.databaseURL.replace(/\/+$/, "");
const DEVICE_ID = APP_CONFIG.device.id;
const OFFLINE_MS = APP_CONFIG.backend.offlineTimeoutMs || 35000;

const args = process.argv.slice(2);
const email = args.find((a) => !a.startsWith("--"));
const watch = args.includes("--watch");

const ok = (msg) => console.log(`  \x1b[32m✔\x1b[0m ${msg}`);
const warn = (msg) => console.log(`  \x1b[33m!\x1b[0m ${msg}`);
const fail = (msg) => console.log(`  \x1b[31m✖\x1b[0m ${msg}`);

function promptHidden(question) {
  if (process.env.GUARDIAN_PASSWORD) return Promise.resolve(process.env.GUARDIAN_PASSWORD);
  return new Promise((resolve) => {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout, terminal: true });
    rl._writeToOutput = (s) => { if (s.includes(question)) process.stdout.write(s); };
    rl.question(question, (answer) => {
      rl.close();
      process.stdout.write("\n");
      resolve(answer);
    });
  });
}

const AUTH_ERRORS = {
  INVALID_LOGIN_CREDENTIALS: "Wrong password, or this email has no account in Firebase Authentication → Users.",
  INVALID_PASSWORD: "Wrong password.",
  EMAIL_NOT_FOUND: "No account with this email in Firebase Authentication → Users.",
  INVALID_EMAIL: "The email address is not valid.",
  USER_DISABLED: "This account is disabled in Firebase Authentication.",
  OPERATION_NOT_ALLOWED: "Email/Password sign-in is disabled (Authentication → Sign-in method).",
  TOO_MANY_ATTEMPTS_TRY_LATER: "Too many attempts; wait a few minutes."
};

async function signIn(password) {
  const res = await fetch(
    `https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=${API_KEY}`,
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ email, password, returnSecureToken: true })
    }
  );
  const body = await res.json();
  if (!res.ok) {
    const code = (body.error?.message || "UNKNOWN").split(" ")[0];
    const reason = code.startsWith("API_KEY") || /API key/i.test(body.error?.message || "")
      ? "The apiKey in website/js/config.js is invalid."
      : AUTH_ERRORS[code] || body.error?.message;
    throw new Error(`Sign-in failed: ${reason}`);
  }
  return body.idToken;
}

async function read(path, token) {
  const res = await fetch(`${DB_URL}/${path}.json?auth=${token}`);
  const serverNow = Date.parse(res.headers.get("date")) || Date.now();
  if (res.status === 401 || res.status === 403) {
    throw Object.assign(new Error(`Database rules deny reading /${path} for this account.`), { denied: true });
  }
  if (!res.ok) throw new Error(`Reading /${path} failed: HTTP ${res.status}`);
  return { data: await res.json(), serverNow };
}

function age(ms) {
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

function show(label, value) {
  console.log(`      ${label.padEnd(14)} ${value ?? "—"}`);
}

async function check(token) {
  const base = `devices/${DEVICE_ID}`;
  console.log(`\nDevice ${DEVICE_ID}  (${new Date().toLocaleTimeString()})`);

  const { data: live, serverNow } = await read(`${base}/live`, token);
  if (!live) {
    fail(`No data at /${base}/live — the device has never uploaded.`);
    console.log("      Check the serial monitor for 'Wi-Fi connected' and 'PUT live -> 204'.");
    console.log("      'PUT live -> 401' there means the rules deny device writes.");
    return;
  }

  const ts = Number(live.timestamp);
  if (!Number.isFinite(ts) || ts < 1e12) {
    warn("live.timestamp is missing or not a server timestamp (old firmware?).");
  } else {
    const ageMs = serverNow - ts;
    if (ageMs <= OFFLINE_MS) ok(`ONLINE — last heartbeat ${age(ageMs)} ago`);
    else fail(`OFFLINE — last heartbeat ${age(ageMs)} ago (${new Date(ts).toLocaleString()})`);
  }

  show("Firmware", live.fw ? `${live.fw}, uptime ${age((live.uptimeSec || 0) * 1000)}` : "pre-1.1 (no heartbeat fields)");
  show("Wi-Fi", live.wifiRssi ? `${live.wifiRssi} dBm` : live.network);
  show("Modem", live.modemOk == null ? null : live.modemOk ? "A9G responding" : "NOT responding");
  show("GPS", live.gpsFix == null ? "not reported" : live.gpsFix ? "fix" : live.gpsEnabled === false ? "disabled" : "searching");
  show("Location", live.lat != null ? `${live.lat}, ${live.lng}${live.fixAgeSec != null ? ` (fix ${age(live.fixAgeSec * 1000)} old)` : ""}` : "none yet");
  show("Speed", live.speed != null ? `${live.speed} km/h` : null);
  show("Battery", live.battery != null ? `${live.battery}%` : "not reported");
  show("Cellular", live.simReady === false ? "No SIM" : `${live.signal ?? "—"}${live.gsmCsq != null ? ` (CSQ ${live.gsmCsq})` : ""}`);

  const { data: history } = await read(`${base}/history`, token).catch(() => ({ data: null }));
  const count = history ? Object.keys(history).length : 0;
  show("History", `${count} point${count === 1 ? "" : "s"}`);

  const { data: sos } = await read(`${base}/sos_alert`, token).catch(() => ({ data: null }));
  show("SOS", sos?.active ? `ACTIVE (${sos.stage || "triggered"})` : "none");
}

async function main() {
  if (!email) {
    console.log("Usage: node tools/check-device.mjs <guardian-email> [--watch]");
    process.exitCode = 2;
    return;
  }

  console.log(`Firebase project: ${APP_CONFIG.backend.firebase.projectId}`);
  console.log(`Database:         ${DB_URL}`);

  const password = await promptHidden(`Password for ${email}: `);

  let token;
  try {
    token = await signIn(password);
    ok(`Signed in as ${email}`);
  } catch (err) {
    fail(err.message);
    process.exitCode = 1;
    return;
  }

  try {
    await check(token);
    while (watch) {
      await new Promise((r) => setTimeout(r, 10000));
      await check(token);
    }
  } catch (err) {
    fail(err.message);
    if (err.denied) {
      console.log('      Rules need at least: devices/beacon-01 ".read": "auth != null" (see USER_GUIDE.md).');
    }
    process.exitCode = 1;
    return;
  }
}

main();
