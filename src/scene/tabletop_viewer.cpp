// "tabletop" viewer scene: marble board with the 32 Staunton pieces in the initial position,
// the lever chess clock beside it and a plain table top. Orbit camera (right-drag / wheel).
//
// Options:
//   --view overview|player|knight|clock|pieces|board   camera preset (default overview)
//   --yaw Y --pitch P --dist D --fov F                   override the orbit camera (radians / m / deg)
//   --target x,y,z                                       orbit target
//   --lever 0|1  --running 0|1  --ms0 N --ms1 N --flags F   clock state shown on the LCDs
#include "board.h"
#include "clock_model.h"
#include "pieces.h"
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../core/log.h"
#include "../game/layout.h"
#include "../scene/model.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace m;

namespace {

class TabletopScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        materials::init();
        setupClockMaterials();
        auto t0 = std::chrono::steady_clock::now();
        for (int t = 1; t <= 6; ++t) {
            PieceMeshes pm = buildPiece(t);
            body_[t].upload(pm.body, "piece");
            felt_[t].upload(pm.felt, "felt");
            tris_ += pm.body.indices.size() / 3;
        }
        auto t1 = std::chrono::steady_clock::now();
        Model board = buildBoard();
        board_.upload(board);
        auto t2 = std::chrono::steady_clock::now();
        clock_ = buildClock();
        clockBody_.upload(clock_.body);
        clockLever_.upload(clock_.lever);
        auto t3 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        LOGI("tabletop: pieces %.0f ms (%d tris for the 6 types), board %.0f ms, clock %.0f ms", ms(t0, t1), int(tris_), ms(t1, t2), ms(t2, t3));

        table_.upload(prim::plane(layout::TABLE_WIDTH, layout::TABLE_DEPTH, 1, 1, 1.0f), "table");
        tableMat_ = materials::get(MaterialId::TableWood);
        if (tableMat_.planarReflector < 0) {
            render::PlanarReflector pr;
            pr.point = vec3(0, layout::TABLE_TOP_Y, 0);
            tableMat_.planarReflector = ctx.renderer->addPlanarReflector(pr);
        }

