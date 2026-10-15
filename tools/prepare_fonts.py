#!/usr/bin/env python3
"""Prepares the Arabic, CJK, Korean, Devanagari and Greek handwriting fonts embedded in Scacelith
(reproducible; needs fontTools and uharfbuzz).

    tools/prepare_fonts.py --src DIR [--root REPO] [--only GROUPS]

GROUPS (comma-separated, default all): cjk, arabic, korean, devanagari, mynerve.

Sources (all SIL Open Font License 1.1), downloaded into the --src directory:
    ArefRuqaa-Regular.ttf       google/fonts ofl/arefruqaa          -> Arabic handwriting
    Amiri-Regular.ttf           google/fonts ofl/amiri              -> Arabic UI text
    KleeOne-Regular.ttf         google/fonts ofl/kleeone            -> Japanese handwriting + UI
    LXGWWenKai-Regular.ttf      lxgw/LxgwWenKai fonts/TTF           -> Simplified Chinese
    LXGWWenKaiTC-Regular.ttf    google/fonts ofl/lxgwwenkaitc       -> Traditional Chinese
    GowunBatang-Regular.ttf     google/fonts ofl/gowunbatang        -> Korean UI text
    NanumPenScript-Regular.ttf  google/fonts ofl/nanumpenscript     -> Korean handwriting
    TiroDevanagariHindi-Regular.ttf  google/fonts ofl/tirodevanagarihindi -> Hindi UI text
    Kalam-Regular.ttf           google/fonts ofl/kalam              -> Hindi handwriting
    Mynerve-Regular.ttf         google/fonts ofl/mynerve            -> Greek and Vietnamese handwriting
    OFL-*.txt                   the licence text of each family

What it does:
  * CJK: subsets to the characters a player name or the UI can reasonably need: GB2312 hanzi
    (Simplified), Big5 levels 1 and 2 (Traditional), JIS X 0208 kanji (Japanese), plus kana, CJK
    punctuation, full-width forms, Latin and every character used by the ja / zh-Hans / zh-Hant
    translations (assets/i18n/*.lang) and coach texts (assets/coach/speech/<code>/*.lang, the
    subtitles, and assets/coach/openings/*.lang): rerun this script after changing them.
  * Arabic: stb_truetype does no OpenType shaping, so the game maps letters to the Unicode
    presentation forms (U+FB50..U+FDFF, U+FE70..U+FEFF) itself (src/i18n/unicode.cpp). Amiri maps
    those codepoints already. Aref Ruqaa builds its contextual forms with GSUB multiple
    substitutions + GPOS mark / cursive positioning instead: each letter is shaped with HarfBuzz
    in isolated / final / initial / medial context (tatweel neighbours), the glyphs of the letter's
    cluster become one composite glyph and the presentation-form codepoint is mapped to it (also
    the lam-alef ligatures U+FEF5..U+FEFC).
  * Korean: Hangul syllables of KS X 1001 (the 2,350 in common use) plus every syllable of the ko
    translations, compatibility jamo, CJK punctuation; rerun after changing the ko texts.
  * Devanagari: stb_truetype cannot reorder or combine Devanagari either. Every orthographic
    syllable of the hi translations (plus each consonant with each vowel sign and bindu, for names)
    that a font draws otherwise than its plain glyphs side by side is shaped with HarfBuzz and
    becomes one composite glyph, mapped to a Private Use codepoint (U+E000 on, the same in both
    fonts). assets/fonts/devanagari-clusters.txt lists them; src/i18n/unicode.cpp (shapeDevanagari)
    replaces each syllable of a text by its codepoint when it draws it. Rerun after changing the hi
    texts (the unit test devanagari_clusters_cover_the_hindi_texts says when).
  * Drops hinting and every table stb_truetype does not read (it uses cmap, glyf/loca, head, hhea,
    hmtx, maxp and GPOS pair kerning; only the 'kern' feature is kept so vertical or contextual
    positioning lookups never leak into horizontal kerning).
  * Renames the families (the OFL reserves some of the original names for modified versions) and
    records the origin in the name table; copyright and licence entries are kept.
"""
import argparse
import os
import shutil
import sys
import unicodedata

