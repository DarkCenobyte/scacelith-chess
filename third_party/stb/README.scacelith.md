# stb_truetype in Scacelith

The game's text is rasterised by [stb_truetype](https://github.com/nothings/stb) (Sean Barrett), a
single-file TrueType library in the public domain or under the MIT licence, at the user's choice
(the end of `stb_truetype.h`); the release packages carry no licence text for it, which the
public-domain alternative allows. `src/ui/ui_font.cpp` turns its glyph rasters into the distance
fields of the font atlas and of the markings baked by `renderLineSdf()`.

## Provenance

* `stb_truetype.h` (v1.26) is byte-identical to `stb_truetype.h` of
  <https://github.com/nothings/stb>, git blob `90a5c2e2b3fe563c98585294ca7a949876aec94c`
  (`git hash-object third_party/stb/stb_truetype.h`): the file as last changed upstream by commit
  `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (2024-07-15, the SDF fixes merged after the v1.26
  release of 2021), unchanged on `master` up to `2c980bb59875b0d32144a71867fbdebb2f77cd20`
  (2026-08-01). There is no newer version: the `dev` branch holds an older copy.

## Modifications

**The header is not modified.** `stb_truetype.cpp` (Scacelith's) compiles its implementation
(`STB_TRUETYPE_IMPLEMENTATION`) into the game executable (`CMakeLists.txt`), and
`src/ui/ui_font.cpp` includes only the declarations. The implementation has a unit of its own
because the CodeQL scan (`.github/workflows/codeql.yml`) leaves out the units under `third_party/`,
not the code that a scanned unit includes: compiled inside `ui_font.cpp`, stb_truetype was scanned
as the game's code.

## Fonts and security

stb_truetype does no range checking of the offsets in a font file ("NO SECURITY GUARANTEE -- DO NOT
USE THIS ON UNTRUSTED FONT FILES", at the top of the header). The game only parses the fonts
embedded in the executable at build time (`assets/fonts/`, through `embedded::find`), never a font
file from the player's disk or from the network: keep it that way.

The game rasterises with `stbtt_MakeGlyphBitmap` and `stbtt_MakeGlyphBitmapSubpixel`, into buffers
it sizes itself in `size_t`. The functions that allocate or clear a bitmap of an `int` product of
its sides (`stbtt_GetGlyphBitmap`, `stbtt_GetCodepointBitmap` and their subpixel variants,
`stbtt_GetGlyphSDF` and `stbtt_GetCodepointSDF`, `stbtt_BakeFontBitmap`, `stbtt_PackBegin`), which CodeQL reported as
"Multiplication result converted to larger type" while the header was scanned, are not used.
Upstream pull request [#1867](https://github.com/nothings/stb/pull/1867) (open, not merged, as of 2026-10) would
add overflow checks to `stbtt_GetGlyphBitmapSubpixel`, behind the bitmap functions, and to
`stbtt_GetGlyphSDF`.

## Updating

Replace `stb_truetype.h` with the new upstream file and update the blob and commits above. Keep
`STB_TRUETYPE_IMPLEMENTATION` in `stb_truetype.cpp` only, never in a file under `src/`, or the
implementation is scanned again.
