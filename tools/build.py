#!/usr/bin/env python3
"""Builds the Scacelith website: one copy of every page per language, from templates/ and i18n/.

    python3 tools/build.py                 # writes <lang>/..., index.html, 404.html, sitemap.xml
    python3 tools/build.py --check         # checks the translations and that the output is up to date
    python3 tools/build.py --todo de       # i18n/de.todo.json: the English strings missing in de.json
    python3 tools/build.py --out DIR --api http://127.0.0.1:8443/api/v1
                                           # a complete copy of the site in DIR, for local testing

Python 3.9+, standard library only. Settings: tools/site.json. The trailer is not built in: it is
read at run time from config.js, so changing it needs no build.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import shutil
import sys
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent.parent
TEMPLATES = ROOT / "templates"
I18N = ROOT / "i18n"
STATIC = ["assets", "config.js", "CNAME", "robots.txt", ".nojekyll"]

PLURAL_KEYS = {"zero", "one", "two", "few", "many", "other"}
# The plural categories each language needs (CLDR), for the strings of the "js" tree that depend
# on a number. "other" is always required; a missing category falls back to "other" at run time.
PLURALS = {
    "en": {"one", "other"},
    "fr": {"one", "many", "other"},
    "de": {"one", "other"},
    "es": {"one", "many", "other"},
    "uk": {"one", "few", "many", "other"},
    "ru": {"one", "few", "many", "other"},
    "ar": {"zero", "one", "two", "few", "many", "other"},
    "ja": {"other"},
    "zh-Hans": {"other"},
    "zh-Hant": {"other"},
}
# Markup a translation may hold (strings inserted with {{h:...}}); the tags must match English.
ALLOWED_TAGS = {"em", "strong", "br", "bdi", "code", "abbr", "span"}
TAG_RE = re.compile(r"</?([a-zA-Z][a-zA-Z0-9]*)(?:\s+[^<>]*)?/?>")
# The only markup a string may hold: these exact tags, without attributes. Any other "<" or ">"
# (a tag with attributes, "<svg/onload=...>", "<!--") is refused, since {{h:...}} strings are
# inserted as HTML and the "js" strings are embedded in a <script> element.
MARKUP_OK = re.compile(r"</?(?:em|strong|bdi|code|abbr|span)>|<br>")


def markup_problem(text: str) -> bool:
    return bool(re.search(r"[<>]", MARKUP_OK.sub("", text)))


def script_json(value) -> str:
    """JSON for a <script type="application/json"> element: no "<" at all, so no markup can end
    or reopen the element."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
PLACEHOLDER_RE = re.compile(r"\{([a-zA-Z][a-zA-Z0-9_]*)\}")
TOKEN_RE = re.compile(r"\{\{\s*(?:(>)\s*([a-z0-9_-]+)|([trhv]):([A-Za-z0-9_.-]+))\s*\}\}")


class BuildError(Exception):
    pass


# --------------------------------------------------------------------------------------------
# Translations


def is_plural(value) -> bool:
    return isinstance(value, dict) and value and set(value) <= PLURAL_KEYS and "other" in value


def flatten(tree: dict, prefix: str = "") -> dict:
    """{dotted key: string or plural dict}."""
    out = {}
    for key, value in tree.items():
        path = f"{prefix}{key}"
        if isinstance(value, dict) and not is_plural(value):
            out.update(flatten(value, path + "."))
        else:
            out[path] = value
    return out


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise BuildError(f"{path.relative_to(ROOT)}: invalid JSON: {e}") from None


def strings_of(value) -> list:
    return list(value.values()) if isinstance(value, dict) else [value]


def tags_of(text: str) -> list:
    return sorted(m.group(0) if m.group(1) == "br" else m.group(1) for m in TAG_RE.finditer(text))


def check_language(code: str, english: dict, tree: dict) -> list:
    """The problems of one language against English (an empty list when it is complete)."""
    problems = []
    flat = flatten(tree)
    for key, en_value in english.items():
        if key not in flat:
            problems.append(f"{code}: missing {key}")
            continue
        value = flat[key]
        if is_plural(en_value) != is_plural(value):
            problems.append(f"{code}: {key}: plural forms expected" if is_plural(en_value)
                            else f"{code}: {key}: a plain string expected")
            continue
        if is_plural(value):
            missing = PLURALS[code] - set(value)
            if missing:
                problems.append(f"{code}: {key}: missing plural forms {sorted(missing)}")
        en_holders = set(PLACEHOLDER_RE.findall(" ".join(strings_of(en_value))))
        for text in strings_of(value):
            if not isinstance(text, str) or not text.strip():
                problems.append(f"{code}: {key}: empty or not a string")
                continue
            holders = set(PLACEHOLDER_RE.findall(text))
            if not holders <= en_holders:
                problems.append(f"{code}: {key}: unknown placeholders {sorted(holders - en_holders)}")
            if markup_problem(text):
                problems.append(f"{code}: {key}: markup other than <em>, <strong>, <br>... or a stray < >")
        if not is_plural(en_value):
            if tags_of(value) != tags_of(en_value):
                problems.append(f"{code}: {key}: markup differs from English ({tags_of(en_value)} vs {tags_of(value)})")
            missing = en_holders - set(PLACEHOLDER_RE.findall(value))
            if missing:
                problems.append(f"{code}: {key}: missing placeholders {sorted(missing)}")
    for key in flat:
        if key not in english:
            problems.append(f"{code}: unknown key {key} (not in English)")
    return problems