from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables import _g_l_y_f
import uharfbuzz as hb

# ---------------------------------------------------------------------------------------------
# Character sets
# ---------------------------------------------------------------------------------------------
def rng(a, b):
    return set(range(a, b + 1))


LATIN = rng(0x20, 0x7E) | rng(0xA0, 0xFF)
PUNCT = rng(0x2010, 0x2027) | rng(0x2030, 0x203A) | {0x2116, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212}
CJK_COMMON = (rng(0x3000, 0x303F)     # CJK symbols and punctuation
              | rng(0x3040, 0x309F)   # hiragana
              | rng(0x30A0, 0x30FF)   # katakana
              | rng(0x31F0, 0x31FF)   # katakana phonetic extensions
              | rng(0xFF01, 0xFF5E)   # full-width ASCII
              | rng(0xFF5F, 0xFF65)   # full-width brackets, half-width CJK punctuation
              | rng(0xFFE0, 0xFFE6))  # full-width signs


def decode_table(codec, leads, trails):
    out = set()
    for a in leads:
        for b in trails:
            try:
                s = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.add(ord(s))
    return out


def is_han(cp):
    return 0x3400 <= cp <= 0x4DBF or 0x4E00 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x3134F


def gb2312_hanzi():  # rows 16..87
    return {c for c in decode_table("gb2312", range(0xB0, 0xF8), range(0xA1, 0xFF)) if is_han(c)}


def big5_hanzi(level2=True):
    trails = list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
    lv1 = decode_table("big5", range(0xA4, 0xC7), trails)  # A440..C67E
    out = {c for c in lv1 if is_han(c)}
    if level2:
        out |= {c for c in decode_table("big5", range(0xC9, 0xFA), trails) if is_han(c)}  # C940..F9D5
    return out


def jis0208_kanji():  # EUC-JP rows 16..84 (levels 1 and 2)
    return {c for c in decode_table("euc_jp", range(0xB0, 0xF5), range(0xA1, 0xFF)) if is_han(c)}


def ksx1001_hangul():  # EUC-KR rows 16..40 (the 2,350 precomposed syllables)
    return {c for c in decode_table("euc_kr", range(0xB0, 0xC9), range(0xA1, 0xFF)) if 0xAC00 <= c <= 0xD7A3}


def lang_chars(root, codes):
    chars = set()
    dirs = [("i18n",), ("coach", "speech"), ("coach", "openings")]
    for code in codes:
        paths = []
        for d in dirs:
            # One file per language (assets/i18n/<code>.lang) or a folder of topic files
            # (assets/coach/speech/<code>/*.lang: common, events, lesson, review, appraisal).
            path = os.path.join(root, "assets", *d, code + ".lang")
            if os.path.exists(path):
                paths.append(path)
            folder = os.path.join(root, "assets", *d, code)
            if os.path.isdir(folder):
                paths += [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.endswith(".lang")]
        for path in paths:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    chars |= {ord(c) for c in line.split("=", 1)[1]}
    return {c for c in chars if c >= 0x2E80}


# ---------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------
KEEP_NAME_IDS = [0, 1, 2, 3, 4, 5, 6, 10, 13, 14]


def rename(font, family, origin, note=""):
    """New family name (OFL reserved names must not be used by modified versions) + provenance."""
    if "Ruqaa" in origin:
        note = ", Arabic contextual forms baked into the presentation-form codepoints"
    name = font["name"]
    ps = family.replace(" ", "") + "-Regular"
    keep = []
    for rec in name.names:
        if rec.nameID in (0, 13, 14):
            keep.append(rec)
    name.names = keep
    for pid, eid, lid in ((3, 1, 0x409), (1, 0, 0)):
        name.setName(family, 1, pid, eid, lid)
        name.setName("Regular", 2, pid, eid, lid)
        name.setName(family + " Regular;scacelith", 3, pid, eid, lid)
        name.setName(family + " Regular", 4, pid, eid, lid)
        name.setName("Version 1.0 (Scacelith build)", 5, pid, eid, lid)
        name.setName(ps, 6, pid, eid, lid)
        name.setName("Modified version of " + origin + " for the Scacelith chess game (subset, "
                     "hinting removed" + note + ").", 10, pid, eid, lid)


