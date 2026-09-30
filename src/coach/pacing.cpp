#include "pacing.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace coach {
namespace {

// Punctuation that may carry a pause. ASCII marks count only before a space (not "1.5", "3:05").
bool isPausePunct(char32_t c, bool* needsSpace) {
    *needsSpace = false;
    switch (c) {
    case ',': case ';': case ':': case '.': case '!': case '?':
        *needsSpace = true;
        return true;
    case 0x2014: case 0x2013: case 0x2026:              // — – …
        *needsSpace = true;
        return true;
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF1B: case 0xFF1A: case 0xFF01: case 0xFF1F:   // 、。，；：！？
    case 0x060C: case 0x061B: case 0x061F:              // Arabic comma, semicolon, question mark
        return true;
    default:
        return false;
    }
}

bool isWeightless(char32_t c) {
    if (c < 0x80) return c != ' ' && !std::isalnum(int(c));
    if (uni::isMark(c) || uni::isCjkPunct(c)) return true;
    if (c >= 0x2000 && c <= 0x206F) return c != 0x2002 && c != 0x2003;   // dashes, quotes, marks
    if (c == 0x00A0) return false;                                          // no-break space
    if (c >= 0x00A1 && c <= 0x00BF) return true;                            // ¡ « » ¿ ...
    if (c == 0x060C || c == 0x061B || c == 0x061F || c == 0x066A || c == 0x06D4) return true;
    return false;
}

struct Boundary {
    size_t punct = 0;      // byte offset of the punctuation
    size_t next = 0;       // byte offset where the next stretch of speech starts
    float weight = 0.0f;   // text weight before the punctuation
    bool sentence = false; // . ! ? (almost always a pause)
};

std::vector<Boundary> boundaries(const std::string& text) {
    std::vector<Boundary> out;
    std::u32string t = uni::decode(text);
    // Byte offset of each codepoint.
    std::vector<size_t> at(t.size() + 1, 0);
    {
        size_t b = 0;
        for (size_t i = 0; i < t.size(); ++i) {
            at[i] = b;
            std::string tmp;
            uni::append(tmp, t[i]);
            b += tmp.size();
        }
        at[t.size()] = b;
    }
    float w = 0.0f;
    for (size_t i = 0; i < t.size(); ++i) {
        bool needsSpace = false;
        if (isPausePunct(t[i], &needsSpace)) {
            size_t j = i;
            bool sentence = false;
            // A run of punctuation ("?!", ".»", "—") is one boundary.
            while (j < t.size() && (isWeightless(t[j]) && t[j] != ' ')) {
                if (t[j] == '.' || t[j] == '!' || t[j] == '?' || t[j] == 0x3002 || t[j] == 0xFF01 || t[j] == 0xFF1F ||
                    t[j] == 0x061F)
                    sentence = true;
                ++j;
            }
            bool spaced = j >= t.size() || t[j] == ' ' || t[j] == '\n';
            size_t k = j;
            while (k < t.size() && (t[k] == ' ' || t[k] == '\n' || isWeightless(t[k]))) ++k;
            if ((!needsSpace || spaced) && k < t.size()) {
                Boundary b;
                b.punct = at[i];
                b.next = at[k];
                b.weight = w;
                b.sentence = sentence;
                out.push_back(b);
            }
            for (size_t m = i; m < k; ++m) w += isWeightless(t[m]) ? 0.0f : 1.0f;
            i = k - 1;
            continue;
        }
        w += isWeightless(t[i]) ? 0.0f : 1.0f;
    }
    return out;
}

// Time at 'speech' seconds of speech after 'from', stepping over the pauses in [from, to).
float advance(float from, float to, float speech, const std::vector<SpeechTiming::Pause>& inner) {
    float t = from;
    for (const auto& p : inner) {
        if (p.end <= t || p.start >= to) continue;
        if (p.start - t >= speech) break;
        speech -= std::max(0.0f, p.start - t);
        t = p.end;
    }
    return std::min(to, t + speech);
}

}  // namespace

