// Shared interface pieces: messages, busy buttons, clipboard, file saving, menus and tabs.

import { $, $$, h, icon } from "./dom.js";
import { t, fmt, has } from "./i18n.js";
import { ApiError } from "./api.js";

/* ---------------------------------------------------------------------------- messages */

export function toast(message, { type = "info", timeout = 4500 } = {}) {
  const host = $("[data-toasts]");
  if (!host) return;
  const el = h("div", { class: `toast${type === "error" ? " is-error" : ""}`, role: type === "error" ? "alert" : "status" },
    icon(type === "error" ? "warning" : type === "ok" ? "check" : "info"), h("span", null, message));
  host.append(el);
  setTimeout(() => {
    el.classList.add("is-leaving");
    setTimeout(() => el.remove(), 300);
  }, timeout);
}

/** The sentence to show for an API error (docs: the `error` codes). */
export function errorMessage(error, extra = {}) {
  if (!(error instanceof ApiError)) {
    console.error(error);
    return t("js.errors.unknown", { code: error?.name || "error" });
  }
  const code = error.code;
  const wait = error.retryAfter ? fmt.wait(error.retryAfter) : null;
  switch (code) {
    case "network":
      return t("js.errors.network");
    case "timeout":
      return t("js.errors.timeout");
    case "rate_limited":
      return wait ? t("js.errors.rate_limited", { time: wait }) : t("js.errors.rate_limited_short");
    case "too_many_attempts":
      return t("js.errors.too_many_attempts", { time: wait || fmt.wait(60) });
    case "server_busy":
    case "busy":
      return t("js.errors.server_busy");
    case "banned":
      return error.body.until ? t("js.errors.banned", { date: fmt.dateTime(error.body.until) }) : t("js.errors.banned_permanent");
    case "weak_password": {
      const key = `js.errors.weak_${error.reason}`;
      return has(key) ? t(key, { min: extra.passwordMin ?? 10 }) : t("js.errors.weak_password");
    }
    case "invalid_game_id":
      return t("js.errors.invalid_game");
    case "not_found":
      return extra.notFound || t("js.errors.not_found");
    default:
      if (has(`js.errors.${code}`)) return t(`js.errors.${code}`, extra);
      if (error.status >= 500) return t("js.errors.internal_error");
      return t("js.errors.unknown", { code });
  }
}

export function showMessage(el, message) {
  if (!el) return;
  el.textContent = message || "";
  el.hidden = !message;
}

/**
 * Marks a button as working. It is not disabled, so that it keeps the keyboard focus: the
 * handlers check isBusy() and ignore a second press.
 */
export function setBusy(button, busy) {
  if (!button) return;
  button.classList.toggle("is-busy", busy);
  if (busy) button.setAttribute("aria-disabled", "true");
  else button.removeAttribute("aria-disabled");
  button.setAttribute("aria-busy", busy ? "true" : "false");
}

export function isBusy(button) {
  return Boolean(button?.classList.contains("is-busy"));
}

/* ---------------------------------------------------------------------------- files */

export function saveBlob(blob, fileName) {
  const url = URL.createObjectURL(blob);
  const a = h("a", { href: url, download: fileName, hidden: true });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
}

function legacyCopy(text) {
  const area = h("textarea", { readonly: true, class: "visually-hidden" });
  area.value = text;
  document.body.append(area);
  area.select();
  let ok = false;
  try {
    ok = document.execCommand("copy");
  } catch {
    ok = false;
  }
  area.remove();
  return ok;
}

/**
 * Copies text that may still be on its way (a Promise of a string). Safari only allows a
 * clipboard write during the click itself, which a ClipboardItem with a promise satisfies.
 */
export async function copyText(textOrPromise) {
  const promise = Promise.resolve(textOrPromise);
  if (navigator.clipboard?.write && typeof ClipboardItem === "function" && textOrPromise instanceof Promise) {
    try {
      await navigator.clipboard.write([
        new ClipboardItem({ "text/plain": promise.then((text) => new Blob([text], { type: "text/plain" })) }),
      ]);
      return true;
    } catch (e) {
      if (e instanceof ApiError) throw e;
      /* falls back below */
    }
  }
  const text = await promise;
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch {
      /* falls back below */
    }
  }
  return legacyCopy(text);
}

