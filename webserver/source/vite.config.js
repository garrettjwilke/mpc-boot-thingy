import { defineConfig } from "vite";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { isZip, programError } from "./src/program.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const contentDir = path.resolve(here, "../content");
const liveTarget = "http://127.0.0.1:8080";

function slugOf(title) {
  let out = "";
  let dash = true;
  for (const ch of title) {
    let c = ch;
    if (c >= "A" && c <= "Z") c = c.toLowerCase();
    const alnum = (c >= "a" && c <= "z") || (c >= "0" && c <= "9");
    if (alnum) {
      if (out.length + 1 >= 80) return null;
      out += c;
      dash = false;
    } else if (!dash && out.length + 1 < 80) {
      out += "-";
      dash = true;
    }
  }
  if (out.endsWith("-")) out = out.slice(0, -1);
  if (!out || out.length > 64) return null;
  return out;
}

function oneLine(s) {
  return !s.includes("\n") && !s.includes("\r");
}

function safeComponent(s) {
  if (!s || s.length >= 200 || s === "." || s === ".." || s.includes("..")) return false;
  for (const ch of s) {
    const c = ch.charCodeAt(0);
    if (c < 0x20 || c === 0x7f || ch === "/" || ch === "\\") return false;
  }
  return true;
}

function sendJson(res, status, obj) {
  res.statusCode = status;
  res.setHeader("Content-Type", "application/json; charset=utf-8");
  res.end(JSON.stringify(obj) + "\n");
}