        view_ = ctx.argValue("--view", "overview");
        leverSide_ = std::atoi(ctx.argValue("--lever", "1").c_str());
        running_ = std::atoi(ctx.argValue("--running", "0").c_str());
        ms_[0] = float(std::atof(ctx.argValue("--ms0", "299400").c_str()));
        ms_[1] = float(std::atof(ctx.argValue("--ms1", "300000").c_str()));
        flags_ = float(std::atof(ctx.argValue("--flags", "0").c_str()));
        setupCamera(ctx);
        return true;
    }

    void setupCamera(AppContext& ctx) {
        const vec3 clockPos(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y, layout::CLOCK_Z);
        cam_.target = vec3(0, layout::BOARD_TOP_Y + 0.02f, 0);
        cam_.distance = 0.95f;
        cam_.pitch = 0.62f;
        cam_.yaw = 0.55f;
        cam_.fovY = 40.0f * DEG;
        if (view_ == "knight") {
            // White's b1 knight, seen from the front-right over the pawn rank.
            cam_.target = layout::squareCenter(1, 0) + vec3(0, 0.04f, 0);
            cam_.distance = 0.2f;
            cam_.pitch = 0.45f;
            cam_.yaw = 2.6f;
            cam_.fovY = 40.0f * DEG;
        } else if (view_ == "clock") {
            cam_.target = clockPos + vec3(0, 0.035f, 0);
            cam_.distance = 0.42f;
            cam_.pitch = 0.32f;
            cam_.yaw = -1.5708f - 0.35f;
            cam_.fovY = 35.0f * DEG;
        } else if (view_ == "pieces") {
            cam_.target = vec3(0, layout::BOARD_TOP_Y + 0.04f, 0.1925f + 0.055f * 0.5f);
            cam_.distance = 0.36f;
            cam_.pitch = 0.08f;
            cam_.yaw = 0.0f;
            cam_.fovY = 40.0f * DEG;
        } else if (view_ == "board") {
            cam_.target = vec3(-0.19f, layout::BOARD_TOP_Y, 0.19f);
            cam_.distance = 0.25f;
            cam_.pitch = 0.5f;
            cam_.yaw = -0.6f;
        }
        std::string s;
        if (!(s = ctx.argValue("--yaw")).empty()) cam_.yaw = float(std::atof(s.c_str()));
        if (!(s = ctx.argValue("--pitch")).empty()) cam_.pitch = float(std::atof(s.c_str()));
        if (!(s = ctx.argValue("--dist")).empty()) cam_.distance = float(std::atof(s.c_str()));
        if (!(s = ctx.argValue("--fov")).empty()) cam_.fovY = float(std::atof(s.c_str())) * DEG;
        if (!(s = ctx.argValue("--target")).empty()) {
            vec3 t;
            if (std::sscanf(s.c_str(), "%f,%f,%f", &t.x, &t.y, &t.z) == 3) cam_.target = t;
        }
    }

    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        cam_.update(plat::input());
        const plat::Input& in = plat::input();
        if (in.keyPressed[plat::KEY_SPACE]) { leverSide_ ^= 1; running_ = leverSide_ ^ 1; }
        // --running / --lever are not range-checked: only sides 0 and 1 have a time to count down.
        if (!ctx.screenshotMode && running_ >= 0 && running_ < 2) ms_[running_] = std::max(0.0f, ms_[running_] - dt * 1000.0f);
        return !in.keyPressed[plat::KEY_ESCAPE];
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env;
        env.time = time_;
        render::Camera cam = cam_.camera();
        if (view_ == "player") {
            cam.position = vec3(0, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z);
            cam.fovY = 50.0f * DEG;
            cam.nearZ = 0.02f;
            cam.lookAt(vec3(0, layout::BOARD_TOP_Y, 0));
        }
        r.beginFrame(cam, env, dt);

        render::DrawItem t;
        t.mesh = &table_;
        t.material = &tableMat_;
        t.model = translate(vec3(0, layout::TABLE_TOP_Y, 0));
        t.flags = render::DRAW_STATIC;
        r.submit(t);

        uint32_t id = 100;
        for (auto& p : board_.parts) {
            render::DrawItem d;
            d.mesh = &p.mesh;
            d.material = &materials::get(p.material);
            d.flags = p.flags | render::DRAW_STATIC;
            for (int k = 0; k < 4; ++k) d.inst[k] = p.inst[k];
            d.objectId = id++;
            r.submit(d);
        }

        if (view_ == "pieces") {
            // Two rows of the six piece types for silhouette review.
            for (int c = 0; c < 2; ++c)
                for (int type = 1; type <= 6; ++type) {
                    vec3 pos(-0.1375f + 0.055f * float(type - 1), layout::BOARD_TOP_Y, c == 0 ? 0.1925f : 0.1375f);
                    submitPiece(r, type, c == 0, translate(pos) * rotateY(PI * 0.5f), id++);
                }
        } else {
            static const int backRank[8] = {4, 2, 3, 5, 6, 3, 2, 4};
            for (int f = 0; f < 8; ++f) {
                submitPiece(r, backRank[f], true, translate(layout::squareCenter(f, 0)) * rotateY(PI), id++);
                submitPiece(r, 1, true, translate(layout::squareCenter(f, 1)) * rotateY(PI), id++);
                submitPiece(r, 1, false, translate(layout::squareCenter(f, 6)), id++);
                submitPiece(r, backRank[f], false, translate(layout::squareCenter(f, 7)), id++);
            }
        }

        // Clock: displays face -X (the board), index 0 = local -Z = Black's side here.
        mat4 cm = translate(vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y, layout::CLOCK_Z));
        vec4 lcd(ms_[0], ms_[1], flags_, float(running_));
        for (auto& p : clockBody_.parts) {
            render::DrawItem d;
            d.mesh = &p.mesh;
            d.material = &materials::get(p.material);
            d.model = cm;
            d.flags = p.flags;
            for (int k = 0; k < 4; ++k) d.inst[k] = p.inst[k];
            if (p.material == MaterialId::ClockDisplay) d.inst[0] = lcd;
            d.objectId = id++;
            r.submit(d);
        }
        float angle = leverSide_ == 1 ? clock_.leverMaxAngle : -clock_.leverMaxAngle;
        mat4 lm = cm * translate(clock_.leverPivot) * rotateAxis(clock_.leverAxis, angle) * translate(-clock_.leverPivot);
        for (auto& p : clockLever_.parts) {
            render::DrawItem d;
            d.mesh = &p.mesh;
            d.material = &materials::get(p.material);
            d.model = lm;
            d.flags = p.flags;
            d.objectId = id++;
            r.submit(d);
        }
        r.endFrame();
    }

    void submitPiece(render::Renderer& r, int type, bool white, const mat4& model, uint32_t id) {
        render::DrawItem d;
        d.mesh = &body_[type];
        d.material = &materials::get(white ? MaterialId::MarbleWhitePiece : MaterialId::MarbleBlackPiece);
        d.model = model;
        d.inst[0] = vec4(float(hash32(id * 2654435761u) & 0xFFFF) / 65536.0f, 0, 0, 0);
        d.objectId = id;
        r.submit(d);
        render::DrawItem f = d;
        f.mesh = &felt_[type];
        f.material = &materials::get(MaterialId::PieceFelt);
        r.submit(f);
    }

    void shutdown(AppContext&) override {
        for (auto& m : body_) m.destroy();
        for (auto& m : felt_) m.destroy();
        board_.destroy();
        clockBody_.destroy();
        clockLever_.destroy();
        table_.destroy();
    }

private:
    Mesh body_[7], felt_[7], table_;
    GpuModel board_, clockBody_, clockLever_;
    ClockModel clock_;
    Material tableMat_;
    OrbitCamera cam_;
    std::string view_;
    float time_ = 0;
    size_t tris_ = 0;
    int leverSide_ = 1, running_ = 0;
    float ms_[2] = {299400, 300000};
    float flags_ = 0;
};

}  // namespace

SCACELITH_SCENE("tabletop", "Chess set, marble board and lever clock on a table (--view overview|player|knight|clock|pieces|board)",
                TabletopScene);
