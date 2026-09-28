// "posttest": validation scene of the render-post package (not part of the game).
//
// A closed box room lit only through a mullioned window in the -X wall (sun beam -> volumetric
// shafts and dust motes), a polished black floor without planar reflector (SSR), emissive bulbs
// (bloom), fast moving bright objects (motion blur, TAA disocclusion) and objects spread in depth
// (DOF). Deterministic with --shot/--frames (fixed 60 Hz step).
//
// Options: --cam 0|1|2 (view preset), --post-debug N (PostSettings::debugView), --quality 0..3,
//          --no-ssao --no-ssr --no-vol --no-taa --no-mb --no-dof --no-bloom, --autoexposure,
//          --scale S (render scale), --fade F, --fstop N, --focus M, --ev EV100, --sky LUX (fallback sky ambient; low because the
//          base renderer has no interior occlusion of the sky light), --speed K (animation speed: motion
//          blur tests), --static (freeze animation).
// Keys: 1..3 camera preset, 0/F1..F6 debug views (0 = final).
#include "scene.h"
#include "../render/mesh.h"
#include "../render/post/postfx.h"
#include <cstdlib>

using namespace m;

namespace {
MeshData pawnProfile() {
    return prim::lathe({{0, 0}, {0.066f, 0}, {0.066f, 0}, {0.066f, 0.018f}, {0.048f, 0.036f}, {0.027f, 0.09f}, {0.021f, 0.15f},
                        {0.036f, 0.165f}, {0.036f, 0.174f}, {0.022f, 0.18f}, {0.034f, 0.2f}, {0.030f, 0.24f}, {0.0f, 0.255f}});
}
}  // namespace

class PostTestScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        // Room shell: floor slab, walls (0.3 m thick), ceiling, window with a cross-shaped mullion.
        const float X0 = -5.0f, X1 = 5.0f, Z0 = -4.0f, Z1 = 4.0f, H = 4.5f, T = 0.3f;
        const float wz0 = -1.3f, wz1 = 1.3f, wy0 = 0.9f, wy1 = 3.7f;
        MeshData shell;
        auto addBox = [&](vec3 lo, vec3 hi) { shell.append(prim::box((hi - lo) * 0.5f), translate((lo + hi) * 0.5f)); };
        addBox({X0 - T, H, Z0 - T}, {X1 + T, H + T, Z1 + T});             // ceiling
        addBox({X1, 0, Z0 - T}, {X1 + T, H, Z1 + T});                     // +X wall
        addBox({X0 - T, 0, Z1}, {X1 + T, H, Z1 + T});                     // +Z wall
        addBox({X0 - T, 0, Z0 - T}, {X1 + T, H, Z0});                     // -Z wall
        addBox({X0 - T, 0, Z0}, {X0, wy0, Z1});                           // -X wall: below the window
        addBox({X0 - T, wy1, Z0}, {X0, H, Z1});                           //           above
        addBox({X0 - T, wy0, Z0}, {X0, wy1, wz0});                        //           left
        addBox({X0 - T, wy0, wz1}, {X0, wy1, Z1});                        //           right
        addBox({X0 - T, wy0, -0.06f}, {X0, wy1, 0.06f});                  // mullion
        addBox({X0 - T, 2.65f, wz0}, {X0, 2.75f, wz1});                   // transom
        shell_.upload(shell, "posttest.shell");
        floor_.upload(prim::box(vec3((X1 - X0) * 0.5f + T, 0.05f, (Z1 - Z0) * 0.5f + T)), "posttest.floor");
        sphere_.upload(prim::sphere(1.0f, 64, 32), "posttest.sphere");
        box_.upload(prim::roundedBox(vec3(0.5f), 0.04f, 4), "posttest.box");
        column_.upload(prim::cylinder(0.18f, 2.2f, 48), "posttest.column");
        pawn_.upload(pawnProfile(), "posttest.pawn");

        auto mat = [](Material& mt, vec3 albedo, float rough, float metal = 0.0f, vec3 emission = vec3(0), float coat = 0.0f) {
            mt.params[0] = vec4(albedo, rough);
            mt.params[1] = vec4(metal, 0.5f, coat, 0.04f);
            mt.params[2] = vec4(emission, 0.0f);
        };
        mat(wallMat_, vec3(0.72f, 0.68f, 0.62f), 0.85f);
        mat(floorMat_, vec3(0.025f, 0.025f, 0.028f), 0.07f, 0.0f, vec3(0), 1.0f);
        mat(whiteMat_, vec3(0.82f, 0.8f, 0.77f), 0.18f, 0.0f, vec3(0), 1.0f);
        mat(chromeMat_, vec3(0.95f, 0.93f, 0.88f), 0.06f, 1.0f);
        mat(goldMat_, vec3(1.0f, 0.78f, 0.36f), 0.22f, 1.0f);
        mat(redMat_, vec3(0.5f, 0.05f, 0.04f), 0.3f, 0.0f, vec3(0), 1.0f);
        mat(blackMat_, vec3(0.03f), 0.12f, 0.0f, vec3(0), 1.0f);
        mat(bulbMat_, vec3(1.0f), 0.5f, 0.0f, vec3(1.0f, 0.78f, 0.52f) * 60000.0f);
        mat(blueBulbMat_, vec3(1.0f), 0.5f, 0.0f, vec3(0.45f, 0.65f, 1.0f) * 40000.0f);
        for (Material* mt : {&whiteMat_, &chromeMat_, &goldMat_, &redMat_, &blackMat_}) mt->tessellated = true;

        // Options.
        camPreset_ = std::atoi(ctx.argValue("--cam", "0").c_str());
        animate_ = !ctx.hasArg("--static");
        render::Renderer& r = *ctx.renderer;
        render::RenderSettings rs = r.settings();
        if (ctx.hasArg("--quality")) rs.applyPreset(render::Quality(std::clamp(std::atoi(ctx.argValue("--quality").c_str()), 0, 3)));
        if (ctx.hasArg("--no-ssao")) rs.ssao = false;
        if (ctx.hasArg("--no-ssr")) rs.ssr = false;
        if (ctx.hasArg("--no-vol")) rs.volumetrics = false;
        if (ctx.hasArg("--no-taa")) rs.taa = false;
        if (ctx.hasArg("--no-mb")) rs.motionBlur = false;
        if (ctx.hasArg("--no-dof")) rs.dof = false;
        if (ctx.hasArg("--no-bloom")) rs.bloom = false;
        if (ctx.hasArg("--scale")) rs.renderScale = float(std::atof(ctx.argValue("--scale").c_str()));
        r.setSettings(rs);
        PostSettings& ps = r.post().settings;
        ps.debugView = std::atoi(ctx.argValue("--post-debug", "0").c_str());
        ps.autoExposure = ctx.hasArg("--autoexposure");
        ps.dofFStop = float(std::atof(ctx.argValue("--fstop", "2.0").c_str()));
        focusOverride_ = float(std::atof(ctx.argValue("--focus", "0").c_str()));
        r.fade = float(std::atof(ctx.argValue("--fade", "0").c_str()));
        ev_ = float(std::atof(ctx.argValue("--ev", "11.0").c_str()));
        sky_ = float(std::atof(ctx.argValue("--sky", "2500").c_str()));
        speed_ = float(std::atof(ctx.argValue("--speed", "1").c_str()));
        time_ = ctx.fixedTime >= 0.0f ? ctx.fixedTime : 0.0f;
        return true;
    }

    bool update(AppContext& ctx, float dt) override {
        const plat::Input& in = plat::input();
        if (animate_) time_ += dt * speed_;
        for (int k = 1; k <= 3; ++k)
            if (in.keyPressed[plat::KEY_0 + k]) camPreset_ = k - 1;
        PostSettings& ps = ctx.renderer->post().settings;
        if (in.keyPressed[plat::KEY_0]) ps.debugView = 0;
        for (int k = 0; k < 6; ++k)
            if (in.keyPressed[plat::KEY_F1 + k]) ps.debugView = k + 1;
        return !in.keyPressed[plat::KEY_ESCAPE];
    }

    render::Camera camera() const {
        render::Camera c;
        c.fovY = 50.0f * DEG;
        c.nearZ = 0.03f;
        switch (camPreset_) {
            case 1:  // along the beam towards the window (backlit dust, strong forward scattering)
                c.position = vec3(2.6f, 1.25f, -2.2f);
                c.lookAt(vec3(-5.0f, 2.3f, 0.2f));
                break;
            case 2:  // close-up at table height: pieces at several depths (DOF), moving "hand"
                c.position = vec3(3.3f, 1.05f, 1.9f);
                c.lookAt(vec3(1.6f, 0.62f, 0.6f));
                break;
            default:  // overview: window, beam, sunlit floor patch, glossy floor
                c.position = vec3(4.3f, 1.65f, 3.3f);
                c.lookAt(vec3(-1.6f, 1.05f, -1.0f));
                break;
        }
        return c;
    }

    void submit(render::Renderer& r, const Mesh& mesh, const Material& mat, const mat4& model, const mat4& prev, uint32_t id,
                uint32_t flags = render::DRAW_CAST_SHADOW) {
        render::DrawItem d;
        d.mesh = &mesh;
        d.material = &mat;
        d.model = model;
        d.prevModel = prev;
        d.hasPrevModel = true;
        d.objectId = id;
        d.flags = flags;
        r.submit(d);
    }

    // Animated transforms as a function of time (evaluated at t and t - dt for motion vectors).
    mat4 orbiter(float t) const { return translate(vec3(1.2f + 0.7f * std::cos(t * 7.5f), 0.9f, 0.3f + 0.7f * std::sin(t * 7.5f))) * scale(vec3(0.14f)); }
    mat4 shuttle(float t) const { return translate(vec3(-0.5f + 1.6f * std::sin(t * 4.0f), 0.35f, 1.4f)) * scale(vec3(0.06f)); }
    mat4 hand(float t) const {
        float s = std::sin(t * 5.0f);
        return translate(vec3(2.2f + 0.05f * s, 0.78f + 0.08f * std::cos(t * 5.0f), 1.3f + 0.55f * s)) * rotateY(0.4f + 0.5f * s) *
               scale(vec3(0.09f, 0.035f, 0.18f));
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env;
        env.time = time_;
        env.exposureEV100 = ev_;
        env.skyIlluminance = sky_;
        render::Camera cam = camera();
        PostSettings& ps = r.post().settings;
        vec3 focusTarget = camPreset_ == 2 ? vec3(1.6f, 0.62f, 0.6f) : camPreset_ == 1 ? vec3(-1.5f, 1.4f, -0.8f) : vec3(0.0f, 0.6f, -0.4f);
        ps.dofFocusDistance = focusOverride_ > 0.0f ? focusOverride_ : dot(focusTarget - cam.position, cam.forward());
        r.beginFrame(cam, env, dt);
        float t = time_, tp = time_ - (animate_ ? dt * speed_ : 0.0f);
        const uint32_t still = render::DRAW_CAST_SHADOW | render::DRAW_STATIC;
        mat4 I;
        submit(r, shell_, wallMat_, I, I, 1, still);
        submit(r, floor_, floorMat_, translate(vec3(0, -0.05f, 0)), translate(vec3(0, -0.05f, 0)), 2, still);
        // Columns at several depths (DOF) and a plinth in the sun patch.
        const vec3 cols[4] = {{-3.2f, 0, 2.6f}, {-0.6f, 0, 2.9f}, {2.0f, 0, -3.2f}, {-3.9f, 0, -3.3f}};
        for (int i = 0; i < 4; ++i) submit(r, column_, whiteMat_, translate(cols[i]), translate(cols[i]), 10u + uint32_t(i), still);
        mat4 plinth = translate(vec3(-1.4f, 0.3f, -1.9f)) * scale(vec3(0.7f, 0.6f, 0.7f));
        submit(r, box_, whiteMat_, plinth, plinth, 20, still);
        mat4 bust = translate(vec3(-1.4f, 0.85f, -1.9f)) * scale(vec3(0.25f));
        submit(r, sphere_, whiteMat_, bust, bust, 21, still);
        // Chrome and gold spheres on the glossy floor (SSR on curved metal + their reflections).
        mat4 chrome = translate(vec3(0.6f, 0.45f, -0.9f)) * scale(vec3(0.45f));
        submit(r, sphere_, chromeMat_, chrome, chrome, 22, still);
        mat4 gold = translate(vec3(2.3f, 0.25f, -1.4f)) * scale(vec3(0.25f));
        submit(r, sphere_, goldMat_, gold, gold, 23, still);
        mat4 red = translate(vec3(-2.6f, 0.3f, 0.9f)) * rotateY(0.6f) * scale(vec3(0.6f));
        submit(r, box_, redMat_, red, red, 24, still);
        // A low table with pawns at increasing distance (DOF close-up preset).
        mat4 table = translate(vec3(1.9f, 0.27f, 0.9f)) * scale(vec3(1.3f, 0.54f, 1.0f));
        submit(r, box_, blackMat_, table, table, 30, still);
        for (int i = 0; i < 6; ++i) {
            mat4 pm = translate(vec3(2.35f - 0.22f * float(i), 0.54f, 1.25f - 0.16f * float(i))) * scale(vec3(0.55f));
            submit(r, pawn_, (i & 1) ? blackMat_ : whiteMat_, pm, pm, 31u + uint32_t(i), still);
        }
        // Emissive bulbs (bloom) hanging in the room.
        const vec3 bulbs[3] = {{3.2f, 3.1f, -2.6f}, {-2.2f, 3.4f, 2.8f}, {0.8f, 2.6f, 3.2f}};
        for (int i = 0; i < 3; ++i) {
            mat4 bm = translate(bulbs[i]) * scale(vec3(0.07f));
            submit(r, sphere_, i == 1 ? blueBulbMat_ : bulbMat_, bm, bm, 40u + uint32_t(i), 0);
        }
        // Fast movers: a white orbiting sphere, a bright shuttle, a "hand" swiping over the table.
        submit(r, sphere_, whiteMat_, orbiter(t), orbiter(tp), 50);
        submit(r, sphere_, bulbMat_, shuttle(t), shuttle(tp), 51, 0);
        submit(r, box_, goldMat_, hand(t), hand(tp), 52);
        r.endFrame();
    }

private:
    Mesh shell_, floor_, sphere_, box_, column_, pawn_;
    Material wallMat_, floorMat_, whiteMat_, chromeMat_, goldMat_, redMat_, blackMat_, bulbMat_, blueBulbMat_;
    int camPreset_ = 0;
    bool animate_ = true;
    float time_ = 0.0f, ev_ = 11.0f, sky_ = 2500.0f, speed_ = 1.0f, focusOverride_ = 0.0f;
};

SCACELITH_SCENE("posttest", "Post-processing validation (sun beam, SSR floor, bloom, motion blur, DOF)", PostTestScene);