function readRaw(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

function mockState() {
  return {
    default_id: "stock",
    patches: [
      { id: "wide-screen.list", title: "Wide screen" },
      { id: "extra-pads.list", title: "Extra pads" },
      { id: "shift-reboot.list", title: "Reboot button" },
    ],
    apps: [
      { id: "stock", title: "Stock", run: "", args: "", stock: true, editable: false, patches: [] },
      { id: "app:lab-firmware", title: "Lab firmware", run: "mpcfw", args: "", stock: false, editable: false, patches: [] },
      {
        id: "entry:boot-1.entry",
        title: "Boot 1",
        run: "autorun/boot-edits/boot-1/launch.sh",
        args: "",
        stock: false,
        editable: true,
        patches: ["wide-screen.list"],
      },
    ],
    uname: "preview mpc",
    uptime: "up 1 minute",
    card: "/preview/sd",
    sd_total_bytes: 32 * 1024 * 1024 * 1024,
    sd_free_bytes: 8 * 1024 * 1024 * 1024,
    memory: "               total        used        free\nMem:             512         384         128\n",
    files: [
      { path: "Drums", is_dir: true, size: 0, mtime: Math.floor(Date.now() / 1000) - 3600 },
      { path: "Drums/Kick.wav", is_dir: false, size: 176400, mtime: Math.floor(Date.now() / 1000) - 1800 },
      { path: "Drums/Snare.wav", is_dir: false, size: 128000, mtime: Math.floor(Date.now() / 1000) - 1200 },
      { path: "Synths", is_dir: true, size: 0, mtime: Math.floor(Date.now() / 1000) - 7200 },
      { path: "Synths/Lead.wav", is_dir: false, size: 352800, mtime: Math.floor(Date.now() / 1000) - 2400 },
      { path: "Project-01.mpc", is_dir: false, size: 84200, mtime: Math.floor(Date.now() / 1000) - 600 },
      { path: "README.txt", is_dir: false, size: 256, mtime: Math.floor(Date.now() / 1000) - 300 },
    ],
  };
}

async function handleMock(req, res, url, state) {
  if (req.method === "GET" && url.pathname === "/api/status") {
    sendJson(res, 200, state);
    return;
  }
  if (req.method === "GET" && url.pathname === "/api/patches") {
    sendJson(res, 200, { patches: state.patches });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/boot-edits") {
    const raw = await readRaw(req);
    let body = {};
    try { body = JSON.parse(raw.toString("utf8") || "{}"); }
    catch (e) {
      sendJson(res, 400, { error: "missing boot entry" });
      return;
    }
    const title = typeof body.title === "string" ? body.title.trim() : "";
    const patches = Array.isArray(body.patches) ? body.patches : null;
    const id = typeof body.id === "string" ? body.id : "";
    if (!title || title.includes("\n") || title.length >= 64) {
      sendJson(res, 400, { error: "title must be one short line" });
      return;
    }
    if (!patches || patches.some((name) => !state.patches.some((patch) => patch.id === name))) {
      sendJson(res, 400, { error: "unknown patch" });
      return;
    }
    if (id) {
      const existing = state.apps.find((app) => app.id === id);
      if (!existing || !existing.editable) {
        sendJson(res, 400, { error: "that entry cannot be edited" });
        return;
      }
      existing.title = title;
      existing.patches = patches.slice();
      sendJson(res, 200, { ok: true, id });
      return;
    }
    let n = 1;
    while (state.apps.some((app) => app.id === "entry:boot-" + n + ".entry")) n += 1;
    const made = "entry:boot-" + n + ".entry";
    state.apps.push({
      id: made,
      title,
      run: "autorun/boot-edits/boot-" + n + "/launch.sh",
      args: "",
      stock: false,
      editable: true,
      patches: patches.slice(),
    });
    sendJson(res, 200, { ok: true, id: made });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/apps") {
    const body = await readRaw(req);
    const title = (url.searchParams.get("title") || "").trim();
    const args = (url.searchParams.get("args") || "").trim();
    let name = (url.searchParams.get("name") || "").trim();
    if (!body.length) {
      sendJson(res, 400, { error: "program body is empty" });
      return;
    }
    const zip = isZip(body, name);
    if (!zip) {
      const why = programError(body);
      if (why) {
        sendJson(res, 400, { error: why });
        return;
      }
    }
    if (!oneLine(title) || !title || title.length >= 64) {
      sendJson(res, 400, { error: "title must be one short line" });
      return;
    }
    if (args && (!oneLine(args) || args.length >= 256)) {
      sendJson(res, 400, { error: "args must be one short line" });
      return;
    }
    const slug = slugOf(title);
    if (!slug) {
      sendJson(res, 400, { error: "could not name the app folder" });
      return;
    }
    if (!name) name = slug;
    if (!safeComponent(name) || name.length >= 256 || (!zip && name === "boot.entry")) {
      sendJson(res, 400, { error: "executable name is not allowed" });
      return;
    }
    const id = "app:" + slug;
    const next = { id, title, run: zip ? "autorun/autorun.sh" : name, args, stock: false };
    const existing = state.apps.findIndex((app) => app.id === id);
    if (existing >= 0) state.apps[existing] = next;
    else state.apps.push(next);
    sendJson(res, 200, { ok: true, id });
    return;
  }
  if (req.method === "DELETE" && url.pathname === "/api/apps") {
    const id = url.searchParams.get("id") || "";
    const app = state.apps.find((item) => item.id === id);
    if (!app || app.stock) {
      sendJson(res, 400, { error: "that program cannot be deleted" });
      return;
    }
    state.apps = state.apps.filter((item) => item.id !== id);
    if (state.default_id === id) state.default_id = "stock";
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/default") {
    const id = (await readRaw(req)).toString("utf8").trim();
    if (!state.apps.some((app) => app.id === id)) {
      sendJson(res, 400, { error: "that id is not in the menu" });
      return;
    }
    state.default_id = id;
    sendJson(res, 200, { ok: true, id });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/boot-now") {
    const raw = (await readRaw(req)).toString("utf8").trim();
    let id = raw;
    try {
      const parsed = JSON.parse(raw);
      if (parsed && typeof parsed.id === "string") id = parsed.id;
    } catch (e) {}
    if (id !== "boot-thingy" && !state.apps.some((app) => app.id === id)) {
      sendJson(res, 400, { error: "that id is not in the menu" });
      return;
    }
    sendJson(res, 200, { ok: true, id });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/reboot") {
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "GET" && url.pathname === "/api/files/list") {
    const reqPath = (url.searchParams.get("path") || "").replace(/^\/+|\/+$/g, "");
    const parent = reqPath.includes("/") ? reqPath.slice(0, reqPath.lastIndexOf("/")) : "";
    const prefix = reqPath ? reqPath + "/" : "";
    const entries = [];
    const seen = new Set();
    for (const f of state.files) {
      if (prefix ? f.path.startsWith(prefix) : !f.path.includes("/")) {
        const sub = prefix ? f.path.slice(prefix.length) : f.path;
        const name = sub.split("/")[0];
        if (seen.has(name)) continue;
        seen.add(name);
        const isDir = sub.includes("/") || f.is_dir;
        entries.push({
          name,
          is_dir: isDir,
          size: isDir ? 0 : f.size,
          mtime: f.mtime
        });
      }
    }
    sendJson(res, 200, {
      path: reqPath,
      parent,
      free_bytes: 3.2 * 1024 * 1024 * 1024,
      total_bytes: 4 * 1024 * 1024 * 1024,
      entries
    });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/files/mkdir") {
    const raw = await readRaw(req);
    const body = JSON.parse(raw.toString("utf8") || "{}");
    const p = (body.path || "").replace(/^\/+|\/+$/g, "");
    if (!p) { sendJson(res, 400, { error: "bad path" }); return; }
    state.files.push({ path: p, is_dir: true, size: 0, mtime: Math.floor(Date.now() / 1000) });
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/files/rename") {
    const raw = await readRaw(req);
    const body = JSON.parse(raw.toString("utf8") || "{}");
    const oldP = (body.path || "").replace(/^\/+|\/+$/g, "");
    const newName = (body.name || "").trim();
    const parentDir = oldP.includes("/") ? oldP.slice(0, oldP.lastIndexOf("/")) : "";
    const newP = parentDir ? parentDir + "/" + newName : newName;
    for (const f of state.files) {
      if (f.path === oldP) f.path = newP;
      else if (f.path.startsWith(oldP + "/")) f.path = newP + f.path.slice(oldP.length);
    }
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/files/move") {
    const raw = await readRaw(req);
    const body = JSON.parse(raw.toString("utf8") || "{}");
    const items = body.items || [];
    const dest = (body.dest || "").replace(/^\/+|\/+$/g, "");
    for (const item of items) {
      const base = item.includes("/") ? item.slice(item.lastIndexOf("/") + 1) : item;
      const target = dest ? dest + "/" + base : base;
      for (const f of state.files) {
        if (f.path === item) f.path = target;
        else if (f.path.startsWith(item + "/")) f.path = target + f.path.slice(item.length);
      }
    }
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/files/copy") {
    const raw = await readRaw(req);
    const body = JSON.parse(raw.toString("utf8") || "{}");
    const items = body.items || [];
    const dest = (body.dest || "").replace(/^\/+|\/+$/g, "");
    const toAdd = [];
    for (const item of items) {
      const base = item.includes("/") ? item.slice(item.lastIndexOf("/") + 1) : item;
      const target = dest ? dest + "/" + base : base;
      for (const f of state.files) {
        if (f.path === item) toAdd.push({ ...f, path: target });
        else if (f.path.startsWith(item + "/")) toAdd.push({ ...f, path: target + f.path.slice(item.length) });
      }
    }
    state.files.push(...toAdd);
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/files/delete") {
    const raw = await readRaw(req);
    const body = JSON.parse(raw.toString("utf8") || "{}");
    const items = body.items || [];
    state.files = state.files.filter((f) => !items.some((it) => f.path === it || f.path.startsWith(it + "/")));
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "POST" && url.pathname === "/api/files/upload") {
    const raw = await readRaw(req);
    const dir = (url.searchParams.get("dir") || "").replace(/^\/+|\/+$/g, "");
    const name = url.searchParams.get("name") || "uploaded.bin";
    const target = dir ? dir + "/" + name : name;
    const parts = target.split("/");
    for (let i = 1; i < parts.length; i++) {
      const pdir = parts.slice(0, i).join("/");
      if (!state.files.some((f) => f.path === pdir)) {
        state.files.push({ path: pdir, is_dir: true, size: 0, mtime: Math.floor(Date.now() / 1000) });
      }
    }
    state.files = state.files.filter((f) => f.path !== target);
    state.files.push({
      path: target,
      is_dir: false,
      size: raw.length,
      mtime: Math.floor(Date.now() / 1000)
    });
    sendJson(res, 200, { ok: true });
    return;
  }
  if (req.method === "GET" && url.pathname === "/api/files/download") {
    const p = url.searchParams.get("path") || "";
    res.setHeader("Content-Type", "application/octet-stream");
    res.setHeader("Content-Disposition", `attachment; filename="${p.split("/").pop()}"`);
    res.end(Buffer.from("mock content for " + p));
    return;
  }
  if ((req.method === "GET" || req.method === "POST") && url.pathname === "/api/files/zip") {
    res.setHeader("Content-Type", "application/zip");
    res.setHeader("Content-Disposition", `attachment; filename="mpc-usercontent-backup.zip"`);
    // Minimal valid empty zip: 22-byte EOCD
    const emptyZip = Buffer.from([
      0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    ]);
    res.end(emptyZip);
    return;
  }
  sendJson(res, 404, { error: "not found" });
}

function mockApi() {
  const state = mockState();
  return {
    name: "mock-api",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const url = new URL(req.url || "/", "http://127.0.0.1");
        if (!url.pathname.startsWith("/api/")) return next();
        handleMock(req, res, url, state).catch((err) => {
          sendJson(res, 500, { error: err.message || "request failed" });
        });
      });
    },
  };
}

function rewriteOrigin(proxy) {
  const setOrigin = (proxyReq) => {
    proxyReq.setHeader("Origin", liveTarget);
  };
  proxy.on("proxyReq", setOrigin);
  proxy.on("proxyReqWs", setOrigin);
}

export default defineConfig(({ mode }) => {
  const live = mode === "live";
  return {
    plugins: [live ? null : mockApi()].filter(Boolean),
    server: live
      ? {
          proxy: {
            "/api": {
              target: liveTarget,
              changeOrigin: true,
              configure: rewriteOrigin,
            },
            "/ws": {
              target: liveTarget,
              ws: true,
              changeOrigin: true,
              configure: rewriteOrigin,
            },
          },
        }
      : {},
    build: {
      outDir: contentDir,
      emptyOutDir: true,
      cssCodeSplit: false,
      cssMinify: false,
      sourcemap: false,
    },
  };
});
