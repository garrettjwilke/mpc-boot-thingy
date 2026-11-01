import "../../../deps/js-fileexplorer/file-explorer/file-explorer.css";
import "../../../deps/js-fileexplorer/file-explorer/file-explorer.js";

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
  const container = document.getElementById("filemanager");
  const elDisk = document.getElementById("fb-disk");
  const btnZipAll = document.getElementById("fb-btn-zip-all");
  const showDotfilesInput = document.getElementById("fb-show-dotfiles");

  let feInstance = null;
  let showDotfiles = false;

  function isDotEntry(name) {
    return typeof name === "string" && name.startsWith(".");
  }

  function triggerRefresh(silent = false) {
    if (feInstance) {
      feInstance.RefreshFolders(true);
    }
    updateDiskStats();
    if (!silent) {
      toast("Refreshed");
    }
  }

  function showDisk(data) {
    if (!elDisk || !data) return;
    if (data.total_bytes) {
      elDisk.textContent = `${fmtBytes(data.free_bytes)} free of ${fmtBytes(data.total_bytes)}`;
    } else {
      elDisk.textContent = "Internal and SD Card";
    }
  }

  async function updateDiskStats() {
    try {
      const data = await fetchFiles("");
      showDisk(data);
    } catch (e) {
      // Ignore initial stats error
    }
  }

  function getFolderRelPath(folder) {
    if (!folder) return "";
    const ids = folder.GetPathIDs ? folder.GetPathIDs() : [];
    return ids.filter(Boolean).join("/");
  }

  function pathIds(path) {
    if (!Array.isArray(path)) return "";
    return path
      .map((part) => (Array.isArray(part) ? part[0] : part))
      .filter(Boolean)
      .join("/");
  }

  function destinationName(folder) {
    const path = folder && folder.GetPath ? folder.GetPath() : [];
    if (!path.length) return "Storage";
    const last = path[path.length - 1];
    if (Array.isArray(last)) return last[1] || last[0] || "Storage";
    return String(last || "Storage");
  }

  function selectedEntries(fe, srcpath, srcids) {
    const folder = fe && fe.GetMappedFolderFromPath ? fe.GetMappedFolderFromPath(srcpath) : null;
    const entries = folder && folder.GetEntries ? folder.GetEntries() : [];
    const byId = new Map(entries.map((entry) => [entry.id, entry]));
    return srcids.map((id) => byId.get(id) || { id, name: String(id), type: "file" });
  }

  function describeSelection(entries) {
    if (entries.length === 1) {
      const entry = entries[0];
      return entry.type === "folder"
        ? `the folder "${entry.name}" and everything inside it`
        : `"${entry.name}"`;
    }
    const folders = entries.filter((entry) => entry.type === "folder").length;
    if (folders === entries.length) {
      return `${entries.length} folders and everything inside them`;
    }
    if (folders) {
      return `${entries.length} items, including ${folders} folder${folders === 1 ? "" : "s"} and their contents`;
    }
    return `${entries.length} items`;
  }

  const clipboardGroup = "mpc_boot_data";
  let contextRow = null;
  let activeMenu = null;

  function canModify(folder) {
    if (!folder || folder.waiting) return false;
    const attrs = folder.GetAttributes ? folder.GetAttributes() || {} : {};
    return !("canmodify" in attrs) || !!attrs.canmodify;
  }

  function entryFromRow(row) {
    if (!row || !feInstance) return null;
    const folder = feInstance.GetCurrentFolder();
    const entries = folder && folder.GetEntries ? folder.GetEntries() : [];
    return entries.find((entry) => entry.id === row.dataset.feid) || null;
  }

  function hideClipboardOverlay() {
    const elems = feInstance && feInstance.GetElements ? feInstance.GetElements() : null;
    const wrap = elems && elems.itemsclipboardoverlaypastewrap;
    if (!wrap) return;
    wrap.classList.add("fe_fileexplorer_hidden");
    wrap.classList.remove("fe_fileexplorer_items_clipboard_contextmenu");
    wrap.classList.remove("fe_fileexplorer_items_show_clipboard_overlay_paste");
    if (elems.itemsclipboardoverlay && document.activeElement === elems.itemsclipboardoverlay) {
      elems.itemsclipboardoverlay.blur();
    }
  }

  function closeContextMenu() {
    if (!activeMenu) return;
    const menu = activeMenu;
    activeMenu = null;
    menu.Destroy();
  }

  async function createNewFolderEntry(folder) {
    const rel = getFolderRelPath(folder);
    let candidate = "New Folder";
    let n = 1;
    const existing = folder.GetEntries();
    while (existing.some((entry) => entry.name === candidate)) {
      candidate = `New Folder ${++n}`;
    }
    const targetPath = rel ? rel + "/" + candidate : candidate;
    await createFolder(targetPath);
    return {
      name: candidate,
      id: candidate,
      type: "folder",
      hash: candidate + "_" + Date.now()
    };
  }

  async function startNewFolder() {
    const folder = feInstance.GetCurrentFolder();
    if (!canModify(folder)) return;
    try {
      const entry = await createNewFolderEntry(folder);
      if (!feInstance.IsMappedFolder(folder)) return;
      folder.SetEntry(entry);
      if (feInstance.GetCurrentFolder() === folder) {
        feInstance.SetFocusItem(entry.id, true);
        feInstance.ScrollToFocusedItem();
        feInstance.SetSelectedItems([entry.id]);
        feInstance.RenameSelectedItem();
      }
      toast("Created folder " + entry.name);
    } catch (err) {
      toast(err.message || "Failed to create folder");
    }
  }

  async function downloadEntries(folder, ids, entries) {
    const rel = getFolderRelPath(folder);
    if (ids.length === 1 && entries.length === 1 && entries[0].type === "file") {
      const filePath = rel ? rel + "/" + ids[0] : ids[0];
      const a = document.createElement("a");
      a.href = downloadFileUrl(filePath);
      a.download = entries[0].name;
      document.body.appendChild(a);
      a.click();
      a.remove();
      return;
    }
    const items = ids.map((id) => (rel ? rel + "/" + id : id));
    const zipName = ids.length === 1
      ? `${entries[0].name}.zip`
      : `mpc-usercontent-export-${new Date().toISOString().slice(0, 10)}.zip`;
    toast("Preparing ZIP export...");
    await exportZip(items, zipName);
    toast("ZIP export complete");
  }

  async function downloadCurrentDirectory() {
    const folder = feInstance.GetCurrentFolder();
    const rel = getFolderRelPath(folder);
    if (!rel) {
      toast("Preparing full /data/UserContent backup as ZIP...");
      await exportZip([], `mpc-usercontent-backup-${new Date().toISOString().slice(0, 10)}.zip`);
      toast("Full backup download started!");
      return;
    }
    toast("Preparing ZIP export...");
    await exportZip([rel], `${destinationName(folder)}.zip`);
    toast("ZIP export complete");
  }

  async function transferItems(op, srcpath, srcids, destRel, destName) {
    const entries = selectedEntries(feInstance, srcpath, srcids);
    const verb = op === "move" ? "Move" : "Copy";
    if (!confirm(`${verb} ${describeSelection(entries)} into "${destName}"?`)) {
      return "Cancelled";
    }
    const srcRel = pathIds(srcpath);
    const items = srcids.map((id) => (srcRel ? srcRel + "/" + id : id));
    if (op === "move") await moveFiles(items, destRel);
    else await copyFiles(items, destRel);
    return items.length;
  }

  function pasteTarget(entry) {
    const folder = feInstance.GetCurrentFolder();
    const rel = getFolderRelPath(folder);
    if (entry && entry.type === "folder") {
      return {
        destRel: rel ? rel + "/" + entry.id : entry.id,
        destName: entry.name,
        allowed: true
      };
    }
    return {
      destRel: rel,
      destName: destinationName(folder),
      allowed: canModify(folder)
    };
  }

  function copySelection(type) {
    const folder = feInstance.GetCurrentFolder();
    const ids = feInstance.GetSelectedItemIDs();
    if (!folder || !ids.length) return;
    const text = JSON.stringify({
      "application/file-explorer-clipboard": {
        type,
        source: "menu",
        group: clipboardGroup,
        path: folder.GetPath(),
        ids
      }
    });
    const elems = feInstance.GetElements();
    const wrap = elems.itemsclipboardoverlaypastewrap;
    const overlay = elems.itemsclipboardoverlay;
    wrap.classList.remove("fe_fileexplorer_hidden");
    overlay.value = text;
    overlay.focus();
    overlay.select();
    try {
      document.execCommand(type === "move" ? "cut" : "copy");
    } catch (err) {
      // The text write below still publishes the payload.
    }
    hideClipboardOverlay();
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).catch(() => {});
    }
    toast(type === "move" ? "Cut to clipboard" : "Copied to clipboard");
  }

  async function pasteClipboard(target) {
    if (!target.allowed) return;
    let text = "";
    try {
      text = await navigator.clipboard.readText();
    } catch (err) {
      toast("Nothing to paste");
      return;
    }
    let payload = null;
    try {
      const data = JSON.parse(text);
      payload = data["application/file-explorer-clipboard"] || null;
    } catch (err) {
      payload = null;
    }
    if (!payload || payload.group !== clipboardGroup || !Array.isArray(payload.path) || !Array.isArray(payload.ids) || !payload.ids.length) {
      toast("Nothing to paste");
      return;
    }
    const op = payload.type === "move" ? "move" : "copy";
    try {
      const count = await transferItems(op, payload.path, payload.ids, target.destRel, target.destName);
      if (count === "Cancelled") return;
      toast(op === "move" ? `Moved ${count} item(s)` : `Copied ${count} item(s)`);
      triggerRefresh(true);
    } catch (err) {
      toast(err.message || (op === "move" ? "Move failed" : "Copy failed"));
    }
  }

  function runContextAction(id, folder, target) {
    if (id === "rename") feInstance.RenameSelectedItem();
    else if (id === "copy") copySelection("copy");
    else if (id === "cut") copySelection("move");
    else if (id === "paste") pasteClipboard(target);
    else if (id === "delete") feInstance.DeleteSelectedItems(true);
    else if (id === "download") {
      downloadEntries(folder, feInstance.GetSelectedItemIDs(), feInstance.GetSelectedFolderEntries())
        .catch((err) => toast(err.message || "Download failed"));
    } else if (id === "new-folder") startNewFolder();
    else if (id === "download-dir") {
      downloadCurrentDirectory().catch((err) => toast(err.message || "Download failed"));
    }
  }

  function openContextMenu(clientX, clientY, row, forceItems) {
    const folder = feInstance.GetCurrentFolder();
    if (!folder || folder.waiting || !window.FileExplorer || !window.FileExplorer.PopupMenu) return;
    const entry = entryFromRow(row);
    const onItem = !!entry || !!forceItems;
    const ids = feInstance.GetSelectedItemIDs();
    const modifiable = canModify(folder);
    const target = pasteTarget(onItem ? entry : null);
    const label = (text) => feInstance.EscapeHTML(text);
    const items = onItem
      ? [
          { id: "rename", name: label("Rename"), enabled: modifiable && ids.length === 1 },
          { id: "copy", name: label("Copy"), enabled: ids.length > 0 },
          { id: "cut", name: label("Cut"), enabled: ids.length > 0 && modifiable },
          { id: "paste", name: label("Paste"), enabled: target.allowed },
          "split",
          { id: "delete", name: label("Delete"), enabled: ids.length > 0 && modifiable },
          "split",
          { id: "download", name: label("Download"), enabled: ids.length > 0 }
        ]
      : [
          { id: "new-folder", name: label("New folder"), enabled: modifiable },
          { id: "paste", name: label("Paste"), enabled: target.allowed },
          "split",
          { id: "download-dir", name: label("Download this directory"), enabled: true }
        ];

    closeContextMenu();
    const main = feInstance.GetElements().mainwrap;
    const menu = new window.FileExplorer.PopupMenu(main, {
      items,
      onposition(popupelem) {
        const rect = main.getBoundingClientRect();
        const maxLeft = Math.max(0, main.clientWidth - popupelem.offsetWidth - 2);
        const maxTop = Math.max(0, main.clientHeight - popupelem.offsetHeight - 2);
        popupelem.style.left = Math.min(Math.max(0, clientX - rect.left), maxLeft) + "px";
        popupelem.style.top = Math.min(Math.max(0, clientY - rect.top), maxTop) + "px";
      },
      onselected(id) {
        if (handled) {
          activeMenu = null;
          this.Destroy();
          return;
        }
        handled = true;
        activeMenu = null;
        this.Destroy();
        runContextAction(id, folder, target);
      },
      oncancel() {
        activeMenu = null;
        this.Destroy();
      }
    });
    activeMenu = menu;
    let handled = false;
    const pop = main.querySelector(":scope > .fe_fileexplorer_popup_wrap");
    if (pop) {
      const under = document.elementFromPoint(clientX, clientY);
      const hovered = under && under.closest ? under.closest(".fe_fileexplorer_popup_item_wrap") : null;
      if (hovered) hovered.focus();
      const chooseItem = (e) => {
        if (handled || activeMenu !== menu) return;
        if (e.type === "mouseup" && e.button > 0) return;
        const item = e.target.closest && e.target.closest(".fe_fileexplorer_popup_item_wrap");
        if (!item || item.classList.contains("fe_fileexplorer_popup_item_disabled")) return;
        handled = true;
        activeMenu = null;
        menu.Destroy();
        runContextAction(item.dataset.itemid, folder, target);
      };
      pop.addEventListener("mouseup", chooseItem);
      pop.addEventListener("click", chooseItem);
    }
  }

  function fileListTarget(node) {
    if (!node || !node.closest || !container.contains(node)) return false;
    return !!node.closest(".fe_fileexplorer_items_scroll_wrap, .fe_fileexplorer_items_clipboard_overlay");
  }

  function bindContextMenu() {
    container.addEventListener("mousedown", (e) => {
      if (e.button !== 2 || !feInstance) return;
      if (!e.target.closest || !e.target.closest(".fe_fileexplorer_items_scroll_wrap")) return;
      e.stopPropagation();
      const row = e.target.closest(".fe_fileexplorer_item_wrap");
      contextRow = row;
      const folder = feInstance.GetCurrentFolder();
      if (!folder || folder.waiting) return;
      if (row && row.dataset.feid) {
        const id = row.dataset.feid;
        if (!feInstance.GetSelectedItemIDs().includes(id)) feInstance.SetSelectedItems([id]);
        feInstance.SetFocusItem(id, true);
      } else {
        feInstance.ClearSelectedItems(true);
        contextRow = null;
      }
    }, true);

    container.addEventListener("contextmenu", (e) => {
      if (!feInstance || !fileListTarget(e.target)) return;
      if (e.target.closest && e.target.closest(".fe_fileexplorer_textarea")) return;
      e.preventDefault();
      e.stopPropagation();
      hideClipboardOverlay();
      const direct = e.target.closest && e.target.closest(".fe_fileexplorer_item_wrap");
      const onOverlay = e.target.closest && e.target.closest(".fe_fileexplorer_items_clipboard_overlay");
      const row = direct || (onOverlay ? contextRow : null);
      openContextMenu(e.clientX, e.clientY, row);
    }, true);

    container.addEventListener("keydown", (e) => {
      if (!feInstance) return;
      const menuKey = e.key === "ContextMenu" || e.keyCode === 93 || (e.shiftKey && e.key === "F10");
      if (!menuKey || !fileListTarget(e.target)) return;
      if (e.target.closest && e.target.closest(".fe_fileexplorer_popup_wrap, .fe_fileexplorer_textarea")) return;
      e.preventDefault();
      e.stopPropagation();
      hideClipboardOverlay();
      const focused = feInstance.GetFocusedItem();
      const ids = feInstance.GetSelectedItemIDs();
      const row = focused && ids.includes(focused.dataset.feid) ? focused : null;
      const anchor = row || container.querySelector(".fe_fileexplorer_items_scroll_wrap");
      const rect = anchor ? anchor.getBoundingClientRect() : { left: e.clientX, top: e.clientY };
      openContextMenu(rect.left + 16, rect.top + 16, row, ids.length > 0);
    }, true);
  }

  let activeOverlay = null;

  function lockUI({ title = "Transferring files to MPC…", message = "Uploading…", onCancel = null } = {}) {
    const overlay = document.getElementById("ui-lock-overlay");
    const msgEl = document.getElementById("ui-lock-msg");
    const titleEl = document.getElementById("ui-lock-title");
    const cancelBtn = document.getElementById("ui-lock-cancel");
    const spinner = overlay ? overlay.querySelector(".ui-lock-spinner") : null;

    if (overlay) {
      if (titleEl) titleEl.textContent = title;
      if (msgEl) msgEl.textContent = message;
      if (spinner) spinner.style.display = "block";
      if (cancelBtn) {
        cancelBtn.style.display = onCancel ? "block" : "none";
        cancelBtn.onclick = onCancel;
      }
      overlay.removeAttribute("hidden");
    }

    const preventLeave = (e) => {
      e.preventDefault();
      e.returnValue = "";
    };
    window.addEventListener("beforeunload", preventLeave);

    const blockKey = (e) => {
      if (e.target !== cancelBtn) {
        e.preventDefault();
        e.stopPropagation();
      }
    };
    document.addEventListener("keydown", blockKey, true);

    const handle = {
      setMessage: (msg) => {
        if (msgEl) msgEl.textContent = msg;
      },
      unlock: (statusMsg = null) => {
        window.removeEventListener("beforeunload", preventLeave);
        document.removeEventListener("keydown", blockKey, true);
        if (statusMsg && msgEl) {
          msgEl.textContent = statusMsg;
          if (spinner) spinner.style.display = "none";
          if (cancelBtn) cancelBtn.style.display = "none";
          setTimeout(() => {
            if (overlay) overlay.setAttribute("hidden", "");
            activeOverlay = null;
          }, 1100);
        } else {
          if (overlay) overlay.setAttribute("hidden", "");
          activeOverlay = null;
        }
      }
    };

    activeOverlay = handle;
    return handle;
  }

  function initExplorer() {
    if (!window.FileExplorer || !container) return;
    if (feInstance) return;

    if (!window.FileExplorer._hasRegisteredUploadFolder) {
      window.FileExplorer._hasRegisteredUploadFolder = true;
      window.FileExplorer.RegisterTool(0, function (fe) {
        if (!fe.hasEventListener("init_upload") && !fe.settings.tools.upload) return;

        let enabled = false;
        let inputfield = null;

        const node = fe.AddToolbarButton(
          "fe_fileexplorer_folder_tool_upload_folder",
          fe.Translate("Upload folders")
        );
        node.title = "Upload folders";
        node.setAttribute("aria-label", "Upload folders");

        const uploadFileBtn = fe.GetElements().bodytoolbar.querySelector(".fe_fileexplorer_folder_tool_upload");
        if (uploadFileBtn && uploadFileBtn.nextSibling) {
          fe.GetElements().bodytoolbar.insertBefore(node, uploadFileBtn.nextSibling);
        }

        const RemoveInputField = function () {
          if (inputfield) {
            inputfield.removeEventListener("change", UploadFolders);
            if (inputfield.parentNode) inputfield.parentNode.removeChild(inputfield);
            inputfield = null;
          }
        };

        const UploadFolders = function () {
          const files = Array.from(inputfield.files || []);
          RemoveInputField();
          if (files.length === 0) return;

          if (fe.settings.onuploadfolder) {
            fe.settings.onuploadfolder(files, fe.GetCurrentFolder());
          }
        };

        const CreateInputField = function () {
          RemoveInputField();
          inputfield = document.createElement("input");
          inputfield.type = "file";
          inputfield.className = "fe_fileexplorer_hidden";
          inputfield.webkitdirectory = true;
          inputfield.setAttribute("webkitdirectory", "");
          inputfield.setAttribute("directory", "");
          inputfield.multiple = true;
          inputfield.addEventListener("change", UploadFolders);
          fe.GetElements().mainwrap.appendChild(inputfield);
        };

        const StartSelectFolder = function () {
          if (!enabled) return;
          if (!inputfield) CreateInputField();
          inputfield.click();
        };

        const ClickHandler = function (e) {
          if (!e.isTrusted) return;
          StartSelectFolder();
        };

        node.addEventListener("click", ClickHandler);

        const UpdateToolHandler = function (currfolder, attrs) {
          const prevenabled = enabled;
          enabled = !currfolder.waiting && (!("canmodify" in attrs) || attrs.canmodify);
          if (prevenabled !== enabled) {
            if (enabled) node.classList.remove("fe_fileexplorer_disabled");
            else node.classList.add("fe_fileexplorer_disabled");
            fe.ToolStateUpdated();
          }
        };

        fe.addEventListener("update_tool", UpdateToolHandler);

        const DestroyToolHandler = function () {
          RemoveInputField();
          node.removeEventListener("click", ClickHandler);
        };

        fe.addEventListener("destroy", DestroyToolHandler);
      });
    }

    if (!window.FileExplorer._hasRegisteredRefresh) {
      window.FileExplorer._hasRegisteredRefresh = true;
      window.FileExplorer.RegisterTool(0, function (fe) {
        let enabled = true;
        const node = fe.AddToolbarButton(
          "fe_fileexplorer_folder_tool_refresh",
          fe.Translate("Refresh (F5)")
        );
        node.title = "Refresh (F5)";
        node.setAttribute("aria-label", "Refresh");

        // Insert after upload folder button
        const uploadFolderBtn = fe.GetElements().bodytoolbar.querySelector(".fe_fileexplorer_folder_tool_upload_folder");
        if (uploadFolderBtn && uploadFolderBtn.nextSibling) {
          fe.GetElements().bodytoolbar.insertBefore(node, uploadFolderBtn.nextSibling);
        }

        const ClickHandler = function (e) {
          if (!e.isTrusted) return;
          triggerRefresh();
        };

        node.addEventListener("click", ClickHandler);

        const UpdateToolHandler = function (currfolder) {
          const prevenabled = enabled;
          enabled = !currfolder.waiting;
          if (prevenabled !== enabled) {
            if (enabled) node.classList.remove("fe_fileexplorer_disabled");
            else node.classList.add("fe_fileexplorer_disabled");
            fe.ToolStateUpdated();
          }
        };

        fe.addEventListener("update_tool", UpdateToolHandler);

        const DestroyToolHandler = function () {
          node.removeEventListener("click", ClickHandler);
        };

        fe.addEventListener("destroy", DestroyToolHandler);
      });
    }

    const options = {
      group: "mpc_boot_data",
      capturebrowser: true,
      initpath: [["", "Storage"]],
      langmap: {
        "Upload (Ctrl + U)": "Upload files",
        "Upload": "Upload files"
      },
      tools: {
        item_checkboxes: true
      },

      onrefresh: async function (folder, required) {
        const rel = getFolderRelPath(folder);
        try {
          const data = await fetchFiles(rel);
          showDisk(data);
          const newEntries = (data.entries || [])
            .filter((e) => showDotfiles || !isDotEntry(e.name))
            .map((e) => {
              const id = e.id || e.name;
              return {
                name: e.name,
                id,
                type: e.is_dir ? "folder" : "file",
                size: e.size,
                hash: id + "_" + (e.mtime || 0)
              };
            });
          if (this.IsMappedFolder(folder)) {
            folder.SetEntries(newEntries);
            folder.SetAttributes({ canmodify: rel.length > 0 });
          }
        } catch (err) {
          if (this.IsMappedFolder(folder)) {
            folder.SetEntries([]);
            if (!rel) folder.SetAttributes({ canmodify: false });
          }
          toast(err.message || "Failed to load directory");
        }
      },

      onrename: async function (renamed, folder, entry, newname) {
        const kind = entry.type === "folder" ? "folder" : "file";
        if (!confirm(`Rename the ${kind} "${entry.name}" to "${newname}"?`)) {
          renamed("Cancelled");
          return;
        }
        try {
          const rel = getFolderRelPath(folder);
          const itemPath = rel ? rel + "/" + entry.id : entry.id;
          await renameFile(itemPath, newname);
          entry.id = newname;
          entry.name = newname;
          entry.hash = newname + "_" + Date.now();
          renamed(entry);
          toast("Renamed to " + newname);
        } catch (err) {
          renamed(err.message || "Rename failed");
          toast(err.message || "Rename failed");
        }
      },

      onnewfolder: async function (created, folder) {
        try {
          const entry = await createNewFolderEntry(folder);
          created(entry);
          toast("Created folder " + entry.name);
        } catch (err) {
          created(err.message || "Failed to create folder");
          toast(err.message || "Failed to create folder");
        }
      },

      ondelete: async function (deleted, folder, ids, entries) {
        const count = entries.length;
        if (!confirm(`Delete ${describeSelection(entries)}? This cannot be undone.`)) {
          deleted("Cancelled");
          return;
        }
        try {
          const rel = getFolderRelPath(folder);
          const items = ids.map((id) => (rel ? rel + "/" + id : id));
          await deleteFiles(items);
          deleted(true);
          toast(`Deleted ${count} item(s)`);
          updateDiskStats();
        } catch (err) {
          deleted(err.message || "Delete failed");
          toast(err.message || "Delete failed");
        }
      },

      onmove: async function (moved, srcpath, srcids, destfolder) {
        try {
          const count = await transferItems("move", srcpath, srcids, getFolderRelPath(destfolder), destinationName(destfolder));
          if (count === "Cancelled") {
            moved("Cancelled");
            return;
          }
          moved(true);
          toast(`Moved ${count} item(s)`);
          triggerRefresh(true);
        } catch (err) {
          moved(err.message || "Move failed");
          toast(err.message || "Move failed");
        }
      },

      oncopy: async function (copied, srcpath, srcids, destfolder) {
        try {
          const count = await transferItems("copy", srcpath, srcids, getFolderRelPath(destfolder), destinationName(destfolder));
          if (count === "Cancelled") {
            copied("Cancelled");
            return;
          }
          copied(true);
          toast(`Copied ${count} item(s)`);
          triggerRefresh(true);
        } catch (err) {
          copied(err.message || "Copy failed");
          toast(err.message || "Copy failed");
        }
      },

      oninitupload: function (startupload, fileinfo) {
        const rel = getFolderRelPath(fileinfo.folder);
        const cleanFull = (fileinfo.fullPath || "").replace(/^\/+/, "");
        if (fileinfo.type === "dir") {
          const dirPath = rel ? (cleanFull ? rel + "/" + cleanFull : rel) : cleanFull;
          if (!dirPath) {
            startupload(true);
            return;
          }
          createFolder(dirPath)
            .then(() => startupload(true))
            .catch((e) => startupload(e.message || "Failed to create directory"));
        } else {
          const lastSlash = cleanFull.lastIndexOf("/");
          let targetDir = rel;
          let fileName = fileinfo.file.name;
          if (lastSlash >= 0) {
            const sub = cleanFull.slice(0, lastSlash);
            fileName = cleanFull.slice(lastSlash + 1);
            targetDir = rel ? rel + "/" + sub : sub;
          }

          const abortCtrl = new AbortController();
          const ui = lockUI({
            title: "Transferring file to MPC…",
            message: `Uploading "${fileName}"…`,
            onCancel: () => {
              abortCtrl.abort();
              ui.setMessage("Cancelling transfer…");
            }
          });

          uploadFile(targetDir, fileinfo.file, fileName, abortCtrl.signal)
            .then(() => {
              startupload(true);
              ui.unlock(`Uploaded "${fileName}"!`);
              toast(`Uploaded ${fileName}`);
              triggerRefresh(true);
            })
            .catch((e) => {
              const msg = abortCtrl.signal.aborted ? "Transfer cancelled" : (e.message || "Upload failed");
              ui.unlock(msg);
              startupload(msg);
              toast(msg);
            });
        }
      },

      oninitdownload: async function (startdownload, folder, ids, entries) {
        try {
          await downloadEntries(folder, ids, entries);
        } catch (err) {
          toast(err.message || "Download failed");
        }
      },

      onuploadfolder: async function (files, currentFolder) {
        if (!files || files.length === 0) return;

        const baseRel = getFolderRelPath(currentFolder);
        const abortCtrl = new AbortController();
        let isCancelled = false;

        const ui = lockUI({
          title: "Transferring folder to MPC…",
          message: `Uploading folder (0 / ${files.length} files)…`,
          onCancel: () => {
            isCancelled = true;
            abortCtrl.abort();
            ui.setMessage("Cancelling transfer…");
          }
        });

        const CONCURRENCY = 6;
        let nextIndex = 0;
        let done = 0;
        let failed = 0;

        async function worker() {
          while (nextIndex < files.length && !isCancelled) {
            const idx = nextIndex++;
            const file = files[idx];
            const relPath = file.webkitRelativePath || file.name;
            const clean = relPath.replace(/^\/+/, "");
            const lastSlash = clean.lastIndexOf("/");
            let destDir = baseRel;
            let fileName = file.name;

            if (lastSlash >= 0) {
              const sub = clean.slice(0, lastSlash);
              fileName = clean.slice(lastSlash + 1);
              destDir = baseRel ? baseRel + "/" + sub : sub;
            }

            try {
              await uploadFile(destDir, file, fileName, abortCtrl.signal);
              done++;
            } catch (err) {
              if (abortCtrl.signal.aborted || isCancelled) return;
              failed++;
            }

            if (!isCancelled) {
              ui.setMessage(`Uploading folder (${done + failed} / ${files.length} files)…`);
            }
          }
        }

        const pool = Array.from(
          { length: Math.min(CONCURRENCY, files.length) },
          () => worker()
        );

        await Promise.all(pool);

        if (isCancelled) {
          ui.unlock(`Transfer cancelled (${done} files transferred)`);
          toast(`Upload cancelled (${done} transferred)`);
        } else {
          const finalMsg = failed === 0
            ? `Transfer complete (${done} files)!`
            : `Transfer complete (${done} uploaded, ${failed} failed)`;
          ui.unlock(finalMsg);
          toast(finalMsg);
        }

        triggerRefresh(true);
      }
    };

    feInstance = new window.FileExplorer(container, options);
    bindContextMenu();

    const uploadBtn = container.querySelector(".fe_fileexplorer_folder_tool_upload");
    if (uploadBtn) {
      uploadBtn.title = "Upload files";
      uploadBtn.setAttribute("aria-label", "Upload files");
    }

    const elems = feInstance.GetElements ? feInstance.GetElements() : null;
    if (elems && elems.navtools && !elems.navtools.querySelector(".fe_fileexplorer_navtool_refresh")) {
      const btnNavRefresh = document.createElement("button");
      btnNavRefresh.type = "button";
      btnNavRefresh.className = "fe_fileexplorer_navtool_refresh";
      btnNavRefresh.title = "Refresh (F5)";
      btnNavRefresh.setAttribute("aria-label", "Refresh");
      btnNavRefresh.addEventListener("click", () => {
        triggerRefresh();
      });
      elems.navtools.appendChild(btnNavRefresh);
    }

    container.addEventListener("keydown", (e) => {
      if (e.key === "F5" || (e.key.toLowerCase() === "r" && (e.ctrlKey || e.metaKey))) {
        e.preventDefault();
        e.stopPropagation();
        triggerRefresh();
      }
    });
  }

  if (showDotfilesInput) {
    showDotfilesInput.addEventListener("change", () => {
      showDotfiles = showDotfilesInput.checked;
      triggerRefresh(true);
    });
  }

  if (btnZipAll) {
    btnZipAll.addEventListener("click", async () => {
      try {
        toast("Preparing full /data/UserContent backup as ZIP...");
        await exportZip([], `mpc-usercontent-backup-${new Date().toISOString().slice(0, 10)}.zip`);
        toast("Full backup download started!");
      } catch (err) {
        toast(err.message || "Backup failed");
      }
    });
  }

  // Load explorer
  initExplorer();
  updateDiskStats();

  return {
    refresh: () => {
      triggerRefresh(true);
    },
    show() {
      triggerRefresh(true);
    }
  };
}
