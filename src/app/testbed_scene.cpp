// "testbed": minimal lit scene used to validate the pipeline (sun, shadows, planar reflection,
// materials). Not part of the game.
#include "orbit_camera.h"
#include "scene.h"
#include "../render/mesh.h"

using namespace m;

class TestbedScene : public Scene {
public:
    bool init(AppContext&) override {
        floor_.upload(prim::plane(6, 6, 1, 1, 1.0f), "floor");
        sphere_.upload(prim::sphere(0.12f), "sphere");
        box_.upload(prim::roundedBox(vec3(0.15f, 0.1f, 0.15f), 0.01f, 3), "box");
        MeshData lathe = prim::lathe({{0, 0}, {0.022f, 0}, {0.022f, 0}, {0.022f, 0.006f}, {0.016f, 0.012f}, {0.009f, 0.03f},
                                      {0.007f, 0.05f}, {0.012f, 0.055f}, {0.012f, 0.058f}, {0.0f, 0.075f}});
        pawn_.upload(lathe, "pawn");
        floorMat_.params[0] = vec4(0.8f, 0.78f, 0.74f, 0.15f);
        floorMat_.params[1] = vec4(0, 0.5f, 1.0f, 0.03f);
        floorMat_.planarReflector = 0;
        for (int i = 0; i < 4; ++i) {
            mats_[i].params[0] = vec4(i == 0 ? vec3(0.9f) : i == 1 ? vec3(0.03f) : i == 2 ? vec3(0.95f, 0.64f, 0.54f) : vec3(0.1f, 0.2f, 0.6f),
                                      i == 3 ? 0.6f : 0.25f);
            mats_[i].params[1] = vec4(i == 2 ? 1.0f : 0.0f, 0.5f, i < 2 ? 1.0f : 0.0f, 0.05f);
            mats_[i].params[3] = i == 3 ? vec4(0.4f, 0.4f, 0.6f, 0.4f) : vec4(0);
            mats_[i].tessellated = true;
        }
        render::PlanarReflector pr;
        pr.point = vec3(0, 0, 0);
        render::renderer().addPlanarReflector(pr);
        cam_.target = vec3(0, 0.1f, 0);
        cam_.distance = 1.4f;
        cam_.pitch = 0.3f;
        cam_.yaw = 0.6f;
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
        r.beginFrame(cam_.camera(), env, dt);
        render::DrawItem d;
        d.mesh = &floor_;
        d.material = &floorMat_;
        d.flags = render::DRAW_STATIC;
        r.submit(d);
        for (int i = 0; i < 4; ++i) {
            render::DrawItem s;
            s.mesh = &sphere_;
            s.material = &mats_[i];
            s.model = translate(vec3(-0.45f + 0.3f * float(i), 0.12f, 0.0f));
            s.objectId = uint32_t(i + 1);
            r.submit(s);
        }
        render::DrawItem b;
        b.mesh = &box_;
        b.material = &mats_[0];
        b.model = translate(vec3(0.1f, 0.1f, -0.4f)) * rotateY(time_ * 0.3f);
        r.submit(b);
        for (int i = 0; i < 8; ++i) {
            render::DrawItem p;
            p.mesh = &pawn_;
            p.material = &mats_[i & 1];
            p.model = translate(vec3(-0.25f + 0.07f * float(i), 0.0f, 0.3f)) * scale(vec3(1.3f));
            p.objectId = uint32_t(10 + i);
            r.submit(p);
        }
        r.endFrame();
    }

private:
    Mesh floor_, sphere_, box_, pawn_;
    Material floorMat_, mats_[4];
    OrbitCamera cam_;
    float time_ = 0;
};

SCACELITH_SCENE("testbed", "Pipeline validation scene (spheres, pawns, planar floor)", TestbedScene);
