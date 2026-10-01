// When the coach says each word it points at (research-pedagogy §6.4). The voice (Supertonic)
// gives one duration per utterance and no word timings, so the onset of each anchor word is
// estimated from the synthesised audio itself:
//   1. the voiced span: first and last 10 ms windows above -40 dBFS;
//   2. the pauses: internal silences of 120 ms or more, mapped in order onto the punctuation of
//      the spoken text (, ; : — . ! ? and their CJK and Arabic forms) by a small alignment that
//      lets a comma go without a pause and a hesitation go without a comma;
//   3. inside each stretch of speech between two mapped pauses, the anchor's share of the
//      stretch's characters (letters, digits and spaces count 1, punctuation 0), skipping the
//      pauses the alignment left unmapped;
// then every estimate is moved 0.1 s earlier: a gesture a little early looks natural, a late one
// does not (§6.3). Expected error: about ±0.2 s inside a stretch of 3 s or less.
//
// Engine-free and allocation-light: run it on the TTS worker's result, before playback.
#pragma once
#include "catalog.h"
#include <cstddef>
#include <string>
#include <vector>

namespace coach {

struct PacingOptions {
    float thresholdDb = -40.0f;   // a 10 ms window is voiced above this RMS level (dBFS)
    float window = 0.010f;        // RMS window (s)
    float minPause = 0.12f;       // internal silence long enough to be a pause (s)
    float earlyBias = 0.10f;      // every estimate is moved this much earlier (s)
};

struct SpeechTiming {
    float duration = 0.0f;                     // audio length (s)
    float voiceStart = 0.0f, voiceEnd = 0.0f;  // voiced span (s); both 0 for silent audio
    struct Pause {
        float start = 0.0f, end = 0.0f;
        int boundary = -1;                     // punctuation it was mapped to (index), -1 = none
    };
    std::vector<Pause> pauses;                 // internal pauses in time order
    std::vector<float> anchors;                // per anchor offset: estimated onset in seconds from
                                               // the start of the audio (bias applied, >= 0)
};

// Timing of spoken text read from mono PCM. anchorOffsets: byte offsets into spokenText (the
// catalog's Rendered::anchors of the *spoken* rendering).
SpeechTiming estimateTiming(const float* pcm, size_t samples, int sampleRate, const std::string& spokenText,
                            const std::vector<int>& anchorOffsets, const PacingOptions& o = PacingOptions());
// Same for a spoken rendering: anchors[i] belongs to spoken.anchors[i].
SpeechTiming estimateTiming(const std::vector<float>& pcm, int sampleRate, const Catalog::Rendered& spoken,
                            const PacingOptions& o = PacingOptions());

// Seconds from the start of the audio at which the anchor 'name' is heard (the first anchor with
// that name), for a timing made from 'spoken'. Without such an anchor: 'fallbackFraction' (0..1,
// Gesture::at) of the voiced span, biased like the anchors.
float anchorTime(const SpeechTiming& t, const Catalog::Rendered& spoken, const std::string& name,
                 float fallbackFraction = 0.0f, const PacingOptions& o = PacingOptions());

// Building blocks (tests): the character weight of text[begin, end) and the byte offsets of the
// punctuation marks where a voice may pause (not the ones that end the text).
float textWeight(const std::string& text, size_t begin, size_t end);
std::vector<size_t> pauseBoundaries(const std::string& text);

}  // namespace coach
