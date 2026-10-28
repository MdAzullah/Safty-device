/* =========================================================
   LIVE DATA SERVICE — Guardian Beacon Cloud Backend Adapter
   Streams device telemetry, history, and SOS from Firebase Realtime
   Database. Device listeners are attached only while a guardian is
   signed in (database rules require auth) and are re-attached on every
   sign-in, so data appears without a page refresh.

   Freshness (Online -> Stale -> Offline) is evaluated with Firebase server
   time at the exact moment each threshold passes, and again whenever the
   tab becomes visible or the network returns, so background-tab timer
   throttling cannot delay it.
   ========================================================= */

import { APP_CONFIG } from "../config.js";

const listeners = {
  telemetry: [],
  history: [],
  sos: [],
  sosCleared: [],
  parentLocation: [],
  connection: []
};

function baseState() {
  return {
    id: APP_CONFIG.device.id,
    name: APP_CONFIG.device.name,
    phone: APP_CONFIG.device.phone,
    status: "Waiting",
    lat: null,
    lng: null,
    hasLocation: false,
    gpsFix: false,
    gpsEnabled: null,
    fixAgeSec: null,
    speed: null,
    course: null,
    satellites: null,
    hdop: null,
    battery: null,
    batteryMv: null,
    signal: null,
    gsmCsq: null,
    simReady: null,
    simStatus: null,
    gsmRegistered: null,
    modemOk: null,
    network: null,
    wifiRssi: null,
    fw: null,
    uptimeSec: null,
    heartbeatSec: null,
    diag: null,
    sosActive: false,
    timestamp: null
  };
}

let liveState = baseState();
let historyByKey = new Map();
let connection = { state: "connecting", message: "Connecting…" };

let serverOffsetMs = 0;
let freshnessTimers = [];
let pollTimers = [];
let activeRefs = [];
let parentLocationRef = null;
let sosIsActive = false;
let historyFlushPending = false;
let lifecycleBound = false;

const deviceId = () => APP_CONFIG.device.id;
const maxHistory = () => APP_CONFIG.map.maxHistoryPoints || 50;

function emit(type, payload) {
  listeners[type].forEach((cb) => cb(payload));
}

function setConnection(state, message) {
  if (connection.state === state && connection.message === message) return;
  connection = { state, message };
  emit("connection", { ...connection });
}

function normalizeTimestamp(value) {
  const timestamp = typeof value === "number" ? value : Number(value);
  if (value == null || !Number.isFinite(timestamp) || timestamp <= 0) return null;
  // Only Unix-epoch milliseconds are wall-clock times; old firmware sent uptime.
  return timestamp >= 1000000000000 ? timestamp : null;
}

function toNumber(value) {
  if (value == null || value === "") return null;
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function toBool(value) {
  return typeof value === "boolean" ? value : null;
}

function toText(value) {
  return typeof value === "string" ? value : null;
}

function validCoordinates(lat, lng) {
  return lat != null && lng != null &&
    lat >= -90 && lat <= 90 && lng >= -180 && lng <= 180 &&
    !(lat === 0 && lng === 0);
}

// Thresholds adapt to the heartbeat interval the device reports.
function thresholds(state) {
  const heartbeatMs = (state.heartbeatSec || 10) * 1000;
  return {
    staleMs: Math.max(APP_CONFIG.backend?.staleAfterMs || 9000, heartbeatMs * 1.8),
    offlineMs: Math.max(APP_CONFIG.backend?.offlineTimeoutMs || 15000, heartbeatMs * 3)
  };
}

function computeStatus(state) {
  if (!state.timestamp) return state.status === "Waiting" ? "Waiting" : "Offline";
  const age = Math.max(0, serverNow() - state.timestamp);
  const { staleMs, offlineMs } = thresholds(state);
  if (age > offlineMs) return "Offline";
  if (age > staleMs) return "Stale";
  return "Online";
}

export function serverNow() {
  return Date.now() + serverOffsetMs;
}

function publishTelemetry() {
  emit("telemetry", { ...liveState });
}

// Re-evaluates status; emits only when it changed, then arms timers for
// the next threshold crossing.
function refreshFreshness() {
  freshnessTimers.forEach(clearTimeout);
  freshnessTimers = [];

  const status = computeStatus(liveState);
  if (status !== liveState.status) {
    liveState = { ...liveState, status };
    publishTelemetry();
  }

  if (!liveState.timestamp) return;
  const age = serverNow() - liveState.timestamp;
  const { staleMs, offlineMs } = thresholds(liveState);
  [staleMs, offlineMs].forEach((limit) => {
    if (age <= limit) {
      freshnessTimers.push(setTimeout(refreshFreshness, limit - age + 50));
    }
  });
}

function bindLifecycle() {
  if (lifecycleBound || typeof document === "undefined") return;
  lifecycleBound = true;
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") refreshFreshness();
  });
  window.addEventListener("online", refreshFreshness);
  window.addEventListener("focus", refreshFreshness);
}

