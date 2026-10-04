// The GIF page, opened in a new tab from a game list: asks the server to draw the game as an
// animated GIF (GET /games/:id/gif, signed in), shows it and downloads it.

import "./site.js";
import { onReady } from "./site.js";
import { $, h } from "./lib/dom.js";
import { t, fmt, pageUrl } from "./lib/i18n.js";
import { api, ApiError, fileNameOf } from "./lib/api.js";
import { requestSession } from "./lib/session.js";
import { errorMessage, setBusy, countdown } from "./lib/ui.js";

const params = new URLSearchParams(location.search);
const id = params.get("id") || "";
const baseTitle = document.title;
let objectUrl = null;
let downloaded = false;
let stopCountdown = null;

function status(text, { error = false, spinner = false, quiet = false } = {}) {
  const box = $("[data-gif-status]");
  box.hidden = false;
  box.classList.toggle("is-error", error);
  box.classList.toggle("visually-hidden", quiet);
  $("[data-gif-spinner]").hidden = !spinner;
  $("[data-gif-status-text]").textContent = text;
}

function options() {
  const form = $("[data-gif-options]");
  return {
    size: form.elements.size.value,
    orientation: form.elements.orientation.value,
    delay: form.elements.delay.value,
    coords: form.elements.coords.checked ? "1" : "0",
  };
}

async function describe() {
  try {
    const game = await api(`/games/${encodeURIComponent(id)}`, { auth: "optional" });
    const white = game.white?.name || t("js.game.white");
    const black = game.black?.name || t("js.game.black");
    $("[data-gif-title]").textContent = t("js.gif.title", { white, black });
    $("[data-gif-players]").textContent = t("js.gif.subtitle", { id: game.id, date: fmt.date(game.startedAt || game.endedAt) });
    document.title = `${white} – ${black} · ${baseTitle}`;
    if (!params.get("o") && game.you === "black") $("[data-gif-options]").elements.orientation.value = "black";
  } catch {
    $("[data-gif-players]").textContent = t("js.game.number", { id });
  }
}

async function render() {
  stopCountdown?.();
  const form = $("[data-gif-options]");
  const submit = $("button[type=submit]", form);
  const image = $("[data-gif-image]");
  const download = $("[data-gif-download]");
  setBusy(submit, true);
  image.hidden = true;
  download.hidden = true;
  status(t("js.gif.preparing"), { spinner: true });
  const slow = setTimeout(() => status(t("js.gif.rendering"), { spinner: true }), 1500);
  try {
    const res = await api(`/games/${encodeURIComponent(id)}/gif`, { auth: "required", query: options(), as: "response", timeout: 60000 });
    const blob = await res.blob();
    if (objectUrl) URL.revokeObjectURL(objectUrl);
    objectUrl = URL.createObjectURL(blob);
    const name = fileNameOf(res, `scacelith-${id}.gif`);
    image.src = objectUrl;
    image.alt = $("[data-gif-title]").textContent;
    image.hidden = false;
    download.href = objectUrl;
    download.download = name;
    download.hidden = false;
    if (!downloaded) {
      downloaded = true;
      download.click();
      status(t("js.gif.ready"), { quiet: true });
    } else {
      status(t("js.gif.ready_again"), { quiet: true });
    }
  } catch (e) {
    if (e instanceof ApiError && (e.code === "unauthorized" || e.code === "invalid_token")) {
      needSignIn();
      return;
    }
    const message = errorMessage(e);
    status(message, { error: true });
    if (e instanceof ApiError && e.retryAfter) {
      const text = $("[data-gif-status-text]");
      const render = e.code === "rate_limited"
        ? (time) => t("js.errors.rate_limited", { time })
        : (time) => `${message} ${t("js.gif.retry_in", { time })}`;
      stopCountdown = countdown(text, e.retryAfter, render, () => {
        text.textContent = t("js.gif.retry_now");
      });
    }
  } finally {
    clearTimeout(slow);
    setBusy(submit, false);
  }
}

function needSignIn() {
  status(t("js.gif.signin"), { error: true });
  const box = $("[data-gif-status]");
  box.append(h("a", { class: "btn btn-gold btn-small", href: pageUrl("account") }, t("js.gif.signin_button")));
  $("[data-gif-options]").closest("details").hidden = true;
}

onReady(async () => {
  const form = $("[data-gif-options]");
  if (params.get("o") === "black") form.elements.orientation.value = "black";
  if (["small", "medium", "large"].includes(params.get("size"))) form.elements.size.value = params.get("size");
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    render();
  });
  if (!/^\d{1,16}$/.test(id)) {
    status(t("js.gif.missing"), { error: true });
    form.closest("details").hidden = true;
    return;
  }
  $("[data-gif-players]").textContent = t("js.game.number", { id });
  api("/info").then((i) => {
    if (i?.name) $("[data-server-name]").textContent = i.name;
  }, () => null);
  const session = await requestSession();
  describe();
  if (!session) {
    needSignIn();
    return;
  }
  render();
});

window.addEventListener("pagehide", () => {
  if (objectUrl) URL.revokeObjectURL(objectUrl);
});
