# Scacelith website (gh-pages)

The official website of Scacelith, published by GitHub Pages at <https://scacelith.com/> from this
branch. It presents the game, offers the downloads of the newest release, and talks to the official
server's HTTPS API (`https://caissa.scacelith.com/api/v1`, documented at <https://docs.scacelith.com/>)
so that players can sign in, see their account and ratings, browse their game history, and download
each game as PGN or as an animated GIF. It is translated into the 34 languages of the game.

Static files only: no framework, no runtime dependency, no cookie, no tracker. The pages are
generated once per language by `tools/build.py` (Python 3.9+, standard library only) and committed.

## Changing the trailer

Edit `config.js` and put a YouTube video id or link in `trailer`, then commit:

```js
window.SCACELITH = {
  trailer: "https://www.youtube.com/watch?v=XXXXXXXXXXX",   // or "XXXXXXXXXXX", or a youtu.be link
  trailerStart: 0,
};
```

No build is needed. Until a video is set, the section says the trailer is coming soon. Nothing is
loaded from YouTube until the visitor accepts its player: a notice over the poster says what Google's
player receives and stores, with one button, "Accept and play", and only then is the
`youtube-nocookie.com` frame created. The notice comes from
[iframemanager](https://github.com/orestbida/iframemanager) 1.3.0 (MIT), kept in
`assets/vendor/iframemanager/`; the answer is not remembered, so the site still sets no cookie.

## Replacing the screenshots

The pictures are plain files in `assets/img/`: replace them and commit, no build needed.

| File | Where it shows |
| --- | --- |
| `hero.webp`, `hero-960.webp` | The top of the home page, the trailer's poster, the header of the inner pages and the language page: a scene without interface text (16:9 or wider), shown cropped; its caption is `hero.caption` |
| `og-scacelith.jpg` | The picture of link previews (1200 x 630) |
| `title-screen`, `the-coach`, `coach-challenges`, `analysis-mode`, `watching-a-game`, `the-opponent`, `the-grand-hall`, `the-scoresheet` | The gallery, in this order (the first one large, four beside it, three below): `<name>.webp` is the full picture of the viewer, `<name>-1000.webp` its 1000 px thumbnail (shown in a 16:9 frame) |

`tools/screenshots.sh <name> <picture>` makes these files from a PNG, JPEG or WebP with
ImageMagick (`tools/screenshots.sh` alone lists the names). Without it, any editor will do: WebP,
quality about 80, the sizes above. If a new picture shows something else, change its caption and
description (`gallery` in `i18n/<lang>.json`) and run `python3 tools/build.py`.

## Layout

| Path | Content |
| --- | --- |
| `index.html` | The site root: sends the visitor to their language (`/#choose` shows the list), generated |
| `<lang>/` | The home page of a language, generated |
| `<lang>/players/` | Leaderboard and public profiles (`?u=<name>`, `?c=<time control>`), generated |
| `<lang>/account/` | Sign-in, sign-up, password recovery, and the signed-in player's account, generated |
| `<lang>/gif/` | Opened in a new tab by a game's GIF button (`?id=<game>`), generated |
| `<lang>/legal/` | Legal notice, privacy and terms of use (`#notice`, `#privacy`, `#terms`), generated; update its "Last updated" date (`legal.updated`) in every language when its text changes |
| `404.html`, `sitemap.xml` | Generated |
| `account/google/` | Where Google sends the visitor back after "Sign in with Google", for every language: `assets/js/sso-return.js` hands Google's answer to the account page of the language the sign-in started from, generated |
| `config.js` | The trailer (see above) |
| `templates/` | The pages, with `{{t:key}}` for the translations (`templates/partials/` for the shared parts) |
| `i18n/<lang>.json` | The strings of each language; `en.json` is the source of all others |
| `tools/site.json` | Site address, API address, repositories, contact address of the legal page, languages and pages |
| `tools/build.py` | Generates the pages and checks the translations |
| `tools/screenshots.sh` | Converts a screenshot into the site's picture files (ImageMagick) |
| `assets/css/site.css` | The stylesheet |
| `assets/js/` | One module per page (`home.js`, `players.js`, `account.js`, `gif.js`, `legal.js`), the shared `site.js`, and `lib/` |
| `assets/vendor/iframemanager/` | The trailer's consent notice (iframemanager 1.3.0, MIT, unmodified) |
| `assets/js/pow-worker.js` | The proof-of-work solver of the sign-up (SHA-256, in a worker) |
| `assets/fonts/` | The game's typefaces (Cinzel, EB Garamond with its Greek and Latin Extended subsets, Amiri, SIL OFL 1.1) and its chess figures (GNU FreeFont), subset to woff2, with their licences; Korean and Hindi use the visitor's system fonts |
| `assets/img/`, `assets/icons/` | Screenshots captured in the game, and the game's icon |

Languages: the 34 of the game and of the API documentation, in the order of `tools/site.json`:
English (`en`), Indonesian (`id`), Czech (`cs`), Danish (`da`), German (`de`), Estonian (`et`),
Spanish (`es`), French (`fr`), Croatian (`hr`), Italian (`it`), Latvian (`lv`), Lithuanian (`lt`),
Hungarian (`hu`), Dutch (`nl`), Polish (`pl`), Brazilian Portuguese (`pt-BR`), European Portuguese
(`pt`), Romanian (`ro`), Slovak (`sk`), Slovenian (`sl`), Finnish (`fi`), Swedish (`sv`), Vietnamese
(`vi`), Turkish (`tr`), Greek (`el`), Bulgarian (`bg`), Russian (`ru`), Ukrainian (`uk`), Arabic
(`ar`, right to left), Hindi (`hi`), Korean (`ko`), Japanese (`ja`), Simplified Chinese (`zh-Hans`)
and Traditional Chinese (`zh-Hant`).

## Changing the text or the pages

```sh
python3 tools/build.py --check      # translations complete and generated files up to date?
python3 tools/build.py              # regenerates <lang>/..., index.html, 404.html, sitemap.xml
```

- Change a page in `templates/` and its strings in `i18n/en.json`, then in every other language
  (the build refuses a language that misses a string, or whose placeholders, markup or plural forms
  differ from English). `python3 tools/build.py --todo de` writes `i18n/de.todo.json` with the
  English strings that German still misses (not committed).
- The strings used by the scripts are under `js` in each language file. Plural strings are objects
  with the CLDR categories of the language (`one`, `few`, `many`, `other`...).
- Do not edit the generated files by hand.

A local preview against a local server: run the server with `CORS_ORIGINS=http://localhost:8080`
(see below), then

```sh
python3 tools/build.py --out /tmp/site --api http://127.0.0.1:8443/api/v1
python3 -m http.server 8080 --directory /tmp/site      # http://localhost:8080/
```

## The API and CORS

The site calls the API from the visitor's browser, so the server must allow this site's origins:
the official server runs with `CORS_ORIGINS=https://scacelith.com,https://www.scacelith.com` (a
setting of the server, see its `docs/CONFIG.md`). The session token is a bearer token kept by the
browser (`sessionStorage`, or `localStorage` with "Keep me signed in"); no cookie is used. The pages
carry a strict Content Security Policy: scripts and styles from this site only, connections to the
API and to `api.github.com` (the releases) only, frames from `youtube-nocookie.com` only. Changing
the API address (`tools/site.json`) updates the policy at the next build.

"Sign in with Google" shows when the server's `GET /info` says `sso.googleWeb: true`. It loads
nothing from Google: the account page makes a PKCE pair, asks the server to start
(`POST /auth/sso/google/web/start`), and goes to the Google address of the answer; Google sends the
browser back to `https://scacelith.com/account/google/`, which must be the redirect URI of the
server's Google client ("Web application"), and the account page finishes with the server. The
attempt and Google's answer are kept in the tab's `sessionStorage` until they are used.

Downloads: the menus read the newest release of
[DarkCenobyte/scacelith-chess](https://github.com/DarkCenobyte/scacelith-chess/releases) from the
GitHub API (`/releases/latest`: the newest release that is neither a draft nor a pre-release) and
sort its files by system from their names (`…-windows-x64.zip`, `…-linux-x86_64.tar.gz`,
`…-linux-aarch64.tar.gz`, `…-macos-arm64-experimental.dmg`...), and by processor (`x64`, `x86_64`,
`arm64`, `aarch64`) when a system has builds for several. A file whose name says `experimental` is
marked so, and so is its system when all its files are. New systems appear by themselves when a
release carries their files. The last entry of the menus is the source code: GitHub's ZIP archive
of the release's tag (`https://github.com/DarkCenobyte/scacelith-chess/archive/refs/tags/<tag>.zip`).

When the GitHub API cannot be reached (it allows 60 requests per hour and address), the menus fall
back to the release written in `tools/site.json` (`fallbackRelease`, the newest release when the
site was built). After a release is published, update it and build:

```sh
python3 tools/build.py --update-release   # fallbackRelease from GitHub's newest release
python3 tools/build.py
```

## Publishing

GitHub Pages serves this branch as it is (Settings > Pages: "Deploy from a branch", `gh-pages`,
`/ (root)`); `.nojekyll` turns Jekyll off and `CNAME` holds `scacelith.com`. For the domain, the
apex needs GitHub's four `A` records (`185.199.108.153` to `185.199.111.153`, and optionally the
four `AAAA` records `2606:50c0:8000::153` to `2606:50c0:8003::153`), and `www` a `CNAME` to
`darkcenobyte.github.io`; GitHub then redirects `www.scacelith.com` to `scacelith.com`. Enable
"Enforce HTTPS" once the certificate is ready.

## Licences

The website's code is part of Scacelith, free software under the GNU General Public License v3
(see `LICENSE` on the master branch). The fonts keep their own licences (`assets/fonts/`). The
screenshots are captured in the game.
