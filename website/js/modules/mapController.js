/* =========================================================
   MAP CONTROLLER
   Leaflet map with themed CARTO basemaps, the device marker
   (live / stale / SOS), the guardian's own marker, and the recent
   path. History is updated incrementally: only new points are added.
   ========================================================= */

import { APP_CONFIG } from "../config.js";

const cssVar = (name) =>
  getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  })[c]);
}

export class MapController {
  constructor(elementId) {
    const mapElement = document.getElementById(elementId);

    if (!window.L) {
      if (mapElement) {
        mapElement.innerHTML =
          '<p class="map-error">The map could not load. Check your internet connection and refresh.</p>';
      }
      throw new Error("Leaflet was not loaded; cannot initialize map.");
    }

    this.map = L.map(elementId, {
      zoomControl: false,
      worldCopyJump: true,
      minZoom: 3,
      maxZoom: 20,
      zoomSnap: 0.5,
      zoomDelta: 0.5,
      wheelPxPerZoomLevel: 90
    }).setView(APP_CONFIG.map.defaultCenter, APP_CONFIG.map.defaultZoom);

    L.control.zoom({ position: "bottomright" }).addTo(this.map);
    L.control.scale({ metric: true, imperial: false, position: "bottomright" }).addTo(this.map);

    this.theme = document.documentElement.getAttribute("data-theme") || "light";
    this.createTileLayer();

    this.liveMarker = L.marker(APP_CONFIG.map.defaultCenter, {
      icon: L.divIcon({
        className: "dev-marker-wrap",
        html: '<div class="dev-marker"><span class="dev-pulse"></span><span class="dev-dot"></span></div>',
        iconSize: [22, 22],
        iconAnchor: [11, 11]
      }),
      zIndexOffset: 1000,
      keyboard: false
    });
    this.liveMarker.bindTooltip("Device", { direction: "top", offset: [0, -12] });
    this.hasLiveLocation = false;
    this.liveKey = "";

    this.parentMarker = null;

    this.trailLine = L.polyline([], {
      color: cssVar("--trail"),
      weight: 3,
      opacity: 0.8,
      lineCap: "round",
      lineJoin: "round",
      interactive: false
    }).addTo(this.map);

    this.historyLayer = L.layerGroup().addTo(this.map);
    this.historyDots = new Map();

    if (window.ResizeObserver && mapElement) {
      new ResizeObserver(() => this.map.invalidateSize()).observe(mapElement);
    }
  }

  createTileLayer() {
    if (this.tileLayer) this.map.removeLayer(this.tileLayer);

    const url = this.theme === "dark" ? APP_CONFIG.map.tileUrlDark : APP_CONFIG.map.tileUrlLight;
    this.tileLayer = L.tileLayer(url, {
      attribution: APP_CONFIG.map.tileAttribution,
      subdomains: APP_CONFIG.map.subdomains?.length ? APP_CONFIG.map.subdomains : "abc",
      maxZoom: 20,
      maxNativeZoom: 19,
      detectRetina: false
    }).addTo(this.map);
    this.tileLayer.bringToBack();
  }

  setTheme(theme) {
    if (theme === this.theme) return;
    this.theme = theme;
    this.createTileLayer();
    const trail = cssVar("--trail");
    this.trailLine.setStyle({ color: trail });
    this.historyDots.forEach((dot) => dot.setStyle({ color: trail, fillColor: cssVar("--surface") }));
  }

  isValidLocation(lat, lng) {
    return lat != null && lng != null &&
      Number.isFinite(Number(lat)) && Number.isFinite(Number(lng)) &&
      Number(lat) >= -90 && Number(lat) <= 90 &&
      Number(lng) >= -180 && Number(lng) <= 180;
  }

