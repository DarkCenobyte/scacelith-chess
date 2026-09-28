// "anim" viewer scene: two debug robots (capsules generated from the Skeleton) seated at the
// table, placeholder board/pieces/clock and a scripted timeline exercising every hand task:
//   0.5 s  handshake (both players)
//   3.6 s  White e2-e4 (Reach Lift Carry Place, PressClock, Retract)
//          Black d7-d5 0.7 s after White's clock press (the AI 'thinks' in between)
//          White exd5 0.7 s after Black's clock press (Reach Lift Carry TakeCaptured Place
//          Discard PressClock Retract)
// Command line (after --scene anim):
//   --time t        simulate 0..t with fixed 1/120 s steps, then (in --shot mode) freeze
//   --view v        side | sidel | front | back | top | white | black | hand | handb | handl | shake | orbit |
//                   pinch | pinchs | pinchb | pinchbs (close-ups of White's / Black's right fingers from the
//                   front and from the right side)
//   --selftest      numeric checks of the IK/grasp/timing (results in the log)
// Keys: Space pause, R restart, V next view, S slow motion, arrows = player's head (White).
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../core/log.h"
#include "../game/layout.h"
#include "../render/mesh.h"
#include "animator.h"
#include <cstdlib>
#include <cstring>
#include <string>

using namespace m;
using namespace character;

namespace {

// ---------------------------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------------------------
MeshData capsule(vec3 a, vec3 b, float r0, float r1, int seg = 14) {
    vec3 d = b - a;
    float L = length(d);
    std::vector<vec2> prof;
    const int n = 6;
    for (int i = 0; i <= n; ++i) {
        float th = -0.5f * PI + 0.5f * PI * float(i) / n;
        prof.push_back({r0 * std::cos(th), r0 * std::sin(th)});
    }
    for (int i = 0; i <= n; ++i) {
        float th = 0.5f * PI * float(i) / n;
        prof.push_back({r1 * std::cos(th), L + r1 * std::sin(th)});
    }
    prof.front().x = 0.0f;
    prof.back().x = 0.0f;
    MeshData md = prim::lathe(prof, seg);
    quat q = L > 1e-6f ? fromTo(vec3(0, 1, 0), d / L) : quat();
    md.transform(toMat4(q, a));
    return md;
}
MeshData ellipsoid(vec3 c, vec3 r) {
    MeshData s = prim::sphere(1.0f, 32, 16);
    s.transform(translate(c) * scale(r));
    return s;
}
MeshData rbox(vec3 c, vec3 h, float rad) {
    MeshData b = prim::roundedBox(h, rad, 3);
    b.transform(translate(c));
    return b;
}
// Spherical patch (eyelids): elevation [e0,e1], azimuth [-az, az] around +Z.
MeshData spherePatch(float r, float e0, float e1, float az, int nu = 16, int nv = 8) {
    MeshData d;
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i) {
            float el = lerp(e0, e1, float(j) / nv), a = lerp(-az, az, float(i) / nu);
            vec3 n(std::cos(el) * std::sin(a), std::sin(el), std::cos(el) * std::cos(a));
            Vertex v;
            v.pos = n * r;
            v.normal = n;
            v.tangent = vec4(1, 0, 0, 1);
            v.uv = vec2(float(i) / nu, float(j) / nv);
            d.vertices.push_back(v);
        }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            uint32_t a = uint32_t(j * (nu + 1) + i), b = a + 1, c = a + uint32_t(nu + 1), e = c + 1;
            for (uint32_t k : {a, e, b, a, c, e}) d.indices.push_back(k);
        }
    return d;
}
MeshData pieceMesh(int t) {
    float h = layout::PIECE_HEIGHT[t], rb = layout::PIECE_BASE_RADIUS[t];
    float gh = layout::PIECE_GRIP_HEIGHT[t] * h, gr = layout::PIECE_GRIP_RADIUS[t];
    float head = t == 1 ? rb * 0.72f : rb * 0.66f;
    std::vector<vec2> p = {{0, 0}, {rb, 0}, {rb, 0}, {rb, 0.09f * h}, {rb * 0.80f, 0.18f * h}, {gr * 1.35f, gh - 0.20f * h}, {gr, gh},
                           {gr * 1.25f, gh + 0.05f * h}, {head, gh + 0.5f * (h - gh)}, {head * 0.55f, h * 0.975f}, {0, h}};
    if (t == 4) p = {{0, 0}, {rb, 0}, {rb, 0}, {rb, 0.09f * h}, {rb * 0.82f, 0.2f * h}, {gr, 0.45f * h}, {gr, gh}, {rb * 0.9f, gh + 0.06f * h},
                     {rb * 0.9f, h}, {rb * 0.9f, h}, {0, h}};
    return prim::lathe(p, 32);
}

Material mat(vec3 albedo, float rough, float metal = 0.0f, float clearcoat = 0.0f) {
    Material m;
    m.params[0] = vec4(albedo, rough);
    m.params[1] = vec4(metal, 0.5f, clearcoat, 0.08f);
    return m;
}

// ---------------------------------------------------------------------------------------------
// Debug robot: one mesh per bone in bone-local space
// ---------------------------------------------------------------------------------------------
struct DebugBody {
    Mesh part[BoneCount];
    bool has[BoneCount] = {};
    Mesh pupil;

    void build(const Skeleton& sk) {
        auto set = [&](Bone b, const MeshData& d) {
            part[b].upload(d, boneName(b));
            has[b] = true;
        };
        auto tipOf = [&](Bone b) { return normalize(sk.restOffset[b]) * sk.boneLength[b]; };
        set(Pelvis, rbox({0, 0.015f, -0.015f}, {0.150f, 0.070f, 0.105f}, 0.05f));
        set(Spine1, rbox({0, 0.075f, -0.005f}, {0.130f, 0.085f, 0.092f}, 0.06f));
        {
            MeshData chest = rbox({0, 0.135f, -0.010f}, {0.160f, 0.125f, 0.100f}, 0.07f);
            set(Spine2, chest);
        }
        set(Neck, capsule({0, 0, 0}, sk.restOffset[Head], 0.043f, 0.040f));
        set(Head, ellipsoid({0, 0.080f, 0.0f}, {0.077f, 0.108f, 0.087f}));
        set(EyeL, prim::sphere(0.0115f, 20, 10));
        set(EyeR, prim::sphere(0.0115f, 20, 10));
        MeshData up = spherePatch(0.0128f, 0.44f, 1.75f, 1.25f), lo = spherePatch(0.0128f, -1.75f, -0.62f, 1.25f);
        set(LidUpperL, up);
        set(LidUpperR, up);
        set(LidLowerL, lo);
        set(LidLowerR, lo);
        {
            MeshData p = prim::sphere(0.0050f, 12, 6);
            p.transform(translate({0, 0, 0.0078f}));
            pupil.upload(p, "pupil");
        }
        for (int s = 0; s < 2; ++s) {
            Side side = s == 0 ? Side::Left : Side::Right;
            float ps = side == Side::Right ? 1.0f : -1.0f;
            auto B = [&](Bone l) { return sideBone(l, side); };
            set(B(ClavicleL), capsule({0, 0, 0}, sk.restOffset[B(UpperArmL)], 0.034f, 0.040f));
            set(B(UpperArmL), capsule({0, 0, 0}, sk.restOffset[B(ForeArmL)], 0.043f, 0.036f));
            set(B(ForeArmL), capsule({0, 0, 0}, sk.restOffset[B(HandL)], 0.035f, 0.025f));
            set(B(HandL), rbox({0.0f, -0.047f, 0.002f}, {0.0135f, 0.042f, 0.037f}, 0.010f));
            (void)ps;
            const float rad[5][3] = {{0.0105f, 0.0092f, 0.0082f}, {0.0082f, 0.0075f, 0.0068f}, {0.0086f, 0.0078f, 0.0070f},
                                     {0.0080f, 0.0073f, 0.0066f}, {0.0072f, 0.0066f, 0.0060f}};
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 3; ++j) {
                    Bone b = Bone(B(ThumbL1) + f * 3 + j);
                    vec3 end = j < 2 ? sk.restOffset[b + 1] : tipOf(b);
                    float r1 = j < 2 ? rad[f][j + 1] : rad[f][j] * 0.92f;
                    set(b, capsule({0, 0, 0}, end, rad[f][j], r1, 10));
                }
        }
        for (int s = 0; s < 2; ++s) {
            Bone t = s == 0 ? ThighL : ThighR;
            set(t, capsule({0, 0, 0}, sk.restOffset[t + 1], 0.070f, 0.055f));
            set(Bone(t + 1), capsule({0, 0, 0}, sk.restOffset[t + 2], 0.050f, 0.040f));
            set(Bone(t + 2), rbox({0, -0.035f, 0.065f}, {0.045f, 0.030f, 0.115f}, 0.025f));
        }
    }
    void submit(render::Renderer& r, const mat4* g, const Material& body, const Material& eye, const Material& dark, uint32_t id,
                bool hideHead) {
        for (int b = 0; b < BoneCount; ++b) {
            if (!has[b]) continue;
            render::DrawItem d;
            d.mesh = &part[b];
            d.material = (b == EyeL || b == EyeR) ? &eye : &body;
            d.model = g[b];
            d.objectId = id + uint32_t(b);
            bool headPart = b == Head || b == Neck || (b >= EyeL && b <= LidLowerR);
            if (hideHead && headPart) d.flags |= render::DRAW_HIDDEN_MAIN;
            r.submit(d);
        }
        for (Bone e : {EyeL, EyeR}) {
            render::DrawItem d;
            d.mesh = &pupil;
            d.material = &dark;
            d.model = g[e];
            d.objectId = id + 100 + uint32_t(e);
            if (hideHead) d.flags |= render::DRAW_HIDDEN_MAIN;
            r.submit(d);
        }
    }
};

