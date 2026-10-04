# Scacelith website (gh-pages)

The official website of Scacelith, published by GitHub Pages at <https://scacelith.com/> from this
branch. It presents the game, offers the downloads of the newest release, and talks to the official
server's HTTPS API (`https://caissa.scacelith.com/api/v1`, documented at <https://docs.scacelith.com/>)
so that players can sign in, see their account and ratings, browse their game history, and download
each game as PGN or as an animated GIF. It is translated into the ten languages of the game.

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

No build is needed. Until a video is set, the section says the trailer is coming soon. The video is
loaded from `youtube-nocookie.com` only when the visitor presses play.

## Replacing the screenshots

The pictures are plain files in `assets/img/`: replace them and commit, no build needed.

| File | Where it shows |
| --- | --- |
| `hero.webp`, `hero-960.webp` | The top of the home page, the trailer's poster, the header of the inner pages and the language page: a scene without interface text, about 2:1 |
| `og-scacelith.jpg` | The picture of link previews (1200 x 630) |
| `title-screen`, `the-grand-hall`, `at-the-board`, `the-opponent`, `the-scoresheet` | The gallery, in this order: `<name>.webp` is the full picture of the viewer, `<name>-1000.webp` its 1000 px thumbnail (shown in a 16:9 frame) |

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
| `404.html`, `sitemap.xml` | Generated |
| `config.js` | The trailer (see above) |
| `templates/` | The pages, with `{{t:key}}` for the translations (`templates/partials/` for the shared parts) |
| `i18n/<lang>.json` | The strings of each language; `en.json` is the source of all others |
| `tools/site.json` | Site address, API address, repositories, languages and pages |
| `tools/build.py` | Generates the pages and checks the translations |
| `tools/screenshots.sh` | Converts a screenshot into the site's picture files (ImageMagick) |
| `assets/css/site.css` | The stylesheet |
| `assets/js/` | One module per page (`home.js`, `players.js`, `account.js`, `gif.js`), the shared `site.js`, and `lib/` |
| `assets/js/pow-worker.js` | The proof-of-work solver of the sign-up (SHA-256, in a worker) |
| `assets/fonts/` | The game's typefaces (Cinzel, EB Garamond, Amiri, SIL OFL 1.1) and its chess figures (GNU FreeFont), subset to woff2, with their licences |
| `assets/img/`, `assets/icons/` | Screenshots captured in the game, and the game's icon |

Languages: English (`en`), French (`fr`), German (`de`), Spanish (`es`), Ukrainian (`uk`),
Russian (`ru`), Arabic (`ar`, right to left), Japanese (`ja`), Simplified Chinese (`zh-Hans`) and
Traditional Chinese (`zh-Hant`), the languages of the game and of the API documentation.

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

Downloads: the menus read the newest release of
[DarkCenobyte/scacelith-chess](https://github.com/DarkCenobyte/scacelith-chess/releases) from the
GitHub API (`/releases/latest`, or the newest pre-release while there is no stable one) and sort its
files by system from their names (`…-windows-x64.zip`, `…-linux-….AppImage`, `…-macos-….dmg`...).
New systems appear by themselves when a release carries their files.

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
