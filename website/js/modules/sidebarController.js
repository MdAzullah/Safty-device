/* =========================================================
   SIDEBAR CONTROLLER
   Renders Live Status Card, Metrics, and Recent Activity Feed.
   ========================================================= */

const $ = (id) => document.getElementById(id);

function formatTime(ts) {
  if (!ts) return "—";
  return new Date(ts).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

export class SidebarController {
  updateStatus({ name, lat, lng, timestamp, battery, speed, signal, status, network }) {
    if ($("statChild")) $("statChild").textContent = name || "Guardian Beacon";
    if ($("statUpdated")) $("statUpdated").textContent = formatTime(timestamp);
    if ($("statCoords")) $("statCoords").textContent = (lat != null && lng != null) ? `${lat.toFixed(5)}, ${lng.toFixed(5)}` : "—";
    if ($("statBattery")) $("statBattery").textContent = battery != null ? `${battery}%` : "—";
    if ($("statSpeed")) $("statSpeed").textContent = speed != null ? `${speed} km/h` : "—";
    if ($("statNetwork")) $("statNetwork").textContent = network || "Cellular (A9G)";
    if ($("statSignal")) $("statSignal").textContent = signal ?? "—";

    const pill = $("deviceOnlinePill");
    if (pill) {
      const isOnline = status === "Online" || status === true;
      pill.textContent = isOnline ? "Online" : "Offline";
      pill.className = "pill " + (isOnline ? "online" : "offline");
    }
  }

  renderHistoryList(points) {
    const el = $("historyList");
    if (!el) return;

    el.innerHTML = "";
    const recent = [...(points || [])].slice(-10).reverse();

    if (recent.length === 0) {
      el.innerHTML = `<li style="color:var(--ink-faint)">No recent locations.</li>`;
      return;
    }

    const frag = document.createDocumentFragment();
    recent.forEach((p) => {
      const li = document.createElement("li");
      li.innerHTML = `
        <span class="h-dot"></span>
        <span class="h-time">${formatTime(p.timestamp)}</span>
        <span class="h-coords">${p.lat.toFixed(4)}, ${p.lng.toFixed(4)}</span>
      `;
      frag.appendChild(li);
    });
    el.appendChild(frag);
  }
}
