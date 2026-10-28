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
let latestTelemetry = {};
// Demo mode has no Firebase feed; treat it as always connected.
let latestConnection = { state: "live", message: "" };
let sosActive = false;

const now = () => activeDataService?.serverNow?.() ?? Date.now();

function initDashboard() {
  sidebarController = new SidebarController();
  sosController = new SosController();

  try {
    mapController = new MapController("map");
  } catch (error) {
    console.error(error);
    mapController = null;
  }

  initTheme();
  initAuthUi();
  bindEventHandlers();

  activeDataService = APP_CONFIG.USE_LIVE_DATA ? liveDataService : demoDataService;

  activeDataService.onTelemetry((telemetry) => {
    latestTelemetry = telemetry;
    updateDeviceMarker();
    sidebarController.updateStatus(telemetry);
    sidebarController.updateLastSeen(telemetry.timestamp, now());
    refreshFeedStatus();
  });

  activeDataService.onConnection?.((connection) => {
    latestConnection = connection;
    refreshFeedStatus();
  });

  activeDataService.onHistory((points) => {
    mapController?.setHistory(points);
    sidebarController.renderHistoryList(points);
  });

  activeDataService.onParentLocation?.((location) => {
    if (mapController?.setParentLocation(location.lat, location.lng)) {
      showMapMessage("Your saved location is shown on the map");
    }
  });

  activeDataService.onSOS((sosEvent) => {
    sosActive = true;
    sosController.show(sosEvent);
    if (sosEvent.lat != null && sosEvent.lng != null) {
      mapController?.setLive(sosEvent.lat, sosEvent.lng, { pan: true, state: "sos" });
    }
  });

  activeDataService.onSOSCleared?.(() => {
    sosActive = false;
    sosController.hide();
    updateDeviceMarker();
  });

  if (APP_CONFIG.USE_LIVE_DATA) {
    liveDataService.start();
  } else {
    demoDataService.startSimulation();
  }

  // "Last seen" text; one text node per second.
  setInterval(() => sidebarController.updateLastSeen(latestTelemetry.timestamp, now()), 1000);
}

function updateDeviceMarker() {
  if (!mapController) return;
  const t = latestTelemetry;
  if (t.lat == null || t.lng == null) {
    if (!t.timestamp) mapController.clearLive();
    return;
  }
  const state = sosActive ? "sos" : t.status === "Online" && t.gpsFix !== false ? "live" : "stale";
  mapController.setLive(t.lat, t.lng, { state });
}

// ---------------------------------------------------------
// Status chip + panel notice
// ---------------------------------------------------------

function refreshFeedStatus() {
  const feed = latestConnection.state;
  const t = latestTelemetry;

  let chip = ["connecting", "Connecting"];
  let notice = null;
  let tone = "info";

  if (feed === "signed-out") {
    chip = ["neutral", "Signed out"];
  } else if (feed === "denied" || feed === "error") {
    chip = ["error", "No access"];
    notice = latestConnection.message;
    tone = "error";
  } else if (feed === "connecting") {
    notice = latestConnection.message;
  } else if (t.status === "Online") {
    chip = ["online", "Live"];
    if (t.gpsFix === false) {
      notice = t.hasLocation
        ? "GPS is searching for satellites. The map shows the last known location."
        : "GPS is searching for satellites. Location appears once the device has a fix (needs open sky).";
    }
  } else if (t.status === "Stale") {
    chip = ["stale", "Delayed"];
    notice = "Updates from the device are delayed. Waiting for the next one…";
    tone = "warn";
  } else if (t.status === "Waiting") {
    notice = latestConnection.message || "Waiting for the first update from the device…";
  } else {
    chip = ["offline", "Offline"];
    notice = "The device is not sending updates. Check its power and Wi-Fi connection.";
    tone = "error";
  }

  const pill = $("connPill");
  if (pill) {
    pill.dataset.state = chip[0];
    $("connText").textContent = chip[1];
  }
  sidebarController.setNotice(notice, tone);
}

// ---------------------------------------------------------
// Authentication UI
// ---------------------------------------------------------

function initAuthUi() {
  const overlay = $("signinOverlay");

  if (!APP_CONFIG.USE_LIVE_DATA) {
    $("accountArea").hidden = true;
    overlay.hidden = true;
    return;
  }

  const config = APP_CONFIG.backend?.firebase;
  if (!window.firebase || !config?.apiKey) {
    overlay.hidden = false;
    setAuthError("The sign-in service could not load. Check your internet connection and refresh.");
    return;
  }

  if (!firebase.apps.length) firebase.initializeApp(config);

  firebase.auth().onAuthStateChanged((user) => {
    $("btnOpenSignIn").hidden = Boolean(user);
    $("accountSigned").hidden = !user;
    $("accountEmail").textContent = user?.email || "";
    overlay.hidden = Boolean(user);
    if (!user) {
      setAuthError("");
      $("guardianEmail")?.focus({ preventScroll: true });
    }
  });
}