def subset_font(font, unicodes, keep_kern=True):
    opts = subset.Options()
    opts.layout_features = ["kern"] if keep_kern else []
    opts.hinting = False
    opts.notdef_outline = True
    opts.glyph_names = False
    opts.name_IDs = KEEP_NAME_IDS
    opts.name_languages = ["*"]
    opts.drop_tables += ["vhea", "vmtx", "BASE", "DSIG", "meta", "gasp", "prep", "fpgm", "cvt ", "hdmx", "VDMX",
                         "LTSH", "JSTF", "MATH", "COLR", "CPAL", "SVG ", "STAT"]
    if not keep_kern:
        opts.drop_tables += ["GSUB", "GPOS", "GDEF"]
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(unicodes))
    sub.subset(font)
    # GSUB is never read by the game (no OpenType shaping).
    for t in ("GSUB",):
        if t in font:
            del font[t]
    if "post" in font:
        font["post"].formatType = 3.0  # no glyph names
        font["post"].extraNames = []
        font["post"].mapping = {}


def save(font, path):
    # Keep the source's modification date: the same sources give byte-identical files.
    font.recalcTimestamp = False
    font.save(path)
    return os.path.getsize(path)


# ---------------------------------------------------------------------------------------------
# Arabic: presentation forms
# ---------------------------------------------------------------------------------------------
FORM_TAGS = {"<isolated>": 0, "<final>": 1, "<initial>": 2, "<medial>": 3}
TATWEEL = "ـ"


def presentation_forms():
    """{(base codepoints...): {form index: presentation codepoint}} for single letters and the
    lam-alef ligatures."""
    out = {}
    for cp in list(range(0xFB50, 0xFE00)) + list(range(0xFE70, 0xFF00)):
        d = unicodedata.decomposition(chr(cp))
        if not d:
            continue
        parts = d.split()
        if parts[0] not in FORM_TAGS:
            continue
        bases = tuple(int(p, 16) for p in parts[1:])
        if len(bases) == 1 or (len(bases) == 2 and bases[0] == 0x644 and bases[1] in (0x622, 0x623, 0x625, 0x627)):
            out.setdefault(bases, {})[FORM_TAGS[parts[0]]] = cp
    return out


def bake_arabic_forms(path_in, font):
    """Adds one composite glyph per contextual form of every letter (shaped by HarfBuzz) and maps
    the presentation-form codepoint to it. Returns the set of codepoints added."""
    blob = hb.Blob.from_file_path(path_in)
    hbfont = hb.Font(hb.Face(blob))
    order = list(font.getGlyphOrder())  # original glyph ids (HarfBuzz output)
    cmap = font.getBestCmap()
    tatweel_gid = font.getGlyphID(cmap[0x640]) if 0x640 in cmap else -1
    glyf = font["glyf"]
    hmtx = font["hmtx"]
    added = {}
    for bases, forms in sorted(presentation_forms().items()):
        if any(b not in cmap for b in bases):
            continue
        text = "".join(chr(b) for b in bases)
        for form, pcp in sorted(forms.items()):
            if pcp in cmap:
                continue
            before = TATWEEL if form in (1, 3) else ""  # joined to the previous (right) letter
            after = TATWEEL if form in (2, 3) else ""   # joined to the next (left) letter
            s = before + text + after
            first, last = len(before), len(before) + len(text) - 1
            buf = hb.Buffer()
            buf.add_str(s)
            buf.guess_segment_properties()
            hb.shape(hbfont, buf, {})
            pen = 0
            comps = []
            x0, adv = None, 0
            for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
                if first <= info.cluster <= last and info.codepoint != tatweel_gid:
                    if x0 is None:
                        x0 = pen
                    comps.append((order[info.codepoint], pen + pos.x_offset, pos.y_offset))
                    adv += pos.x_advance
                pen += pos.x_advance
            if not comps:
                continue
            name = "uni%04X" % pcp
            if len(comps) == 1 and comps[0][1] - x0 == 0 and comps[0][2] == 0 and hmtx[comps[0][0]][0] == adv:
                target = comps[0][0]  # the form is an existing glyph as is
            else:
                g = _g_l_y_f.Glyph()
                g.numberOfContours = -1
                g.components = []
                for gname, x, y in comps:
                    c = _g_l_y_f.GlyphComponent()
                    c.glyphName = gname
                    c.x, c.y = int(x - x0), int(y)
                    c.flags = 0x4  # ROUND_XY_TO_GRID (harmless, no hinting)
                    g.components.append(c)
                glyf[name] = g  # also appends the name to the glyph order
                g.recalcBounds(glyf)
                hmtx[name] = (max(0, adv), getattr(g, "xMin", 0))
                target = name
            added[pcp] = target
    font.setGlyphOrder(list(glyf.glyphOrder))
    for table in font["cmap"].tables:
        if table.isUnicode():
            for cp, gname in added.items():
                if cp <= 0xFFFF or table.format in (12, 13):
                    table.cmap[cp] = gname
    font["maxp"].numGlyphs = len(glyf.glyphOrder)
    return set(added)


