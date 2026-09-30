// CPU port of the tonemapper's display transform (src/render/post/display_transform.h), checked
// against values computed from shaders/post/tonemap.frag's displayTransform() and srgbEncode() in
// double precision (an independent transcription of the GLSL).
#include "test.h"
#include "render/post/display_transform.h"
#include <cmath>

namespace {
// sRGB-encoded display colour of a pre-exposed radiance, with the given grade.
m::vec3 encoded(m::vec3 c, float contrast = 1.12f, float saturation = 1.06f, float splitTone = 1.0f) {
    return render::srgbEncode(render::displayTransform(c, contrast, saturation, splitTone));
}
bool near(m::vec3 got, m::vec3 want, float tol = 5e-4f) {
    bool ok = std::fabs(got.x - want.x) < tol && std::fabs(got.y - want.y) < tol && std::fabs(got.z - want.z) < tol;
    if (!ok)
        std::fprintf(stderr, "  got (%.5f, %.5f, %.5f), expected (%.5f, %.5f, %.5f)\n", got.x, got.y, got.z, want.x, want.y,
                     want.z);
    return ok;
}
}  // namespace

// PostSettings' grade (contrast 1.12, saturation 1.06, split tone 1): black stays black, the
// near-black toe, mid grey, white and highlights, and saturated colours.
TEST(display_transform_matches_shader) {
    CHECK(near(encoded(m::vec3(0.0f)), m::vec3(0.0f)));
    CHECK(near(encoded(m::vec3(1e-5f)), m::vec3(0.0f)));
    CHECK(near(encoded(m::vec3(0.0045f)), m::vec3(0.00958f, 0.01045f, 0.01147f)));
    CHECK(near(encoded(m::vec3(0.18f)), m::vec3(0.52434f, 0.50719f, 0.49471f)));
    CHECK(near(encoded(m::vec3(1.0f)), m::vec3(0.86814f, 0.80870f, 0.75731f)));
    CHECK(near(encoded(m::vec3(2.5f)), m::vec3(0.97673f, 0.91032f, 0.85287f)));
    CHECK(near(encoded(m::vec3(0.5f, 0.2f, 0.05f)), m::vec3(0.75287f, 0.52382f, 0.30408f)));
    CHECK(near(encoded(m::vec3(0.02f, 0.05f, 0.3f)), m::vec3(0.09390f, 0.33400f, 0.67785f)));
    CHECK(near(encoded(m::vec3(4.0f, 3.0f, 2.0f)), m::vec3(1.0f, 0.92265f, 0.83605f)));
    // The linear output, before the transfer function.
    CHECK(near(render::displayTransform(m::vec3(0.18f), 1.12f, 1.06f, 1.0f), m::vec3(0.237263f, 0.220753f, 0.209181f), 2e-4f));
}

// A neutral grade (no contrast, saturation or split tone) takes the grade parameters into account.
TEST(display_transform_grade_parameters) {
    CHECK(near(encoded(m::vec3(0.18f), 1.0f, 1.0f, 0.0f), m::vec3(0.50055f, 0.50050f, 0.50049f)));
    CHECK(near(encoded(m::vec3(0.5f, 0.2f, 0.05f), 1.0f, 1.0f, 0.0f), m::vec3(0.69777f, 0.52004f, 0.35093f)));
    CHECK(near(encoded(m::vec3(0.02f, 0.05f, 0.3f), 1.0f, 1.0f, 0.0f), m::vec3(0.17197f, 0.34264f, 0.62733f)));
}

// Exposure compensation brightens monotonically, and the transfer function matches sRGB.
TEST(display_transform_monotonic_and_srgb) {
    float prev = -1.0f;
    for (float ev = -2.0f; ev <= 2.01f; ev += 0.25f) {
        float v = encoded(m::vec3(0.0065f * std::exp2(ev))).y;
        CHECK(v > prev);
        prev = v;
    }
    CHECK(near(render::srgbEncode(m::vec3(0.0f, 0.002f, 1.0f)), m::vec3(0.0f, 0.02584f, 1.0f), 1e-5f));
    CHECK(near(render::srgbEncode(m::vec3(0.5f)), m::vec3(0.73536f), 1e-4f));
}
