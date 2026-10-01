// Coach pacing (src/coach/pacing.*): anchor times estimated from synthetic speech whose word
// timings are known: voiced tones per character with random durations, short gaps between words,
// pauses at some punctuation marks and not at others, hesitations away from punctuation.
#include "test.h"
#include "coach/catalog.h"
#include "coach/pacing.h"
#include "core/embedded.h"
#include "i18n/unicode.h"
#include <algorithm>
#include <cmath>
#include <random>

using coach::Catalog;

namespace {

constexpr int kRate = 44100;

struct Speech {
    std::vector<float> pcm;
    std::vector<float> at;   // start time of the character at each byte offset (-1 inside a character)
};

// Speaks 'text' with a tone per letter or digit (duration base x 0.75..1.25), silence for spaces
// (the gap between words), a 'pause' after the punctuation marks listed in 'pauseAt' (byte
// offsets), and extra silence before the byte offsets of 'hesitateAt'.
Speech speak(const std::string& text, std::mt19937& rng, float base, const std::vector<size_t>& pauseAt,
             const std::vector<size_t>& hesitateAt = {}, float pause = 0.30f, float hesitation = 0.16f) {
    Speech s;
    s.at.assign(text.size() + 1, -1.0f);
    std::uniform_real_distribution<float> jitter(0.75f, 1.25f);
    std::uniform_real_distribution<float> noise(-1e-4f, 1e-4f);
    auto silence = [&](float sec) {
        size_t n = size_t(sec * kRate);
        for (size_t i = 0; i < n; ++i) s.pcm.push_back(noise(rng));
    };
    auto tone = [&](float sec) {
        size_t n = size_t(sec * kRate);
        float f0 = 110.0f + 40.0f * jitter(rng);
        for (size_t i = 0; i < n; ++i) {
            float tt = float(i) / kRate;
            float env = std::min(1.0f, std::min(float(i), float(n - i)) / (0.004f * kRate));
            s.pcm.push_back(0.12f * env * (std::sin(6.2831853f * f0 * tt) + 0.5f * std::sin(6.2831853f * 2.0f * f0 * tt)));
        }
    };
    silence(0.20f);
    size_t b = 0;
    for (char32_t c : uni::decode(text)) {
        std::string enc;
        uni::append(enc, c);
        for (size_t h : hesitateAt)
            if (h == b) silence(hesitation);
        s.at[b] = float(s.pcm.size()) / kRate;
        if (c == ' ') silence(base * jitter(rng));
        else if (std::isalnum(int(c < 0x80 ? c : 'a')) || c >= 0x80) tone(base * jitter(rng));
        for (size_t p : pauseAt)
            if (p == b) silence(pause);
        b += enc.size();
    }
    s.at[text.size()] = float(s.pcm.size()) / kRate;
    silence(0.30f);
    return s;
}

size_t offsetOf(const std::string& text, const std::string& word, size_t from = 0) { return text.find(word, from); }

float g_maxError = 0.0f;   // largest raw error seen by the current test (printed)

// Checks every anchor's raw estimate (the bias taken back) is within 0.2 s of the truth, and the
// biased one is never later than the truth.
void checkAnchors(const coach::SpeechTiming& t, const Speech& s, const std::vector<int>& offsets, float bias = 0.10f) {
    CHECK_EQ(t.anchors.size(), offsets.size());
    for (size_t i = 0; i < offsets.size() && i < t.anchors.size(); ++i) {
        float truth = s.at[size_t(offsets[i])];
        float raw = t.anchors[i] + bias;
        g_maxError = std::max(g_maxError, std::fabs(raw - truth));
        if (std::fabs(raw - truth) > 0.2f) {
            std::fprintf(stderr, "  anchor %d: estimated %.3f, true %.3f\n", int(i), raw, truth);
            CHECK(false);
        }
        CHECK(t.anchors[i] <= truth + 0.1f);
    }
}

}  // namespace

TEST(coach_pacing_text_weights_and_boundaries) {
    CHECK_EQ(coach::textWeight("a, b", 0, 4), 3.0f);
    CHECK_EQ(coach::textWeight("Übung!", 0, 7), 5.0f);
    std::string t = "Hello, world. 1.5 and 3:05 \xE2\x80\x94 yes!";
    std::vector<size_t> b = coach::pauseBoundaries(t);
    CHECK_EQ(b.size(), size_t(3));   // the comma, the full stop, the dash; not "1.5", "3:05" or the final "!"
    if (b.size() == 3) {
        CHECK_EQ(t[b[0]], ',');
        CHECK_EQ(t[b[1]], '.');
        CHECK_EQ(b[2], t.find("\xE2\x80\x94"));
    }
    std::vector<size_t> zh = coach::pauseBoundaries("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xE3\x80\x82");   // 你好，世界。
    CHECK_EQ(zh.size(), size_t(1));
}

