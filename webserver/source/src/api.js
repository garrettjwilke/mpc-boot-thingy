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
