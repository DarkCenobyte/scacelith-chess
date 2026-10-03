// Stone hall reverb (~14 x 22 x 9 m): early reflections (image-source taps for a listener seated
// at the table: robots/table clutter, side walls, vault, end walls, plus diffuse clusters) and a
// 16-line feedback delay network (Hadamard matrix, 22-76 ms lines, per-line frequency-dependent
// absorption for RT60 ~2.5 s low / ~1.8 s at 4 kHz / ~1.0 s at 8 kHz, slow delay modulation
// against metallic ringing), 24 ms pre-delay, input diffusion and a decorrelated diffuse onset so
// the late field starts right at the pre-delay. Mono in, stereo out.
#pragma once
#include "dsp.h"

namespace audio {

class HallReverb {
public:
    static constexpr int kLines = 16;
    static constexpr int kTaps = 18;
    static constexpr int kBlock = 256;

    void prepare(float sampleRate);
    void clear();
    // in: mono send (n samples, any n). Adds the wet stereo signal into outL / outR.
    void process(const float* in, float* outL, float* outR, int n);

    float rt60Low = 2.6f;    // s, set before prepare()
    float rt60High = 0.5f;   // s at Nyquist (one-pole absorption shape)
    float preDelay = 0.024f;
    float earlyGain = 1.0f, lateGain = 1.0f;

private:
    void processBlock(const float* in, float* outL, float* outR, int n);

    float fs_ = 48000.0f;
    dsp::OnePole inHp_, inLp_, outLpL_, outLpR_;
    dsp::DelayLine er_;
    int tapDelay_[kTaps] = {};
    float tapL_[kTaps] = {}, tapR_[kTaps] = {};
    dsp::DelayLine pre_;
    int preLen_ = 1;
    dsp::Allpass ap_[4];
    dsp::Allpass onsetL_, onsetR_;
    // FDN lines share one buffer: line i occupies [i * lineSize_, (i + 1) * lineSize_).
    std::vector<float> lines_;
    uint32_t lineSize_ = 0, lineMask_ = 0, lineW_ = 0;
    int len_[kLines] = {};
    float fa_[kLines] = {}, fb_[kLines] = {}, fz_[kLines] = {};
    // Modulated lines: delay = len + depth * (1 + sin(phase)), updated per block, ramped per sample.
    static constexpr int kModLines = 4;
    int modLine_[kModLines] = {};
    float modDepth_[kModLines] = {}, modPhase_[kModLines] = {}, modInc_[kModLines] = {}, modCur_[kModLines] = {};
    float norm_ = 1.0f;
};

}  // namespace audio
