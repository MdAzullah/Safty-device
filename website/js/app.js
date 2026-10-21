/* =========================================================
   GUARDIAN BEACON — Application Entrypoint
   ========================================================= */

import { APP_CONFIG } from "./config.js";
import { demoDataService } from "./demoData.js";
import { liveDataService } from "./services/liveDataService.js";
import { MapController } from "./modules/mapController.js";
import { SidebarController } from "./modules/sidebarController.js";
import { SosController } from "./modules/sosController.js";

const $ = (id) => document.getElementById(id);
const STORAGE_THEME_KEY = "guardian_beacon_theme";

let mapController;
let sidebarController;
let sosController;
let activeDataService;

async function initDashboard() {
  // 1. Initialize Controllers
  mapController = new MapController("map");
  sidebarController = new SidebarController();
  sosController = new SosController();

  initializeFirebaseAuth();

  // 2. Initialize Theme
  initTheme();

  // 3. Bind UI Interaction Handlers
  bindEventHandlers();

  // 4. Select Data Provider (Live Cloud Device vs. Demo Simulator)
  activeDataService = APP_CONFIG.USE_LIVE_DATA ? liveDataService : demoDataService;

  activeDataService.onTelemetry((telemetry) => {
    if (telemetry.lat != null && telemetry.lng != null) {
      mapController.setLive(telemetry.lat, telemetry.lng);
    }
    sidebarController.updateStatus(telemetry);
    updateConnectionStatus(telemetry.status);
  });

  activeDataService.onHistory((historyPoints) => {
    mapController.setHistory(historyPoints);
    sidebarController.renderHistoryList(historyPoints);
  });

  activeDataService.onParentLocation?.((location) => {
    if (mapController.setParentLocation(location.lat, location.lng)) {
      const status = $("parentLocationStatus");
      if (status) status.textContent = "Guardian location synced from Firebase";
      mapController.fitChildAndParent();
    }
  });

  activeDataService.onSOS((sosEvent) => {
    sosController.show(sosEvent.deviceName, sosEvent.timestamp);
    if (sosEvent.lat != null && sosEvent.lng != null) {
      mapController.setLive(sosEvent.lat, sosEvent.lng, { pan: true });
    }
  });

  // 5. Start Service Feed
  if (APP_CONFIG.USE_LIVE_DATA) {
    liveDataService.start();
  } else {
    demoDataService.startSimulation();
  }
}

function updateConnectionStatus(status) {
  const pill = $("connPill");
  const text = $("connText");
  if (!pill || !text) return;

  const isOnline = status === "Online" || status === true;
  if (isOnline) {
    pill.className = "conn-pill connected";
    text.textContent = APP_CONFIG.USE_LIVE_DATA ? "Hardware Live" : "Beacon Active";
  } else if (status === "Connecting...") {
    pill.className = "conn-pill connecting";
    text.textContent = "Connecting...";
  } else {
    pill.className = "conn-pill disconnected";
    text.textContent = "Beacon Offline";
  }
}