  // state: "live" | "stale" | "sos". Does nothing when nothing changed.
  setLive(lat, lng, { pan = false, state = "live" } = {}) {
    if (!this.isValidLocation(lat, lng)) return;

    const key = `${lat},${lng},${state}`;
    if (key === this.liveKey && !pan) return;
    this.liveKey = key;

    const location = L.latLng(Number(lat), Number(lng));
    this.liveMarker.setLatLng(location);

    if (!this.hasLiveLocation) {
      this.hasLiveLocation = true;
      this.liveMarker.addTo(this.map);
      // First real position: bring the device into view.
      this.map.setView(location, APP_CONFIG.map.defaultZoom || 16, { animate: false });
    } else if (pan) {
      this.map.panTo(location, { animate: true, duration: 0.6 });
    }

    const el = this.liveMarker.getElement();
    if (el) {
      el.classList.toggle("is-stale", state === "stale");
      el.classList.toggle("is-sos", state === "sos");
    }
    this.liveMarker.setTooltipContent(
      state === "sos" ? "Device · SOS" : state === "stale" ? "Device · last known location" : "Device"
    );
  }

  clearLive() {
    if (!this.hasLiveLocation) return;
    this.hasLiveLocation = false;
    this.liveKey = "";
    this.liveMarker.remove();
  }

  setParentLocation(lat, lng) {
    if (!this.isValidLocation(lat, lng)) return false;
    const location = [Number(lat), Number(lng)];
    if (!this.parentMarker) {
      this.parentMarker = L.marker(location, {
        icon: L.divIcon({
          className: "you-marker-wrap",
          html: '<div class="you-marker"></div>',
          iconSize: [16, 16],
          iconAnchor: [8, 8]
        }),
        zIndexOffset: 1100,
        keyboard: false
      })
        .bindTooltip("You", { direction: "top", offset: [0, -10] })
        .addTo(this.map);
    } else {
      this.parentMarker.setLatLng(location);
    }
    return true;
  }

  fitChildAndParent() {
    if (!this.parentMarker) return;
    if (!this.hasLiveLocation) {
      this.map.setView(this.parentMarker.getLatLng(), APP_CONFIG.map.defaultZoom || 16);
      return;
    }
    this.map.fitBounds(
      L.latLngBounds([this.liveMarker.getLatLng(), this.parentMarker.getLatLng()]),
      { padding: [60, 60], maxZoom: APP_CONFIG.map.defaultZoom || 16 }
    );
  }

  // Adds dots for new points and removes dots that left the window.
  setHistory(points) {
    const valid = (points || []).filter((p) => this.isValidLocation(p?.lat, p?.lng));
    this.trailLine.setLatLngs(valid.map((p) => [p.lat, p.lng]));

    const keep = new Set();
    const trail = cssVar("--trail");
    const fill = cssVar("--surface");

    valid.forEach((p) => {
      const key = String(p.key ?? p.timestamp ?? `${p.lat},${p.lng}`);
      keep.add(key);
      if (this.historyDots.has(key)) return;

      const time = p.timestamp
        ? new Date(p.timestamp).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })
        : "Unknown time";
      const dot = L.circleMarker([p.lat, p.lng], {
        radius: 3.5,
        weight: 2,
        color: trail,
        fillColor: fill,
        fillOpacity: 1
      }).bindTooltip(
        `<strong>${escapeHtml(time)}</strong><br>${p.lat.toFixed(5)}, ${p.lng.toFixed(5)}`,
        { direction: "top", offset: [0, -4] }
      );
      dot.addTo(this.historyLayer);
      this.historyDots.set(key, dot);
    });

    this.historyDots.forEach((dot, key) => {
      if (!keep.has(key)) {
        this.historyLayer.removeLayer(dot);
        this.historyDots.delete(key);
      }
    });
  }

  centerOnLive() {
    if (!this.hasLiveLocation) return false;
    this.map.setView(this.liveMarker.getLatLng(), Math.max(this.map.getZoom(), APP_CONFIG.map.defaultZoom || 16), {
      animate: true
    });
    return true;
  }
}
