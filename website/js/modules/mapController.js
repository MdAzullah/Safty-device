/* =========================================================
   MAP CONTROLLER — Real Google Maps Engine
   Integrates authentic Google Maps raster vector layers,
   Google teardrop beacon with concentric radar waves, glowing trail,
   and smooth zoom/pan physics.
   ========================================================= */

import { APP_CONFIG } from "../config.js";

// Google-style Live Location Beacon with Dual Concentric Radar Waves
const googleBeaconIconHtml = `
  <div class="google-live-beacon">
    <div class="g-radar-wave-1"></div>
    <div class="g-radar-wave-2"></div>
    <div class="g-marker-core"></div>
  </div>`;

export class MapController {
  constructor(elementId) {
    if (!window.L) {
      const mapElement = document.getElementById(elementId);
      if (mapElement) {
        mapElement.textContent = "Map library did not load. Check your internet connection or CDN access, then refresh.";
        mapElement.style.display = "grid";
        mapElement.style.placeItems = "center";
        mapElement.style.color = "#e7ecf6";
        mapElement.style.padding = "24px";
      }
      throw new Error("Leaflet was not loaded; cannot initialize map.");
    }
    this.currentTheme = APP_CONFIG.map.defaultTheme || "dark";
    
    // Initialize Leaflet map with smooth inertia
    this.map = L.map(elementId, {
      zoomControl: true,
      worldCopyJump: true,
      preferCanvas: true,
      minZoom: 2,
      maxZoom: 20,
      zoomDelta: 0.5,
      zoomSnap: 0.5,
      wheelPxPerZoomLevel: 80,
      zoomAnimation: true,
      fadeAnimation: true
    }).setView(APP_CONFIG.map.defaultCenter, APP_CONFIG.map.defaultZoom);

    // Add scale bar
    L.control.scale({
      metric: true,
      imperial: false,
      position: "bottomleft"
    }).addTo(this.map);

    // Tile Layer
    this.createTileLayer(this.currentTheme);

    // Live Google Beacon Marker
    this.liveIcon = L.divIcon({
      className: "google-beacon-wrapper",
      html: googleBeaconIconHtml,
      iconSize: [44, 44],
      iconAnchor: [22, 22]
    });

    this.liveMarker = L.marker(APP_CONFIG.map.defaultCenter, {
      icon: this.liveIcon,
      zIndexOffset: 1000
    }).addTo(this.map);
    this.liveMarker.bindTooltip("Child device", { direction: "top", offset: [0, -20] });

    this.parentIcon = L.divIcon({
      className: "parent-location-wrapper",
      html: '<div class="parent-location-marker"><span></span></div>',
      iconSize: [28, 28],
      iconAnchor: [14, 14]
    });
    this.parentMarker = null;

    // Glowing Trail Lines (Dual-layered for high-end navigation trail effect)
    this.trailGlowLine = L.polyline([], {
      color: "#4285F4",
      weight: 7,
      opacity: 0.35,
      lineCap: "round",
      lineJoin: "round"
    }).addTo(this.map);

    this.trailLine = L.polyline([], {
      color: this.getTrailColor(),
      weight: 3.5,
      opacity: 0.95,
      lineCap: "round",
      lineJoin: "round"
    }).addTo(this.map);

    this.historyDotsLayer = L.layerGroup().addTo(this.map);

    // Dynamic resize observer to guarantee 100% full-canvas coverage
    const mapElement = document.getElementById(elementId);
    if (window.ResizeObserver && mapElement) {
      const resizeObserver = new ResizeObserver(() => {
        this.map.invalidateSize();
      });
      resizeObserver.observe(mapElement);
    }

    setTimeout(() => {
      this.map.invalidateSize();
    }, 150);

    window.addEventListener("resize", () => {
      this.map.invalidateSize();
    });
  }

  createTileLayer(theme) {
    if (this.tileLayer) {
      this.map.removeLayer(this.tileLayer);
    }
    
    // Real Google Maps Streets Tiles
    const url = theme === "light"
      ? (APP_CONFIG.map.tileUrlLight || APP_CONFIG.map.tileUrlDark)
      : APP_CONFIG.map.tileUrlDark;
    this.tileLayer = L.tileLayer(url, {
      attribution: APP_CONFIG.map.tileAttribution,
      subdomains: APP_CONFIG.map.subdomains || ["0", "1", "2", "3"],
      maxZoom: 20
    }).addTo(this.map);

    this.currentTheme = theme;
  }

