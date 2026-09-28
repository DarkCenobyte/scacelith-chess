// "testbed": outdoor material / lighting validation scene (physical sky and sun, PCSS contact
// shadows, planar reflections, light probe, and one sphere per BRDF feature). Not part of the game.
//
//   spheres, left to right: white marble (subsurface + clear coat), black marble, gold, blue velvet
//   (sheen), brushed steel (anisotropy), window glass (transmission), red lacquer (clear coat over a
//   rough base); a row of marble pawns; a rotating porcelain box; a thin backlit cloth panel.
//
//   --sun az,el   sun azimuth / elevation in degrees (azimuth 0 = from -X, positive towards +Z)
//   --ev X        exposure EV100 (default 14.7, sunny outdoors)
//   --dusk        civil twilight (sun 4 deg below the horizon), EV 8.5: the point and spot lights dominate
//   --view N      0 default orbit, 1 low grazing view over the reflective floor, 2 close-up of the pawns
#include "orbit_camera.h"
#include "scene.h"
#include "../render/mesh.h"
#include <cstdio>

using namespace m;

namespace {
Material testMat(vec3 albedo, float rough, float metal, float cc, float ccRough) {
    Material m;
    m.surface = "shaders/lighting/test_stone.glsl";
    m.params[0] = vec4(albedo, rough);
    m.params[1] = vec4(metal, 0.5f, cc, ccRough);
    m.params[3] = vec4(1, 1, 1, 0);
    m.params[4] = vec4(0, 0, 0.01f, 0);
    return m;
}
}  // namespace

class TestbedScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        floor_.upload(prim::plane(6, 6, 1, 1, 1.0f), "floor");
        sphere_.upload(prim::sphere(0.12f, 64, 32), "sphere");
        box_.upload(prim::roundedBox(vec3(0.15f, 0.1f, 0.15f), 0.01f, 3), "box");
        MeshData lathe = prim::lathe({{0, 0}, {0.022f, 0}, {0.022f, 0}, {0.022f, 0.006f}, {0.016f, 0.012f}, {0.009f, 0.03f},
                                      {0.007f, 0.05f}, {0.012f, 0.055f}, {0.012f, 0.058f}, {0.0f, 0.075f}});
        pawn_.upload(lathe, "pawn");
        panel_.upload(prim::roundedBox(vec3(0.001f, 0.2f, 0.16f), 0.0008f, 1), "panel");

        floorMat_ = testMat(vec3(0.80f, 0.78f, 0.74f), 0.18f, 0.0f, 1.0f, 0.03f);
        floorMat_.params[2] = vec4(0.55f, 0.53f, 0.5f, 0.8f);
        floorMat_.params[4] = vec4(0.5f, 2.2f, 0.01f, 0.35f);
        floorMat_.planarReflector = 0;

        Material& whiteMarble = mats_[0];
        whiteMarble = testMat(vec3(0.88f, 0.86f, 0.81f), 0.3f, 0.0f, 1.0f, 0.04f);
        whiteMarble.params[2] = vec4(0.55f, 0.55f, 0.56f, 0.7f);
        whiteMarble.params[3] = vec4(1.0f, 0.86f, 0.72f, 0.75f);
        whiteMarble.params[4] = vec4(0, 22.0f, 0.012f, 0);
        Material& blackMarble = mats_[1];
        blackMarble = testMat(vec3(0.022f, 0.021f, 0.02f), 0.3f, 0.0f, 1.0f, 0.04f);
        blackMarble.params[2] = vec4(0.7f, 0.66f, 0.55f, 0.5f);
        blackMarble.params[4] = vec4(0, 22.0f, 0.004f, 0);
        mats_[2] = testMat(vec3(1.0f, 0.77f, 0.36f), 0.22f, 1.0f, 0.0f, 0.1f);          // gold
        Material& velvet = mats_[3];
        velvet = testMat(vec3(0.035f, 0.055f, 0.32f), 0.85f, 0.0f, 0.0f, 0.1f);
        velvet.params[5] = vec4(0.35f, 0.4f, 0.75f, 0.4f);
        Material& steel = mats_[4];
        steel = testMat(vec3(0.62f, 0.62f, 0.64f), 0.35f, 1.0f, 0.0f, 0.1f);
        steel.params[6] = vec4(0.8f, 0, 0, 0);
        Material& glass = mats_[5];
        glass.surface = "shaders/lighting/test_glass.glsl";
        glass.transparent = true;
        glass.castShadow = false;
        glass.params[0] = vec4(0.92f, 0.97f, 0.95f, 0.02f);
        glass.params[1] = vec4(1.5f, 1.0f, 1.0f, 0);
        mats_[6] = testMat(vec3(0.45f, 0.02f, 0.02f), 0.6f, 0.0f, 1.0f, 0.02f);          // red lacquer
        Material& cloth = mats_[7];
        cloth = testMat(vec3(0.6f, 0.08f, 0.06f), 0.9f, 0.0f, 0.0f, 0.1f);
        cloth.params[3] = vec4(1.0f, 0.4f, 0.3f, 0.6f);
        cloth.params[5] = vec4(0.5f, 0.2f, 0.2f, 0.5f);
        cloth.params[6] = vec4(0, 0.002f, 0, 0);
        cloth.doubleSided = true;
        porcelain_ = testMat(vec3(0.85f, 0.85f, 0.83f), 0.35f, 0.0f, 1.0f, 0.05f);
        porcelain_.params[3] = vec4(1.0f, 0.95f, 0.9f, 0.3f);
        for (int k = 0; k < 7; ++k) mats_[k].tessellated = !mats_[k].transparent;

        render::Renderer& r = *ctx.renderer;
        render::PlanarReflector pr;
        pr.point = vec3(0, 0, 0);
        pr.bounds.add(vec3(-3, 0, -3));
        pr.bounds.add(vec3(3, 0, 3));
        r.addPlanarReflector(pr);
        // Lighting layout for an outdoor terrace: one local probe, shadow regions around the props.
        AABB bounds;
        bounds.add(vec3(-3.2f, -0.1f, -3.2f));
        bounds.add(vec3(3.2f, 1.2f, 3.2f));
        r.setSceneBounds(bounds);
        AABB regions[2];
        regions[0].add(vec3(-1.1f, 0.0f, -1.0f));
        regions[0].add(vec3(1.1f, 0.45f, 0.9f));
        regions[1] = bounds;
        r.setShadowRegions(regions, 2);
        render::LightProbeDesc probe;
        probe.position = vec3(0.0f, 0.35f, 0.0f);
        probe.radius = 4.0f;
        probe.innerRadius = 2.5f;
        probe.priority = true;
        probe.box.add(vec3(-60, -0.001f, -60));   // open sky: distant parallax box, floor at y = 0
        probe.box.add(vec3(60, 60, 60));
        r.setLightProbes({probe});

        cam_.target = vec3(0, 0.1f, 0);
        cam_.distance = 1.9f;
        cam_.pitch = 0.3f;
        cam_.yaw = 0.6f;
        cam_.fovY = 40.0f * DEG;
        int view = std::atoi(ctx.argValue("--view", "0").c_str());
        if (view == 1) { cam_.target = vec3(-0.2f, 0.12f, 0.0f); cam_.distance = 2.2f; cam_.pitch = 0.08f; cam_.yaw = 1.2f; }
        if (view == 2) { cam_.target = vec3(0.0f, 0.04f, 0.35f); cam_.distance = 0.55f; cam_.pitch = 0.35f; cam_.yaw = 0.3f; }
        float az = 25.0f, el = 35.0f;
        ev_ = 14.7f;
        if (ctx.hasArg("--dusk")) { el = -4.0f; ev_ = 8.5f; dusk_ = true; }
        std::string sun = ctx.argValue("--sun");
        if (!sun.empty()) std::sscanf(sun.c_str(), "%f,%f", &az, &el);
        std::string ev = ctx.argValue("--ev");
        if (!ev.empty()) ev_ = float(std::atof(ev.c_str()));
        sunDir_ = normalize(vec3(-std::cos(el * DEG) * std::cos(az * DEG), std::sin(el * DEG), std::cos(el * DEG) * std::sin(az * DEG)));
        return true;
    }
    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        cam_.update(plat::input());
        return !plat::input().keyPressed[plat::KEY_ESCAPE];
    }
    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env;
        env.time = time_;
        env.sunDirection = sunDir_;
        env.exposureEV100 = ev_;
        r.beginFrame(cam_.camera(), env, dt);
        render::DrawItem d;
        d.mesh = &floor_;
        d.material = &floorMat_;
        d.flags = render::DRAW_STATIC | render::DRAW_CAST_SHADOW;
        d.objectId = 1;
        r.submit(d);
        for (int i = 0; i < 7; ++i) {
            render::DrawItem s;
            s.mesh = &sphere_;
            s.material = &mats_[i];
            s.model = translate(vec3(-0.9f + 0.3f * float(i), 0.12f, 0.0f));
            s.objectId = uint32_t(i + 2);
            r.submit(s);
        }
        render::DrawItem b;
        b.mesh = &box_;
        b.material = &porcelain_;
        b.model = translate(vec3(0.1f, 0.1f, -0.45f)) * rotateY(time_ * 0.3f);
        b.objectId = 20;
        r.submit(b);
        render::DrawItem c;
        c.mesh = &panel_;
        c.material = &mats_[7];
        c.model = translate(vec3(0.75f, 0.2f, -0.5f)) * rotateY(0.5f);
        c.objectId = 21;
        r.submit(c);
        for (int i = 0; i < 8; ++i) {
            render::DrawItem p;
            p.mesh = &pawn_;
            p.material = &mats_[i & 1];
            p.model = translate(vec3(-0.25f + 0.07f * float(i), 0.0f, 0.3f)) * scale(vec3(1.3f));
            p.objectId = uint32_t(10 + i);
            r.submit(p);
        }
        // A warm point light ("candle") and a spot light; they only matter at dusk.
        render::PointLight candle;
        candle.position = vec3(-0.45f, 0.32f, 0.22f);
        candle.color = vec3(1.0f, 0.62f, 0.3f);   // a 60 cd (~750 lm) warm lamp at dusk
        candle.intensity = dusk_ ? 60.0f : 4.0f;
        candle.radius = 3.0f;
        candle.sourceRadius = 0.012f;
        r.addLight(candle);
        render::PointLight spot;
        spot.position = vec3(1.4f, 1.6f, 1.2f);
        spot.color = vec3(0.75f, 0.85f, 1.0f);
        spot.intensity = dusk_ ? 8000.0f : 100.0f;
        spot.radius = 6.0f;
        spot.sourceRadius = 0.03f;
        spot.setSpot(vec3(-1.4f, -1.5f, -1.2f), 10.0f * DEG, 22.0f * DEG);
        r.addLight(spot);
        r.endFrame();
    }

private:
    Mesh floor_, sphere_, box_, pawn_, panel_;
    Material floorMat_, mats_[8], porcelain_;
    OrbitCamera cam_;
    vec3 sunDir_;
    float ev_ = 14.7f;
    bool dusk_ = false;
    float time_ = 0;
};

SCACELITH_SCENE("testbed", "Material / lighting validation scene (sky, PCSS, probes, planar floor)", TestbedScene);
