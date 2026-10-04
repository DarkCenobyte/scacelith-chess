// Game lists: who had White and Black, who won, how the game ended, and the PGN and GIF of each.

import { h, icon, clear } from "./dom.js";
import { t, fmt, pageUrl } from "./i18n.js";
import { api, ApiError } from "./api.js";
import { getSession } from "./session.js";
import { toast, errorMessage, saveBlob, copyText } from "./ui.js";

const pgnCache = new Map();

/** The PGN text of a game (kept for the page's lifetime: copying twice asks once). */
export function fetchPgn(id) {
  const key = String(id);
  if (!pgnCache.has(key)) {
    const p = api(`/games/${encodeURIComponent(key)}/pgn`, { auth: "optional", as: "text" });
    pgnCache.set(key, p);
    p.catch(() => pgnCache.delete(key));
  }
  return pgnCache.get(key);
}

/** The outcome from one side: win, loss, draw or aborted. */
export function outcomeFor(game, color) {
  if (game.outcome) return game.outcome;
  if (game.status === 4 || game.result === "*") return "aborted";
  if (game.status === 3 || game.result === "1/2-1/2") return "draw";
  const whiteWon = game.status === 1 || game.result === "1-0";
  if (!color) return whiteWon ? "white" : "black";
  return (color === "white") === whiteWon ? "win" : "loss";
}

export function reasonText(game) {
  const key = `js.game.reason.${game.reason}`;
  const text = t(key);
  return text === key ? game.termination || t("js.game.termination_unknown") : text;
}

function scores(game) {
  if (game.status === 4 || game.result === "*") return ["–", "–"];
  if (game.status === 3 || game.result === "1/2-1/2") return ["½", "½"];
  return game.status === 1 || game.result === "1-0" ? ["1", "0"] : ["0", "1"];
}

function isDeleted(name) {
  return /^deleted#\d+$/.test(String(name || ""));
}

function playerLine(game, side, { me, owner, linkPlayers }) {
  const p = game[side] || {};
  const [ws, bs] = scores(game);
  const score = side === "white" ? ws : bs;
  const same = (a, b) => Boolean(a && b && a.toLowerCase() === b.toLowerCase());
  const isMe = same(p.name, me);
  const isOwner = isMe || same(p.name, owner);
  const name = p.name || t("js.common.dash");
  const nameEl = linkPlayers && p.name && !isDeleted(p.name)
    ? h("a", { href: pageUrl("players", { u: p.name }), title: t("js.players.view", { name: p.name }) }, name)
    : h("span", { class: "name" }, name);
  let rating = null;
  if (p.rating !== null && p.rating !== undefined) {
    const diff = p.ratingDiff;
    rating = h("span", { class: "player-rating" },
      fmt.rating(p.rating),
      diff !== null && diff !== undefined
        ? h("span", { class: diff > 0 ? "diff-up" : diff < 0 ? "diff-down" : "", title: t("js.game.rating_diff", { diff: fmt.signed(diff) }) },
          ` ${diff === 0 ? "±0" : fmt.signed(diff)}`)
        : null);
  }
  return h("div", {
    class: `player-line is-${side}${isOwner ? " is-me" : ""}${score === "1" ? " is-winner" : ""}`,
  },
  h("span", { class: "player-piece", "aria-hidden": "true" }, h("span", { class: "piece" }, "♚")),
  h("span", { class: "player-name" },
    h("span", { class: "visually-hidden" }, `${t(`js.game.${side}`)}: `),
    nameEl,
    isMe ? h("span", { class: "me-tag" }, t("js.game.you")) : null),
  rating || h("span", { class: "player-rating" }),
  h("span", { class: "player-score" }, score));
}

