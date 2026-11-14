export function bindScreen({ toast, useWs = true }) {
  const statusEl = document.getElementById("screen-status");
  const metaEl = document.getElementById("screen-meta");
  const toggleBtn = document.getElementById("screen-toggle");
  const refreshBtn = document.getElementById("screen-refresh");
  const downloadBtn = document.getElementById("screen-download");
  const stageEl = document.getElementById("screen-stage");
  const imgEl = document.getElementById("screen-img");
  const scaleBtns = document.querySelectorAll(".screen-scale-group button[data-scale]");

  let active = false;
  let paused = false;
  let ws = null;
  let pollTimer = null;
  let reconnectTimer = null;
  let currentUrl = null;
  let latestBlob = null;
  let sourceName = "";
  let frameTimes = [];

  const savedScale = localStorage.getItem("mpc-screen-scale");
  if (savedScale && ["1", "2", "3", "fit"].includes(savedScale)) {
    applyScale(savedScale);
  }

  function applyScale(scale) {
    stageEl.dataset.scale = scale;
    scaleBtns.forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.scale === scale);
    });
    localStorage.setItem("mpc-screen-scale", scale);
  }

  scaleBtns.forEach((btn) => {
    btn.addEventListener("click", () => applyScale(btn.dataset.scale));
  });

  function setStatus(text, kind = "") {
    statusEl.textContent = text;
    statusEl.className = kind ? "badge " + kind : "badge";
  }

  function updateMeta() {
    const now = performance.now();
    frameTimes = frameTimes.filter((t) => now - t <= 1000);
    const fps = frameTimes.length;
    const parts = ["320×240"];
    if (sourceName) parts.push(sourceName.toUpperCase());
    if (fps > 0 && !paused) parts.push(fps + " fps");
    metaEl.textContent = parts.join(" · ");
  }

  function displayBlob(blob) {
    latestBlob = blob;
    const nextUrl = URL.createObjectURL(blob);
    imgEl.src = nextUrl;
    if (currentUrl) URL.revokeObjectURL(currentUrl);
    currentUrl = nextUrl;
    frameTimes.push(performance.now());
    updateMeta();
  }

  async function fetchSingleFrame() {
    try {
      const res = await fetch("/api/screen.png?t=" + Date.now(), { cache: "no-store" });
      if (!res.ok) throw new Error("Screen capture HTTP " + res.status);
      const srcHdr = res.headers.get("X-Screen-Source");
      if (srcHdr) sourceName = srcHdr;
      const blob = await res.blob();
      displayBlob(blob);
      setStatus(paused ? "paused" : "live", paused ? "" : "ok");
      return blob;
    } catch (e) {
      setStatus("offline", "warn");
      throw e;
    }
  }

  function stopPolling() {
    if (pollTimer) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function startHttpPolling() {
    stopPolling();
    if (!active || paused || document.hidden) return;
    const tick = async () => {
      if (!active || paused || document.hidden) return;
      try {
        await fetchSingleFrame();
      } catch (_) {}
      if (active && !paused && !document.hidden) {
        pollTimer = setTimeout(tick, 200);
      }
    };
    tick();
  }

  function closeWs() {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    if (ws) {
      ws.onopen = null;
      ws.onmessage = null;
      ws.onerror = null;
      ws.onclose = null;
      try {
        ws.close();
      } catch (_) {}
      ws = null;
    }
  }

  function connectStream() {
    closeWs();
    stopPolling();
    if (!active || document.hidden) return;

    if (!useWs) {
      if (paused) {
        setStatus("paused");
        fetchSingleFrame().catch(() => {});
      } else {
        startHttpPolling();
      }
      return;
    }

    const proto = location.protocol === "https:" ? "wss://" : "ws://";
    setStatus("connecting…");
    ws = new WebSocket(proto + location.host + "/ws/screen");
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
      setStatus(paused ? "paused" : "live", paused ? "" : "ok");
      if (paused) {
        try {
          ws.send("pause");
        } catch (_) {}
      }
    };

    ws.onmessage = (ev) => {
      if (typeof ev.data === "string") {
        try {
          const meta = JSON.parse(ev.data);
          if (meta && meta.source) {
            sourceName = meta.source;
            updateMeta();
          }
        } catch (_) {}
        return;
      }
      const blob = new Blob([ev.data], { type: "image/png" });
      displayBlob(blob);
      setStatus(paused ? "paused" : "live", paused ? "" : "ok");
    };

    ws.onerror = () => {
      /* Fallback handled in onclose */
    };

    ws.onclose = () => {
      ws = null;
      if (!active || document.hidden) return;
      setStatus("reconnecting…", "warn");
      fetchSingleFrame().catch(() => {});
      reconnectTimer = setTimeout(() => {
        if (active && !document.hidden) connectStream();
      }, 1500);
    };
  }

  toggleBtn.addEventListener("click", () => {
    paused = !paused;
    toggleBtn.textContent = paused ? "Resume Live" : "Pause";
    setStatus(paused ? "paused" : "live", paused ? "" : "ok");
    updateMeta();
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(paused ? "pause" : "resume");
    } else if (!useWs) {
      if (paused) stopPolling();
      else startHttpPolling();
    } else if (!paused) {
      connectStream();
    }
  });

  refreshBtn.addEventListener("click", () => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send("refresh");
    } else {
      fetchSingleFrame().catch((e) => toast(e.message));
    }
  });

  downloadBtn.addEventListener("click", async () => {
    try {
      const blob = latestBlob || (await fetchSingleFrame());
      const now = new Date();
      const pad = (n) => String(n).padStart(2, "0");
      const stamp =
        now.getFullYear() +
        pad(now.getMonth() + 1) +
        pad(now.getDate()) +
        "-" +
        pad(now.getHours()) +
        pad(now.getMinutes()) +
        pad(now.getSeconds());
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = `mpc-screen-${stamp}.png`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (e) {
      toast(e.message || "Could not download screen capture");
    }
  });

  document.addEventListener("visibilitychange", () => {
    if (!active) return;
    if (document.hidden) {
      closeWs();
      stopPolling();
    } else {
      connectStream();
    }
  });

  return {
    show() {
      active = true;
      connectStream();
    },
    hide() {
      active = false;
      closeWs();
      stopPolling();
    },
    refresh() {
      if (!active) return;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send("refresh");
      } else {
        fetchSingleFrame().catch((e) => toast(e.message));
      }
    },
  };
}

