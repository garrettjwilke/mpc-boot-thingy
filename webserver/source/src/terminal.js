export function bindTerminal({ connect }) {
  const termStatus = document.getElementById("term-status");
  let ws = null;
  let term = null;
  let fit = null;
  let termReady = false;

  function setTermStatus(text, ok) {
    termStatus.textContent = text;
    termStatus.className = ok ? "badge ok" : "badge";
  }

  function ensureTerm() {
    if (termReady) return;
    const Terminal = globalThis.Terminal;
    const FitAddon = globalThis.FitAddon;
    if (!Terminal || !FitAddon) {
      setTermStatus("xterm missing", false);
      return;
    }
    term = new Terminal({
      cursorBlink: true,
      fontFamily: "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace",
      fontSize: 13,
      theme: termTheme()
    });
    fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open(document.getElementById("term"));
    term.onData((data) => {
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(new TextEncoder().encode(data));
    });
    term.onResize(() => sendSize());
    window.addEventListener("resize", () => {
      if (fit && document.getElementById("tab-terminal").classList.contains("active")) fit.fit();
    });
    termReady = true;
  }

  function termTheme() {
    const style = getComputedStyle(document.documentElement);
    return {
      background: style.getPropertyValue("--input").trim(),
      foreground: style.getPropertyValue("--text").trim(),
      cursor: style.getPropertyValue("--accent").trim()
    };
  }

  document.addEventListener("mpc-theme", () => {
    if (term) term.options.theme = termTheme();
  });

  function sendSize() {
    if (!term || !ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ cols: term.cols, rows: term.rows }));
  }

  function connectTerm() {
    ensureTerm();
    if (!termReady) return;
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) ws.close();
    setTermStatus("connecting", false);
    const url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws/term";
    ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      setTermStatus("connected", true);
      fit.fit();
      sendSize();
      term.focus();
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data === "string") term.write(ev.data);
      else term.write(new Uint8Array(ev.data));
    };
    ws.onclose = () => setTermStatus("closed", false);
    ws.onerror = () => setTermStatus("error", false);
  }

  document.getElementById("reconnect").addEventListener("click", () => {
    if (!connect) {
      setTermStatus("closed", false);
      return;
    }
    ensureTerm();
    if (fit) fit.fit();
    connectTerm();
  });

  return {
    show() {
      if (!connect) {
        setTermStatus("closed", false);
        return;
      }
      ensureTerm();
      requestAnimationFrame(() => {
        if (fit) fit.fit();
        if (!ws || ws.readyState === WebSocket.CLOSED || ws.readyState === WebSocket.CLOSING) connectTerm();
        else sendSize();
      });
    }
  };
}
