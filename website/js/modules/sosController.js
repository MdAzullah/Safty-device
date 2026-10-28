/* =========================================================
   SOS CONTROLLER
   Emergency banner, tab title and a short audible alert.
   ========================================================= */

const $ = (id) => document.getElementById(id);

const LOCATION_LABELS = {
  current: "Live GPS location",
  last_known: "Last known location",
  none: "Location unavailable"
};

export class SosController {
  constructor() {
    this.baseTitle = document.title;
    this.activeEventId = null;
  }

  show({ eventId, deviceName = "Guardian Beacon", timestamp = Date.now(), locationSource, smsSent, callStarted, stage } = {}) {
    const banner = $("sosBanner");
    if (!banner) return;

    $("sosTitle").textContent = `SOS from ${deviceName}`;

    const parts = [`Triggered ${new Date(timestamp).toLocaleTimeString()}`];
    if (locationSource) parts.push(LOCATION_LABELS[locationSource] || locationSource);
    if (stage === "completed") {
      parts.push(smsSent ? "SMS sent" : "SMS not sent");
      parts.push(callStarted ? "Call started" : "Call not started");
    } else if (stage === "triggered") {
      parts.push("Device is contacting guardians");
    }
    $("sosSub").textContent = parts.join(" · ");

    const isNewAlert = banner.hidden || (eventId && eventId !== this.activeEventId);
    this.activeEventId = eventId || this.activeEventId;
    banner.hidden = false;
    document.title = `SOS · ${this.baseTitle}`;

    if (isNewAlert) this.chime();
  }

  hide() {
    const banner = $("sosBanner");
    if (banner) banner.hidden = true;
    this.activeEventId = null;
    document.title = this.baseTitle;
  }

  // Three short tones. Browsers may block audio until the page has been
  // interacted with (signing in counts).
  chime() {
    try {
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      if (!AudioCtx) return;
      const ctx = new AudioCtx();
      [0, 0.28, 0.56].forEach((offset) => {
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.frequency.value = 880;
        gain.gain.setValueAtTime(0.0001, ctx.currentTime + offset);
        gain.gain.exponentialRampToValueAtTime(0.2, ctx.currentTime + offset + 0.02);
        gain.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + offset + 0.2);
        osc.connect(gain).connect(ctx.destination);
        osc.start(ctx.currentTime + offset);
        osc.stop(ctx.currentTime + offset + 0.22);
      });
      setTimeout(() => ctx.close(), 1200);
    } catch {
      // Audio is a courtesy; the banner is the alert.
    }
  }
}
