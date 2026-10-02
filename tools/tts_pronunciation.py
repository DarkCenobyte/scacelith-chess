#!/usr/bin/env python3
"""How Supertonic 3 reads chess notation, per speech language (input for the coach's speech catalog).

    tools/tts_pronunciation.py --model DIR [--voice N] [--seeds 4] [--wav OUTDIR] [--lang xx ...]
                               [--probe "lang:form=candidate|candidate|..." ...]

No speech recogniser is available offline, so the reading of each written form ("e4", "Nf3",
"O-O", ...) is identified acoustically: the form and several spelled-out candidates ("e four",
"four", ...) are synthesised with the same voice and several noise seeds, turned into
mean-normalised log-mel spectrograms, and compared with dynamic time warping (the template
matching of isolated-word recognisers, usable here because the speaker is the same). For each
form the script prints its 'self' distance (mean DTW distance between its own renditions, i.e. the
spread due to the noise seed) and the candidates from the closest to the farthest, each with

    excess = mean distance(form, candidate) - (self(form) + self(candidate)) / 2

which is about 0 when the candidate is read like the form and grows with the difference. A clear
winner (excess near 0, well below the next candidate) is how the model reads the form; when even
the best candidate stays well above 0, the form is read in a way none of the candidates describes
(often a mumble), and the catalog must spell it out. This is a heuristic: listen to the --wav
files before relying on a close call.
--wav writes every rendition (seed 0) for listening; --probe replaces the built-in probes with
the given ones (e.g. --probe "en:a8=ay eight|uh eight"). Needs numpy and onnxruntime.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tts_reference import SAMPLE_RATE, Model, write_wav  # noqa: E402

# Written form -> spelled-out candidates, per language. Candidates cover the plausible readings:
# letter names of the language, the digit in the language, parts dropped, other conventions.
PROBES = {
    "en": {
        "e4": ["ee four", "four", "eh four", "e for"],
        "Nf3": ["en ef three", "knight ef three", "ef three", "nuff three"],
        "O-O": ["oh oh", "oh dash oh", "castles", "zero zero"],
        "h7": ["aitch seven", "seven", "hey seven", "age seven"],
        "1-0": ["one zero", "one oh", "one dash zero", "one to nothing", "one minus zero", "ten"],
        "+1.5": ["plus one point five", "one point five", "plus one five", "plus fifteen", "plus one dot five",
                 "plus one and a half"],
    },
    "fr": {
        "e4": ["é quatre", "euh quatre", "i quatre", "quatre"],
        "Nf3": ["ène èf trois", "cavalier èf trois", "èf trois", "nèf trois"],
        "O-O": ["o o", "o tiret o", "petit roque", "zéro zéro"],
        "h7": ["hache sept", "sept", "ache sept", "é sept"],
        "1-0": ["un zéro", "un tiret zéro", "un moins zéro", "un à zéro", "dix"],
        "+1.5": ["plus un virgule cinq", "plus un point cinq", "un point cinq", "plus un cinq", "plus quinze"],
    },
    "de": {
        "e4": ["eh vier", "vier", "e für", "i vier"],
        "Nf3": ["enn eff drei", "Springer eff drei", "eff drei"],
        "O-O": ["oh oh", "null null", "kurze Rochade", "oh Strich oh"],
        "h7": ["ha sieben", "sieben", "hah sieben"],
        "1-0": ["eins null", "eins zu null", "eins minus null", "eins Strich null", "zehn"],
        "+1.5": ["plus eins Komma fünf", "plus eins Punkt fünf", "eins Punkt fünf", "plus eins fünf", "plus fünfzehn"],
    },
    "es": {
        "e4": ["e cuatro", "cuatro", "i cuatro"],
        "Nf3": ["ene efe tres", "caballo efe tres", "efe tres"],
        "O-O": ["o o", "cero cero", "enroque corto", "o guion o"],
        "h7": ["hache siete", "siete", "ache siete"],
        "1-0": ["uno cero", "uno a cero", "uno menos cero", "uno guion cero", "diez"],
        "+1.5": ["más uno coma cinco", "más uno punto cinco", "uno punto cinco", "más uno cinco", "más quince"],
    },
    "ru": {
        "e4": ["е четыре", "э четыре", "и четыре", "четыре"],
        "Nf3": ["эн эф три", "конь эф три", "эф три"],
        "O-O": ["о о", "ноль ноль", "короткая рокировка", "о тире о"],
        "h7": ["аш семь", "эйч семь", "ха семь", "семь"],
        "1-0": ["один ноль", "один тире ноль", "один минус ноль", "десять"],
        "+1.5": ["плюс один и пять", "плюс один точка пять", "плюс полтора", "один пять"],
    },
    "uk": {
        "e4": ["е чотири", "і чотири", "чотири"],
        "Nf3": ["ен еф три", "кінь еф три", "еф три"],
        "O-O": ["о о", "нуль нуль", "коротка рокіровка"],
        "h7": ["аш сім", "ха сім", "ейч сім", "сім"],
        "1-0": ["один нуль", "один тире нуль", "один мінус нуль", "десять"],
        "+1.5": ["плюс один кома п'ять", "плюс один крапка п'ять", "плюс півтора", "один п'ять"],
    },
    "ar": {
        "e4": ["إي أربعة", "إي فور", "أربعة"],
        "Nf3": ["إن إف ثلاثة", "الحصان إف ثلاثة", "إف ثلاثة", "إن إف ثري"],
        "O-O": ["أو أو", "صفر صفر", "تبييت قصير"],
        "h7": ["إتش سبعة", "سبعة", "إتش سفن"],
        "1-0": ["واحد صفر", "واحد ناقص صفر", "واحد إلى صفر", "عشرة"],
        "+1.5": ["زائد واحد فاصلة خمسة", "زائد واحد ونصف", "واحد فاصلة خمسة", "بلس وان بوينت فايف"],
    },
    "ja": {
        "e4": ["イーよん", "イーフォー", "よん", "えよん"],
        "Nf3": ["エヌエフさん", "ナイトエフさん", "エフさん", "エヌエフスリー"],
        "O-O": ["オーオー", "ゼロゼロ", "キャスリング"],
        "h7": ["エイチなな", "エッチなな", "なな", "エイチセブン"],
        "1-0": ["いちゼロ", "いちたいゼロ", "いちマイナスゼロ", "じゅう"],
        "+1.5": ["プラスいってんご", "いってんご", "プラスいちご", "プラスワンポイントファイブ"],
    },
}


def mel_filters(n_fft, n_mels, fmin, fmax):
    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def mel2hz(m):
        return 700.0 * (10.0 ** (m / 2595.0) - 1.0)
    pts = mel2hz(np.linspace(hz2mel(fmin), hz2mel(fmax), n_mels + 2))
    bins = np.fft.rfftfreq(n_fft, 1.0 / SAMPLE_RATE)
    fb = np.zeros((n_mels, bins.size))
    for i in range(n_mels):
        lo, mid, hi = pts[i], pts[i + 1], pts[i + 2]
        fb[i] = np.clip(np.minimum((bins - lo) / (mid - lo), (hi - bins) / (hi - mid)), 0.0, None)
    return fb


N_FFT, HOP = 1024, 441     # 23 ms windows, 10 ms hop
MEL = mel_filters(N_FFT, 40, 80.0, 8000.0)


def features(wav):
    """Mean-normalised log-mel spectrogram of the voiced part (frames within 40 dB of the loudest)."""
    x = wav.astype(np.float64)
    if x.size < N_FFT:
        x = np.pad(x, (0, N_FFT - x.size))
    frames = np.lib.stride_tricks.sliding_window_view(x, N_FFT)[::HOP] * np.hanning(N_FFT)
    power = np.abs(np.fft.rfft(frames, axis=1)) ** 2
    energy = power.sum(axis=1)
    keep = np.nonzero(energy > energy.max() * 1e-4)[0]
    if keep.size:
        power = power[keep[0]:keep[-1] + 1]
    f = np.log(power @ MEL.T + 1e-8)
    return f - f.mean(axis=0)


def dtw(a, b):
    """DTW distance (Euclidean frame cost, steps (1,0) (0,1) (1,1)) divided by n + m."""
    n, m = len(a), len(b)
    cost = np.sqrt(((a[:, None, :] - b[None, :, :]) ** 2).sum(axis=2))
    d = np.full((n + 1, m + 1), np.inf)
    d[0, 0] = 0.0
    for k in range(2, n + m + 1):   # anti-diagonals i + j = k
        i = np.arange(max(1, k - m), min(n, k - 1) + 1)
        j = k - i
        d[i, j] = cost[i - 1, j - 1] + np.minimum(np.minimum(d[i - 1, j - 1], d[i - 1, j]), d[i, j - 1])
    return d[n, m] / (n + m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--voice", type=int, default=7)  # M3, the game's default voice
    ap.add_argument("--steps", type=int, default=5)
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--wav", default="")
    ap.add_argument("--lang", nargs="*", default=list(PROBES))
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--probe", action="append", default=[])
    a = ap.parse_args()
    if a.seeds < 2:
        ap.error("--seeds must be at least 2 (the self distance compares pairs of renditions)")
    probes = PROBES
    if a.probe:
        probes = {}
        for spec in a.probe:
            head, cands = spec.split("=", 1)
            lang, form = head.split(":", 1)
            probes.setdefault(lang, {})[form] = cands.split("|")
        a.lang = list(probes)
    m = Model(a.model, a.threads)
    cache = {}

    def renditions(text, lang):
        key = (text, lang)
        if key not in cache:
            wavs = [m.run(text, lang, a.voice, a.steps, 1.0, 1000 + s)[0] for s in range(a.seeds)]
            cache[key] = (wavs, [features(w) for w in wavs])
        return cache[key]

    def spread(f):
        return float(np.mean([dtw(f[i], f[j]) for i in range(len(f)) for j in range(i + 1, len(f))]))

    def mean_cross(fa, fb):
        return float(np.mean([dtw(x, y) for x in fa for y in fb]))

    for lang in a.lang:
        print("== %s" % lang)
        for form, cands in probes[lang].items():
            wf, ff = renditions(form, lang)
            self_d = spread(ff)
            rows = []
            for c in cands:
                wc, fc = renditions(c, lang)
                rows.append((mean_cross(ff, fc) - 0.5 * (self_d + spread(fc)), c))
            rows.sort()
            print("  %-5s self %5.2f | %s" % (form, self_d, "  ".join("%s %+.2f" % (c, d) for d, c in rows)))
            if a.wav:
                os.makedirs(a.wav, exist_ok=True)
                idx = list(probes[lang]).index(form)
                write_wav(os.path.join(a.wav, "%s_%d_form.wav" % (lang, idx)), wf[0])
                for k, c in enumerate(cands):
                    write_wav(os.path.join(a.wav, "%s_%d_cand%d.wav" % (lang, idx, k)), renditions(c, lang)[0][0])
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
