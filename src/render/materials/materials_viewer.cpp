// "materials" viewer scene: shader-ball presentation of the whole material library under the
// default lighting. Close-ups: --view <overview|marble|board|pieces|floor|wood|fabric|metal|glass|
// stone|ceiling>. Optional: --sun <azimuthDeg,elevationDeg>, --orbit <yawDeg> (adds to the view).
// Keys: 1..9/0 switch views, right-drag orbit, wheel zoom, middle-drag pan.
#include "material_library.h"
#include "../../app/orbit_camera.h"
#include "../../app/scene.h"
#include "../../core/log.h"
#include "../../game/layout.h"
#include "../mesh.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace m;

namespace {

// ---- Geometry helpers --------------------------------------------------------------------------
MeshData transformed(MeshData d, const mat4& t) {
    d.transform(t);
    return d;
}

// Staunton-like lathe profiles (meters), bottom to top.
std::vector<vec2> pawnProfile(float h) {
    std::vector<vec2> p = {{0.0f, 0.0f},        {0.0145f, 0.0f},     {0.0145f, 0.0f},     {0.0147f, 0.0025f},
                           {0.0138f, 0.0045f},  {0.0124f, 0.0058f},  {0.0118f, 0.0075f},  {0.0102f, 0.0098f},
                           {0.0080f, 0.0140f},  {0.0066f, 0.0200f},  {0.0060f, 0.0260f},  {0.0094f, 0.0288f},
                           {0.0098f, 0.0302f},  {0.0070f, 0.0318f},  {0.0056f, 0.0330f}};
    const float cy = 0.0410f, r = 0.0088f;
    for (int i = 0; i <= 12; ++i) {
        float a = -0.95f + (PI * 0.5f + 0.95f) * float(i) / 12.0f;
        p.push_back({r * std::cos(a), cy + r * std::sin(a)});
    }
    p.back().x = 0.0f;
    for (auto& v : p) v.y *= h / 0.0498f;
    return p;
}

std::vector<vec2> bishopProfile() {
    std::vector<vec2> p = {{0.0f, 0.0f},        {0.0175f, 0.0f},     {0.0175f, 0.0f},     {0.0177f, 0.0030f},
                           {0.0165f, 0.0055f},  {0.0148f, 0.0070f},  {0.0140f, 0.0092f},  {0.0118f, 0.0120f},
                           {0.0090f, 0.0200f},  {0.0068f, 0.0350f},  {0.0060f, 0.0450f},  {0.0102f, 0.0482f},
                           {0.0105f, 0.0500f},  {0.0070f, 0.0520f},  {0.0068f, 0.0545f}};
    // Mitre (ogive) and finial.
    for (int i = 0; i <= 10; ++i) {
        float t = float(i) / 10.0f;
        float y = 0.0545f + t * 0.021f;
        float r = 0.0092f * std::sin(PI * (0.18f + 0.82f * t) * 0.62f + 0.35f) * (1.0f - t * t * 0.9f);
        p.push_back({std::max(r, 0.0018f), y});
    }
    const float cy = 0.0790f, r = 0.0034f;
    for (int i = 0; i <= 8; ++i) {
        float a = -PI * 0.5f + PI * float(i) / 8.0f;
        p.push_back({r * std::cos(a), cy + r * std::sin(a)});
    }
    p.back().x = 0.0f;
    return p;
}

// Board squares of one colour: 32 thin boxes, uv per board package convention (top face uv spans
// [file/8,(file+1)/8] x [rank/8,(rank+1)/8]).
MeshData boardSquares(bool light) {
    MeshData all;
    const float s = layout::SQUARE_SIZE, th = layout::BOARD_THICKNESS;
    for (int rank = 0; rank < 8; ++rank)
        for (int file = 0; file < 8; ++file) {
            bool isLight = ((file + rank) & 1) == 1;
            if (isLight != light) continue;
            MeshData b = prim::box(vec3(s * 0.5f, th * 0.5f, s * 0.5f));
            vec3 c = layout::squareCenter(file, rank) - vec3(0, th * 0.5f, 0);
            for (auto& v : b.vertices) {
                v.pos += c;
                v.uv = vec2((float(file) + v.uv.x) / 8.0f, (float(rank) + v.uv.y) / 8.0f);
            }
            all.append(b);
        }
    return all;
}

MeshData boardFrame() {
    MeshData all;
    const float half = layout::BOARD_PLAY_SIZE * 0.5f, b = layout::BOARD_BORDER, th = layout::BOARD_THICKNESS;
    const float y = layout::TABLE_TOP_Y + th * 0.5f;
    all.append(transformed(prim::roundedBox(vec3(half + b, th * 0.5f, b * 0.5f), 0.002f, 2), translate(vec3(0, y, half + b * 0.5f))));
    all.append(transformed(prim::roundedBox(vec3(half + b, th * 0.5f, b * 0.5f), 0.002f, 2), translate(vec3(0, y, -half - b * 0.5f))));
    all.append(transformed(prim::roundedBox(vec3(b * 0.5f, th * 0.5f, half), 0.002f, 2), translate(vec3(half + b * 0.5f, y, 0))));
    all.append(transformed(prim::roundedBox(vec3(b * 0.5f, th * 0.5f, half), 0.002f, 2), translate(vec3(-half - b * 0.5f, y, 0))));
    return all;
}

// Vertical cloth with soft folds (curtain / velvet sample), x in [-w/2,w/2], y in [0,h], uv [0,1].
MeshData foldedCloth(float w, float h, float depth, int folds) {
    MeshData d;
    const int nx = 160, ny = 60;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) {
            float u = float(i) / nx, v = float(j) / ny;
            float a = u * TAU * float(folds);
            float amp = depth * (0.55f + 0.45f * v);
            Vertex vx;
            vx.pos = vec3((u - 0.5f) * w, v * h, std::sin(a) * amp);
            vx.uv = vec2(u, v);
            d.vertices.push_back(vx);
        }
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            uint32_t a = uint32_t(j * (nx + 1) + i), b = a + 1, c = a + uint32_t(nx + 1), e = c + 1;
            for (uint32_t k : {a, b, e, a, e, c}) d.indices.push_back(k);
        }
    d.computeNormals();
    d.computeTangents();
    return d;
}

