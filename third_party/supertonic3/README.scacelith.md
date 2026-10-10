# Supertonic 3 in Scacelith (the coach's voice)

The coach speaks with [Supertonic 3](https://github.com/supertone-inc/supertonic), a text-to-speech
model by Supertone Inc., in its official ONNX release (fp32 graphs), unmodified. Scacelith runs it
with its own ONNX interpreter (`src/tts`, no ONNX Runtime).

**Scacelith does not redistribute the model.** The files are not in git, not in the executable and
not in the release package (which holds the executable only). The game downloads them itself, on
the player's request, the first time Coach mode or its voice option needs them, from their
publisher, into a per-user folder (`src/tts/model_store.h`, `src/game/coach_model.h`). The weights
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

The files lie flat in that folder: the repository's `onnx/` and `voice_styles/` prefixes are
dropped. `--coach-dir <folder>` replaces the folder (development builds: `--coach-dir build/coach`).
Nothing is read from or written beside the executable. Deleting the folder is harmless: the coach
then speaks through subtitles and offers the download again.

## Where the game fetches them

From Hugging Face, file by file, one after the other; no archive, no extraction:

1. **Supertone's repository**, at the revision Supertone's own Python SDK pins:
   `https://huggingface.co/Supertone/supertonic-3/resolve/724fb5abbf5502583fb520898d45929e62f02c0b/<path>`
   (the resolve endpoint answers the small files itself and redirects the large ones to its CDN).
2. If anything fails there (network, an HTTP error, a file whose SHA-256 differs, a redirect to a
   non-HTTPS host), **Supertone's archive copy**, at the revision Supertone's GitHub README pins,
   for that file and the ones after it:
   `https://huggingface.co/supertone-oss-archive/supertonic-3/resolve/aafc6e32416a594460b32413efc49d7fe4ce6d46/<path>`
   The two revisions hold the same files (same sizes and SHA-256). A file that cannot be written
   (disk full, no permission) ends the download instead: another source would not help.

`<path>` is the file's path in the repository (table below). The progress panel says which
repository is in use. Files already in the folder are hashed first and kept when they match.
Every missing file streams in as `<name>.part`, is checked against the manifest (size and
SHA-256), and only then gets its name; an interrupted download continues with HTTP `Range` the
next time. The downloads send a `User-Agent: Scacelith/<version>` header and no credentials,
follow at most five redirects, HTTPS only.

## Provenance

* Model: Supertonic 3 (released 2026-04-29, 31 languages), Supertone Inc. The upstream GitHub
  repository (`supertone-inc/supertonic`) is archived and v3 is its final version; its README
  points to the copies under `supertone-oss-archive`.
* `Supertone/supertonic-3` at revision `724fb5abbf5502583fb520898d45929e62f02c0b`: the revision
  Supertone's Python SDK downloads (PyPI `supertonic` 1.3.1, `supertonic/config.py`,
  `MODEL_CONFIGS["supertonic-3"]`).
* `supertone-oss-archive/supertonic-3` at revision `aafc6e32416a594460b32413efc49d7fe4ce6d46`: the
  revision of the Quick Start of Supertone's GitHub README (`--revision`).
