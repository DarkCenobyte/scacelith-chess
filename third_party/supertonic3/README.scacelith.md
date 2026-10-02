# Supertonic 3 in Scacelith (the coach's voice)

The coach speaks with [Supertonic 3](https://github.com/supertone-inc/supertonic), a text-to-speech
model by Supertone Inc., in the 8-bit (INT8) ONNX conversion published by the
[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) project. Scacelith runs it with its own ONNX
interpreter (`src/tts`, no ONNX Runtime).

**Scacelith does not redistribute the model.** The files are not in git, not in the executable and
not in the release package (which holds the executable only). The game downloads them itself, on
the player's request, the first time Coach mode or its voice option needs them, from their
publishers, into a per-user folder (`src/tts/model_store.h`, `src/game/coach_model.h`). The weights
are licensed under the **BigScience Open RAIL-M** licence, not under the GPL-3.0 of Scacelith, and
their use stays subject to that licence's use-based restrictions, which the download prompt points
to; see [Licence](#licence).

## Where the game puts them

The model folder is `coach/` inside the game's per-user application folder `scacelith/`:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\scacelith\coach\` (Roaming application data) |
| Linux | `$XDG_DATA_HOME/scacelith/coach/`, by default `~/.local/share/scacelith/coach/` |
| macOS (no build yet; the rule for a port) | `~/Library/Application Support/scacelith/coach/` |

`--coach-dir <folder>` replaces it (development builds: `--coach-dir build/coach`). Nothing is read
from or written beside the executable. Deleting the folder is harmless: the coach then speaks
through subtitles and offers the download again.

## Where the game fetches them

1. **Hugging Face**, file by file, no extraction:
   `https://huggingface.co/csukuangfj2/sherpa-onnx-supertonic-3-tts-int8-2026-05-11/resolve/main/<file>`
   (the resolve endpoint answers the small files itself and redirects the large ones to its CDN).
2. If anything fails there (network, an HTTP error, a file whose SHA-256 differs, a redirect to a
   non-HTTPS host), **the sherpa-onnx release archive on GitHub**:
   `https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2`
   (128,774,318 bytes, SHA-256 `82fa96f91c4ef8abaae3a14a3f4153facf88bed821d1f7331cec2700f432c427`),
   checked, extracted with the project's own bzip2 and tar readers (`src/core/bzip2.h`,
   `src/core/tar.h`; only the nine files below, only regular files inside the release folder), then
   deleted.

The progress panel says which host is in use. Every file streams in as `<name>.part`, is checked
against the manifest (size and SHA-256), and only then gets its name; an interrupted download
continues with HTTP `Range` the next time. The downloads send a `User-Agent: Scacelith/<version>`
header and no credentials, follow at most five redirects, HTTPS only.

## Provenance

* Model: Supertonic 3 (released 2026-04-29, 31 languages), Supertone Inc.,
  <https://huggingface.co/Supertone/supertonic-3> (archived as `supertone-oss-archive/supertonic-3`).
  The upstream repository is archived and v3 is its final version.
* Conversion: sherpa-onnx release tag `tts-models`, asset
  `sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2` (above). It was produced by sherpa-onnx's
  `scripts/supertonic/` (`convert.py`, `run.sh`, `generate_indexer_bin.py`,
  `generate_voices_bin.py`) from the official ONNX files. The Hugging Face repository
  `csukuangfj2/sherpa-onnx-supertonic-3-tts-int8-2026-05-11` holds the same nine files (same sizes
  and SHA-256).

The nine files (the manifest of `tts::supertonicManifest()`, 145,316,356 bytes; the runtime reads
the last six):

| File | Bytes | SHA-256 | Content |
|---|---:|---|---|
| `LICENSE` | 1,070 | `0dfe0d0b...ac096fa2` | the **MIT licence of Supertone's sample code**, copied by sherpa-onnx from Supertone's repository; not the model's licence |
| `README.md` | 19,518 | `a96c3479...29caf863` | Supertone's description of the model |
| `tts.json` | 8,253 | `42078d3a...b06a0d09` | the original configuration (its constants, 44.1 kHz, latent 24 x 6 channels, 3072 samples per latent frame, are compiled into `src/tts/model.h`) |
| `duration_predictor.int8.onnx` | 3,700,147 | `c3eb9141...39325db` | official fp32 graph, copied as is (no quantized operator; producer "pytorch 2.9.0") |
| `text_encoder.int8.onnx` | 36,416,150 | `c7befd5e...b9902ff` | official fp32 graph, copied as is (same) |
| `vector_estimator.int8.onnx` | 78,400,833 | `20cd86fa...c0db3fd` | **modified**: MatMuls dynamically quantized (`DynamicQuantizeLinear` + `MatMulInteger`, int8 weights per tensor), the weights of 80 of its 86 convolutions stored as per-output-channel int8 behind `DequantizeLinear` (the last six stay float) |
| `vocoder.int8.onnx` | 25,991,073 | `e923d60f...0da6152` | **modified**: static QDQ quantization of 25 of its 33 convolutions (uint8 activations, int8 per-channel weights, calibrated over the 31 languages); the last eight keep float activations with per-channel int8 weights |
| `unicode_indexer.bin` | 262,144 | `8402ca48...9883b30` | the official `unicode_indexer.json` as 65,536 little-endian int32 (code point to character id, -1 unknown) |
| `voice.bin` | 517,168 | `67d5209b...8a93ce8` | the ten official voice styles: int64 header `[10, 50, 256, 10, 8, 16]`, then the ten `style_ttl` [50 x 256] float32 tensors, then the ten `style_dp` [8 x 16]; order F1..F5, M1..M5 (checked by pitch: indices 0-4 at 170-195 Hz, 5-9 at 88-132 Hz) |

Full hashes are in `src/tts/model_store.cpp` and `supertonic3.cmake` (`SUPERTONIC3_FILES`).

## Modifications

The vector estimator and the vocoder are *Derivatives of the Model* in the sense of the licence
(section 1(e)): sherpa-onnx quantized them to 8-bit integers, as described above. **Scacelith does
not modify any model file**: the game stores them byte for byte (SHA-256 checked) and the runtime
maps them read-only. Beside them the game writes `README.txt` (from
`assets/tts/coach-folder-README.txt`: where the files came from, that Scacelith does not
redistribute them, their licences and use restrictions, the notice on the modified files) and
`Supertonic-3-OpenRAIL-M.txt`.

Everything Scacelith-specific:

| File | Purpose |
|---|---|
| `src/tts/model_store.{h,cpp}` | The manifest, the folder, the status check and the download job (hub, then archive) |
| `src/game/coach_model.{h,cpp}`, `src/ui/ui_model_download.cpp` | When the game offers the download, the prompt and the progress panel |
| `assets/tts/coach-folder-README.txt` | The `README.txt` written beside the files (`{source}` = where they came from) |
| `assets/licences/Supertonic-3-OpenRAIL-M.txt` | The model licence, verbatim; shown by the licence viewer and the prompt, written beside the files |
| `supertonic3.cmake` | Developers only: a checked copy in `${CMAKE_BINARY_DIR}/coach/` for the tests and `--coach-dir` |

## Licence

* The licence text is `assets/licences/Supertonic-3-OpenRAIL-M.txt`, verbatim from
  `hf://models/supertone-oss-archive/supertonic-3/LICENSE` (15,007 bytes, SHA-256
  `0d944a9110fed9a9602d60e0423a272903e7bd21ab060490774efc77c2275e9f`): "BigScience Open RAIL-M
  License, dated August 18, 2022", unchanged from BigScience's text (no Supertone-specific names or
  clauses). The game's licence viewer (Credits > Licences) lists it, and the download prompt opens
  it in place.