// ---------------------------------------------------------------------------------------------
struct Piece {
    int type = 1, color = 0;
    mat4 xf;
    int heldBy = -1;   // animator index
};
// The 32 pieces plus the two spare queens beside the board (as in the game).
constexpr int kPieces = 34;

// Starting position: ids 0-7 White pawns a-h, 8-15 White back rank, 16-23 Black pawns, 24-31
// Black back rank, 32/33 the White/Black spare queens where the game keeps them (clock at +X:
// beside the board at -X, near their owner).
void initialPieces(Piece out[kPieces]) {
    static const int back[8] = {4, 2, 3, 5, 6, 3, 2, 4};
    for (int i = 32; i < kPieces; ++i) {
        out[i].type = 5;
        out[i].color = i - 32;
        out[i].heldBy = -1;
        out[i].xf = translate(vec3(-(layout::BOARD_SIZE * 0.5f + 0.05f), layout::TABLE_TOP_Y, i == 32 ? 0.33f : -0.33f)) * rotateY(i == 32 ? 0.0f : PI);
    }
    for (int i = 0; i < 32; ++i) {
        Piece& p = out[i];
        int color = i >= 16 ? 1 : 0, k = i & 15;
        int file = k & 7, rank = color == 0 ? (k < 8 ? 1 : 0) : (k < 8 ? 6 : 7);
        p.type = k < 8 ? 1 : back[file];
        p.color = color;
        p.heldBy = -1;
        p.xf = translate(layout::squareCenter(file, rank)) * rotateY(color ? PI : 0.0f);
    }
}

// Obstacle queries run while an animator plans a task inside update(), before the game has seen
// that call's events: a piece the animator has just gripped still looks free here, so ask the
// animators (holding) as well. Pieces it has just released are known to the animator itself.
bool heldNow(const Piece* ps, int i, const anim::Animator* h0, const anim::Animator* h1) {
    return ps[i].heldBy >= 0 || (h0 && h0->holding(i)) || (h1 && h1->holding(i));
}

// Highest piece top close to the segment from-to (pieces standing on the board or the table).
float piecesTopNear(const Piece* ps, vec3 from, vec3 to, const anim::Animator* h0 = nullptr, const anim::Animator* h1 = nullptr) {
    float top = layout::BOARD_TOP_Y;
    vec3 d(to.x - from.x, 0, to.z - from.z);
    float len2 = std::max(1e-8f, length2(d));
    for (int i = 0; i < kPieces; ++i) {
        if (heldNow(ps, i, h0, h1)) continue;
        vec3 p = ps[i].xf.translation();
        if (p.y > layout::BOARD_TOP_Y + 0.01f) continue;
        float s = clamp(dot(vec3(p.x - from.x, 0, p.z - from.z), d) / len2, 0.0f, 1.0f);
        vec3 c = from + d * s;
        if (length(vec3(p.x - c.x, 0, p.z - c.z)) < layout::PIECE_BASE_RADIUS[ps[i].type] + 0.028f)
            top = std::max(top, p.y + layout::PIECE_HEIGHT[ps[i].type]);
    }
    return top;
}

// Highest top of the standing pieces whose base comes within 'radius' of p (horizontally).
float piecesTopAt(const Piece* ps, vec3 p, float radius, int ignoreId, const anim::Animator* h0 = nullptr, const anim::Animator* h1 = nullptr) {
    float top = layout::BOARD_TOP_Y;
    for (int i = 0; i < kPieces; ++i) {
        if (i == ignoreId || heldNow(ps, i, h0, h1)) continue;
        vec3 c = ps[i].xf.translation();
        if (c.y > layout::BOARD_TOP_Y + 0.01f) continue;
        if (length(vec3(p.x - c.x, 0, p.z - c.z)) < layout::PIECE_BASE_RADIUS[ps[i].type] + radius)
            top = std::max(top, c.y + layout::PIECE_HEIGHT[ps[i].type]);
    }
    return top;
}

// Deepest overlap of the finger joints / tips (7 mm) and the forearms (3 cm) with the standing
// pieces (base + body profile). Pieces skipA/skipB are ignored.
struct Overlap {
    float depth = 0.0f;
    int bone = 0, piece = -1;
};
// skip(piece, forearm): pieces the hand may touch (the ones it grips) are skipped for the hand
// and fingers only; nothing excuses the forearm.
template <class Skip>
Overlap pieceOverlapIf(const Skeleton& sk, const mat4* g, const Piece* ps, Skip skip) {
    Overlap o;
    auto test = [&](vec3 p, float rad, int bone) {
        const bool arm = bone == ForeArmL || bone == ForeArmR;
        for (int i = 0; i < kPieces; ++i) {
            if (ps[i].heldBy >= 0 || skip(i, arm)) continue;
            // Staunton-like profile: full base up to 12% of the height, then a waist/body about
            // 60% of the base radius up to the top.
            vec3 c = ps[i].xf.translation();
            float H = layout::PIECE_HEIGHT[ps[i].type], rb = layout::PIECE_BASE_RADIUS[ps[i].type];
            float yRel = p.y - c.y;
            float r = (yRel < 0.12f * H + rad ? rb * 0.95f : rb * 0.62f) + rad;
            float top = c.y + H + rad;
            float depth = std::min(r - length(vec3(p.x - c.x, 0, p.z - c.z)), top - p.y);
            if (depth > o.depth) { o.depth = depth; o.bone = bone; o.piece = i; }
        }
    };
    for (int b = HandL; b <= PinkyR3; ++b) {
        if (b > PinkyL3 && b < HandR) continue;
        bool hand = b == HandL || b == HandR;
        bool distal = !hand && (b - (b >= HandR ? HandR : HandL)) % 3 == 0;
        vec3 dir = hand ? sk.restOffset[b + 7] : distal ? sk.restOffset[b] : sk.restOffset[b + 1];
        test(g[b].translation(), 0.007f, b);
        test(transformPoint(g[b], normalize(dir) * sk.boneLength[b]), 0.007f, b);
    }
    for (Bone fb : {ForeArmL, ForeArmR})
        for (int k = 0; k <= 6; ++k)
            test(g[fb].translation() + (g[fb + 1].translation() - g[fb].translation()) * (0.35f + 0.65f * float(k) / 6.0f), 0.030f, fb);
    return o;
}
Overlap pieceOverlap(const Skeleton& sk, const mat4* g, const Piece* ps, int skipA, int skipB) {
    return pieceOverlapIf(sk, g, ps, [&](int i, bool) { return i == skipA || i == skipB; });
}

const char* kViews[] = {"side", "sidel", "front", "back", "top", "white", "black", "hand", "handb", "handl", "shake", "orbit",
                        "pinch", "pinchs", "pinchb", "pinchbs"};
constexpr int kViewCount = int(sizeof(kViews) / sizeof(kViews[0]));
constexpr int kOrbitView = 11;

class AnimViewer : public Scene {
public:
    bool init(AppContext& ctx) override {
        sk_ = &robotSkeleton();
        body_.build(*sk_);
        buildProps();
        std::string v = ctx.argValue("--view", "side");
        for (int i = 0; i < kViewCount; ++i)
            if (v == kViews[i]) view_ = i;
        if (ctx.hasArg("--selftest")) { selfTest(); timelineCheck(); }
        solo_ = ctx.hasArg("--solo");
        reset();
        float t0 = ctx.fixedTime >= 0 ? ctx.fixedTime : 0.0f;
        simulateTo(t0);
        frozen_ = ctx.screenshotMode && ctx.fixedTime >= 0;
        orbit_.target = vec3(0, 0.95f, 0);
        orbit_.distance = 1.9f;
        orbit_.yaw = 1.2f;
        orbit_.pitch = 0.35f;
        return true;
    }