* The repository also holds the nine other voice styles (F1 to F5, M1, M2, M4, M5), `LICENSE` (the
  OpenRAIL-M text), `README.md` and `config.json` (a stub for Hugging Face's download counts). The
  game downloads only what its runtime reads: M3 is the male "teacher" voice the coach speaks with
  (`tts::defaultVoiceName()`). Instead of the repository's `LICENSE` and `README.md`, it writes its
  own `README.txt` and the licence text beside the files (below).

The seven files (the manifest of `tts::supertonicManifest()`, 398,651,400 bytes, about 399 MB; the
runtime reads all of them):

| File | Repository path | Bytes | SHA-256 | Content |
|---|---|---:|---|---|
| `tts.json` | `onnx/tts.json` | 8,253 | `42078d3a...b06a0d09` | the configuration (its constants, 44.1 kHz, latent 24 x 6 channels, 3072 samples per latent frame, are compiled into `src/tts/model.h`, and checked against this file when it loads) |
| `unicode_indexer.json` | `onnx/unicode_indexer.json` | 277,676 | `9bf7346e...493cb24f` | the character indexer: 65,536 numbers, code point to character id (-1 unknown) |
| `M3.json` | `voice_styles/M3.json` | 290,198 | `ea1ac35c...954e159b` | the voice style M3: `style_ttl` [1, 50, 256] and `style_dp` [1, 8, 16], as JSON numbers |
| `duration_predictor.onnx` | `onnx/duration_predictor.onnx` | 3,700,147 | `c3eb9141...639325db` | the duration predictor, fp32 (producer "pytorch 2.9.0") |
| `text_encoder.onnx` | `onnx/text_encoder.onnx` | 36,416,150 | `c7befd5e...4b9902ff` | the text encoder, fp32 (producer "pytorch 2.9.0") |
| `vector_estimator.onnx` | `onnx/vector_estimator.onnx` | 256,534,781 | `883ac868...80d7c61c` | the flow-matching vector estimator, fp32 (producer "pytorch 2.5.1") |
| `vocoder.onnx` | `onnx/vocoder.onnx` | 101,424,195 | `085de76d...d2a4c4ba` | the vocoder, fp32 (producer "pytorch 2.9.0") |

Full hashes are in `src/tts/model_store.cpp` and `supertonic3.cmake` (`SUPERTONIC3_FILES`). The
in-house interpreter runs these fp32 graphs about as fast as it ran the old INT8 ones: a real-time
factor (synthesis time over speech duration) of about 0.29, against 0.25 to 0.30, on a 4-core
AVX-512 Xeon with 2 threads.

## Unmodified files

**Scacelith uses the files as Supertone publishes them**: no conversion, no quantization. The game
stores them byte for byte (SHA-256 checked); the runtime maps the graphs read-only and reads the
JSON files into memory. None of them is a *Derivative of the Model* in the sense of the licence
(section 1(e)); only their place differs (flat in the model folder, without the repository's
subfolders). Beside them the game writes `README.txt` (from `assets/tts/coach-folder-README.txt`:
where the files came from, that Scacelith does not redistribute them, their licence and use
restrictions) and `Supertonic-3-OpenRAIL-M.txt` (`tts::writeFolderNotices`).

## The old INT8 model

Earlier versions of the game downloaded sherpa-onnx's 8-bit conversion of the same model (release
`sherpa-onnx-supertonic-3-tts-int8-2026-05-11` of the [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)
project) into the same folder: nine files, 145,316,356 bytes. `duration_predictor.int8.onnx` and
`text_encoder.int8.onnx` were the official fp32 graphs under other names (the same SHA-256 as the
files above); `vector_estimator.int8.onnx` and `vocoder.int8.onnx` were quantized to 8-bit
integers by sherpa-onnx (Derivatives of the Model); `unicode_indexer.bin` and `voice.bin` were
binary conversions of the character indexer and of the ten voice styles; `tts.json` was the
original configuration; `LICENSE` was the MIT licence of Supertone's sample code and `README.md`
Supertone's description of the model. `tts::legacyManifest()` lists them with their sizes and
SHA-256, and no source: the game never downloads them any more.

* **A player who has them keeps the voice.** The runtime loads either layout (`src/tts/model.h`):
  the official files when they are all there, else the old ones.
* **The update is offered.** At start-up, when the old files are all there (sizes) and the official
  ones are not, a background thread hashes them; when they are genuine, the game offers the update
  once (`[coach] voice_update_offered`). From then on, Options > Audio shows an update row under
  Coach voice, which opens the same prompt. "Not now" keeps the old voice.
* **The update deletes the old files first.** Any download into a folder that holds old files (the
  update, or a download after the old files failed to load) first stops the scene's TTS worker,
  which maps them, then deletes every file of the old model (`tts.json` included, although the
  official model has the same one), their `.part` files, the release archive (or its `.part`) that
  an interrupted old download left behind and the notices written for them (`README.txt`,
  `Supertonic-3-OpenRAIL-M.txt`; `tts::removeLegacyModel`), and only then downloads the seven files
  above. The space the old files free counts for the new ones.

## Everything Scacelith-specific

