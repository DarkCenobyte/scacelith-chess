// Procedural noise textures generated once at start-up for the post chain.
#pragma once
#include <cstdint>
#include <vector>

namespace postnoise {
// Void-and-cluster blue noise threshold map (Ulichney 1993), size x size (power of two),
// values are ranks in [0, 65535]. Deterministic.
std::vector<uint16_t> blueNoise(int size, float sigma = 1.9f, uint32_t seed = 1);
// Tiling 3D fractal value noise (period = size), R8, used for the drifting dust density.
std::vector<uint8_t> dustNoise3D(int size, uint32_t seed = 7);
}  // namespace postnoise
