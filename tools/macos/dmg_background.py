#!/usr/bin/env python3
"""The background of the macOS disk image's window, from a render of the game.

Writes res/macos/dmg-background.png (660 x 400, the window's size in points) and
dmg-background@2x.png (1320 x 800, for Retina screens; make-dmg.sh's dmgbuild joins the two into
one HiDPI TIFF). The layout matches tools/macos/dmg-settings.py: the app's icon centred at
(165, 196) and the Applications link's at (495, 196), 128 points wide.

What it draws, over the render darkened and warmed like the game's evening light:
- the title "Scacelith" in Cinzel (the game's title font, SIL Open Font License), with a rule;
- two frosted-glass tiles where Finder puts the icons. Finder writes the icon names under them in
  black (light appearance) or in white (dark appearance), so the tiles are blurred to an even
  mid-grey luminance (about 0.18, where black and white text both have a contrast ratio of about
  4.5) instead of the picture's dark, busy tones;
- an arrow from the app's tile to the Applications tile, "Drag Scacelith to Applications" and
  "Experimental build for Apple Silicon - macOS 26 or later" in EB Garamond (OFL).

Usage: python3 -I tools/macos/dmg_background.py <render.png> [--preview <dir>]
The render used (1320 x 800, the player's view of the robot across the board):
  tools/shot.sh game render.png 8 1320x800 --start --no-intro --human white --mouse 0.5,0.03
--preview also writes the window as Finder should show it (mock icons and labels, light and dark),
to judge the result without a Mac. Needs Pillow.
"""
import argparse
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
FONTS = os.path.join(ROOT, "assets", "fonts")
OUT = os.path.join(ROOT, "res", "macos")

W, H = 660, 400  # points
S = 2  # the master is drawn at 2x
ICON = 128
SLOTS = {"app": (165, 196), "apps": (495, 196)}  # icon centres, as in dmg-settings.py
TILE = (176, 186)  # tile size: the icon and its name below
TILE_TOP = 64 + 22  # from the icon's centre up to the tile's top edge
LABEL_LUMINANCE = 0.18

IVORY = (240, 230, 208)
GOLD_LIGHT = (236, 214, 160)
GOLD_DARK = (176, 138, 74)


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), round(size * S))


def srgb_to_linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


LUT = [srgb_to_linear(v) for v in range(256)]