function gameRow(game, opts) {
  const side = opts.owner || opts.me;
  const color = game.color || (side && game.white?.name?.toLowerCase() === side.toLowerCase() ? "white"
    : side && game.black?.name?.toLowerCase() === side.toLowerCase() ? "black" : null);
  const outcome = outcomeFor(game, color);
  let outcomeLabel;
  let outcomeClass = outcome;
  if (outcome === "white" || outcome === "black") {
    outcomeLabel = t(outcome === "white" ? "js.game.result_white" : "js.game.result_black");
    outcomeClass = "neutral";
  } else {
    outcomeLabel = t(`js.game.outcome_${outcome}`);
  }
  const plies = game.plies || 0;
  const moves = Math.ceil(plies / 2);
  const custom = game.category === "custom";
  const category = custom ? game.timeControl || "" : game.category || game.timeControl;
  const ended = game.endedAt || game.startedAt;
  const duration = game.startedAt && game.endedAt ? game.endedAt - game.startedAt : null;

  const actions = h("div", { class: "game-actions" });
  const pgnHref = `${document.documentElement.dataset.api}/games/${game.id}/pgn`;
  const pgnLink = h("a", { class: "action", href: pgnHref, title: t("js.game.pgn_title"), download: `scacelith-${game.id}.pgn` },
    icon("download"), h("span", null, t("js.game.pgn")));
  pgnLink.addEventListener("click", async (event) => {
    if (event.ctrlKey || event.metaKey || event.shiftKey || event.button !== 0) return;
    event.preventDefault();
    pgnLink.classList.add("is-busy");
    try {
      const text = await fetchPgn(game.id);
      saveBlob(new Blob([text], { type: "application/x-chess-pgn;charset=utf-8" }), `scacelith-${game.id}.pgn`);
      toast(t("js.game.pgn_saved"), { type: "ok", timeout: 2500 });
    } catch (e) {
      toast(errorMessage(e), { type: "error" });
    } finally {
      pgnLink.classList.remove("is-busy");
    }
  });
  const copyButton = h("button", { class: "action", type: "button", title: t("js.game.copy_pgn_title") },
    icon("copy"), h("span", null, t("js.game.copy_pgn")));
  copyButton.addEventListener("click", async () => {
    copyButton.classList.add("is-busy");
    try {
      const ok = await copyText(fetchPgn(game.id));
      if (!ok) throw new Error("copy");
      copyButton.classList.add("is-done");
      toast(t("js.game.pgn_copied"), { type: "ok", timeout: 2500 });
      setTimeout(() => copyButton.classList.remove("is-done"), 2000);
    } catch (e) {
      toast(e instanceof ApiError ? errorMessage(e) : t("js.common.copy_failed"), { type: "error" });
    } finally {
      copyButton.classList.remove("is-busy");
    }
  });
  actions.append(pgnLink, copyButton);
  if (getSession()) {
    const gif = h("a", {
      class: "action",
      href: pageUrl("gif", { id: game.id, o: color === "black" ? "black" : null }),
      target: "_blank",
      rel: "opener",
      title: t("js.game.gif_title"),
    }, icon("film"), h("span", null, t("js.game.gif")));
    actions.append(gif);
  } else {
    actions.append(h("a", {
      class: "action is-signin",
      href: pageUrl("account"),
      title: t("js.game.gif_signin"),
    }, icon("film"), h("span", null, t("js.game.gif")), h("span", { class: "visually-hidden" }, ` – ${t("js.game.gif_signin")}`)));
  }

  return h("li", { class: `game is-${outcomeClass}` },
    h("span", { class: "visually-hidden" }, `${t("js.game.number", { id: game.id })}. `),
    h("div", { class: "game-outcome" },
      h("span", { class: "outcome-label" }, outcomeLabel),
      h("span", { class: "outcome-reason" }, reasonText(game))),
    h("div", { class: "game-players" },
      playerLine(game, "white", opts),
      playerLine(game, "black", opts)),
    h("div", { class: "game-meta" },
      h("span", { class: "meta-cat" },
        custom ? `${t("js.game.custom")} · ` : null, h("bdi", { dir: "ltr" }, category), " · ", t(game.rated ? "js.game.rated" : "js.game.casual")),
      h("span", null, t("js.game.moves", { count: moves }), duration !== null ? ` · ${fmt.duration(duration)}` : ""),
      ended ? h("time", { datetime: fmt.isoDate(ended), title: fmt.dateTime(ended) }, fmt.dateTime(ended)) : null),
    actions);
}

/**
 * A paged list of games in `container`. `load(before)` returns { games, next, total? }.
 * Options: owner (the player whose list it is: their side gives the outcome and is highlighted),
 * me (the signed-in viewer, tagged "you"), linkPlayers, empty (message),
 * limit (show at most this many, without "Show more"), onTotal(total).
 */
export function gameList(container, load, opts = {}) {
  let next = null;
  let token = 0;
  const list = h("ol", { class: "games" });
  const foot = h("div", { class: "list-foot" });

  async function fetchPage(before, my) {
    const more = before !== null && before !== undefined;
    const button = foot.querySelector("button");
    if (more && button) {
      button.classList.add("is-busy");
      button.disabled = true;
    }
    try {
      const page = await load(more ? before : null);
      if (my !== token) return;
      if (!more) clear(list);
      const first = list.children.length;
      for (const game of page.games || []) list.append(gameRow(game, opts));
      // "Show more" goes away or moves: the focus goes to the first new game.
      if (more && list.children[first]) {
        list.children[first].tabIndex = -1;
        list.children[first].focus();
      }
      next = opts.limit ? null : page.next ?? null;
      if (!more && typeof page.total === "number") opts.onTotal?.(page.total);
      clear(foot);
      if (!list.children.length) {
        clear(container);
        container.append(h("p", { class: "empty" }, opts.empty || t("js.game.none")));
        return;
      }
      if (next !== null) {
        const b = h("button", { class: "btn btn-ghost btn-small", type: "button" }, t("js.common.load_more"));
        b.addEventListener("click", () => fetchPage(next, token));
        foot.append(b);
      }
      if (!container.contains(list)) {
        clear(container);
        container.append(list, foot);
      }
    } catch (e) {
      if (my !== token) return;
      clear(foot);
      const retry = h("button", { class: "btn btn-ghost btn-small", type: "button" }, icon("refresh"), h("span", null, t("js.common.retry")));
      retry.addEventListener("click", () => fetchPage(before, token));
      const message = h("p", { class: "form-error", role: "alert" }, errorMessage(e, opts.errorExtra));
      if (more) {
        foot.append(message, retry);
      } else {
        clear(container);
        container.append(h("div", { class: "list-foot" }, message, retry));
      }
    } finally {
      const b = foot.querySelector("button.is-busy");
      if (b) {
        b.classList.remove("is-busy");
        b.disabled = false;
      }
    }
  }

  function reload() {
    token += 1;
    clear(container);
    container.append(h("div", { class: "loading-block" }, h("span", { class: "spinner", "aria-hidden": "true" }), h("span", null, t("js.common.loading"))));
    return fetchPage(null, token);
  }

  reload();
  return { reload };
}