    bool update(AppContext& ctx, float dt) override {
        const plat::Input& in = plat::input();
        if (in.keyPressed[plat::KEY_SPACE]) paused_ = !paused_;
        if (in.keyPressed['R']) { reset(); }
        if (in.keyPressed['V']) view_ = (view_ + 1) % kViewCount;
        if (in.keyPressed['S']) slow_ = !slow_;
        if (view_ == kOrbitView) orbit_.update(in);
        if (!frozen_ && !paused_) simulateTo(simTime_ + (slow_ ? dt * 0.2f : dt));
        (void)ctx;
        return !in.keyPressed[plat::KEY_ESCAPE];
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env;
        env.time = simTime_;
        env.sunDirection = normalize(vec3(-0.55f, 0.62f, 0.25f));
        render::Camera cam = camera();
        r.beginFrame(cam, env, dt);
        auto draw = [&](const Mesh& mesh, const Material& m, const mat4& model, uint32_t id, uint32_t flags = render::DRAW_CAST_SHADOW) {
            render::DrawItem d;
            d.mesh = &mesh;
            d.material = &m;
            d.model = model;
            d.objectId = id;
            d.flags = flags;
            r.submit(d);
        };
        draw(floor_, floorMat_, mat4(), 1, render::DRAW_STATIC);
        draw(table_, tableMat_, mat4(), 2, render::DRAW_STATIC | render::DRAW_CAST_SHADOW);
        draw(boardLight_, boardLightMat_, mat4(), 3, render::DRAW_STATIC | render::DRAW_CAST_SHADOW);
        draw(boardDark_, boardDarkMat_, mat4(), 4, render::DRAW_STATIC);
        draw(chairs_, tableMat_, mat4(), 5, render::DRAW_STATIC | render::DRAW_CAST_SHADOW);
        draw(clock_, clockMat_, mat4(), 6);
        for (int i = 0; i < 2; ++i) {
            float down = (clockSide_ == i) ? -0.004f : 0.0f;
            draw(button_, buttonMat_, translate(vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + down, i == 0 ? 0.045f : -0.045f)), 7 + uint32_t(i));
        }
        for (int i = 0; i < kPieces; ++i) {
            if (solo_ && !nearHand(i)) continue;
            draw(pieceMesh_[pieces_[i].type], pieces_[i].color ? pieceBlackMat_ : pieceWhiteMat_, pieces_[i].xf, 100 + uint32_t(i));
        }
        body_.submit(r, anim_[0].globals(), robotMat_[0], eyeMat_, darkMat_, 1000, view_ == 5);
        body_.submit(r, anim_[1].globals(), robotMat_[1], eyeMat_, darkMat_, 2000, view_ == 6);
        r.endFrame();
    }

