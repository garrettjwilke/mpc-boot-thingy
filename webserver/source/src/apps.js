import { postDefault, removeApp } from "./api.js";

export function renderApps(apps, defaultId, { toast, refresh, onEdit }) {
  const appsEl = document.getElementById("apps");
  const hdrDefault = document.getElementById("hdr-default");
  appsEl.textContent = "";
  hdrDefault.textContent = "Default: " + (defaultId || "stock");
  hdrDefault.className = "badge ok";
  for (const app of apps) {
    const row = document.createElement("div");
    row.className = "app";
    const info = document.createElement("div");
    const title = document.createElement("strong");
    title.textContent = app.title || app.id;
    info.appendChild(title);
    if (app.id === defaultId) {
      const mark = document.createElement("span");
      mark.className = "badge ok";
      mark.textContent = "Default";
      info.appendChild(mark);
    }
    const meta = document.createElement("div");
    meta.className = "meta";
    meta.textContent = app.editable
      ? "boot entry" + (app.patches && app.patches.length ? "  " + app.patches.join(", ") : "")
      : app.stock ? app.id : (app.id + (app.run ? "  run " + app.run : "") + (app.args ? "  " + app.args : ""));
    info.appendChild(meta);
    const actions = document.createElement("div");
    actions.className = "row";
    if (app.id !== defaultId) {
      const setBtn = document.createElement("button");
      setBtn.className = "btn sm";
      setBtn.type = "button";
      setBtn.textContent = "Set default";
      setBtn.addEventListener("click", () => changeDefault(app, toast, refresh));
      actions.appendChild(setBtn);
    }
    if (app.editable && onEdit) {
      const editBtn = document.createElement("button");
      editBtn.className = "btn secondary sm";
      editBtn.type = "button";
      editBtn.textContent = "Edit";
      editBtn.addEventListener("click", () => onEdit(app));
      actions.appendChild(editBtn);
    }
    if (!app.stock) {
      const delBtn = document.createElement("button");
      delBtn.className = "btn secondary sm";
      delBtn.type = "button";
      delBtn.textContent = "Delete";
      delBtn.addEventListener("click", () => deleteApp(app, toast, refresh));
      actions.appendChild(delBtn);
    }
    row.appendChild(info);
    row.appendChild(actions);
    appsEl.appendChild(row);
  }
}

async function changeDefault(app, toast, refresh) {
  try {
    await postDefault(app.id);
    toast(app.title + " starts on the next boot if SHIFT is not held.");
    await refresh();
  } catch (e) {
    toast(e.message);
  }
}

async function deleteApp(app, toast, refresh) {
  if (!confirm("Delete " + app.title + "? This removes it from the card.")) return;
  try {
    await removeApp(app.id);
    toast("Removed " + app.title + ".");
    await refresh();
  } catch (e) {
    toast(e.message);
  }
}
