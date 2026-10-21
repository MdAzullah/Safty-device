/* =========================================================
   DEMO DATA LAYER
   Provides static/simulated safety device telemetry for the
   frontend dashboard prototype.

   Architecture Note:
   In future hardware phases, this file can be seamlessly
   replaced by `deviceDataService.js` without altering the UI logic.
   ========================================================= */

import { APP_CONFIG } from "./config.js";

// Initial Demo Device State
export const demoDevice = {
  id: APP_CONFIG.device.id,
  name: APP_CONFIG.device.name,
  phone: APP_CONFIG.device.phone,
  status: "Online",
  battery: 54,
  speed: 9.9,
  signal: "Good",
  lat: 28.61621,
  lng: 77.20796,
  timestamp: Date.now()
};

// Initial Demo History Points (Past breadcrumb trail)
const demoHistoryPoints = [
  { lat: 28.61340, lng: 77.20540, timestamp: Date.now() - 15 * 60 * 1000, battery: 58, speed: 12.4, signal: "Good" },
  { lat: 28.61420, lng: 77.20610, timestamp: Date.now() - 12 * 60 * 1000, battery: 57, speed: 10.1, signal: "Good" },
  { lat: 28.61490, lng: 77.20680, timestamp: Date.now() - 9 * 60 * 1000, battery: 56, speed: 8.5, signal: "Good" },
  { lat: 28.61560, lng: 77.20730, timestamp: Date.now() - 6 * 60 * 1000, battery: 55, speed: 9.2, signal: "Weak" },
  { lat: 28.61600, lng: 77.20770, timestamp: Date.now() - 3 * 60 * 1000, battery: 54, speed: 9.8, signal: "Good" },
  { lat: 28.61621, lng: 77.20796, timestamp: Date.now(), battery: 54, speed: 9.9, signal: "Good" }
];

let listeners = {
  telemetry: [],
  history: [],
  sos: []
};

let simTimer = null;

export const demoDataService = {
  getDeviceState() {
    return { ...demoDevice };
  },

  getHistory() {
    return [...demoHistoryPoints];
  },

  onTelemetry(callback) {
    listeners.telemetry.push(callback);
    callback({ ...demoDevice });
  },

  onHistory(callback) {
    listeners.history.push(callback);
    callback([...demoHistoryPoints]);
  },

  onSOS(callback) {
    listeners.sos.push(callback);
  },

  startSimulation() {
    if (simTimer) return;
    simTimer = setInterval(() => {
      // Subtle realistic micro-movement for dynamic live dashboard demo
      const latDelta = (Math.random() - 0.48) * 0.0003;
      const lngDelta = (Math.random() - 0.48) * 0.0003;

      demoDevice.lat = +(demoDevice.lat + latDelta).toFixed(5);
      demoDevice.lng = +(demoDevice.lng + lngDelta).toFixed(5);
      demoDevice.timestamp = Date.now();
      demoDevice.speed = +(Math.random() * 8 + 4).toFixed(1);
      demoDevice.signal = Math.random() > 0.1 ? "Good" : "Weak";

      const newPoint = {
        lat: demoDevice.lat,
        lng: demoDevice.lng,
        timestamp: demoDevice.timestamp,
        battery: demoDevice.battery,
        speed: demoDevice.speed,
        signal: demoDevice.signal
      };

      demoHistoryPoints.push(newPoint);
      if (demoHistoryPoints.length > APP_CONFIG.map.maxHistoryPoints) {
        demoHistoryPoints.shift();
      }

      listeners.telemetry.forEach((cb) => cb({ ...demoDevice }));
      listeners.history.forEach((cb) => cb([...demoHistoryPoints]));
    }, APP_CONFIG.MOCK_PING_INTERVAL_MS);
  },

  stopSimulation() {
    if (simTimer) {
      clearInterval(simTimer);
      simTimer = null;
    }
  },

  triggerSimulatedSOS() {
    const sosEvent = {
      active: true,
      deviceName: demoDevice.name,
      timestamp: Date.now(),
      lat: demoDevice.lat,
      lng: demoDevice.lng
    };
    listeners.sos.forEach((cb) => cb(sosEvent));
  }
};
