import { saveBootEdit } from "./api.js";

export function bindBootEdit({ toast, refresh }) {
  const form = document.getElementById("boot-form");
  const titleInput = document.getElementById("boot-title");
  const checks = document.getElementById("boot-patches");
  const modal = document.getElementById("unsaved");
  let catalog = [];
  let open = null;
  let pending = null;

  function selected() {
    return [...checks.querySelectorAll("input[type=checkbox]:checked")].map((box) => box.value).sort();
  }

  function snapshot() {
    return JSON.stringify({ title: titleInput.value, patches: selected() });
  }

  function dirty() {
    return !!(open && snapshot() !== open.base);
  }

  function renderChecks() {
    const on = new Set(open ? open.patches : []);
    checks.textContent = "";
    if (!catalog.length) {
      const empty = document.createElement("p");
      empty.className = "muted";
      empty.textContent = "No extra patches are on the card.";
      checks.appendChild(empty);
      return;
    }
    for (const patch of catalog) {
      const row = document.createElement("label");
      row.className = "patch";
      const box = document.createElement("input");
      box.type = "checkbox";
      box.value = patch.id;
      box.checked = on.has(patch.id);
      const name = document.createElement("span");
      name.textContent = patch.title || patch.id;
      row.appendChild(box);
      row.appendChild(name);
      checks.appendChild(row);
    }
  }

  function show(entry) {
    open = {
      id: entry && entry.id ? entry.id : "",
      patches: entry && entry.patches ? entry.patches.slice() : [],
      base: ""
    };
    titleInput.value = entry && entry.title ? entry.title : "";
    form.hidden = false;
    renderChecks();
    open.base = snapshot();
    titleInput.focus();
  }

  function hide() {
    open = null;
    pending = null;
    form.hidden = true;
    modal.hidden = true;
    titleInput.value = "";
    checks.textContent = "";
  }

  function applyPending() {
    const next = pending;
    pending = null;
    modal.hidden = true;
    if (!next || next.type === "close") {
      hide();
      return;
    }
    if (next.type === "new") show(null);
    else show(next.entry);
  }

  function ask(next) {
    pending = next;
    if (dirty()) {
      modal.hidden = false;
      return;
    }
    applyPending();
  }

  async function save() {
    const title = titleInput.value.trim();
    const patches = selected();
    if (!title) {
      toast("Give the entry a title.");
      return false;
    }
    try {
      await saveBootEdit({ title, patches, id: open && open.id });
      toast("Saved " + title + ".");
      hide();
      await refresh();
      return true;
    } catch (e) {
      toast(e.message);
      return false;
    }
  }

  document.getElementById("boot-new").addEventListener("click", () => ask({ type: "new" }));
  document.getElementById("boot-back").addEventListener("click", () => ask({ type: "close" }));
  document.getElementById("boot-save").addEventListener("click", () => { save(); });
  document.getElementById("unsaved-save").addEventListener("click", async () => {
    const next = pending;
    if (!await save()) return;
    pending = next;
    if (next && next.type !== "close") applyPending();
  });
  document.getElementById("unsaved-discard").addEventListener("click", () => applyPending());
  document.getElementById("unsaved-cancel").addEventListener("click", () => {
    pending = null;
    modal.hidden = true;
  });
  window.addEventListener("beforeunload", (ev) => {
    if (!dirty()) return;
    ev.preventDefault();
    ev.returnValue = "";
  });

  return {
    setCatalog(list) {
      const keep = open ? selected() : null;
      catalog = list || [];
      if (!open) return;
      open.patches = keep;
      renderChecks();
    },
    edit(entry) {
      ask({ type: "edit", entry });
    }
  };
}