float textWeight(const std::string& text, size_t begin, size_t end) {
    end = std::min(end, text.size());
    if (begin >= end) return 0.0f;
    float w = 0.0f;
    for (char32_t c : uni::decode(text.substr(begin, end - begin))) w += isWeightless(c) ? 0.0f : 1.0f;
    return w;
}

std::vector<size_t> pauseBoundaries(const std::string& text) {
    std::vector<size_t> out;
    for (const Boundary& b : boundaries(text)) out.push_back(b.punct);
    return out;
}

SpeechTiming estimateTiming(const float* pcm, size_t samples, int sampleRate, const std::string& text,
                            const std::vector<int>& anchorOffsets, const PacingOptions& o) {
    SpeechTiming t;
    t.anchors.assign(anchorOffsets.size(), 0.0f);
    if (!pcm || sampleRate <= 0 || samples == 0) return t;
    t.duration = float(samples) / float(sampleRate);

    // 1. Voiced windows.
    size_t win = std::max<size_t>(1, size_t(std::lround(o.window * float(sampleRate))));
    size_t frames = (samples + win - 1) / win;
    float threshold = std::pow(10.0f, o.thresholdDb / 20.0f);
    float thr2 = threshold * threshold;
    std::vector<uint8_t> voiced(frames, 0);
    for (size_t f = 0; f < frames; ++f) {
        size_t a = f * win, b = std::min(samples, a + win);
        double e = 0.0;
        for (size_t i = a; i < b; ++i) e += double(pcm[i]) * double(pcm[i]);
        voiced[f] = e / double(b - a) > double(thr2);
    }
    size_t first = frames, last = 0;
    for (size_t f = 0; f < frames; ++f)
        if (voiced[f]) { if (first == frames) first = f; last = f; }
    if (first == frames) return t;   // silent
    float fs = float(win) / float(sampleRate);
    t.voiceStart = float(first) * fs;
    t.voiceEnd = std::min(t.duration, float(last + 1) * fs);

    // 2. Internal pauses.
    for (size_t f = first; f <= last;) {
        if (voiced[f]) { ++f; continue; }
        size_t g = f;
        while (g <= last && !voiced[g]) ++g;
        if (float(g - f) * fs >= o.minPause - 1e-4f) {
            SpeechTiming::Pause p;
            p.start = float(f) * fs;
            p.end = float(g) * fs;
            t.pauses.push_back(p);
        }
        f = g;
    }
    float pauseTotal = 0.0f;
    for (auto& p : t.pauses) pauseTotal += p.end - p.start;
    float speechTotal = std::max(1e-3f, t.voiceEnd - t.voiceStart - pauseTotal);

    // Map pauses onto punctuation: monotonic alignment of the boundaries' share of the text with
    // the pauses' share of the speech time.
    std::vector<Boundary> bs = boundaries(text);
    float totalWeight = std::max(1.0f, textWeight(text, 0, text.size()));
    size_t n = bs.size(), m = t.pauses.size();
    std::vector<float> pauseShare(m);
    {
        float before = 0.0f;
        for (size_t j = 0; j < m; ++j) {
            pauseShare[j] = (t.pauses[j].start - t.voiceStart - before) / speechTotal;
            before += t.pauses[j].end - t.pauses[j].start;
        }
    }
    // Costs: a comma read without a pause is common, a sentence end without one is not, a pause
    // away from punctuation (a hesitation) is rare; a match costs its distance squared, so that a
    // pause two words away from a comma is not taken for it.
    const float kInf = 1e9f, kSkipComma = 0.10f, kSkipSentence = 0.25f, kSkipPause = 0.20f, kMaxGap = 0.30f;
    std::vector<float> dp((n + 1) * (m + 1), kInf);
    std::vector<uint8_t> from((n + 1) * (m + 1), 0);   // 1 skip boundary, 2 skip pause, 3 match
    auto at = [&](size_t i, size_t j) -> float& { return dp[i * (m + 1) + j]; };
    at(0, 0) = 0.0f;
    for (size_t i = 0; i <= n; ++i) {
        for (size_t j = 0; j <= m; ++j) {
            float cur = at(i, j);
            if (cur >= kInf) continue;
            if (i < n) {   // a boundary said without a pause (cheap for commas)
                float c = cur + (bs[i].sentence ? kSkipSentence : kSkipComma);
                if (c < at(i + 1, j)) { at(i + 1, j) = c; from[(i + 1) * (m + 1) + j] = 1; }
            }
            if (j < m) {   // a pause that is no punctuation (a hesitation)
                float c = cur + kSkipPause;
                if (c < at(i, j + 1)) { at(i, j + 1) = c; from[i * (m + 1) + j + 1] = 2; }
            }
            if (i < n && j < m) {
                float gap = std::fabs(bs[i].weight / totalWeight - pauseShare[j]);
                if (gap <= kMaxGap) {
                    float c = cur + 0.1f * (gap / 0.08f) * (gap / 0.08f);
                    if (c < at(i + 1, j + 1)) { at(i + 1, j + 1) = c; from[(i + 1) * (m + 1) + j + 1] = 3; }
                }
            }
        }
    }
    std::vector<int> matchOf(n, -1);
    for (size_t i = n, j = m; i > 0 || j > 0;) {
        uint8_t how = from[i * (m + 1) + j];
        if (how == 3) { matchOf[i - 1] = int(j - 1); t.pauses[j - 1].boundary = int(i - 1); --i; --j; }
        else if (how == 2) --j;
        else --i;
    }

    // 3. Stretches of speech between mapped pauses: text [b0, b1), time [s0, s1).
    struct Stretch { size_t b0, b1; float s0, s1; };
    std::vector<Stretch> stretches;
    Stretch cur{0, text.size(), t.voiceStart, t.voiceEnd};
    for (size_t i = 0; i < n; ++i) {
        if (matchOf[i] < 0) continue;
        const auto& p = t.pauses[size_t(matchOf[i])];
        cur.b1 = bs[i].punct;
        cur.s1 = p.start;
        stretches.push_back(cur);
        cur = Stretch{bs[i].next, text.size(), p.end, t.voiceEnd};
    }
    stretches.push_back(cur);

    for (size_t a = 0; a < anchorOffsets.size(); ++a) {
        size_t off = size_t(std::max(0, anchorOffsets[a]));
        const Stretch* s = &stretches.back();
        for (const Stretch& st : stretches)
            if (off < st.b1 || &st == &stretches.back()) { s = &st; break; }
        size_t b0 = s->b0;
        float whole = textWeight(text, b0, s->b1);
        float part = off <= b0 ? 0.0f : textWeight(text, b0, std::min(off, s->b1));
        float frac = whole > 0.0f ? part / whole : 0.0f;
        // Unmapped pauses inside the stretch take time but say nothing.
        float inner = 0.0f;
        for (const auto& p : t.pauses)
            if (p.boundary < 0 && p.start >= s->s0 && p.end <= s->s1) inner += p.end - p.start;
        float speech = std::max(0.0f, s->s1 - s->s0 - inner);
        float ta = advance(s->s0, s->s1, frac * speech, t.pauses);
        t.anchors[a] = std::max(0.0f, ta - o.earlyBias);
    }
    return t;
}

SpeechTiming estimateTiming(const std::vector<float>& pcm, int sampleRate, const Catalog::Rendered& spoken,
                            const PacingOptions& o) {
    std::vector<int> offsets;
    for (auto& a : spoken.anchors) offsets.push_back(a.offset);
    return estimateTiming(pcm.data(), pcm.size(), sampleRate, spoken.text, offsets, o);
}

float anchorTime(const SpeechTiming& t, const Catalog::Rendered& spoken, const std::string& name,
                 float fallbackFraction, const PacingOptions& o) {
    for (size_t i = 0; i < spoken.anchors.size() && i < t.anchors.size(); ++i)
        if (spoken.anchors[i].name == name) return t.anchors[i];
    float f = std::min(1.0f, std::max(0.0f, fallbackFraction));
    return std::max(0.0f, t.voiceStart + f * (t.voiceEnd - t.voiceStart) - o.earlyBias);
}

}  // namespace coach