def lookup(tree: dict, key: str):
    node = tree
    for part in key.split("."):
        if not isinstance(node, dict) or part not in node:
            raise BuildError(f"no translation for {key}")
        node = node[part]
    return node


# --------------------------------------------------------------------------------------------
# Templates


def render(text: str, ctx: dict, strings: dict, depth: int = 0) -> str:
    if depth > 8:
        raise BuildError("templates include each other too deeply")

    def replace(m: re.Match) -> str:
        if m.group(1):
            part = TEMPLATES / "partials" / f"{m.group(2)}.html"
            if not part.exists():
                raise BuildError(f"no partial {m.group(2)}")
            return render(part.read_text(encoding="utf-8"), ctx, strings, depth + 1)
        kind, key = m.group(3), m.group(4)
        if kind in "th":
            value = lookup(strings, key)
            if not isinstance(value, str):
                raise BuildError(f"{key} is not a plain string")
            return html.escape(value, quote=True) if kind == "t" else value
        if key not in ctx:
            raise BuildError(f"no value for {kind}:{key}")
        value = str(ctx[key])
        return html.escape(value, quote=True) if kind == "v" else value

    return TOKEN_RE.sub(replace, text)


def attr(value: str) -> str:
    return html.escape(value, quote=True)


# --------------------------------------------------------------------------------------------
# Site


