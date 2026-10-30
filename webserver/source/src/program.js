export const PROGRAM_ERROR = "program must be an aarch64 Linux executable or a sh/bash script";

export function isZip(bytes, name) {
  const lower = String(name || "").toLowerCase();
  if (lower.endsWith(".zip")) return true;
  if (!bytes || bytes.length < 4) return false;
  return bytes[0] === 0x50 && bytes[1] === 0x4b && bytes[2] === 0x03 && bytes[3] === 0x04;
}

function shName(name) {
  return name === "sh" || name === "bash";
}

function baseName(path) {
  const slash = path.lastIndexOf("/");
  return slash >= 0 ? path.slice(slash + 1) : path;
}

function shellScript(bytes) {
  if (bytes.length < 3 || bytes[0] !== 0x23 || bytes[1] !== 0x21) return false;
  let i = 2;
  if (bytes[i] === 0x20) i++;
  let line = "";
  for (; i < bytes.length && bytes[i] !== 0x0a; i++) {
    const c = bytes[i];
    if (c === 0x0d || c === 0 || c < 0x20 || c > 0x7e) return false;
    line += String.fromCharCode(c);
    if (line.length > 128) return false;
  }
  line = line.trim();
  if (!line) return false;
  const parts = line.split(/ +/);
  const base = baseName(parts[0]);
  if (shName(base)) return true;
  return base === "env" && shName(parts[1] || "");
}

function aarch64Exec(bytes) {
  if (bytes.length < 32) return false;
  if (bytes[0] !== 0x7f || bytes[1] !== 0x45 || bytes[2] !== 0x4c || bytes[3] !== 0x46) return false;
  if (bytes[4] !== 2 || bytes[5] !== 1 || bytes[6] !== 1) return false;
  const type = bytes[16] | (bytes[17] << 8);
  const machine = bytes[18] | (bytes[19] << 8);
  if (machine !== 183 || (type !== 2 && type !== 3)) return false;
  let entry = 0n;
  for (let i = 0; i < 8; i++) entry |= BigInt(bytes[24 + i]) << BigInt(8 * i);
  return entry !== 0n;
}

export function programError(bytes) {
  const head = bytes.length > 512 ? bytes.subarray(0, 512) : bytes;
  if (aarch64Exec(head) || shellScript(head)) return "";
  return PROGRAM_ERROR;
}
