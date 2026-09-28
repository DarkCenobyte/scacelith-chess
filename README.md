# Scacelith website

A self-contained, static, English-language landing page for **Scacelith**.
Designed for **https://scacelith.com/** and ready for GitHub Pages.

## Publish on GitHub Pages

1. Create a repository on GitHub, for example `scacelith-website`. A public repository works with GitHub Free.
2. Extract this ZIP. Upload **its contents** to the root of the repository, including `index.html`, `CNAME`, `.nojekyll`, and the complete `assets` folder. Do not upload the ZIP itself or nest everything inside another folder.
3. Commit to the `main` branch.
4. Open **Settings → Pages → Build and deployment**.
5. Set **Source** to **Deploy from a branch**, then choose **main** and **/(root)**. Click **Save**.
6. Under **Custom domain**, enter **scacelith.com** and click **Save**. Set the domain in GitHub before changing DNS. The included `CNAME` file already contains this hostname, but it does not replace the repository setting.
7. Configure the DNS records below with the service managing your domain's DNS.
8. Once GitHub's DNS check succeeds and its certificate is ready, enable **Enforce HTTPS** in Settings → Pages.

The project needs no build command, framework, Node.js installation, or custom GitHub Actions workflow. Future commits to the selected branch automatically publish updates.

### DNS for scacelith.com

Use these four A records for the apex domain:

| Type | Host / Name | Value |
| --- | --- | --- |
| A | `@` | `185.199.108.153` |
| A | `@` | `185.199.109.153` |
| A | `@` | `185.199.110.153` |
| A | `@` | `185.199.111.153` |
| CNAME | `www` | `YOUR_USERNAME.github.io` |

Replace `YOUR_USERNAME` with the GitHub account or organization that owns the repository. For a repository owned by **DarkCenobyte**, the CNAME target is **darkcenobyte.github.io**. Do not include `https://` or a repository name in the target. Some DNS providers use an empty name instead of `@` for the apex.

Replace conflicting web-hosting or parking records for `@` and `www`. Preserve unrelated records, especially mail (MX and email TXT) records. If old AAAA records point to another provider, remove them or replace them with the optional GitHub Pages IPv6 records below.

| Type | Host / Name | Value |
| --- | --- | --- |
| AAAA | `@` | `2606:50c0:8000::153` |
| AAAA | `@` | `2606:50c0:8001::153` |
| AAAA | `@` | `2606:50c0:8002::153` |
| AAAA | `@` | `2606:50c0:8003::153` |

With the apex configured as the custom domain and both record sets in place, GitHub Pages redirects `www.scacelith.com` to `scacelith.com`. DNS propagation and HTTPS certificate provisioning can take up to 24 hours.

Official GitHub documentation:

- [Configure a publishing source](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)
- [Manage a custom domain and DNS](https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site)

## Preview and edit

Open `index.html` in your browser for a local preview. Everything is local, including fonts and screenshots. Alternatively, serve this directory with any static HTTP server, for example `python -m http.server 8000`, and visit `http://localhost:8000`.

- **Text, sections, accessibility labels, SEO:** `index.html`
- **Colors, typography, responsive layout:** `assets/styles.css`
- **Screenshot viewer:** `assets/site.js`
- **Screenshots:** `assets/images/`
- **Fonts and their licenses:** `assets/fonts/`
- **Domain:** `CNAME`, the canonical and Open Graph URLs in `index.html`, `robots.txt`, and `sitemap.xml`

The page remains readable without JavaScript. Screenshot links then open the complete image directly. With JavaScript, a native dialog provides a gallery, previous/next controls, arrow-key navigation, Escape to close, and focus restoration. Motion respects the reduced-motion preference.

## Content and assets

- Promotes an ultra-realistic experience inspired by over-the-board chess.
- Marks the project **Work-in-Progress**, with no download link or announced release date.
- Credits development assistance from **Claude Opus 5.5**.
- Mentions **Stockfish integration for solo play in the game**. This static website does not run or distribute the engine.
- Uses four screenshots supplied by the game creator, encoded as WebP in two sizes. The homepage uses CSS framing; the gallery opens the uncropped screenshots. The original French game interface remains visible in the full screenshots.
- Uses self-hosted **Cormorant Garamond** and **Manrope**, distributed under the included SIL Open Font License texts. Font subsets contain Latin characters; system fonts cover other symbols.
- No analytics, cookies, account system, external font calls, or runtime third-party dependencies.

Screenshot mapping:

| Website asset | Supplied screenshot |
| --- | --- |
| `at-the-board` | `image(20260928-191125).png` |
| `the-grand-hall` | `image(20260928-191012).png` |
| `the-opponent` | `image(20260928-191112).png` |
| `the-scoresheet` | `image(20260928-191221).png` |

The font licenses apply to the fonts. This package does not assign a new license to the game, its screenshots, or its branding.