* **The download prompt** says what the model is and its size, that it is published under OpenRAIL-M
  which forbids certain uses of the voice it produces (presenting it as a human voice, imitating a
  real person: Attachment A (e) and (g)), offers the full text ("Read the licence"), and states that
  choosing Download accepts the licence's terms. "Not now" downloads nothing and switches the
  coach's voice off (subtitles only), until the player switches it back on in Options > Audio.
* Section 4 binds whoever distributes the model. Scacelith does not distribute it: the player's
  computer fetches it from its publishers (Hugging Face, or sherpa-onnx's GitHub release), which
  distribute it under the licence. Scacelith still keeps the notices with the files: the licence
  text (4(b)), `README.txt` with the restrictions (4(a)) and the notice on modified files (4(c)),
  and the attribution "Synthetic speech generated by Supertonic 3" on the title page and in the
  credits, without suggesting endorsement (section 8).
* Attachment A (e) forbids placing generated content anywhere without disclosing that it is
  machine generated, and (g) forbids impersonation. The coach is an on-screen robot whose voice is
  obviously synthetic; the title page and the credits name the model.
* Section 7 asks for reasonable efforts to use the latest version: v3 is the last release of an
  archived project; the manifest pins it by hash.
* Section 6: Supertone claims no rights on the generated audio.
* **GPL-3.0.** The use restrictions are "further restrictions" that GPL-3.0 section 10 does not
  allow on the GPL-covered program. The model is therefore never part of the program: not in the
  executable, not in the release package; the game fetches it at the player's request, and works
  without it (the coach then shows subtitles only). (This is the project's reading, not legal
  advice.)

## Build options (developers)

`third_party/supertonic3/supertonic3.cmake`, included by the top-level `CMakeLists.txt`, prepares a
checked copy of the nine files in `${CMAKE_BINARY_DIR}/coach/` for the unit tests (`tts_*`) and for
runs with `--coach-dir build/coach`. The game itself never reads that folder unless told to.

| Cache variable | Default | Meaning |
|---|---|---|
| `SCACELITH_SUPERTONIC_DIR` | `$ENV{SCACELITH_SUPERTONIC_DIR}` | Folder of the extracted release (used as is when set) |
| `SCACELITH_SUPERTONIC_ARCHIVE` | `$ENV{SCACELITH_SUPERTONIC_ARCHIVE}` | Local copy of the `.tar.bz2` (else `build/_deps/`) |
| `SCACELITH_SUPERTONIC_DOWNLOAD` | `ON` | Download the archive from GitHub when no local copy is given (checked against its SHA-256) |

Without the files the configure step warns, the build succeeds and the TTS tests that need the
model are skipped. There is no option to embed the model in the executable any more.