private:
    // ---- scene props
    void buildProps() {
        floor_.upload(prim::plane(8, 8), "floor");
        MeshData t = rbox({0, layout::TABLE_TOP_Y - layout::TABLE_TOP_THICKNESS * 0.5f, 0},
                          {layout::TABLE_WIDTH * 0.5f, layout::TABLE_TOP_THICKNESS * 0.5f, layout::TABLE_DEPTH * 0.5f}, 0.006f);
        for (int i = 0; i < 4; ++i) {
            float x = (i & 1 ? 1.0f : -1.0f) * (layout::TABLE_WIDTH * 0.5f - 0.06f), z = (i & 2 ? 1.0f : -1.0f) * (layout::TABLE_DEPTH * 0.5f - 0.06f);
            t.append(prim::box({0.03f, layout::TABLE_TOP_Y * 0.5f, 0.03f}), translate({x, layout::TABLE_TOP_Y * 0.5f, z}));
        }
        table_.upload(t, "table");
        float hb = layout::BOARD_SIZE * 0.5f;
        boardLight_.upload(rbox({0, layout::TABLE_TOP_Y + layout::BOARD_THICKNESS * 0.5f, 0}, {hb, layout::BOARD_THICKNESS * 0.5f, hb}, 0.002f), "board");
        MeshData dark;
        for (int sq = 0; sq < 64; ++sq) {
            int f = sq & 7, rk = sq >> 3;
            if (((f + rk) & 1) != 0) continue;   // a1 is dark
            vec3 c = layout::squareCenter(sq);
            dark.append(prim::box({layout::SQUARE_SIZE * 0.5f, 0.0004f, layout::SQUARE_SIZE * 0.5f}), translate(c));
        }
        boardDark_.upload(dark, "board-dark");
        MeshData ch;
        for (int s = 0; s < 2; ++s) {
            float z = (s == 0 ? 1.0f : -1.0f) * layout::CHAIR_Z;
            ch.append(rbox({0, layout::SEAT_HEIGHT - 0.03f, z}, {0.22f, 0.03f, 0.21f}, 0.01f));
            ch.append(rbox({0, layout::SEAT_HEIGHT * 0.5f - 0.03f, z}, {0.03f, layout::SEAT_HEIGHT * 0.5f - 0.03f, 0.03f}, 0.005f));
            ch.append(rbox({0, layout::SEAT_HEIGHT + 0.27f, z + (s == 0 ? 0.21f : -0.21f)}, {0.21f, 0.24f, 0.02f}, 0.01f));
        }
        chairs_.upload(ch, "chairs");
        clock_.upload(rbox({layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT * 0.5f, layout::CLOCK_Z},
                           {layout::CLOCK_DEPTH * 0.5f, layout::CLOCK_HEIGHT * 0.5f, layout::CLOCK_WIDTH * 0.5f}, 0.006f),
                      "clock");
        button_.upload(prim::cylinder(0.012f, 0.005f, 24), "button");
        for (int t2 = 1; t2 <= 6; ++t2) pieceMesh_[t2].upload(pieceMesh(t2), "piece");
        floorMat_ = mat(vec3(0.42f, 0.40f, 0.38f), 0.7f);
        tableMat_ = mat(vec3(0.30f, 0.17f, 0.09f), 0.45f, 0.0f, 0.6f);
        boardLightMat_ = mat(vec3(0.80f, 0.74f, 0.62f), 0.35f, 0.0f, 0.5f);
        boardDarkMat_ = mat(vec3(0.24f, 0.15f, 0.09f), 0.35f, 0.0f, 0.5f);
        clockMat_ = mat(vec3(0.05f, 0.05f, 0.05f), 0.4f);
        buttonMat_ = mat(vec3(0.8f, 0.7f, 0.4f), 0.3f, 1.0f);
        pieceWhiteMat_ = mat(vec3(0.90f, 0.87f, 0.80f), 0.35f, 0.0f, 0.8f);
        pieceBlackMat_ = mat(vec3(0.03f, 0.03f, 0.03f), 0.30f, 0.0f, 0.8f);
        robotMat_[0] = mat(vec3(0.88f, 0.87f, 0.84f), 0.28f, 0.0f, 1.0f);
        robotMat_[1] = mat(vec3(0.70f, 0.74f, 0.80f), 0.28f, 0.0f, 1.0f);
        eyeMat_ = mat(vec3(0.95f, 0.95f, 0.95f), 0.1f, 0.0f, 1.0f);
        darkMat_ = mat(vec3(0.02f, 0.03f, 0.05f), 0.15f, 0.0f, 1.0f);
        for (auto* mm : {&robotMat_[0], &robotMat_[1]}) mm->doubleSided = true;
    }

    // ---- simulation
    void reset() {
        initialPieces(pieces_);
        const float pz = layout::PLAYER_PELVIS_Z, py = layout::PLAYER_PELVIS_Y;
        anim_[0].init(*sk_, vec3(0, py, pz), 1.0f);
        anim_[1].init(*sk_, vec3(0, py, -pz), -1.0f);
        for (int a = 0; a < 2; ++a) {
            anim_[a].pieceTransform = [this](int id) { return id >= 0 && id < kPieces ? pieces_[id].xf : mat4(); };
            anim_[a].pieceGripInfo = [this](int id) {
                int t = id >= 0 && id < kPieces ? pieces_[id].type : 1;
                return vec3(layout::PIECE_HEIGHT[t], layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
            };
            // Highest piece top close to the carried piece's path (pieces standing on the board).
            anim_[a].pathObstacleTop = [this](vec3 from, vec3 to) { return piecesTopNear(pieces_, from, to, &anim_[0], &anim_[1]); };
            anim_[a].obstacleTopNear = [this](vec3 p, float r, int ignore) { return piecesTopAt(pieces_, p, r, ignore, &anim_[0], &anim_[1]); };
        }
        simTime_ = 0.0f;
        stage_ = 0;
        nextAt_ = 0.5f;
        clockSide_ = -1;
        headYaw_ = 0.0f;
        headPitch_ = -0.55f;
    }
    // --solo: only pieces held or within 6 cm of a right index fingertip are drawn.
    bool nearHand(int i) const {
        if (pieces_[i].heldBy >= 0) return true;
        vec3 p = pieces_[i].xf.translation();
        for (int a = 0; a < 2; ++a) {
            vec3 f = anim_[a].globals()[IndexR3].translation();
            if (length(vec3(p.x - f.x, 0, p.z - f.z)) < 0.06f) return true;
        }
        return false;
    }
    static int sq(const char* s) { return (s[0] - 'a') + (s[1] - '1') * 8; }
    int pieceAt(int square) const {
        vec3 c = layout::squareCenter(square);
        for (int i = 0; i < kPieces; ++i) {
            vec3 p = pieces_[i].xf.translation();
            if (pieces_[i].heldBy < 0 && std::fabs(p.x - c.x) < 0.01f && std::fabs(p.z - c.z) < 0.01f && p.y < layout::BOARD_TOP_Y + 0.01f) return i;
        }
        return -1;
    }
    vec3 pressPoint(int player) const {
        return vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, player == 0 ? 0.045f : -0.045f);
    }
    void quietMove(int player, const char* from, const char* to) {
        using namespace anim;
        int id = pieceAt(sq(from));
        vec3 dst = layout::squareCenter(sq(to));
        std::vector<Task> ts;
        Task t;
        t.type = TaskType::Reach; t.pieceId = id; ts.push_back(t);
        t = Task(); t.type = TaskType::Lift; ts.push_back(t);
        t = Task(); t.type = TaskType::Carry; t.position = dst; ts.push_back(t);
        t = Task(); t.type = TaskType::Place; t.position = dst; ts.push_back(t);
        t = Task(); t.type = TaskType::PressClock; t.position = pressPoint(player); ts.push_back(t);
        t = Task(); t.type = TaskType::Retract; ts.push_back(t);
        anim_[player].enqueue(ts);
        LOGI("anim viewer: %s %s-%s (piece %d) at t=%.3f", player ? "Black" : "White", from, to, id, simTime_);
    }
    void captureMove(int player, const char* from, const char* to) {
        using namespace anim;
        int id = pieceAt(sq(from)), victim = pieceAt(sq(to));
        vec3 dst = layout::squareCenter(sq(to));
        float sideSign = player == 0 ? -1.0f : 1.0f;   // captured pieces on the clock-free side
        vec3 spot(sideSign * layout::CAPTURE_ROW_X, layout::TABLE_TOP_Y, (player == 0 ? 1.0f : -1.0f) * 0.22f);
        std::vector<Task> ts;
        Task t;
        t.type = TaskType::Reach; t.pieceId = id; ts.push_back(t);
        t = Task(); t.type = TaskType::Lift; ts.push_back(t);
        t = Task(); t.type = TaskType::Carry; t.position = dst; ts.push_back(t);
        t = Task(); t.type = TaskType::TakeCaptured; t.pieceId = victim; ts.push_back(t);
        t = Task(); t.type = TaskType::Place; t.position = dst; ts.push_back(t);
        t = Task(); t.type = TaskType::Discard; t.position = spot; ts.push_back(t);
        t = Task(); t.type = TaskType::PressClock; t.position = pressPoint(player); ts.push_back(t);
        t = Task(); t.type = TaskType::Retract; ts.push_back(t);
        anim_[player].enqueue(ts);
        LOGI("anim viewer: %s %sx%s (piece %d takes %d) at t=%.3f", player ? "Black" : "White", from, to, id, victim, simTime_);
    }
    void script() {
        using namespace anim;
        if (stage_ == 0 && simTime_ >= nextAt_) {
            Task h;
            h.type = TaskType::Handshake;
            h.partner = &anim_[1];
            anim_[0].enqueue(h);
            h.partner = &anim_[0];
            anim_[1].enqueue(h);
            LOGI("anim viewer: handshake at t=%.3f", simTime_);
            stage_ = 1;
            nextAt_ = simTime_ + Timing::Handshake + 0.5f;
        } else if (stage_ == 1 && simTime_ >= nextAt_) {
            quietMove(0, "e2", "e4");
            stage_ = 2;
            nextAt_ = 1e9f;
        } else if (stage_ == 3 && simTime_ >= nextAt_) {
            quietMove(1, "d7", "d5");
            stage_ = 4;
            nextAt_ = 1e9f;
        } else if (stage_ == 5 && simTime_ >= nextAt_) {
            captureMove(0, "e4", "d5");
            stage_ = 6;
            nextAt_ = 1e9f;
        }
    }
    void handleEvents(int a, const std::vector<anim::Event>& ev) {
        using namespace anim;
        for (auto& e : ev) {
            switch (e.type) {
                case EventType::PieceGripped:
                case EventType::CapturedGripped:
                    if (e.pieceId >= 0) pieces_[e.pieceId].heldBy = a;
                    break;
                case EventType::PieceReleased:
                case EventType::CapturedReleased:
                    if (e.pieceId >= 0) {
                        pieces_[e.pieceId].heldBy = -1;
                        pieces_[e.pieceId].xf = e.transform;
                    }
                    break;
                case EventType::ClockPressed:
                    clockSide_ = a;
                    LOGI("anim viewer: %s pressed the clock at t=%.4f", a ? "Black" : "White", e.time);
                    if (stage_ == 2 && a == 0) { stage_ = 3; nextAt_ = e.time + 0.7f; }
                    if (stage_ == 4 && a == 1) { stage_ = 5; nextAt_ = e.time + 0.7f; }
                    break;
                case EventType::HandshakeClasp:
                    LOGI("anim viewer: handshake clasp (%d) at t=%.4f", a, e.time);
                    break;
                default: break;
            }
        }
    }
    void step(float dt) {
        simTime_ += dt;
        script();
        // Black is the AI: watches White's hand while White plays, thinks otherwise.
        const mat4* wg = anim_[0].globals();
        if (anim_[0].busy() && stage_ >= 2) anim_[1].lookAt(wg[HandR].translation(), 1.0f);
        else anim_[1].lookAt(vec3(0, layout::BOARD_TOP_Y, 0), 0.0f);
        anim_[1].setThinking(stage_ >= 2 && !anim_[1].busy());
        // White is the first-person player: a virtual mouse follows the action.
        vec3 interest(0, layout::BOARD_TOP_Y, 0.02f);
        if (anim_[0].busy() && stage_ >= 2) interest = lerp(interest, wg[IndexR2].translation(), 0.75f);
        else if (anim_[1].busy() && stage_ >= 2) interest = lerp(interest, anim_[1].globals()[IndexR2].translation(), 0.6f);
        else if (stage_ == 1 || simTime_ < 3.3f) interest = anim_[1].eyeCameraTransform().translation();
        vec3 eye = anim_[0].eyeCameraTransform().translation();
        vec3 d = interest - eye;
        vec3 dc(-d.x, d.y, -d.z);   // White's character space (rotated 180 degrees about Y)
        float yaw = std::atan2(dc.x, dc.z), pitch = std::atan2(dc.y, length(vec3(dc.x, 0, dc.z)));
        float k = 1.0f - std::exp(-dt * 5.0f);
        headYaw_ += (yaw - headYaw_) * k;
        headPitch_ += (pitch - headPitch_) * k;
        anim_[0].setHeadOverride(true, headYaw_, headPitch_);
        for (int a = 0; a < 2; ++a) {
            std::vector<anim::Event> ev;
            anim_[a].update(dt, ev);
            handleEvents(a, ev);
        }
        for (int a = 0; a < 2; ++a)
            for (int i = 0; i < kPieces; ++i) {
                mat4 x;
                if (anim_[a].heldPieceTransform(i, x)) pieces_[i].xf = x;
            }
        if (collect_) collectStats(dt);
    }
    // ---- timeline statistics (--selftest)
    struct Stats {
        float minTip = 1e9f, minTipT = 0, minHeld = 1e9f, minHeldT = 0, maxSpeed = 0, maxSpeedT = 0, maxRoll = 0;
        int minTipBone = 0, maxSpeedBone = 0, minTipA = 0, maxSpeedA = 0;
        vec3 minTipP{0, 0, 0};
        int popLogs = 0, minHeldId = -1, maxPenBone = 0, maxPenPiece = -1, maxPenA = 0;
        float maxPen = 0.0f, maxPenT = 0.0f;
        quat prev[2][BoneCount];
        bool havePrev = false;
    } st_;
    bool collect_ = false;
    void collectStats(float dt) {
        const Skeleton& sk = *sk_;
        const float hb = layout::BOARD_SIZE * 0.5f;
        for (int a = 0; a < 2; ++a) {
            const mat4* g = anim_[a].globals();
            for (Side s : {Side::Left, Side::Right})
                for (int f = 0; f < 5; ++f)
                    for (int j = 0; j < 3; ++j) {
                        Bone b = Bone(sideBone(ThumbL1, s) + f * 3 + j);
                        vec3 p = transformPoint(g[b], normalize(sk.restOffset[j < 2 ? b + 1 : b]) * sk.boneLength[b]);
                        float surf = (std::fabs(p.x) < hb && std::fabs(p.z) < hb) ? layout::BOARD_TOP_Y
                                     : (std::fabs(p.x) < layout::TABLE_WIDTH * 0.5f && std::fabs(p.z) < layout::TABLE_DEPTH * 0.5f) ? layout::TABLE_TOP_Y
                                                                                                                                  : -1.0f;
                        float h = p.y - surf - 0.0065f;
                        if (h < st_.minTip) { st_.minTip = h; st_.minTipT = simTime_; st_.minTipBone = b; st_.minTipA = a; st_.minTipP = p; }
                    }
            // Hands and forearms against the standing pieces (the two pawns that move are
            // handled by the grip itself and skipped).
            Overlap o = pieceOverlap(sk, g, pieces_, 4, 19);
            if (o.depth > st_.maxPen) { st_.maxPen = o.depth; st_.maxPenT = simTime_; st_.maxPenBone = o.bone; st_.maxPenPiece = o.piece; st_.maxPenA = a; }
            if (o.depth > 0.003f && std::getenv("SCACELITH_TL_TRACE")) {
                vec3 bp = g[o.bone].translation(), pc = pieces_[o.piece].xf.translation();
                LOGI("timeline contact t=%.3f %s %s %.1f mm at %.3f %.3f %.3f, piece %d at %.3f %.3f", simTime_, a ? "Black" : "White", boneName(Bone(o.bone)),
                     o.depth * 1000.0f, bp.x, bp.y, bp.z, o.piece, pc.x, pc.z);
            }
            for (int i = 0; i < kPieces; ++i) {
                mat4 x;
                if (!anim_[a].heldPieceTransform(i, x)) continue;
                vec3 p = x.translation();
                float surf = (std::fabs(p.x) < hb && std::fabs(p.z) < hb) ? layout::BOARD_TOP_Y : layout::TABLE_TOP_Y;
                if (p.y - surf < st_.minHeld) { st_.minHeld = p.y - surf; st_.minHeldT = simTime_; st_.minHeldId = i; }
            }
            for (int b = 0; b < BoneCount; ++b) {
                if (b == EyeL || b == EyeR || (b >= LidUpperL && b <= LidLowerR)) continue;
                quat q = fromMat3(g[b].upper3());
                if (st_.havePrev && dt > 1e-4f) {
                    float d = std::fabs(dot(q, st_.prev[a][b]));
                    float ang = 2.0f * std::acos(clamp(d, 0.0f, 1.0f)) / dt;
                    if (ang > 1800.0f * DEG && st_.popLogs < 40) {
                        ++st_.popLogs;
                        LOGI("selftest pop: %s %s %.0f deg/s at t=%.3f", a ? "Black" : "White", boneName(Bone(b)), ang / DEG, simTime_);
                    }
                    if (ang > st_.maxSpeed) { st_.maxSpeed = ang; st_.maxSpeedT = simTime_; st_.maxSpeedBone = b; st_.maxSpeedA = a; }
                }
                st_.prev[a][b] = q;
            }
        }
        st_.havePrev = true;
        mat4 e = anim_[0].eyeCameraTransform();
        st_.maxRoll = std::max(st_.maxRoll, std::fabs(std::asin(clamp(transformDir(e, vec3(1, 0, 0)).y, -1.0f, 1.0f))));
    }
    void timelineCheck() {
        reset();
        st_ = Stats();
        collect_ = true;
        simulateTo(11.0f);
        collect_ = false;
        LOGI("selftest timeline: lowest fingertip %.1f mm (%s of %s at t=%.2f, %.3f %.3f %.3f), lowest held piece base %.1f mm (piece %d, t=%.2f)",
             st_.minTip * 1000.0f, boneName(Bone(st_.minTipBone)), st_.minTipA ? "Black" : "White", st_.minTipT, st_.minTipP.x, st_.minTipP.y,
             st_.minTipP.z, st_.minHeld * 1000.0f, st_.minHeldId, st_.minHeldT);
        LOGI("selftest timeline: deepest hand/piece overlap %.1f mm (%s of %s, piece %d, t=%.2f)", st_.maxPen * 1000.0f,
             boneName(Bone(st_.maxPenBone)), st_.maxPenA ? "Black" : "White", st_.maxPenPiece, st_.maxPenT);
        LOGI("selftest timeline: max bone angular speed %.0f deg/s (%s of %s at t=%.3f), max camera roll %.3f deg", st_.maxSpeed / DEG,
             boneName(Bone(st_.maxSpeedBone)), st_.maxSpeedA ? "Black" : "White", st_.maxSpeedT, st_.maxRoll / DEG);
    }
    void simulateTo(float t) {
        const float h = 1.0f / 120.0f;
        while (simTime_ + h <= t + 1e-6f) step(h);
        if (t - simTime_ > 1e-6f) step(t - simTime_);
    }

    render::Camera camera() const {
        render::Camera c;
        c.fovY = 42.0f * DEG;
        c.nearZ = 0.02f;
        auto look = [&](vec3 pos, vec3 target, float fov) {
            c.position = pos;
            c.fovY = fov * DEG;
            c.lookAt(target);
        };
        switch (view_) {
            case 0: look({1.50f, 1.18f, 0.0f}, {0, 0.93f, 0}, 44); break;
            case 1: look({-1.50f, 1.18f, 0.0f}, {0, 0.93f, 0}, 44); break;
            case 2: look({0.35f, 1.35f, -1.75f}, {0, 0.95f, 0.25f}, 40); break;
            case 3: look({-0.35f, 1.35f, 1.75f}, {0, 0.95f, -0.25f}, 40); break;
            case 4: look({0.0f, 2.3f, 0.02f}, {0, 0.8f, 0}, 40); break;
            case 5:
            case 6: {
                mat4 e = anim_[view_ - 5].eyeCameraTransform();
                c.position = e.translation();
                c.orientation = fromMat3(e.upper3());
                c.fovY = 55.0f * DEG;
                c.nearZ = 0.02f;
                break;
            }
            case 7:
            case 8:
            case 9: {
                // Close-up of a right hand: from its right-front (hand, handb) or left-front (handl).
                int a = view_ == 8 ? 1 : 0;
                const mat4* g = anim_[a].globals();
                vec3 hp = (g[IndexR3].translation() + g[ThumbR3].translation()) * 0.5f;
                float s = a == 0 ? 1.0f : -1.0f;   // world X of the character's right, -Z = its front
                vec3 right(s, 0, 0), front(0, 0, -s);
                vec3 off = view_ == 9 ? (-right * 0.30f + front * 0.02f) : (right * 0.30f + front * 0.02f);
                look(hp + off + vec3(0, 0.05f, 0), hp + vec3(0, 0.01f, 0), 34);
                break;
            }
            case 10: look({0.42f, 1.08f, 0.02f}, {0, 1.0f, 0}, 34); break;
            case 12:
            case 13:
            case 14:
            case 15: {
                int a = view_ >= 14 ? 1 : 0;
                const mat4* g = anim_[a].globals();
                vec3 hp = (g[IndexR3].translation() + g[ThumbR3].translation()) * 0.5f;
                float s = a == 0 ? 1.0f : -1.0f;
                vec3 right(s, 0, 0), front(0, 0, -s);
                vec3 off = (view_ & 1) ? (right * 0.34f + front * 0.06f) : (front * 0.34f - right * 0.08f);
                look(hp + off + vec3(0, 0.09f, 0), hp + vec3(0, 0.02f, 0), 32);
                break;
            }
            default: return orbit_.camera();
        }
        return c;
    }

    // ---- self test (numbers in the log)
    void selfTest();

    const Skeleton* sk_ = nullptr;
    DebugBody body_;
    Mesh floor_, table_, boardLight_, boardDark_, chairs_, clock_, button_, pieceMesh_[7];
    Material floorMat_, tableMat_, boardLightMat_, boardDarkMat_, clockMat_, buttonMat_, pieceWhiteMat_, pieceBlackMat_;
    Material robotMat_[2], eyeMat_, darkMat_;
    anim::Animator anim_[2];
    Piece pieces_[kPieces];
    float simTime_ = 0.0f;
    int stage_ = 0;
    float nextAt_ = 0.0f;
    int clockSide_ = -1;
    float headYaw_ = 0, headPitch_ = 0;
    int view_ = 0;
    bool frozen_ = false, paused_ = false, slow_ = false, solo_ = false;
    OrbitCamera orbit_;
};

