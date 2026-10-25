import { fmtBytes, rebootDevice } from "./api.js";

export function renderDevice(body) {
  const free = fmtBytes(body.sd_free_bytes);
  const total = fmtBytes(body.sd_total_bytes);
  document.getElementById("sys").textContent = [
    body.uname || "",
    body.uptime || "",
    "",
    body.card || "",
    "SD free " + free + " of " + total
  ].join("\n");
  document.getElementById("mem").textContent = body.memory || "";
}

export function bindReboot({ toast }) {
  document.getElementById("reboot").addEventListener("click", async () => {
    if (!confirm("Reboot the MPC now?")) return;
    try {
      await rebootDevice();
      toast("Rebooting.");
    } catch (e) {
      toast(e.message);
    }
  });
}
