// The strings of the page's language (embedded by tools/build.py as #i18n) and the formatters
// of dates, numbers and durations in that language.

const data = (() => {
  try {
    return JSON.parse(document.getElementById("i18n")?.textContent || "{}");
  } catch {
    return {};
  }
})();

export const lang = data.lang || document.documentElement.lang || "en";
export const dir = data.dir || document.documentElement.dir || "ltr";
const strings = data.strings || {};

// Ratings, clocks and moves read best with Western digits, as in the game.
const locale = lang === "ar" ? "ar-u-nu-latn" : lang;

const plural = new Intl.PluralRules(locale);
const numberFormat = new Intl.NumberFormat(locale);
const signedFormat = new Intl.NumberFormat(locale, { signDisplay: "exceptZero", useGrouping: false });
// Ratings are written as in the game and in chess generally: 1500, not 1,500.
const ratingFormat = new Intl.NumberFormat(locale, { useGrouping: false });

function lookup(key) {
  let node = strings;
  // The page holds the "js" tree of the translations: "js.game.white" is strings.game.white.
  for (const part of key.replace(/^js\./, "").split(".")) {
    if (node === null || typeof node !== "object" || !(part in node)) return undefined;
    node = node[part];
  }
  return node;
}

/** The string `key` with its `{placeholders}` filled; plural strings pick their form from `count`. */
export function t(key, vars = {}) {
  let value = lookup(key);
  if (value && typeof value === "object") {
    const form = plural.select(Number(vars.count ?? 0));
    value = value[form] ?? value.other;
  }
  if (typeof value !== "string") return key;
  return value.replace(/\{([a-zA-Z][a-zA-Z0-9_]*)\}/g, (match, name) => {
    if (!(name in vars)) return match;
    const v = vars[name];
    const text = name === "count" && typeof v === "number" ? numberFormat.format(v) : String(v);
    // In a right-to-left sentence, a version, an address or a name keeps its own order.
    return dir === "rtl" ? `\u2068${text}\u2069` : text;
  });
}

export function has(key) {
  return lookup(key) !== undefined;
}

const dateFormat = new Intl.DateTimeFormat(locale, { day: "numeric", month: "long", year: "numeric" });
const shortDateFormat = new Intl.DateTimeFormat(locale, { day: "numeric", month: "short", year: "numeric" });
const dateTimeFormat = new Intl.DateTimeFormat(locale, {
  day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit",
});
const relativeFormat = new Intl.RelativeTimeFormat(locale, { numeric: "auto" });
const unitFormats = {};
function unitFormat(unit) {
  unitFormats[unit] ||= new Intl.NumberFormat(locale, { style: "unit", unit, unitDisplay: "short", maximumFractionDigits: unit === "megabyte" || unit === "kilobyte" ? 1 : 0 });
  return unitFormats[unit];
}
const listFormat = typeof Intl.ListFormat === "function" ? new Intl.ListFormat(locale, { type: "unit", style: "narrow" }) : null;

export const fmt = {
  number: (n) => numberFormat.format(n),
  rating: (n) => ratingFormat.format(n),
  signed: (n) => signedFormat.format(n),
  date: (ms) => dateFormat.format(new Date(ms)),
  shortDate: (ms) => shortDateFormat.format(new Date(ms)),
  dateTime: (ms) => dateTimeFormat.format(new Date(ms)),
  isoDate: (ms) => new Date(ms).toISOString(),

  /** "3 hours ago", "in 2 days". */
  relative(ms, now = Date.now()) {
    const seconds = Math.round((ms - now) / 1000);
    const abs = Math.abs(seconds);
    const steps = [
      [60, "second", 1], [3600, "minute", 60], [86400, "hour", 3600],
      [86400 * 30, "day", 86400], [86400 * 365, "month", 86400 * 30], [Infinity, "year", 86400 * 365],
    ];
    for (const [limit, unit, size] of steps) {
      if (abs < limit) return relativeFormat.format(Math.round(seconds / size), unit);
    }
    return dateFormat.format(new Date(ms));
  },

  /** A length of time: "6 min 46 s", "1 h 05 min". */
  duration(ms) {
    let total = Math.max(0, Math.round(ms / 1000));
    const h = Math.floor(total / 3600);
    total -= h * 3600;
    const m = Math.floor(total / 60);
    const s = total - m * 60;
    // The two largest units, without the zero ones ("1 h", "6 min 46 s", "0 s").
    const units = h ? [["hours", h], ["minutes", m]] : m ? [["minutes", m], ["seconds", s]] : [["seconds", s]];
    const shown = units.filter(([, v], i) => v || i === 0);
    if (typeof Intl.DurationFormat === "function") {
      try {
        const text = new Intl.DurationFormat(locale, { style: "short" }).format(Object.fromEntries(shown));
        if (text) return text;
      } catch { /* falls through */ }
    }
    const single = { hours: "hour", minutes: "minute", seconds: "second" };
    const parts = shown.map(([u, v]) => unitFormat(single[u]).format(v));
    return listFormat ? listFormat.format(parts) : parts.join(" ");
  },

  /** A wait before a retry: "45 seconds", "3 minutes". */
  wait(seconds) {
    const s = Math.max(1, Math.ceil(seconds));
    return s < 90 ? t("js.time.seconds", { count: s }) : t("js.time.minutes", { count: Math.ceil(s / 60) });
  },

  /** A file size in binary units, as GitHub shows them (95,159,654 bytes: "90.8 MB"). */
  bytes(n) {
    if (n >= 1048576) return unitFormat("megabyte").format(n / 1048576);
    return unitFormat("kilobyte").format(Math.max(1, n / 1024));
  },
};

/** The URL of a page of the site in this language: page("account"), page("gif", {id: 1}). */
export function pageUrl(page, params, hash) {
  const root = document.documentElement.dataset.root || "/";
  let url = `${root}${lang}/${page ? page + "/" : ""}`;
  if (params) {
    const query = new URLSearchParams();
    for (const [k, v] of Object.entries(params)) if (v !== undefined && v !== null && v !== "") query.set(k, v);
    const qs = query.toString();
    if (qs) url += `?${qs}`;
  }
  if (hash) url += `#${hash}`;
  return url;
}
