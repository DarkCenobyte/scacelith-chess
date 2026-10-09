# QR Code generator in Scacelith

The QR code of the two-factor setup page (the `otpauth://` link, `src/ui/ui_qr.cpp`) is made with
Project Nayuki's [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library),
C++ version, under the **MIT License** (`LICENSE`, which is shipped with the game).

## Provenance

* `qrcodegen.cpp`, `qrcodegen.hpp` and `Readme.markdown` are byte-identical to `cpp/qrcodegen.cpp`,
  `cpp/qrcodegen.hpp` and `Readme.markdown` of <https://github.com/nayuki/QR-Code-generator>,
  branch `master`, commit `3c6d0b3cefb4e049dc337e82237c9644399716a8` (2026-08-31).
* The latest tag, `v1.8.0`, is older: since then the C++ code only changed in commit `8329a71`
  (2024-09-01, a simpler formula for the spacing of the alignment patterns, same results).
* `LICENSE` is the licence text of the sources' header, with a note on its use here.

## Modifications

**None.** `qrcodegen.cpp` is compiled as a unit of its own into the game executable (`GAME_EXTRA`
in `CMakeLists.txt`); `src/ui/ui_qr.cpp` includes `qrcodegen.hpp` only.

## Code scanning

CodeQL leaves `third_party/` out (`paths-ignore` in `.github/workflows/codeql.yml`), but that only
skips the files compiled on their own: a vendored `.cpp` that a scanned file `#include`s is
analysed with it. Upstream's code raises six alerts there, none of them a bug (checked at the
commit above, which no upstream branch or pull request changes on these lines):

* `cpp/integer-multiplication-cast-to-long`, four times `finderPenalty...(...) * PENALTY_N3` and
  once `k * PENALTY_N4` in `getPenaltyScore()`: `int` products added to the `long` score. The
  pattern counts are 0 to 2 and `k` is 0 to 9 (asserted), so the products are at most 80 and 90,
  and the whole score at most 2,568,888 (asserted), which even a 32-bit `long` (Windows) holds.
* `cpp/incorrect-not-operator-usage`, `invert & !isFunction.at(y).at(x)` in `applyMask()`: both
  operands are `bool` and the `!` is meant (function modules are never masked); `&` and `&&` give
  the same result on them.

## Updating

Replace the three upstream files with those of the new commit and update the commit above. Keep
`qrcodegen.cpp` out of `#include`s, or its alerts come back.
