export function fmtBytes(n) {
  const v = Number(n) || 0;
  if (v < 1024) return v + " B";
  const units = ["KB", "MB", "GB", "TB"];
  let x = v / 1024, i = 0;
  while (x >= 1024 && i < units.length - 1) { x /= 1024; i++; }
  return x.toFixed(1) + " " + units[i];
}

export async function readBody(res) {
  const text = await res.text();
  let body = {};
  try { body = text ? JSON.parse(text) : {}; }
  catch (e) { body = { error: text || res.statusText }; }
  if (!res.ok) throw new Error(body.error || res.statusText || "request failed");
  return body;
}

export async function fetchStatus() {
  return readBody(await fetch("/api/status"));
}

export async function postDefault(id) {
  return readBody(await fetch("/api/default", {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: id
  }));
}

export async function bootNow(id) {
  return readBody(await fetch("/api/boot-now", {
    method: "POST",
    headers: { "Content-Type": "text/plain" },
    body: id
  }));
}

export async function removeApp(id) {
  return readBody(await fetch("/api/apps?id=" + encodeURIComponent(id), { method: "DELETE" }));
}

export async function installProgram({ title, args, name, file }) {
  const q = new URLSearchParams({ title, args, name });
  return readBody(await fetch("/api/apps?" + q.toString(), { method: "POST", body: file }));
}

export async function fetchPatches() {
  return readBody(await fetch("/api/patches"));
}

export async function saveBootEdit({ title, patches, id }) {
  const body = { title, patches };
  if (id) body.id = id;
  return readBody(await fetch("/api/boot-edits", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body)
  }));
}

export async function rebootDevice() {
  return readBody(await fetch("/api/reboot", { method: "POST" }));
}

export async function fetchFiles(path = "") {
  const q = path ? "?path=" + encodeURIComponent(path) : "";
  return readBody(await fetch("/api/files/list" + q));
}

export async function createFolder(path) {
  return readBody(await fetch("/api/files/mkdir", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path })
  }));
}

export async function renameFile(path, name) {
  return readBody(await fetch("/api/files/rename", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path, name })
  }));
}

export async function moveFiles(items, dest) {
  return readBody(await fetch("/api/files/move", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ items, dest })
  }));
}

export async function copyFiles(items, dest) {
  return readBody(await fetch("/api/files/copy", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ items, dest })
  }));
}

export async function deleteFiles(items) {
  return readBody(await fetch("/api/files/delete", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ items })
  }));
}

export async function uploadFile(dir, file, nameOverride = null) {
  const name = nameOverride || file.name;
  const q = new URLSearchParams({ dir, name });
  return readBody(await fetch("/api/files/upload?" + q.toString(), {
    method: "POST",
    body: file
  }));
}

export function downloadFileUrl(path) {
  return "/api/files/download?path=" + encodeURIComponent(path);
}

export async function exportZip(items = null, filename = "mpc-usercontent-backup.zip") {
  const body = items && items.length > 0 ? JSON.stringify({ items }) : "{}";
  const res = await fetch("/api/files/zip", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(err || "ZIP export failed");
  }
  const blob = await res.blob();
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => {
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }, 1000);
}