# ---------------------------------------------------------------------------------------------
# Devanagari: orthographic syllables baked into Private Use codepoints
# ---------------------------------------------------------------------------------------------
# The segmentation must stay the same as uni::devanagariSyllables (src/i18n/unicode.cpp).
DEVA_PUA_FIRST, DEVA_PUA_LAST = 0xE000, 0xEFFF


def deva_class(c):
    if 0x0915 <= c <= 0x0939 or 0x0958 <= c <= 0x095F or 0x0978 <= c <= 0x097F:
        return "C"
    if 0x0904 <= c <= 0x0914 or 0x0960 <= c <= 0x0961 or 0x0972 <= c <= 0x0977:
        return "V"
    if c == 0x093C:
        return "N"
    if c == 0x094D:
        return "H"
    if 0x093A <= c <= 0x093B or 0x093E <= c <= 0x094C or 0x094E <= c <= 0x094F or 0x0955 <= c <= 0x0957 \
            or 0x0962 <= c <= 0x0963:
        return "M"
    if 0x0900 <= c <= 0x0903:
        return "B"
    if c in (0x200C, 0x200D):
        return "J"
    return ""


def deva_syllables(text):
    """Splits a string into Devanagari orthographic syllables (other characters alone)."""
    cps = [ord(ch) for ch in text]
    out, i, n = [], 0, len(cps)
    while i < n:
        k = deva_class(cps[i])
        j = i + 1
        if k in ("C", "V"):
            if j < n and deva_class(cps[j]) == "N":
                j += 1
            while k == "C" and j < n and deva_class(cps[j]) == "H":
                j += 1
                if j < n and deva_class(cps[j]) == "J":
                    j += 1
                if j < n and deva_class(cps[j]) == "C":
                    j += 1
                    if j < n and deva_class(cps[j]) == "N":
                        j += 1
                else:
                    break
            while j < n and deva_class(cps[j]) in ("M", "N"):
                j += 1
            while j < n and deva_class(cps[j]) == "B":
                j += 1
        out.append("".join(chr(c) for c in cps[i:j]))
        i = j
    return out


def lang_values(root, code):
    """Every displayed value of a language: UI, coach subtitles (not the .spoken lines), openings."""
    vals = []
    paths = [os.path.join(root, "assets", "i18n", code + ".lang"),
             os.path.join(root, "assets", "coach", "openings", code + ".lang")]
    folder = os.path.join(root, "assets", "coach", "speech", code)
    if os.path.isdir(folder):
        paths += [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.endswith(".lang")]
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k.strip().endswith(".spoken"):
                    continue
                vals.append(v.strip())
    return vals


