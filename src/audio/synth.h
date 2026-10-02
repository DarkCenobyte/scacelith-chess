// Procedural one-shot synthesis (no samples): physically inspired modal + noise models.
// Each call renders one variant (mono, kBankRate) from a seed; all physical parameters
// (contact times, mode frequencies/decays, impact positions, bounces...) are jittered per seed.
#pragma once
#include "audio.h"
#include <cstdint>
#include <vector>

namespace audio {

constexpr int kBankRate = 48000;
constexpr int kBankVariants = 6;  // synthesised variants per Sfx kept in the bank (max)

struct SfxInfo {
    const char* name;
    float level;        // linear peak level at the 0.5 m reference distance (before volumes)
    float roomSend;     // hall send, relative to level (distance independent: diffuse field)
    float pitchJitter;  // +- relative pitch randomisation per trigger
    float levelJitterDb;
    bool ui;            // UI bus (playUI renders it centred; the hall send still applies)
};
const SfxInfo& sfxInfo(Sfx s);

// Renders one variant of 's' (mono, kBankRate Hz), peak-normalised to 1, DC-free, faded tail.
std::vector<float> synthesize(Sfx s, uint32_t seed);
// Number of variants of 's' kept in the bank (<= kBankVariants).
int bankVariants(Sfx s);

}  // namespace audio