TEST(coach_pacing_anchors_with_pauses_at_punctuation) {
    g_maxError = 0.0f;
    std::string text = "If you play queen gee five, my knight forks your king and your queen.";
    std::vector<int> offs = {int(offsetOf(text, "queen")), int(offsetOf(text, "my")), int(offsetOf(text, "your king")),
                             int(offsetOf(text, "your queen"))};
    for (unsigned seed = 1; seed <= 12; ++seed) {
        std::mt19937 rng(seed);
        float base = 0.050f + 0.0015f * float(seed);
        Speech s = speak(text, rng, base, {text.find(',')});
        coach::SpeechTiming t = coach::estimateTiming(s.pcm.data(), s.pcm.size(), kRate, text, offs);
        CHECK(std::fabs(t.voiceStart - 0.20f) < 0.02f);
        CHECK_EQ(t.pauses.size(), size_t(1));
        if (!t.pauses.empty()) CHECK_EQ(t.pauses[0].boundary, 0);
        checkAnchors(t, s, offs);
    }
    std::fprintf(stderr, "  largest anchor error: %.3f s\n", g_maxError);
}

TEST(coach_pacing_comma_without_pause_and_hesitation) {
    g_maxError = 0.0f;
    std::string text = "Look at this. My bishop attacks your rook, and your queen is also attacked! Move one of them.";
    std::vector<int> offs = {int(offsetOf(text, "My")), int(offsetOf(text, "your rook")), int(offsetOf(text, "your queen")),
                             int(offsetOf(text, "Move")), int(offsetOf(text, "them"))};
    for (unsigned seed = 1; seed <= 12; ++seed) {
        std::mt19937 rng(100 + seed);
        float base = 0.048f + 0.0018f * float(seed);
        // Pauses after both sentence ends, none at the comma, a hesitation before "attacks" and
        // one before "also".
        Speech s = speak(text, rng, base, {text.find('.'), text.find('!')},
                         {text.find("attacks"), text.find("also")});
        coach::SpeechTiming t = coach::estimateTiming(s.pcm.data(), s.pcm.size(), kRate, text, offs);
        CHECK_EQ(t.pauses.size(), size_t(4));
        int mapped = 0;
        for (auto& p : t.pauses) mapped += p.boundary >= 0;
        CHECK_EQ(mapped, 2);
        checkAnchors(t, s, offs);
    }
    std::fprintf(stderr, "  largest anchor error: %.3f s\n", g_maxError);
}

TEST(coach_pacing_catalog_line_and_fallbacks) {
    Catalog c;
    CHECK(c.load());
    coach::Line l;
    l.key = "lesson.values.pieces";   // "A knight or a {@}bishop, three. A {@2}rook, five. The {@3}queen, nine."
    Catalog::Rendered r = c.renderVariant(l, "en", true, 1);
    CHECK_EQ(r.anchors.size(), size_t(3));
    std::mt19937 rng(7);
    std::vector<size_t> pauses;
    for (size_t i = 0; i < r.text.size(); ++i)
        if (r.text[i] == ',' || (r.text[i] == '.' && i + 1 < r.text.size())) pauses.push_back(i);
    Speech s = speak(r.text, rng, 0.065f, pauses, {}, 0.25f);
    coach::SpeechTiming t = coach::estimateTiming(s.pcm, kRate, r);
    std::vector<int> offs;
    for (auto& a : r.anchors) offs.push_back(a.offset);
    checkAnchors(t, s, offs);
    CHECK_EQ(coach::anchorTime(t, r, "@2"), t.anchors[1]);
    // No such anchor: a fraction of the voiced span (Gesture::at), biased the same way.
    float half = coach::anchorTime(t, r, "nothing", 0.5f);
    CHECK(std::fabs(half + 0.1f - 0.5f * (t.voiceStart + t.voiceEnd)) < 1e-3f);
    // Silent audio: nothing voiced, every anchor at 0.
    std::vector<float> silent(kRate, 0.0f);
    coach::SpeechTiming z = coach::estimateTiming(silent, kRate, r);
    CHECK_EQ(z.voiceEnd, 0.0f);
    CHECK_EQ(z.anchors.size(), size_t(3));
    CHECK_EQ(z.anchors[2], 0.0f);
    coach::SpeechTiming e = coach::estimateTiming(nullptr, 0, kRate, r.text, offs);
    CHECK_EQ(e.anchors.size(), size_t(3));
}