def deva_candidates(root):
    """Syllables to bake: those of the hi texts and of the language's native name, and every
    consonant (with the usual nukta letters) with every vowel sign, virama and bindu."""
    found = set()
    for v in lang_values(root, "hi") + ["\u0939\u093F\u0928\u094D\u0926\u0940"]:   # हिन्दी
        for syl in deva_syllables(v):
            if len(syl) > 1 and deva_class(ord(syl[0])) in ("C", "V"):
                found.add(syl)
    cons = [chr(c) for c in range(0x0915, 0x093A)]
    cons += [chr(c) + "\u093C" for c in (0x0915, 0x0916, 0x0917, 0x091C, 0x0921, 0x0922, 0x092B, 0x092F)]
    signs = [""] + [chr(c) for c in range(0x093E, 0x094D)] + ["\u094D"]
    for c in cons:
        for m in signs:
            for b in ("", "\u0902", "\u0901"):
                syl = c + m + b
                if len(syl) > 1:
                    found.add(syl)
    for v in [chr(c) for c in range(0x0905, 0x0915)]:
        for b in ("\u0902", "\u0901", "\u0903"):
            found.add(v + b)
    return found


def shape_cluster(hbfont, text):
    buf = hb.Buffer()
    buf.add_str(text)
    buf.guess_segment_properties()
    hb.shape(hbfont, buf, {})
    return list(zip(buf.glyph_infos, buf.glyph_positions))


def needs_baking(font, hbfont, text):
    """True when the font draws the syllable otherwise than its plain glyphs side by side."""
    cmap = font.getBestCmap()
    if any(ord(ch) not in cmap for ch in text):
        return None   # the font cannot draw it
    order = font.getGlyphOrder()
    shaped = shape_cluster(hbfont, text)
    if len(shaped) != len(text):
        return True
    hmtx = font["hmtx"]
    for (info, pos), ch in zip(shaped, text):
        name = order[info.codepoint]
        if name != cmap[ord(ch)] or pos.x_offset or pos.y_offset or pos.x_advance != hmtx[name][0]:
            return True
    return False


def bake_clusters(path_in, font, table):
    """Adds the composite glyph of every syllable of table {syllable: codepoint} and maps the
    codepoint to it. Returns the number of glyphs added."""
    hbfont = hb.Font(hb.Face(hb.Blob.from_file_path(path_in)))
    order = list(font.getGlyphOrder())
    glyf, hmtx = font["glyf"], font["hmtx"]
    mapped, added = {}, 0
    for syl, pcp in sorted(table.items(), key=lambda kv: kv[1]):
        comps, pen, adv = [], 0, 0
        for info, pos in shape_cluster(hbfont, syl):
            comps.append((order[info.codepoint], pen + pos.x_offset, pos.y_offset))
            pen += pos.x_advance
            adv += pos.x_advance
        if len(comps) == 1 and comps[0][1] == 0 and comps[0][2] == 0 and hmtx[comps[0][0]][0] == adv:
            mapped[pcp] = comps[0][0]
            continue
        g = _g_l_y_f.Glyph()
        g.numberOfContours = -1
        g.components = []
        for gname, x, y in comps:
            c = _g_l_y_f.GlyphComponent()
            c.glyphName = gname
            c.x, c.y = int(x), int(y)
            c.flags = 0x4
            g.components.append(c)
        name = "deva%04X" % pcp
        glyf[name] = g
        g.recalcBounds(glyf)
        hmtx[name] = (max(0, adv), getattr(g, "xMin", 0))
        mapped[pcp] = name
        added += 1
    font.setGlyphOrder(list(glyf.glyphOrder))
    for t in font["cmap"].tables:
        if t.isUnicode():
            t.cmap.update(mapped)
    font["maxp"].numGlyphs = len(glyf.glyphOrder)
    return added


def devanagari_table(root, src, fonts):
    """{syllable: Private Use codepoint} of the syllables some font must bake, in a stable order."""
    want = set()
    for fname in fonts:
        path = os.path.join(src, fname)
        font = TTFont(path)
        hbfont = hb.Font(hb.Face(hb.Blob.from_file_path(path)))
        for syl in deva_candidates(root):
            if needs_baking(font, hbfont, syl):
                want.add(syl)
    ordered = sorted(want, key=lambda t: [ord(c) for c in t])
    if len(ordered) > DEVA_PUA_LAST - DEVA_PUA_FIRST + 1:
        raise SystemExit("too many Devanagari syllables: %d" % len(ordered))
    return {syl: DEVA_PUA_FIRST + i for i, syl in enumerate(ordered)}


