// The golden-ratio phase that animates the post chain's blue noise (postnoise::goldenPhase,
// PostUBO misc.w), and the shader's use of it in blueNoise() (shaders/post/post_common.glsl).
#include "test.h"
#include "render/post/post_noise.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

namespace {
// Distance between two phases on the unit circle (they wrap at 1).
double wrapDist(long double a, long double b) {
    long double d = std::fabs(a - b);
    d -= std::floor(d);
    return double(std::min(d, 1.0L - d));
}
// blueNoise() in single precision: fract(v + misc.w * (1 + channel)).
float shaderNoise(float v, float phase, int channel) {
    float x = v + phase * float(1 + channel);
    return x - std::floor(x);
}
}  // namespace

// The phase is frac(frame * 0.61803398875) to float precision at any frame count. (The float
// product frame * phi the shader used to form kept only 1/16 of its fraction at frame 2^23.)
TEST(post_noise_golden_phase_exact) {
    const uint32_t frames[] = {0u, 1u, 2u, 1000u, 100000u, 400000u, 1500000u, 3000000u,
                               (1u << 23) + 5u, (1u << 24) + 3u, 123456789u, 0xFFFFFFFFu};
    for (uint32_t n : frames) {
        const float p = postnoise::goldenPhase(n);
        CHECK(p >= 0.0f && p <= 1.0f);
        CHECK(wrapDist(p, std::fmod((long double)n * 0.61803398875L, 1.0L)) < 1e-6);
    }
}

// fract(v + phase * (1 + c)) is the intended fract(v + frame * phi * (1 + c)), since
// floor(frame * phi) * (1 + c) is an integer; and late in a session the noise keeps every level.
TEST(post_noise_golden_phase_noise) {
    for (uint32_t n : {3u, 777u, 3000000u, 0xFFFFFFF0u})
        for (int c = 0; c < 10; ++c)
            for (float v : {0.0f, 0.123f, 0.5f, 0.987f}) {
                const long double want = std::fmod((long double)v + (long double)n * 0.61803398875L * (1 + c), 1.0L);
                CHECK(wrapDist(shaderNoise(v, postnoise::goldenPhase(n), c), want) < 2e-5);
            }
    // 4096 noise inputs at frame 3,000,000 (14 h at 60 Hz) on the volumetric channel (5): the
    // GPU's float product collapsed them to a single value.
    std::set<float> distinct;
    const float phase = postnoise::goldenPhase(3000000u);
    for (int k = 0; k < 4096; ++k) distinct.insert(shaderNoise(float(k) / 4096.0f, phase, 5));
    CHECK_EQ(distinct.size(), size_t(4096));
}