// ---------------------------------------------------------------------------------------------
// Self test: every square for both players, pawn and king, Reach -> Lift -> Carry -> Place.
// ---------------------------------------------------------------------------------------------
void AnimViewer::selfTest() {
    using namespace anim;
    const Skeleton& sk = robotSkeleton();
    auto tip = [&](const mat4* g, Bone b) {
        vec3 d = normalize(sk.restOffset[b]) * sk.boneLength[b];
        return transformPoint(g[b], d);
    };
    int fails = 0;
    float worstTimeErr = 0, worstPlace = 0, worstThumb = 0, worstIndexH = 0, minFinger = 1e9f, maxDist = 0, minHeld = 1e9f;
    float sumThumb = 0;
    int count = 0;
    char minHeldWhere[96] = "", minFingerWhere[96] = "";
    for (int player = 0; player < 2; ++player)
        for (int type : {1, 6})
            for (int s = 0; s < 64; ++s) {
                Animator a;
                const float pz = layout::PLAYER_PELVIS_Z, py = layout::PLAYER_PELVIS_Y;
                a.init(sk, vec3(0, py, player == 0 ? pz : -pz), player == 0 ? 1.0f : -1.0f);
                mat4 pieceXf = translate(layout::squareCenter(s));
                a.pieceTransform = [&](int i) { return i == 7 ? pieceXf : mat4(); };   // only piece 7 exists
                a.pieceGripInfo = [&](int) { return vec3(layout::PIECE_HEIGHT[type], layout::PIECE_GRIP_HEIGHT[type], layout::PIECE_GRIP_RADIUS[type]); };
                int dstSq = (s + 19) & 63;
                vec3 dst = layout::squareCenter(dstSq);
                std::vector<Task> ts(4);
                ts[0].type = TaskType::Reach; ts[0].pieceId = 7;
                ts[1].type = TaskType::Lift;
                ts[2].type = TaskType::Carry; ts[2].position = dst;
                ts[3].type = TaskType::Place; ts[3].position = dst;
                a.enqueue(ts);
                float t = 0, h = 1.0f / 120.0f;
                bool gripped = false, released = false;
                float gripTime = -1, relTime = -1;
                mat4 relXf, lastHeld;
                std::vector<Event> ev;
                while (t < 1.2f) {
                    ev.clear();
                    a.update(h, ev);
                    t += h;
                    for (auto& e : ev) {
                        if (e.type == EventType::PieceGripped) { gripped = true; gripTime = e.time; }
                        if (e.type == EventType::PieceReleased) { released = true; relTime = e.time; relXf = e.transform; }
                    }
                    mat4 held;
                    if (a.heldPieceTransform(7, held)) {
                        pieceXf = held;
                        lastHeld = held;
                        vec3 up = transformDir(held, vec3(0, 1, 0));
                        float low = held.translation().y - layout::PIECE_BASE_RADIUS[type] * std::sqrt(std::max(0.0f, 1.0f - up.y * up.y));
                        if (low - layout::BOARD_TOP_Y < minHeld) {
                            minHeld = low - layout::BOARD_TOP_Y;
                            snprintf(minHeldWhere, sizeof(minHeldWhere), "player %d type %d %c%c->%c%c t=%.3f", player, type, 'a' + (s & 7), '1' + (s >> 3),
                                     'a' + (dstSq & 7), '1' + (dstSq >> 3), t);
                        }
                    }
                    const mat4* g = a.globals();
                    const float hb = layout::BOARD_SIZE * 0.5f;
                    for (int f = 0; f < 5; ++f) {
                        vec3 p = tip(g, Bone(ThumbR3 + f * 3));
                        if (std::fabs(p.x) > hb || std::fabs(p.z) > hb) continue;
                        if (p.y - 0.0065f - layout::BOARD_TOP_Y < minFinger) {
                            minFinger = p.y - 0.0065f - layout::BOARD_TOP_Y;
                            snprintf(minFingerWhere, sizeof(minFingerWhere), "player %d type %d %c%c->%c%c t=%.3f finger %d", player, type, 'a' + (s & 7),
                                     '1' + (s >> 3), 'a' + (dstSq & 7), '1' + (dstSq >> 3), t, f);
                        }
                    }
                    if (gripped && std::fabs(t - gripTime) < 0.5f * h) {
                        // At the grip instant: thumb/index tips around the piece at grip height.
                        vec3 c = layout::squareCenter(s);
                        float gh = layout::BOARD_TOP_Y + layout::PIECE_GRIP_HEIGHT[type] * layout::PIECE_HEIGHT[type];
                        vec3 th = tip(g, ThumbR3), ix = tip(g, IndexR3);
                        float dth = length(vec3(th.x - c.x, 0, th.z - c.z)), dix = length(vec3(ix.x - c.x, 0, ix.z - c.z));
                        worstThumb = std::max(worstThumb, std::fabs(dth - (layout::PIECE_GRIP_RADIUS[type] + 0.007f)));
                        sumThumb += dth;
                        worstIndexH = std::max(worstIndexH, std::fabs(ix.y - gh));
                        maxDist = std::max(maxDist, dix);
                        ++count;
                    }
                }
                float expectGrip = Timing::Reach, expectRel = Timing::Reach + Timing::Lift + Timing::Carry + Timing::Place;
                worstTimeErr = std::max(worstTimeErr, std::max(std::fabs(gripTime - expectGrip), std::fabs(relTime - expectRel)));
                if (released) worstPlace = std::max(worstPlace, length(lastHeld.translation() - dst));
                if (!gripped || !released) ++fails;
                if (released && length(lastHeld.translation() - dst) > 0.003f)
                    LOGW("selftest: player %d type %d %c%c->%c%c placed %.1f mm off", player, type, 'a' + (s & 7), '1' + (s >> 3), 'a' + (dstSq & 7),
                         '1' + (dstSq >> 3), length(lastHeld.translation() - dst) * 1000.0f);
            }
    LOGI("selftest: %d sequences failed, event timing err %.2e s, worst hand placement %.2f mm", fails, worstTimeErr, worstPlace * 1000.0f);
    LOGI("selftest: grip thumb radial err max %.1f mm (mean dist %.1f mm), index height err max %.1f mm, index max dist %.1f mm",
         worstThumb * 1000.0f, count ? sumThumb / count * 1000.0f : 0.0f, worstIndexH * 1000.0f, maxDist * 1000.0f);
    LOGI("selftest: lowest fingertip pad over the board %.1f mm (%s), lowest held piece rim %.1f mm (%s)", minFinger * 1000.0f, minFingerWhere,
         minHeld * 1000.0f, minHeldWhere);
    // Full starting position: every piece of each side to three empty squares; hands and forearms
    // must stay clear of the other pieces, the piece must land where requested.
    {
        float worstPen = 0, worstRel = 0, worstRim = 1e9f;
        char penWhere[128] = "", relWhere[96] = "", rimWhere[96] = "";
        int bad = 0;
        for (int player = 0; player < 2; ++player)
            for (int k16 = 0; k16 < 16; ++k16)
                for (int k = 0; k < 3; ++k) {
                    if (const char* only = std::getenv("SCACELITH_SWEEP_ONLY")) {   // "id,k": trace one move
                        int oid = -1, ok = -1;
                        if (sscanf(only, "%d,%d", &oid, &ok) == 2 && (oid != player * 16 + k16 || ok != k)) continue;
                    }
                    Piece ps[kPieces];
                    initialPieces(ps);
                    int id = player * 16 + k16;
                    int file = (k16 & 7), rank = player == 0 ? 2 + k : 5 - k;
                    int dstSq = ((file + 3 * k + 1) & 7) + rank * 8;
                    vec3 dst = layout::squareCenter(dstSq);
                    Animator a;
                    const float pz = layout::PLAYER_PELVIS_Z, py = layout::PLAYER_PELVIS_Y;
                    a.init(sk, vec3(0, py, player == 0 ? pz : -pz), player == 0 ? 1.0f : -1.0f);
                    a.pieceTransform = [&](int i) { return i >= 0 && i < kPieces ? ps[i].xf : mat4(); };
                    a.pieceGripInfo = [&](int i) {
                        int t = i >= 0 && i < kPieces ? ps[i].type : 1;
                        return vec3(layout::PIECE_HEIGHT[t], layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
                    };
                    a.pathObstacleTop = [&](vec3 f, vec3 t) { return piecesTopNear(ps, f, t, &a); };
                    a.obstacleTopNear = [&](vec3 p, float r, int ignore) { return piecesTopAt(ps, p, r, ignore, &a); };
                    std::vector<Task> ts(5);
                    ts[0].type = TaskType::Reach; ts[0].pieceId = id;
                    ts[1].type = TaskType::Lift;
                    ts[2].type = TaskType::Carry; ts[2].position = dst;
                    ts[3].type = TaskType::Place; ts[3].position = dst;
                    ts[4].type = TaskType::Retract;
                    a.enqueue(ts);
                    std::vector<Event> ev;
                    mat4 lastHeld;
                    float pen = 0, rel = 0, penT = 0;
                    int penBone = 0;
                    for (float t = 0; t < 1.6f; t += 1.0f / 120.0f) {
                        ev.clear();
                        a.update(1.0f / 120.0f, ev);
                        for (auto& e : ev) {
                            if (e.type == EventType::PieceGripped) ps[id].heldBy = 0;
                            if (e.type == EventType::PieceReleased) {
                                rel = length(lastHeld.translation() - dst);
                                ps[id].heldBy = -1;
                                ps[id].xf = e.transform;
                            }
                        }
                        mat4 held;
                        if (a.heldPieceTransform(id, held)) {
                            ps[id].xf = held;
                            lastHeld = held;
                            vec3 up = transformDir(held, vec3(0, 1, 0));
                            float low = held.translation().y - layout::PIECE_BASE_RADIUS[ps[id].type] * std::sqrt(std::max(0.0f, 1.0f - up.y * up.y));
                            if (low - layout::BOARD_TOP_Y < worstRim) {
                                worstRim = low - layout::BOARD_TOP_Y;
                                snprintf(rimWhere, sizeof(rimWhere), "%s piece %d to %c%c t=%.3f", player ? "Black" : "White", id, 'a' + (dstSq & 7),
                                         '1' + (dstSq >> 3), t);
                            }
                        }
                        if (std::getenv("SCACELITH_SWEEP_ONLY") && a.heldPieceTransform(id, held))
                            LOGI("trace t=%.3f held base %.1f mm up %.3f wrist %.3f %.3f %.3f elbow %.3f %.3f %.3f", t, (held.translation().y - layout::BOARD_TOP_Y) * 1000.0f,
                                 transformDir(held, vec3(0, 1, 0)).y, a.globals()[HandR].translation().x, a.globals()[HandR].translation().y,
                                 a.globals()[HandR].translation().z, a.globals()[ForeArmR].translation().x, a.globals()[ForeArmR].translation().y,
                                 a.globals()[ForeArmR].translation().z);
                        Overlap o = pieceOverlap(sk, a.globals(), ps, id, id);
                        if (std::getenv("SCACELITH_SWEEP_ONLY") && o.depth > 0.0f) {
                            const mat4* gg = a.globals();
                            vec3 m0 = gg[MiddleR1].translation(), m1 = gg[MiddleR2].translation(), w = gg[HandR].translation();
                            LOGI("trace t=%.3f overlap %.1f mm %s vs %d | mcp %.3f %.3f %.3f pip %.3f %.3f %.3f wrist %.3f %.3f %.3f", t, o.depth * 1000.0f,
                                 boneName(Bone(o.bone)), o.piece, m0.x, m0.y, m0.z, m1.x, m1.y, m1.z, w.x, w.y, w.z);
                        }
                        if (o.depth > pen) { pen = o.depth; penBone = o.bone; penT = t; }
                        if (o.depth > worstPen) {
                            worstPen = o.depth;
                            snprintf(penWhere, sizeof(penWhere), "%s %s %c%c->%c%c t=%.2f vs piece %d", player ? "Black" : "White", boneName(Bone(o.bone)),
                                     'a' + file, '1' + (player == 0 ? (k16 < 8 ? 1 : 0) : (k16 < 8 ? 6 : 7)), 'a' + (dstSq & 7), '1' + (dstSq >> 3), t, o.piece);
                        }
                    }
                    if (pen > 0.005f) {
                        ++bad;
                        if (std::getenv("SCACELITH_SWEEP_VERBOSE")) LOGI("sweep: %s piece %d (type %d) to %c%c: overlap %.1f mm (%s t=%.2f)", player ? "Black" : "White",
                                                                       id, ps[id].type, 'a' + (dstSq & 7), '1' + (dstSq >> 3), pen * 1000.0f, boneName(Bone(penBone)), penT);
                    }
                    if (rel > worstRel) {
                        worstRel = rel;
                        snprintf(relWhere, sizeof(relWhere), "%s piece %d to %c%c", player ? "Black" : "White", id, 'a' + (dstSq & 7), '1' + (dstSq >> 3));
                    }
                }
        LOGI("selftest board: %d of 96 moves touch another piece by > 5 mm; deepest %.1f mm (%s)", bad, worstPen * 1000.0f, penWhere);
        LOGI("selftest board: worst placement %.2f mm (%s), lowest held rim %.1f mm (%s)", worstRel * 1000.0f, relWhere, worstRim * 1000.0f, rimWhere);
    }
    // The game's compositions (src/game/game_scene.cpp: planPlacement, planPromotionSwap), with
    // its lift heights, capture slots and spare-queen spot, with and without the obstacle
    // callbacks (the game may leave them unset): event order and instants, placement, contact
    // with the other pieces (forearm: any piece), and how high the hand goes.
    {
        const vec3 lever[2] = {vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, 0.045f),
                               vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, -0.045f)};
        auto carryH = [](const Piece* ps, vec3 from, vec3 to, int ia, int ib) {   // GameScene::carryHeight
            float top = 0.0f;
            vec3 d(to.x - from.x, 0, to.z - from.z);
            float len2 = std::max(1e-8f, length2(d));
            for (int i = 0; i < kPieces; ++i) {
                if (i == ia || i == ib || ps[i].heldBy >= 0) continue;
                vec3 p = ps[i].xf.translation();
                if (p.y > layout::BOARD_TOP_Y + 0.01f) continue;
                float sN = clamp(dot(vec3(p.x - from.x, 0, p.z - from.z), d) / len2, 0.0f, 1.0f);
                vec3 c = from + d * sN;
                if (length(vec3(p.x - c.x, 0, p.z - c.z)) < layout::PIECE_BASE_RADIUS[ps[i].type] + 0.024f)
                    top = std::max(top, layout::PIECE_HEIGHT[ps[i].type]);
            }
            return std::max(0.022f, top + 0.014f);
        };
        auto mk = [](TaskType ty, int id, vec3 pos = vec3(0), float h = 0.0f) {
            Task t;
            t.type = ty;
            t.pieceId = id;
            t.position = pos;
            t.height = h;
            return t;
        };
        auto sqPos = [](const char* s) { return layout::squareCenter(s[0] - 'a', s[1] - '1'); };
        // Game capture slot n for a capturer sitting at zSign (clock at +X: slots at -X).
        auto slot = [](int n, float zSign) {
            int row = n / 8, col = n % 8;
            return vec3(-(layout::CAPTURE_ROW_X + float(row) * layout::CAPTURE_SPACING), layout::TABLE_TOP_Y,
                        zSign * (0.03f + float(col) * layout::CAPTURE_SPACING * 0.62f));
        };
        auto off = [](Piece& p) { p.xf = translate(vec3(3.0f, 0.0f, 0.0f)); };   // not in the game any more
        struct Comp {
            const char* name;
            int player;
            Piece ps[kPieces];
            std::vector<Task> tasks;
        };
        std::vector<Comp> comps;
        auto add = [&](const char* name, int player) -> Comp& {
            comps.emplace_back();
            Comp& c = comps.back();
            c.name = name;
            c.player = player;
            initialPieces(c.ps);
            return c;
        };
        auto move = [&](Comp& c, int id, vec3 to, int victim, float zSign, int slotN) {   // planPlacement
            Piece* ps = c.ps;
            vec3 from = ps[id].xf.translation();
            c.tasks.push_back(mk(TaskType::Reach, id));
            c.tasks.push_back(mk(TaskType::Lift, id, vec3(0), carryH(ps, from, to, id, victim)));
            c.tasks.push_back(mk(TaskType::Carry, id, to));
            if (victim >= 0) c.tasks.push_back(mk(TaskType::TakeCaptured, victim));
            c.tasks.push_back(mk(TaskType::Place, id, to));
            if (victim >= 0) c.tasks.push_back(mk(TaskType::Discard, victim, slot(slotN, zSign)));
        };
        auto finish = [&](Comp& c) {
            c.tasks.push_back(mk(TaskType::PressClock, -1, lever[c.player]));
            c.tasks.push_back(mk(TaskType::Retract, -1));
        };
        {   // exd3 (a black pawn on d3), first capture slot
            Comp& c = add("capture exd3, slot 0", 0);
            c.ps[19].xf = translate(sqPos("d3")) * rotateY(PI);
            move(c, 4, sqPos("d3"), 19, 1.0f, 0);
            finish(c);
        }
        {   // Nxe5 into the middle, eighth slot (at the player's edge)
            Comp& c = add("capture Nf3xe5, slot 7", 0);
            c.ps[14].xf = translate(sqPos("f3"));
            c.ps[20].xf = translate(sqPos("e5")) * rotateY(PI);
            move(c, 14, sqPos("e5"), 20, 1.0f, 7);
            finish(c);
        }
        {   // Black: ...Qxh4 on White's side, second row of the capture slots
            Comp& c = add("black capture Qd8xh4, slot 9", 1);
            c.ps[7].xf = translate(sqPos("h4"));
            c.ps[20].xf = translate(sqPos("e6")) * rotateY(PI);   // opens the diagonal d8-h4
            move(c, 27, sqPos("h4"), 7, -1.0f, 9);
            finish(c);
        }
        {   // O-O: king e1-g1 then rook h1-f1
            Comp& c = add("castling O-O", 0);
            off(c.ps[13]);
            off(c.ps[14]);
            move(c, 12, sqPos("g1"), -1, 1.0f, 0);
            c.ps[12].xf = translate(sqPos("g1"));   // (for the rook's lift height, as the game plans it)
            move(c, 15, sqPos("f1"), -1, 1.0f, 0);
            c.ps[12].xf = translate(sqPos("e1"));
            finish(c);
        }
        {   // Black O-O-O: king e8-c8 then rook a8-d8
            Comp& c = add("black castling O-O-O", 1);
            off(c.ps[25]);
            off(c.ps[26]);
            off(c.ps[27]);
            move(c, 28, sqPos("c8"), -1, -1.0f, 0);
            c.ps[28].xf = translate(sqPos("c8")) * rotateY(PI);
            move(c, 24, sqPos("d8"), -1, -1.0f, 0);
            c.ps[28].xf = translate(sqPos("e8")) * rotateY(PI);
            finish(c);
        }
        {   // b7-b8=Q: the pawn goes to the capture row, the spare queen (reserve spot) to b8
            Comp& c = add("promotion b8=Q", 0);
            off(c.ps[17]);
            off(c.ps[25]);
            c.ps[1].xf = translate(sqPos("b7"));
            move(c, 1, sqPos("b8"), -1, 1.0f, 0);
            c.tasks.push_back(mk(TaskType::Reach, 1));
            c.tasks.push_back(mk(TaskType::Lift, 1, vec3(0), 0.03f));
            c.tasks.push_back(mk(TaskType::Carry, 1, slot(0, 1.0f)));
            c.tasks.push_back(mk(TaskType::Place, 1, slot(0, 1.0f)));
            c.tasks.push_back(mk(TaskType::Reach, 32));   // the spare White queen
            c.ps[1].xf = translate(slot(0, 1.0f));   // (where the pawn is by then, for the lift height)
            c.tasks.push_back(mk(TaskType::Lift, 32, vec3(0), carryH(c.ps, c.ps[32].xf.translation(), sqPos("b8"), 32, 1)));
            c.ps[1].xf = translate(sqPos("b7"));
            c.tasks.push_back(mk(TaskType::Carry, 32, sqPos("b8")));
            c.tasks.push_back(mk(TaskType::Place, 32, sqPos("b8")));
            finish(c);
        }
        for (int withCb = 1; withCb >= 0; --withCb)
            for (Comp& c0 : comps) {
                Piece ps[kPieces];
                for (int i = 0; i < kPieces; ++i) ps[i] = c0.ps[i];
                bool touched[kPieces] = {};
                for (const Task& t : c0.tasks)
                    if (t.pieceId >= 0) touched[t.pieceId] = true;
                const float facing = c0.player == 0 ? 1.0f : -1.0f;
                Animator a;
                a.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, facing * layout::PLAYER_PELVIS_Z), facing);
                a.setRestHand(vec3(facing * 0.265f, layout::TABLE_TOP_Y, facing * 0.305f));   // as the game does
                a.pieceTransform = [&](int i) { return i >= 0 && i < kPieces ? ps[i].xf : mat4(); };
                a.pieceGripInfo = [&](int i) {
                    int t = i >= 0 && i < kPieces ? ps[i].type : 1;
                    return vec3(layout::PIECE_HEIGHT[t], layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
                };
                if (withCb) {
                    a.pathObstacleTop = [&](vec3 f, vec3 t) { return piecesTopNear(ps, f, t, &a); };
                    a.obstacleTopNear = [&](vec3 p, float r, int ignore) { return piecesTopAt(ps, p, r, ignore, &a); };
                }
                // Expected piece/clock events: type, instant, position.
                struct Want {
                    EventType type;
                    float t;
                    vec3 pos;
                };
                std::vector<Want> want;
                float tEnd = 0.0f;
                for (const Task& t : c0.tasks) {
                    tEnd += taskDuration(t);
                    switch (t.type) {
                        case TaskType::Reach: want.push_back({EventType::PieceGripped, tEnd, vec3(0)}); break;
                        case TaskType::Place: want.push_back({EventType::PieceReleased, tEnd, t.position}); break;
                        case TaskType::TakeCaptured: want.push_back({EventType::CapturedGripped, tEnd, vec3(0)}); break;
                        case TaskType::Discard: want.push_back({EventType::CapturedReleased, tEnd, t.position}); break;
                        case TaskType::PressClock: want.push_back({EventType::ClockPressed, tEnd, t.position}); break;
                        default: break;
                    }
                }
                a.enqueue(c0.tasks);
                std::vector<Event> ev;
                size_t next = 0;
                int bad = 0;
                float timeErr = 0.0f, place = 0.0f, handTop = 0.0f, handTopT = 0.0f;
                const char* onlyComp = std::getenv("SCACELITH_COMP_ONLY");   // trace one composition: "name-prefix"
                const bool traceComp = onlyComp && std::strncmp(c0.name, onlyComp, std::strlen(onlyComp)) == 0;
                if (onlyComp && !traceComp) continue;
                Overlap worst;
                float worstT = 0.0f;
                mat4 lastHeld[kPieces];
                const float dt = 1.0f / 120.0f;
                for (float t = 0; t < tEnd + 0.4f; t += dt) {
                    ev.clear();
                    a.update(dt, ev);
                    for (auto& e : ev) {
                        bool piece = e.type == EventType::PieceGripped || e.type == EventType::PieceReleased || e.type == EventType::CapturedGripped ||
                                     e.type == EventType::CapturedReleased || e.type == EventType::ClockPressed;
                        if (!piece) continue;
                        if (next >= want.size() || want[next].type != e.type) {
                            ++bad;
                        } else {
                            timeErr = std::max(timeErr, std::fabs(e.time - want[next].t));
                            bool hasPos = e.type != EventType::PieceGripped && e.type != EventType::CapturedGripped;
                            if (hasPos && length(e.position - want[next].pos) > 1e-4f) ++bad;
                        }
                        ++next;
                        if (e.type == EventType::PieceGripped || e.type == EventType::CapturedGripped) ps[e.pieceId].heldBy = 0;
                        if (e.type == EventType::PieceReleased || e.type == EventType::CapturedReleased) {
                            place = std::max(place, length(lastHeld[e.pieceId].translation() - e.position));
                            ps[e.pieceId].heldBy = -1;
                            ps[e.pieceId].xf = e.transform;
                        }
                    }
                    for (int i = 0; i < kPieces; ++i) {
                        mat4 held;
                        if (a.heldPieceTransform(i, held)) ps[i].xf = lastHeld[i] = held;
                    }
                    Overlap o = pieceOverlapIf(sk, a.globals(), ps, [&](int i, bool arm) { return !arm && touched[i]; });
                    if (o.depth > worst.depth) { worst = o; worstT = t; }
                    float hy = a.globals()[HandR].translation().y - layout::BOARD_TOP_Y;
                    if (hy > handTop) { handTop = hy; handTopT = t; }
                    if (traceComp && int(t * 120.0f + 0.5f) % 6 == 0) {
                        vec3 w = a.globals()[HandR].translation(), e = a.globals()[ForeArmR].translation();
                        LOGI("comp t=%.3f wrist %.3f %.3f %.3f elbow %.3f %.3f %.3f contact %.1f mm (%s, piece %d)", t, w.x, w.y, w.z, e.x, e.y, e.z,
                             o.depth * 1000.0f, boneName(Bone(o.bone)), o.piece);
                    }
                }
                if (next != want.size()) bad += int(want.size() > next ? want.size() - next : next - want.size());
                bool fail = bad > 0 || timeErr > 1e-4f || place > 0.004f || worst.depth > 0.006f || handTop > 0.35f;
                ::logx::write(fail ? ::logx::Level::Warn : ::logx::Level::Info, "selftest composition %-30s %s: %d events, %d wrong, timing err %.1e s, placement %.2f mm, deepest contact %.1f mm "
                                     "(%s, piece %d, t=%.2f), hand up to %.0f mm (t=%.2f)",
                                     c0.name, withCb ? "cb" : "--", int(want.size()), bad, timeErr, place * 1000.0f, worst.depth * 1000.0f,
                                     boneName(Bone(worst.bone)), worst.piece, worstT, handTop * 1000.0f, handTopT);
            }
    }
    // Head override: camera direction matches and stays stable.
    {
        Animator a;
        a.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
        float yaw = 0.4f, pitch = -0.5f;
        std::vector<Event> ev;
        float maxAng = 0, maxMove = 0;
        mat4 first;
        for (int i = 0; i < 480; ++i) {
            a.setHeadOverride(true, yaw, pitch);
            a.update(1.0f / 120.0f, ev);
            mat4 e = a.eyeCameraTransform();
            if (i == 120) first = e;
            if (i > 120) {
                maxAng = std::max(maxAng, std::acos(clamp(dot(transformDir(e, vec3(0, 0, -1)), transformDir(first, vec3(0, 0, -1))), -1.0f, 1.0f)));
                maxMove = std::max(maxMove, length(e.translation() - first.translation()));
            }
        }
        mat4 e = a.eyeCameraTransform();
        vec3 f = transformDir(e, vec3(0, 0, -1));
        vec3 fc(-f.x, f.y, -f.z);
        float gotYaw = std::atan2(fc.x, fc.z), gotPitch = std::asin(clamp(fc.y, -1.0f, 1.0f));
        LOGI("selftest: head override yaw %.2f/%.2f pitch %.2f/%.2f deg, drift %.3f deg, eye motion %.2f mm", gotYaw / DEG, yaw / DEG,
             gotPitch / DEG, pitch / DEG, maxAng / DEG, maxMove * 1000.0f);
    }
}

}  // namespace

SCACELITH_SCENE("anim", "Animation viewer: two debug robots, handshake, moves, captures, clock", AnimViewer);