| File | Purpose |
|---|---|
| `src/tts/model_store.{h,cpp}` | The manifests (official and old), the folder, the status check, the removal of the old model and the download job (Supertone's repository, then the archive copy) |
| `src/tts/model.{h,cpp}` | Loads the official layout, or the old INT8 one while it is installed |
| `src/game/coach_model.{h,cpp}`, `src/ui/ui_model_download.cpp` | When the game offers the download or the update, the prompt and the progress panel |
| `assets/tts/coach-folder-README.txt` | The `README.txt` written beside the files (`{source}` = where they came from) |
| `assets/licences/Supertonic-3-OpenRAIL-M.txt` | The model licence, verbatim; shown by the licence viewer and the prompt, written beside the files |
| `supertonic3.cmake` | Developers only: a checked copy in `${CMAKE_BINARY_DIR}/coach/` for the tests and `--coach-dir` |

## Licence

* The licence text is `assets/licences/Supertonic-3-OpenRAIL-M.txt`, verbatim from the
  repository's `LICENSE` (15,007 bytes, SHA-256
  `0d944a9110fed9a9602d60e0423a272903e7bd21ab060490774efc77c2275e9f`): "BigScience Open RAIL-M
  License, dated August 18, 2022", unchanged from BigScience's text (no Supertone-specific names or
  clauses). The game's licence viewer (Credits > Licences) lists it, and the download prompt opens
  it in place.
* **The download prompt** says what the model is and its size, that it is published under OpenRAIL-M
  which forbids certain uses of the voice it produces (presenting it as a human voice, imitating a
  real person: Attachment A (e) and (g)), offers the full text ("Read the licence"), and states that
  choosing Download accepts the licence's terms. "Not now" downloads nothing and switches the
  coach's voice off (subtitles only), until the player switches it back on in Options > Audio. The
  update form of the prompt (the old model installed) says that the official files replace the
  8-bit ones, which are deleted first, with both sizes; it has the same licence lines with Update
  in place of Download, and its "Not now" keeps the old voice.
* Section 4 binds whoever distributes the model. Scacelith does not distribute it: the player's
  computer fetches it from its publisher (Supertone's repositories on Hugging Face), which
  distributes it under the licence. Scacelith still keeps the notices with the files: the licence
  text (4(b)) and `README.txt` with the restrictions (4(a)); no file is modified, so 4(c) has
  nothing to mark. The attribution "Synthetic speech generated by Supertonic 3" is on the title
  page and in the credits, without suggesting endorsement (section 8).
* Attachment A (e) forbids placing generated content anywhere without disclosing that it is
  machine generated, and (g) forbids impersonation. The coach is an on-screen robot whose voice is
  obviously synthetic; the title page and the credits name the model.
* Section 7 asks for reasonable efforts to use the latest version: v3 is the last release of an
  archived project; the manifest pins it by revision and hash, and the update replaces the old
  conversion with the official files.
* Section 6: Supertone claims no rights on the generated audio.
* **GPL-3.0.** The use restrictions are "further restrictions" that GPL-3.0 section 10 does not
  allow on the GPL-covered program. The model is therefore never part of the program: not in the
  executable, not in the release package; the game fetches it at the player's request, and works
  without it (the coach then shows subtitles only). (This is the project's reading, not legal
  advice.)

## Build options (developers)

`third_party/supertonic3/supertonic3.cmake`, included by the top-level `CMakeLists.txt`, prepares a
checked copy of the seven files in `${CMAKE_BINARY_DIR}/coach/`, with the `README.txt` and the
licence text the game writes, for the unit tests (`tts_*`) and for runs with
`--coach-dir build/coach`. The game itself never reads that folder unless told to. Without a local
copy, the configure step downloads the files one by one into `${CMAKE_BINARY_DIR}/_deps/supertonic-3/`
(from the same two sources, in the same order; each checked against its SHA-256 and kept there for
the next configure), then copies them.

| Cache variable | Default | Meaning |
|---|---|---|
| `SCACELITH_SUPERTONIC_DIR` | `$ENV{SCACELITH_SUPERTONIC_DIR}` | Local copy of the repository (`onnx/`, `voice_styles/`) or a folder holding the seven files themselves; used instead of the download when set |
| `SCACELITH_SUPERTONIC_DOWNLOAD` | `ON` | Download the files from Hugging Face when no local copy is given |

Without the files the configure step warns, the build succeeds and the TTS tests that need the
model are skipped; a file of the local copy whose SHA-256 differs stops the configure step.
`SCACELITH_SUPERTONIC_ARCHIVE` (the sherpa-onnx archive of the old model) no longer exists. There
is no option to embed the model in the executable.
