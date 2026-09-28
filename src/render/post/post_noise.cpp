#include "post_noise.h"
#include "../../math/math.h"
#include <algorithm>
#include <cmath>

namespace postnoise {

std::vector<uint16_t> blueNoise(int size, float sigma, uint32_t seed) {
    const int n = size * size, mask = size - 1;
    // Toroidal Gaussian kernel indexed by wrapped offset.
    std::vector<float> kernel(size_t(n), 0.0f);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int dx = std::min(x, size - x), dy = std::min(y, size - y);
            kernel[size_t(y * size + x)] = std::exp(-float(dx * dx + dy * dy) / (2.0f * sigma * sigma));
        }
    std::vector<float> energy(size_t(n), 0.0f);
    std::vector<uint8_t> bits(size_t(n), 0);
    auto toggle = [&](int q, float s) {
        int qx = q & mask, qy = q / size;
        for (int y = 0; y < size; ++y) {
            const float* krow = &kernel[size_t(((y - qy) & mask) * size)];
            float* erow = &energy[size_t(y * size)];
            for (int x = 0; x < size; ++x) erow[x] += s * krow[(x - qx) & mask];
        }
    };
    auto tightestCluster = [&]() {  // the 1 with the highest energy
        int best = -1;
        float e = -1e30f;
        for (int i = 0; i < n; ++i)
            if (bits[size_t(i)] && energy[size_t(i)] > e) { e = energy[size_t(i)]; best = i; }
        return best;
    };
    auto largestVoid = [&]() {  // the 0 with the lowest energy
        int best = -1;
        float e = 1e30f;
        for (int i = 0; i < n; ++i)
            if (!bits[size_t(i)] && energy[size_t(i)] < e) { e = energy[size_t(i)]; best = i; }
        return best;
    };
    // Initial random pattern (~10 %), then relax it into a blue-noise pattern.
    m::Rng rng(seed);
    int ones = n / 10;
    for (int placed = 0; placed < ones;) {
        int i = int(rng.next() % uint32_t(n));
        if (bits[size_t(i)]) continue;
        bits[size_t(i)] = 1;
        toggle(i, 1.0f);
        ++placed;
    }
    for (int iter = 0; iter < n * 4; ++iter) {
        int c = tightestCluster();
        bits[size_t(c)] = 0;
        toggle(c, -1.0f);
        int v = largestVoid();
        bits[size_t(v)] = 1;
        toggle(v, 1.0f);
        if (v == c) break;
    }
    std::vector<uint8_t> proto = bits;
    std::vector<float> protoEnergy = energy;
    std::vector<int> rank(size_t(n), 0);
    // Phase 1: rank the prototype's ones by removing tightest clusters.
    for (int r = ones - 1; r >= 0; --r) {
        int c = tightestCluster();
        bits[size_t(c)] = 0;
        toggle(c, -1.0f);
        rank[size_t(c)] = r;
    }
    // Phases 2+3: from the prototype, fill the largest voids (min energy of ones == max energy
    // of zeros, so both phases reduce to the same rule).
    bits = proto;
    energy = protoEnergy;
    for (int r = ones; r < n; ++r) {
        int v = largestVoid();
        bits[size_t(v)] = 1;
        toggle(v, 1.0f);
        rank[size_t(v)] = r;
    }
    std::vector<uint16_t> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out[size_t(i)] = uint16_t((uint32_t(rank[size_t(i)]) * 65535u) / uint32_t(n - 1));
    return out;
}

std::vector<uint8_t> dustNoise3D(int size, uint32_t seed) {
    auto lattice = [&](int x, int y, int z, int period) {
        x &= period - 1; y &= period - 1; z &= period - 1;
        uint32_t h = m::hash32(uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u ^ seed * 2654435761u);
        return float(h & 0xFFFFFF) / 16777215.0f;
    };
    auto valueNoise = [&](float x, float y, float z, int period) {
        int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
        float fx = x - float(ix), fy = y - float(iy), fz = z - float(iz);
        auto s = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
        float ux = s(fx), uy = s(fy), uz = s(fz);
        float r = 0.0f;
        for (int k = 0; k < 8; ++k) {
            int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
            float w = (dx ? ux : 1.0f - ux) * (dy ? uy : 1.0f - uy) * (dz ? uz : 1.0f - uz);
            r += w * lattice(ix + dx, iy + dy, iz + dz, period);
        }
        return r;
    };
    std::vector<float> v(size_t(size) * size_t(size) * size_t(size));
    float lo = 1e9f, hi = -1e9f;
    for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f, amp = 0.55f, norm = 0.0f;
                int period = 4;
                for (int o = 0; o < 4 && period <= size; ++o) {
                    float f = float(period) / float(size);
                    sum += amp * valueNoise(float(x) * f, float(y) * f, float(z) * f, period);
                    norm += amp;
                    amp *= 0.5f;
                    period *= 2;
                }
                float val = sum / norm;
                v[size_t((z * size + y) * size + x)] = val;
                lo = std::min(lo, val);
                hi = std::max(hi, val);
            }
    std::vector<uint8_t> out(v.size());
    for (size_t i = 0; i < v.size(); ++i) out[i] = uint8_t(std::lround(255.0f * (v[i] - lo) / std::max(hi - lo, 1e-6f)));
    return out;
}

}  // namespace postnoise
