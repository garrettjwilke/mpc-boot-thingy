import { rebootDevice } from "./api.js";

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
