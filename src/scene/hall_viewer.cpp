// "hall" viewer scene: the royal hall with the chess table and the two chairs, default renderer
// lighting, planar reflections on the marble floor.
//
//   scacelith --scene hall [--view player|wide|windows|ceiling|table|orbit]
//             [--cam x,y,z --target x,y,z] [--fov deg] [--sun x,y,z] [--ev EV100]
#include "furniture.h"
#include "hall.h"
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../core/log.h"
#include <chrono>
#include <cstdio>

using namespace m;

namespace {

bool parseVec3(const std::string& s, vec3& out) {
    float x, y, z;
    if (std::sscanf(s.c_str(), "%f,%f,%f", &x, &y, &z) != 3) return false;
    out = vec3(x, y, z);
    return true;
}

class HallViewerScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        static bool matsReady = false;
        if (!matsReady) matsReady = materials::init();
        auto t0 = std::chrono::steady_clock::now();
        hall::BuildStats hs;
        Model hallModel = hall::buildHall(&hs);
        Model table = furniture::buildTable();
        Model chair = furniture::buildChair();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t tt = 0, tc = 0;
        for (auto& p : table.parts) tt += p.mesh.indices.size() / 3;
        for (auto& p : chair.parts) tc += p.mesh.indices.size() / 3;
        LOGI("hall viewer: hall %u tris (%.3f s), table %u tris, chair %u tris, total generation %.3f s", unsigned(hs.triangles), hs.seconds,
             unsigned(tt), unsigned(tc), secs);
        hall_.upload(hallModel);
        table_.upload(table);
        chair_.upload(chair);

        // Floor planar reflection: copy of the library material pointing at our reflector.
        render::PlanarReflector pr;
        pr.point = vec3(0, 0, 0);
        pr.normal = vec3(0, 1, 0);
        int idx = ctx.renderer->addPlanarReflector(pr);
        floorMat_ = materials::get(MaterialId::FloorMarble);
        floorMat_.planarReflector = idx;

        std::string sun = ctx.argValue("--sun");
        sunDir_ = hall::recommendedSunDirection();
        if (!sun.empty()) {
            vec3 s;
            if (parseVec3(sun, s)) sunDir_ = normalize(s);
        }
        noGlass_ = ctx.hasArg("--noglass");  // debug: hide the (placeholder, opaque) window glass
        skip_ = ctx.argValue("--skip");       // debug: hide parts whose name contains this text
        std::string ev = ctx.argValue("--ev");
        if (!ev.empty()) ev_ = float(std::atof(ev.c_str()));

        view_ = ctx.argValue("--view", "orbit");
        fov_ = 55.0f;
        if (view_ == "player") {
            eye_ = vec3(0, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z);
            // Whole board in the lower part of the frame (its near edge is ~52 deg below the eye),
            // the far wall and the window wall (left) above it.
            target_ = vec3(-0.1f, 0.84f, -0.45f);
            fov_ = 70.0f;
        } else if (view_ == "wide") {
            eye_ = vec3(5.6f, 3.1f, 10.2f);
            target_ = vec3(-1.8f, 2.4f, -3.5f);
            fov_ = 64.0f;
        } else if (view_ == "windows") {
            eye_ = vec3(5.0f, 1.7f, 5.0f);
            target_ = vec3(-7.0f, 3.6f, -1.5f);
            fov_ = 62.0f;
        } else if (view_ == "ceiling") {
            eye_ = vec3(3.5f, 1.6f, 6.5f);
            target_ = vec3(0.0f, 6.5f, -1.0f);
            fov_ = 70.0f;
        } else if (view_ == "table") {
            eye_ = vec3(1.9f, 1.25f, 1.6f);
            target_ = vec3(0.0f, 0.5f, 0.0f);
            fov_ = 45.0f;
        } else {
            view_ = "orbit";
        }
        vec3 c, t;
        if (parseVec3(ctx.argValue("--cam"), c)) { eye_ = c; if (view_ == "orbit") view_ = "custom"; }
        if (parseVec3(ctx.argValue("--target"), t)) target_ = t;
        std::string fov = ctx.argValue("--fov");
        if (!fov.empty()) fov_ = float(std::atof(fov.c_str()));
        orbit_.target = vec3(0, 1.5f, 0);
        orbit_.distance = 9.0f;
        orbit_.pitch = 0.25f;
        orbit_.yaw = 0.7f;
        orbit_.fovY = 55.0f * DEG;
        return true;
    }
    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        const plat::Input& in = plat::input();
        if (in.mouseDown[plat::MOUSE_RIGHT] || in.mouseDown[plat::MOUSE_MIDDLE] || in.wheel != 0.0f) {
            if (view_ != "orbit") {
                // Switch to orbit around the current look target.
                orbit_.target = target_;
                vec3 d = eye_ - target_;
                orbit_.distance = length(d);
                orbit_.pitch = std::asin(clamp(d.y / orbit_.distance, -1.0f, 1.0f));
                orbit_.yaw = std::atan2(d.x, d.z);
                orbit_.fovY = fov_ * DEG;
                view_ = "orbit";
            }
        }
        if (view_ == "orbit") orbit_.update(in);
        return !in.keyPressed[plat::KEY_ESCAPE];
    }
    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Camera cam;
        if (view_ == "orbit") {
            cam = orbit_.camera();
        } else {
            cam.position = eye_;
            cam.fovY = fov_ * DEG;
            cam.nearZ = 0.03f;
            cam.lookAt(target_);
        }
        render::Environment env;
        env.time = time_;
        env.sunDirection = sunDir_;
        env.exposureEV100 = ev_;
        r.beginFrame(cam, env, dt);
        uint32_t id = 1;
        auto submitModel = [&](const GpuModel& gm, const mat4& xf) {
            for (const GpuModelPart& p : gm.parts) {
                if (noGlass_ && p.material == MaterialId::WindowGlass) continue;
                if (!skip_.empty() && p.mesh.name.find(skip_) != std::string::npos) continue;
                render::DrawItem d;
                d.mesh = &p.mesh;
                d.material = p.material == MaterialId::FloorMarble ? &floorMat_ : &materials::get(p.material);
                d.model = xf;
                d.flags = p.flags;
                for (int k = 0; k < 4; ++k) d.inst[k] = p.inst[k];
                d.objectId = id++;
                r.submit(d);
            }
        };
        submitModel(hall_, mat4());
        submitModel(table_, mat4());
        submitModel(chair_, translate(vec3(0, 0, layout::CHAIR_Z)) * rotateY(PI));  // White
        submitModel(chair_, translate(vec3(0, 0, -layout::CHAIR_Z)));              // Black
        r.endFrame();
    }
    void shutdown(AppContext&) override {
        hall_.destroy();
        table_.destroy();
        chair_.destroy();
    }

private:
    GpuModel hall_, table_, chair_;
    Material floorMat_;
    OrbitCamera orbit_;
    std::string view_;
    vec3 eye_{0, 1.6f, 5.0f}, target_{0, 1.2f, 0};
    float fov_ = 55.0f;
    vec3 sunDir_;
    float ev_ = 11.5f;
    bool noGlass_ = false;
    std::string skip_;
    float time_ = 0.0f;
};

}  // namespace

SCACELITH_SCENE("hall", "Royal hall architecture with the chess table and chairs (--view player|wide|windows|ceiling|table)", HallViewerScene);
