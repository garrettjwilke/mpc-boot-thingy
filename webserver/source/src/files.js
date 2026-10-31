import {
  fmtBytes,
  fetchFiles,
  createFolder,
  renameFile,
  moveFiles,
  copyFiles,
  deleteFiles,
  uploadFile,
  downloadFileUrl,
  exportZip
} from "./api.js";

export function bindFiles({ toast }) {
  let cwd = "";
  let parent = "";
  let entries = [];
  const selected = new Set();
  let clipboard = null; // { op: 'copy' | 'move', items: [] }
  let activeRenamePath = "";

  const tabFiles = document.getElementById("tab-files");
  const elBreadcrumbs = document.getElementById("fb-breadcrumbs");
  const elDisk = document.getElementById("fb-disk");
  const elList = document.getElementById("fb-list");
  const elSelectAll = document.getElementById("fb-select-all");
  const elSelectionBar = document.getElementById("fb-selection-bar");
  const elSelectedCount = document.getElementById("fb-selected-count");
  const elPasteBar = document.getElementById("fb-paste-bar");
  const elPasteInfo = document.getElementById("fb-paste-info");
  const fileInput = document.getElementById("fb-file-input");

  // Modals
  const modalMkdir = document.getElementById("modal-mkdir");
  const inputMkdir = document.getElementById("mkdir-name");
  const modalRename = document.getElementById("modal-rename");
  const inputRename = document.getElementById("rename-name");
  const modalDelete = document.getElementById("modal-delete");
  const elDeleteList = document.getElementById("delete-list");

  function getRelPath(name) {
    return cwd ? cwd + "/" + name : name;
  }

  function updateSelectionBar() {
    const count = selected.size;
    if (count > 0) {
      elSelectionBar.hidden = false;
      elSelectedCount.textContent = count + (count === 1 ? " item selected" : " items selected");
    } else {
      elSelectionBar.hidden = true;
    }
    if (elSelectAll) {
      elSelectAll.checked = entries.length > 0 && selected.size === entries.length;
      elSelectAll.indeterminate = selected.size > 0 && selected.size < entries.length;
    }
  }

  function updatePasteBar() {
    if (clipboard && clipboard.items.length > 0) {
      elPasteBar.hidden = false;
      const act = clipboard.op === "move" ? "Move" : "Copy";
      elPasteInfo.textContent = `${act} ${clipboard.items.length} item(s) here`;
    } else {
      elPasteBar.hidden = true;
    }
  }

  function renderBreadcrumbs() {
    elBreadcrumbs.innerHTML = "";

    const rootBtn = document.createElement("button");
    rootBtn.type = "button";
    rootBtn.className = "crumb-btn";
    rootBtn.textContent = "/data";
    rootBtn.addEventListener("click", () => navigateTo(""));
    elBreadcrumbs.appendChild(rootBtn);

    if (!cwd) return;

    const parts = cwd.split("/").filter(Boolean);
    let accum = "";
    for (let i = 0; i < parts.length; i++) {
      const sep = document.createElement("span");
      sep.className = "crumb-sep";
      sep.textContent = "/";
      elBreadcrumbs.appendChild(sep);

      accum = accum ? accum + "/" + parts[i] : parts[i];
      const target = accum;
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "crumb-btn" + (i === parts.length - 1 ? " current" : "");
      btn.textContent = parts[i];
      if (i < parts.length - 1) {
        btn.addEventListener("click", () => navigateTo(target));
      }
      elBreadcrumbs.appendChild(btn);
    }
  }

  function renderList() {
    elList.innerHTML = "";
    renderBreadcrumbs();
    updateSelectionBar();
    updatePasteBar();

    // Sort: directories first, then alphabetical by name
    const sorted = entries.slice().sort((a, b) => {
      if (a.is_dir && !b.is_dir) return -1;
      if (!a.is_dir && b.is_dir) return 1;
      return a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" });
    });

    if (sorted.length === 0) {
      const tr = document.createElement("tr");
      tr.innerHTML = `<td colspan="5" class="empty-hint">Folder is empty</td>`;
      elList.appendChild(tr);
      return;
    }

    for (const item of sorted) {
      const rel = getRelPath(item.name);
      const isSel = selected.has(rel);

      const tr = document.createElement("tr");
      if (isSel) tr.classList.add("selected");

      // Checkbox
      const tdCheck = document.createElement("td");
      tdCheck.className = "col-check";
      const chk = document.createElement("input");
      chk.type = "checkbox";
      chk.checked = isSel;
      chk.addEventListener("change", (e) => {
        e.stopPropagation();
        if (chk.checked) selected.add(rel);
        else selected.delete(rel);
        if (chk.checked) tr.classList.add("selected");
        else tr.classList.remove("selected");
        updateSelectionBar();
      });
      tdCheck.appendChild(chk);
      tr.appendChild(tdCheck);

      // Icon & Name
      const tdName = document.createElement("td");
      tdName.className = "col-name";
      const icon = document.createElement("span");
      icon.className = "file-icon " + (item.is_dir ? "icon-dir" : "icon-file");
      icon.textContent = item.is_dir ? "📁 " : "📄 ";

      const nameSpan = document.createElement("span");
      nameSpan.className = "file-title" + (item.is_dir ? " is-dir" : "");
      nameSpan.textContent = item.name;

      tdName.appendChild(icon);
      tdName.appendChild(nameSpan);

      if (item.is_dir) {
        tdName.style.cursor = "pointer";
        tdName.addEventListener("click", () => navigateTo(rel));
      }
      tr.appendChild(tdName);

      // Size
      const tdSize = document.createElement("td");
      tdSize.className = "col-size muted";
      tdSize.textContent = item.is_dir ? "—" : fmtBytes(item.size);
      tr.appendChild(tdSize);

      // Date
      const tdDate = document.createElement("td");
      tdDate.className = "col-date muted";
      if (item.mtime) {
        const d = new Date(item.mtime * 1000);
        tdDate.textContent = d.toLocaleDateString() + " " + d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
      } else {
        tdDate.textContent = "—";
      }
      tr.appendChild(tdDate);

      // Actions
      const tdAct = document.createElement("td");
      tdAct.className = "col-actions";

      if (!item.is_dir) {
        const dlBtn = document.createElement("a");
        dlBtn.className = "action-link";
        dlBtn.href = downloadFileUrl(rel);
        dlBtn.title = "Download";
        dlBtn.textContent = "⬇";
        tdAct.appendChild(dlBtn);
      }

      const renBtn = document.createElement("button");
      renBtn.type = "button";
      renBtn.className = "action-btn";
      renBtn.title = "Rename";
      renBtn.textContent = "✎";
      renBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        openRename(rel, item.name);
      });
      tdAct.appendChild(renBtn);

      const delBtn = document.createElement("button");
      delBtn.type = "button";
      delBtn.className = "action-btn danger";
      delBtn.title = "Delete";
      delBtn.textContent = "✕";
      delBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        openDelete([rel]);
      });
      tdAct.appendChild(delBtn);

      tr.appendChild(tdAct);
      elList.appendChild(tr);
    }
  }

  async function loadDirectory(path = "") {
    try {
      const data = await fetchFiles(path);
      cwd = data.path || "";
      parent = data.parent || "";
      entries = data.entries || [];
      selected.clear();

      if (elDisk && data.total_bytes) {
        const free = fmtBytes(data.free_bytes);
        const total = fmtBytes(data.total_bytes);
        elDisk.textContent = `${free} free / ${total} total (/data)`;
      }

      renderList();
    } catch (e) {
      toast(e.message || "Failed to load directory");
    }
  }

  function navigateTo(path) {
    loadDirectory(path);
  }

  // Select all checkbox
  if (elSelectAll) {
    elSelectAll.addEventListener("change", () => {
      if (elSelectAll.checked) {
        for (const item of entries) {
          selected.add(getRelPath(item.name));
        }
      } else {
        selected.clear();
      }
      renderList();
    });
  }

  // New folder modal
  document.getElementById("fb-btn-mkdir")?.addEventListener("click", () => {
    inputMkdir.value = "";
    modalMkdir.hidden = false;
    inputMkdir.focus();
  });

  document.getElementById("mkdir-cancel")?.addEventListener("click", () => {
    modalMkdir.hidden = true;
  });

  document.getElementById("mkdir-confirm")?.addEventListener("click", async () => {
    const name = inputMkdir.value.trim();
    if (!name) return;
    try {
      const target = getRelPath(name);
      await createFolder(target);
      modalMkdir.hidden = true;
      toast(`Created folder "${name}"`);
      await loadDirectory(cwd);
    } catch (e) {
      toast(e.message);
    }
  });

  // Rename modal
  function openRename(relPath, oldName) {
    activeRenamePath = relPath;
    inputRename.value = oldName;
    modalRename.hidden = false;
    inputRename.focus();
    inputRename.select();
  }

  document.getElementById("rename-cancel")?.addEventListener("click", () => {
    modalRename.hidden = true;
  });

  document.getElementById("rename-confirm")?.addEventListener("click", async () => {
    const newName = inputRename.value.trim();
    if (!newName) return;
    try {
      await renameFile(activeRenamePath, newName);
      modalRename.hidden = true;
      toast(`Renamed to "${newName}"`);
      await loadDirectory(cwd);
    } catch (e) {
      toast(e.message);
    }
  });

  // Delete modal
  let itemsToDelete = [];
  function openDelete(items) {
    itemsToDelete = items;
    elDeleteList.innerHTML = "";
    for (const item of items) {
      const li = document.createElement("li");
      li.textContent = item;
      elDeleteList.appendChild(li);
    }
    modalDelete.hidden = false;
  }

  document.getElementById("delete-cancel")?.addEventListener("click", () => {
    modalDelete.hidden = true;
    itemsToDelete = [];
  });

  document.getElementById("delete-confirm")?.addEventListener("click", async () => {
    if (itemsToDelete.length === 0) return;
    try {
      await deleteFiles(itemsToDelete);
      modalDelete.hidden = true;
      toast(`Deleted ${itemsToDelete.length} item(s)`);
      itemsToDelete = [];
      await loadDirectory(cwd);
    } catch (e) {
      toast(e.message);
    }
  });

  // Batch actions
  document.getElementById("fb-btn-del-selected")?.addEventListener("click", () => {
    if (selected.size === 0) return;
    openDelete(Array.from(selected));
  });

  document.getElementById("fb-btn-deselect")?.addEventListener("click", () => {
    selected.clear();
    renderList();
  });

  document.getElementById("fb-btn-copy")?.addEventListener("click", () => {
    if (selected.size === 0) return;
    clipboard = { op: "copy", items: Array.from(selected) };
    selected.clear();
    toast(`Copied ${clipboard.items.length} item(s) to clipboard`);
    renderList();
  });

  document.getElementById("fb-btn-cut")?.addEventListener("click", () => {
    if (selected.size === 0) return;
    clipboard = { op: "move", items: Array.from(selected) };
    selected.clear();
    toast(`Cut ${clipboard.items.length} item(s) to clipboard`);
    renderList();
  });

  document.getElementById("fb-btn-paste-cancel")?.addEventListener("click", () => {
    clipboard = null;
    updatePasteBar();
  });

  document.getElementById("fb-btn-paste")?.addEventListener("click", async () => {
    if (!clipboard || clipboard.items.length === 0) return;
    const { op, items } = clipboard;
    try {
      if (op === "copy") {
        await copyFiles(items, cwd);
        toast(`Copied ${items.length} item(s)`);
      } else if (op === "move") {
        await moveFiles(items, cwd);
        toast(`Moved ${items.length} item(s)`);
      }
      clipboard = null;
      await loadDirectory(cwd);
    } catch (e) {
      toast(e.message);
    }
  });

  // Upload
  document.getElementById("fb-btn-upload")?.addEventListener("click", () => {
    fileInput.value = "";
    fileInput.click();
  });

  async function handleFilesUpload(fileList) {
    if (!fileList || fileList.length === 0) return;
    let uploaded = 0;
    for (const file of fileList) {
      try {
        toast(`Uploading ${file.name}…`);
        await uploadFile(cwd, file);
        uploaded++;
      } catch (e) {
        toast(`Failed to upload ${file.name}: ${e.message}`);
      }
    }
    if (uploaded > 0) {
      toast(`Successfully uploaded ${uploaded} file(s)`);
      await loadDirectory(cwd);
    }
  }

  fileInput?.addEventListener("change", (e) => {
    handleFilesUpload(e.target.files);
  });

  // Drag and drop onto tab
  tabFiles?.addEventListener("dragover", (e) => {
    e.preventDefault();
    tabFiles.classList.add("drag-over");
  });
  tabFiles?.addEventListener("dragleave", (e) => {
    if (!tabFiles.contains(e.relatedTarget)) {
      tabFiles.classList.remove("drag-over");
    }
  });
  tabFiles?.addEventListener("drop", (e) => {
    e.preventDefault();
    tabFiles.classList.remove("drag-over");
    if (e.dataTransfer && e.dataTransfer.files) {
      handleFilesUpload(e.dataTransfer.files);
    }
  });

  // ZIP Exports
  document.getElementById("fb-btn-zip-all")?.addEventListener("click", async () => {
    try {
      toast("Generating /data backup ZIP…");
      const d = new Date().toISOString().slice(0, 10);
      await exportZip(null, `mpc-data-all-${d}.zip`);
      toast("ZIP download started!");
    } catch (e) {
      toast(e.message);
    }
  });

  document.getElementById("fb-btn-zip-selected")?.addEventListener("click", async () => {
    if (selected.size === 0) return;
    try {
      const items = Array.from(selected);
      toast(`Generating ZIP of ${items.length} item(s)…`);
      const d = new Date().toISOString().slice(0, 10);
      await exportZip(items, `mpc-data-selected-${d}.zip`);
      toast("ZIP download started!");
    } catch (e) {
      toast(e.message);
    }
  });

  document.getElementById("fb-btn-refresh")?.addEventListener("click", () => {
    loadDirectory(cwd);
  });

  return {
    show: () => loadDirectory(cwd),
    refresh: () => loadDirectory(cwd)
  };
}
