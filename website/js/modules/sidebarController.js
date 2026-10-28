/* =========================================================
   SIDEBAR CONTROLLER
   Renders the device panel: summary, location, health, recent
   locations and diagnostics. Every metric renders independently (an
   unknown value shows "—"), and the DOM is touched only when a value
   actually changes.
   ========================================================= */

const $ = (id) => document.getElementById(id);

function formatTime(ts) {
  if (!ts) return "—";
  return new Date(ts).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

export function formatAge(ms) {
  if (ms == null) return "";
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 5) return "just now";
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} h ago`;
  return `${Math.floor(h / 24)} d ago`;
}

function formatDuration(sec) {
  if (sec == null) return "—";
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  return h > 0 ? `${h}h ${m}m` : `${m}m ${Math.floor(sec % 60)}s`;
}

function setText(id, text) {
  const el = $(id);
  if (el && el.textContent !== text) el.textContent = text;
}

function setTone(id, tone) {
  const el = $(id);
  if (el && el.dataset.tone !== tone) el.dataset.tone = tone;
}

function wifiQuality(rssi) {
  if (rssi == null || rssi === 0) return null;
  if (rssi >= -60) return ["Excellent", "ok"];
  if (rssi >= -70) return ["Good", "ok"];
  if (rssi >= -80) return ["Fair", "warn"];
  return ["Weak", "danger"];
}

const STATUS_BADGE = {
  Online: ["Online", "ok"],
  Stale: ["Delayed", "warn"],
  Offline: ["Offline", "danger"],
  Waiting: ["Waiting", "neutral"]
};

export class SidebarController {
  constructor() {
    this.historyKey = "";
    this.diagKey = "";
  }

  updateStatus(state) {
    const {
      name, id, lat, lng, timestamp, battery, batteryMv, speed, signal, status,
      wifiRssi, hasLocation, gpsFix, gpsEnabled, fixAgeSec, satellites, hdop,
      simReady, gsmRegistered, gsmCsq, modemOk
    } = state;
    const reported = Boolean(timestamp);

    setText("statChild", name || "Guardian Beacon");
    if (id) setText("deviceIdLabel", id);

    const [badgeText, badgeTone] = STATUS_BADGE[status] || STATUS_BADGE.Waiting;
    setText("deviceOnlinePill", badgeText);
    setTone("deviceOnlinePill", badgeTone);

    // Location
    const coords = hasLocation && lat != null && lng != null ? `${lat.toFixed(5)}, ${lng.toFixed(5)}` : "—";
    setText("statCoords", coords);
    const copyBtn = $("btnCopyCoords");
    if (copyBtn) copyBtn.disabled = coords === "—";

    if (!reported) setText("statGps", "—");
    else if (gpsFix) setText("statGps", satellites != null ? `Fixed · ${satellites} sat` : "Fixed");
    else if (gpsEnabled === false || modemOk === false) setText("statGps", "Unavailable");
    else if (hasLocation && fixAgeSec != null) setText("statGps", `Searching · ${formatAge(fixAgeSec * 1000)}`);
    else setText("statGps", satellites != null ? `Searching · ${satellites} sat` : "Searching");

    setText("statSpeed", gpsFix && speed != null ? `${Number(speed).toFixed(1)} km/h` : "—");
    // HDOP × ~5 m is a common rough horizontal-accuracy estimate for consumer GPS.
    setText("statAccuracy", gpsFix && hdop != null && hdop > 0 ? `±${Math.max(2, Math.round(hdop * 5))} m` : "—");

    // Battery
    if (battery != null) {
      setText("statBattery", `${battery}%${batteryMv ? ` · ${(batteryMv / 1000).toFixed(2)} V` : ""}`);
      setTone("rowBattery", battery <= 15 ? "danger" : battery <= 30 ? "warn" : "ok");
    } else {
      setText("statBattery", reported ? "Not reported" : "—");
      setTone("rowBattery", "neutral");
    }

    // Wi-Fi (the device's uplink)
    const quality = wifiQuality(wifiRssi);
    if (reported && quality) {
      setText("statNetwork", `${quality[0]} · ${wifiRssi} dBm`);
      setTone("rowWifi", status === "Online" ? quality[1] : "neutral");
    } else {
      setText("statNetwork", "—");
      setTone("rowWifi", "neutral");
    }

    // Cellular (A9G SIM / GSM)
    if (!reported) {
      setText("statSignal", "—");
      setTone("rowCellular", "neutral");
    } else if (modemOk === false) {
      setText("statSignal", "Unavailable");
      setTone("rowCellular", "danger");
    } else if (simReady === false) {
      setText("statSignal", signal && signal !== "Unknown" ? signal : "No SIM");
      setTone("rowCellular", "warn");
    } else if (gsmRegistered === false) {
      setText("statSignal", "Searching network");
      setTone("rowCellular", "warn");
    } else {
      setText("statSignal", `${signal || "—"}${gsmCsq != null ? ` · CSQ ${gsmCsq}` : ""}`);
      setTone("rowCellular", signal === "Weak" ? "warn" : signal ? "ok" : "neutral");
    }

    // Modem
    if (modemOk == null) {
      setText("statModem", "—");
      setTone("rowModem", "neutral");
    } else {
      setText("statModem", modemOk ? "Responding" : "Not responding");
      setTone("rowModem", modemOk ? "ok" : "danger");
    }

    this.renderDiagnostics(state);
  }

  // Called every second; cheap (one text node).
  updateLastSeen(timestamp, nowMs) {
    setText("statUpdated", timestamp ? `${formatAge(nowMs - timestamp)} · ${formatTime(timestamp)}` : "—");
  }

  setNotice(message, tone = "info") {
    const el = $("feedNotice");
    if (!el) return;
    if (el.textContent !== (message || "")) el.textContent = message || "";
    el.hidden = !message;
    if (el.dataset.tone !== tone) el.dataset.tone = tone;
  }

  renderDiagnostics({ fw, uptimeSec, heartbeatSec, simStatus, diag }) {
    const rows = [
      ["Firmware", fw || "—"],
      ["Uptime", formatDuration(uptimeSec)],
      ["Heartbeat", heartbeatSec ? `every ${heartbeatSec} s` : "—"],
      ["SIM (modem reply)", simStatus || "—"]
    ];
    if (diag) {
      rows.push(
        ["Last reset", String(diag.resetReason || "—").replace(/_/g, " ")],
        ["Boot count", diag.bootCount ?? "—"],
        ["Free memory", diag.heapFree != null ? `${Math.round(diag.heapFree / 1024)} KB (min ${Math.round((diag.heapMin || 0) / 1024)} KB)` : "—"],
        ["Uploads", `${diag.uploadsOk ?? 0} ok · ${diag.uploadsFailed ?? 0} failed`],
        ["Wi-Fi reconnects", diag.wifiReconnects ?? "—"],
        ["Clock", diag.timeSynced ? "Synchronized" : "Not synchronized"]
      );
    }

    const key = JSON.stringify(rows);
    if (key === this.diagKey) return;
    this.diagKey = key;

    const list = $("diagList");
    if (!list) return;
    const frag = document.createDocumentFragment();
    rows.forEach(([label, value]) => {
      const dt = document.createElement("dt");
      dt.textContent = label;
      const dd = document.createElement("dd");
      dd.textContent = String(value);
      frag.append(dt, dd);
    });
    list.replaceChildren(frag);
  }

  renderHistoryList(points) {
    const list = $("historyList");
    if (!list) return;

    const recent = [...(points || [])].slice(-12).reverse();
    const key = recent.map((p) => p.key ?? p.timestamp).join("|");
    if (key === this.historyKey && list.childElementCount > 0) return;
    this.historyKey = key;

    setText("historyCount", points?.length ? `${points.length} stored` : "");

    if (recent.length === 0) {
      const li = document.createElement("li");
      li.className = "empty";
      li.textContent = "No locations yet. They appear once the device has a GPS fix.";
      list.replaceChildren(li);
      return;
    }

    const frag = document.createDocumentFragment();
    recent.forEach((p) => {
      const li = document.createElement("li");
      const time = document.createElement("span");
      time.className = "h-time";
      time.textContent = formatTime(p.timestamp);
      const speed = document.createElement("span");
      speed.className = "h-speed";
      speed.textContent = p.speed != null ? `${Number(p.speed).toFixed(0)} km/h` : "";
      const coords = document.createElement("span");
      coords.className = "h-coords";
      coords.textContent = `${p.lat.toFixed(5)}, ${p.lng.toFixed(5)}`;
      li.append(time, speed, coords);
      frag.appendChild(li);
    });
    list.replaceChildren(frag);
  }
}
