// The leaderboard of the official server and the public profiles of its players.

import "./site.js";
import { onReady } from "./site.js";
import { $, h, clear } from "./lib/dom.js";
import { t, fmt, pageUrl } from "./lib/i18n.js";
import { api, ApiError } from "./lib/api.js";
import { getSession } from "./lib/session.js";
import { errorMessage, showMessage } from "./lib/ui.js";
import { renderRatings, ratingText } from "./lib/ratings.js";
import { gameList } from "./lib/games.js";

const USERNAME = /^[A-Za-z0-9_.-]{2,24}$/;
const baseTitle = document.title;
let categories = [];
let infoPromise = null;

function info() {
  infoPromise ||= api("/info").then((i) => {
    categories = (i.categories || []).map((c) => c.id);
    for (const el of document.querySelectorAll("[data-server-name]")) if (i.name) el.textContent = i.name;
    return i;
  }).catch((e) => {
    infoPromise = null;
    throw e;
  });
  return infoPromise;
}

/* ---------------------------------------------------------------------------- leaderboard */

let boardToken = 0;

async function showBoard(category) {
  const tabs = $("[data-board-tabs]");
  const body = $("[data-board-body]");
  const note = $("[data-board-note]");
  const my = ++boardToken;
  clear(body).append(h("tr", null, h("td", { colspan: 6, class: "placeholder" }, t("js.common.loading"))));
  try {
    await info();
  } catch (e) {
    clear(body).append(h("tr", null, h("td", { colspan: 6, class: "placeholder" }, errorMessage(e))));
    return;
  }
  if (!categories.length) {
    clear(body).append(h("tr", null, h("td", { colspan: 6, class: "placeholder" }, t("js.server.empty"))));
    return;
  }
  if (!categories.includes(category)) category = categories.includes("3+2") ? "3+2" : categories[0];
  if (!tabs.children.length) {
    for (const c of categories) {
      const b = h("button", { type: "button", "aria-pressed": "false", dataset: { category: c } }, c);
      b.addEventListener("click", () => {
        const url = new URL(location.href);
        url.searchParams.set("c", c);
        history.replaceState(null, "", url);
        showBoard(c);
      });
      tabs.append(b);
    }
  }
  for (const b of tabs.children) {
    b.setAttribute("aria-pressed", b.dataset.category === category ? "true" : "false");
  }
  $("[data-board-caption]").textContent = `${t("js.players.board")} ${category}`;
  try {
    const data = await api("/leaderboard", { query: { category, limit: 100 } });
    if (my !== boardToken) return;
    clear(body);
    note.textContent = [
      data.minGames ? t("js.server.min_games", { count: data.minGames }) : "",
      data.updatedAt ? t("js.server.updated", { time: fmt.relative(data.updatedAt) }) : "",
    ].filter(Boolean).join(" ");
    if (!data.players?.length) {
      body.append(h("tr", null, h("td", { colspan: 6, class: "placeholder" }, t("js.server.empty"))));
      return;
    }
    const me = getSession()?.user?.username?.toLowerCase();
    for (const p of data.players) {
      const link = h("a", { href: pageUrl("players", { u: p.username }) }, p.username);
      link.addEventListener("click", (event) => {
        if (event.ctrlKey || event.metaKey || event.shiftKey || event.button !== 0) return;
        event.preventDefault();
        openProfile(p.username, true);
      });
      body.append(h("tr", { class: p.rank <= 3 ? `rank-${p.rank}` : null },
        h("td", { class: "num rank-cell" }, fmt.number(p.rank)),
        h("td", null, link, me && p.username.toLowerCase() === me ? h("span", { class: "me-tag" }, ` ${t("js.players.you")}`) : null),
        h("td", { class: "num" }, ratingText(p.rating, false)),
        h("td", { class: "num hide-sm" }, fmt.rating(p.peak ?? p.rating)),
        h("td", { class: "num hide-sm" }, fmt.number(p.games)),
        h("td", { class: "num wdl-col" }, `${fmt.number(p.wins)} / ${fmt.number(p.draws)} / ${fmt.number(p.losses)}`)));
    }
  } catch (e) {
    if (my !== boardToken) return;
    note.textContent = "";
    clear(body).append(h("tr", null, h("td", { colspan: 6, class: "placeholder" }, errorMessage(e))));
  }
}

