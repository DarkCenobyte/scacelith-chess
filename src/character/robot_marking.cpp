// Chest marking (Coach mode): a word printed in the porcelain glaze of the chest shell. The word's
// distance field is baked on the CPU from the title face and sampled by robot_porcelain.glsl
// (ROBOT_MARKING), projected along Spine2's +Z; see ChestMarking in robot.h.
#include "robot.h"
#include "../core/log.h"
#include "../platform/platform.h"
#include "../render/gpu.h"
#include "../ui/ui_font.h"

using namespace m;

namespace character {

namespace {
// Distance field: cap height kCapPx texels in a kTexW x kTexH texture, kSpread texels each side.
constexpr int kTexW = 512, kTexH = 128, kSpread = 8;
constexpr float kCapPx = 64.0f;
// Print size and place (Spine2 space, metres). The front of the upper chest is nearly flat between
// the pectoral seam (y ~ 0.119) and the collar slope (y > 0.19): over |x| < 0.055 the surface stays
// within 5 mm of a plane and 17 degrees of +Z, so a projection along +Z does not stretch the
// letters. A 19 mm cap height reads at the human's seat (~1.1 m: ~20 px at 1080p, ~14 at 720p).
constexpr float kCapHeight = 0.019f;
constexpr float kCentreY = 0.166f;        // middle of the cap band
constexpr float kTracking = 0.14f;        // em: spaced capitals, as maker's marks are
// Cinzel's stems are ~0.075 em (2 mm here) and its hairlines ~0.03 em (0.8 mm): at about a pixel
// per millimetre, and through the depth of field of a player looking at the board, the regular
// weight fades to a grey smear, so the print is bolder than the face (0.5 mm added on each side
// of every stroke: stems ~3 mm, hairlines ~1.8 mm).
constexpr float kDilation = 0.0005f;
constexpr float kBleed = 0.00009f;        // soft pigment edge (fired under-glaze colour)
// Cobalt blue (linear albedo): the colour of blue-and-white porcelain, and the coach's accent.
const vec3 kPigment(0.008f, 0.020f, 0.135f);
constexpr float kTranslucencyUnderInk = 0.2f;
constexpr float kMinNormalZ = 0.5f;
}  // namespace

bool ChestMarking::create(const std::string& text) {
    destroy();
    double t0 = plat::time();
    std::vector<uint8_t> px;
    float inkPx = 0.0f;
    if (!ui::font::renderLineSdf(ui::font::FACE_TITLE, text, kCapPx, kSpread, kTracking, kTexW, kTexH, px, &inkPx)) {
        LOGW("robot: chest marking \"%s\" not rendered", text.c_str());
        return false;
    }
    gpu::Texture tex = gpu::createTexture2D(kTexW, kTexH, GL_R8, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(tex.id, 0, 0, 0, kTexW, kTexH, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateTextureMipmap(tex.id);
    gpu::setFilter(tex, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
    gpu::setWrap(tex, GL_CLAMP_TO_EDGE);   // the border texels are outside the ink (margin > spread)
    gpu::setAnisotropy(tex, 8.0f);         // seen from the other chair, ~15 degrees off the axis
    texture = tex.id;

    const float texel = kCapHeight / kCapPx;  // metres per texel
    material = materials::get(MaterialId::RobotPorcelain);
    material.name = "RobotPorcelainMarked";
    material.defines.push_back("ROBOT_MARKING");
    material.textures[0] = texture;
    material.params[5] = vec4(0.0f, kCentreY, float(kTexW) * texel, float(kTexH) * texel);
    material.params[6] = vec4(kPigment, 2.0f * float(kSpread));
    material.params[7] = vec4(kDilation / texel, kTranslucencyUnderInk, kMinNormalZ, kBleed / texel);
    LOGI("robot: chest marking \"%s\" %.0f mm wide (%.0f ms)", text.c_str(), double(inkPx * texel * 1000.0f),
         (plat::time() - t0) * 1000.0);
    return true;
}

void ChestMarking::destroy() {
    if (texture) glDeleteTextures(1, &texture);
    texture = 0;
    material = Material();
}

}  // namespace character
