/* =========================================================
   APP CONFIG — Guardian Beacon Frontend Configuration
   ========================================================= */

export const APP_CONFIG = {
  // Toggle between Mock Demo Simulation (false) and Real ESP32 Device (true)
  USE_LIVE_DATA: true,

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
      apiKey: "",
      authDomain: "",
      projectId: "safety-device-77b03",
      storageBucket: "",
      messagingSenderId: "",
      appId: ""
    },
    parentLocationPath: "guardians",
    // Timeout to mark device as Offline if no ping received (ms)
    offlineTimeoutMs: 45000
  },

  // Map Default Settings — Real Google Maps Tile Provider
  map: {
    defaultCenter: [28.61621, 77.20796], // New Delhi demo coordinates
    defaultZoom: 15.5,
    defaultTheme: "dark",
    tileUrlDark: "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    tileUrlLight: "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    tileUrlSatellite: "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    tileAttribution: '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener">OpenStreetMap</a>',
    subdomains: ["a", "b", "c"],
    maxHistoryPoints: 50,
    historyDotStride: 1
  },

  // Mock location simulation interval (ms)
  MOCK_PING_INTERVAL_MS: 3000
};

// Optional ignored per-developer overrides; do not commit private phone numbers.
try {
  const localConfig = await import("./config.local.js");
  if (localConfig.default?.device) {
    Object.assign(APP_CONFIG.device, localConfig.default.device);
  }
} catch {
  // The local override is intentionally absent from a clean checkout.
}
