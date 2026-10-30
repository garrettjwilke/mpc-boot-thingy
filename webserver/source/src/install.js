import { installProgram } from "./api.js";
import { isZip, programError } from "./program.js";

export function titleFromFilename(filename) {
  const base = String(filename || "").split(/[/\\]/).pop() || "";
  const dot = base.lastIndexOf(".");
  const stem = dot > 0 ? base.slice(0, dot) : "";
  return stem || base;
}

export function bindInstall({ toast, refresh }) {
  const fileInput = document.getElementById("file");
  const titleInput = document.getElementById("title");
  fileInput.addEventListener("change", () => {
    const file = fileInput.files && fileInput.files[0];
    if (!file) return;
    titleInput.value = titleFromFilename(file.name);
  });
  document.getElementById("install").addEventListener("click", async () => {
    const file = fileInput.files[0];
    const title = titleInput.value.trim();
    const args = document.getElementById("args").value.trim();
    if (!file || !title) {
      toast("Choose a file and a title.");
      return;
    }
    const head = new Uint8Array(await file.slice(0, 512).arrayBuffer());
    if (!isZip(head, file.name)) {
      const why = programError(head);
      if (why) {
        toast(why);
        return;
      }
    }
    const name = (file.name || "app").split(/[/\\]/).pop();
    try {
      await installProgram({ title, args, name, file });
      toast("Installed " + title + ".");
      fileInput.value = "";
      await refresh();
    } catch (e) {
      toast(e.message);
    }
  });
}
