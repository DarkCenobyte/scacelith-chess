// The newest release of the game on GitHub and its files, sorted by operating system.
// GitHub's "latest" release never includes pre-releases, so when there is none (a beta), the
// newest published release of the list is used instead.

import * as store from "./store.js";

const REPO = document.documentElement.dataset.repo || "DarkCenobyte/scacelith-chess";
const CACHE_KEY = "scacelith.release.v1";
const CACHE_MS = 15 * 60 * 1000;
export const RELEASES_URL = `https://github.com/${REPO}/releases`;
export const OSES = ["windows", "macos", "linux"];

function osOf(name) {
  const n = name.toLowerCase();
  if (/(^|[-_.])(windows|win64|win32|win)([-_.]|$)|\.exe$|\.msi$/.test(n)) return "windows";
  if (/(^|[-_.])(macos|darwin|osx|mac)([-_.]|$)|\.dmg$|\.pkg$/.test(n)) return "macos";
  if (/(^|[-_.])linux([-_.]|$)|\.appimage$|\.deb$|\.rpm$|\.flatpak$/.test(n)) return "linux";
  return null;
}

function kindOf(name) {
  const n = name.toLowerCase();
  for (const ext of ["exe", "msi", "zip", "dmg", "pkg", "appimage", "deb", "rpm", "flatpak", "tar.gz", "tar.xz", "7z"]) {
    if (n.endsWith(`.${ext}`)) return ext;
  }
  return "file";
}

function archOf(name) {
  const n = name.toLowerCase();
  if (/arm64|aarch64/.test(n)) return "arm64";
  if (/x64|x86_64|amd64|win64/.test(n)) return "x64";
  if (/universal/.test(n)) return "universal";
  return null;
}

function normalize(release) {
  const assets = (release.assets || [])
    .filter((a) => a && a.state !== "starter" && a.browser_download_url)
    .map((a) => ({
      name: a.name,
      size: a.size,
      url: a.browser_download_url,
      os: osOf(a.name),
      kind: kindOf(a.name),
      arch: archOf(a.name),
      digest: a.digest || null,
    }));
  const tag = release.tag_name || "";
  return {
    tag,
    version: tag.replace(/^v/, ""),
    name: release.name || tag,
    prerelease: Boolean(release.prerelease),
    published: Date.parse(release.published_at || release.created_at || "") || null,
    url: release.html_url || `${RELEASES_URL}/tag/${encodeURIComponent(tag)}`,
    assets,
    sums: assets.find((a) => /sha256sums/i.test(a.name)) || null,
  };
}

async function github(path) {
  const res = await fetch(`https://api.github.com/repos/${REPO}${path}`, {
    headers: { Accept: "application/vnd.github+json" },
    credentials: "omit",
    referrerPolicy: "no-referrer",
  });
  return res;
}

let pending = null;

/**
 * The release known when the site was built (tools/site.json "fallbackRelease"), used when the
 * GitHub API cannot be reached or refuses (it allows 60 requests per hour and address). Its
 * download links stay valid; a newer release may exist.
 */
function fallbackRelease() {
  try {
    const f = JSON.parse(document.getElementById("release-fallback")?.textContent || "null");
    if (!f || !f.tag || !Array.isArray(f.assets)) return null;
    const base = `https://github.com/${REPO}/releases/download/${encodeURIComponent(f.tag)}/`;
    return {
      ...normalize({
        tag_name: f.tag,
        name: f.name,
        prerelease: f.prerelease,
        published_at: f.published,
        assets: f.assets.map((a) => ({ name: a.name, size: a.size, browser_download_url: base + encodeURIComponent(a.name), state: "uploaded" })),
      }),
      fallback: true,
    };
  } catch {
    return null;
  }
}

/** The newest release (cached for a quarter of an hour), else the one known at build time. */
export function latestRelease() {
  if (pending) return pending;
  const cached = store.get(CACHE_KEY);
  if (cached && cached.at > Date.now() - CACHE_MS && cached.release?.tag) return (pending = Promise.resolve(cached.release));
  pending = (async () => {
    let raw = null;
    const latest = await github("/releases/latest");
    if (latest.ok) raw = await latest.json().catch(() => null);
    else if (latest.status !== 404) throw new Error(`GitHub ${latest.status}`);
    if (!raw?.tag_name) {
      // No "latest" release (only pre-releases so far): the newest published one of the list.
      const list = await github("/releases?per_page=10");
      if (!list.ok) throw new Error(`GitHub ${list.status}`);
      const all = await list.json().catch(() => null);
      raw = (Array.isArray(all) ? all : []).find((r) => r && !r.draft && r.tag_name) || null;
    }
    if (!raw) throw new Error("no release");
    const release = normalize(raw);
    store.set(CACHE_KEY, { at: Date.now(), release });
    return release;
  })();
  pending = pending.catch((e) => {
    pending = null;
    const fallback = fallbackRelease();
    if (fallback) return fallback;
    throw e;
  });
  return pending;
}

/** The visitor's operating system, as far as the browser tells. */
export function detectOS() {
  const platform = (navigator.userAgentData?.platform || navigator.platform || "").toLowerCase();
  const ua = (navigator.userAgent || "").toLowerCase();
  if (/android/.test(ua)) return "android";
  if (/iphone|ipad|ipod/.test(ua) || (platform === "macintel" && navigator.maxTouchPoints > 1)) return "ios";
  if (platform.startsWith("win") || /windows/.test(ua)) return "windows";
  if (platform.startsWith("mac") || /mac os x/.test(ua)) return "macos";
  if (platform.includes("linux") || /linux|x11|cros/.test(ua)) return "linux";
  return null;
}

/** The files of one system, the preferred format first (the ZIP with its licence, then the rest). */
export function filesFor(release, os) {
  const order = ["zip", "exe", "msi", "dmg", "pkg", "appimage", "deb", "rpm", "flatpak", "tar.gz", "tar.xz", "7z", "file"];
  return release.assets
    .filter((a) => a.os === os)
    .sort((a, b) => order.indexOf(a.kind) - order.indexOf(b.kind));
}
