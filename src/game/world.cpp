#include "world.h"
#include "../character/robot.h"
#include "../core/log.h"
#include "../platform/platform.h"
#include "../render/materials/material_library.h"
#include "../scene/board.h"
#include "../scene/clock_model.h"
#include "../scene/furniture.h"
#include "../scene/hall.h"
#include "../scene/pieces.h"
#include "layout.h"

using namespace m;
using namespace chess;

namespace game {

namespace {
enum ObjectIds : uint32_t {
    OBJ_HALL = 1, OBJ_TABLE = 2, OBJ_CHAIR = 3, OBJ_BOARD = 10, OBJ_CLOCK = 20, OBJ_LEVER = 21,
    OBJ_MARKER = 30, OBJ_PIECE = 100, OBJ_ROBOT = 1000
};

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
    character::GpuRobot robot;
    Mesh markerQuad;
    Material markerMat;
    int reflFloor = -1, reflTable = -1, reflBoard = -1;
};

static const char* kStepLabels[] = {
    "Preparing materials", "Raising the hall", "Carving the table and chairs", "Polishing the board",
    "Turning the chess set", "Assembling the clock", "Firing the porcelain", "Ready"};
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
}

bool World::loaded() const { return impl_->step >= kStepCount; }
float World::loadProgress() const { return float(impl_->step) / float(kStepCount); }
const char* World::loadLabel() const { return kStepLabels[impl_->step < kStepCount ? impl_->step : kStepCount]; }

bool World::loadStep() {
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
        w.markerMat.params[0] = vec4(1.0f, 0.86f, 0.55f, 5200.0f);
        w.markerQuad.upload(prim::plane(layout::SQUARE_SIZE, layout::SQUARE_SIZE, 1, 1, 1.0f), "marker");
        break;
    }
    case 1: w.hall.upload(buildHall()); break;
    case 2:
        w.table.upload(buildTable());
        w.chair.upload(buildChair());
        break;
    case 3: w.board.upload(buildBoard()); break;
    case 4:
        for (int t = Pawn; t <= King; ++t) {
            PieceMeshes pm = buildPiece(t);
            w.pieceBody[t].upload(pm.body, "piece");
            w.pieceFelt[t].upload(pm.felt, "piece_felt");
        }
        break;
    case 5:
        w.clockDesc = buildClock();
        w.clockBody.upload(w.clockDesc.body);
        w.clockLever.upload(w.clockDesc.lever);
        break;
    case 6: w.robot.upload(character::buildRobot()); break;
    default: break;
    }
    LOGI("World: %s (%.0f ms)", kStepLabels[w.step], (plat::time() - t0) * 1000.0);
    ++w.step;
    return loaded();
}

void World::setupRenderer(render::Renderer& r) {
    Impl& w = *impl_;
    render::PlanarReflector floor;
    floor.point = vec3(0, 0, 0);
    floor.resolutionScale = 0.5f;
    w.reflFloor = r.addPlanarReflector(floor);
    render::PlanarReflector table;
    table.point = vec3(0, layout::TABLE_TOP_Y, 0);
    table.resolutionScale = 0.5f;
    w.reflTable = r.addPlanarReflector(table);
    render::PlanarReflector board;
    board.point = vec3(0, layout::BOARD_TOP_Y, 0);
    board.resolutionScale = 0.5f;
    w.reflBoard = r.addPlanarReflector(board);
    materials::getMutable(MaterialId::FloorMarble).planarReflector = w.reflFloor;
    materials::getMutable(MaterialId::FloorMarbleInlay).planarReflector = w.reflFloor;
    materials::getMutable(MaterialId::TableWood).planarReflector = w.reflTable;
    materials::getMutable(MaterialId::BoardSquareLight).planarReflector = w.reflBoard;
    materials::getMutable(MaterialId::BoardSquareDark).planarReflector = w.reflBoard;
    materials::getMutable(MaterialId::BoardFrame).planarReflector = w.reflBoard;
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
    env.sunDirection = normalize(vec3(-0.80f, 0.52f, 0.22f));
    env.time = time;
    return env;
}

void World::submitStatic(render::Renderer& r) {
    Impl& w = *impl_;
    submitModel(r, w.hall, mat4(), OBJ_HALL, render::DRAW_STATIC);
    submitModel(r, w.table, mat4(), OBJ_TABLE, render::DRAW_STATIC);
    submitModel(r, w.chair, translate(vec3(0, 0, layout::CHAIR_Z)) * rotateY(PI), OBJ_CHAIR, render::DRAW_STATIC);
    submitModel(r, w.chair, translate(vec3(0, 0, -layout::CHAIR_Z)), OBJ_CHAIR + 1, render::DRAW_STATIC);
    submitModel(r, w.board, mat4(), OBJ_BOARD, render::DRAW_STATIC);
}

void World::submitPieces(render::Renderer& r, const PhysicalBoard& board) {
    Impl& w = *impl_;
    for (const PieceObject& p : board.pieces()) {
        if (p.type == NoPiece) continue;
        if (p.inReserve && !p.held) {
            // Spare pieces are only shown once they have been brought to the table.
        }
        render::DrawItem d;
        d.mesh = &w.pieceBody[p.type];
        d.material = &materials::get(p.color == White ? MaterialId::MarbleWhitePiece : MaterialId::MarbleBlackPiece);
        d.model = p.transform;
        d.prevModel = p.prevTransform;
        d.hasPrevModel = true;
        d.objectId = OBJ_PIECE + uint32_t(p.id);
        d.flags = render::DRAW_CAST_SHADOW;
        r.submit(d);
        render::DrawItem f = d;
        f.mesh = &w.pieceFelt[p.type];
        f.material = &materials::get(MaterialId::PieceFelt);
        f.flags = 0;
        r.submit(f);
    }
}

void World::submitClock(render::Renderer& r, const ClockDisplay& cd) {
    Impl& w = *impl_;
    mat4 ct = clockTransform();
    vec4 inst[4] = {};
    uint32_t flags = (cd.unlimited ? CLOCK_FLAG_UNLIMITED : 0u) | (cd.flagged[0] ? CLOCK_FLAG_FALLEN_0 : 0u) |
                     (cd.flagged[1] ? CLOCK_FLAG_FALLEN_1 : 0u) | (cd.paused ? CLOCK_FLAG_PAUSED : 0u);
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

void World::submitRobot(render::Renderer& r, int seat, const mat4* globals, const mat4* prevGlobals, bool firstPerson) {
    character::submitRobot(r, impl_->robot, globals, firstPerson, OBJ_ROBOT + uint32_t(seat) * 200u, prevGlobals);
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
        d.flags = render::DRAW_NO_REFLECTION | render::DRAW_NO_VELOCITY;
        d.objectId = OBJ_MARKER + uint32_t(mk.square);
        r.submit(d);
    }
}

float World::pieceHeight(PieceType t) { return layout::PIECE_HEIGHT[t]; }
float World::pieceRadius(PieceType t) { return layout::PIECE_BASE_RADIUS[t]; }

}  // namespace game
