# Supertonic 3 in Scacelith (the coach's voice)

The coach speaks with [Supertonic 3](https://github.com/supertone-inc/supertonic), a text-to-speech
model by Supertone Inc., in the 8-bit (INT8) ONNX conversion published by the
[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) project. Scacelith runs it with its own ONNX
interpreter (`src/tts`, no ONNX Runtime). The **model files are not in git and not part of the
Scacelith program**: the build downloads them (or takes a local copy), checks them and copies them to
a `coach/` folder beside the executable, and the game reads them from there at run time.

The weights are licensed under the **BigScience Open RAIL-M** licence, not under the GPL-3.0 of
Scacelith; see [Licence](#licence).

## Provenance

* Model: Supertonic 3 (released 2026-04-29, 31 languages), Supertone Inc.,
  <https://huggingface.co/Supertone/supertonic-3> (archived as `supertone-oss-archive/supertonic-3`).
  The upstream repository is archived and v3 is its final version.
* Conversion: sherpa-onnx release tag `tts-models`, asset
  `sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2`,
  <https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2>,
  128,774,318 bytes, SHA-256 `82fa96f91c4ef8abaae3a14a3f4153facf88bed821d1f7331cec2700f432c427`
  (checked by the build).
  It was produced by sherpa-onnx's `scripts/supertonic/` (`convert.py`, `run.sh`,
  `generate_indexer_bin.py`, `generate_voices_bin.py`) from the official ONNX files.

Files of the archive that the runtime uses (copied unchanged to `coach/`; the build verifies each
SHA-256, `supertonic3.cmake`):

| File | Bytes | SHA-256 | Content |
|---|---:|---|---|
| `duration_predictor.int8.onnx` | 3,700,147 | `c3eb9141...39325db` | official fp32 graph, copied as is (no quantized operator; producer "pytorch 2.9.0") |
| `text_encoder.int8.onnx` | 36,416,150 | `c7befd5e...b9902ff` | official fp32 graph, copied as is (same) |
| `vector_estimator.int8.onnx` | 78,400,833 | `20cd86fa...c0db3fd` | **modified**: MatMuls dynamically quantized (`DynamicQuantizeLinear` + `MatMulInteger`, int8 weights per tensor), the weights of 80 of its 86 convolutions stored as per-output-channel int8 behind `DequantizeLinear` (the last six stay float) |
| `vocoder.int8.onnx` | 25,991,073 | `e923d60f...0da6152` | **modified**: static QDQ quantization of 25 of its 33 convolutions (uint8 activations, int8 per-channel weights, calibrated over the 31 languages); the last eight keep float activations with per-channel int8 weights |
| `unicode_indexer.bin` | 262,144 | `8402ca48...9883b30` | the official `unicode_indexer.json` as 65,536 little-endian int32 (code point to character id, -1 unknown) |
| `voice.bin` | 517,168 | `67d5209b...8a93ce8` | the ten official voice styles: int64 header `[10, 50, 256, 10, 8, 16]`, then the ten `style_ttl` [50 x 256] float32 tensors, then the ten `style_dp` [8 x 16]; order F1..F5, M1..M5 (checked by pitch: indices 0-4 at 170-195 Hz, 5-9 at 88-132 Hz) |

Full hashes are in `supertonic3.cmake` (`SUPERTONIC3_FILES`). Not used: `tts.json` (its constants,
44.1 kHz, latent 24 x 6 channels, 3072 samples per latent frame, are compiled into
`src/tts/model.h`), `README.md` (Supertone's README) and `LICENSE`, which is the **MIT licence of
Supertone's sample code**, not the model's licence (sherpa-onnx copies the repository's `LICENSE`).

## Modifications

The vector estimator and the vocoder are *Derivatives of the Model* in the sense of the licence
(section 1(e)): sherpa-onnx quantized them to 8-bit integers, as described above. **Scacelith does
not modify any model file**: the build copies them byte for byte (SHA-256 checked) and the runtime
maps them read-only. The notice required by section 4(c) for modified files is `coach/README.txt`
(from `coach-folder-README.txt` in this folder), shipped in the same folder as the files.

Everything Scacelith-specific lives next to the files:

| File | Purpose |
|---|---|
| `supertonic3.cmake` | Download (or local copy), SHA-256 checks, extraction, copy to `${CMAKE_BINARY_DIR}/coach/`, optional embedding |
| `coach-folder-README.txt` | Shipped as `coach/README.txt`: what the folder is, its licence, the modification notice, the use restrictions |
| `../../assets/licences/Supertonic-3-OpenRAIL-M.txt` | The model licence, verbatim; also copied to `coach/` |

## Licence

* The licence text is `assets/licences/Supertonic-3-OpenRAIL-M.txt`, verbatim from
  `hf://models/supertone-oss-archive/supertonic-3/LICENSE` (15,007 bytes, SHA-256
  `0d944a9110fed9a9602d60e0423a272903e7bd21ab060490774efc77c2275e9f`): "BigScience Open RAIL-M
  License, dated August 18, 2022", unchanged from BigScience's text (no Supertone-specific names or
  clauses). The game's licences viewer lists it (`assets/licences/*.txt`) and the build copies it
  into `coach/`.
* Obligations when distributing the model files (section 4) and how Scacelith meets them:
  * (a) the use-based restrictions of Attachment A must be an enforceable provision of the terms
    under which the files are distributed, with notice to users: the licence travels with the
    files, `coach/README.txt` states that the restrictions apply, and the release notes / installer
    licence page should say it too;
  * (b) a copy of the licence: `coach/Supertonic-3-OpenRAIL-M.txt`;
  * (c) notices on modified files: `coach/README.txt` (see [Modifications](#modifications));
  * (d) retain attribution: credit "Voice synthesis: Supertonic 3 by Supertone Inc. (OpenRAIL-M)",
    without suggesting endorsement (section 8).
* Attachment A (e) forbids placing generated content anywhere without disclosing that it is
  machine generated, and (g) forbids impersonation. The coach is an on-screen robot whose voice is
  obviously synthetic; the credits name the model.
* Section 7 asks for reasonable efforts to use the latest version: v3 is the last release of an
  archived project; the build pins it by hash.
* Section 6: Supertone claims no rights on the generated audio.
* **GPL-3.0.** The use restrictions are "further restrictions" that GPL-3.0 section 10 does not
  allow on the GPL-covered program. The model is therefore kept out of the executable and
  distributed beside it as a separate, separately licensed work (an aggregate, GPL-3.0 section 5):
  `Scacelith.exe` never contains it by default, and deleting `coach/` leaves a working game whose
  coach shows subtitles only. `-DSCACELITH_TTS_EMBED=ON` embeds the files in the executables for
  special builds; an executable built that way contains OpenRAIL-M material and must not be
  distributed as a plain GPL-3.0 program. (This is the project's reading, not legal advice.)

## Build options

`third_party/supertonic3/supertonic3.cmake`, included by the top-level `CMakeLists.txt`:

| Cache variable | Default | Meaning |
|---|---|---|
| `SCACELITH_SUPERTONIC_DIR` | `$ENV{SCACELITH_SUPERTONIC_DIR}` | Folder of the extracted release (used as is when set) |
| `SCACELITH_SUPERTONIC_ARCHIVE` | `$ENV{SCACELITH_SUPERTONIC_ARCHIVE}` | Local copy of the `.tar.bz2` (else `build/_deps/`) |
| `SCACELITH_SUPERTONIC_DOWNLOAD` | `ON` | Download the archive from GitHub when no local copy is given (`EXPECTED_HASH SHA256`) |
| `SCACELITH_TTS_EMBED` | `OFF` | Also embed the six files in the executables (the `coach/` folder still takes precedence) |

Without the files the configure step warns and the build still succeeds (speech unavailable).
Packaging: ship `build*/coach/` (six model files, `README.txt`, the licence) as a `coach` folder
beside `Scacelith.exe`; about 145 MB (129 MB compressed).
