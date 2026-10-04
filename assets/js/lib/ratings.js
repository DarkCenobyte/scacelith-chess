// Rating cards: one per time control, with the record and a win / draw / loss bar.

import { h, clear } from "./dom.js";
import { t, fmt } from "./i18n.js";

export function ratingText(rating, provisional) {
  if (rating === null || rating === undefined) return t("js.common.dash");
  return provisional ? `${fmt.rating(rating)}?` : fmt.rating(rating);
}

export function renderRatings(container, ratings, { order = [] } = {}) {
  clear(container);
  const list = [...(ratings || [])].sort((a, b) => {
    const ia = order.indexOf(a.category), ib = order.indexOf(b.category);
    return (ia < 0 ? 999 : ia) - (ib < 0 ? 999 : ib) || String(a.category).localeCompare(String(b.category));
  });
  if (!list.length) {
    container.append(h("p", { class: "ratings-empty" }, t("js.ratings.none")));
    return;
  }
  let anyProvisional = false;
  for (const r of list) {
    const games = r.games || 0;
    const pct = (n) => (games ? `${(100 * n) / games}%` : "0");
    anyProvisional ||= Boolean(r.provisional);
    const bar = h("div", { class: "wdl-bar", "aria-hidden": "true" },
      h("span", { class: "w" }), h("span", { class: "d" }), h("span", { class: "l" }));
    const [w, d, l] = bar.children;
    w.style.width = pct(r.wins || 0);
    d.style.width = pct(r.draws || 0);
    l.style.width = pct(r.losses || 0);
    container.append(h("article", { class: "rating-card" },
      h("p", { class: "rating-cat" }, r.category),
      h("p", { class: "rating-value", title: r.provisional ? t("js.ratings.provisional") : null },
        fmt.rating(r.rating), r.provisional ? h("span", { class: "provisional" }, "?") : null,
        r.provisional ? h("span", { class: "visually-hidden" }, ` (${t("js.ratings.provisional")})`) : null),
      h("p", { class: "rating-meta" },
        h("span", null, `${t("js.ratings.games")} ${fmt.number(games)}`),
        h("span", null, `${t("js.ratings.peak")} ${fmt.rating(r.peak ?? r.rating)}`)),
      bar,
      h("p", { class: "wdl-text" },
        h("span", { class: "visually-hidden" }, `${t("js.ratings.wdl")}: `),
        h("span", { class: "w" }, `+${fmt.number(r.wins || 0)}`), " ",
        h("span", { class: "d" }, `=${fmt.number(r.draws || 0)}`), " ",
        h("span", { class: "l" }, `−${fmt.number(r.losses || 0)}`))));
  }
  if (anyProvisional) container.append(h("p", { class: "ratings-hint" }, t("js.ratings.unrated_hint")));
}
