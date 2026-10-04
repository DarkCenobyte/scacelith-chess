// The home page: gallery viewer, trailer, the official server's status and leaderboard, and the
// files of the newest release.

import "./site.js";
import { onReady, releaseReady } from "./site.js";
import { $, $$, h, icon, clear } from "./lib/dom.js";
import { t, fmt, pageUrl } from "./lib/i18n.js";
import { api } from "./lib/api.js";
import { getSession, onSessionChange } from "./lib/session.js";
import { detectOS, filesFor, OSES } from "./lib/releases.js";
import { ratingText } from "./lib/ratings.js";

/* ---------------------------------------------------------------------------- gallery */

function initGallery() {
  const dialog = $("[data-lightbox]");
  const shots = $$("[data-shot]");
  if (!dialog || !shots.length || typeof dialog.showModal !== "function") return;
  const image = $("[data-lightbox-image]", dialog);
  const caption = $("[data-lightbox-caption]", dialog);
  const count = $("[data-lightbox-count]", dialog);
  let index = 0;
  let opener = null;

  const show = (i) => {
    index = (i + shots.length) % shots.length;
    const shot = shots[index];
    const thumb = shot.querySelector("img");
    image.src = shot.href;
    image.alt = thumb?.alt || "";
    caption.textContent = shot.querySelector(".shot-caption")?.textContent || "";
    count.textContent = `${index + 1} / ${shots.length}`;
  };
  shots.forEach((shot, i) => shot.addEventListener("click", (event) => {
    if (event.ctrlKey || event.metaKey || event.shiftKey || event.button !== 0) return;
    event.preventDefault();
    opener = shot;
    show(i);
    dialog.showModal();
  }));
  $("[data-lightbox-close]", dialog).addEventListener("click", () => dialog.close());
  $("[data-lightbox-prev]", dialog).addEventListener("click", () => show(index - 1));
  $("[data-lightbox-next]", dialog).addEventListener("click", () => show(index + 1));
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog || event.target.classList.contains("lightbox-frame")) dialog.close();
  });
  dialog.addEventListener("keydown", (event) => {
    const rtl = document.dir === "rtl";
    if (event.key === "ArrowRight") show(index + (rtl ? -1 : 1));
    else if (event.key === "ArrowLeft") show(index + (rtl ? 1 : -1));
  });
  let startX = null;
  dialog.addEventListener("touchstart", (event) => { startX = event.touches[0].clientX; }, { passive: true });
  dialog.addEventListener("touchend", (event) => {
    if (startX === null) return;
    const dx = event.changedTouches[0].clientX - startX;
    startX = null;
    if (Math.abs(dx) < 50) return;
    const forward = dx < 0 !== (document.dir === "rtl");
    show(index + (forward ? 1 : -1));
  });
  dialog.addEventListener("close", () => {
    image.removeAttribute("src");
    opener?.focus();
  });
}

/* ---------------------------------------------------------------------------- trailer */

/** The YouTube id of config.js's `trailer`: an id or any usual YouTube URL. */
export function youtubeId(value) {
  const v = String(value || "").trim();
  if (!v) return null;
  if (/^[\w-]{11}$/.test(v)) return v;
  try {
    const url = new URL(v);
    const host = url.hostname.replace(/^www\.|^m\./, "");
    if (host === "youtu.be") return /^[\w-]{11}$/.test(url.pathname.slice(1)) ? url.pathname.slice(1) : null;
    if (host === "youtube.com" || host === "youtube-nocookie.com" || host === "music.youtube.com") {
      const id = url.searchParams.get("v") || (url.pathname.match(/^\/(?:embed|shorts|live|v)\/([\w-]{11})/) || [])[1];
      return id && /^[\w-]{11}$/.test(id) ? id : null;
    }
  } catch {
    return null;
  }
  return null;
}

function initTrailer() {
  const box = $("[data-video]");
  if (!box) return;
  const config = window.SCACELITH || {};
  const id = youtubeId(config.trailer);
  const play = $("[data-video-play]", box);
  const soon = $("[data-video-soon]", box);
  const note = $("[data-video-note]");
  if (!id) return;
  soon.hidden = true;
  play.hidden = false;
  note.hidden = false;
  $("[data-video-external]").href = `https://www.youtube.com/watch?v=${id}`;
  const start = Number.parseInt(config.trailerStart, 10);
  play.addEventListener("click", () => {
    const params = new URLSearchParams({ autoplay: "1", rel: "0", modestbranding: "1", playsinline: "1", hl: document.documentElement.lang });
    if (Number.isFinite(start) && start > 0) params.set("start", String(start));
    const frame = h("iframe", {
      src: `https://www.youtube-nocookie.com/embed/${id}?${params}`,
      title: t("js.trailer.title"),
      allow: "accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share",
      allowfullscreen: true,
      referrerpolicy: "strict-origin-when-cross-origin",
    });
    clear(box);
    box.append(frame);
    frame.focus();
  }, { once: true });
}

/* ---------------------------------------------------------------------------- server and leaderboard */