function historyList() {
  return [...historyByKey.values()]
    .sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0) || (a.key < b.key ? -1 : 1))
    .slice(-maxHistory());
}

// Child events arrive one by one; deliver them to the UI once per frame.
function scheduleHistoryEmit() {
  if (historyFlushPending) return;
  historyFlushPending = true;
  const flush = () => {
    historyFlushPending = false;
    emit("history", historyList());
  };
  if (typeof requestAnimationFrame === "function") requestAnimationFrame(flush);
  else setTimeout(flush, 0);
}

function normalizePoint(key, point) {
  const lat = toNumber(point?.lat);
  const lng = toNumber(point?.lng);
  if (!validCoordinates(lat, lng)) return null;
  return {
    key,
    lat,
    lng,
    speed: toNumber(point?.speed),
    hdop: toNumber(point?.hdop),
    battery: toNumber(point?.battery),
    timestamp: normalizeTimestamp(point?.timestamp)
  };
}

function friendlyError(error, path) {
  if (error?.code === "PERMISSION_DENIED" || /permission/i.test(error?.message || "")) {
    return `Permission denied reading ${path}. Check this account and the Realtime Database rules.`;
  }
  return `Could not read ${path}: ${error?.message || error}`;
}

export const liveDataService = {
  getDeviceState() {
    return { ...liveState };
  },

  getHistory() {
    return historyList();
  },

  serverNow,

  onTelemetry(callback) {
    listeners.telemetry.push(callback);
    callback({ ...liveState });
  },

  onHistory(callback) {
    listeners.history.push(callback);
    callback(historyList());
  },

  onSOS(callback) {
    listeners.sos.push(callback);
  },

  onSOSCleared(callback) {
    listeners.sosCleared.push(callback);
  },

  onParentLocation(callback) {
    listeners.parentLocation.push(callback);
  },

  onConnection(callback) {
    listeners.connection.push(callback);
    callback({ ...connection });
  },

  start() {
    bindLifecycle();
    const hasSdk = window.firebase && firebase.apps?.length > 0 && APP_CONFIG.backend?.firebase?.apiKey;

    if (hasSdk) {
      this.initFirebaseSdk();
    } else {
      const dbUrl = (APP_CONFIG.backend?.firebase?.databaseURL || "").replace(/\/+$/, "");
      if (dbUrl) {
        console.warn("Firebase SDK unavailable; using anonymous REST polling. Protected database rules will deny it.");
        this.initRestPolling(dbUrl);
      } else {
        setConnection("error", "No database URL configured in js/config.js");
      }
    }
  },

  // -------------------------------------------------------
  // Firebase SDK (primary)
  // -------------------------------------------------------

  initFirebaseSdk() {
    const db = firebase.database();

    db.ref(".info/serverTimeOffset").on("value", (snap) => {
      serverOffsetMs = Number(snap.val()) || 0;
      refreshFreshness();
    });

    db.ref(".info/connected").on("value", (snap) => {
      if (snap.val() === false && firebase.auth().currentUser) {
        setConnection("connecting", "Reconnecting to Firebase…");
      } else if (snap.val() === true && connection.state === "connecting" && activeRefs.length) {
        setConnection("live", "Connected");
      }
    });

    firebase.auth().onAuthStateChanged((user) => {
      this.detachDeviceListeners();
      this.detachParentListener();

      if (!user) {
        liveState = baseState();
        historyByKey = new Map();
        sosIsActive = false;
        refreshFreshness();
        publishTelemetry();
        emit("history", []);
        emit("sosCleared", null);
        setConnection("signed-out", "Sign in to view live device data");
        return;
      }

      setConnection("connecting", "Connecting to device feed…");
      this.attachDeviceListeners(db);
      this.attachParentListener(db, user);
    });
  },

  attachDeviceListeners(db) {
    const base = `devices/${deviceId()}`;
    const onError = (path) => (error) => {
      console.error(`Firebase listener for ${path} cancelled:`, error);
      setConnection("denied", friendlyError(error, path));
    };

    const live = db.ref(`${base}/live`);
    live.on("value", (snap) => {
      const data = snap.val();
      this.processTelemetry(data);
      setConnection("live", data
        ? "Connected"
        : `Connected. Waiting for the first heartbeat from ${deviceId()}…`);
    }, onError(`${base}/live`));
    activeRefs.push(live);

    // Child events deliver only what changed: one new point per update
    // instead of the whole window.
    historyByKey = new Map();
    const history = db.ref(`${base}/history`).limitToLast(maxHistory());
    history.on("child_added", (snap) => {
      const point = normalizePoint(snap.key, snap.val());
      if (point) {
        historyByKey.set(snap.key, point);
        scheduleHistoryEmit();
      }
    }, onError(`${base}/history`));
    history.on("child_removed", (snap) => {
      if (historyByKey.delete(snap.key)) scheduleHistoryEmit();
    });
    activeRefs.push(history);
    // An empty history never fires child_added; make the empty state visible.
    scheduleHistoryEmit();

    const sos = db.ref(`${base}/sos_alert`);
    sos.on("value", (snap) => this.processSosSnapshot(snap.val()), onError(`${base}/sos_alert`));
    activeRefs.push(sos);
  },

  detachDeviceListeners() {
    activeRefs.forEach((ref) => ref.off());
    activeRefs = [];
  },

  attachParentListener(db, user) {
    const parentPath = APP_CONFIG.backend?.parentLocationPath;
    if (!parentPath) return;
    parentLocationRef = db.ref(`${parentPath}/${user.uid}/location`);
    parentLocationRef.on("value", (snapshot) => {
      const data = snapshot.val();
      const lat = toNumber(data?.lat);
      const lng = toNumber(data?.lng);
      if (lat != null && lng != null) emit("parentLocation", { lat, lng });
    }, (error) => console.warn("Guardian location listener cancelled:", error));
  },

  detachParentListener() {
    if (parentLocationRef) {
      parentLocationRef.off();
      parentLocationRef = null;
    }
  },

  // -------------------------------------------------------
  // REST fallback (SDK failed to load)
  // -------------------------------------------------------

  initRestPolling(baseUrl) {
    const base = `${baseUrl}/devices/${deviceId()}`;
    const get = async (url) => {
      const res = await fetch(url);
      if (!res.ok) {
        throw Object.assign(new Error(`HTTP ${res.status}`), {
          code: res.status === 401 || res.status === 403 ? "PERMISSION_DENIED" : undefined
        });
      }
      return res.json();
    };

    const pollLive = async () => {
      try {
        const [live, sos] = await Promise.all([get(`${base}/live.json`), get(`${base}/sos_alert.json`)]);
        this.processTelemetry(live);
        this.processSosSnapshot(sos);
        setConnection("live", "Connected (REST polling)");
      } catch (error) {
        setConnection("denied", friendlyError(error, `devices/${deviceId()}`));
      }
    };

    const pollHistory = async () => {
      try {
        this.processHistory(await get(`${base}/history.json?orderBy="$key"&limitToLast=${maxHistory()}`));
      } catch {
        // Reported by pollLive.
      }
    };

    pollLive();
    pollHistory();
    pollTimers.push(setInterval(pollLive, 5000), setInterval(pollHistory, 15000));
  },

  // -------------------------------------------------------
  // Data processing (shared by SDK and REST)
  // -------------------------------------------------------

  processTelemetry(data) {
    if (!data) {
      liveState = baseState();
      refreshFreshness();
      publishTelemetry();
      return;
    }

    const lat = toNumber(data.lat);
    const lng = toNumber(data.lng);
    const hasLocation = validCoordinates(lat, lng);
    const gpsFix = toBool(data.gpsFix);
    const diag = data.diag && typeof data.diag === "object" ? { ...data.diag } : null;

    liveState = {
      ...baseState(),
      lat: hasLocation ? lat : null,
      lng: hasLocation ? lng : null,
      hasLocation,
      // Firmware before 1.1 only sent coordinates with a fresh fix.
      gpsFix: gpsFix ?? hasLocation,
      gpsEnabled: toBool(data.gpsEnabled),
      fixAgeSec: toNumber(data.fixAgeSec),
      speed: toNumber(data.speed),
      course: toNumber(data.course),
      satellites: toNumber(data.satellites),
      hdop: toNumber(data.hdop),
      battery: toNumber(data.battery),
      batteryMv: toNumber(data.batteryMv),
      signal: toText(data.signal),
      gsmCsq: toNumber(data.gsmCsq),
      simReady: toBool(data.simReady),
      simStatus: toText(data.simStatus),
      gsmRegistered: toBool(data.gsmRegistered),
      modemOk: toBool(data.modemOk),
      network: toText(data.network),
      wifiRssi: toNumber(data.wifiRssi),
      fw: toText(data.fw),
      uptimeSec: toNumber(data.uptimeSec),
      heartbeatSec: toNumber(data.heartbeatSec),
      diag,
      sosActive: data.sosActive === true,
      timestamp: normalizeTimestamp(data.timestamp),
      status: "Offline"
    };
    liveState.status = computeStatus(liveState);

    publishTelemetry();
    refreshFreshness();
  },

  // Whole-window replacement (REST polling and tests).
  processHistory(data) {
    const entries = Array.isArray(data)
      ? data.map((point, index) => [String(index), point])
      : Object.entries(data || {});
    historyByKey = new Map();
    entries.forEach(([key, value]) => {
      const point = normalizePoint(key, value);
      if (point) historyByKey.set(key, point);
    });
    emit("history", historyList());
  },

  processSosSnapshot(data) {
    if (data?.active === true) {
      this.processSOS(data);
    } else if (sosIsActive) {
      // Acknowledged here or in another session.
      sosIsActive = false;
      emit("sosCleared", null);
    }
  },

  processSOS(sosData) {
    sosIsActive = true;
    const lat = toNumber(sosData.lat);
    const lng = toNumber(sosData.lng);
    const hasLocation = validCoordinates(lat, lng);
    emit("sos", {
      active: true,
      eventId: sosData.eventId || null,
      deviceName: sosData.deviceName || APP_CONFIG.device.name,
      timestamp: normalizeTimestamp(sosData.timestamp) || Date.now(),
      stage: sosData.stage || null,
      locationSource: hasLocation ? (sosData.locationSource || "current") : "none",
      smsSent: toBool(sosData.smsSent),
      callStarted: toBool(sosData.callStarted),
      lat: hasLocation ? lat : null,
      lng: hasLocation ? lng : null
    });
  },

  // -------------------------------------------------------
  // Guardian actions
  // -------------------------------------------------------

  async publishParentLocation(lat, lng) {
    if (!window.firebase || !firebase.apps?.length || !APP_CONFIG.backend?.firebase?.apiKey) return false;
    const user = firebase.auth().currentUser;
    const path = APP_CONFIG.backend?.parentLocationPath;
    if (!user || !path) return false;
    try {
      await firebase.database().ref(`${path}/${user.uid}/location`).set({
        lat,
        lng,
        updatedAt: firebase.database.ServerValue.TIMESTAMP,
        uid: user.uid
      });
      return true;
    } catch (error) {
      console.error("Could not publish guardian location:", error);
      return false;
    }
  },

  // Returns true when the acknowledgement reached the database.
  async acknowledgeSOS() {
    const path = `devices/${deviceId()}/sos_alert`;
    try {
      if (window.firebase && firebase.apps?.length > 0) {
        await firebase.database().ref(path).update({
          active: false,
          acknowledgedAt: firebase.database.ServerValue.TIMESTAMP
        });
      } else {
        const dbUrl = (APP_CONFIG.backend?.firebase?.databaseURL || "").replace(/\/+$/, "");
        const response = await fetch(`${dbUrl}/${path}.json`, {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ active: false, acknowledgedAt: { ".sv": "timestamp" } })
        });
        if (!response.ok) throw new Error(`Firebase responded ${response.status}`);
      }
      sosIsActive = false;
      return true;
    } catch (err) {
      console.error("Failed to acknowledge SOS:", err);
      return false;
    }
  },

  stop() {
    this.detachDeviceListeners();
    this.detachParentListener();
    pollTimers.forEach(clearInterval);
    pollTimers = [];
    freshnessTimers.forEach(clearTimeout);
    freshnessTimers = [];
  }
};
