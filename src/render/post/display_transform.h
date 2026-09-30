// CPU port of the final display transform of shaders/post/tonemap.frag: displayTransform() (AgX
// in the Rec.2020 working space with the grade's pivoted contrast, split toning and saturation)
// and srgbEncode(). The UI uses it to show colours exactly as the 3D frame would show a given
// pre-exposed radiance (the brightness calibration's patches); tests/display_transform_tests.cpp
// checks it against reference values computed from the shader.
//
// Keep it line for line with the shader: the same matrices (GLSL mat3 constructors take columns,
// as m::mat3 does), constants and operation order. Vignette, bloom, grain and dither are not part
// of it (they depend on the pixel).
#pragma once
#include "../../math/math.h"
#include <algorithm>
#include <cmath>

namespace render {

// 'c' is linear sRGB radiance after exposure (the shader's colour after c *= exp2(ev)); returns
// linear display sRGB in [0, 1]. contrast, saturation and splitTone are PostSettings' grade.
inline m::vec3 displayTransform(m::vec3 c, float contrast, float saturation, float splitTone) {
    const m::mat3 srgbToRec2020(m::vec3(0.6274f, 0.0691f, 0.0164f), m::vec3(0.3293f, 0.9195f, 0.0880f),
                                m::vec3(0.0433f, 0.0113f, 0.8956f));
    const m::mat3 rec2020ToSrgb(m::vec3(1.6605f, -0.1246f, -0.0182f), m::vec3(-0.5876f, 1.1329f, -0.1006f),
                                m::vec3(-0.0728f, -0.0083f, 1.1187f));
    const m::mat3 agxInset(m::vec3(0.856627153315983f, 0.137318972929847f, 0.11189821299995f),
                           m::vec3(0.0951212405381588f, 0.761241990602591f, 0.0767994186031903f),
                           m::vec3(0.0482516061458583f, 0.101439036467562f, 0.811302368396859f));
    const m::mat3 agxOutset(m::vec3(1.1271005818144368f, -0.1413297634984383f, -0.14132976349843826f),
                            m::vec3(-0.11060664309660323f, 1.157823702216272f, -0.11060664309660294f),
                            m::vec3(-0.016493938717834573f, -0.016493938717834257f, 1.2519364065950405f));
    const float minEv = -12.47393f, maxEv = 4.026069f;
    const m::vec3 lumaWeights(0.2626f, 0.6780f, 0.0593f);
    c = srgbToRec2020 * c;
    c = agxInset * c;
    for (int i = 0; i < 3; ++i) {
        float x = m::clamp((std::log2(std::max(c[i], 1e-10f)) - minEv) / (maxEv - minEv), 0.0f, 1.0f);
        float x2 = x * x, x4 = x2 * x2;
        c[i] = 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    }
    // Look: pivoted S-curve contrast, cool shadows / warm highlights, saturation.
    float l = m::dot(c, lumaWeights);
    const float pivot = 0.42f;
    for (int i = 0; i < 3; ++i) {
        float x = m::clamp(c[i], 0.0f, 1.0f);
        c[i] = x < pivot ? pivot * std::pow(x / pivot, contrast)
                         : 1.0f - (1.0f - pivot) * std::pow((1.0f - x) / (1.0f - pivot), contrast);
    }
    const m::vec3 cool(0.985f, 1.0f, 1.03f), warm(1.03f, 1.0f, 0.955f);
    m::vec3 tone = m::lerp(cool, warm, m::smoothstep(0.18f, 0.72f, l));
    c = c * m::lerp(m::vec3(1.0f), tone, splitTone);
    l = m::dot(c, lumaWeights);
    c = m::vec3(l) + (c - m::vec3(l)) * saturation;
    c = agxOutset * c;
    for (int i = 0; i < 3; ++i) c[i] = std::pow(std::max(c[i], 0.0f), 2.2f);
    c = rec2020ToSrgb * c;
    return m::clamp(c, 0.0f, 1.0f);
}

// Linear -> sRGB transfer function (the shader's srgbEncode).
inline m::vec3 srgbEncode(m::vec3 c) {
    for (int i = 0; i < 3; ++i) c[i] = c[i] < 0.0031308f ? c[i] * 12.92f : 1.055f * std::pow(c[i], 1.0f / 2.4f) - 0.055f;
    return c;
}

}  // namespace render
