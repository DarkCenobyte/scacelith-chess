// Shared by every page: header, menus, language choice, account link and download menus.

import { $, $$, h, icon, clear } from "./lib/dom.js";
import { t, fmt, lang, pageUrl } from "./lib/i18n.js";
import * as store from "./lib/store.js";
import { getSession, onSessionChange, requestSession } from "./lib/session.js";
import { latestRelease, detectOS, filesFor, OSES, RELEASES_URL } from "./lib/releases.js";
import { initMenus, copyText, toast } from "./lib/ui.js";

const page = document.documentElement.dataset.page;

/* ---------------------------------------------------------------------------- header */

function initHeader() {
  const header = $("[data-header]");
  const nav = $("[data-nav]");
  const toggle = $("[data-nav-toggle]");
  if (!header) return;

  if (page === "home") {
    const update = () => header.classList.toggle("is-top", window.scrollY < 24);
    update();
    window.addEventListener("scroll", update, { passive: true });
  }

  const close = () => {
    nav.classList.remove("is-open");
    toggle?.setAttribute("aria-expanded", "false");
    document.body.classList.remove("nav-open");
  };
  toggle?.addEventListener("click", () => {
    const open = !nav.classList.contains("is-open");
    nav.classList.toggle("is-open", open);
    toggle.setAttribute("aria-expanded", open ? "true" : "false");
    document.body.classList.toggle("nav-open", open);
    if (open) nav.querySelector("a")?.focus();
  });
  nav?.addEventListener("click", (event) => {
    if (event.target.closest("a")) close();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && nav?.classList.contains("is-open")) {
      close();
      toggle?.focus();
    }
  });
  window.matchMedia("(min-width: 1181px)").addEventListener("change", (m) => m.matches && close());

  for (const link of $$("[data-nav-page]")) {
    if (link.dataset.navPage === page) link.setAttribute("aria-current", "page");
  }
  if (page === "home" && "IntersectionObserver" in window) {
    const links = new Map($$("[data-nav-section]").map((a) => [a.dataset.navSection, a]));
    const observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        const link = links.get(entry.target.id);
        if (!link) continue;
        if (entry.isIntersecting) {
          for (const other of links.values()) other.removeAttribute("aria-current");
          link.setAttribute("aria-current", "true");
        }
      }
    }, { rootMargin: "-45% 0px -50% 0px" });
    for (const id of links.keys()) {
      const section = document.getElementById(id);
      if (section) observer.observe(section);
    }
  }
}

/* ---------------------------------------------------------------------------- language */

function initLanguage() {
  for (const link of $$("a[data-lang]")) {
    link.addEventListener("click", () => {
      store.set("scacelith.lang", link.dataset.lang);
      if (location.hash && !link.hash) link.hash = location.hash;
      if (location.search && !link.search && page !== "gif") link.search = location.search;
    });
  }
  store.set("scacelith.lang", lang);
}

/* ---------------------------------------------------------------------------- account link */

function initAccountLink() {
  const render = (session) => {
    for (const label of $$("[data-account-label]")) {
      label.textContent = session?.user?.username || t("js.account.signin_label");
    }
    for (const link of $$("[data-account-link]")) {
      link.classList.toggle("is-signed-in", Boolean(session));
      link.title = session ? session.user.username : "";
    }
  };
  render(getSession());
  onSessionChange(render);
  // A tab opened by hand shares the sign-in of another open tab of the site, as a session
  // cookie would.
  if (!getSession()) requestSession(350).then((s) => s && render(s));
}

/* ---------------------------------------------------------------------------- downloads */

const OS_LABEL = { windows: "Windows", macos: "macOS", linux: "Linux" };
let resolveRelease;
/** Resolves with the newest release once the download menus have it, or with null. */
export const releaseReady = new Promise((resolve) => {
  resolveRelease = resolve;
});
const OS_ICON = { windows: "windows", macos: "apple", linux: "linux" };

function fileLabel(asset) {
  if (asset.kind === "exe") return `${t("js.download.exe")} (.exe)`;
  if (asset.kind === "zip") return `${t("js.download.zip")} (.zip)`;
  if (asset.kind === "file") return asset.name;
  return `${t("js.download.file")} (.${asset.kind})`;
}

function renderDownloadMenus(release) {
  const visitor = detectOS();
  const order = [...OSES].sort((a, b) => (a === visitor ? -1 : b === visitor ? 1 : 0));
  for (const menu of $$("[data-download-menu]")) {
    const list = $("[data-os-list]", menu);
    clear(list);
    for (const os of order) {
      const files = filesFor(release, os);
      const current = os === visitor;
      const head = h("div", { class: "os-group-head" },
        icon(OS_ICON[os], "os-icon"),
        h("span", { class: "os-text" },
          h("span", { class: "os-name", dataset: { current: t("js.download.your_system") } }, OS_LABEL[os]),
          h("span", { class: "os-meta" }, files.length ? (files[0].arch === "x64" ? t("js.download.x64") : t("js.download.available")) : t("js.download.unavailable"))));
      const group = h("li", { class: `os-group${files.length ? "" : " is-unavailable"}${current ? " is-current" : ""}` }, head);
      if (files.length) {
        group.append(h("ul", { class: "os-files" }, files.map((f) =>
          h("li", null, h("a", { class: "os-file", href: f.url, rel: "noopener" },
            h("span", { class: "os-file-name" }, icon("download"), fileLabel(f)),
            h("span", { class: "os-file-size" }, fmt.bytes(f.size)))))));
      }
      list.append(group);
    }
    const label = $("[data-release-label]", menu);
    if (label) {
      clear(label).append(h("bdi", null, release.name), release.prerelease ? ` · ${t("js.download.prerelease_short")}` : "");
    }
    const link = $("[data-release-link]", menu);
    if (link) link.href = RELEASES_URL;
  }
  resolveRelease(release);
}

function renderDownloadFallback() {
  for (const menu of $$("[data-download-menu]")) {
    const label = $("[data-release-label]", menu);
    if (label) label.textContent = t("js.download.unreachable");
  }
  resolveRelease(null);
}

function initDownloads() {
  const menus = $$("[data-download-menu]");
  if (!menus.length) return;
  let started = false;
  const start = () => {
    if (started) return;
    started = true;
    latestRelease().then(renderDownloadMenus, renderDownloadFallback);
  };
  // The home page needs the release at once (hero and download section); elsewhere, on demand.
  if (page === "home") start();
  for (const menu of menus) {
    menu.addEventListener("toggle", start);
    menu.addEventListener("pointerenter", start, { once: true });
    menu.addEventListener("focusin", start, { once: true });
  }
}

/* ---------------------------------------------------------------------------- copy buttons */

function initCopyButtons() {
  for (const button of $$("[data-copy-target]")) {
    button.addEventListener("click", async () => {
      const target = $(button.dataset.copyTarget);
      if (!target) return;
      const ok = await copyText(target.textContent.trim());
      toast(ok ? t("js.common.copied") : t("js.common.copy_failed"), { type: ok ? "ok" : "error", timeout: 2500 });
    });
  }
}

export function onReady(fn) {
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", fn, { once: true });
  else fn();
}

onReady(() => {
  initHeader();
  initMenus();
  initLanguage();
  initAccountLink();
  initDownloads();
  initCopyButtons();
});

export { pageUrl };