class Site:
    def __init__(self, api_override: str | None = None, only: list | None = None):
        self.cfg = load_json(ROOT / "tools" / "site.json")
        if api_override:
            self.cfg["api"] = api_override.rstrip("/")
        self.langs = [lang for lang in self.cfg["languages"] if not only or lang["code"] in only]
        self.codes = [lang["code"] for lang in self.langs]
        self.pages = self.cfg["pages"]
        self.base = self.cfg["baseUrl"]
        self.english = load_json(I18N / "en.json")
        self.trees = {}
        for code in self.codes:
            path = I18N / f"{code}.json"
            self.trees[code] = load_json(path) if path.exists() else None

    # --- checks

    def problems(self) -> list:
        english = flatten(self.english)
        out = []
        for code in self.codes[1:]:
            tree = self.trees[code]
            if tree is None:
                out.append(f"{code}: i18n/{code}.json does not exist")
                continue
            out.extend(check_language(code, english, tree))
        for key, value in english.items():
            for text in strings_of(value):
                if markup_problem(text):
                    out.append(f"en: {key}: markup other than <em>, <strong>, <br>... or a stray < >")
        return out

    def todo(self, code: str) -> dict:
        english = flatten(self.english)
        flat = flatten(self.trees.get(code) or {})
        return {key: value for key, value in english.items() if key not in flat}

    # --- output

    def origin(self, url: str) -> str:
        parts = urlsplit(url)
        return f"{parts.scheme}://{parts.netloc}"

    def csp(self) -> str:
        connect = " ".join(["'self'", self.origin(self.cfg["api"])] + self.cfg["connect"])
        frames = " ".join(self.cfg["frames"])
        return (
            "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; "
            f"font-src 'self'; connect-src {connect}; frame-src {frames}; worker-src 'self'; "
            "manifest-src 'self'; base-uri 'none'; form-action 'none'; object-src 'none'"
        )

    def page_url(self, code: str, page: dict) -> str:
        return f"{self.base}{code}/{page['path']}"

    def x_default(self, page: dict) -> str:
        """The address for a visitor whose language is none of the site's: the site root, which
        sends them to their language, for the home page; the English page otherwise."""
        return self.base if page["path"] == "" else self.page_url("en", page)

    def context(self, code: str, page: dict) -> dict:
        lang = next(lang for lang in self.langs if lang["code"] == code)
        depth = 1 + page["path"].count("/")
        root = "../" * depth
        strings = self.trees[code]
        alternates = "\n".join(
            f'<link rel="alternate" hreflang="{other}" href="{attr(self.page_url(other, page))}">'
            for other in self.codes
        ) + f'\n<link rel="alternate" hreflang="x-default" href="{attr(self.x_default(page))}">'
        lang_links = "\n".join(
            f'<li><a href="{root}{other["code"]}/{page["path"]}" hreflang="{other["code"]}" lang="{other["code"]}"'
            f' dir="{other["dir"]}" data-lang="{other["code"]}"'
            + (' aria-current="true"' if other["code"] == code else "")
            + f'>{html.escape(other["name"])}</a></li>'
            for other in self.langs
        )
        footer_langs = "\n".join(
            f'<li><a href="{root}{other["code"]}/{page["path"]}" hreflang="{other["code"]}" lang="{other["code"]}"'
            f' dir="{other["dir"]}" data-lang="{other["code"]}"'
            + (' aria-current="true"' if other["code"] == code else "")
            + f'>{html.escape(other["name"])}</a></li>'
            for other in self.langs
        )
        runtime = dict(lookup(strings, "js"))
        runtime_json = script_json({"lang": code, "dir": lang["dir"], "strings": runtime})
        title = lookup(strings, f"pages.{page['key']}.title")
        description = lookup(strings, f"pages.{page['key']}.description")
        head_extra = ""
        if page["key"] == "home":
            data = {
                "@context": "https://schema.org",
                "@type": "VideoGame",
                "name": "Scacelith",
                "url": self.page_url(code, page),
                "description": description,
                "image": f"{self.base}assets/img/og-scacelith.jpg",
                "inLanguage": code,
                "genre": ["Chess"],
                "gamePlatform": "PC",
                "operatingSystem": "Windows",
                "applicationCategory": "GameApplication",
                "playMode": ["SinglePlayer", "MultiPlayer"],
                "license": "https://www.gnu.org/licenses/gpl-3.0.html",
                "sameAs": [f"https://github.com/{self.cfg['releasesRepo']}"],
                "downloadUrl": f"https://github.com/{self.cfg['releasesRepo']}/releases",
                "offers": {"@type": "Offer", "price": "0", "priceCurrency": "EUR"},
            }
            version = (self.cfg.get("fallbackRelease") or {}).get("tag", "").lstrip("v")
            if version:
                data["softwareVersion"] = version
            head_extra = ('<script type="application/ld+json">'
                          + script_json(data)
                          + "</script>")
        fallback = self.cfg.get("fallbackRelease") or {}
        release_json = script_json(fallback)
        return {
            "release_json": release_json,
            "title": title,
            "description": description,
            "head_extra": head_extra,
            "lang": code,
            "dir": lang["dir"],
            "og_locale": lang["og"],
            "og_alternates": "\n".join(
                f'<meta property="og:locale:alternate" content="{other["og"]}">'
                for other in self.langs if other["code"] != code
            ),
            "root": root,
            "home": f"{root}{code}/",
            "page": page["key"],
            "page_path": page["path"],
            "canonical": self.page_url(code, page),
            "base_url": self.base,
            "og_image": f"{self.base}assets/img/og-scacelith.jpg",
            "api": self.cfg["api"],
            "repo": self.cfg["releasesRepo"],
            "repo_url": f"https://github.com/{self.cfg['releasesRepo']}",
            "releases_url": f"https://github.com/{self.cfg['releasesRepo']}/releases",
            "server_repo_url": f"https://github.com/{self.cfg['serverRepo']}",
            "docs_url": f"{self.cfg['docsUrl']}{code}/",
            "csp": self.csp(),
            "robots": "index, follow" if page["index"] else "noindex, follow",
            "alternates": alternates,
            "lang_links": lang_links,
            "footer_langs": footer_langs,
            "lang_name": lang["name"],
            "i18n_json": runtime_json,
        }

    def build(self) -> dict:
        """{relative path: content} of every generated file."""
        problems = self.problems()
        if problems:
            raise BuildError("translations are incomplete:\n  " + "\n  ".join(problems[:60])
                             + (f"\n  ... and {len(problems) - 60} more" if len(problems) > 60 else ""))
        out = {}
        for code in self.codes:
            for page in self.pages:
                ctx = self.context(code, page)
                body = (TEMPLATES / page["template"]).read_text(encoding="utf-8")
                ctx["content"] = render(body, ctx, self.trees[code])
                text = render((TEMPLATES / "base.html").read_text(encoding="utf-8"), ctx, self.trees[code])
                out[f"{code}/{page['path']}index.html"] = tidy(text)
        out["index.html"] = tidy(self.root_page())
        out["404.html"] = tidy(self.not_found_page())
        out["sitemap.xml"] = self.sitemap()
        return out

    def chooser(self, root: str, path: str) -> str:
        return "\n".join(
            f'<li><a href="{root}{lang["code"]}/{path}" hreflang="{lang["code"]}" lang="{lang["code"]}"'
            f' dir="{lang["dir"]}" data-lang="{lang["code"]}"><span class="chooser-name">{html.escape(lang["name"])}</span>'
            f'<span class="chooser-tagline">{html.escape(lookup(self.trees[lang["code"]], "brand.tagline"))}</span></a></li>'
            for lang in self.langs
        )

    def small_page(self, template: str, extra: dict) -> str:
        ctx = self.context("en", self.pages[0])
        ctx.update(extra)
        return render((TEMPLATES / template).read_text(encoding="utf-8"), ctx, self.trees["en"])

    def root_page(self) -> str:
        alternates = "\n".join(
            f'<link rel="alternate" hreflang="{code}" href="{attr(self.base + code + "/")}">' for code in self.codes
        ) + f'\n<link rel="alternate" hreflang="x-default" href="{attr(self.base)}">'
        return self.small_page("root.html", {
            "root": "", "canonical": self.base, "alternates": alternates,
            "chooser": self.chooser("", ""), "langs_json": json.dumps(self.codes),
        })

    def not_found_page(self) -> str:
        messages = {
            code: {
                "title": lookup(self.trees[code], "notfound.title"),
                "text": lookup(self.trees[code], "notfound.text"),
                "home": lookup(self.trees[code], "notfound.home"),
                "dir": next(lang["dir"] for lang in self.langs if lang["code"] == code),
            }
            for code in self.codes
        }
        data = script_json(messages)
        # GitHub Pages serves 404.html at any depth: links and assets are absolute from the site root.
        return self.small_page("404.html", {"root": "/", "chooser": self.chooser("/", ""), "notfound_json": data})

    def sitemap(self) -> str:
        lines = ['<?xml version="1.0" encoding="UTF-8"?>',
                 '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:xhtml="http://www.w3.org/1999/xhtml">']
        for page in self.pages:
            if not page["index"]:
                continue
            for code in self.codes:
                lines.append("  <url>")
                lines.append(f"    <loc>{html.escape(self.page_url(code, page))}</loc>")
                for other in self.codes:
                    lines.append(f'    <xhtml:link rel="alternate" hreflang="{other}" href="{html.escape(self.page_url(other, page))}"/>')
                lines.append(f'    <xhtml:link rel="alternate" hreflang="x-default" href="{html.escape(self.x_default(page))}"/>')
                lines.append("  </url>")
        lines.append("</urlset>")
        return "\n".join(lines) + "\n"


