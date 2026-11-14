export function bindScreen({ toast, useWs = true }) {
  const statusEl = document.getElementById("screen-status");
  const metaEl = document.getElementById("screen-meta");
  const toggleBtn = document.getElementById("screen-toggle");
  const refreshBtn = document.getElementById("screen-refresh");
  const downloadBtn = document.getElementById("screen-download");
  const fullscreenBtn = document.getElementById("screen-fullscreen");
  const floatingFsBtn = document.getElementById("screen-floating-fs");
  const popupBtn = document.getElementById("screen-popup");
  const stageEl = document.getElementById("screen-stage");
  const bezelEl = stageEl.querySelector(".screen-bezel");
  const imgEl = document.getElementById("screen-img");
  const scaleBtns = document.querySelectorAll(".screen-scale-group button[data-scale]");
  const isPopup = document.documentElement.dataset.popup === "screen";

  let active = false;
  let paused = false;
  let ws = null;
  let pollTimer = null;
  let reconnectTimer = null;
  let currentUrl = null;
  let latestBlob = null;
  let sourceName = "";
  let frameTimes = [];
  let popupWin = null;

  const savedScale = localStorage.getItem("mpc-screen-scale");
  if (savedScale && ["1", "2", "3"].includes(savedScale)) {
    applyScale(savedScale);
  }

  function isFullscreen() {
    return Boolean(document.fullscreenElement || document.webkitFullscreenElement);
  }

  function syncFullscreenAndPopupLayout() {
    const fs = isFullscreen();
    const fillViewport = fs || isPopup;
    document.documentElement.classList.toggle("screen-is-fullscreen", fs);
    if (fillViewport) {
      stageEl.dataset.fullscreen = "true";
      const vw = window.innerWidth || document.documentElement.clientWidth || 640;
      const vh = window.innerHeight || document.documentElement.clientHeight || 480;
      const w = Math.min(vw, Math.floor((vh * 4) / 3));
      const h = Math.min(vh, Math.floor((vw * 3) / 4));

      stageEl.style.padding = "0";
      stageEl.style.margin = "0";
      stageEl.style.border = "none";
      stageEl.style.borderRadius = "0";
      stageEl.style.background = "#000";
      stageEl.style.width = "100vw";
      stageEl.style.height = "100vh";
      stageEl.style.overflow = "hidden";

      if (bezelEl) {
        bezelEl.style.padding = "0";
        bezelEl.style.margin = "0";
        bezelEl.style.border = "none";
        bezelEl.style.borderRadius = "0";
        bezelEl.style.boxShadow = "none";
        bezelEl.style.background = "#000";
        bezelEl.style.width = "100vw";
        bezelEl.style.height = "100vh";
        bezelEl.style.display = "flex";
        bezelEl.style.justifyContent = "center";
        bezelEl.style.alignItems = "center";
      }

      imgEl.style.width = w + "px";
      imgEl.style.height = h + "px";
      imgEl.style.maxWidth = "none";
      imgEl.style.maxHeight = "none";
    } else {
      delete stageEl.dataset.fullscreen;
      stageEl.style.padding = "";
      stageEl.style.margin = "";
      stageEl.style.border = "";
      stageEl.style.borderRadius = "";
      stageEl.style.background = "";
      stageEl.style.width = "";
      stageEl.style.height = "";
      stageEl.style.overflow = "";

      if (bezelEl) {
        bezelEl.style.padding = "";
        bezelEl.style.margin = "";
        bezelEl.style.border = "";
        bezelEl.style.borderRadius = "";
        bezelEl.style.boxShadow = "";
        bezelEl.style.background = "";
        bezelEl.style.width = "";
        bezelEl.style.height = "";
        bezelEl.style.display = "";
        bezelEl.style.justifyContent = "";
        bezelEl.style.alignItems = "";
      }

      imgEl.style.width = "";
      imgEl.style.height = "";
      imgEl.style.maxWidth = "";
      imgEl.style.maxHeight = "";
    }

    const label = fs ? "Exit Fullscreen" : "Fullscreen";
    if (fullscreenBtn) fullscreenBtn.textContent = label;
    if (floatingFsBtn) floatingFsBtn.textContent = label;
  }

  function applyScale(scale) {
    stageEl.dataset.scale = scale;
    scaleBtns.forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.scale === scale);
    });
    localStorage.setItem("mpc-screen-scale", scale);
    syncFullscreenAndPopupLayout();
  }

  scaleBtns.forEach((btn) => {
    btn.addEventListener("click", () => applyScale(btn.dataset.scale));
  });

  function scheduleLayoutSync() {
    syncFullscreenAndPopupLayout();
    requestAnimationFrame(syncFullscreenAndPopupLayout);
    setTimeout(syncFullscreenAndPopupLayout, 60);
    setTimeout(syncFullscreenAndPopupLayout, 180);
  }

  async function toggleFullscreen() {
    try {
      if (isFullscreen()) {
        if (document.exitFullscreen) await document.exitFullscreen();
        else if (document.webkitExitFullscreen) document.webkitExitFullscreen();
      } else {
        const target = isPopup ? document.documentElement : stageEl;
        if (target.requestFullscreen) await target.requestFullscreen();
        else if (target.webkitRequestFullscreen) target.webkitRequestFullscreen();
      }
      scheduleLayoutSync();
    } catch (e) {
      toast(e.message || "Fullscreen is not available");
    }
  }

  if (fullscreenBtn) fullscreenBtn.addEventListener("click", toggleFullscreen);
  if (floatingFsBtn) floatingFsBtn.addEventListener("click", toggleFullscreen);
  imgEl.addEventListener("dblclick", toggleFullscreen);
  document.addEventListener("fullscreenchange", scheduleLayoutSync);
  document.addEventListener("webkitfullscreenchange", scheduleLayoutSync);
  window.addEventListener("resize", syncFullscreenAndPopupLayout);

  if (popupBtn) {
    popupBtn.addEventListener("click", () => {
      if (popupWin && !popupWin.closed) {
        popupWin.focus();
        return;
      }
      const url = new URL(location.href);
      url.searchParams.set("popup", "screen");
      popupWin = window.open(
        url.toString(),
        "mpc-screen-popup",
        "width=640,height=480,menubar=no,toolbar=no,location=no,status=no,resizable=yes"
      );
      if (!popupWin) {
        toast("Pop-up blocked by browser — please allow pop-ups for this site");
      }
    });
  }

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

  const api = {
    show() {
      active = true;
      syncFullscreenAndPopupLayout();
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

  if (isPopup) {
    document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
    document.getElementById("tab-screen").classList.add("active");
    scheduleLayoutSync();
    api.show();
  }

  return api;
}