function setAuthError(message, invalid = false) {
  const status = $("guardianAuthStatus");
  if (status) status.textContent = message;
  ["guardianEmail", "guardianPassword"].forEach((id) => {
    $(id)?.setAttribute("aria-invalid", invalid ? "true" : "false");
  });
}

async function signIn(event) {
  event?.preventDefault();
  const email = $("guardianEmail").value.trim();
  const password = $("guardianPassword").value;
  const button = $("guardianSignIn");

  if (!email || !password) {
    setAuthError("Enter your email and password.", true);
    return;
  }
  if (!window.firebase?.apps?.length) {
    setAuthError("The sign-in service could not load. Check your internet connection and refresh.");
    return;
  }

  button.disabled = true;
  button.textContent = "Signing in…";
  setAuthError("");
  try {
    await firebase.auth().signInWithEmailAndPassword(email, password);
    $("guardianPassword").value = "";
  } catch (error) {
    console.error("Guardian sign-in failed:", error.code, error.message);
    setAuthError(describeAuthError(error), true);
  } finally {
    button.disabled = false;
    button.textContent = "Sign in";
  }
}

// Firebase hides whether an email exists (email enumeration protection), so
// a missing account and a wrong password produce the same error.
function describeAuthError(error) {
  switch (error?.code) {
    case "auth/invalid-credential":
    case "auth/invalid-login-credentials":
    case "auth/wrong-password":
    case "auth/user-not-found":
      return "Email or password is incorrect.";
    case "auth/invalid-email":
      return "That email address is not valid.";
    case "auth/user-disabled":
      return "This account has been disabled.";
    case "auth/too-many-requests":
      return "Too many attempts. Wait a few minutes and try again.";
    case "auth/operation-not-allowed":
      return "Email sign-in is disabled for this project (Firebase Authentication → Sign-in method).";
    case "auth/network-request-failed":
      return "Network error. Check your internet connection.";
    case "auth/api-key-not-valid.-please-pass-a-valid-api-key.":
    case "auth/invalid-api-key":
      return "The Firebase API key is invalid. Check apiKey in js/config.js.";
    default:
      return `Sign-in failed (${error?.code || error?.message || "unknown error"}).`;
  }
}

// ---------------------------------------------------------
// Actions
// ---------------------------------------------------------

function bindEventHandlers() {
  const centerOnDevice = () => {
    if (!mapController?.centerOnLive()) showToast("No device location received yet");
  };
  $("btnCenter").addEventListener("click", centerOnDevice);
  $("btnCenterMapTop").addEventListener("click", centerOnDevice);
  $("sosLocate").addEventListener("click", centerOnDevice);

  $("signinForm").addEventListener("submit", signIn);
  $("btnOpenSignIn").addEventListener("click", () => {
    $("signinOverlay").hidden = false;
    $("guardianEmail").focus();
  });
  $("guardianSignOut").addEventListener("click", async () => {
    try {
      await firebase.auth().signOut();
    } catch (error) {
      showToast(error.message);
    }
  });

  $("btnParentLocation").addEventListener("click", locateGuardian);

  $("btnCopyCoords").addEventListener("click", async () => {
    const { lat, lng } = activeDataService.getDeviceState();
    if (lat == null) return;
    try {
      await navigator.clipboard.writeText(`${lat.toFixed(6)}, ${lng.toFixed(6)}`);
      showToast("Coordinates copied");
    } catch {
      showToast("Copy is not available in this browser");
    }
  });

  const callBtn = $("btnCall");
  if (!APP_CONFIG.device.phone) {
    callBtn.disabled = true;
    callBtn.title = "Add the device phone number in js/config.local.js";
  }
  callBtn.addEventListener("click", () => {
    const phone = APP_CONFIG.device.phone;
    if (phone) window.location.href = `tel:${phone.replace(/\s+/g, "")}`;
  });

  $("btnShare").addEventListener("click", shareLocation);

  $("sosAck").addEventListener("click", async () => {
    if (!APP_CONFIG.USE_LIVE_DATA) {
      sosActive = false;
      sosController.hide();
      updateDeviceMarker();
      return;
    }
    const button = $("sosAck");
    button.disabled = true;
    const ok = await liveDataService.acknowledgeSOS();
    button.disabled = false;
    if (ok) {
      sosActive = false;
      sosController.hide();
      updateDeviceMarker();
      showToast("SOS acknowledged");
    } else {
      showToast("Could not acknowledge the SOS. Check your connection and sign-in.");
    }
  });

  // Browser-only preview of the alert UI; does not contact the device.
  $("btnTestSos").addEventListener("click", () => {
    const state = activeDataService.getDeviceState();
    if (APP_CONFIG.USE_LIVE_DATA) {
      liveDataService.processSOS({
        active: true,
        eventId: `test-${Date.now()}`,
        deviceName: `${APP_CONFIG.device.name} (test)`,
        stage: "triggered",
        timestamp: Date.now(),
        lat: state.lat,
        lng: state.lng
      });
    } else {
      demoDataService.triggerSimulatedSOS();
    }
  });
}

