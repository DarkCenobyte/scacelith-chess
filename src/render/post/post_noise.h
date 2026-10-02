// Procedural noise textures generated once at start-up for the post chain.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace postnoise {
// Void-and-cluster blue noise threshold map (Ulichney 1993), size x size (power of two),
// values are ranks in [0, 65535]. Deterministic.
std::vector<uint16_t> blueNoise(int size, float sigma = 1.9f, uint32_t seed = 1);
// Tiling 3D fractal value noise (period = size), R8, used for the drifting dust density.
std::vector<uint8_t> dustNoise3D(int size, uint32_t seed = 7);
// Golden-ratio phase that animates the blue noise (PostUBO misc.w): frac(frame * 0.61803398875),
// computed in double. The float product the shader used to form lost its fraction as the frame
// count grew, and the noise collapsed to a few values within hours.
inline float goldenPhase(uint32_t frame) { return float(std::fmod(double(frame) * 0.61803398875, 1.0)); }
}  // namespace postnoise
