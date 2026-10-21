/* =========================================================
   SOS CONTROLLER
   Manages emergency alert banner visibility and acknowledgment.
   ========================================================= */

const $ = (id) => document.getElementById(id);

export class SosController {
  constructor() {
    $("sosAck")?.addEventListener("click", () => this.hide());
  }

  show(deviceName = "Guardian Beacon", timestamp = Date.now()) {
    const banner = $("sosBanner");
    if (!banner) return;
    if ($("sosTitle")) $("sosTitle").textContent = `SOS ALERT — ${deviceName}`;
    if ($("sosSub")) {
      $("sosSub").textContent = `Triggered ${new Date(timestamp).toLocaleTimeString()} · Live beacon broadcast active`;
    }
    banner.hidden = false;
  }

  hide() {
    const banner = $("sosBanner");
    if (banner) banner.hidden = true;
  }
}
