const KEY = "mpc-boot-theme";

function preferred() {
  const saved = localStorage.getItem(KEY);
  if (saved === "light" || saved === "dark") return saved;
  return window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
}

export function currentTheme() {
  return document.documentElement.dataset.theme === "light" ? "light" : "dark";
}

export function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  const btn = document.getElementById("theme");
  if (btn) btn.textContent = theme === "light" ? "Dark" : "Light";
  document.dispatchEvent(new Event("mpc-theme"));
}

export function bindTheme() {
  applyTheme(preferred());
  document.getElementById("theme").addEventListener("click", () => {
    const next = currentTheme() === "light" ? "dark" : "light";
    localStorage.setItem(KEY, next);
    applyTheme(next);
  });
  window.matchMedia("(prefers-color-scheme: light)").addEventListener("change", () => {
    if (localStorage.getItem(KEY)) return;
    applyTheme(preferred());
  });
}