// Vertical plane facing +Z: x in [-w/2,w/2], y in [0,h], uv [0,1] (u right, v up).
MeshData wallPlane(float w, float h) {
    MeshData d = prim::plane(w, h, 1, 1, 1.0f);
    for (auto& v : d.vertices) {
        vec3 p = v.pos;
        v.pos = vec3(p.x, -p.z + h * 0.5f, 0.0f);
        v.normal = vec3(0, 0, 1);
        v.tangent = vec4(1, 0, 0, 1);
        v.uv = vec2(v.uv.x / w, v.uv.y / h);
    }
    return d;
}

// Unshares vertices and uses face normals (faceted crystal).
MeshData faceted(const MeshData& src) {
    MeshData d;
    for (size_t i = 0; i + 2 < src.indices.size(); i += 3) {
        Vertex a = src.vertices[src.indices[i]], b = src.vertices[src.indices[i + 1]], c = src.vertices[src.indices[i + 2]];
        vec3 n = cross(b.pos - a.pos, c.pos - a.pos);
        if (length2(n) < 1e-18f) continue;
        n = normalize(n);
        for (Vertex* v : {&a, &b, &c}) {
            v->normal = n;
            d.vertices.push_back(*v);
            d.indices.push_back(uint32_t(d.vertices.size() - 1));
        }
    }
    d.computeTangents();
    return d;
}