def luminance(img, box):
    """Mean and 5th/95th percentile of the relative luminance of img in box (pixels)."""
    r, g, b = img.crop(box).convert("RGB").split()
    values = sorted(0.2126 * LUT[x] + 0.7152 * LUT[y] + 0.0722 * LUT[z]
                    for x, y, z in zip(r.tobytes(), g.tobytes(), b.tobytes()))
    n = len(values)
    return sum(values) / n, values[n // 20], values[n - 1 - n // 20]


def contrast(l1, l2):
    hi, lo = max(l1, l2), min(l1, l2)
    return (hi + 0.05) / (lo + 0.05)


def tile_box(centre):
    x, y = centre
    left = x - TILE[0] / 2
    top = y - TILE_TOP
    return tuple(round(v * S) for v in (left, top, left + TILE[0], top + TILE[1]))


def label_box(centre):
    """Where Finder writes the name: below the icon, about 13 points high."""
    x, y = centre
    return tuple(round(v * S) for v in (x - 60, y + ICON / 2 + 4, x + 60, y + ICON / 2 + 24))


def grade(render):
    """The render, darker and warmer, calmer in its details, with a vignette."""
    img = render.convert("RGB").resize((W * S, H * S), Image.LANCZOS)
    img = img.filter(ImageFilter.GaussianBlur(2.0 * S))
    # Exposure and a warm tone: multiply by a dim amber.
    img = ImageChops.multiply(img, Image.new("RGB", img.size, (132, 112, 90)))
    # Vignette: dark corners, and a deeper bottom where the text stands.
    vignette = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(vignette)
    d.ellipse((-0.15 * W * S, -0.35 * H * S, 1.15 * W * S, 1.05 * H * S), fill=255)
    vignette = vignette.filter(ImageFilter.GaussianBlur(70 * S))
    img = Image.composite(img, Image.new("RGB", img.size, (8, 6, 5)), vignette)
    shade = Image.linear_gradient("L").resize(img.size)  # 0 at the top, 255 at the bottom
    shade = shade.point(lambda v: max(0, int((v - 150) * 1.9)) if v > 150 else 0)
    img = Image.composite(Image.new("RGB", img.size, (10, 8, 6)), img, shade.point(lambda v: min(v, 225)))
    top = Image.linear_gradient("L").rotate(180).resize(img.size).point(lambda v: max(0, v - 150))
    img = Image.composite(Image.new("RGB", img.size, (10, 8, 6)), img, top)
    return img


def frosted_tile(base, box):
    """A rounded tile of frosted glass: the picture behind, blurred and brought to an even
    mid-grey luminance, with a light rim and a shadow."""
    x0, y0, x1, y1 = box
    radius = 26 * S
    pad = 40 * S
    region = base.crop((x0 - pad, y0 - pad, x1 + pad, y1 + pad)).filter(ImageFilter.GaussianBlur(16 * S))
    region = region.crop((pad, pad, pad + x1 - x0, pad + y1 - y0))
    # Desaturate a little, then scale towards the target luminance under the label.
    grey = region.convert("L").convert("RGB")
    region = Image.blend(region, grey, 0.35)
    glass = Image.new("RGB", region.size, (128, 120, 108))
    region = Image.blend(region, glass, 0.55)

    mask = Image.new("L", region.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, x1 - x0 - 1, y1 - y0 - 1), radius, fill=255)

    # Shadow under the tile.
    shadow = Image.new("L", base.size, 0)
    ImageDraw.Draw(shadow).rounded_rectangle((x0, y0 + 6 * S, x1, y1 + 10 * S), radius, fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(12 * S))
    base = Image.composite(Image.new("RGB", base.size, (0, 0, 0)), base, shadow)
    base.paste(region, (x0, y0), mask)

    # Rim: a thin light edge, brighter along the top (light from above).
    rim = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(rim)
    d.rounded_rectangle((x0, y0, x1 - 1, y1 - 1), radius, outline=(255, 246, 228, 70), width=max(1, S))
    sheen = Image.new("L", (x1 - x0, y1 - y0), 0)
    ImageDraw.Draw(sheen).rounded_rectangle((0, 0, x1 - x0 - 1, (y1 - y0) // 2), radius, fill=26)
    sheen = Image.composite(sheen, Image.new("L", sheen.size, 0), mask)
    sheen_rgba = Image.new("RGBA", sheen.size, (255, 250, 240, 0))
    sheen_rgba.putalpha(sheen.filter(ImageFilter.GaussianBlur(6 * S)))
    rim.alpha_composite(sheen_rgba, (x0, y0))
    base = base.convert("RGBA")
    base.alpha_composite(rim)
    return base.convert("RGB")


def match_label_luminance(img, centre):
    """Scales the tile's lower part so that the mean luminance under the label is the target."""
    box = tile_box(centre)
    lb = label_box(centre)
    for _ in range(6):
        mean, _, _ = luminance(img, lb)
        if abs(mean - LABEL_LUMINANCE) < 0.004:
            break
        # A gain in linear light, applied as a gamma-corrected factor to the tile.
        factor = (LABEL_LUMINANCE / mean) ** (1 / 2.2)
        tile = img.crop(box)
        tile = tile.point(lambda v, f=factor: min(255, int(v * f + 0.5)))
        mask = Image.new("L", tile.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, tile.size[0] - 1, tile.size[1] - 1), 26 * S, fill=255)
        img.paste(tile, box[:2], mask)
    return img


def draw_text_centred(layer, xy, text, fnt, fill, tracking=0.0, shadow=None):
    """Text centred on xy (points), with letter spacing (in em) and an optional soft shadow."""
    d = ImageDraw.Draw(layer)
    size = fnt.size
    widths = [d.textlength(ch, font=fnt) for ch in text]
    total = sum(widths) + tracking * size * (len(text) - 1)
    x = xy[0] * S - total / 2
    y = xy[1] * S
    if shadow:
        sh = Image.new("RGBA", layer.size, (0, 0, 0, 0))
        sd = ImageDraw.Draw(sh)
        sx = x
        for ch, w in zip(text, widths):
            sd.text((sx, y + shadow[1] * S), ch, font=fnt, fill=shadow[0], anchor="ls")
            sx += w + tracking * size
        layer.alpha_composite(sh.filter(ImageFilter.GaussianBlur(shadow[2] * S)))
    for ch, w in zip(text, widths):
        d.text((x, y), ch, font=fnt, fill=fill, anchor="ls")
        x += w + tracking * size
    return total


def gold_gradient(size, horizontal=True):
    grad = Image.linear_gradient("L")
    grad = grad.rotate(90 if horizontal else 0).resize(size)
    return Image.merge("RGB", [grad.point(lambda v, a=a, b=b: int(a + (b - a) * v / 255))
                               for a, b in zip(GOLD_LIGHT, GOLD_DARK)])


def draw_title(img):
    layer = img.convert("RGBA")
    cinzel = font("Cinzel.ttf", 36)
    cinzel.set_variation_by_axes([600])
    text_layer = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    width = draw_text_centred(text_layer, (W / 2, 62), "Scacelith", cinzel, (255, 255, 255, 255), tracking=0.12)
    # Fill the letters with a vertical gold gradient.
    alpha = text_layer.getchannel("A")
    gold = gold_gradient(layer.size, horizontal=False)
    gold = gold.crop((0, 0) + layer.size)
    top, bottom = 30 * S, 64 * S
    grad = Image.linear_gradient("L").resize((layer.size[0], bottom - top))
    ramp = Image.new("L", layer.size, 0)
    ramp.paste(grad, (0, top))
    ramp.paste(255, (0, bottom, layer.size[0], layer.size[1]))
    gold = Image.composite(Image.new("RGB", layer.size, GOLD_DARK), Image.new("RGB", layer.size, (250, 236, 200)), ramp)
    shadow = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    shadow.putalpha(alpha.filter(ImageFilter.GaussianBlur(3 * S)).point(lambda v: v * 200 // 255))
    layer.alpha_composite(shadow, (0, 2 * S))
    letters = gold.convert("RGBA")
    letters.putalpha(alpha)
    layer.alpha_composite(letters)

    # A rule under the title: two hairlines and a small lozenge, like the game's menus.
    d = ImageDraw.Draw(layer)
    y = 80 * S
    half = width / 2 / S + 6
    for sign in (-1, 1):
        x_in = W / 2 + sign * 10
        x_out = W / 2 + sign * half
        line = Image.new("L", (round(abs(x_out - x_in) * S), max(1, S)), 0)
        line = Image.linear_gradient("L").rotate(90 if sign < 0 else -90).resize(line.size)
        rgba = Image.new("RGBA", line.size, GOLD_LIGHT + (0,))
        rgba.putalpha(line.point(lambda v: v * 170 // 255))
        layer.alpha_composite(rgba, (round(min(x_in, x_out) * S), y))
    cx, r = W / 2 * S, 3.5 * S
    d.polygon([(cx, y - r + S / 2), (cx + r, y + S / 2), (cx, y + r + S / 2), (cx - r, y + S / 2)], fill=GOLD_LIGHT + (210,))
    return layer.convert("RGB")


def draw_arrow(img):
    """A tapering gold arrow from the app's tile to the Applications tile, over a soft shadow
    band that keeps it clear of what the picture shows behind."""
    x0 = (SLOTS["app"][0] + TILE[0] / 2 + 14) * S
    x1 = (SLOTS["apps"][0] - TILE[0] / 2 - 14) * S
    y = SLOTS["app"][1] * S
    head = 20 * S
    shape = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(shape)
    # Shaft: widens from a point to 5 points, then the head with swept-back barbs.
    d.polygon([(x0 + 18 * S, y), (x1 - head + 2 * S, y - 2.5 * S), (x1 - head + 2 * S, y + 2.5 * S)], fill=255)
    d.polygon([(x1 - head - 3 * S, y - 11 * S), (x1, y), (x1 - head - 3 * S, y + 11 * S),
               (x1 - head + 4 * S, y)], fill=255)
    # Dots trailing behind, like the squares a piece crosses.
    for i, dx in enumerate((0, 7, 13)):
        r = (1.1 + 0.5 * i) * S
        cx = x0 + dx * S
        d.ellipse((cx - r, y - r, cx + r, y + r), fill=120 + 55 * i)
    band = Image.new("L", img.size, 0)
    ImageDraw.Draw(band).ellipse((x0 - 10 * S, y - 22 * S, x1 + 10 * S, y + 22 * S), fill=150)
    band = band.filter(ImageFilter.GaussianBlur(14 * S))
    base = Image.composite(Image.new("RGB", img.size, (12, 9, 6)), img, band).convert("RGBA")
    glow = Image.new("RGBA", img.size, (255, 206, 120, 0))
    glow.putalpha(shape.filter(ImageFilter.GaussianBlur(8 * S)).point(lambda v: v * 150 // 255))
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    shadow.putalpha(shape.filter(ImageFilter.GaussianBlur(2 * S)).point(lambda v: v * 170 // 255))
    gold = gold_gradient(img.size).convert("RGBA")
    gold.putalpha(shape)
    base.alpha_composite(glow)
    base.alpha_composite(shadow, (0, 2 * S))
    base.alpha_composite(gold)
    return base.convert("RGB")


def draw_texts(img):
    layer = img.convert("RGBA")
    italic = font("EBGaramond12-Italic.ttf", 21)
    draw_text_centred(layer, (W / 2, 334), "Drag Scacelith to Applications", italic, IVORY + (255,),
                      tracking=0.01, shadow=((0, 0, 0, 230), 1.2, 2.5))
    small = font("EBGaramond12-Regular.ttf", 12.5)
    draw_text_centred(layer, (W / 2, 360), "Experimental build for Apple Silicon – macOS 26 or later", small,
                      IVORY + (175,), tracking=0.06, shadow=((0, 0, 0, 200), 1, 2))
    return layer.convert("RGB")


def compose(render):
    img = grade(render)
    for centre in SLOTS.values():
        img = frosted_tile(img, tile_box(centre))
        img = match_label_luminance(img, centre)
    img = draw_title(img)
    img = draw_arrow(img)
    img = draw_texts(img)
    return img


def mock_applications_icon(size):
    """A stand-in for macOS's Applications folder icon (a blue folder with the App Store's "A")."""
    s = size / 128
    icon = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(icon)
    d.rounded_rectangle((10 * s, 22 * s, 58 * s, 40 * s), 6 * s, fill=(80, 160, 230, 255))
    d.rounded_rectangle((10 * s, 30 * s, 118 * s, 108 * s), 9 * s, fill=(98, 178, 240, 255))
    d.rounded_rectangle((10 * s, 40 * s, 118 * s, 108 * s), 9 * s, fill=(120, 196, 248, 255))
    f = ImageFont.truetype("/usr/share/fonts/opentype/inter/Inter-Bold.otf", round(44 * s)) \
        if os.path.exists("/usr/share/fonts/opentype/inter/Inter-Bold.otf") else ImageFont.load_default()
    d.text((64 * s, 76 * s), "A", font=f, fill=(60, 120, 190, 255), anchor="mm")
    return icon


def preview(background, out_dir, version="1.0.0-beta.2"):
    """The Finder window as it should look, at 2x: title bar, icons, names (light and dark)."""
    os.makedirs(out_dir, exist_ok=True)
    app_icon = Image.open(os.path.join(ROOT, "res", "icons", "png", "scacelith-256.png")).convert("RGBA")
    apps_icon = mock_applications_icon(ICON * S)
    label_font_path = "/usr/share/fonts/opentype/inter/Inter-Regular.otf"
    label_font = ImageFont.truetype(label_font_path, 13 * S) if os.path.exists(label_font_path) \
        else ImageFont.load_default()
    for mode, text_colour, bar in (("light", (0, 0, 0), (236, 236, 236)), ("dark", (255, 255, 255), (50, 50, 52))):
        bar_h = 28 * S
        win = Image.new("RGB", (W * S, H * S + bar_h), bar)
        win.paste(background, (0, bar_h))
        d = ImageDraw.Draw(win)
        for i, c in enumerate(((255, 95, 87), (254, 188, 46), (40, 200, 64))):
            cx, cy, r = (20 + 20 * i) * S, bar_h / 2, 6 * S
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=c)
        d.text((W * S / 2, bar_h / 2), f"Scacelith {version}", font=label_font, fill=text_colour, anchor="mm")
        for key, icon, name in (("app", app_icon, "Scacelith"), ("apps", apps_icon, "Applications")):
            cx, cy = SLOTS[key]
            ic = icon.resize((ICON * S, ICON * S), Image.LANCZOS)
            win.paste(ic, (round((cx - ICON / 2) * S), round((cy - ICON / 2) * S) + bar_h), ic)
            d.text((cx * S, (cy + ICON / 2 + 15) * S + bar_h), name, font=label_font, fill=text_colour, anchor="mm")
        path = os.path.join(out_dir, f"dmg-window-{mode}@2x.png")
        win.save(path, optimize=True)
        win.resize((W, H + 28), Image.LANCZOS).save(os.path.join(out_dir, f"dmg-window-{mode}.png"), optimize=True)
        print("preview:", path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("render", help="a 1320x800 render of the game (see the module's docstring)")
    parser.add_argument("--preview", metavar="DIR", help="also write mock Finder windows there")
    parser.add_argument("--out", default=OUT, help="output folder (default: res/macos)")
    args = parser.parse_args()

    master = compose(Image.open(args.render))
    os.makedirs(args.out, exist_ok=True)
    hi = os.path.join(args.out, "dmg-background@2x.png")
    lo = os.path.join(args.out, "dmg-background.png")
    # 72 and 144 dpi: the resolutions of a HiDPI pair, which tiffutil -cathidpicheck (dmgbuild
    # joins the two images with it) expects, and which tell Finder the size in points.
    master.save(hi, optimize=True, dpi=(144, 144))
    master.resize((W, H), Image.LANCZOS).save(lo, optimize=True, dpi=(72, 72))
    for path in (lo, hi):
        size = os.path.getsize(path)
        print(f"{path}: {Image.open(path).size[0]}x{Image.open(path).size[1]}, {size / 1e6:.2f} MB")
        if size > 1.5e6:
            sys.exit(f"{path} is larger than 1.5 MB")

    # The names Finder writes under the icons: contrast of black and of white text there.
    for key, centre in SLOTS.items():
        mean, p5, p95 = luminance(master, label_box(centre))
        print(f"{key}: luminance under the name {mean:.3f} (5%: {p5:.3f}, 95%: {p95:.3f}); "
              f"contrast with black text {contrast(mean, 0):.2f} (worst {contrast(p5, 0):.2f}), "
              f"with white text {contrast(mean, 1):.2f} (worst {contrast(p95, 1):.2f})")
    if args.preview:
        preview(master, args.preview)


if __name__ == "__main__":
    main()