def tidy(text: str) -> str:
    """Drops the blank lines and trailing spaces that the templates leave."""
    lines = [line.rstrip() for line in text.splitlines()]
    return "\n".join(line for line in lines if line.strip()) + "\n"


# --------------------------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="check translations and that the output is up to date")
    parser.add_argument("--todo", metavar="LANG", help="write i18n/LANG.todo.json with the missing English strings")
    parser.add_argument("--out", metavar="DIR", help="build a complete copy of the site in DIR")
    parser.add_argument("--api", metavar="URL", help="another API base URL (local testing only)")
    parser.add_argument("--only", metavar="LANGS", help="comma-separated languages (local testing only, with --out)")
    args = parser.parse_args()
    try:
        if args.only and not args.out:
            raise BuildError("--only is for local copies only: use it with --out")
        only = args.only.split(",") if args.only else None
        if only and "en" not in only:
            only.insert(0, "en")
        site = Site(args.api, only)
        if args.todo:
            if args.todo not in site.codes:
                raise BuildError(f"unknown language {args.todo}")
            todo = site.todo(args.todo)
            path = I18N / f"{args.todo}.todo.json"
            path.write_text(json.dumps(todo, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print(f"{path.relative_to(ROOT)}: {len(todo)} strings to translate")
            return 0
        files = site.build()
        if args.check:
            stale = [name for name, text in files.items()
                     if not (ROOT / name).exists() or (ROOT / name).read_text(encoding="utf-8") != text]
            if stale:
                print("out of date (run python3 tools/build.py):\n  " + "\n  ".join(stale))
                return 1
            print(f"ok: {len(site.codes)} languages, {len(files)} generated files up to date")
            return 0
        target = Path(args.out).resolve() if args.out else ROOT
        if args.out:
            if target == ROOT:
                raise BuildError("--out must be another directory")
            target.mkdir(parents=True, exist_ok=True)
            for name in STATIC:
                src = ROOT / name
                dst = target / name
                if dst.is_dir():
                    shutil.rmtree(dst)
                if src.is_dir():
                    shutil.copytree(src, dst)
                elif src.exists():
                    shutil.copy2(src, dst)
        elif args.api:
            raise BuildError("--api is for local copies only: use it with --out")
        for name, text in files.items():
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        print(f"wrote {len(files)} files for {len(site.codes)} languages to {target}")
        return 0
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