  getTrailColor() {
    return this.currentTheme === "light" ? "#1A73E8" : "#4285F4";
  }

  setTheme(theme) {
    this.currentTheme = theme;
    this.createTileLayer(theme);
    this.trailLine.setStyle({ color: this.getTrailColor() });
    this.trailGlowLine.setStyle({ color: theme === "light" ? "#1A73E8" : "#4285F4" });

    this.historyDotsLayer.eachLayer((layer) => {
      if (layer.setStyle) {
        layer.setStyle({
          color: theme === "light" ? "#1A73E8" : "#8AB4F8",
          fillColor: theme === "light" ? "#1A73E8" : "#4285F4"
        });
      }
    });
  }

  setLive(lat, lng, { pan = false } = {}) {
    if (!this.isValidLocation(lat, lng)) return;
    const location = L.latLng(lat, lng);
    this.liveMarker.setLatLng(location);
    if (pan) {
      this.map.panTo(location, { animate: true, duration: 0.8 });
    }
  }

  isValidLocation(lat, lng) {
    return lat != null && lng != null &&
      Number.isFinite(Number(lat)) && Number.isFinite(Number(lng)) &&
      Number(lat) >= -90 && Number(lat) <= 90 &&
      Number(lng) >= -180 && Number(lng) <= 180;
  }

  setParentLocation(lat, lng, { pan = false } = {}) {
    if (!this.isValidLocation(lat, lng)) return false;
    const location = [Number(lat), Number(lng)];
    if (!this.parentMarker) {
      this.parentMarker = L.marker(location, { icon: this.parentIcon, zIndexOffset: 1100 })
        .bindTooltip("Your location", { direction: "top", offset: [0, -12] })
        .addTo(this.map);
    } else {
      this.parentMarker.setLatLng(location);
    }
    if (pan) this.map.panTo(location, { animate: true, duration: 0.8 });
    return true;
  }

  fitChildAndParent() {
    if (this.parentMarker && this.liveMarker) {
      this.map.fitBounds(L.latLngBounds([
        this.liveMarker.getLatLng(),
        this.parentMarker.getLatLng()
      ]), { padding: [40, 40], maxZoom: APP_CONFIG.map.defaultZoom || 16 });
    }
  }

  setHistory(points) {
    const validPoints = (points || []).filter((p) => this.isValidLocation(p?.lat, p?.lng));
    if (validPoints.length === 0) {
      this.trailLine.setLatLngs([]);
      this.trailGlowLine.setLatLngs([]);
      this.historyDotsLayer.clearLayers();
      return;
    }
    const latlngs = validPoints.map((p) => [Number(p.lat), Number(p.lng)]);
    this.trailLine.setLatLngs(latlngs);
    this.trailGlowLine.setLatLngs(latlngs);

    this.historyDotsLayer.clearLayers();
    const stride = APP_CONFIG.map.historyDotStride || 1;

    validPoints.forEach((p, i) => {
      if (i % stride !== 0 && i !== validPoints.length - 1) return;
      
      const timeStr = new Date(p.timestamp).toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit"
      });

      L.circleMarker([p.lat, p.lng], {
        radius: 4,
        weight: 1.5,
        color: this.currentTheme === "light" ? "#1A73E8" : "#8AB4F8",
        fillColor: this.currentTheme === "light" ? "#1A73E8" : "#4285F4",
        fillOpacity: 0.95
      })
        .bindTooltip(
          `<strong>${timeStr}</strong><br/>${p.lat.toFixed(4)}, ${p.lng.toFixed(4)}`,
          { direction: "top", offset: [0, -4] }
        )
        .addTo(this.historyDotsLayer);
    });
  }

  centerOnLive() {
    const latLng = this.liveMarker.getLatLng();
    if (latLng) {
      this.map.setView(latLng, APP_CONFIG.map.defaultZoom || 16, {
        animate: true,
        duration: 0.75
      });
    }
  }
}
