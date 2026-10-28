/* =========================================================
   APP CONFIG — Guardian Beacon Frontend Configuration
   ========================================================= */

export const APP_CONFIG = {
  // Real device via Firebase (true) or in-browser simulation (false).
  // Add ?demo to the page URL to preview the simulation without editing this.
  USE_LIVE_DATA: true,

  // Load js/config.local.js (git-ignored) for private values such as the
  // device phone number. Set false when that file does not exist to avoid a
  // failed request on every page load.
  LOAD_LOCAL_CONFIG: true,

  // Device Definition
  device: {
    id: "beacon-01",
    name: "Guardian Beacon",
    phone: ""
  },

  // Cloud Backend Configuration (Firebase Realtime DB / REST)
  backend: {
    firebase: {
      databaseURL: "https://safety-device-77b03-default-rtdb.asia-southeast1.firebasedatabase.app",
      apiKey: "AIzaSyAIVLPpW6aDHMYEMh7Crwy16RFHuAfrfN8",
      authDomain: "safety-device-77b03.firebaseapp.com",
      projectId: "safety-device-77b03",
      storageBucket: "safety-device-77b03.firebasestorage.app",
      messagingSenderId: "1064875268534",
      appId: "1:1064875268534:web:ceb72c9d5016d3f424c917"
    },
    parentLocationPath: "guardians",
    // Freshness of the device heartbeat (firmware 1.2 sends one every 5 s).
    // Stale: a heartbeat is late. Offline: about three were missed.
    // Both scale up automatically for firmware with a slower heartbeat.
    staleAfterMs: 9000,
    offlineTimeoutMs: 15000
  },

  map: {
    defaultCenter: [28.61621, 77.20796],
    defaultZoom: 16,
    // Initial theme when the visitor has no saved choice: "system", "light" or "dark"
    defaultTheme: "system",
    // OpenStreetMap tiles (no API key; follow the OSM tile usage policy and
    // keep the attribution visible). The dark theme is produced by a CSS
    // filter on the tile layer (see .leaflet-tile-pane in style.css).
    tileUrlLight: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    tileUrlDark: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    tileAttribution: '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener">OpenStreetMap</a> contributors',
    subdomains: [],
    maxHistoryPoints: 50
  },

  // Mock location simulation interval (ms)
  MOCK_PING_INTERVAL_MS: 3000
};

if (typeof window !== "undefined" && new URLSearchParams(window.location.search).has("demo")) {
  APP_CONFIG.USE_LIVE_DATA = false;
}

// Optional ignored per-developer overrides; do not commit private phone numbers.
if (APP_CONFIG.LOAD_LOCAL_CONFIG) {
  try {
    const localConfig = await import("./config.local.js");
    if (localConfig.default?.device) {
      Object.assign(APP_CONFIG.device, localConfig.default.device);
    }
  } catch {
    // The local override is intentionally absent from a clean checkout.
  }
}
