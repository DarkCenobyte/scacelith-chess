// "lightbox": lighting validation interior built from boxes, matching game/layout.h: a
// 14 x 22 x 9 m hall with 0.9 m thick walls, three tall round-arched windows in the -X wall
// (glass + mullions), a tiled polished stone floor (planar reflector), tapestries, a glossy table
// (planar reflector) with a marble board (planar reflector), pawns / kings / spheres and two box
// "players" (one hand hovering above the board). Checks: sun beams on the floor, PCSS contact
// hardening, bounce light on the ceiling and shadowed walls, floor reflections of the windows.
//
//   --view N   0 overview towards the windows, 1 player view of the board, 2 ceiling / bounce,
//              3 along the window wall, 4 low grazing view of the floor reflections
//   --sun az,el  sun azimuth (deg, 0 = from -X, positive towards +Z) and elevation (deg)
#include "orbit_camera.h"
#include "scene.h"
#include "../game/layout.h"
#include "../render/mesh.h"
#include <cstdio>

using namespace m;

namespace {
void addBox(MeshData& md, vec3 lo, vec3 hi) {
    MeshData b = prim::box((hi - lo) * 0.5f);
    md.append(b, translate((lo + hi) * 0.5f));
}

Material stone(vec3 albedo, float rough, float cc, float ccRough) {
    Material m;
    m.surface = "shaders/lighting/test_stone.glsl";
    m.params[0] = vec4(albedo, rough);
    m.params[1] = vec4(0, 0.5f, cc, ccRough);
    m.params[3] = vec4(1, 1, 1, 0);
    m.params[4] = vec4(0, 0, 0.01f, 0);
    return m;
}
}  // namespace

class LightboxScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        using namespace layout;
        const float T = WALL_THICKNESS;
        // ---- Hall shell (static) ------------------------------------------------------------
        MeshData walls, ceiling, floorMd, mullions;
        addBox(floorMd, vec3(HALL_MIN_X, -0.2f, HALL_MIN_Z), vec3(HALL_MAX_X, 0.0f, HALL_MAX_Z));
        addBox(ceiling, vec3(HALL_MIN_X - T, HALL_HEIGHT, HALL_MIN_Z - T), vec3(HALL_MAX_X + T, HALL_HEIGHT + 0.4f, HALL_MAX_Z + T));
        addBox(walls, vec3(HALL_MAX_X, 0, HALL_MIN_Z - T), vec3(HALL_MAX_X + T, HALL_HEIGHT, HALL_MAX_Z + T));  // +X
        addBox(walls, vec3(HALL_MIN_X - T, 0, HALL_MIN_Z - T), vec3(HALL_MAX_X + T, HALL_HEIGHT, HALL_MIN_Z));  // -Z
        addBox(walls, vec3(HALL_MIN_X - T, 0, HALL_MAX_Z), vec3(HALL_MAX_X + T, HALL_HEIGHT, HALL_MAX_Z + T));  // +Z
        // -X wall with three round-arched openings (arch = stepped boxes).
        const float x0 = HALL_MIN_X - T, x1 = HALL_MIN_X, R = WINDOW_WIDTH * 0.5f;
        const float archTop = WINDOW_TOP_Y + R;
        addBox(walls, vec3(x0, 0, HALL_MIN_Z), vec3(x1, WINDOW_SILL_Y, HALL_MAX_Z));
        addBox(walls, vec3(x0, archTop, HALL_MIN_Z), vec3(x1, HALL_HEIGHT, HALL_MAX_Z));
        float zc[3] = {-WINDOW_SPACING, 0.0f, WINDOW_SPACING};
        float zPrev = HALL_MIN_Z;
        for (int w = 0; w < 3; ++w) {
            addBox(walls, vec3(x0, WINDOW_SILL_Y, zPrev), vec3(x1, archTop, zc[w] - R));
            zPrev = zc[w] + R;
            const int steps = 16;
            for (int s = 0; s < steps; ++s) {
                float ya = WINDOW_TOP_Y + R * float(s) / steps, yb = WINDOW_TOP_Y + R * float(s + 1) / steps;
                float hy = (ya + yb) * 0.5f - WINDOW_TOP_Y;
                float half = std::sqrt(std::max(R * R - hy * hy, 0.0f));
                addBox(walls, vec3(x0, ya, zc[w] - R), vec3(x1, yb, zc[w] - half));
                addBox(walls, vec3(x0, ya, zc[w] + half), vec3(x1, yb, zc[w] + R));
            }
            // Iron mullions: one vertical bar and horizontal transoms (they draw the classic
            // grid of sun patches on the floor).
            float xm = HALL_MIN_X - T * 0.5f;
            addBox(mullions, vec3(xm - 0.03f, WINDOW_SILL_Y, zc[w] - 0.025f), vec3(xm + 0.03f, WINDOW_TOP_Y, zc[w] + 0.025f));
            for (int k = 1; k <= 4; ++k) {
                float y = WINDOW_SILL_Y + (WINDOW_TOP_Y - WINDOW_SILL_Y) * float(k) / 5.0f;
                addBox(mullions, vec3(xm - 0.03f, y - 0.02f, zc[w] - R), vec3(xm + 0.03f, y + 0.02f, zc[w] + R));
            }
            addBox(mullions, vec3(xm - 0.03f, WINDOW_TOP_Y - 0.03f, zc[w] - R), vec3(xm + 0.03f, WINDOW_TOP_Y + 0.03f, zc[w] + R));
            MeshData pane;
            addBox(pane, vec3(xm - 0.004f, WINDOW_SILL_Y, zc[w] - R), vec3(xm + 0.004f, WINDOW_TOP_Y, zc[w] + R));
            glass_[w].upload(pane, "lightbox.glass");
        }
        addBox(walls, vec3(x0, WINDOW_SILL_Y, zPrev), vec3(x1, archTop, HALL_MAX_Z));
        walls_.upload(walls, "lightbox.walls");
        ceiling_.upload(ceiling, "lightbox.ceiling");
        floor_.upload(floorMd, "lightbox.floor");
        mullions_.upload(mullions, "lightbox.mullions");

        // Tapestries (royal blue / red) on the +X and end walls.
        MeshData tapBlue, tapRed;
        for (int k = 0; k < 3; ++k) {
            MeshData& t = (k == 1) ? tapRed : tapBlue;
            addBox(t, vec3(HALL_MAX_X - 0.04f, 1.6f, zc[k] - 1.3f), vec3(HALL_MAX_X, 6.6f, zc[k] + 1.3f));
        }
        addBox(tapRed, vec3(-2.0f, 1.8f, HALL_MIN_Z), vec3(2.0f, 6.8f, HALL_MIN_Z + 0.04f));
        addBox(tapBlue, vec3(-2.0f, 1.8f, HALL_MAX_Z - 0.04f), vec3(2.0f, 6.8f, HALL_MAX_Z));
        tapBlue_.upload(tapBlue, "lightbox.tapestry_blue");
        tapRed_.upload(tapRed, "lightbox.tapestry_red");

        // ---- Table, chairs, board (static) --------------------------------------------------
        MeshData table, legs, chairs, frame, lightSq, darkSq;
        addBox(table, vec3(-TABLE_WIDTH * 0.5f, TABLE_TOP_Y - TABLE_TOP_THICKNESS, -TABLE_DEPTH * 0.5f),
               vec3(TABLE_WIDTH * 0.5f, TABLE_TOP_Y, TABLE_DEPTH * 0.5f));
        for (int k = 0; k < 4; ++k) {
            float sx = (k & 1) ? 1.0f : -1.0f, sz = (k & 2) ? 1.0f : -1.0f;
            vec3 c(sx * (TABLE_WIDTH * 0.5f - 0.08f), 0, sz * (TABLE_DEPTH * 0.5f - 0.08f));
            addBox(legs, c - vec3(0.035f, 0, 0.035f), c + vec3(0.035f, TABLE_TOP_Y - TABLE_TOP_THICKNESS, 0.035f));
        }
        for (int s = -1; s <= 1; s += 2) {
            float z = float(s) * CHAIR_Z;
            addBox(chairs, vec3(-0.24f, SEAT_HEIGHT - 0.05f, z - 0.22f), vec3(0.24f, SEAT_HEIGHT, z + 0.22f));
            addBox(chairs, vec3(-0.24f, SEAT_HEIGHT, z + float(s) * 0.2f - 0.025f), vec3(0.24f, SEAT_HEIGHT + 0.6f, z + float(s) * 0.2f + 0.025f));
            for (int k = 0; k < 4; ++k) {
                float lx = (k & 1) ? 0.21f : -0.21f, lz = z + ((k & 2) ? 0.19f : -0.19f);
                addBox(chairs, vec3(lx - 0.02f, 0, lz - 0.02f), vec3(lx + 0.02f, SEAT_HEIGHT - 0.05f, lz + 0.02f));
            }
        }
        const float hb = BOARD_SIZE * 0.5f;
        addBox(frame, vec3(-hb, TABLE_TOP_Y, -hb), vec3(hb, BOARD_TOP_Y - 0.0004f, hb));
        for (int r = 0; r < 8; ++r)
            for (int f = 0; f < 8; ++f) {
                vec3 c = squareCenter(f, r);
                MeshData& md = ((f + r) & 1) ? lightSq : darkSq;
                addBox(md, vec3(c.x - SQUARE_SIZE * 0.5f, BOARD_TOP_Y - 0.001f, c.z - SQUARE_SIZE * 0.5f),
                       vec3(c.x + SQUARE_SIZE * 0.5f, BOARD_TOP_Y, c.z + SQUARE_SIZE * 0.5f));
            }
        table_.upload(table, "lightbox.table");
        legs_.upload(legs, "lightbox.legs");
        chairs_.upload(chairs, "lightbox.chairs");
        boardFrame_.upload(frame, "lightbox.board_frame");
        lightSq_.upload(lightSq, "lightbox.squares_light");
        darkSq_.upload(darkSq, "lightbox.squares_dark");

        // ---- Pieces and props (dynamic) -----------------------------------------------------
        pawn_.upload(prim::lathe({{0, 0}, {0.0145f, 0}, {0.0145f, 0}, {0.0145f, 0.004f}, {0.011f, 0.008f}, {0.006f, 0.02f},
                                  {0.0047f, 0.033f}, {0.008f, 0.0365f}, {0.008f, 0.0385f}, {0.0068f, 0.04f}, {0.0075f, 0.045f},
                                  {0.0045f, 0.049f}, {0.0f, 0.050f}}),
                     "lightbox.pawn");
        king_.upload(prim::lathe({{0, 0}, {0.020f, 0}, {0.020f, 0}, {0.020f, 0.007f}, {0.016f, 0.013f}, {0.010f, 0.03f},
                                  {0.0075f, 0.06f}, {0.0125f, 0.066f}, {0.0125f, 0.07f}, {0.009f, 0.073f}, {0.012f, 0.082f},
                                  {0.004f, 0.086f}, {0.004f, 0.086f}, {0.0035f, 0.095f}, {0.0f, 0.095f}}),
                     "lightbox.king");
        ball_.upload(prim::sphere(1.0f, 48, 24), "lightbox.ball");
        MeshData torso, arm;
        addBox(torso, vec3(-0.22f, -0.3f, -0.13f), vec3(0.22f, 0.3f, 0.13f));
        torso_.upload(torso, "lightbox.torso");
        addBox(arm, vec3(-0.03f, -0.03f, -0.25f), vec3(0.03f, 0.03f, 0.25f));
        arm_.upload(arm, "lightbox.arm");

        // ---- Materials ----------------------------------------------------------------------
        floorMat_ = stone(vec3(0.84f, 0.80f, 0.72f), 0.22f, 1.0f, 0.035f);
        floorMat_.params[2] = vec4(0.55f, 0.52f, 0.48f, 0.9f);
        floorMat_.params[4] = vec4(1.2f, 1.1f, 0.01f, 0.35f);
        floorMat_.params[3] = vec4(1.0f, 0.9f, 0.8f, 0.2f);
        floorMat_.planarReflector = 0;
        wallMat_ = stone(vec3(0.74f, 0.70f, 0.62f), 0.85f, 0.0f, 0.1f);
        ceilMat_ = stone(vec3(0.80f, 0.77f, 0.70f), 0.9f, 0.0f, 0.1f);
        ironMat_ = stone(vec3(0.05f, 0.05f, 0.05f), 0.45f, 0.0f, 0.1f);
        ironMat_.params[1].x = 0.6f;
        glassMat_.surface = "shaders/lighting/test_glass.glsl";
        glassMat_.transparent = true;
        glassMat_.doubleSided = true;
        glassMat_.castShadow = false;
        glassMat_.params[0] = vec4(0.97f, 0.99f, 0.98f, 0.02f);
        glassMat_.params[1] = vec4(1.52f, 1.0f, 1.0f, 0);
        tapBlueMat_ = stone(vec3(0.035f, 0.055f, 0.32f), 0.9f, 0.0f, 0.1f);
        tapBlueMat_.params[5] = vec4(0.25f, 0.28f, 0.55f, 0.45f);
        tapRedMat_ = stone(vec3(0.38f, 0.025f, 0.035f), 0.9f, 0.0f, 0.1f);
        tapRedMat_.params[5] = vec4(0.55f, 0.22f, 0.22f, 0.45f);
        tableMat_ = stone(vec3(0.17f, 0.075f, 0.033f), 0.38f, 1.0f, 0.035f);
        tableMat_.planarReflector = 1;
        woodMat_ = stone(vec3(0.15f, 0.065f, 0.03f), 0.5f, 0.6f, 0.15f);
        frameMat_ = stone(vec3(0.10f, 0.045f, 0.025f), 0.35f, 1.0f, 0.04f);
        frameMat_.planarReflector = 2;
        lightSqMat_ = stone(vec3(0.86f, 0.84f, 0.79f), 0.25f, 1.0f, 0.03f);
        lightSqMat_.params[2] = vec4(0.5f, 0.5f, 0.5f, 0.8f);
        lightSqMat_.params[3] = vec4(1.0f, 0.88f, 0.75f, 0.5f);
        lightSqMat_.params[4] = vec4(0, 18.0f, 0.006f, 0);
        lightSqMat_.planarReflector = 2;
        darkSqMat_ = stone(vec3(0.028f, 0.027f, 0.026f), 0.25f, 1.0f, 0.03f);
        darkSqMat_.params[2] = vec4(0.55f, 0.52f, 0.45f, 0.6f);
        darkSqMat_.params[4] = vec4(0, 18.0f, 0.004f, 0);
        darkSqMat_.planarReflector = 2;
        whiteMarble_ = stone(vec3(0.88f, 0.86f, 0.81f), 0.3f, 1.0f, 0.04f);
        whiteMarble_.params[2] = vec4(0.55f, 0.55f, 0.56f, 0.7f);
        whiteMarble_.params[3] = vec4(1.0f, 0.86f, 0.72f, 0.75f);
        whiteMarble_.params[4] = vec4(0, 60.0f, 0.008f, 0);
        whiteMarble_.tessellated = true;
        blackMarble_ = stone(vec3(0.022f, 0.021f, 0.02f), 0.3f, 1.0f, 0.04f);
        blackMarble_.params[2] = vec4(0.7f, 0.66f, 0.55f, 0.5f);
        blackMarble_.params[4] = vec4(0, 60.0f, 0.004f, 0);
        blackMarble_.tessellated = true;
        porcelain_ = stone(vec3(0.82f, 0.82f, 0.80f), 0.35f, 1.0f, 0.06f);
        porcelain_.params[3] = vec4(1.0f, 0.95f, 0.9f, 0.3f);
        gold_ = stone(vec3(1.0f, 0.77f, 0.36f), 0.22f, 0.0f, 0.1f);
        gold_.params[1].x = 1.0f;

        // ---- Planar reflectors --------------------------------------------------------------
        render::Renderer& rr = *ctx.renderer;
        render::PlanarReflector pf;
        pf.point = vec3(0, 0, 0);
        pf.bounds.add(vec3(HALL_MIN_X, 0, HALL_MIN_Z));
        pf.bounds.add(vec3(HALL_MAX_X, 0, HALL_MAX_Z));
        rr.addPlanarReflector(pf);
        render::PlanarReflector pt;
        pt.point = vec3(0, TABLE_TOP_Y, 0);
        pt.bounds.add(vec3(-TABLE_WIDTH * 0.5f, TABLE_TOP_Y, -TABLE_DEPTH * 0.5f));
        pt.bounds.add(vec3(TABLE_WIDTH * 0.5f, TABLE_TOP_Y, TABLE_DEPTH * 0.5f));
        pt.minObjectSize = 0.0f;
        rr.addPlanarReflector(pt);
        render::PlanarReflector pb;
        pb.point = vec3(0, BOARD_TOP_Y, 0);
        pb.bounds.add(vec3(-hb, BOARD_TOP_Y, -hb));
        pb.bounds.add(vec3(hb, BOARD_TOP_Y, hb));
        pb.minObjectSize = 0.0f;
        rr.addPlanarReflector(pb);

        // ---- Camera / sun -------------------------------------------------------------------
        int view = std::atoi(ctx.argValue("--view", "0").c_str());
        vec3 eye, target;
        switch (view) {
            case 1: eye = vec3(0.42f, 1.08f, 0.50f); target = vec3(-0.02f, BOARD_TOP_Y, -0.06f); cam_.fovY = 42 * DEG; break;
            case 2: eye = vec3(4.5f, 1.6f, 3.0f); target = vec3(-1.0f, 8.0f, -2.5f); cam_.fovY = 70 * DEG; break;
            case 3: eye = vec3(-5.2f, 1.7f, 10.0f); target = vec3(-5.5f, 2.2f, -4.0f); cam_.fovY = 60 * DEG; break;
            case 4: eye = vec3(5.5f, 0.45f, 2.5f); target = vec3(-6.5f, 1.2f, -1.0f); cam_.fovY = 60 * DEG; break;
            default: eye = vec3(6.2f, 2.1f, 9.0f); target = vec3(-4.0f, 2.6f, -2.0f); cam_.fovY = 62 * DEG; break;
        }
        vec3 d = eye - target;
        cam_.target = target;
        cam_.distance = length(d);
        cam_.pitch = std::asin(d.y / cam_.distance);
        cam_.yaw = std::atan2(d.x, d.z);
        float az = 5.0f, el = 35.0f;
        std::string sun = ctx.argValue("--sun");
        if (!sun.empty()) std::sscanf(sun.c_str(), "%f,%f", &az, &el);
        sunDir_ = normalize(vec3(-std::cos(el * DEG) * std::cos(az * DEG), std::sin(el * DEG), std::cos(el * DEG) * std::sin(az * DEG)));
        ev_ = float(std::atof(ctx.argValue("--ev", "0").c_str()));
        return true;
    }

    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        cam_.update(plat::input());
        return !plat::input().keyPressed[plat::KEY_ESCAPE];
    }

    void render(AppContext& ctx, float dt) override {
        using namespace layout;
        render::Renderer& r = *ctx.renderer;
        render::Environment env;
        env.time = time_;
        env.sunDirection = sunDir_;
        if (ev_ != 0.0f) env.exposureEV100 = ev_;
        r.beginFrame(cam_.camera(), env, dt);
        auto put = [&](const Mesh& mesh, const Material& mat, const mat4& model, uint32_t flags, uint32_t id) {
            render::DrawItem d;
            d.mesh = &mesh;
            d.material = &mat;
            d.model = model;
            d.flags = flags;
            d.objectId = id;
            r.submit(d);
        };
        const uint32_t S = render::DRAW_STATIC | render::DRAW_CAST_SHADOW, D = render::DRAW_CAST_SHADOW;
        put(floor_, floorMat_, mat4(), S, 1);
        put(walls_, wallMat_, mat4(), S, 2);
        put(ceiling_, ceilMat_, mat4(), S, 3);
        put(mullions_, ironMat_, mat4(), S, 4);
        put(tapBlue_, tapBlueMat_, mat4(), S, 5);
        put(tapRed_, tapRedMat_, mat4(), S, 6);
        put(table_, tableMat_, mat4(), S, 7);
        put(legs_, woodMat_, mat4(), S, 8);
        put(chairs_, woodMat_, mat4(), S, 9);
        put(boardFrame_, frameMat_, mat4(), S, 10);
        put(lightSq_, lightSqMat_, mat4(), S, 11);
        put(darkSq_, darkSqMat_, mat4(), S, 12);
        for (int w = 0; w < 3; ++w) put(glass_[w], glassMat_, mat4(), S, uint32_t(13 + w));
        // Pieces: pawns on ranks 2 and 7, kings on e1 / e8, a few loose ones.
        uint32_t id = 100;
        for (int f = 0; f < 8; ++f) {
            put(pawn_, whiteMarble_, translate(squareCenter(f, 1)), D, id++);
            put(pawn_, blackMarble_, translate(squareCenter(f, 6)), D, id++);
        }
        put(king_, whiteMarble_, translate(squareCenter(4, 0)), D, id++);
        put(king_, blackMarble_, translate(squareCenter(4, 7)), D, id++);
        put(king_, whiteMarble_, translate(squareCenter(2, 3)), D, id++);
        put(pawn_, blackMarble_, translate(squareCenter(3, 4)), D, id++);
        put(pawn_, whiteMarble_, translate(squareCenter(5, 3)), D, id++);
        // Marble and gold balls on the table.
        put(ball_, whiteMarble_, translate(vec3(0.40f, TABLE_TOP_Y + 0.03f, 0.18f)) * scale(vec3(0.03f)), D, id++);
        put(ball_, blackMarble_, translate(vec3(0.46f, TABLE_TOP_Y + 0.025f, 0.08f)) * scale(vec3(0.025f)), D, id++);
        put(ball_, gold_, translate(vec3(-0.42f, TABLE_TOP_Y + 0.035f, -0.15f)) * scale(vec3(0.035f)), D, id++);
        // Players: porcelain torso + head, the black player's arm reaching over the board.
        for (int s = -1; s <= 1; s += 2) {
            float z = float(s) * (PLAYER_PELVIS_Z + 0.12f);
            put(torso_, porcelain_, translate(vec3(0, PLAYER_PELVIS_Y + 0.38f, z)), D, id++);
            put(ball_, porcelain_, translate(vec3(0, EYE_HEIGHT + 0.03f, z - float(s) * 0.06f)) * scale(vec3(0.11f)), D, id++);
        }
        put(arm_, porcelain_, translate(vec3(0.07f, BOARD_TOP_Y + 0.085f, -0.36f)), D, id++);
        put(ball_, porcelain_, translate(vec3(0.07f, BOARD_TOP_Y + 0.08f, -0.09f)) * scale(vec3(0.035f)), D, id++);
        // A warm spot light grazing the red tapestry of the -Z wall.
        render::PointLight spot;
        spot.position = vec3(0.0f, 8.2f, HALL_MIN_Z + 3.5f);
        spot.color = vec3(1.0f, 0.78f, 0.55f);
        spot.intensity = 150000.0f;
        spot.radius = 16.0f;
        spot.sourceRadius = 0.05f;
        spot.setSpot(vec3(0.0f, -0.9f, -1.0f), 14.0f * DEG, 26.0f * DEG);
        r.addLight(spot);
        r.endFrame();
    }

private:
    Mesh walls_, ceiling_, floor_, mullions_, glass_[3], tapBlue_, tapRed_, table_, legs_, chairs_, boardFrame_, lightSq_, darkSq_;
    Mesh pawn_, king_, ball_, torso_, arm_;
    Material floorMat_, wallMat_, ceilMat_, ironMat_, glassMat_, tapBlueMat_, tapRedMat_, tableMat_, woodMat_, frameMat_;
    Material lightSqMat_, darkSqMat_, whiteMarble_, blackMarble_, porcelain_, gold_;
    OrbitCamera cam_;
    vec3 sunDir_;
    float ev_ = 0.0f;
    float time_ = 0;
};

SCACELITH_SCENE("lightbox", "Lighting validation hall (sun beams, PCSS, GI bounce, planar reflections)", LightboxScene);