struct ViewPreset {
    const char* name;
    vec3 target;
    float yawDeg, pitchDeg, distance, fovDeg;
};
const ViewPreset kViews[] = {
    {"overview", {0.0f, 0.55f, -0.3f}, 20.0f, 18.0f, 4.2f, 45.0f},
    {"marble", {0.0f, 0.79f, 0.02f}, 8.0f, 38.0f, 0.62f, 45.0f},
    {"board", {-0.06f, 0.782f, 0.06f}, 25.0f, 30.0f, 0.34f, 40.0f},
    {"pieces", {0.02f, 0.81f, 0.13f}, -12.0f, 14.0f, 0.24f, 35.0f},
    {"floor", {-0.2f, 0.0f, 1.2f}, 30.0f, 28.0f, 2.2f, 50.0f},
    {"wood", {0.25f, 0.70f, 0.25f}, 35.0f, 30.0f, 1.0f, 45.0f},
    {"tabletop", {-0.38f, 0.76f, 0.28f}, 20.0f, 32.0f, 0.45f, 45.0f},
    {"carved", {1.0f, 0.45f, 0.3f}, 20.0f, 10.0f, 0.75f, 45.0f},
    {"fabric", {2.8f, 1.0f, -1.9f}, 0.0f, 8.0f, 3.2f, 45.0f},
    {"tapestry", {1.55f, 1.25f, -2.2f}, 0.0f, 0.0f, 1.0f, 45.0f},
    {"velvet", {2.6f, 0.2f, -1.3f}, 10.0f, 25.0f, 1.0f, 45.0f},
    {"metal", {-2.2f, 0.3f, -1.1f}, 10.0f, 18.0f, 1.3f, 45.0f},
    {"gilded", {-2.6f, 0.2f, -1.2f}, 15.0f, 20.0f, 0.55f, 45.0f},
    {"glass", {-2.9f, 1.2f, -1.9f}, 15.0f, 5.0f, 2.3f, 45.0f},
    {"stone", {-0.5f, 1.4f, -2.4f}, 5.0f, 5.0f, 3.0f, 50.0f},
    {"ceiling", {0.3f, 3.2f, -1.4f}, 0.0f, -55.0f, 2.2f, 55.0f},
    {"spheres", {0.0f, 0.25f, 1.3f}, 0.0f, 12.0f, 1.8f, 45.0f},
    {"slabs", {0.0f, 0.0f, 2.75f}, 0.0f, 62.0f, 1.25f, 45.0f},
    {"swatches", {0.0f, 0.0f, 3.1f}, 0.0f, 89.0f, 1.55f, 45.0f},
};

struct Obj {
    Mesh* mesh;
    MaterialId mat;
    mat4 model;
    vec4 inst0;
    uint32_t id;
    vec4 inst1;
};

}  // namespace

class MaterialsViewer : public Scene {
public:
    bool init(AppContext& ctx) override {
        if (!materials::init()) return false;
        // Planar reflection for the floor.
        render::PlanarReflector pr;
        pr.point = vec3(0, 0, 0);
        floorPlanar_ = render::renderer().addPlanarReflector(pr);
        materials::getMutable(MaterialId::FloorMarble).planarReflector = floorPlanar_;
        materials::getMutable(MaterialId::FloorMarbleInlay).planarReflector = floorPlanar_;

        albedoDebug_ = ctx.hasArg("--albedo");
        // --albedo: every material shows its albedo unlit (library shaders honour MAT_DEBUG_ALBEDO).
        if (albedoDebug_)
            for (int k = 0; k < int(MaterialId::Count); ++k) materials::getMutable(MaterialId(k)).defines.push_back("MAT_DEBUG_ALBEDO");
        build();
        std::string v = ctx.argValue("--view", "overview");
        setView(v);
        std::string orbit = ctx.argValue("--orbit");
        if (!orbit.empty()) cam_.yaw += float(std::atof(orbit.c_str())) * DEG;
        std::string sun = ctx.argValue("--sun");
        if (!sun.empty()) {
            float az = 0, el = 0;
            if (std::sscanf(sun.c_str(), "%f,%f", &az, &el) == 2)
                env_.sunDirection = normalize(vec3(std::cos(el * DEG) * std::sin(az * DEG), std::sin(el * DEG), std::cos(el * DEG) * std::cos(az * DEG)));
        }
        return true;
    }

    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        const plat::Input& in = plat::input();
        for (int k = 0; k < 10; ++k)
            if (in.keyPressed[plat::KEY_0 + k]) {
                int idx = k == 0 ? 9 : k - 1;
                if (idx < int(sizeof(kViews) / sizeof(kViews[0]))) setView(kViews[idx].name);
            }
        cam_.update(in);
        return !in.keyPressed[plat::KEY_ESCAPE];
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        env_.time = time_;
        r.beginFrame(cam_.camera(), env_, dt);
        for (const Obj& o : objs_) {
            render::DrawItem d;
            d.mesh = o.mesh;
            d.material = &materials::get(o.mat);
            d.model = o.model;
            d.inst[0] = o.inst0;
            d.inst[1] = o.inst1;
            d.objectId = o.id;
            d.flags = render::DRAW_CAST_SHADOW | render::DRAW_STATIC;
            r.submit(d);
        }
        uint32_t id = 1000;
        for (const Extra& e : extra_) {
            render::DrawItem d;
            d.mesh = e.mesh;
            d.material = e.mat;
            d.model = e.model;
            d.inst[0] = e.inst0;
            d.objectId = id++;
            r.submit(d);
        }
        r.endFrame();
    }

    void shutdown(AppContext&) override {
        materials::getMutable(MaterialId::FloorMarble).planarReflector = -1;
        materials::getMutable(MaterialId::FloorMarbleInlay).planarReflector = -1;
        if (albedoDebug_)
            for (int k = 0; k < int(MaterialId::Count); ++k) {
                auto& defs = materials::getMutable(MaterialId(k)).defines;
                defs.erase(std::remove(defs.begin(), defs.end(), std::string("MAT_DEBUG_ALBEDO")), defs.end());
            }
        for (Mesh* m : meshes_) { m->destroy(); delete m; }
        meshes_.clear();
    }