def write_cluster_list(path, table):
    lines = ["# Devanagari syllables drawn as one precomposed glyph (Private Use codepoints) by the game's",
             "# Devanagari fonts. GENERATED by tools/prepare_fonts.py with the fonts: do not edit by hand.",
             "# <codepoint> <syllable>"]
    for syl, cp in sorted(table.items(), key=lambda kv: kv[1]):
        lines.append("%04X %s" % (cp, syl))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="directory holding the upstream fonts and OFL texts")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--no-big5-level2", action="store_true", help="only Big5 level 1 hanzi for Traditional Chinese")
    ap.add_argument("--only", default="cjk,arabic,korean,devanagari,mynerve",
                    help="comma-separated groups to prepare: cjk, arabic, korean, devanagari, mynerve")
    args = ap.parse_args()
    groups = set(args.only.split(","))
    src, root = args.src, args.root
    hand = os.path.join(root, "assets", "fonts", "hand")
    ui = os.path.join(root, "assets", "fonts")
    os.makedirs(hand, exist_ok=True)
    report = []

    translations = lang_chars(root, ["ja", "zh-Hans", "zh-Hant"])
    common = LATIN | PUNCT | CJK_COMMON | translations

    jobs = [
        ("KleeOne-Regular.ttf", "KleeOne-Hand.ttf", "Scacelith Hand JA", "Klee One", common | jis0208_kanji(),
         "OFL-KleeOne.txt"),
        ("LXGWWenKai-Regular.ttf", "LXGWWenKai-Hand.ttf", "Scacelith Hand SC", "LXGW WenKai",
         common | gb2312_hanzi(), "OFL-LXGWWenKai.txt"),
        ("LXGWWenKaiTC-Regular.ttf", "LXGWWenKaiTC-Hand.ttf", "Scacelith Hand TC", "LXGW WenKai TC",
         common | big5_hanzi(not args.no_big5_level2), "OFL-LXGWWenKaiTC.txt"),
    ]
    if "cjk" not in groups:
        jobs = []
    for srcname, outname, family, origin, chars, ofl in jobs:
        font = TTFont(os.path.join(src, srcname))
        subset_font(font, chars)
        rename(font, family, origin)
        size = save(font, os.path.join(hand, outname))
        shutil.copyfile(os.path.join(src, ofl), os.path.join(hand, ofl))
        report.append((outname, len(font.getGlyphOrder()), size))

    arabic = rng(0x0600, 0x06FF) | rng(0xFB50, 0xFDFF) | rng(0xFE70, 0xFEFF) | {0x200C, 0x200D, 0x200E, 0x200F}
    if "arabic" in groups:
        prepare_arabic(src, ui, hand, arabic, report)
    if "korean" in groups:
        prepare_korean(root, src, ui, hand, report)
    if "devanagari" in groups:
        prepare_devanagari(root, src, ui, hand, report)
    if "mynerve" in groups:
        font = TTFont(os.path.join(src, "Mynerve-Regular.ttf"))
        greek = rng(0x0370, 0x03FF) | rng(0x1F00, 0x1FFF)
        subset_font(font, LATIN | PUNCT | rng(0x0100, 0x024F) | rng(0x0300, 0x036F) | rng(0x1E00, 0x1EFF) | greek)
        rename(font, "Scacelith Hand Greek", "Mynerve")
        size = save(font, os.path.join(hand, "Mynerve-Hand.ttf"))
        shutil.copyfile(os.path.join(src, "OFL-Mynerve.txt"), os.path.join(hand, "OFL-Mynerve.txt"))
        report.append(("Mynerve-Hand.ttf", len(font.getGlyphOrder()), size))

    total = 0
    for name, glyphs, size in report:
        total += size
        print("%-44s %6d glyphs %9.2f MB" % (name, glyphs, size / 1e6))
    print("%-44s %6s        %9.2f MB" % ("total", "", total / 1e6))
    return 0