/* ---------------------------------------------------------------------------- profiles */

let profileToken = 0;

async function openProfile(username, push) {
  const panel = $("[data-profile]");
  const board = $("[data-leaderboard]");
  const error = $("[data-search-error]");
  showMessage(error, "");
  if (push) {
    const url = new URL(location.href);
    url.searchParams.set("u", username);
    history.pushState({ u: username }, "", url);
  }
  const my = ++profileToken;
  panel.hidden = false;
  board.hidden = true;
  $("[data-profile-name]").textContent = username;
  $("[data-profile-initial]").textContent = username.slice(0, 1);
  $("[data-profile-since]").textContent = t("js.common.loading");
  clear($("[data-profile-stats]"));
  clear($("[data-profile-ratings]"));
  clear($("[data-profile-games]"));
  document.title = `${username} — ${baseTitle}`;

  let profile;
  try {
    [profile] = await Promise.all([api(`/players/${encodeURIComponent(username)}`, { auth: "optional" }), info().catch(() => null)]);
  } catch (e) {
    if (my !== profileToken) return;
    panel.hidden = true;
    board.hidden = false;
    document.title = baseTitle;
    const notFound = e instanceof ApiError && e.status === 404;
    showMessage(error, notFound ? t("js.errors.player_not_found") : errorMessage(e));
    $("#player-query").value = username;
    return;
  }
  if (my !== profileToken) return;
  const name = profile.username;
  $("[data-profile-name]").textContent = name;
  $("[data-profile-initial]").textContent = name.slice(0, 1);
  $("[data-profile-since]").textContent = t("js.players.since", { date: fmt.date(profile.createdAt) });
  const g = profile.games || {};
  const stats = $("[data-profile-stats]");
  for (const [key, value] of [["games", g.total], ["rated", g.rated], ["wins", g.wins], ["draws", g.draws], ["losses", g.losses]]) {
    stats.append(h("div", null, h("dt", null, t(`js.players.${key}`)), h("dd", null, fmt.number(value || 0))));
  }
  renderRatings($("[data-profile-ratings]"), profile.ratings, { order: categories });
  gameList($("[data-profile-games]"), (before) =>
    api(`/players/${encodeURIComponent(name)}/games`, { auth: "optional", query: { before, limit: 20 } }), {
    me: name,
    linkPlayers: true,
    empty: t("js.game.none_public"),
  });
  panel.focus?.();
}

function closeProfile(push) {
  profileToken += 1;
  $("[data-profile]").hidden = true;
  $("[data-leaderboard]").hidden = false;
  document.title = baseTitle;
  if (push) {
    const url = new URL(location.href);
    url.searchParams.delete("u");
    history.pushState(null, "", url);
  }
}

function route() {
  const params = new URLSearchParams(location.search);
  const u = params.get("u");
  if (u && USERNAME.test(u)) openProfile(u, false);
  else closeProfile(false);
  showBoard(params.get("c") || "");
}

onReady(() => {
  const form = $("[data-player-search]");
  const input = $("#player-query");
  const error = $("[data-search-error]");
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    const name = input.value.trim();
    if (!USERNAME.test(name)) {
      showMessage(error, t("js.errors.invalid_player"));
      input.setAttribute("aria-invalid", "true");
      input.focus();
      return;
    }
    input.removeAttribute("aria-invalid");
    openProfile(name, true);
  });
  input.addEventListener("input", () => {
    input.removeAttribute("aria-invalid");
    showMessage(error, "");
  });
  $("[data-back-to-board]").addEventListener("click", (event) => {
    event.preventDefault();
    closeProfile(true);
    $("[data-leaderboard]").scrollIntoView({ block: "start" });
  });
  window.addEventListener("popstate", route);
  const profile = $("[data-profile]");
  profile.tabIndex = -1;
  route();
});
