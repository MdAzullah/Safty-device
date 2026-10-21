/* =========================================================
   LIVE DATA SERVICE — Guardian Beacon Cloud Backend Adapter
   Connects the frontend dashboard directly to Firebase Realtime Database
   or custom REST endpoints to stream device telemetry, history, and SOS.
   ========================================================= */

import { APP_CONFIG } from "../config.js";

let listeners = {
  telemetry: [],
  history: [],
  sos: [],
  parentLocation: []
};

let liveState = {
  id: APP_CONFIG.device.id,
  name: APP_CONFIG.device.name,
  phone: APP_CONFIG.device.phone,
  status: "Connecting...",
  battery: null,
  speed: null,
  signal: null,
  lat: null,
  lng: null,
  timestamp: null
};

let historyPoints = [];
let eventSource = null;
let pollTimer = null;
let watchdogTimer = null;
let seenSosTimestamp = null;
let sosIsActive = false;
let parentLocationRef = null;

function normalizeTimestamp(value) {
  if (value == null) return null;
  const timestamp = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(timestamp) || timestamp <= 0) return null;
  // Permit old firmware that stored milliseconds since boot but only treat
  // Unix-epoch timestamps as real wall-clock times.
  return timestamp >= 1000000000000 ? timestamp : null;
}

function processDeviceSnapshot(data) {
  if (!data) return;
  if (data.live) liveDataService.processTelemetry(data.live);
  if (data.history) liveDataService.processHistory(data.history);
  if (data.sos_alert?.active) liveDataService.processSOS(data.sos_alert);
  else if (data.sos_alert?.active === false) {
    seenSosTimestamp = null;
    sosIsActive = false;
  }
}