function bindEventHandlers() {
  // Center Map Buttons
  $("btnCenter")?.addEventListener("click", () => mapController.centerOnLive());
  $("btnCenterMapTop")?.addEventListener("click", () => mapController.centerOnLive());

  $("btnParentLocation")?.addEventListener("click", () => {
    const status = $("parentLocationStatus");
    if (!navigator.geolocation) {
      if (status) status.textContent = "Geolocation is not supported by this browser";
      return;
    }
    if (status) status.textContent = "Waiting for location permission…";
    navigator.geolocation.getCurrentPosition(
      ({ coords }) => {
        if (mapController.setParentLocation(coords.latitude, coords.longitude)) {
          status.textContent = "Your location is shown on this map";
          mapController.fitChildAndParent();
          if (APP_CONFIG.USE_LIVE_DATA) {
            liveDataService.publishParentLocation(coords.latitude, coords.longitude).then((saved) => {
              if (saved && status) status.textContent = "Your location is shared with this guardian account";
              else if (status) status.textContent = "Shown on this device only; sign in to sync";
            });
          }
        } else if (status) {
          status.textContent = "Browser returned invalid coordinates";
        }
      },
      (error) => {
        if (!status) return;
        status.textContent = error.code === error.PERMISSION_DENIED
          ? "Location permission denied"
          : error.code === error.TIMEOUT
            ? "Location request timed out; try again"
            : "Unable to get your location";
      },
      { enableHighAccuracy: true, timeout: 15000, maximumAge: 10000 }
    );
  });

  // Call Device Action
  $("btnCall")?.addEventListener("click", () => {
    const phone = APP_CONFIG.device.phone;
    if (phone) {
      window.location.href = `tel:${phone.replace(/\s+/g, "")}`;
    }
  });

  // Share Live Location Action
  $("btnShare")?.addEventListener("click", async () => {
    const state = activeDataService.getDeviceState();
    const lat = state.lat ?? APP_CONFIG.map.defaultCenter[0];
    const lng = state.lng ?? APP_CONFIG.map.defaultCenter[1];
    const shareText = `Guardian Beacon Location: https://www.google.com/maps?q=${lat},${lng} (${state.name} at ${new Date(state.timestamp || Date.now()).toLocaleTimeString()})`;

    if (navigator.share) {
      try {
        await navigator.share({
          title: "Guardian Beacon Location",
          text: shareText,
          url: `https://www.google.com/maps?q=${lat},${lng}`
        });
      } catch {
        // User cancelled or share dismissed
      }
    } else {
      try {
        await navigator.clipboard.writeText(shareText);
        showToast("Location copied to clipboard!");
      } catch {
        alert(shareText);
      }
    }
  });

  // SOS Acknowledge handler (also syncs with cloud database)
  $("sosAck")?.addEventListener("click", () => {
    if (APP_CONFIG.USE_LIVE_DATA) {
      liveDataService.acknowledgeSOS();
    }
  });

  $("guardianSignIn")?.addEventListener("click", async () => {
    const email = $("guardianEmail")?.value.trim();
    const password = $("guardianPassword")?.value;
    const status = $("guardianAuthStatus");
    if (!email || !password) {
      if (status) status.textContent = "Enter email and password";
      return;
    }
    if (!window.firebase?.apps?.length || !APP_CONFIG.backend.firebase.apiKey) {
      if (status) status.textContent = "Set Firebase Web App config first";
      return;
    }
    try {
      await firebase.auth().signInWithEmailAndPassword(email, password);
      $("guardianPassword").value = "";
    } catch (error) {
      if (status) status.textContent = error.code === "auth/invalid-credential" || error.code === "auth/wrong-password"
        ? "Email or password is incorrect"
        : error.message;
    }
  });

  $("guardianSignOut")?.addEventListener("click", async () => {
    try {
      await firebase.auth().signOut();
    } catch (error) {
      const status = $("guardianAuthStatus");
      if (status) status.textContent = error.message;
    }
  });

  // Simulate SOS Action
  $("btnTestSos")?.addEventListener("click", () => {
    if (APP_CONFIG.USE_LIVE_DATA) {
      liveDataService.processSOS({
        active: true,
        deviceName: APP_CONFIG.device.name,
        timestamp: Date.now(),
        lat: activeDataService.getDeviceState().lat,
        lng: activeDataService.getDeviceState().lng
      });
    } else {
      demoDataService.triggerSimulatedSOS();
    }
  });
}

function initializeFirebaseAuth() {
  const config = APP_CONFIG.backend?.firebase;
  if (!window.firebase || !config?.apiKey) return;
  if (!firebase.apps.length) firebase.initializeApp(config);
  firebase.auth().onAuthStateChanged((user) => {
    const status = $("guardianAuthStatus");
    const signIn = $("guardianSignIn");
    const signOut = $("guardianSignOut");
    if (status) status.textContent = user ? `Signed in: ${user.email}` : "Not signed in";
    if (signIn) signIn.hidden = Boolean(user);
    if (signOut) signOut.hidden = !user;
  });
}

function initTheme() {
  const savedTheme = localStorage.getItem(STORAGE_THEME_KEY) || APP_CONFIG.map.defaultTheme || "dark";
  applyTheme(savedTheme);

  $("themeToggleBtn")?.addEventListener("click", () => {
    const currentTheme = document.documentElement.getAttribute("data-theme") || "dark";
    const nextTheme = currentTheme === "dark" ? "light" : "dark";
    applyTheme(nextTheme);
    localStorage.setItem(STORAGE_THEME_KEY, nextTheme);
  });
}

function applyTheme(theme) {
  document.documentElement.setAttribute("data-theme", theme);
  const toggleBtn = $("themeToggleBtn");
  if (toggleBtn) {
    toggleBtn.textContent = theme === "light" ? "☀️" : "🌙";
    toggleBtn.setAttribute("title", theme === "light" ? "Switch to dark theme" : "Switch to light theme");
  }
  if (mapController) {
    mapController.setTheme(theme);
  }
}

function showToast(message) {
  const existing = document.querySelector(".beacon-toast");
  if (existing) existing.remove();

  const toast = document.createElement("div");
  toast.className = "beacon-toast";
  toast.textContent = message;
  document.body.appendChild(toast);

  setTimeout(() => {
    toast.classList.add("fade-out");
    setTimeout(() => toast.remove(), 300);
  }, 2500);
}

// Boot on DOM ready
if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", initDashboard);
} else {
  initDashboard();
}

