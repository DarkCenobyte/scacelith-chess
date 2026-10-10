#include "world.h"
#include "../character/robot.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../render/materials/material_library.h"
#include "../render/post/display_transform.h"
#include "../render/post/postfx.h"
#include "../scene/board.h"
#include "../scene/clock_model.h"
#include "../scene/furniture.h"
#include "../scene/hall.h"
#include "../scene/piece_silhouette.h"
#include "../scene/pieces.h"
#include "../ui/ui_font.h"
#include "layout.h"
#include <algorithm>
#include <chrono>
#include <future>

using namespace m;
using namespace chess;

namespace game {

namespace {
enum ObjectIds : uint32_t {
    OBJ_HALL = 1, OBJ_TABLE = 2, OBJ_CHAIR = 3, OBJ_BOARD = 10, OBJ_CLOCK = 20, OBJ_LEVER = 21,
    OBJ_MARKER = 30, OBJ_PIECE = 100, OBJ_ROBOT = 1000, OBJ_COACH_MARK = 2000, OBJ_ANALYSIS_MARK = 2500
};

// The coach's light (Coach mode highlights and marks): a cobalt azure, the blue of the "COACH"
// under-glaze print, apart from the gold and ivory of the player's own markers.
const vec3 kCoachLight(0.25f, 0.50f, 1.0f);

// Board coordinates (marble.glsl, BOARD_COORDINATES): 16 distance-field cells of kCoordCell
// texels in a row (a..h, then 1..8), each glyph's ink centred in its cell. Cinzel, like the titles
// and the "COACH" print: inscriptional capitals suit letters inlaid in marble; its lowercase are
// small capitals, set a little smaller than the figures.
constexpr int kCoordCell = 64, kCoordSpread = 6;
constexpr float kCoordDigitPx = 36.0f;          // cap height of the figures, texels
constexpr float kCoordDigitHeight = 0.008f;     // ... in metres: ~13 px tall at 1080p (~9 at 720p) on the player's edge
constexpr float kCoordLetterScale = 0.86f;      // ink height of the letters / the figures'
constexpr float kCoordDilation = 0.00016f;      // bolder than the face (m on each side of the strokes)
const vec3 kCoordInlay(0.43f, 0.31f, 0.155f);  // pale gold stone (linear albedo)

GLuint bakeCoordinateAtlas() {
    const int W = kCoordCell * 16, H = kCoordCell;
    std::vector<uint8_t> atlas(size_t(W) * size_t(H), 0), px;
    auto inkRows = [](const std::vector<uint8_t>& v, int& top, int& bottom) {
        top = kCoordCell;
        bottom = -1;
        for (int y = 0; y < kCoordCell; ++y)
            for (int x = 0; x < kCoordCell; ++x)
                if (v[size_t(y) * kCoordCell + size_t(x)] >= 128) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
        return bottom >= top;
    };
    // The letters' size: small capitals measured against the figures at the same size.
    float letterPx = kCoordDigitPx;
    {
        int t0, b0, t1, b1;
        std::vector<uint8_t> d, l;
        if (ui::font::renderLineSdf(ui::font::FACE_TITLE, "8", kCoordDigitPx, kCoordSpread, 0.0f, kCoordCell, kCoordCell, d) &&
            ui::font::renderLineSdf(ui::font::FACE_TITLE, "h", kCoordDigitPx, kCoordSpread, 0.0f, kCoordCell, kCoordCell, l) &&
            inkRows(d, t0, b0) && inkRows(l, t1, b1))
            letterPx = kCoordDigitPx * kCoordLetterScale * float(b0 - t0 + 1) / float(b1 - t1 + 1);
    }
    for (int i = 0; i < 16; ++i) {
        std::string label(1, i < 8 ? char('a' + i) : char('1' + i - 8));
        if (!ui::font::renderLineSdf(ui::font::FACE_TITLE, label, i < 8 ? letterPx : kCoordDigitPx, kCoordSpread, 0.0f,
                                     kCoordCell, kCoordCell, px)) {
            LOGW("World: board coordinate \"%s\" not rendered", label.c_str());
            return 0;
        }
        int top = 0, bottom = kCoordCell - 1;
        inkRows(px, top, bottom);
        int shift = (top + bottom + 1) / 2 - kCoordCell / 2;  // centre the ink vertically
        for (int y = 0; y < H; ++y) {
            int sy = y + shift;
            if (sy < 0 || sy >= kCoordCell) continue;
            std::copy_n(&px[size_t(sy) * kCoordCell], kCoordCell, &atlas[size_t(y) * size_t(W) + size_t(i * kCoordCell)]);
        }
    }
    gpu::Texture tex = gpu::createTexture2D(W, H, GL_R8, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(tex.id, 0, 0, 0, W, H, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateTextureMipmap(tex.id);
    gpu::setFilter(tex, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
    gpu::setWrap(tex, GL_CLAMP_TO_EDGE);
    gpu::setAnisotropy(tex, 8.0f);  // the far edge is seen at a grazing angle
    return tex.id;
}

void submitModel(render::Renderer& r, const GpuModel& gm, const mat4& model, uint32_t objectId, uint32_t extraFlags,
                 const mat4* prevModel = nullptr, const vec4* inst = nullptr) {
    for (auto& p : gm.parts) {
        render::DrawItem d;
        d.mesh = &p.mesh;
        d.material = &materials::get(p.material);
        d.model = model;
        if (prevModel) {
            d.prevModel = *prevModel;
            d.hasPrevModel = true;
        }
        for (int k = 0; k < 4; ++k) d.inst[k] = inst ? inst[k] : p.inst[k];
        d.flags = p.flags | extraFlags;
        d.objectId = objectId;
        r.submit(d);
    }
}
}  // namespace

struct World::Impl {
    int step = 0;
    GpuModel hall, table, chair, board, clockBody, clockLever;
    ClockModel clockDesc;
    Mesh pieceBody[7], pieceFelt[7];
    PieceSilhouette pieceSilhouette[7];   // for picking (World::pieceSilhouette)
    character::GpuRobot robot;
    Mesh markerQuad;
    Material markerMat;
    // Coach mode: the coach's chest marking, its seat, and the material of its marks.
    character::ChestMarking coachMarking;
    int coachSeat = -1;
    Material coachMarkMat;
    // The unit quad (1 m, flat, uv over it) the coach's marks and the review's flat marks are drawn
    // on, scaled to cover each.
    Mesh markQuad;
    // Analysis mode: the material of the review's marks, and the badges' quad (sorted nearer than
    // it stands, see loadStep).
    Material analysisMarkMat;
    Mesh badgeQuad;
    // Board coordinates: the frame's material with the inlaid labels, their atlas, on/off.
    Material frameCoordMat;
    GLuint coordTex = 0;
    bool boardCoords = false;
    int reflFloor = -1, reflTable = -1, reflBoard = -1;
    // The robot's meshes being built on a worker thread (loading.porcelain) and when it started.
    std::future<std::vector<character::RobotPart>> robotParts;
    double robotStart = 0.0;
};

// Translation keys (assets/i18n, section "Loading").
static const char* kStepLabels[] = {
    "loading.materials", "loading.hall", "loading.furniture", "loading.board",
    "loading.pieces", "loading.clock", "loading.porcelain", "loading.ready"};
static constexpr int kStepCount = int(sizeof(kStepLabels) / sizeof(kStepLabels[0])) - 1;

World::World() : impl_(new Impl) {}
World::~World() {
    if (!impl_) return;
    impl_->hall.destroy();
    impl_->table.destroy();
    impl_->chair.destroy();
    impl_->board.destroy();
    impl_->clockBody.destroy();
    impl_->clockLever.destroy();
    for (int t = 0; t < 7; ++t) {
        impl_->pieceBody[t].destroy();
        impl_->pieceFelt[t].destroy();
    }
    impl_->robot.destroy();
    impl_->markerQuad.destroy();
    impl_->coachMarking.destroy();
    impl_->markQuad.destroy();
    impl_->badgeQuad.destroy();
    if (impl_->coordTex) glDeleteTextures(1, &impl_->coordTex);
}

bool World::loaded() const { return impl_->step >= kStepCount; }
float World::loadProgress() const { return float(impl_->step) / float(kStepCount); }
const char* World::loadLabel() const { return i18n::tr(kStepLabels[impl_->step < kStepCount ? impl_->step : kStepCount]); }

bool World::loadStep(bool wait) {
    Impl& w = *impl_;
    if (loaded()) return true;
    double t0 = plat::time();
    switch (w.step) {
    case 0: {
        materials::init();
        setupClockMaterials();
        character::setupRobotMaterials();
        w.markerMat.name = "game_marker";
        w.markerMat.surface = "shaders/materials/game_marker.glsl";
        w.markerMat.transparent = true;
        w.markerMat.castShadow = false;
        // Gold and ivory lines at about the level of sunlit white marble (game_marker.glsl).
        w.markerMat.params[0] = vec4(1.0f, 0.78f, 0.42f, 2.2f);
        w.markerMat.params[1] = vec4(0.95f, 0.93f, 0.88f, 0.0f);
        // uv spans [0,1] over the square (plane() gives uv in metres times uvScale): with metres,
        // the marker shapes saw only the corner of their [-1,1] space, which lit the whole touched
        // square and left the legal-move dots and rings out.
        w.markerQuad.upload(prim::plane(layout::SQUARE_SIZE, layout::SQUARE_SIZE, 1, 1, 1.0f / layout::SQUARE_SIZE), "marker");
        // Coach marks: cobalt lines a little below the gold markers' level (the blue saturates
        // less than a whiter line would), a thin dark rim and a soft halo on the board; no wash
        // (on a black square it read as a lit tile).
        w.coachMarkMat.name = "coach_marker";
        w.coachMarkMat.surface = "shaders/materials/coach_marker.glsl";
        w.coachMarkMat.transparent = true;
        w.coachMarkMat.castShadow = false;
        w.coachMarkMat.params[0] = vec4(kCoachLight, 1.9f);
        w.coachMarkMat.params[1] = vec4(0.34f, 0.24f, 0.0f, 0.45f);
        w.markQuad.upload(prim::plane(1.0f, 1.0f, 1, 1, 1.0f), "mark");
        // The review's marks (analysis_marker.glsl). The tint: a wash about as strong as a
        // tournament app's move highlight (half the board shows through), lit like a coloured glaze
        // (levels after exposure: on a dark square well above the black marble, or it would vanish);
        // a rim a little stronger. The arrow: four fifths opaque. The badge: tinted glass, 85 %
        // opaque (the rest of the light behind it passes in its hue, so its colour holds over a
        // sunlit square), the symbol white at the level of sunlit white marble.
        w.analysisMarkMat.name = "analysis_marker";
        w.analysisMarkMat.surface = "shaders/materials/analysis_marker.glsl";
        w.analysisMarkMat.transparent = true;
        w.analysisMarkMat.castShadow = false;
        w.analysisMarkMat.params[0] = vec4(0.42f, 0.90f, 2.4f, 0.55f);
        w.analysisMarkMat.params[1] = vec4(1.6f, 0.25f, 0.80f, 1.8f);
        w.analysisMarkMat.params[2] = vec4(0.85f, 0.35f, 2.4f, 0.35f);
        // A badge hovers above the board, so a flat mark under it on screen (a tint, an arrow) lies
        // behind it; transparents are drawn back to front by the centre of their bounds, and an
        // arrow's centre can be nearer than a badge's. The badge quad's bounds reach 0.3 m along its
        // normal (towards the camera, see submitAnalysisMarks): it sorts 15 cm nearer than it is,
        // after the flat marks.
        w.badgeQuad.upload(prim::plane(1.0f, 1.0f, 1, 1, 1.0f), "analysis_badge");
        w.badgeQuad.bounds.add(vec3(0.0f, 0.3f, 0.0f));
        break;
    }
    case 1: {
        w.hall.upload(hall::buildHall());
        // Floor tile grids aligned with the hall's layout (field tiles start at the field corner;
        // the inlay grid is offset so its joints miss the cabochons at the tile corners).
        materials::getMutable(MaterialId::FloorMarble).params[4] =
            vec4(hall::FLOOR_TILE, 0.0015f, -hall::FLOOR_FIELD_HALF_X, -hall::FLOOR_FIELD_HALF_Z);
        materials::getMutable(MaterialId::FloorMarbleInlay).params[4] =
            vec4(hall::FLOOR_TILE * 0.5f, 0.0015f, -hall::FLOOR_FIELD_HALF_X + 0.2f, -hall::FLOOR_FIELD_HALF_Z + 0.2f);
        break;
    }
    case 2:
        w.table.upload(furniture::buildTable());
        w.chair.upload(furniture::buildChair());
        break;
    case 3:
        w.board.upload(buildBoard());
        w.coordTex = bakeCoordinateAtlas();
        w.frameCoordMat = materials::get(MaterialId::BoardFrame);
        w.frameCoordMat.name = "BoardFrameCoordinates";
        w.frameCoordMat.defines.push_back("BOARD_COORDINATES");
        w.frameCoordMat.textures[1] = w.coordTex;
        break;
    case 4:
        for (int t = Pawn; t <= King; ++t) {
            PieceMeshes pm = buildPiece(t);
            w.pieceBody[t].upload(pm.body, "piece");
            w.pieceFelt[t].upload(pm.felt, "piece_felt");
            w.pieceSilhouette[t] = PieceSilhouette::fromMesh(pm.body);
        }
        break;
    case 5:
        w.clockDesc = buildClock();
        w.clockBody.upload(w.clockDesc.body);
        w.clockLever.upload(w.clockDesc.lever);
        break;
    case 6:
        // Built on a worker thread (pure CPU) while the loading screen keeps drawing and pumping
        // messages; uploaded here, on the GL thread, once ready. Each loading frame waits up to
        // 10 ms for it, so that with vsync off the loading screen does not take the build's CPU.
        if (!wait && !w.robotParts.valid()) {
            w.robotParts = std::async(std::launch::async, character::buildRobot);
            w.robotStart = t0;
        }
        if (w.robotParts.valid()) {
            if (!wait && w.robotParts.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) return false;
            t0 = w.robotStart;
            w.robot.upload(w.robotParts.get());
        } else {
            w.robot.upload(character::buildRobot());
        }
        w.coachMarking.create("COACH");
        break;
    default: break;
    }
    LOGI("World: %s (%.0f ms)", i18n::english(kStepLabels[w.step]), (plat::time() - t0) * 1000.0);
    ++w.step;
    return loaded();
}

void World::setupRenderer(render::Renderer& r) {
    Impl& w = *impl_;
    render::PlanarReflector floor;
    floor.point = vec3(0, 0, 0);
    floor.bounds.add(vec3(layout::HALL_MIN_X, 0.0f, layout::HALL_MIN_Z));
    floor.bounds.add(vec3(layout::HALL_MAX_X, 0.0f, layout::HALL_MAX_Z));
    w.reflFloor = r.addPlanarReflector(floor);
    render::PlanarReflector table;
    table.point = vec3(0, layout::TABLE_TOP_Y, 0);
    table.bounds.add(vec3(-layout::TABLE_WIDTH * 0.5f, layout::TABLE_TOP_Y, -layout::TABLE_DEPTH * 0.5f));
    table.bounds.add(vec3(layout::TABLE_WIDTH * 0.5f, layout::TABLE_TOP_Y, layout::TABLE_DEPTH * 0.5f));
    w.reflTable = r.addPlanarReflector(table);
    render::PlanarReflector board;
    board.point = vec3(0, layout::BOARD_TOP_Y, 0);
    board.bounds.add(vec3(-layout::BOARD_SIZE * 0.5f, layout::BOARD_TOP_Y, -layout::BOARD_SIZE * 0.5f));
    board.bounds.add(vec3(layout::BOARD_SIZE * 0.5f, layout::BOARD_TOP_Y, layout::BOARD_SIZE * 0.5f));
    w.reflBoard = r.addPlanarReflector(board);
    materials::getMutable(MaterialId::FloorMarble).planarReflector = w.reflFloor;
    materials::getMutable(MaterialId::FloorMarbleInlay).planarReflector = w.reflFloor;
    materials::getMutable(MaterialId::TableWood).planarReflector = w.reflTable;
    materials::getMutable(MaterialId::BoardSquareLight).planarReflector = w.reflBoard;
    materials::getMutable(MaterialId::BoardSquareDark).planarReflector = w.reflBoard;
    materials::getMutable(MaterialId::BoardFrame).planarReflector = w.reflBoard;
    // Markers, coach marks and the review's marks (transparent, main view only) first show during play.
    r.warmProgram(w.markerMat, render::PassId::Main);
    r.warmProgram(w.coachMarkMat, render::PassId::Main);
    r.warmProgram(w.analysisMarkMat, render::PassId::Main);
}

void World::setClockSide(bool positiveX) {
    clockPosX_ = positiveX;
    hasPrevLever_ = false;
}

mat4 World::clockTransform() const {
    // The clock's displays face local -X: towards the board when it stands at +X; rotated by
    // 180 degrees about Y when it stands at -X.
    float x = clockPosX_ ? layout::CLOCK_OFFSET_X : -layout::CLOCK_OFFSET_X;
    return translate(vec3(x, layout::TABLE_TOP_Y, layout::CLOCK_Z)) * rotateY(clockPosX_ ? 0.0f : PI);
}

vec3 World::clockPressPoint(int half) const {
    return transformPoint(clockTransform(), impl_->clockDesc.pressPoint[half & 1]);
}

int World::clockHalfForSeat(float seatZSign) const {
    // Half 0 is at local -Z. With the clock at +X (no rotation) local -Z is world -Z (Black's
    // seat); at -X the clock is turned around so half 0 faces White's seat (+Z).
    bool seatPosZ = seatZSign > 0.0f;
    return clockPosX_ ? (seatPosZ ? 1 : 0) : (seatPosZ ? 0 : 1);
}

const PieceSilhouette* World::pieceSilhouette(chess::PieceType t) const {
    if (t < Pawn || t > King) return nullptr;
    const PieceSilhouette& s = impl_->pieceSilhouette[t];
    return s.empty() ? nullptr : &s;
}

bool World::rayHitsClock(const Ray& ray, float* t) const {
    mat4 inv = inverseAffine(clockTransform());
    Ray local;
    local.o = transformPoint(inv, ray.o);
    local.d = transformDir(inv, ray.d);  // rigid transform: stays normalized
    AABB box;
    box.add(vec3(-layout::CLOCK_DEPTH * 0.5f - 0.01f, 0.0f, -layout::CLOCK_WIDTH * 0.5f - 0.01f));
    box.add(vec3(layout::CLOCK_DEPTH * 0.5f + 0.01f, layout::CLOCK_HEIGHT + 0.03f, layout::CLOCK_WIDTH * 0.5f + 0.01f));
    float tn = rayAABB(local, box);
    if (tn < 0.0f) return false;
    if (t) *t = tn;
    return true;
}

render::Environment World::environment(float time) const {
    render::Environment env;
    // Late morning sun through the three windows of the -X wall.
    env.sunDirection = hall::recommendedSunDirection();
    env.time = time;
    return env;
}

void World::submitStatic(render::Renderer& r) {
    Impl& w = *impl_;
    submitModel(r, w.hall, mat4(), OBJ_HALL, render::DRAW_STATIC);
    submitModel(r, w.table, mat4(), OBJ_TABLE, render::DRAW_STATIC);
    submitModel(r, w.chair, translate(vec3(0, 0, layout::CHAIR_Z)) * rotateY(PI), OBJ_CHAIR, render::DRAW_STATIC);
    submitModel(r, w.chair, translate(vec3(0, 0, -layout::CHAIR_Z)), OBJ_CHAIR + 1, render::DRAW_STATIC);
    if (!w.boardCoords || !w.coordTex) {
        submitModel(r, w.board, mat4(), OBJ_BOARD, render::DRAW_STATIC);
        return;
    }
    // The frame with its inlaid coordinates (its planar reflection is set up after loading).
    w.frameCoordMat.planarReflector = materials::get(MaterialId::BoardFrame).planarReflector;
    const float texel = kCoordDigitHeight / kCoordDigitPx;
    const float band = layout::BOARD_PLAY_SIZE * 0.5f + 0.0118f;  // middle of the border's flat (scene/board.cpp)
    vec4 inst[4] = {vec4(0.0f, band, float(kCoordCell) * texel, layout::BOARD_PLAY_SIZE * 0.5f),
                    vec4(kCoordInlay, kCoordDilation / texel),
                    vec4(2.0f * float(kCoordSpread), layout::SQUARE_SIZE, float(kCoordCell), layout::BOARD_TOP_Y), vec4(0.0f)};
    for (auto& p : w.board.parts) {
        render::DrawItem d;
        d.mesh = &p.mesh;
        bool frame = p.material == MaterialId::BoardFrame;
        d.material = frame ? &w.frameCoordMat : &materials::get(p.material);
        for (int k = 0; k < 4; ++k) d.inst[k] = frame ? inst[k] : p.inst[k];
        d.flags = p.flags | render::DRAW_STATIC;
        d.objectId = OBJ_BOARD;
        r.submit(d);
    }
}

void World::submitPieces(render::Renderer& r, const PhysicalBoard& board, const std::vector<PieceHighlight>* highlights) {
    Impl& w = *impl_;
    for (const PieceObject& p : board.pieces()) {
        if (p.type == NoPiece) continue;
        render::DrawItem d;
        d.mesh = &w.pieceBody[p.type];
        d.material = &materials::get(p.color == White ? MaterialId::MarbleWhitePiece : MaterialId::MarbleBlackPiece);
        d.model = p.transform;
        d.prevModel = p.prevTransform;
        d.hasPrevModel = true;
        d.objectId = OBJ_PIECE + uint32_t(p.id);
        d.inst[0] = vec4(float(hash32(uint32_t(p.id) * 2654435761u) & 0xFFFFu) / 65536.0f, 0, 0, 0);  // vein seed
        d.flags = render::DRAW_CAST_SHADOW;
        if (highlights)
            for (const PieceHighlight& h : *highlights)
                if (h.pieceId == p.id && h.strength > 0.0f) d.highlight = vec4(kCoachLight * 1.2f, std::min(h.strength, 1.0f));
        r.submit(d);
        render::DrawItem f = d;
        f.mesh = &w.pieceFelt[p.type];
        f.material = &materials::get(MaterialId::PieceFelt);
        f.flags = 0;
        f.highlight = vec4(0.0f);
        r.submit(f);
    }
}

void World::submitClock(render::Renderer& r, const ClockDisplay& cd) {
    Impl& w = *impl_;
    mat4 ct = clockTransform();
    vec4 inst[4] = {};
    uint32_t flags = (cd.unlimited ? CLOCK_FLAG_UNLIMITED : 0u) | (cd.flagged[0] ? CLOCK_FLAG_FALLEN_0 : 0u) |
                     (cd.flagged[1] ? CLOCK_FLAG_FALLEN_1 : 0u) | (cd.paused ? CLOCK_FLAG_PAUSED : 0u) |
                     (cd.dashes ? CLOCK_FLAG_DASHES : 0u);
    inst[0] = clockDisplayState(cd.ms[0], cd.ms[1], flags, cd.running);
    for (auto& p : w.clockBody.parts) {
        render::DrawItem d;
        d.mesh = &p.mesh;
        d.material = &materials::get(p.material);
        d.model = ct;
        for (int k = 0; k < 4; ++k) d.inst[k] = p.material == MaterialId::ClockDisplay ? inst[k] : p.inst[k];
        d.flags = p.flags;
        d.objectId = OBJ_CLOCK;
        r.submit(d);
    }
    const ClockModel& c = w.clockDesc;
    float angle = cd.leverSide * c.leverMaxAngle;
    mat4 lever = ct * translate(c.leverPivot) * rotateAxis(c.leverAxis, angle) * translate(-c.leverPivot);
    mat4 prevLever = ct * translate(c.leverPivot) * rotateAxis(c.leverAxis, hasPrevLever_ ? prevLeverAngle_ : angle) *
                     translate(-c.leverPivot);
    prevLeverAngle_ = angle;
    hasPrevLever_ = true;
    submitModel(r, w.clockLever, lever, OBJ_LEVER, 0, &prevLever);
}

void World::submitRobot(render::Renderer& r, int seat, const mat4* globals, const mat4* prevGlobals, bool firstPerson,
                        float armSeeThrough, character::Side armSide) {
    // The see-through arm keeps this share of its pixels: a ghost that still shows the hand and its
    // grip while the squares, markers and pieces under it read clearly. The blend is linear (HDR),
    // so a white arm at 25 % over dark squares already looks half-way; multiples of 1/4 are also
    // the dither's most even patterns under TAA (see screenDoorHidden()).
    constexpr float kGhostOpacity = 0.25f;
    float s = clamp(armSeeThrough, 0.0f, 1.0f);
    s = s * s * (3.0f - 2.0f * s);
    float opacity = 1.0f - (1.0f - kGhostOpacity) * s;
    const Material* chest = seat == impl_->coachSeat && impl_->coachMarking.valid() ? &impl_->coachMarking.material : nullptr;
    character::submitRobot(r, impl_->robot, globals, firstPerson, OBJ_ROBOT + uint32_t(seat) * 200u, prevGlobals,
                           /*pupilDilation=*/0.35f, opacity, armSide, chest);
}

void World::submitMarkers(render::Renderer& r, const std::vector<Marker>& markers) {
    Impl& w = *impl_;
    for (const Marker& mk : markers) {
        if (mk.square == NoSquare) continue;
        render::DrawItem d;
        d.mesh = &w.markerQuad;
        d.material = &w.markerMat;
        d.model = translate(layout::squareCenter(mk.square) + vec3(0, 0.0004f, 0));
        d.inst[0] = vec4(float(mk.kind), mk.strength, 0, 0);
        d.flags = render::DRAW_NO_REFLECTION;
        d.objectId = OBJ_MARKER + uint32_t(mk.square);
        r.submit(d);
    }
}

// ---- Coach mode -----------------------------------------------------------------------------------

void World::setCoachSeat(int seat) { impl_->coachSeat = seat == 0 || seat == 1 ? seat : -1; }
void World::setBoardCoordinates(bool on) { impl_->boardCoords = on; }

void World::submitPieces(render::Renderer& r, const PhysicalBoard& board) { submitPieces(r, board, nullptr); }

void World::submitCoachMarks(render::Renderer& r, const std::vector<CoachMark>& marks) {
    Impl& w = *impl_;
    // Arrow geometry (m). The shaft starts at the edge of the widest piece base (the king's 20 mm)
    // so it comes out from under the piece; the tip stops 16 mm short of the target's centre.
    constexpr float kStartClear = 0.021f, kTipClear = 0.016f;
    constexpr float kShaftHalf = 0.0026f, kHeadLen = 0.0150f, kHeadHalf = 0.0075f;
    constexpr float kFlowPeriod = 0.045f, kFlowSpeed = 0.07f;
    constexpr float kHaloReach = 0.012f;   // quad margin beyond the shape: the halo fades within it
    const float y = layout::BOARD_TOP_Y + 0.0005f;  // above the game markers (+0.4 mm)
    uint32_t n = 0;
    for (const CoachMark& mk : marks) {
        if (mk.strength <= 0.0f) continue;
        render::DrawItem d;
        d.mesh = &w.markQuad;
        d.material = &w.coachMarkMat;
        d.flags = render::DRAW_NO_REFLECTION;
        d.objectId = OBJ_COACH_MARK + n;
        float strength = std::min(mk.strength, 1.0f), age = std::max(mk.age, 0.0f);
        if (mk.kind == CoachMark::Square) {
            if (mk.sq == NoSquare) continue;
            vec3 c = layout::squareCenter(mk.sq);
            float half = layout::SQUARE_SIZE * 0.5f;
            float size = layout::SQUARE_SIZE * 1.3f;
            d.model = translate(vec3(c.x, y, c.z)) * scale(vec3(size, 1.0f, size));
            d.inst[0] = vec4(0.0f, strength, age, 0.0f);
            d.inst[1] = vec4(c.x, c.z, half, 0.0f);
        } else {
            if (mk.from == NoSquare || mk.to == NoSquare || mk.from == mk.to) continue;
            vec3 f3 = layout::squareCenter(mk.from), t3 = layout::squareCenter(mk.to);
            vec2 from(f3.x, f3.z), to(t3.x, t3.z), via = from;
            bool corner = mk.via != NoSquare && mk.via != mk.from && mk.via != mk.to;
            if (corner) {
                vec3 v3 = layout::squareCenter(mk.via);
                via = vec2(v3.x, v3.z);
            }
            vec2 firstDir = normalize((corner ? via : to) - from);
            vec2 lastDir = normalize(to - (corner ? via : from));
            vec2 start = from + firstDir * kStartClear;
            vec2 tip = to - lastDir * kTipClear;
            if (!corner) via = start;
            vec2 lo = min(min(start, via), tip), hi = max(max(start, via), tip);
            float grow = kHeadHalf + kHaloReach;
            lo = lo - vec2(grow);
            hi = hi + vec2(grow);
            vec2 c = (lo + hi) * 0.5f, size = hi - lo;
            d.model = translate(vec3(c.x, y, c.y)) * scale(vec3(size.x, 1.0f, size.y));
            d.inst[0] = vec4(1.0f, strength, age, 0.0f);
            d.inst[1] = vec4(start.x, start.y, via.x, via.y);
            d.inst[2] = vec4(tip.x, tip.y, kShaftHalf, kHeadLen);
            d.inst[3] = vec4(kHeadHalf, kFlowPeriod, kFlowSpeed, 0.0f);
        }
        r.submit(d);
        ++n;
    }
}

// ---- Analysis mode --------------------------------------------------------------------------------

namespace {
// The radiance after exposure that the final display transform (tonemap.frag, with the frame's
// grade) shows as the linear sRGB colour 'target': a multiplicative fixed-point iteration per
// channel, which settles in about 16 steps. Colours beyond AgX's reach (the most saturated
// oranges, yellows and cyans) come out as near as it gets, a little paler.
vec3 radianceShownAs(vec3 target, const PostSettings& ps) {
    vec3 x = target;
    for (int it = 0; it < 20; ++it) {
        vec3 shown = render::displayTransform(x, ps.contrast, ps.saturation, ps.splitTone);
        for (int k = 0; k < 3; ++k) x[k] = clamp(x[k] * std::max(target[k], 1e-3f) / std::max(shown[k], 1e-3f), 1e-4f, 16.0f);
    }
    return x;
}

// Scale of a badge popping in: 0.6 -> 1 in 0.25 s, overshooting by ~6 % on the way (an ease-out
// with a back swing).
float badgePop(float age) {
    float t = clamp(age / 0.25f, 0.0f, 1.0f) - 1.0f;
    constexpr float kBack = 2.2f;
    return 0.6f + 0.4f * (1.0f + (kBack + 1.0f) * t * t * t + kBack * t * t);
}
}  // namespace

void World::submitAnalysisMarks(render::Renderer& r, const std::vector<AnalysisMark>& marks, const vec3& camera) {
    Impl& w = *impl_;
    // Arrow geometry (m), more than twice the coach's: a 12.5 mm shaft and a 28 mm wide head, in
    // the proportions a chess site draws on a 55 mm square. Like the coach's it starts at the edge
    // of the widest piece base (the king's 20 mm); its tip stops 11 mm short of the target's centre.
    constexpr float kStartClear = 0.021f, kTipClear = 0.011f;
    constexpr float kShaftHalf = 0.00625f, kHeadLen = 0.021f, kHeadHalf = 0.014f;
    constexpr float kGrowTime = 0.35f;
    // Badge: 26 mm across (about 28 px at 720p mid-board from the player's chair, 24 on the far
    // rank), its centre 73 mm above the board: its lower edge just above the king's shoulder (the
    // collar under its crown, at 58 mm). Over a corner of the square, 39 mm from the axis of any
    // piece around that corner: no piece reaches that far at that height (the widest, the king's
    // crown, 12 mm), so a badge never cuts into one.
    constexpr float kBadgeDiameter = 0.026f, kBadgeHeight = 0.073f;
    constexpr float kBadgeQuad = 1.08f;   // quad half side / disc radius: room for the anti-aliasing
    const float yTint = layout::BOARD_TOP_Y + 0.00045f;   // a hair above the game markers (+0.4 mm)
    const float yArrow = layout::BOARD_TOP_Y + 0.0006f;   // above the tints
    const PostSettings& ps = r.post().settings;
    uint32_t n = 0;
    for (const AnalysisMark& mk : marks) {
        if (mk.strength <= 0.0f) continue;
        render::DrawItem d;
        d.mesh = &w.markQuad;
        d.material = &w.analysisMarkMat;
        d.flags = render::DRAW_NO_REFLECTION;
        d.objectId = OBJ_ANALYSIS_MARK + n;
        float strength = std::min(mk.strength, 1.0f), age = std::max(mk.age, 0.0f);
        vec3 radiance = radianceShownAs(max(mk.color, vec3(0.0f)), ps);
        if (mk.kind == AnalysisMark::Tint) {
            if (mk.sq == NoSquare) continue;
            vec3 c = layout::squareCenter(mk.sq);
            float size = layout::SQUARE_SIZE * 1.04f;
            bool light = ((mk.sq & 7) + (mk.sq >> 3)) & 1;   // a1 is dark
            d.model = translate(vec3(c.x, yTint, c.z)) * scale(vec3(size, 1.0f, size));
            d.inst[0] = vec4(0.0f, strength, age, 0.0f);
            d.inst[1] = vec4(c.x, c.z, layout::SQUARE_SIZE * 0.5f, light ? 1.0f : 0.0f);
            d.inst[2] = vec4(radiance, 0.0f);
        } else if (mk.kind == AnalysisMark::Arrow) {
            if (mk.from == NoSquare || mk.to == NoSquare || mk.from == mk.to) continue;
            vec3 f3 = layout::squareCenter(mk.from), t3 = layout::squareCenter(mk.to);
            vec2 from(f3.x, f3.z), to(t3.x, t3.z), via = from;
            bool corner = mk.via != NoSquare && mk.via != mk.from && mk.via != mk.to;
            if (corner) {
                vec3 v3 = layout::squareCenter(mk.via);
                via = vec2(v3.x, v3.z);
            }
            vec2 firstDir = normalize((corner ? via : to) - from);
            vec2 lastDir = normalize(to - (corner ? via : from));
            vec2 start = from + firstDir * kStartClear;
            vec2 tip = to - lastDir * kTipClear;
            // The quad covers the whole arrow; the shader gets the part grown so far: the arrow
            // runs out from its tail, its head in front (eased out, as a hand drawing it slows
            // down onto the square), round the corner of a knight's L.
            vec2 lo = min(min(start, corner ? via : start), tip), hi = max(max(start, corner ? via : start), tip);
            lo = lo - vec2(kHeadHalf + 0.002f);
            hi = hi + vec2(kHeadHalf + 0.002f);
            float l1 = corner ? length(via - start) : 0.0f, l2 = length(tip - (corner ? via : start));
            float g = 1.0f - clamp(age / kGrowTime, 0.0f, 1.0f);
            float grown = (1.0f - g * g * g) * (l1 + l2);
            if (grown < 0.001f) continue;
            vec2 nowCorner = start, nowTip = start + firstDir * grown;
            if (corner && grown > l1) {
                nowCorner = via;
                nowTip = via + lastDir * (grown - l1);
            }
            vec2 c = (lo + hi) * 0.5f, size = hi - lo;
            d.model = translate(vec3(c.x, yArrow, c.y)) * scale(vec3(size.x, 1.0f, size.y));
            d.inst[0] = vec4(1.0f, strength, age, kHeadHalf);
            d.inst[1] = vec4(start.x, start.y, nowCorner.x, nowCorner.y);
            d.inst[2] = vec4(nowTip.x, nowTip.y, kShaftHalf, kHeadLen);
            d.inst[3] = vec4(radiance, 0.0f);
        } else {
            if (mk.sq == NoSquare || mk.glyph < 1 || mk.glyph > 6) continue;
            vec3 sq = layout::squareCenter(mk.sq);
            // Which corner: the one that shows the badge at the upper right of the piece's head,
            // clear of it, as the camera sees the square. The right-hand ones (as the camera
            // faces the square). From a chair or an observer's usual height, the near one: raised
            // to the shoulder it shows just right of the head (the far one would float two
            // squares above, by the neighbour's head). Looking down from straight above, the
            // height no longer lifts it on screen: the far one, slid to as the view steepens
            // (between 55 and 80 degrees down; looking straight down: as if from White's side).
            vec2 ahead(sq.x - camera.x, sq.z - camera.z);
            float across = length(ahead);
            ahead = across > 0.01f ? ahead / across : vec2(0.0f, -1.0f);
            vec2 right(-ahead.y, ahead.x);
            float down = std::atan2(camera.y - layout::BOARD_TOP_Y, across);
            float farSide = 2.0f * smoothstep(55.0f * DEG, 80.0f * DEG, down) - 1.0f;
            vec2 at = vec2(sq.x, sq.z) + (ahead * farSide + right) * (layout::SQUARE_SIZE * 0.5f);
            vec3 c(at.x, layout::BOARD_TOP_Y + kBadgeHeight, at.y);
            // Billboard: the quad's normal (local +Y) towards the camera, its uv's up (local -Z)
            // the world's up as seen from there, so the symbol stands upright.
            vec3 nrm = normalize(camera - c);
            vec3 up = vec3(0.0f, 1.0f, 0.0f) - nrm * nrm.y;
            up = length(up) > 1e-3f ? normalize(up) : vec3(ahead.x, 0.0f, ahead.y);
            vec3 side = cross(up, nrm);
            float s = kBadgeDiameter * kBadgeQuad * badgePop(age);
            d.mesh = &w.badgeQuad;
            d.model = mat4(vec4(side * s, 0.0f), vec4(nrm, 0.0f), vec4(-up * s, 0.0f), vec4(c, 1.0f));
            d.inst[0] = vec4(2.0f, strength, age, 0.0f);
            d.inst[1] = vec4(float(mk.glyph), kBadgeQuad, 0.0f, 0.0f);
            d.inst[2] = vec4(radiance, 0.0f);
        }
        r.submit(d);
        ++n;
    }
}

std::vector<AnalysisMark> analysisMarksTestSet(float seconds) {
    auto srgb = [](uint32_t hex) {
        auto lin = [](uint32_t v) {
            float c = float(v & 0xFFu) / 255.0f;
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        return vec3(lin(hex >> 16), lin(hex >> 8), lin(hex));
    };
    // The palette of the symbols (analysis::nagColor) and the better move's green.
    const vec3 blunder = srgb(0xCA3431), mistake = srgb(0xE58F2A), dubious = srgb(0xF7C045);
    const vec3 interesting = srgb(0xB57FD6), good = srgb(0x5C8BB0), brilliant = srgb(0x1BACA6);
    const vec3 better = srgb(0x81B64C);
    std::vector<AnalysisMark> marks;
    auto add = [&](AnalysisMark::Kind kind, const char* sq, int glyph, vec3 color) {
        AnalysisMark mk;
        mk.kind = kind;
        mk.sq = parseSquare(sq);
        mk.glyph = glyph;
        mk.color = color;
        marks.push_back(mk);
    };
    auto arrow = [&](const char* from, const char* via, const char* to) {
        AnalysisMark mk;
        mk.kind = AnalysisMark::Arrow;
        mk.from = parseSquare(from);
        mk.via = via ? parseSquare(via) : NoSquare;
        mk.to = parseSquare(to);
        mk.color = better;
        marks.push_back(mk);
    };
    add(AnalysisMark::Tint, "e5", 0, blunder);
    add(AnalysisMark::Badge, "e5", 4, blunder);       // ??
    add(AnalysisMark::Badge, "f3", 3, brilliant);     // !!
    add(AnalysisMark::Tint, "f3", 0, brilliant);
    add(AnalysisMark::Badge, "c4", 5, interesting);   // !?
    add(AnalysisMark::Badge, "d4", 6, dubious);       // ?!
    add(AnalysisMark::Badge, "g1", 1, good);          // !
    add(AnalysisMark::Badge, "b8", 2, mistake);       // ?
    arrow("d2", nullptr, "d4");
    arrow("b1", "b3", "c3");   // a knight's L: its long leg first
    // Their arrivals staggered by 0.15 s.
    for (size_t k = 0; k < marks.size(); ++k) {
        marks[k].age = seconds - 0.15f * float(k);
        marks[k].strength = marks[k].age >= 0.0f ? 1.0f : 0.0f;
    }
    return marks;
}

}  // namespace game