export const liveDataService = {
  getDeviceState() {
    return { ...liveState };
  },

  getHistory() {
    return [...historyPoints];
  },

  onTelemetry(callback) {
    listeners.telemetry.push(callback);
    callback({ ...liveState });
  },

  onHistory(callback) {
    listeners.history.push(callback);
    callback([...historyPoints]);
  },

  onSOS(callback) {
    listeners.sos.push(callback);
  },

  onParentLocation(callback) {
    listeners.parentLocation.push(callback);
  },

  // Initialize real-time cloud subscription
  start() {
    const deviceId = APP_CONFIG.device.id;
    const dbUrl = (APP_CONFIG.backend?.firebase?.databaseURL || "").replace(/\/+$/, "");

    if (window.firebase && firebase.apps?.length > 0 && APP_CONFIG.backend?.firebase?.apiKey) {
      // Use Firebase JS SDK Realtime DB
      this.initFirebaseSdk(deviceId);
    } else if (dbUrl) {
      // Use Firebase Realtime Database REST Streaming (Server-Sent Events) or REST polling
      this.initRestStreaming(dbUrl, deviceId);
    } else {
      console.warn("No cloud backend URL configured in js/config.js. Falling back to polling.");
    }

    // Offline watchdog (flags device offline if no ping received within timeout)
    this.startWatchdog();
  },

  initFirebaseSdk(deviceId) {
    const db = firebase.database();

    // 1. Live Telemetry Listener
    db.ref(`devices/${deviceId}/live`).on("value", (snapshot) => {
      const data = snapshot.val();
      if (data) {
        this.processTelemetry(data);
      }
    });

    // 2. History Listener
    db.ref(`devices/${deviceId}/history`).limitToLast(APP_CONFIG.map.maxHistoryPoints || 50).on("value", (snapshot) => {
      const data = snapshot.val();
      if (data) {
        historyPoints = Object.values(data).sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
        listeners.history.forEach((cb) => cb([...historyPoints]));
      }
    });

    // 3. SOS Alert Listener
    db.ref(`devices/${deviceId}/sos_alert`).on("value", (snapshot) => {
      const data = snapshot.val();
      if (data && data.active) {
        listeners.sos.forEach((cb) => cb({
          active: true,
          deviceName: data.deviceName || APP_CONFIG.device.name,
          timestamp: data.timestamp || Date.now(),
          lat: data.lat,
          lng: data.lng
        }));
      }
    });

    const parentPath = APP_CONFIG.backend?.parentLocationPath;
    if (parentPath && firebase.auth) {
      firebase.auth().onAuthStateChanged((user) => {
        if (parentLocationRef) {
          parentLocationRef.off();
          parentLocationRef = null;
        }
        if (!user) return;
        parentLocationRef = db.ref(`${parentPath}/${user.uid}/location`);
        parentLocationRef.on("value", (snapshot) => {
          const data = snapshot.val();
          if (data && data.lat != null && data.lng != null &&
              Number.isFinite(Number(data.lat)) && Number.isFinite(Number(data.lng))) {
            listeners.parentLocation.forEach((cb) => cb({ lat: Number(data.lat), lng: Number(data.lng) }));
          }
        });
      });
    }
  },

  initRestStreaming(baseUrl, deviceId) {
    // 1. Initial REST Fetch
    this.fetchRestData(baseUrl, deviceId);

    // 2. Attempt Real-time Server-Sent Events (SSE) from Firebase REST
    const streamUrl = `${baseUrl}/devices/${deviceId}.json`;
    try {
      if (window.EventSource) {
        eventSource = new EventSource(streamUrl);
        const handleStreamEvent = (e) => {
          try {
            const parsed = JSON.parse(e.data);
            if (parsed.path === "/" && parsed.data) {
              processDeviceSnapshot(parsed.data);
            } else if (parsed.path === "/live" && parsed.data) {
              this.processTelemetry(parsed.data);
            } else if (parsed.path?.startsWith("/history")) {
              // POST allocates a Firebase child key; refresh the bounded list.
              this.fetchRestData(baseUrl, deviceId);
            } else if (parsed.path === "/sos_alert" && parsed.data?.active) {
              this.processSOS(parsed.data);
            }
          } catch (err) {
            console.error("SSE parse error:", err);
          }
        };
        eventSource.addEventListener("put", handleStreamEvent);
        eventSource.addEventListener("patch", handleStreamEvent);

        eventSource.onerror = () => {
          // If SSE fails or drops, start fallback HTTP polling
          if (!pollTimer) {
            pollTimer = setInterval(() => this.fetchRestData(baseUrl, deviceId), 5000);
          }
        };
      } else {
        pollTimer = setInterval(() => this.fetchRestData(baseUrl, deviceId), 5000);
      }
    } catch (e) {
      pollTimer = setInterval(() => this.fetchRestData(baseUrl, deviceId), 5000);
    }
  },

  async fetchRestData(baseUrl, deviceId) {
    try {
      const res = await fetch(`${baseUrl}/devices/${deviceId}.json`);
      if (!res.ok) return;
      const data = await res.json();
      if (!data) return;

      processDeviceSnapshot(data);
    } catch (err) {
      console.warn("Failed fetching live cloud data:", err);
    }
  },

  processTelemetry(data) {
    const timestamp = normalizeTimestamp(data.timestamp);
    liveState = {
      ...liveState,
      ...data,
      name: APP_CONFIG.device.name,
      status: timestamp ? "Online" : "Offline",
      timestamp
    };
    listeners.telemetry.forEach((cb) => cb({ ...liveState }));
  },

  processHistory(data) {
    const values = Array.isArray(data) ? data : Object.values(data || {});
    historyPoints = values
      .filter((point) => point && Number.isFinite(Number(point.lat)) && Number.isFinite(Number(point.lng)))
      .map((point) => ({ ...point, timestamp: normalizeTimestamp(point.timestamp) }))
      .sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0))
      .slice(-(APP_CONFIG.map.maxHistoryPoints || 50));
    listeners.history.forEach((cb) => cb([...historyPoints]));
  },

  processSOS(sosData) {
    const eventTimestamp = normalizeTimestamp(sosData.timestamp);
    if (sosIsActive && (eventTimestamp == null || eventTimestamp === seenSosTimestamp)) return;
    sosIsActive = true;
    seenSosTimestamp = eventTimestamp;
    const coordinatesValid = sosData.lat != null && sosData.lng != null &&
      Number.isFinite(Number(sosData.lat)) && Number.isFinite(Number(sosData.lng)) &&
      (Number(sosData.lat) !== 0 || Number(sosData.lng) !== 0);
    listeners.sos.forEach((cb) => cb({
      active: true,
      deviceName: sosData.deviceName || APP_CONFIG.device.name,
      timestamp: eventTimestamp || Date.now(),
      lat: coordinatesValid ? Number(sosData.lat) : null,
      lng: coordinatesValid ? Number(sosData.lng) : null
    }));
  },

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

  async acknowledgeSOS() {
    const deviceId = APP_CONFIG.device.id;
    const dbUrl = (APP_CONFIG.backend?.firebase?.databaseURL || "").replace(/\/+$/, "");

    if (window.firebase && firebase.apps?.length > 0) {
      await firebase.database().ref(`devices/${deviceId}/sos_alert`).update({ active: false });
    } else if (dbUrl) {
      try {
        const response = await fetch(`${dbUrl}/devices/${deviceId}/sos_alert.json`, {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ active: false })
        });
        if (!response.ok) throw new Error(`Firebase responded ${response.status}`);
        seenSosTimestamp = null;
        sosIsActive = false;
      } catch (err) {
        console.error("Failed to reset SOS on cloud:", err);
      }
    }
  },

  startWatchdog() {
    const timeout = APP_CONFIG.backend?.offlineTimeoutMs || 45000;
    if (watchdogTimer) clearInterval(watchdogTimer);

    watchdogTimer = setInterval(() => {
      if (!liveState.timestamp || Date.now() - liveState.timestamp > timeout) {
        if (liveState.status !== "Offline") {
          liveState.status = "Offline";
          listeners.telemetry.forEach((cb) => cb({ ...liveState }));
        }
      }
    }, 5000);
  },

  stop() {
    if (eventSource) {
      eventSource.close();
      eventSource = null;
    }
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
    if (watchdogTimer) {
      clearInterval(watchdogTimer);
      watchdogTimer = null;
    }
    if (parentLocationRef) {
      parentLocationRef.off();
      parentLocationRef = null;
    }
  }
};