private:
    Mesh* upload(const MeshData& d, const char* name) {
        Mesh* m = new Mesh();
        m->upload(d, name);
        meshes_.push_back(m);
        return m;
    }
    void add(Mesh* mesh, MaterialId mat, const mat4& model, vec4 inst0 = vec4(0)) {
        objs_.push_back({mesh, mat, model, inst0, uint32_t(objs_.size() + 1), vec4(0)});
    }

    void setView(const std::string& name) {
        for (const ViewPreset& v : kViews)
            if (name == v.name) {
                cam_.target = v.target;
                cam_.yaw = v.yawDeg * DEG;
                cam_.pitch = v.pitchDeg * DEG;
                cam_.distance = v.distance;
                cam_.fovY = v.fovDeg * DEG;
                return;
            }
        LOGW("materials viewer: unknown view '%s'", name.c_str());
    }

    void build() {
        // Floor + inlay bands.
        add(upload(prim::plane(14, 14, 4, 4, 1.0f), "floor"), MaterialId::FloorMarble, mat4());
        Mesh* band = upload(prim::plane(14.0f, 0.24f, 8, 1, 1.0f), "inlay");
        add(band, MaterialId::FloorMarbleInlay, translate(vec3(0, 0.0004f, 2.0f)));
        add(band, MaterialId::FloorMarbleInlay, translate(vec3(-3.2f, 0.0004f, 0)) * rotateY(PI * 0.5f));

        // Table (wood samples) with the marble board and pieces.
        const float ty = layout::TABLE_TOP_Y, tt = layout::TABLE_TOP_THICKNESS;
        add(upload(prim::roundedBox(vec3(layout::TABLE_WIDTH * 0.5f, tt * 0.5f, layout::TABLE_DEPTH * 0.5f), 0.008f, 3), "tabletop"),
            MaterialId::TableWood, translate(vec3(0, ty - tt * 0.5f, 0)));
        MeshData leg = prim::lathe({{0.0f, 0.0f}, {0.030f, 0.0f}, {0.030f, 0.0f}, {0.034f, 0.03f}, {0.026f, 0.06f}, {0.022f, 0.25f},
                                    {0.030f, 0.42f}, {0.036f, 0.50f}, {0.028f, 0.60f}, {0.032f, 0.64f}, {0.032f, 0.72f}, {0.0f, 0.72f}},
                                   48);
        Mesh* legM = upload(leg, "leg");
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                add(legM, MaterialId::TableWoodCarved, translate(vec3(sx * 0.53f, 0.0f, sz * 0.36f)));
        add(upload(boardSquares(true), "squaresLight"), MaterialId::BoardSquareLight, mat4());
        add(upload(boardSquares(false), "squaresDark"), MaterialId::BoardSquareDark, mat4());
        add(upload(boardFrame(), "boardFrame"), MaterialId::BoardFrame, mat4());
        Mesh* pawn = upload(prim::lathe(pawnProfile(layout::PIECE_HEIGHT[1]), 96), "pawn");
        Mesh* bishop = upload(prim::lathe(bishopProfile(), 96), "bishop");
        for (int f = 0; f < 8; ++f) {
            add(pawn, MaterialId::MarbleWhitePiece, translate(layout::squareCenter(f, 1)), vec4(float(f) + 1.0f, 0, 0, 0));
            add(pawn, MaterialId::MarbleBlackPiece, translate(layout::squareCenter(f, 6)), vec4(float(f) + 11.0f, 0, 0, 0));
        }
        add(bishop, MaterialId::MarbleWhitePiece, translate(layout::squareCenter(2, 0)), vec4(21, 0, 0, 0));
        add(bishop, MaterialId::MarbleWhitePiece, translate(layout::squareCenter(5, 0)), vec4(22, 0, 0, 0));
        add(bishop, MaterialId::MarbleBlackPiece, translate(layout::squareCenter(2, 7)), vec4(23, 0, 0, 0));
        add(bishop, MaterialId::MarbleBlackPiece, translate(layout::squareCenter(5, 7)), vec4(24, 0, 0, 0));
        // A white pawn advanced to the centre for close-ups.
        add(pawn, MaterialId::MarbleWhitePiece, translate(layout::squareCenter(4, 3)), vec4(31, 0, 0, 0));
        add(bishop, MaterialId::MarbleBlackPiece, translate(layout::squareCenter(3, 4)), vec4(32, 0, 0, 0));

        // Wood samples: clock case on the table, carved baluster + rail (chair), wainscot panel.
        add(upload(prim::roundedBox(vec3(layout::CLOCK_DEPTH * 0.5f, layout::CLOCK_HEIGHT * 0.5f, layout::CLOCK_WIDTH * 0.5f), 0.006f, 3), "clockCase"),
            MaterialId::ClockCase, translate(vec3(layout::CLOCK_OFFSET_X, ty + layout::CLOCK_HEIGHT * 0.5f, 0.0f)));
        MeshData bal = prim::lathe({{0.0f, 0.0f}, {0.035f, 0.0f}, {0.035f, 0.0f}, {0.035f, 0.05f}, {0.028f, 0.06f}, {0.040f, 0.08f},
                                    {0.030f, 0.10f}, {0.020f, 0.16f}, {0.034f, 0.30f}, {0.042f, 0.38f}, {0.030f, 0.46f},
                                    {0.020f, 0.52f}, {0.026f, 0.55f}, {0.018f, 0.58f}, {0.024f, 0.62f}, {0.024f, 0.62f},
                                    {0.036f, 0.64f}, {0.036f, 0.68f}, {0.0f, 0.68f}}, 64);
        Mesh* balM = upload(bal, "baluster");
        add(balM, MaterialId::ChairWood, translate(vec3(0.95f, 0.0f, 0.35f)));
        add(balM, MaterialId::TableWoodCarved, translate(vec3(1.15f, 0.0f, 0.1f)));
        add(upload(prim::roundedBox(vec3(0.3f, 0.025f, 0.02f), 0.006f, 3), "rail"), MaterialId::ChairWood,
            translate(vec3(1.05f, 0.45f, 0.35f)), vec4(1, 0, 0, 3));
        add(upload(prim::roundedBox(vec3(0.5f, 0.45f, 0.02f), 0.004f, 2), "panel"), MaterialId::WallPanelWood,
            translate(vec3(1.3f, 0.45f, -0.7f)));

        // Fabrics: two hangings (blue fleur-de-lis, red damask), a velvet curtain, a velvet
        // cushion and ball, green baize.
        Mesh* hang = upload(wallPlane(1.4f, 2.1f), "hanging");
        add(hang, MaterialId::Tapestry, translate(vec3(1.55f, 0.15f, -2.2f)), vec4(0, 0, 0.3f, 0));
        objs_.back().inst1 = vec4(1.4f, 2.1f, 0, 0);
        add(hang, MaterialId::Tapestry, translate(vec3(3.1f, 0.15f, -2.2f)), vec4(1, 1, 0.7f, 0));
        objs_.back().inst1 = vec4(1.4f, 2.1f, 0, 0);
        add(upload(foldedCloth(1.1f, 2.3f, 0.06f, 5), "curtain"), MaterialId::Curtain, translate(vec3(4.45f, 0.02f, -2.0f)));
        add(upload(prim::roundedBox(vec3(0.24f, 0.05f, 0.24f), 0.045f, 5), "cushion"), MaterialId::ChairVelvet,
            translate(vec3(2.1f, 0.05f, -1.3f)));
        add(upload(prim::sphere(0.14f, 96, 48), "velvetBall"), MaterialId::ChairVelvet, translate(vec3(2.65f, 0.14f, -1.25f)));
        add(upload(prim::cylinder(0.16f, 0.012f, 64), "baize"), MaterialId::PieceFelt, translate(vec3(3.15f, 0.0f, -1.25f)));
        add(upload(prim::sphere(0.08f, 64, 32), "feltBall"), MaterialId::PieceFelt, translate(vec3(3.15f, 0.092f, -1.25f)));

        // Hall backdrop: limestone wall, walnut wainscot with a gilded rail, coffered ceiling.
        add(upload(prim::box(vec3(5.0f, 2.2f, 0.1f)), "wall"), MaterialId::WallStone, translate(vec3(0.5f, 2.2f, -2.5f)));
        add(upload(prim::roundedBox(vec3(1.9f, 0.5f, 0.02f), 0.004f, 2), "wainscot"), MaterialId::WallPanelWood,
            translate(vec3(-2.3f, 0.5f, -2.38f)));
        MeshData rail = prim::lathe({{0.0f, -1.9f}, {0.026f, -1.9f}, {0.026f, -1.9f}, {0.026f, 1.9f}, {0.026f, 1.9f}, {0.0f, 1.9f}}, 48);
        add(upload(transformed(rail, rotateZ(PI * 0.5f)), "gildedRail"), MaterialId::GildedTrim, translate(vec3(-2.3f, 1.02f, -2.36f)));
        MeshData ceil = prim::plane(3.6f, 1.8f, 1, 1, 1.0f / 0.9f);
        add(upload(transformed(ceil, rotateX(PI)), "ceiling"), MaterialId::CeilingPainted, translate(vec3(0.3f, 3.2f, -1.4f)));

        // Metals: carved gilded capital, gilded ball, brass ball, brass candlestick with a candle,
        // clock lever / panel samples.
        MeshData capital = prim::lathe({{0.0f, 0.0f}, {0.14f, 0.0f}, {0.14f, 0.0f}, {0.14f, 0.02f}, {0.12f, 0.03f}, {0.15f, 0.05f},
                                        {0.12f, 0.07f}, {0.10f, 0.08f}, {0.10f, 0.12f}, {0.13f, 0.14f}, {0.10f, 0.16f},
                                        {0.08f, 0.20f}, {0.09f, 0.24f}, {0.14f, 0.26f}, {0.16f, 0.29f}, {0.16f, 0.29f},
                                        {0.16f, 0.32f}, {0.16f, 0.32f}, {0.0f, 0.32f}}, 96);
        add(upload(capital, "capital"), MaterialId::GildedTrim, translate(vec3(-2.7f, 0.0f, -1.3f)));
        Mesh* mball = upload(prim::sphere(0.13f, 96, 48), "metalBall");
        add(mball, MaterialId::GildedTrim, translate(vec3(-2.3f, 0.13f, -0.95f)));
        add(mball, MaterialId::Brass, translate(vec3(-1.95f, 0.13f, -0.95f)));
        MeshData stick = prim::lathe({{0.0f, 0.0f}, {0.075f, 0.0f}, {0.075f, 0.0f}, {0.075f, 0.012f}, {0.05f, 0.03f}, {0.02f, 0.06f},
                                      {0.016f, 0.2f}, {0.026f, 0.22f}, {0.014f, 0.24f}, {0.014f, 0.3f}, {0.04f, 0.315f},
                                      {0.04f, 0.325f}, {0.012f, 0.33f}, {0.0f, 0.33f}}, 64);
        add(upload(stick, "candlestick"), MaterialId::Brass, translate(vec3(-1.7f, 0.0f, -1.35f)));
        add(upload(prim::cylinder(0.011f, 0.25f, 32), "candle"), MaterialId::CandleWax, translate(vec3(-1.7f, 0.33f, -1.35f)));
        add(upload(prim::roundedBox(vec3(0.12f, 0.08f, 0.01f), 0.003f, 3), "clockPanel"), MaterialId::ClockPanel,
            translate(vec3(-2.05f, 0.35f, -1.5f)));
        add(upload(prim::roundedBox(vec3(0.03f, 0.012f, 0.012f), 0.006f, 4), "lever"), MaterialId::ClockLever,
            translate(vec3(-2.05f, 0.29f, -1.47f)));

        // Glass: a window (painted frame, old glass in 2x2 panes) and crystal drops.
        {
            const float W = 1.2f, H = 1.8f, b = 0.07f;
            MeshData frame;
            frame.append(transformed(prim::box(vec3(W * 0.5f, b * 0.5f, 0.04f)), translate(vec3(0, b * 0.5f, 0))));
            frame.append(transformed(prim::box(vec3(W * 0.5f, b * 0.5f, 0.04f)), translate(vec3(0, H - b * 0.5f, 0))));
            frame.append(transformed(prim::box(vec3(b * 0.5f, H * 0.5f, 0.04f)), translate(vec3(-W * 0.5f + b * 0.5f, H * 0.5f, 0))));
            frame.append(transformed(prim::box(vec3(b * 0.5f, H * 0.5f, 0.04f)), translate(vec3(W * 0.5f - b * 0.5f, H * 0.5f, 0))));
            frame.append(transformed(prim::box(vec3(0.02f, H * 0.5f - b, 0.03f)), translate(vec3(0, H * 0.5f, 0))));
            frame.append(transformed(prim::box(vec3(W * 0.5f - b, 0.02f, 0.03f)), translate(vec3(0, H * 0.5f, 0))));
            add(upload(frame, "windowFrame"), MaterialId::WindowFrame, translate(vec3(-3.4f, 0.3f, -2.0f)));
            add(upload(transformed(wallPlane(W - 2 * b, H - 2 * b), translate(vec3(0, b, 0))), "windowGlass"), MaterialId::WindowGlass,
                translate(vec3(-3.4f, 0.3f, -2.0f)), vec4(2, 2, 0, 0));
        }
        MeshData drop = faceted(prim::lathe({{0.0f, -0.07f}, {0.02f, -0.035f}, {0.03f, 0.0f}, {0.0f, 0.03f}}, 8));
        Mesh* dropM = upload(drop, "crystalDrop");
        for (int k = 0; k < 5; ++k)
            add(dropM, MaterialId::Crystal, translate(vec3(-2.6f + 0.12f * float(k), 1.35f - 0.05f * float(k & 1), -1.7f)), vec4(0));
        add(upload(faceted(prim::sphere(0.09f, 12, 6)), "crystalBall"), MaterialId::Crystal, translate(vec3(-2.0f, 1.2f, -1.7f)));

        // Marble shader balls in front of the table.
        Mesh* ball = upload(prim::sphere(0.12f, 96, 48), "ball");
        const MaterialId balls[] = {MaterialId::MarbleWhitePiece, MaterialId::MarbleBlackPiece, MaterialId::BoardFrame,
                                    MaterialId::BoardSquareLight};
        for (int i = 0; i < 4; ++i)
            add(ball, balls[i], translate(vec3(-0.45f + 0.3f * float(i), 0.12f, 1.25f)), vec4(float(40 + i), 0, 0, 0));
        // Flat sample slabs (read the pattern like a photo of a slab).
        Mesh* slab = upload(prim::roundedBox(vec3(0.19f, 0.01f, 0.19f), 0.003f, 2), "slab");
        const MaterialId slabs[] = {MaterialId::MarbleWhitePiece, MaterialId::MarbleBlackPiece, MaterialId::BoardFrame,
                                    MaterialId::BoardSquareLight};
        for (int i = 0; i < 4; ++i) {
            // Untessellated copies (Phong tessellation would inflate the flat slabs).
            slabMats_[i] = materials::get(slabs[i]);
            slabMats_[i].tessellated = false;
            extra_.push_back({slab, &slabMats_[i], translate(vec3(-0.6f + 0.4f * float(i), 0.01f, 2.75f)), vec4(float(50 + i), 0, 0, 0)});
        }
        // Floor swatch (1.6 x 0.8 m) behind the slabs.
        slabMats_[4] = materials::get(MaterialId::FloorMarble);
        slabMats_[4].planarReflector = -1;
        extra_.push_back({upload(prim::plane(1.6f, 0.8f, 1, 1, 1.0f), "floorSwatch"), &slabMats_[4], translate(vec3(0.0f, 0.001f, 3.45f)), vec4(0)});
    }

    std::vector<Mesh*> meshes_;
    std::vector<Obj> objs_;
    struct Extra { Mesh* mesh; const Material* mat; mat4 model; vec4 inst0; };
    std::vector<Extra> extra_;
    Material slabMats_[5];
    bool albedoDebug_ = false;
    OrbitCamera cam_;
    render::Environment env_;
    int floorPlanar_ = -1;
    float time_ = 0.0f;
};

SCACELITH_SCENE("materials", "Material library viewer (shader balls, --view marble|wood|fabric|metal|glass|...)", MaterialsViewer);
