# The Scacelith icon

An ivory marble S, crowned with a king's cross and standing on a chess piece's base, on a dark
marble medallion with a brass rim. Made for Scacelith on 2026-10-02; part of the project like the
rest of the repository (same licence).

| File | Used by |
|---|---|
| `scacelith.ico` (16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 px; 32-bit with alpha, the 256 px entry a PNG) | the Windows executable's icon resource (`res/scacelith.rc.in`, id 1, which the window class loads); embedded in the Linux build, whose window icon (`_NET_WM_ICON`) is read from its uncompressed entries (16 to 128 px) |
| `png/scacelith-<N>.png` (N = 16, 24, 32, 48, 64, 96, 128, 256, 512) | the Linux archive's `share/icons/hicolor/<N>x<N>/apps/scacelith.png` (the sizes the hicolor theme has) |
| `png/scacelith-<N>.png` (N = 16, 32, 64, 128, 256, 512, 1024) | the macOS app's `Scacelith.icns`, made by `tools/macos/make-app.sh` with `iconutil` |

The launcher that goes with them is `res/linux/scacelith.desktop` (`Icon=scacelith`,
`StartupWMClass=Scacelith`, the window's WM_CLASS).

Provenance: drawn and retouched with an image generation tool, then resized with ImageMagick
(Catmull-Rom below 64 px, Lanczos from 64 px). Every ICO entry was checked against the PNG of
the same size, alpha included, and the four corners are transparent at every size.
