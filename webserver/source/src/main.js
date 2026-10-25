import "./styles.css";
import { fetchStatus } from "./api.js";
import { renderApps } from "./apps.js";
import { bindInstall } from "./install.js";
import { renderDevice, bindReboot } from "./device.js";
import { bindTerminal } from "./terminal.js";
import { bindTheme } from "./theme.js";

const hdrDefault = document.getElementById("hdr-default");
const sysEl = document.getElementById("sys");
document.getElementById("hdr-link").textContent = location.host;
bindTheme();

function toast(msg) {
  const el = document.getElementById("toast");
  el.textContent = msg;
  el.style.display = "block";
  clearTimeout(toast._t);
  toast._t = setTimeout(() => { el.style.display = "none"; }, 4200);
}

async function refreshStatus() {
  const body = await fetchStatus();
  renderApps(body.apps || [], body.default_id || "stock", { toast, refresh: refreshStatus });
  renderDevice(body);
  return body;
}

bindInstall({ toast, refresh: refreshStatus });
bindReboot({ toast });
const terminal = bindTerminal({
  connect: !import.meta.env.DEV || import.meta.env.MODE === "live"
});

document.getElementById("refresh").addEventListener("click", () => {
  refreshStatus().catch((e) => toast(e.message));
});

document.querySelectorAll("nav button").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll("nav button").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
    btn.classList.add("active");
    document.getElementById("tab-" + btn.dataset.tab).classList.add("active");
    if (btn.dataset.tab === "terminal") terminal.show();
  });
});

refreshStatus().catch((e) => {
  hdrDefault.textContent = "Offline";
  hdrDefault.className = "badge warn";
  sysEl.textContent = e.message;
  toast(e.message);
});
