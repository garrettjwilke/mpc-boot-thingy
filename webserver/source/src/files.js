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

  let feInstance = null;

  async function updateDiskStats() {
    try {
      const data = await fetchFiles("");
      if (elDisk && data.total_bytes) {
        elDisk.textContent = `${fmtBytes(data.free_bytes)} free of ${fmtBytes(data.total_bytes)}`;
      }
    } catch (e) {
      // Ignore initial stats error
    }
  }

  function getFolderRelPath(folder) {
    if (!folder) return "";
    const ids = folder.GetPathIDs ? folder.GetPathIDs() : [];
    return ids.filter(Boolean).join("/");
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

    const options = {
      group: "mpc_boot_data",
      capturebrowser: true,
      initpath: [["", "/data/UserContent"]],
      langmap: {
        "Upload (Ctrl + U)": "Upload files",
        "Upload": "Upload files"
      },
      tools: {
        item_checkboxes: true
      },

      onrefresh: async function (folder, required) {
        try {
          const rel = getFolderRelPath(folder);
          const data = await fetchFiles(rel);
          if (elDisk && data.total_bytes) {
            elDisk.textContent = `${fmtBytes(data.free_bytes)} free of ${fmtBytes(data.total_bytes)}`;
          }
          const newEntries = (data.entries || []).map((e) => ({
            name: e.name,
            id: e.name,
            type: e.is_dir ? "folder" : "file",
            size: e.size,
            hash: e.name + "_" + (e.mtime || 0)
          }));
          if (this.IsMappedFolder(folder)) {
            folder.SetEntries(newEntries);
          }
        } catch (err) {
          toast(err.message || "Failed to load directory");
        }
      },

      onrename: async function (renamed, folder, entry, newname) {
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
          const rel = getFolderRelPath(folder);
          let candidate = "New Folder";
          let n = 1;
          const existing = folder.GetEntries();
          while (existing.some((e) => e.name === candidate)) {
            candidate = `New Folder ${++n}`;
          }
          const targetPath = rel ? rel + "/" + candidate : candidate;
          await createFolder(targetPath);
          const entry = {
            name: candidate,
            id: candidate,
            type: "folder",
            hash: candidate + "_" + Date.now()
          };
          created(entry);
          toast("Created folder " + candidate);
        } catch (err) {
          created(err.message || "Failed to create folder");
          toast(err.message || "Failed to create folder");
        }
      },

      ondelete: async function (deleted, folder, ids, entries) {
        const count = entries.length;
        const msg = count === 1 ? `"${entries[0].name}"` : `${count} items`;
        if (!confirm(`Are you sure you want to permanently delete ${msg}? This cannot be undone.`)) {
          deleted("Cancelled deletion");
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
          const srcRel = srcpath.filter(Boolean).join("/");
          const destRel = getFolderRelPath(destfolder);
          const items = srcids.map((id) => (srcRel ? srcRel + "/" + id : id));
          await moveFiles(items, destRel);
          moved(true);
          toast(`Moved ${srcids.length} item(s)`);
          destfolder.Refresh();
        } catch (err) {
          moved(err.message || "Move failed");
          toast(err.message || "Move failed");
        }
      },

      oncopy: async function (copied, srcpath, srcids, destfolder) {
        try {
          const srcRel = srcpath.filter(Boolean).join("/");
          const destRel = getFolderRelPath(destfolder);
          const items = srcids.map((id) => (srcRel ? srcRel + "/" + id : id));
          await copyFiles(items, destRel);
          copied(true);
          toast(`Copied ${srcids.length} item(s)`);
          destfolder.Refresh();
          updateDiskStats();
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
          uploadFile(targetDir, fileinfo.file, fileName)
            .then(() => {
              startupload(true);
              toast(`Uploaded ${fileName}`);
              fileinfo.folder.Refresh();
              updateDiskStats();
            })
            .catch((e) => {
              startupload(e.message || "Upload failed");
              toast(e.message || "Upload failed");
            });
        }
      },

      oninitdownload: async function (startdownload, folder, ids, entries) {
        try {
          const rel = getFolderRelPath(folder);
          if (ids.length === 1 && entries.length === 1 && entries[0].type === "file") {
            const filePath = rel ? rel + "/" + ids[0] : ids[0];
            const a = document.createElement("a");
            a.href = downloadFileUrl(filePath);
            a.download = entries[0].name;
            document.body.appendChild(a);
            a.click();
            a.remove();
          } else {
            const items = ids.map((id) => (rel ? rel + "/" + id : id));
            const zipName = ids.length === 1
              ? `${entries[0].name}.zip`
              : `mpc-usercontent-export-${new Date().toISOString().slice(0, 10)}.zip`;
            toast("Preparing ZIP export...");
            await exportZip(items, zipName);
            toast("ZIP export complete");
          }
        } catch (err) {
          toast(err.message || "Download failed");
        }
      },

      onuploadfolder: async function (files, currentFolder) {
        const baseRel = getFolderRelPath(currentFolder);
        toast(`Uploading folder (${files.length} files)...`);
        let done = 0;
        let failed = 0;

        for (const file of files) {
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
            await uploadFile(destDir, file, fileName);
            done++;
          } catch (err) {
            failed++;
          }
        }

        if (failed === 0) {
          toast(`Uploaded folder (${done} files)`);
        } else {
          toast(`Uploaded ${done} files, ${failed} failed`);
        }

        if (currentFolder) currentFolder.Refresh();
        updateDiskStats();
      }
    };

    feInstance = new window.FileExplorer(container, options);

    const uploadBtn = container.querySelector(".fe_fileexplorer_folder_tool_upload");
    if (uploadBtn) {
      uploadBtn.title = "Upload files";
      uploadBtn.setAttribute("aria-label", "Upload files");
    }
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
      if (feInstance && feInstance.GetCurrentFolder()) {
        feInstance.GetCurrentFolder().Refresh();
      }
      updateDiskStats();
    }
  };
}