def prepare_korean(root, src, ui, hand, report):
    hangul = ksx1001_hangul() | {c for c in lang_chars(root, ["ko"]) if 0xAC00 <= c <= 0xD7A3}
    chars = rng(0x20, 0x7E) | PUNCT | hangul | rng(0x3131, 0x318E) | rng(0x3000, 0x303F) | rng(0xFF01, 0xFF5E)
    for srcname, outname, folder, family, origin, ofl in (
            ("GowunBatang-Regular.ttf", "GowunBatangUI-Regular.ttf", ui, "Scacelith UI Korean", "Gowun Batang",
             "OFL-GowunBatang.txt"),
            ("NanumPenScript-Regular.ttf", "NanumPenScript-Hand.ttf", hand, "Scacelith Hand Korean",
             "Nanum Pen Script", "OFL-NanumPenScript.txt")):
        font = TTFont(os.path.join(src, srcname))
        subset_font(font, chars, keep_kern=False)
        rename(font, family, origin)
        size = save(font, os.path.join(folder, outname))
        shutil.copyfile(os.path.join(src, ofl), os.path.join(folder, ofl))
        report.append((outname, len(font.getGlyphOrder()), size))


def prepare_devanagari(root, src, ui, hand, report):
    fonts = (("TiroDevanagariHindi-Regular.ttf", "TiroDevanagariUI-Regular.ttf", ui, "Scacelith UI Devanagari",
              "Tiro Devanagari Hindi", "OFL-TiroDevanagariHindi.txt"),
             ("Kalam-Regular.ttf", "Kalam-Hand.ttf", hand, "Scacelith Hand Devanagari", "Kalam", "OFL-Kalam.txt"))
    table = devanagari_table(root, src, [f[0] for f in fonts])
    write_cluster_list(os.path.join(ui, "devanagari-clusters.txt"), table)
    deva = rng(0x0900, 0x097F) | {0x200C, 0x200D, 0x25CC} | set(table.values())
    for srcname, outname, folder, family, origin, ofl in fonts:
        path = os.path.join(src, srcname)
        font = TTFont(path)
        baked = bake_clusters(path, font, table)
        subset_font(font, rng(0x20, 0x7E) | PUNCT | deva, keep_kern=False)
        rename(font, family, origin, ", Devanagari syllables baked into Private Use codepoints")
        size = save(font, os.path.join(folder, outname))
        shutil.copyfile(os.path.join(src, ofl), os.path.join(folder, ofl))
        report.append(("%s (%d syllables, %d baked)" % (outname, len(table), baked), len(font.getGlyphOrder()), size))


def prepare_arabic(src, ui, hand, arabic, report):
    # Aref Ruqaa: bake the contextual forms, then subset without any layout table.
    ruqaa_src = os.path.join(src, "ArefRuqaa-Regular.ttf")
    font = TTFont(ruqaa_src)
    baked = bake_arabic_forms(ruqaa_src, font)
    subset_font(font, LATIN | PUNCT | arabic, keep_kern=False)
    rename(font, "Scacelith Hand Arabic", "Aref Ruqaa")
    size = save(font, os.path.join(hand, "ArefRuqaa-Hand.ttf"))
    shutil.copyfile(os.path.join(src, "OFL-ArefRuqaa.txt"), os.path.join(hand, "OFL-ArefRuqaa.txt"))
    report.append(("ArefRuqaa-Hand.ttf (%d baked forms)" % len(baked), len(font.getGlyphOrder()), size))

    # Amiri (UI text): its cmap already maps the presentation forms. No kerning: the game does not
    # kern right-to-left runs.
    font = TTFont(os.path.join(src, "Amiri-Regular.ttf"))
    subset_font(font, rng(0x20, 0x7E) | PUNCT | arabic, keep_kern=False)
    rename(font, "Scacelith UI Arabic", "Amiri")
    size = save(font, os.path.join(ui, "AmiriUI-Regular.ttf"))
    shutil.copyfile(os.path.join(src, "OFL-Amiri.txt"), os.path.join(ui, "OFL-Amiri.txt"))
    report.append(("AmiriUI-Regular.ttf", len(font.getGlyphOrder()), size))


if __name__ == "__main__":
    sys.exit(main())
