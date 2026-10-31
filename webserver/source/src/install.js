import { installProgram } from "./api.js";
import { isZip, programError } from "./program.js";

export function titleFromFilename(filename) {
  const base = String(filename || "").split(/[/\\]/).pop() || "";
  const dot = base.lastIndexOf(".");
  const stem = dot > 0 ? base.slice(0, dot) : "";
  return stem || base;
}

function droppedDirectory(dataTransfer) {
  const item = dataTransfer && dataTransfer.items && dataTransfer.items[0];
  if (!item || !item.webkitGetAsEntry) return false;
  const entry = item.webkitGetAsEntry();
  return !!(entry && entry.isDirectory);
}

async function rejection(file) {
  if (!file) return "Choose a zip or an app.";
  if (file.size === 0) return "That file is empty.";
  const head = new Uint8Array(await file.slice(0, 512).arrayBuffer());
  if (isZip(head, file.name)) return "";
  return programError(head);
}

export function bindInstall({ toast, refresh }) {
  const modal = document.getElementById("install-modal");
  const drop = document.getElementById("install-drop");
  const fileInput = document.getElementById("file");
  const stepFile = document.getElementById("install-step-file");
  const stepName = document.getElementById("install-step-name");
  const errorEl = document.getElementById("install-error");
  const filenameEl = document.getElementById("install-filename");
  const titleInput = document.getElementById("title");
  const argsInput = document.getElementById("args");
  const installBtn = document.getElementById("install");
  const cancelBtn = document.getElementById("install-cancel");
  let file = null;
  let busy = false;
  let token = 0;
  let dragDepth = 0;

  function showError(msg) {
    errorEl.hidden = !msg;
    errorEl.textContent = msg || "";
  }

  function showFileStep() {
    stepFile.hidden = false;
    stepName.hidden = true;
    installBtn.hidden = true;
  }

  function showNameStep() {
    stepFile.hidden = true;
    stepName.hidden = false;
    installBtn.hidden = false;
    filenameEl.textContent = file.name;
    titleInput.value = titleFromFilename(file.name);
    titleInput.focus();
    const end = titleInput.value.length;
    titleInput.setSelectionRange(end, end);
  }

  function setBusy(next) {
    busy = next;
    installBtn.disabled = next;
    cancelBtn.disabled = next;
    installBtn.textContent = next ? "Installing…" : "Install";
  }

  function reset() {
    token += 1;
    file = null;
    dragDepth = 0;
    fileInput.value = "";
    titleInput.value = "";
    argsInput.value = "";
    filenameEl.textContent = "";
    drop.classList.remove("dragover");
    showError("");
    showFileStep();
    setBusy(false);
  }

  function open() {
    reset();
    modal.hidden = false;
    document.getElementById("install-browse").focus();
  }

  function close() {
    if (busy) return;
    modal.hidden = true;
    reset();
  }

  async function acceptFile(next) {
    const mine = ++token;
    showError("");
    const why = await rejection(next);
    if (mine !== token || modal.hidden) return;
    if (why) {
      showError(why);
      return;
    }
    file = next;
    showNameStep();
  }

  function takeDrop(dataTransfer) {
    const files = dataTransfer && dataTransfer.files;
    if (!files || files.length === 0) {
      showError("Drop a zip or an app.");
      return;
    }
    if (files.length > 1) {
      showError("Drop one zip or app.");
      return;
    }
    if (droppedDirectory(dataTransfer)) {
      showError("Drop a zip or an app file, not a folder.");
      return;
    }
    acceptFile(files[0]);
  }

  function openPicker() {
    fileInput.value = "";
    fileInput.click();
  }

  titleInput.addEventListener("input", () => {
    if (titleInput.value.trim()) showError("");
  });
  document.getElementById("install-open").addEventListener("click", open);
  cancelBtn.addEventListener("click", close);
  document.getElementById("install-change").addEventListener("click", () => {
    file = null;
    showError("");
    showFileStep();
    document.getElementById("install-browse").focus();
  });
  document.getElementById("install-browse").addEventListener("click", (e) => {
    e.stopPropagation();
    openPicker();
  });
  drop.addEventListener("click", (e) => {
    if (e.target.closest("button")) return;
    openPicker();
  });

  fileInput.addEventListener("change", () => {
    const picked = fileInput.files && fileInput.files[0];
    if (picked) acceptFile(picked);
  });

  modal.addEventListener("click", (e) => {
    if (e.target === modal) close();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !modal.hidden) close();
    if (e.key === "Enter" && !modal.hidden && !stepName.hidden && e.target instanceof HTMLInputElement) {
      e.preventDefault();
      installBtn.click();
    }
  });

  modal.addEventListener("dragenter", (e) => {
    e.preventDefault();
    dragDepth += 1;
    if (!stepFile.hidden) drop.classList.add("dragover");
  });
  modal.addEventListener("dragover", (e) => {
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = "copy";
  });
  modal.addEventListener("dragleave", () => {
    dragDepth -= 1;
    if (dragDepth <= 0) {
      dragDepth = 0;
      drop.classList.remove("dragover");
    }
  });
  window.addEventListener("dragover", (e) => {
    if (!modal.hidden) e.preventDefault();
  });
  window.addEventListener("drop", (e) => {
    if (modal.hidden) return;
    e.preventDefault();
    dragDepth = 0;
    drop.classList.remove("dragover");
    if (!busy) takeDrop(e.dataTransfer);
  });

  installBtn.addEventListener("click", async () => {
    if (!file || busy) return;
    const title = titleInput.value.trim();
    const args = argsInput.value.trim();
    if (!title) {
      showError("Give the app a name.");
      titleInput.focus();
      return;
    }
    const name = (file.name || "app").split(/[/\\]/).pop();
    setBusy(true);
    showError("");
    try {
      await installProgram({ title, args, name, file });
      setBusy(false);
      close();
      toast("Installed " + title + ".");
      await refresh();
    } catch (e) {
      setBusy(false);
      if (modal.hidden) toast(e.message);
      else showError(e.message);
    }
  });
}