/* ---------------------------------------------------------------------------- menus */

export function initMenus(root = document) {
  const menus = $$("details[data-menu]", root);
  for (const menu of menus) {
    if (menu.dataset.ready) continue;
    menu.dataset.ready = "1";
    // A disclosure (the native <details>), not an ARIA menu: Tab moves through the links.
    const summary = menu.querySelector("summary");
    const sync = () => summary?.setAttribute("aria-expanded", menu.open ? "true" : "false");
    sync();
    menu.addEventListener("toggle", () => {
      sync();
      if (menu.open) for (const other of $$("details[data-menu][open]")) if (other !== menu) other.open = false;
    });
    menu.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && menu.open) {
        menu.open = false;
        summary?.focus();
      }
    });
    menu.addEventListener("click", (event) => {
      if (event.target.closest("a[href]")) menu.open = false;
    });
  }
  if (!document.documentElement.dataset.menusReady) {
    document.documentElement.dataset.menusReady = "1";
    document.addEventListener("click", (event) => {
      for (const menu of $$("details[data-menu][open]")) if (!menu.contains(event.target)) menu.open = false;
    });
    document.addEventListener("focusin", (event) => {
      for (const menu of $$("details[data-menu][open]")) if (!menu.contains(event.target)) menu.open = false;
    });
  }
}

/* ---------------------------------------------------------------------------- tabs */

/**
 * ARIA tabs: buttons [role=tab][data-tab] inside `list`, panels found by `panelFor(name)`.
 * Arrow keys, Home and End move between tabs. Returns select(name).
 */
export function initTabs(list, panelFor, onSelect) {
  const tabs = $$("[role=tab]", list);
  function select(name, { focus = false } = {}) {
    const tab = tabs.find((x) => x.dataset.tab === name) || tabs[0];
    for (const x of tabs) {
      const on = x === tab;
      x.setAttribute("aria-selected", on ? "true" : "false");
      x.tabIndex = on ? 0 : -1;
      const panel = panelFor(x.dataset.tab);
      if (panel) panel.hidden = !on;
    }
    if (focus) tab.focus();
    onSelect?.(tab.dataset.tab);
    return tab.dataset.tab;
  }
  list.addEventListener("click", (event) => {
    const tab = event.target.closest("[role=tab]");
    if (tab && list.contains(tab)) select(tab.dataset.tab);
  });
  list.addEventListener("keydown", (event) => {
    const i = tabs.indexOf(document.activeElement);
    if (i < 0) return;
    const rtl = document.dir === "rtl";
    let next = null;
    if (event.key === (rtl ? "ArrowLeft" : "ArrowRight")) next = tabs[(i + 1) % tabs.length];
    else if (event.key === (rtl ? "ArrowRight" : "ArrowLeft")) next = tabs[(i - 1 + tabs.length) % tabs.length];
    else if (event.key === "Home") next = tabs[0];
    else if (event.key === "End") next = tabs[tabs.length - 1];
    if (next) {
      event.preventDefault();
      select(next.dataset.tab, { focus: true });
    }
  });
  return select;
}

/* ---------------------------------------------------------------------------- forms */

export function initReveal(root = document) {
  for (const button of $$("[data-reveal]", root)) {
    button.addEventListener("click", () => {
      const input = button.parentElement.querySelector("input");
      const show = input.type === "password";
      input.type = show ? "text" : "password";
      // A toggle button keeps its name ("Show the password"); its pressed state says the rest.
      button.setAttribute("aria-pressed", show ? "true" : "false");
    });
  }
}

/** Marks a field as invalid with its message (or clears it). */
export function fieldError(input, message) {
  if (!input) return;
  if (message) {
    input.setAttribute("aria-invalid", "true");
  } else {
    input.removeAttribute("aria-invalid");
  }
}

/** A live "try again in N s" countdown inside `el`; calls done() when it reaches zero. */
export function countdown(el, seconds, render, done) {
  let left = Math.ceil(seconds);
  const tick = () => {
    if (!el.isConnected) return clearInterval(timer);
    if (left <= 0) {
      clearInterval(timer);
      done?.();
      return;
    }
    el.textContent = render(fmt.wait(left));
    left -= 1;
  };
  const timer = setInterval(tick, 1000);
  tick();
  return () => clearInterval(timer);
}
