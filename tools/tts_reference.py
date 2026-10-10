#!/usr/bin/env python3
"""Reference runs of the Supertonic 3 model with ONNX Runtime, for the C++ runtime in src/tts.

    tools/tts_reference.py --model DIR dump OUT.bin [--text T] [--lang L] [--voice NAME] [--steps S]
                           [--speed X] [--seed N]
    tools/tts_reference.py --model DIR wav OUT.wav --text T [--lang L] [--voice NAME] [--steps S] ...
    tools/tts_reference.py --model DIR ids OUT.bin            (front-end cases, no inference)

DIR holds one of the layouts the game loads (src/tts/model.h):
  - the official release (Supertone/supertonic-3): a copy of the repository (onnx/, voice_styles/)
    or the game's flat model folder (build/coach): duration_predictor.onnx, text_encoder.onnx,
    vector_estimator.onnx, vocoder.onnx, unicode_indexer.json and the voice styles (M3.json, ...);
  - the old INT8 conversion by sherpa-onnx: duration_predictor.int8.onnx, text_encoder.int8.onnx,
    vector_estimator.int8.onnx, vocoder.int8.onnx, unicode_indexer.bin, voice.bin (ten voices).
Voices are named (F1..F5, M1..M5; the game's is M3). Needs numpy and onnxruntime (1.29 was used for
the committed dumps).

The pipeline follows the official py/helper.py of Supertonic 3 (supertone-inc/supertonic v3.0.0):
NFKD front end, <lang>...</lang> wrapping, duration predictor, text encoder, the Euler loop of the
vector estimator (classifier-free guidance is inside the graph) and the vocoder. The noise is drawn
with numpy's seeded generator and stored in the dump, so the C++ tests feed the same tensor.

'dump' writes a small tensor container read by tests/tts_tests.cpp:
    "STTD" u32 version=1, u32 count, then per tensor: u16 name length, name bytes, u8 dtype
    (ONNX element type: 1 float32, 2 uint8, 3 int8, 6 int32, 7 int64, 9 bool as bytes), u8 rank,
    rank x i64 dims, raw little-endian data.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
import unicodedata

import numpy as np

LANGS = ["en", "ko", "ja", "ar", "bg", "cs", "da", "de", "el", "es", "et", "fi", "fr", "hi", "hr", "hu", "id",
         "it", "lt", "lv", "nl", "pl", "pt", "ro", "ru", "sk", "sl", "sv", "tr", "uk", "vi", "na"]
SAMPLE_RATE = 44100
CHUNK = 512 * 6            # samples per latent frame (base_chunk_size x chunk_compress_factor)
LATENT_CHANNELS = 24 * 6


def preprocess(text, lang):
    """Official Supertonic 3 text normalisation (py/helper.py, UnicodeProcessor._preprocess_text)."""
    text = unicodedata.normalize("NFKD", text)
    emoji = re.compile("[\U0001f600-\U0001f64f\U0001f300-\U0001f5ff\U0001f680-\U0001f6ff\U0001f700-\U0001f77f"
                       "\U0001f780-\U0001f7ff\U0001f800-\U0001f8ff\U0001f900-\U0001f9ff\U0001fa00-\U0001fa6f"
                       "\U0001fa70-\U0001faff☀-⛿✀-➿\U0001f1e6-\U0001f1ff]+", flags=re.UNICODE)
    text = emoji.sub("", text)
    for k, v in {"–": "-", "‑": "-", "—": "-", "_": " ", "“": '"', "”": '"',
                 "‘": "'", "’": "'", "´": "'", "`": "'", "[": " ", "]": " ", "|": " ", "/": " ",
                 "#": " ", "→": " ", "←": " "}.items():
        text = text.replace(k, v)
    text = re.sub(r"[♥☆♡©\\]", "", text)
    for k, v in {"@": " at ", "e.g.,": "for example, ", "i.e.,": "that is, "}.items():
        text = text.replace(k, v)
    for p in [",", r"\.", "!", r"\?", ";", ":", "'"]:
        text = re.sub(" " + p, p.replace("\\", ""), text)
    while '""' in text:
        text = text.replace('""', '"')
    while "''" in text:
        text = text.replace("''", "'")
    while "``" in text:
        text = text.replace("``", "`")
    text = re.sub(r"\s+", " ", text).strip()
    if not re.search(r"[.!?;:,'\"')\]}…。」』】〉》›»]$", text):
        text += "."
    if lang not in LANGS:
        raise ValueError("unknown language " + lang)
    return "<" + lang + ">" + text + "</" + lang + ">"


class Model:
    def __init__(self, model_dir, threads=1, optimise=True):
        import onnxruntime as ort
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = threads
        opts.inter_op_num_threads = 1
        if not optimise:
            opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL

        def load(path):
            return ort.InferenceSession(path, sess_options=opts, providers=["CPUExecutionProvider"])

        def first(*paths):
            return next((p for p in paths if os.path.isfile(p)), None)

        onnx = first(model_dir + "/duration_predictor.onnx", model_dir + "/onnx/duration_predictor.onnx")
        if onnx:
            # The official release, flat or as the repository lays it out.
            base = os.path.dirname(onnx) + "/"
            self.kind = "official"
            self.dp = load(base + "duration_predictor.onnx")
            self.te = load(base + "text_encoder.onnx")
            self.ve = load(base + "vector_estimator.onnx")
            self.voc = load(base + "vocoder.onnx")
            self.indexer = np.array(json.load(open(base + "unicode_indexer.json")), dtype=np.int32)
            styles = sorted(glob.glob(model_dir + "/voice_styles/*.json")) or sorted(
                p for p in glob.glob(model_dir + "/*.json")
                if os.path.basename(p) not in ("tts.json", "unicode_indexer.json", "config.json"))
            self.voices, ttl, dps = [], [], []
            for p in styles:
                v = json.load(open(p))
                ttl.append(np.array(v["style_ttl"]["data"], dtype=np.float32).reshape(v["style_ttl"]["dims"])[0])
                dps.append(np.array(v["style_dp"]["data"], dtype=np.float32).reshape(v["style_dp"]["dims"])[0])
                self.voices.append(os.path.splitext(os.path.basename(p))[0])
            self.ttl, self.dpstyle = np.stack(ttl), np.stack(dps)
        else:
            self.kind = "legacy"
            self.dp = load(model_dir + "/duration_predictor.int8.onnx")
            self.te = load(model_dir + "/text_encoder.int8.onnx")
            self.ve = load(model_dir + "/vector_estimator.int8.onnx")
            self.voc = load(model_dir + "/vocoder.int8.onnx")
            self.indexer = np.fromfile(model_dir + "/unicode_indexer.bin", dtype=np.int32)
            raw = open(model_dir + "/voice.bin", "rb").read()
            dims = np.frombuffer(raw[:48], dtype=np.int64)
            n, a, b, _, c, d = (int(x) for x in dims)
            floats = np.frombuffer(raw[48:], dtype=np.float32)
            self.ttl = floats[:n * a * b].reshape(n, a, b)
            self.dpstyle = floats[n * a * b:].reshape(n, c, d)
            self.voices = ["F1", "F2", "F3", "F4", "F5", "M1", "M2", "M3", "M4", "M5"][:n]
        assert self.indexer.shape == (65536,), "unicode indexer: 65,536 entries expected"

    def voice(self, name):
        """Index of a voice by name (F1..F5, M1..M5)."""
        if name not in self.voices:
            raise ValueError("voice %s not in the model (%s)" % (name, ", ".join(self.voices)))
        return self.voices.index(name)

    def ids(self, text, lang):
        s = preprocess(text, lang)
        # Characters the indexer does not know (-1, or outside the BMP) are dropped, as the C++
        # runtime does (the official helper would pass -1 on to Gather, which picks the last row).
        ids = [int(self.indexer[ord(ch)]) if ord(ch) < 65536 else -1 for ch in s]
        return s, np.array([[i for i in ids if i >= 0]], dtype=np.int64)

    def run(self, text, lang, voice, steps, speed, seed, out=None):
        """'voice' is a name (M3) or an index into self.voices."""
        name = voice if isinstance(voice, str) else self.voices[voice]
        voice = self.voice(name)
        s, text_ids = self.ids(text, lang)
        mask = np.ones((1, 1, text_ids.shape[1]), dtype=np.float32)
        style_ttl = self.ttl[voice:voice + 1].copy()
        style_dp = self.dpstyle[voice:voice + 1].copy()
        dur = self.dp.run(None, {"text_ids": text_ids, "style_dp": style_dp, "text_mask": mask})[0]
        emb = self.te.run(None, {"text_ids": text_ids, "style_ttl": style_ttl, "text_mask": mask})[0]
        seconds = dur / speed
        wav_len = int(seconds[0] * SAMPLE_RATE)
        latent_len = (wav_len + CHUNK - 1) // CHUNK
        rng = np.random.default_rng(seed)
        noise = rng.standard_normal((1, LATENT_CHANNELS, latent_len)).astype(np.float32)
        lmask = np.ones((1, 1, latent_len), dtype=np.float32)
        x = noise
        xs = []
        for step in range(steps):
            x = self.ve.run(None, {"noisy_latent": x, "text_emb": emb, "style_ttl": style_ttl, "text_mask": mask,
                                   "latent_mask": lmask, "current_step": np.array([step], dtype=np.float32),
                                   "total_step": np.array([steps], dtype=np.float32)})[0]
            xs.append(x)
        wav = self.voc.run(None, {"latent": x})[0]
        if out is not None:
            out.update({"text_ids": text_ids, "text_mask": mask, "style_ttl": style_ttl, "style_dp": style_dp,
                        "duration": dur, "text_emb": emb, "noise": noise, "latent_mask": lmask,
                        "wav": wav, "speed": np.array([speed], dtype=np.float32),
                        "voice": np.frombuffer(name.encode(), dtype=np.uint8).astype(np.int64),
                        "steps": np.array([steps], dtype=np.int64)})
            for i, xi in enumerate(xs):
                out["latent%d" % (i + 1)] = xi
        return wav[0, :wav_len], s


def write_container(path, tensors):
    with open(path, "wb") as f:
        f.write(b"STTD" + struct.pack("<II", 1, len(tensors)))
        for name, a in tensors.items():
            a = np.ascontiguousarray(a)
            code = {np.dtype(np.float32): 1, np.dtype(np.uint8): 2, np.dtype(np.int8): 3, np.dtype(np.int32): 6,
                    np.dtype(np.int64): 7, np.dtype(np.bool_): 9}[a.dtype]
            nb = name.encode()
            f.write(struct.pack("<H", len(nb)) + nb + struct.pack("<BB", code, a.ndim))
            f.write(struct.pack("<%dq" % a.ndim, *a.shape))
            f.write((a.astype(np.uint8) if a.dtype == np.bool_ else a.astype(a.dtype.newbyteorder("<"))).tobytes())


def write_wav(path, pcm):
    pcm16 = np.clip(np.round(pcm * 32767.0), -32768, 32767).astype("<i2")
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + pcm16.nbytes) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", pcm16.nbytes) + pcm16.tobytes())


# Front-end cases: one per speech language plus normalisation corner cases (NFKD, quotes, dashes,
# symbols, auto-period). Each entry: (language, text).
FRONT_END_CASES = [
    ("en", "Knight to f3 — a “classical” move!"),
    ("en", "Castles kingside_now   ,  then  #4 [e.g., later]"),
    ("fr", "Le cavalier va en f3 ; échec à la dame. Énorme !"),
    ("de", "Der Springer schlägt auf e5 – großartig"),
    ("es", "¡El alfil captura en c6! ¿Por qué no?"),
    ("ru", "Конь на f3, ёж и йод"),
    ("uk", "Кінь б’є на еп'ять її"),
    ("ar", "الحصان إلى إف ثلاثة؟"),
    ("ar", "ﻻﺎ"),
    ("ja", "ナイトがｆ３へ。ガギﾊﾞです"),
    ("ja", "良い手ですね！"),
    ("en", "Vous êtes là → ½ point © 2026 \U0001f600 ok"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("mode", choices=["dump", "wav", "ids"])
    ap.add_argument("out")
    ap.add_argument("--text", default="Good move, well played.")
    ap.add_argument("--lang", default="en")
    ap.add_argument("--voice", default="M3")
    ap.add_argument("--steps", type=int, default=5)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--threads", type=int, default=1)
    a = ap.parse_args()
    m = Model(a.model, a.threads)
    if a.mode == "ids":
        t = {}
        for i, (lang, text) in enumerate(FRONT_END_CASES):
            s, ids = m.ids(text, lang)
            t["case%d.text" % i] = np.frombuffer(text.encode("utf-8"), dtype=np.uint8).astype(np.int64)
            t["case%d.lang" % i] = np.frombuffer(lang.encode(), dtype=np.uint8).astype(np.int64)
            t["case%d.ids" % i] = ids[0]
            print(lang, repr(s))
        write_container(a.out, t)
        return 0
    t = {}
    wav, s = m.run(a.text, a.lang, a.voice, a.steps, a.speed, a.seed, t)
    print("text", repr(s), "ids", t["text_ids"].shape[1], "duration", float(t["duration"][0]),
          "latent", t["noise"].shape[2], "samples", wav.size)
    if a.mode == "dump":
        write_container(a.out, t)
    else:
        write_wav(a.out, wav)
    return 0


if __name__ == "__main__":
    sys.exit(main())