async function initServer() {
  const card = $("[data-server-card]");
  if (!card) return;
  const status = $("[data-server-status]", card);
  const statusText = $("[data-server-status-text]", card);
  const select = $("[data-mini-category]", card);
  const board = $("[data-mini-board]", card);

  let info;
  try {
    info = await api("/info", { timeout: 12000 });
  } catch {
    status.classList.add("is-offline");
    statusText.textContent = t("js.server.offline");
    clear(board).append(h("li", { class: "placeholder" }, t("js.server.error")));
    return;
  }
  status.classList.add("is-online");
  statusText.textContent = t("js.server.online");
  for (const el of $$("[data-server-name]")) if (info.name) el.textContent = info.name;
  $("[data-server-registration]", card).textContent = t(info.registration === "open" ? "js.server.open" : "js.server.closed");
  const cats = (info.categories || []).map((c) => c.id);
  const chips = h("span", { class: "chips" }, cats.map((c) => h("span", { class: "chip" }, c)));
  clear($("[data-server-categories]", card)).append(cats.length ? chips : t("js.common.dash"));

  if (!cats.length) {
    clear(board).append(h("li", { class: "placeholder" }, t("js.server.empty")));
    return;
  }
  for (const c of cats) select.append(h("option", { value: c }, c));
  select.disabled = false;
  select.value = cats.includes("3+2") ? "3+2" : cats[0];

  const load = async () => {
    const category = select.value;
    clear(board).append(h("li", { class: "placeholder" }, t("js.common.loading")));
    try {
      const data = await api("/leaderboard", { query: { category, limit: 5 } });
      if (select.value !== category) return;
      clear(board);
      if (!data.players?.length) {
        board.append(h("li", { class: "placeholder" }, t("js.server.empty")));
        return;
      }
      for (const p of data.players) {
        board.append(h("li", null,
          h("span", { class: "rank" }, fmt.number(p.rank)),
          h("a", { href: pageUrl("players", { u: p.username }) }, p.username),
          h("span", { class: "rating" }, ratingText(p.rating, false))));
      }
    } catch {
      clear(board).append(h("li", { class: "placeholder" }, t("js.server.error")));
    }
  };
  select.addEventListener("change", load);
  load();
}

function initOnlineButtons() {
  const primary = $("[data-online-primary]");
  const secondary = $("[data-online-secondary]");
  if (!primary || !secondary) return;
  const render = (session) => {
    if (session) {
      primary.textContent = t("js.home.my_games");
      primary.href = pageUrl("account", null, "games");
      secondary.textContent = t("js.home.my_account");
      secondary.href = pageUrl("account");
    } else {
      primary.textContent = t("js.home.create_account");
      primary.href = pageUrl("account", null, "register");
      secondary.textContent = t("js.home.signin");
      secondary.href = pageUrl("account");
    }
  };
  render(getSession());
  onSessionChange(render);
}

/* ---------------------------------------------------------------------------- release */

const OS_LABEL = { windows: "Windows", macos: "macOS", linux: "Linux" };

function renderRelease(release) {
  const eyebrow = $("[data-release-eyebrow]");
  const name = $("[data-release-name]");
  const pre = $("[data-release-pre]");
  const date = $("[data-release-date]");
  const notes = $("[data-release-notes]");
  const sums = $("[data-sums-link]");
  const verify = $("[data-verify-command]");
  if (!release) {
    for (const el of $$("[data-platform]")) {
      const state = $("[data-platform-state]", el);
      state.textContent = "";
    }
    if (name) name.textContent = t("js.download.unreachable");
    return;
  }
  if (eyebrow) eyebrow.textContent = t(release.prerelease ? "js.download.eyebrow_beta" : "js.download.eyebrow_release", { version: release.version });
  if (name) name.textContent = release.name;
  if (pre) pre.hidden = !release.prerelease;
  if (date && release.published) {
    date.textContent = t("js.download.published", { date: fmt.date(release.published) });
  }
  if (notes) notes.href = release.url;
  if (sums && release.sums) sums.href = release.sums.url;

  const visitor = detectOS();
  let verifyFile = null;
  for (const os of OSES) {
    const card = $(`[data-platform="${os}"]`);
    if (!card) continue;
    const files = filesFor(release, os);
    const state = $("[data-platform-state]", card);
    const box = clear($("[data-platform-files]", card));
    card.classList.toggle("is-unavailable", !files.length);
    clear(state);
    if (files.length) {
      state.append(h("span", { class: "badge badge-ok" }, icon("check"), t("js.download.available")));
      if (os === visitor) state.append(" ", h("span", { class: "badge" }, t("js.download.your_system")));
      files.forEach((f, i) => {
        verifyFile ||= f.name;
        box.append(h("a", { class: `btn ${i === 0 ? "btn-gold" : "btn-ghost"} file-button`, href: f.url, rel: "noopener" },
          h("span", { class: "file-button-row" }, icon("download"), f.kind === "exe" ? t("js.download.exe") : f.kind === "zip" ? t("js.download.zip") : f.name),
          h("span", { class: "file-size" }, `.${f.kind} · ${fmt.bytes(f.size)}`)));
      });
    } else {
      state.append(h("span", { class: "badge badge-muted" }, t("js.download.unavailable")));
      box.append(h("span", null, t("js.download.no_build", { os: OS_LABEL[os] })));
    }
  }
  if (verify && verifyFile) {
    verify.textContent = `gh attestation verify ${verifyFile} --repo ${document.documentElement.dataset.repo}`;
  }
}

onReady(() => {
  initGallery();
  initTrailer();
  initServer();
  initOnlineButtons();
  releaseReady.then(renderRelease);
});