function locateGuardian() {
  if (!navigator.geolocation) {
    showMapMessage("This browser cannot share its location");
    return;
  }
  showMapMessage("Waiting for location permission…", 0);
  navigator.geolocation.getCurrentPosition(
    ({ coords }) => {
      if (!mapController?.setParentLocation(coords.latitude, coords.longitude)) {
        showMapMessage("The browser returned an invalid location");
        return;
      }
      mapController.fitChildAndParent();
      showMapMessage("Your location is shown on the map");
      if (APP_CONFIG.USE_LIVE_DATA) {
        liveDataService.publishParentLocation(coords.latitude, coords.longitude).then((saved) => {
          if (saved) showMapMessage("Your location is saved to your account");
        });
      }
    },
    (error) => {
      showMapMessage(error.code === error.PERMISSION_DENIED
        ? "Location permission was denied"
        : error.code === error.TIMEOUT
          ? "Location request timed out. Try again."
          : "Your location is unavailable");
    },
    { enableHighAccuracy: true, timeout: 15000, maximumAge: 10000 }
  );
}

async function shareLocation() {
  const state = activeDataService.getDeviceState();
  if (state.lat == null || state.lng == null) {
    showToast("No device location to share yet");
    return;
  }
  const url = `https://www.google.com/maps?q=${state.lat},${state.lng}`;
  const text = `${state.name} location (${new Date(state.timestamp || Date.now()).toLocaleTimeString()}): ${url}`;

  if (navigator.share) {
    try {
      await navigator.share({ title: `${state.name} location`, text, url });
    } catch {
      // Share sheet dismissed
    }
    return;
  }
  try {
    await navigator.clipboard.writeText(text);
    showToast("Location link copied");
  } catch {
    showToast("Sharing is not available in this browser");
  }
}

// ---------------------------------------------------------
// Theme
// ---------------------------------------------------------

function storedTheme() {
  try {
    return localStorage.getItem(STORAGE_THEME_KEY);
  } catch {
    return null;
  }
}

function initTheme() {
  // The inline script in <head> already applied the theme before paint.
  let theme = document.documentElement.getAttribute("data-theme");
  if (!storedTheme() && APP_CONFIG.map.defaultTheme && APP_CONFIG.map.defaultTheme !== "system") {
    theme = APP_CONFIG.map.defaultTheme;
  }
  applyTheme(theme || "light");

  $("themeToggleBtn").addEventListener("click", () => {
    const next = document.documentElement.getAttribute("data-theme") === "dark" ? "light" : "dark";
    applyTheme(next);
    try {
      localStorage.setItem(STORAGE_THEME_KEY, next);
    } catch {
      // Private mode: theme applies for this visit only.
    }
  });

  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", (event) => {
    if (!storedTheme()) applyTheme(event.matches ? "dark" : "light");
  });
}

function applyTheme(theme) {
  document.documentElement.setAttribute("data-theme", theme);
  const dark = theme === "dark";
  $("themeIcon")?.setAttribute("href", dark ? "#i-sun" : "#i-moon");
  const button = $("themeToggleBtn");
  button?.setAttribute("aria-label", dark ? "Switch to light theme" : "Switch to dark theme");
  button?.setAttribute("title", dark ? "Switch to light theme" : "Switch to dark theme");
  document.querySelector('meta[name="theme-color"]')?.setAttribute("content", dark ? "#13171d" : "#ffffff");
  mapController?.setTheme(theme);
}

// ---------------------------------------------------------
// Feedback
// ---------------------------------------------------------

let mapMessageTimer = null;

// durationMs 0 keeps the message until replaced.
function showMapMessage(message, durationMs = 4000) {
  const el = $("parentLocationStatus");
  if (!el) return;
  el.textContent = message;
  el.hidden = false;
  clearTimeout(mapMessageTimer);
  if (durationMs > 0) mapMessageTimer = setTimeout(() => { el.hidden = true; }, durationMs);
}

function showToast(message) {
  document.querySelector(".toast")?.remove();
  const toast = document.createElement("div");
  toast.className = "toast";
  toast.setAttribute("role", "status");
  toast.textContent = message;
  document.body.appendChild(toast);
  setTimeout(() => {
    toast.classList.add("is-leaving");
    setTimeout(() => toast.remove(), 200);
  }, 2600);
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", initDashboard);
} else {
  initDashboard();
}
