// "anim" viewer scene: two debug robots (capsules generated from the Skeleton) seated at the
// table, placeholder board/pieces/clock and a scripted timeline exercising every hand task:
//   0.5 s  handshake (both players)
//   3.6 s  White e2-e4 (Reach Lift Carry Place, PressClock, Retract)
//          Black d7-d5 0.7 s after White's clock press (the AI 'thinks' in between)
//          White exd5 0.7 s after Black's clock press (Reach Lift Carry TakeCaptured Place
//          Discard PressClock Retract)
// Other timelines (--demo), with the game's table layout (clock at +X, so Black plays with its
// LEFT hand and both scoresheets lie at -X):
//   lefty    handshake (Black's right hand = its writing hand), pens picked up, 1.e4 d5 2.exd5 Qxd5
//            played with White's right / Black's left hand while the other hand writes each move
//            (Black presses the clock with its left hand while its right hand writes), both players
//            turn a page, White lays its pen down, final handshake while Black still holds its pen
//            (it lays it down first)
//   lcastle  Black (left hand) castles short
//   lpromo   Black (left hand) promotes b2-b1=Q (pawn to the capture slots on its side, spare queen
//            to b1)
//   coach    Black is the coach (left hand; position after 1.e4 d5 2.Nc3), speaking (a synthetic
//            voice envelope: head bobs, nods, a head shake, blinks) while it gestures:
//             0.6 s  open hand towards the player (queued at 0.4 s with notBefore), two beats
//             2.8 s  points at b1 (a square on the far rank; its eyes come back to the player
//                    while the finger stays), then at the knight on c3 (with emphasis)
//             6.0 s  traces the knight's L g1-g3-f3 and the bishop's diagonal f1-b5, presents the
//                    board with the open hand, beats, retracts
//            then demonstrates 2...dxe4 3.Nxe4 Nf6 with both colours (every captured piece to the
//            coach's own capture slots) and takes the three moves back, one move at a time
// Command line (after --scene anim):
//   --demo d        default | lefty | lcastle | lpromo | coach
//   --time t        simulate 0..t with fixed 1/120 s steps, then (in --shot mode) freeze
//   --view v        side | sidel | front | back | top | white | black | hand | handb | handl | shake | orbit |
//                   pinch | pinchs | pinchb | pinchbs (close-ups of White's / Black's playing fingers from the
//                   front and from the side) | pen | pens | penb | penbs (White's / Black's writing hand,
//                   from the front and from the thumb side) | page | pageb (page corner) | lhand (Black's
//                   playing hand from its left) | pad | padb (the scoresheet from above) | clock |
//                   coachhand | coachhands | coachhandt (Black's playing hand close up, from the front /
//                   from its outside / from its thumb side)
//   --robot         draw the real porcelain robot instead of the capsule robots (slower start)
//   --solo          draw only the pieces held or within 6 cm of a playing index fingertip
//   --selftest      numeric checks of the IK/grasp/timing/writing/mirroring and of the coach demo (results in
//                   the log)
// Keys: Space pause, R restart, V next view, S slow motion, Escape quit (White's head follows the
// action by itself).
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../character/robot.h"
#include "../core/log.h"
#include "../game/layout.h"
#include "../render/materials/material_library.h"
#include "../render/mesh.h"
#include "animator.h"
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
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
            auto B = [&](Bone l) { return sideBone(l, side); };
            set(B(ClavicleL), capsule({0, 0, 0}, sk.restOffset[B(UpperArmL)], 0.034f, 0.040f));
            set(B(UpperArmL), capsule({0, 0, 0}, sk.restOffset[B(ForeArmL)], 0.043f, 0.036f));
            set(B(ForeArmL), capsule({0, 0, 0}, sk.restOffset[B(HandL)], 0.035f, 0.025f));
            set(B(HandL), rbox({0.0f, -0.047f, 0.002f}, {0.0135f, 0.042f, 0.037f}, 0.010f));
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
// beyond the clock, near their owner, see PhysicalBoard::reserveSlot).
void initialPieces(Piece out[kPieces]) {
    static const int back[8] = {4, 2, 3, 5, 6, 3, 2, 4};
    for (int i = 32; i < kPieces; ++i) {
        out[i].type = 5;
        out[i].color = i - 32;
        out[i].heldBy = -1;
        out[i].xf = translate(vec3(layout::RESERVE_X, layout::TABLE_TOP_Y, i == 32 ? layout::RESERVE_Z : -layout::RESERVE_Z)) * rotateY(i == 32 ? 0.0f : PI);
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

// Capture slot n of the pieces of 'capturedColor' (PhysicalBoard::captureSlot with the clock at
// +X): on the clock side, in the half of the player who captured them.
vec3 captureSlot(int n, int capturedColor) {
    vec2 s = layout::captureSlot(n);
    return vec3(s.x, layout::TABLE_TOP_Y, capturedColor == 1 ? s.y : -s.y);
}
// A bone of the right arm moved to 'side' (the playing hand of a left-handed player).
Bone onSide(Bone rightBone, Side s) { return s == Side::Right ? rightBone : Bone(rightBone - (ClavicleR - ClavicleL)); }

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

// ---------------------------------------------------------------------------------------------
// Demo handwriting (the game has its own handwriting model): a small single-stroke font turned
// into pen-tip keys every 1/60 s, strokes with minimum-jerk speed, pen lifted between strokes.
// ---------------------------------------------------------------------------------------------
struct Glyph {
    char c;
    float w;                                   // advance (cap heights)
    std::vector<std::vector<vec2>> strokes;    // x right, y up; 1 = cap height, 0.62 = x-height
};
const Glyph* glyphFor(char c) {
    static const std::vector<Glyph> font = {
        {'a', 0.72f, {{{0.55f, 0.50f}, {0.40f, 0.62f}, {0.18f, 0.56f}, {0.04f, 0.32f}, {0.10f, 0.06f}, {0.30f, 0.00f}, {0.50f, 0.12f}, {0.56f, 0.40f},
                       {0.57f, 0.62f}, {0.56f, 0.20f}, {0.60f, 0.02f}, {0.68f, 0.00f}}}},
        {'b', 0.70f, {{{0.08f, 1.00f}, {0.05f, 0.50f}, {0.04f, 0.00f}, {0.06f, 0.30f}, {0.25f, 0.58f}, {0.47f, 0.56f}, {0.60f, 0.32f}, {0.50f, 0.07f},
                       {0.28f, 0.00f}, {0.06f, 0.06f}}}},
        {'c', 0.62f, {{{0.52f, 0.50f}, {0.36f, 0.62f}, {0.14f, 0.54f}, {0.03f, 0.30f}, {0.12f, 0.06f}, {0.32f, 0.00f}, {0.54f, 0.10f}}}},
        {'d', 0.72f, {{{0.52f, 0.48f}, {0.36f, 0.62f}, {0.14f, 0.54f}, {0.03f, 0.30f}, {0.12f, 0.06f}, {0.32f, 0.00f}, {0.50f, 0.12f}, {0.56f, 0.45f},
                       {0.58f, 1.00f}, {0.57f, 0.40f}, {0.58f, 0.08f}, {0.66f, 0.00f}}}},
        {'e', 0.62f, {{{0.06f, 0.30f}, {0.52f, 0.34f}, {0.50f, 0.52f}, {0.32f, 0.62f}, {0.12f, 0.54f}, {0.03f, 0.30f}, {0.12f, 0.06f}, {0.32f, 0.00f},
                       {0.54f, 0.10f}}}},
        {'f', 0.50f, {{{0.50f, 0.95f}, {0.38f, 1.00f}, {0.24f, 0.92f}, {0.20f, 0.60f}, {0.20f, 0.00f}}, {{0.02f, 0.60f}, {0.44f, 0.60f}}}},
        {'g', 0.68f, {{{0.52f, 0.48f}, {0.36f, 0.62f}, {0.14f, 0.54f}, {0.04f, 0.32f}, {0.14f, 0.10f}, {0.34f, 0.06f}, {0.52f, 0.20f}, {0.56f, 0.45f},
                       {0.57f, 0.62f}, {0.56f, 0.00f}, {0.50f, -0.28f}, {0.30f, -0.36f}, {0.08f, -0.28f}}}},
        {'h', 0.66f, {{{0.06f, 1.00f}, {0.05f, 0.50f}, {0.04f, 0.00f}, {0.07f, 0.32f}, {0.22f, 0.56f}, {0.42f, 0.60f}, {0.54f, 0.44f}, {0.56f, 0.00f}}}},
        {'x', 0.58f, {{{0.02f, 0.62f}, {0.52f, 0.00f}}, {{0.52f, 0.62f}, {0.02f, 0.00f}}}},
        {'1', 0.50f, {{{0.10f, 0.78f}, {0.32f, 1.00f}, {0.32f, 0.00f}}}},
        {'2', 0.66f, {{{0.06f, 0.78f}, {0.18f, 0.96f}, {0.38f, 1.00f}, {0.54f, 0.86f}, {0.52f, 0.62f}, {0.30f, 0.36f}, {0.04f, 0.00f}, {0.58f, 0.00f}}}},
        {'3', 0.64f, {{{0.06f, 0.90f}, {0.28f, 1.00f}, {0.50f, 0.90f}, {0.50f, 0.66f}, {0.28f, 0.54f}, {0.52f, 0.42f}, {0.56f, 0.18f}, {0.40f, 0.02f},
                       {0.18f, 0.00f}, {0.02f, 0.10f}}}},
        {'4', 0.66f, {{{0.44f, 0.00f}, {0.44f, 1.00f}, {0.02f, 0.30f}, {0.60f, 0.30f}}}},
        {'5', 0.64f, {{{0.52f, 1.00f}, {0.12f, 1.00f}, {0.08f, 0.56f}, {0.30f, 0.62f}, {0.50f, 0.52f}, {0.56f, 0.28f}, {0.44f, 0.06f}, {0.22f, 0.00f},
                       {0.02f, 0.10f}}}},
        {'6', 0.62f, {{{0.50f, 0.94f}, {0.30f, 1.00f}, {0.12f, 0.84f}, {0.03f, 0.50f}, {0.06f, 0.16f}, {0.24f, 0.00f}, {0.46f, 0.06f}, {0.54f, 0.28f},
                       {0.44f, 0.52f}, {0.24f, 0.56f}, {0.06f, 0.40f}}}},
        {'7', 0.62f, {{{0.04f, 1.00f}, {0.56f, 1.00f}, {0.30f, 0.40f}, {0.22f, 0.00f}}}},
        {'8', 0.60f, {{{0.46f, 0.86f}, {0.28f, 1.00f}, {0.10f, 0.88f}, {0.14f, 0.66f}, {0.30f, 0.54f}, {0.52f, 0.36f}, {0.50f, 0.10f}, {0.28f, 0.00f},
                       {0.06f, 0.12f}, {0.08f, 0.36f}, {0.30f, 0.54f}, {0.44f, 0.70f}, {0.46f, 0.86f}}}},
        {'N', 0.66f, {{{0.04f, 0.00f}, {0.06f, 1.00f}, {0.56f, 0.00f}, {0.58f, 1.00f}}}},
        {'B', 0.64f, {{{0.05f, 0.00f}, {0.05f, 1.00f}, {0.38f, 0.98f}, {0.52f, 0.84f}, {0.46f, 0.60f}, {0.10f, 0.54f}, {0.46f, 0.50f}, {0.58f, 0.30f},
                       {0.50f, 0.08f}, {0.30f, 0.00f}, {0.05f, 0.00f}}}},
        {'R', 0.64f, {{{0.05f, 0.00f}, {0.05f, 1.00f}, {0.40f, 0.98f}, {0.54f, 0.82f}, {0.46f, 0.60f}, {0.10f, 0.54f}, {0.30f, 0.52f}, {0.56f, 0.00f}}}},
        {'Q', 0.78f, {{{0.36f, 1.00f}, {0.10f, 0.86f}, {0.02f, 0.50f}, {0.10f, 0.14f}, {0.36f, 0.00f}, {0.60f, 0.14f}, {0.68f, 0.50f}, {0.60f, 0.86f},
                       {0.36f, 1.00f}},
                      {{0.40f, 0.26f}, {0.72f, -0.06f}}}},
        {'K', 0.64f, {{{0.05f, 1.00f}, {0.05f, 0.00f}}, {{0.56f, 1.00f}, {0.08f, 0.42f}, {0.58f, 0.00f}}}},
        {'O', 0.76f, {{{0.36f, 1.00f}, {0.10f, 0.86f}, {0.02f, 0.50f}, {0.10f, 0.14f}, {0.36f, 0.00f}, {0.60f, 0.14f}, {0.68f, 0.50f}, {0.60f, 0.86f},
                       {0.36f, 1.00f}}}},
        {'-', 0.46f, {{{0.04f, 0.48f}, {0.40f, 0.48f}}}},
        {'=', 0.50f, {{{0.04f, 0.62f}, {0.44f, 0.62f}}, {{0.04f, 0.34f}, {0.44f, 0.34f}}}},
        {'+', 0.54f, {{{0.04f, 0.48f}, {0.48f, 0.48f}}, {{0.26f, 0.72f}, {0.26f, 0.24f}}}},
        {'.', 0.25f, {{{0.08f, 0.02f}, {0.10f, 0.00f}}}},
        {' ', 0.40f, {}},
    };
    for (const Glyph& g : font)
        if (g.c == c) return &g;
    return nullptr;
}
vec2 catmull(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float u) {
    float u2 = u * u, u3 = u2 * u;
    return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) * 0.5f;
}
// Pen-tip keys writing 'text' on the paper plane: baseline starting at 'base' (world, on the
// paper), 'right' and 'up' = the writer's right and forward on the page. capH: capital height.
std::vector<anim::PenKey> handwriting(const std::string& text, vec3 base, vec3 right, vec3 up, float capH = 0.0045f) {
    std::vector<anim::PenKey> keys;
    // Slanted a little to the right, like most hands.
    auto world = [&](vec2 p, float h) { return base + right * (p.x + 0.18f * p.y) * capH + up * (p.y * capH) + vec3(0, 1, 0) * h; };
    float x = 0.0f, t = 0.0f;
    const float step = 1.0f / 60.0f;
    bool first = true;
    vec2 last(0, 0);
    for (char c : text) {
        const Glyph* g = glyphFor(c);
        if (!g) continue;
        for (const auto& st : g->strokes) {
            // Dense polyline through the stroke points.
            std::vector<vec2> pts;
            for (size_t i = 0; i + 1 < st.size(); ++i) {
                vec2 p0 = st[i > 0 ? i - 1 : i], p1 = st[i], p2 = st[i + 1], p3 = st[i + 2 < st.size() ? i + 2 : i + 1];
                for (int k = 0; k < 8; ++k) pts.push_back(catmull(p0, p1, p2, p3, float(k) / 8.0f) + vec2(x, 0));
            }
            pts.push_back(st.back() + vec2(x, 0));
            std::vector<float> acc(1, 0.0f);
            for (size_t i = 1; i < pts.size(); ++i) acc.push_back(acc.back() + length(pts[i] - pts[i - 1]) * capH);
            const float L = acc.back();
            auto at = [&](float s) {
                size_t i = 1;
                while (i + 1 < acc.size() && acc[i] < s) ++i;
                float u = acc[i] > acc[i - 1] ? clamp((s - acc[i - 1]) / (acc[i] - acc[i - 1]), 0.0f, 1.0f) : 0.0f;
                return lerp(pts[i - 1], pts[i], u);
            };
            // Travel to the stroke with the pen lifted (or hover down onto the paper at the start).
            if (first) {
                keys.push_back({0.0f, world(pts.front(), 0.0025f), false});
                t = 0.09f;
                first = false;
            } else {
                vec3 a = world(last, 0.0f), b = world(pts.front(), 0.0f);
                float d = length(b - a), T = std::max(0.07f, d / 0.07f);
                keys.back().down = false;
                keys.push_back({t + T * 0.5f, (a + b) * 0.5f + vec3(0, 0.0015f + 0.10f * d, 0), false});
                t += T;
            }
            // The stroke: minimum-jerk speed profile.
            const float T = 0.05f + L / 0.040f;
            const int n = std::max(2, int(std::ceil(T / step)));
            for (int i = 0; i <= n; ++i) {
                float u = float(i) / float(n);
                keys.push_back({t + u * T, world(at(L * smootherstep(u)), 0.0f), true});
            }
            t += T;
            last = pts.back();
        }
        x += g->w + 0.12f;
    }
    if (!keys.empty()) {
        keys.back().down = false;
        keys.push_back({t + 0.09f, keys.back().tip + vec3(0, 0.003f, 0), false});
    }
    return keys;
}

// Scoresheet pad of player a (0 White, 1 Black) with the clock at +X: on the other side, in front
// of its owner. Writer's right / forward (towards the board) on the page.
struct PadFrame {
    vec3 centre, right, up;
    float top;                 // paper height
};
PadFrame padFrame(int a) {
    const float zs = a == 0 ? 1.0f : -1.0f;
    PadFrame f;
    f.top = layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS;
    f.centre = vec3(-layout::SCORESHEET_X, f.top, zs * layout::SCORESHEET_Z);
    f.right = vec3(zs, 0, 0);      // White (facing -Z): its right is +X
    f.up = vec3(0, 0, -zs);        // towards the board
    return f;
}
// Baseline start of row r (0 = first under the header), column c (0 White's move, 1 Black's).
vec3 rowBase(int a, int r, int c) {
    PadFrame f = padFrame(a);
    const float W = layout::SCORESHEET_WIDTH, L = layout::SCORESHEET_LENGTH;
    float y = L * 0.5f - 0.032f - float(r + 1) * 0.007f + 0.0014f;
    float x = -W * 0.5f + 0.017f + float(c) * 0.026f;
    return f.centre + f.right * x + f.up * y;
}
// Pen lying beside the pad's outer edge, parallel to Z, tip towards the board.
mat4 penRestFrame(int a) {
    PadFrame f = padFrame(a);
    const float zs = a == 0 ? 1.0f : -1.0f;
    vec3 tip = f.centre + vec3(-(layout::SCORESHEET_WIDTH * 0.5f + 0.030f), 0, 0) + f.up * (layout::PEN_LENGTH * 0.5f);
    tip.y = layout::TABLE_TOP_Y + layout::PEN_RADIUS;
    return translate(tip) * toMat4(fromTo(vec3(0, 1, 0), vec3(0, 0, zs)), vec3(0));
}
// Page flip geometry (shared by the page mesh and the TurnPage corner): the page hinges on the
// top edge; flip angle A(s) with a bend B(s) (the corner leads while it is lifted). v in [0,1]
// from the binding; returns the page point on its outer edge (xOff = 0) or anywhere across.
vec3 pagePoint(int a, float s, float v, float xOff) {
    PadFrame f = padFrame(a);
    const float L = layout::SCORESHEET_LENGTH;
    const float A = s * (PI + 0.024f), B = 0.9f * std::sin(PI * clamp(s, 0.0f, 1.0f));
    vec3 bind = f.centre + f.up * (L * 0.5f) + vec3(0, 0.0003f, 0);
    vec3 toWriter = -f.up;
    vec3 p = bind;
    const int n = 24;
    for (int i = 0; i < n && float(i) / n < v; ++i) {
        float v0 = float(i) / n, dv = std::min(1.0f / n, v - v0);
        float th = clamp(A + B * (v0 + 0.5f * dv - 0.5f), 0.0f, PI + 0.024f);
        p = p + (toWriter * std::cos(th) + vec3(0, 1, 0) * std::sin(th)) * (L * dv);
    }
    return p + f.right * xOff;
}
vec3 pageCorner(int a, float s) {   // the outer corner of the bottom edge (the writing hand's side)
    return pagePoint(a, s, 1.0f, -layout::SCORESHEET_WIDTH * 0.5f * padFrame(a).right.x);
}

const char* kViews[] = {"side", "sidel", "front", "back", "top", "white", "black", "hand", "handb", "handl", "shake", "orbit",
                        "pinch", "pinchs", "pinchb", "pinchbs", "pen", "pens", "penb", "penbs", "page", "pageb", "lhand", "pad", "padb", "clock",
                        "coachhand", "coachhands", "coachhandt"};
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
        demo_ = ctx.argValue("--demo", "default");
        if (demo_ != "lefty" && demo_ != "lcastle" && demo_ != "lpromo" && demo_ != "coach") demo_ = "default";
        if (ctx.hasArg("--robot")) {
            robot_ = true;
            materials::init();
            character::setupRobotMaterials();
            gpuRobot_.upload(character::buildRobot());
        }
        if (ctx.hasArg("--selftest")) {
            selfTest();
            writingSelfTest();
            const std::string keep = demo_;
            demo_ = "default";
            timelineCheck();
            demo_ = "coach";
            coachTimelineCheck();
            demo_ = keep;
        }
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
        if (sheets_) renderSheets(draw);
        if (robot_) {
            character::submitRobot(r, gpuRobot_, anim_[0].globals(), view_ == 5, 1000);
            character::submitRobot(r, gpuRobot_, anim_[1].globals(), view_ == 6, 2000);
        } else {
            body_.submit(r, anim_[0].globals(), robotMat_[0], eyeMat_, darkMat_, 1000, view_ == 5);
            body_.submit(r, anim_[1].globals(), robotMat_[1], eyeMat_, darkMat_, 2000, view_ == 6);
        }
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
        // Scoresheet pads with their row lines, the pens, a page lying flipped beyond each pad.
        const float W = layout::SCORESHEET_WIDTH, L = layout::SCORESHEET_LENGTH, T = layout::SCORESHEET_THICKNESS;
        MeshData pads, lines;
        for (int a = 0; a < 2; ++a) {
            PadFrame f = padFrame(a);
            pads.append(rbox(f.centre - vec3(0, T * 0.5f, 0), {W * 0.5f, T * 0.5f, L * 0.5f}, 0.0015f));
            for (int r = -1; r < layout::SCORESHEET_ROWS; ++r) {
                float y = L * 0.5f - 0.032f - float(r + 1) * 0.007f;
                vec3 c = f.centre + f.up * y + vec3(0, 0.00025f, 0);
                flatSeg(lines, c - f.right * (W * 0.5f - 0.006f), c + f.right * (W * 0.5f - 0.006f), 0.0003f);
            }
            MeshData flipped = pageMesh(a, 1.0f);
            pageFlat_[a].upload(flipped, "page-flipped");
        }
        padMesh_.upload(pads, "pads");
        lineMesh_.upload(lines, "pad-lines");
        const float pr = layout::PEN_RADIUS, pl = layout::PEN_LENGTH;
        std::vector<vec2> pen = {{0, 0}, {0.0005f, 0.0f}, {0.0009f, 0.0012f}, {0.0021f, 0.009f}, {pr * 0.8f, 0.017f}, {pr, 0.023f},
                                 {pr, pl - 0.004f}, {pr * 0.8f, pl}, {0, pl}};
        penMesh_.upload(prim::lathe(pen, 24), "pen");
        paperMat_ = mat(vec3(0.93f, 0.92f, 0.88f), 0.85f);
        paperMat_.doubleSided = true;
        lineMat_ = mat(vec3(0.55f, 0.64f, 0.80f), 0.8f);
        lineMat_.doubleSided = true;
        inkMat_ = mat(vec3(0.03f, 0.05f, 0.30f), 0.45f);
        inkMat_.doubleSided = true;
        penMat_ = mat(vec3(0.07f, 0.09f, 0.20f), 0.25f, 0.0f, 0.8f);
    }
    // Flat quad from a to b, w wide, facing up (ink strokes, ruled lines).
    static void flatSeg(MeshData& md, vec3 a, vec3 b, float w) {
        vec3 d(b.x - a.x, 0, b.z - a.z);
        float len = length(d);
        if (len < 1e-7f) return;
        d = d / len;
        vec3 n = cross(vec3(0, 1, 0), d) * (w * 0.5f), e = d * (w * 0.35f);
        uint32_t base = uint32_t(md.vertices.size());
        for (vec3 p : {a - e - n, a - e + n, b + e + n, b + e - n}) {
            Vertex v;
            v.pos = p;
            v.normal = vec3(0, 1, 0);
            v.tangent = vec4(1, 0, 0, 1);
            v.uv = vec2(0, 0);
            md.vertices.push_back(v);
        }
        for (uint32_t k : {0u, 2u, 1u, 0u, 3u, 2u}) md.indices.push_back(base + k);   // counter-clockwise from above
    }
    // The page being turned (flip progress s), as a bent strip.
    static MeshData pageMesh(int a, float s) {
        MeshData md;
        const int n = 24;
        const float W = layout::SCORESHEET_WIDTH;
        for (int i = 0; i <= n; ++i) {
            float v = float(i) / n;
            vec3 pa = pagePoint(a, s, v, -W * 0.5f), pb = pagePoint(a, s, v, W * 0.5f);
            vec3 dv = pagePoint(a, s, std::min(1.0f, v + 0.02f), 0.0f) - pagePoint(a, s, std::max(0.0f, v - 0.02f), 0.0f);
            vec3 nrm = safeNormal(cross(dv, padFrame(a).right));
            for (vec3 p : {pa, pb}) {
                Vertex vx;
                vx.pos = p;
                vx.normal = nrm;
                vx.tangent = vec4(1, 0, 0, 1);
                vx.uv = vec2(p == pa ? 0.0f : 1.0f, v);
                md.vertices.push_back(vx);
            }
        }
        for (int i = 0; i < n; ++i) {
            uint32_t k = uint32_t(2 * i);
            for (uint32_t q : {k, k + 3, k + 1, k, k + 2, k + 3}) md.indices.push_back(q);   // front = the side 'nrm' is on
        }
        return md;
    }
    static vec3 safeNormal(vec3 v) {
        float l = length(v);
        return l > 1e-8f ? v / l : vec3(0, 1, 0);
    }
    // Ink of player a: the finished strokes of the current page and the running path up to the
    // tip, laid on the page (following it while it turns).
    MeshData inkMesh(int a) const {
        const SheetState& s = sheet_[a];
        MeshData md;
        auto poly = [&](const std::vector<vec3>& p) {
            for (size_t i = 1; i < p.size(); ++i) flatSeg(md, p[i - 1], p[i], 0.00045f);
        };
        for (const auto& p : s.ink) poly(p);
        const float tNow = anim_[a].writingPathTime();
        if (tNow >= 0.0f && !s.paths.empty()) {
            std::vector<std::vector<vec3>> run;
            inkOf(s.paths.front(), tNow, run);
            for (const auto& p : run) poly(p);
        }
        const float turn = anim_[a].pageTurnProgress();
        if (turn > 0.0f) {
            // Onto the bent page: page coordinates of each vertex, then the page point there.
            PadFrame f = padFrame(a);
            const float L = layout::SCORESHEET_LENGTH;
            for (Vertex& v : md.vertices) {
                vec3 rel = v.pos - f.centre;
                float pv = (L * 0.5f - dot(rel, f.up)) / L, x = dot(rel, f.right);
                vec3 p = pagePoint(a, turn, pv, x);
                vec3 dv = pagePoint(a, turn, std::min(1.0f, pv + 0.01f), x) - pagePoint(a, turn, std::max(0.0f, pv - 0.01f), x);
                vec3 nrm = safeNormal(cross(dv, f.right));
                v.pos = p + nrm * 0.0002f;
                v.normal = nrm;
            }
        }
        return md;
    }
    // Ink polylines of a path up to time t (the tip curve itself, sampled every 4 ms).
    static void inkOf(const std::vector<anim::PenKey>& path, float t, std::vector<std::vector<vec3>>& out) {
        if (path.empty()) return;
        t = std::min(t, path.back().t);
        bool was = false;
        for (float u = 0.0f;; u += 0.004f) {
            float uu = std::min(u, t);
            bool down = anim::penPathDown(path, uu) || (uu > 0.0f && anim::penPathDown(path, uu - 1e-4f));
            if (down) {
                if (!was) out.emplace_back();
                vec3 p = anim::penPathPoint(path, uu);
                p.y = std::max(p.y, layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS) + 0.0003f;
                out.back().push_back(p);
            }
            was = down;
            if (uu >= t) break;
        }
    }
    template <class Draw>
    void renderSheets(Draw& draw) {
        draw(padMesh_, paperMat_, mat4(), 50);
        draw(lineMesh_, lineMat_, mat4(), 51, 0u);
        for (int a = 0; a < 2; ++a) {
            mat4 px;
            if (!anim_[a].penTransform(px)) px = sheet_[a].penTable;
            draw(penMesh_, penMat_, px, 52 + uint32_t(a));
            const float turn = anim_[a].pageTurnProgress();
            if (turn > 0.0f && turn < 1.0f) {
                pageMesh_[a].upload(pageMesh(a, turn), "page");
                draw(pageMesh_[a], paperMat_, mat4(), 56 + uint32_t(a));
            }
            if (sheet_[a].pagesTurned > 0) draw(pageFlat_[a], paperMat_, mat4(), 58 + uint32_t(a));
            MeshData ink = inkMesh(a);
            if (!ink.indices.empty()) {
                inkMesh_[a].upload(ink, "ink");
                draw(inkMesh_[a], inkMat_, mat4(), 60 + uint32_t(a), 0u);
            }
        }
    }

    // ---- simulation
    void reset() {
        initialPieces(pieces_);
        const bool lefty = demo_ != "default";   // the game's layout: Black's clock is on its left
        sheets_ = demo_ == "lefty";
        const float pz = layout::PLAYER_PELVIS_Z, py = layout::PLAYER_PELVIS_Y;
        anim_[0].init(*sk_, vec3(0, py, pz), 1.0f);
        anim_[1].init(*sk_, vec3(0, py, -pz), -1.0f, lefty ? Side::Left : Side::Right);
        if (lefty) {
            // As the game does: the playing hands rest on the clock side, in front of the body.
            anim_[0].setRestHand(vec3(0.24f, layout::TABLE_TOP_Y, 0.34f));
            anim_[1].setRestHand(vec3(0.24f, layout::TABLE_TOP_Y, -0.34f));
        }
        captures_[0] = captures_[1] = 0;
        for (int a = 0; a < 2; ++a) {
            sheet_[a] = SheetState();
            sheet_[a].penTable = penRestFrame(a);
        }
        actions_.clear();
        chain_.clear();
        demoSteps_.clear();
        demoTouch_[0] = demoTouch_[1] = demoTouch_[2] = demoTouch_[3] = -1;
        phrases_.clear();
        chainArmed_ = false;
        if (demo_ == "lefty") scriptLefty();
        if (demo_ == "coach") scriptCoach();
        if (demo_ == "lcastle") {
            pieces_[29].xf = pieces_[30].xf = translate(vec3(3.0f, 0.0f, 0.0f));   // f8, g8 gone
            at(0.5f, [this] {
                std::vector<anim::Task> ts;
                movePiece(ts, 1, 28, "g8");
                movePiece(ts, 1, 31, "f8");
                finishMove(ts, 1);
                anim_[1].enqueue(ts);
                LOGI("anim viewer: Black O-O (left hand) at t=%.3f", simTime_);
            });
        }
        if (demo_ == "lpromo") {
            pieces_[17].xf = translate(layout::squareCenter(1, 1)) * rotateY(PI);   // Black pawn on b2
            pieces_[1].xf = pieces_[9].xf = translate(vec3(3.0f, 0.0f, 0.0f));     // b2, b1 gone
            at(0.5f, [this] {
                using namespace anim;
                std::vector<Task> ts;
                movePiece(ts, 1, 17, "b1");
                vec3 slot = captureSlot(captures_[0]++, 0);   // beside Black, among the pieces it captured
                ts.push_back(mkTask(TaskType::Reach, 17));
                ts.push_back(mkTask(TaskType::Lift, 17, vec3(0), 0.03f));
                ts.push_back(mkTask(TaskType::Carry, 17, slot));
                ts.push_back(mkTask(TaskType::Place, 17, slot));
                vec3 b1 = layout::squareCenter(1, 0);
                ts.push_back(mkTask(TaskType::Reach, 33));   // the spare Black queen
                ts.push_back(mkTask(TaskType::Lift, 33, vec3(0), 0.07f));
                ts.push_back(mkTask(TaskType::Carry, 33, b1));
                ts.push_back(mkTask(TaskType::Place, 33, b1));
                finishMove(ts, 1);
                anim_[1].enqueue(ts);
                LOGI("anim viewer: Black b2-b1=Q (left hand) at t=%.3f", simTime_);
            });
        }
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
    // --solo: only pieces held or within 6 cm of a playing index fingertip are drawn.
    bool nearHand(int i) const {
        if (pieces_[i].heldBy >= 0) return true;
        vec3 p = pieces_[i].xf.translation();
        for (int a = 0; a < 2; ++a) {
            vec3 f = anim_[a].globals()[onSide(IndexR3, anim_[a].playHand())].translation();
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
        const int victimColor = 1 - player;
        vec3 spot = captureSlot(captures_[victimColor]++, victimColor);   // clock side, capturer's half
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
    // ---- demo timelines other than the default one: actions at fixed instants
    void at(float t, std::function<void()> f) { actions_.push_back({t, std::move(f), false}); }
    static anim::Task mkTask(anim::TaskType ty, int id = -1, vec3 pos = vec3(0), float h = 0.0f) {
        anim::Task t;
        t.type = ty;
        t.pieceId = id;
        t.position = pos;
        t.height = h;
        return t;
    }
    void movePiece(std::vector<anim::Task>& ts, int player, int id, const char* to) {
        using namespace anim;
        vec3 dst = layout::squareCenter(sq(to));
        ts.push_back(mkTask(TaskType::Reach, id));
        ts.push_back(mkTask(TaskType::Lift, id));
        ts.push_back(mkTask(TaskType::Carry, id, dst));
        ts.push_back(mkTask(TaskType::Place, id, dst));
    }
    void finishMove(std::vector<anim::Task>& ts, int player) {
        ts.push_back(mkTask(anim::TaskType::PressClock, -1, pressPoint(player)));
        ts.push_back(mkTask(anim::TaskType::Retract));
    }
    void handshake() {
        anim::Task h;
        h.type = anim::TaskType::Handshake;
        h.partner = &anim_[1];
        anim_[0].enqueue(h);
        h.partner = &anim_[0];
        anim_[1].enqueue(h);
        LOGI("anim viewer: handshake at t=%.3f (Black %s its pen)", simTime_, anim_[1].holdsPen() ? "holds" : "does not hold");
    }
    void pickPen(int a) {
        anim::WriteTask w;
        w.type = anim::WriteTaskType::PickPen;
        w.frame = sheet_[a].penTable;
        anim_[a].enqueueWriting(w);
    }
    void putPen(int a) {
        anim::WriteTask w;
        w.type = anim::WriteTaskType::PutPen;
        w.frame = penRestFrame(a);
        anim_[a].enqueueWriting(w);
    }
    // Writes 'text' in row r, column c (0 White's move, 1 Black's) of player a's scoresheet.
    void write(int a, int r, int c, const char* text) {
        PadFrame f = padFrame(a);
        anim::WriteTask w;
        w.type = anim::WriteTaskType::Write;
        w.path = handwriting(text, rowBase(a, r, c), f.right, f.up);
        sheet_[a].paths.push_back(w.path);
        anim_[a].setWritingRest(rowBase(a, r + 1, 0) + f.up * 0.004f - f.right * 0.006f);
        anim_[a].enqueueWriting(w);
    }
    void turnPage(int a) {
        anim::WriteTask w;
        w.type = anim::WriteTaskType::TurnPage;
        w.pageCorner = [a](float s) { return pageCorner(a, s); };
        anim_[a].setWritingRest(rowBase(a, 0, 0) + padFrame(a).up * 0.004f);
        anim_[a].enqueueWriting(w);
    }
    // The game's layout: White right-handed, Black left-handed, both write with the other hand.
    void scriptLefty() {
        at(0.5f, [this] { handshake(); });
        at(3.2f, [this] { pickPen(0); pickPen(1); });
        at(3.6f, [this] { quietMove(0, "e2", "e4"); write(0, 0, 0, "e4"); });
        at(5.2f, [this] { quietMove(1, "d7", "d5"); });
        at(5.35f, [this] { write(1, 0, 0, "e4"); write(1, 0, 1, "d5"); });
        at(6.6f, [this] { write(0, 0, 1, "d5"); });
        at(7.2f, [this] { captureMove(0, "e4", "d5"); write(0, 1, 0, "exd5"); });
        at(9.3f, [this] { captureMove(1, "d8", "d5"); write(1, 1, 0, "exd5"); write(1, 1, 1, "Qxd5"); });
        at(11.2f, [this] { write(0, 1, 1, "Qxd5"); });
        at(13.3f, [this] { turnPage(0); turnPage(1); });
        at(14.9f, [this] { putPen(0); });
        at(15.7f, [this] { handshake(); });
    }

    // ---- the coach demo (Black, left hand): gestures while speaking, then a demonstration line
    // played with both colours and taken back, one move at a time (each move is planned once the
    // previous one has been released, so the obstacle queries see the pieces where they are).
    struct DemoStep {
        int mover = -1, victim = -1;
        int from = -1, to = -1;
        vec3 slot{0, 0, 0};
    };
    std::vector<DemoStep> demoSteps_;                    // played so far (the rewind pops them)
    // Pieces the hand may touch (self test): the mover / victim of the move being played or undone,
    // and for a moment the previous move's (the fingers open around a piece they have just set down).
    int demoTouch_[4] = {-1, -1, -1, -1};
    float demoTouchAt_ = 0.0f;
    void touchDemo(int mover, int victim) {
        demoTouch_[2] = demoTouch_[0];
        demoTouch_[3] = demoTouch_[1];
        demoTouch_[0] = mover;
        demoTouch_[1] = victim;
        demoTouchAt_ = simTime_;
    }
    bool demoTouches(int piece) const {
        return piece >= 0 && (piece == demoTouch_[0] || piece == demoTouch_[1] ||
                              (simTime_ < demoTouchAt_ + 0.3f && (piece == demoTouch_[2] || piece == demoTouch_[3])));
    }
    std::deque<std::function<void()>> chain_;            // coach hand batches, run one after the other
    bool chainArmed_ = false;
    float chainAt_ = 0.0f;
    struct Phrase {
        float t0, t1;
    };
    std::vector<Phrase> phrases_;                        // when the coach "speaks"
    // Synthetic voice envelope: syllables at about 4.5 per second, a stressed one every 0.8 s,
    // short fades at the phrase ends.
    float speechLevel(float t) const {
        for (const Phrase& p : phrases_) {
            if (t < p.t0 || t > p.t1) continue;
            const float u = t - p.t0, fade = std::min(1.0f, std::min(u, p.t1 - t) / 0.08f);
            const float syl = std::fabs(std::sin(PI * 4.5f * u + 0.4f * std::sin(1.7f * u)));
            const float stress = std::pow(std::max(0.0f, std::sin(TAU * u / 0.8f + 0.6f)), 6.0f);
            return fade * clamp(0.18f + 0.50f * std::sqrt(syl) + 0.35f * stress, 0.0f, 1.0f);
        }
        return 0.0f;
    }
    static anim::Task gesture(anim::HandShape shape, float duration, int tag) {
        anim::Task t = mkTask(anim::TaskType::Gesture);
        t.shape = shape;
        t.duration = duration;
        t.tag = tag;
        return t;
    }
    // A demonstration move by the coach (either colour, slower than in play), its victim to the
    // coach's own capture slots: the human's half is out of the coach's reach.
    void demoMove(const char* from, const char* to) {
        using namespace anim;
        DemoStep s;
        s.from = sq(from);
        s.to = sq(to);
        s.mover = pieceAt(s.from);
        s.victim = pieceAt(s.to);
        const vec3 dst = layout::squareCenter(s.to);
        std::vector<Task> ts;
        auto add = [&](TaskType ty, int id, vec3 pos, float dur, float h = 0.0f) {
            Task t = mkTask(ty, id, pos, h);
            t.duration = dur;
            ts.push_back(t);
        };
        add(TaskType::Reach, s.mover, vec3(0), 0.55f);
        add(TaskType::Lift, s.mover, vec3(0), 0.20f);
        add(TaskType::Carry, s.mover, dst, 0.60f);
        if (s.victim >= 0) add(TaskType::TakeCaptured, s.victim, vec3(0), 0.25f);
        add(TaskType::Place, s.mover, dst, 0.30f);
        if (s.victim >= 0) {
            s.slot = captureSlot(captures_[0]++, 0);   // beside the coach
            add(TaskType::Discard, s.victim, s.slot, 0.50f);
        }
        anim_[1].enqueue(ts);
        demoSteps_.push_back(s);
        touchDemo(s.mover, s.victim);
        LOGI("anim viewer: coach demonstrates %s-%s (piece %d%s) at t=%.3f", from, to, s.mover, s.victim >= 0 ? ", a capture" : "", simTime_);
    }
    // Takes the last demonstration move back: the mover home, then the victim from its slot.
    void demoUndo() {
        using namespace anim;
        if (demoSteps_.empty()) return;
        const DemoStep s = demoSteps_.back();
        demoSteps_.pop_back();
        const vec3 from = layout::squareCenter(s.from), to = layout::squareCenter(s.to);
        std::vector<Task> ts;
        auto add = [&](TaskType ty, int id, vec3 pos, float dur, float h = 0.0f) {
            Task t = mkTask(ty, id, pos, h);
            t.duration = dur;
            ts.push_back(t);
        };
        add(TaskType::Reach, s.mover, vec3(0), 0.50f);
        add(TaskType::Lift, s.mover, vec3(0), 0.18f);
        add(TaskType::Carry, s.mover, from, 0.55f);
        add(TaskType::Place, s.mover, from, 0.28f);
        if (s.victim >= 0) {
            add(TaskType::Reach, s.victim, vec3(0), 0.55f);
            add(TaskType::Lift, s.victim, vec3(0), 0.22f, 0.06f);
            add(TaskType::Carry, s.victim, to, 0.60f);
            add(TaskType::Place, s.victim, to, 0.30f);
            --captures_[0];
        }
        anim_[1].enqueue(ts);
        touchDemo(s.mover, s.victim);
        LOGI("anim viewer: coach takes back its demonstration move (piece %d) at t=%.3f", s.mover, simTime_);
    }
    void scriptCoach() {
        using namespace anim;
        // The position after 1.e4 d5 2.Nc3.
        pieces_[4].xf = translate(layout::squareCenter(sq("e4")));
        pieces_[19].xf = translate(layout::squareCenter(sq("d5"))) * rotateY(PI);
        pieces_[9].xf = translate(layout::squareCenter(sq("c3")));
        phrases_ = {{0.45f, 2.55f}, {2.85f, 4.35f}, {4.55f, 6.05f}, {6.25f, 8.1f}, {8.4f, 10.2f}, {10.4f, 12.9f}};
        // "Let's look at this position together." An open hand towards the player, two beats.
        at(0.4f, [this] {
            Task open = gesture(HandShape::Open, 1.2f, 1);
            open.notBefore = 0.6f;
            anim_[1].enqueue({open, gesture(HandShape::Beat, 0.9f, 2)});
        });
        at(1.9f, [this] { anim_[1].nod(); });
        at(2.6f, [this] { anim_[1].blink(); });
        // "This square, b1, is now free... and this knight on c3 controls e4."
        at(2.8f, [this] {
            Task p1 = mkTask(TaskType::Point, -1, layout::squareCenter(sq("b1")));
            p1.duration = 1.7f;
            p1.gazeHold = 0.9f;   // then back to the player while the finger stays
            p1.tag = 3;
            Task p2 = mkTask(TaskType::Point, 9);
            p2.duration = 1.6f;
            p2.emphasis = true;
            p2.tag = 4;
            anim_[1].enqueue({p1, p2});
        });
        at(4.4f, [this] { anim_[1].blink(); });
        // "Your other knight goes to f3 like this, and the bishop can come out to b5."
        at(6.0f, [this] {
            Task k = mkTask(TaskType::Trace);
            k.path = moveTracePath(sq("g1"), sq("f3"));
            k.tag = 5;
            Task b = mkTask(TaskType::Trace);
            b.path = moveTracePath(sq("f1"), sq("b5"));
            b.gazeHold = 0.3f;
            b.tag = 6;
            anim_[1].enqueue({k, b, gesture(HandShape::Present, 1.4f, 7), gesture(HandShape::Beat, 0.9f, 8), mkTask(TaskType::Retract)});
            // Then the demonstration: 2...dxe4 3.Nxe4 Nf6, taken back move by move.
            chain_ = {[this] { demoMove("d5", "e4"); }, [this] { demoMove("c3", "e4"); }, [this] { demoMove("g8", "f6"); },
                      [this] { demoUndo(); },          [this] { demoUndo(); },          [this] { demoUndo(); },
                      [this] {
                          anim_[1].enqueue(mkTask(TaskType::Retract));
                          phrases_.push_back({simTime_ + 0.4f, simTime_ + 2.6f});   // "Not like that..."
                          at(simTime_ + 0.7f, [this] { anim_[1].shakeHead(); });
                          at(simTime_ + 2.0f, [this] { anim_[1].nod(0.09f, 0.5f); });
                          at(simTime_ + 2.7f, [this] { anim_[1].blink(); });
                      }};
            chainArmed_ = true;
            chainAt_ = simTime_ + 0.5f;
        });
        at(8.2f, [this] { anim_[1].nod(0.05f, 0.4f); });
    }

    void script() {
        using namespace anim;
        if (demo_ != "default") {
            for (size_t i = 0; i < actions_.size(); ++i) {   // (an action may schedule more)
                if (actions_[i].done || simTime_ < actions_[i].t) continue;
                actions_[i].done = true;
                std::function<void()> fn = actions_[i].fn;
                fn();
            }
            // The coach's hand batches: the next one once the hand is done with the previous one.
            if (chainArmed_ && !chain_.empty() && !anim_[1].busy() && simTime_ >= chainAt_) {
                std::function<void()> fn = chain_.front();
                chain_.pop_front();
                fn();
                chainAt_ = simTime_ + 0.35f;
            }
            return;
        }
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
                case EventType::PenPicked:
                case EventType::PenPut:
                    if (e.type == EventType::PenPut) sheet_[a].penTable = e.transform;
                    LOGI("anim viewer: %s %s its pen at t=%.4f", a ? "Black" : "White", e.type == EventType::PenPut ? "put down" : "picked up", e.time);
                    break;
                case EventType::WritingDone:
                    if (!sheet_[a].paths.empty()) {
                        std::vector<std::vector<vec3>> ink;
                        inkOf(sheet_[a].paths.front(), 1e9f, ink);
                        for (auto& p : ink) sheet_[a].ink.push_back(p);
                        sheet_[a].paths.pop_front();
                    }
                    break;
                case EventType::PageTurned:
                    sheet_[a].ink.clear();   // on the flipped page (face down)
                    ++sheet_[a].pagesTurned;
                    LOGI("anim viewer: %s turned a page at t=%.4f", a ? "Black" : "White", e.time);
                    break;
                default: break;
            }
        }
    }
    void step(float dt) {
        simTime_ += dt;
        script();
        if (demo_ == "coach") {
            // The coach looks at the player (its gestures take its eyes to their targets), speaks
            // with the synthetic envelope; the player watches the coach's hand, else its face.
            anim_[1].lookAt(anim_[0].eyeCameraTransform().translation(), 1.0f);
            anim_[1].setSpeechLevel(speechLevel(simTime_));
            const bool watch = anim_[1].busy();
            anim_[0].lookAt(watch ? anim_[1].globals()[onSide(IndexR3, anim_[1].playHand())].translation() : anim_[1].eyeCameraTransform().translation(), 1.0f);
        } else if (demo_ != "default") {
            // Both robots are driven by their own gaze (the writing look included).
            for (int a = 0; a < 2; ++a) {
                const int o = 1 - a;
                bool watch = anim_[o].busy() && !anim_[a].busy();
                anim_[a].lookAt(anim_[o].globals()[onSide(HandR, anim_[o].playHand())].translation(), watch ? 0.8f : 0.0f);
            }
        }
        if (demo_ != "default") {
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
            return;
        }
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
    // The coach demo end to end: the coach's hand and forearm against the standing pieces during
    // the gestures and during the demonstration (the pieces of the move under way excepted for the
    // hand, which grips them), its lowest fingertip above the board while gesturing, and the board
    // after the rewind against the board before the demonstration.
    void coachTimelineCheck() {
        using namespace anim;
        const Skeleton& sk = *sk_;
        reset();
        vec3 start[kPieces];
        for (int i = 0; i < kPieces; ++i) start[i] = pieces_[i].xf.translation();
        Overlap worst[2];
        float worstT[2] = {0, 0}, minTip = 1e9f, minTipT = 0;
        int worstTask[2] = {-1, -1};   // TaskType running then
        const float hb = layout::BOARD_SIZE * 0.5f, h = 1.0f / 120.0f;
        while (simTime_ < 90.0f && !(chainArmed_ && chain_.empty() && !anim_[1].busy() && simTime_ > chainAt_ + 0.5f)) {
            step(h);
            const mat4* g = anim_[1].globals();
            const int k = demoTouch_[0] < 0 ? 0 : 1;   // 0 gestures, 1 demonstration
            Overlap o = pieceOverlapIf(sk, g, pieces_, [&](int i, bool arm) { return !arm && demoTouches(i); });
            if (o.depth > worst[k].depth) {
                worst[k] = o;
                worstT[k] = simTime_;
                worstTask[k] = -1;
                for (int ty = 0; ty <= int(TaskType::Gesture); ++ty)
                    if (anim_[1].runningTask(TaskType(ty))) worstTask[k] = ty;
            }
            if (k != 0) continue;
            const Side ps = anim_[1].playHand();
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 3; ++j) {
                    const Bone b = onSide(Bone(ThumbR1 + f * 3 + j), ps);
                    const vec3 p = transformPoint(g[b], normalize(sk.restOffset[j < 2 ? b + 1 : b]) * sk.boneLength[b]);
                    if (std::fabs(p.x) > hb || std::fabs(p.z) > hb) continue;
                    const float above = p.y - layout::BOARD_TOP_Y - 0.0065f;
                    if (above < minTip) {
                        minTip = above;
                        minTipT = simTime_;
                    }
                }
        }
        float moved = 0;
        int movedId = -1;
        for (int i = 0; i < kPieces; ++i) {
            const float d = length(pieces_[i].xf.translation() - start[i]);
            if (d > moved) {
                moved = d;
                movedId = i;
            }
        }
        LOGI("selftest coach: done at t=%.2f; gestures: deepest hand/piece overlap %.1f mm (%s, piece %d, t=%.2f, task %d), lowest fingertip "
             "%.1f mm above the board (t=%.2f)",
             simTime_, worst[0].depth * 1000.0f, boneName(Bone(worst[0].bone)), worst[0].piece, worstT[0], worstTask[0], minTip * 1000.0f, minTipT);
        LOGI("selftest coach: demonstration: deepest hand/piece overlap %.1f mm (%s, piece %d, t=%.2f, task %d); after the rewind the farthest "
             "piece is %.2f mm from its start (piece %d)",
             worst[1].depth * 1000.0f, boneName(Bone(worst[1].bone)), worst[1].piece, worstT[1], worstTask[1], moved * 1000.0f, movedId);
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
                // Close-up of a playing hand: from its right-front (hand, handb) or left-front (handl).
                int a = view_ == 8 ? 1 : 0;
                const mat4* g = anim_[a].globals();
                const Side ps = anim_[a].playHand();
                vec3 hp = (g[onSide(IndexR3, ps)].translation() + g[onSide(ThumbR3, ps)].translation()) * 0.5f;
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
                const Side ps = anim_[a].playHand();
                vec3 hp = (g[onSide(IndexR3, ps)].translation() + g[onSide(ThumbR3, ps)].translation()) * 0.5f;
                float s = a == 0 ? 1.0f : -1.0f;
                if (ps == Side::Left) s = -s;   // the outside of a left hand is the character's left
                vec3 right(s, 0, 0), front(0, 0, a == 0 ? -1.0f : 1.0f);
                vec3 off = (view_ & 1) ? (right * 0.34f + front * 0.06f) : (front * 0.34f - right * 0.08f);
                look(hp + off + vec3(0, 0.09f, 0), hp + vec3(0, 0.02f, 0), 32);
                break;
            }
            case 16:
            case 17:
            case 18:
            case 19: {
                // Writing hand: pen tip (or wrist) from the front and above (pen, penb), or from
                // the thumb side, i.e. from the body's middle line (pens, penbs).
                int a = view_ >= 18 ? 1 : 0;
                const mat4* g = anim_[a].globals();
                const Side ws = anim_[a].writingHand();
                mat4 px;
                vec3 tgt = anim_[a].penTransform(px) ? transformPoint(px, vec3(0, 0.03f, 0)) : g[onSide(MiddleR1, ws)].translation();
                vec3 front(0, 0, a == 0 ? -1.0f : 1.0f), out(-1, 0, 0);   // the scoresheets lie at -X
                vec3 off = (view_ & 1) ? (-out * 0.21f + front * 0.07f + vec3(0, 0.10f, 0)) : (front * 0.21f + out * 0.06f + vec3(0, 0.15f, 0));
                look(tgt + off, tgt, 34);
                break;
            }
            case 20:
            case 21: {   // page corner
                int a = view_ - 20;
                PadFrame f = padFrame(a);
                vec3 c = pageCorner(a, 0.0f) + vec3(0, 0.03f, 0) + f.up * 0.05f;
                look(c + vec3(-0.30f, 0.22f, 0) - f.up * 0.22f, c, 40);
                break;
            }
            case 22: {   // Black's playing hand from its outside
                const mat4* g = anim_[1].globals();
                const Side ps = anim_[1].playHand();
                vec3 hp = (g[onSide(IndexR3, ps)].translation() + g[onSide(ThumbR3, ps)].translation()) * 0.5f;
                vec3 out(ps == Side::Left ? 1.0f : -1.0f, 0, 0);
                look(hp + out * 0.30f + vec3(0, 0.06f, 0.03f), hp + vec3(0, 0.01f, 0), 34);
                break;
            }
            case 23:
            case 24: {   // the scoresheet from above, as its writer reads it
                int a = view_ - 23;
                PadFrame f = padFrame(a);
                vec3 c = f.centre + f.up * 0.045f;
                look(c + vec3(0, 0.30f, 0) - f.up * 0.10f, c, 34);
                break;
            }
            case 25: look({1.05f, 1.05f, -0.35f}, {0.36f, 0.82f, -0.10f}, 36); break;
            case 26:
            case 27:
            case 28: {
                // Black's playing hand close up: from the front (the player's side, a little above),
                // from its outside, or from its thumb side.
                const mat4* g = anim_[1].globals();
                const Side ps = anim_[1].playHand();
                const vec3 hp = (g[onSide(HandR, ps)].translation() + g[onSide(IndexR3, ps)].translation()) * 0.5f;
                const vec3 out(ps == Side::Left ? 1.0f : -1.0f, 0, 0);
                const vec3 off = view_ == 26   ? vec3(0.04f, 0.13f, 0.30f)
                                 : view_ == 27 ? out * 0.30f + vec3(0, 0.08f, 0.05f)
                                               : out * -0.26f + vec3(0, 0.06f, 0.06f);
                look(hp + off, hp, 34);
                break;
            }
            default: return orbit_.camera();
        }
        return c;
    }

    // ---- self test (numbers in the log)
    void selfTest();
    void writingSelfTest();

    // ---- scoresheets
    struct SheetState {
        mat4 penTable;                                   // where the pen lies when not held
        std::deque<std::vector<anim::PenKey>> paths;     // queued Write paths (front = running / next)
        std::vector<std::vector<vec3>> ink;              // finished strokes on the current page
        int pagesTurned = 0;
    } sheet_[2];
    bool sheets_ = false;
    int captures_[2] = {0, 0};                           // pieces captured, per colour
    struct Action {
        float t;
        std::function<void()> fn;
        bool done;
    };
    std::vector<Action> actions_;
    std::string demo_ = "default";
    bool robot_ = false;
    character::GpuRobot gpuRobot_;
    Mesh padMesh_, lineMesh_, penMesh_, inkMesh_[2], pageMesh_[2], pageFlat_[2];
    Material paperMat_, lineMat_, inkMat_, penMat_;

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
        // Game capture slot n for a capturer sitting at zSign (clock at +X: slots on the clock side).
        auto slot = [](int n, float zSign) { return captureSlot(n, zSign > 0.0f ? 1 : 0); };
        auto off = [](Piece& p) { p.xf = translate(vec3(3.0f, 0.0f, 0.0f)); };   // not in the game any more
        struct Comp {
            std::string name;
            int player;
            Piece ps[kPieces];
            std::vector<Task> tasks;
        };
        std::vector<Comp> comps;
        auto add = [&](const std::string& name, int player) -> Comp& {
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
        {   // Nxe5 into the middle, last slot of the first row (at the player's edge, by the resting hand)
            Comp& c = add("capture Nf3xe5, slot 4", 0);
            c.ps[14].xf = translate(sqPos("f3"));
            c.ps[20].xf = translate(sqPos("e5")) * rotateY(PI);
            move(c, 14, sqPos("e5"), 20, 1.0f, 4);
            finish(c);
        }
        {   // Black: ...Qxh4 on White's side, second row of the capture slots
            Comp& c = add("black capture Qd8xh4, slot 5", 1);
            c.ps[7].xf = translate(sqPos("h4"));
            c.ps[20].xf = translate(sqPos("e6")) * rotateY(PI);   // opens the diagonal d8-h4
            move(c, 27, sqPos("h4"), 7, -1.0f, 5);
            finish(c);
        }
        // A capture into slot n with the slots before it taken by queens (the widest pieces, facing
        // any way): every other piece of the set but the mover and its victim stands there, and
        // the board is empty besides them.
        auto crowded = [&](const std::string& name, int player, int n) {
            Comp& c = add(name, player);
            const float zSign = player == 0 ? 1.0f : -1.0f;
            const int mover = player == 0 ? 4 : 20, victim = player == 0 ? 19 : 3;   // exd4 / ...exd5
            int k = 0;
            for (int i = 0; i < 32; ++i) {
                if (i == mover || i == victim) continue;
                if (k < n) {
                    c.ps[i].type = 5;
                    c.ps[i].xf = translate(slot(k, zSign)) * rotateY(0.9f * float(k));
                    ++k;
                } else {
                    off(c.ps[i]);
                }
            }
            c.ps[mover].xf = translate(sqPos(player == 0 ? "e3" : "e6")) * rotateY(player ? PI : 0.0f);
            c.ps[victim].xf = translate(sqPos(player == 0 ? "d4" : "d5")) * rotateY(player ? 0.0f : PI);
            move(c, mover, c.ps[victim].xf.translation(), victim, zSign, n);
            finish(c);
        };
        // Every slot a game fills before the late ones (see layout.h), for White, and the last of
        // them for Black (its left hand, the mirror image).
        for (int n = 1; n < layout::captureSlotEarlyCount(); ++n) crowded("capture into slot " + std::to_string(n), 0, n);
        crowded("black capture into slot " + std::to_string(layout::captureSlotEarlyCount() - 1), 1, layout::captureSlotEarlyCount() - 1);
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
        // Promotions that fill capture slots and take pieces from them. The promotion square's
        // neighbours are cleared: these check the slots, not a placement between two pieces.
        {   // g7xh8=Q: the rook to slot 0 and the pawn to slot 1 beside the player (both kept when the
            // move is planned), then the spare queen to h8
            Comp& c = add("promotion with capture gxh8=Q", 0);
            off(c.ps[22]);
            off(c.ps[30]);
            c.ps[6].xf = translate(sqPos("g7"));
            move(c, 6, sqPos("h8"), 31, 1.0f, 0);
            c.tasks.push_back(mk(TaskType::Reach, 6));
            c.tasks.push_back(mk(TaskType::Lift, 6, vec3(0), 0.03f));
            c.tasks.push_back(mk(TaskType::Carry, 6, slot(1, 1.0f)));
            c.tasks.push_back(mk(TaskType::Place, 6, slot(1, 1.0f)));
            c.tasks.push_back(mk(TaskType::Reach, 32));   // the spare White queen
            c.ps[6].xf = translate(slot(1, 1.0f));   // (where the pawn and the rook are by then)
            c.ps[31].xf = translate(slot(0, 1.0f));
            c.tasks.push_back(mk(TaskType::Lift, 32, vec3(0), carryH(c.ps, c.ps[32].xf.translation(), sqPos("h8"), 32, 6)));
            c.ps[6].xf = translate(sqPos("g7"));
            c.ps[31].xf = translate(sqPos("h8")) * rotateY(PI);
            c.tasks.push_back(mk(TaskType::Carry, 32, sqPos("h8")));
            c.tasks.push_back(mk(TaskType::Place, 32, sqPos("h8")));
            finish(c);
        }
        {   // b7-b8=N: the pawn to slot 0, a new knight (PhysicalBoard::takeSpare: none captured
            // beside the player) from the next free slot to b8
            Comp& c = add("under-promotion b8=N, new knight", 0);
            off(c.ps[17]);
            off(c.ps[25]);
            off(c.ps[26]);
            c.ps[1].xf = translate(sqPos("b7"));
            c.ps[9].xf = translate(slot(1, 1.0f));   // White's b1 knight stands in for the new one
            move(c, 1, sqPos("b8"), -1, 1.0f, 0);
            c.tasks.push_back(mk(TaskType::Reach, 1));
            c.tasks.push_back(mk(TaskType::Lift, 1, vec3(0), 0.03f));
            c.tasks.push_back(mk(TaskType::Carry, 1, slot(0, 1.0f)));
            c.tasks.push_back(mk(TaskType::Place, 1, slot(0, 1.0f)));
            c.tasks.push_back(mk(TaskType::Reach, 9));
            c.ps[1].xf = translate(slot(0, 1.0f));
            c.tasks.push_back(mk(TaskType::Lift, 9, vec3(0), carryH(c.ps, c.ps[9].xf.translation(), sqPos("b8"), 9, 1)));
            c.ps[1].xf = translate(sqPos("b7"));
            c.tasks.push_back(mk(TaskType::Carry, 9, sqPos("b8")));
            c.tasks.push_back(mk(TaskType::Place, 9, sqPos("b8")));
            finish(c);
        }
        for (int withCb = 1; withCb >= 0; --withCb)
            for (Comp& c0 : comps) {
                Piece ps[kPieces];
                for (int i = 0; i < kPieces; ++i) ps[i] = c0.ps[i];
                bool touched[kPieces] = {};
                for (const Task& t : c0.tasks)
                    if (t.pieceId >= 0) touched[t.pieceId] = true;
                // Clock at +X: Black plays with its left hand (its clock side), as in the game.
                const float facing = c0.player == 0 ? 1.0f : -1.0f;
                Animator a;
                a.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, facing * layout::PLAYER_PELVIS_Z), facing, c0.player == 0 ? Side::Right : Side::Left);
                a.setRestHand(vec3(0.24f, layout::TABLE_TOP_Y, facing * 0.34f));   // the playing hand, clock side (as in the game)
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
                const bool traceComp = onlyComp && c0.name.compare(0, std::strlen(onlyComp), onlyComp) == 0;
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
                    float hy = a.globals()[onSide(HandR, a.playHand())].translation().y - layout::BOARD_TOP_Y;
                    if (hy > handTop) { handTop = hy; handTopT = t; }
                    if (traceComp && int(t * 120.0f + 0.5f) % 6 == 0) {
                        vec3 w = a.globals()[onSide(HandR, a.playHand())].translation(), e = a.globals()[onSide(ForeArmR, a.playHand())].translation();
                        LOGI("comp t=%.3f wrist %.3f %.3f %.3f elbow %.3f %.3f %.3f contact %.1f mm (%s, piece %d)", t, w.x, w.y, w.z, e.x, e.y, e.z,
                             o.depth * 1000.0f, boneName(Bone(o.bone)), o.piece);
                    }
                }
                if (next != want.size()) bad += int(want.size() > next ? want.size() - next : next - want.size());
                bool fail = bad > 0 || timeErr > 1e-4f || place > 0.004f || worst.depth > 0.006f || handTop > 0.35f;
                ::logx::write(fail ? ::logx::Level::Warn : ::logx::Level::Info, "selftest composition %-30s %s: %d events, %d wrong, timing err %.1e s, placement %.2f mm, deepest contact %.1f mm "
                                     "(%s, piece %d, t=%.2f), hand up to %.0f mm (t=%.2f)",
                                     c0.name.c_str(), withCb ? "cb" : "--", int(want.size()), bad, timeErr, place * 1000.0f, worst.depth * 1000.0f,
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

// ---------------------------------------------------------------------------------------------
// Writing hand and left-handed play: White (right-handed) and Black (left-handed) each pick the
// pen up, write, turn a page, write again and put the pen down while the playing hand makes a
// move; then the mirror image check and a handshake with the pen still in hand.
// ---------------------------------------------------------------------------------------------
void AnimViewer::writingSelfTest() {
    using namespace anim;
    const Skeleton& sk = robotSkeleton();
    const float paper = layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS;
    auto setup = [&](Animator& an, int a, Piece* ps, bool lefty) {
        const float zs = a == 0 ? 1.0f : -1.0f;
        an.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), zs, lefty ? Side::Left : Side::Right);
        an.setRestHand(vec3(0.24f, layout::TABLE_TOP_Y, zs * 0.34f));
        an.pieceTransform = [ps](int i) { return i >= 0 && i < kPieces ? ps[i].xf : mat4(); };
        an.pieceGripInfo = [ps](int i) {
            int t = i >= 0 && i < kPieces ? ps[i].type : 1;
            return vec3(layout::PIECE_HEIGHT[t], layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
        };
    };
    // Pinch point of the writing hand (between the thumb and index pads, world).
    auto pinchOf = [&](const mat4* g, Side s) {
        // Midpoint of the thumb and index pads (skin surface: the pad side of the distal phalanx).
        auto pad = [&](Bone b, bool thumb) {
            vec3 d = normalize(sk.restOffset[b]);
            vec3 side = thumb ? normalize(cross(vec3(1, 0, 0), d)) : vec3(s == Side::Right ? 1.0f : -1.0f, 0, 0);
            return transformPoint(g[b], d * (sk.boneLength[b] * 0.72f) + side * 0.0068f);
        };
        return (pad(onSide(ThumbR3, s), true) + pad(onSide(IndexR3, s), false)) * 0.5f;
    };
    for (int a = 0; a < 2; ++a) {
        Piece ps[kPieces];
        initialPieces(ps);
        Animator an;
        setup(an, a, ps, a == 1);
        PadFrame f = padFrame(a);
        const mat4 pen0 = penRestFrame(a);
        std::vector<PenKey> p1 = handwriting("Qxd5", rowBase(a, 3, 0), f.right, f.up), p2 = handwriting("e4", rowBase(a, 0, 1), f.right, f.up);
        std::vector<WriteTask> wt(6);
        wt[0].type = WriteTaskType::PickPen;
        wt[0].frame = pen0;
        wt[1].type = WriteTaskType::Write;
        wt[1].path = p1;
        wt[2].type = WriteTaskType::TurnPage;
        wt[2].pageCorner = [a](float s) { return pageCorner(a, s); };
        wt[3].type = WriteTaskType::Write;
        wt[3].path = p2;
        wt[4].type = WriteTaskType::PutPen;
        wt[4].frame = pen0 * translate(vec3(0, 0.004f, 0));   // a little further back than it was
        wt[5].type = WriteTaskType::Wait;
        wt[5].duration = 0.2f;
        an.enqueueWriting(wt);
        // The playing hand makes a move meanwhile (its instants must not move): e2-e4 / d7-d5.
        const int pawn = a == 0 ? 4 : 19;
        vec3 dst = layout::squareCenter(a == 0 ? sq("e4") : sq("d5"));
        std::vector<Task> ts(6);
        ts[0].type = TaskType::Reach; ts[0].pieceId = pawn;
        ts[1].type = TaskType::Lift;
        ts[2].type = TaskType::Carry; ts[2].position = dst;
        ts[3].type = TaskType::Place; ts[3].position = dst;
        ts[4].type = TaskType::PressClock; ts[4].position = vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, a == 0 ? 0.045f : -0.045f);
        ts[5].type = TaskType::Retract;
        Task wait;
        wait.type = TaskType::Wait;
        wait.duration = 0.9f;   // the move starts while the pen is being written with
        an.enqueue(wait);
        an.enqueue(ts);
        float tRelease = 0.9f + Timing::Reach + Timing::Lift + Timing::Carry + Timing::Place, tClock = tRelease + Timing::PressClock;
        // Expected writing instants.
        float t0 = 0.0f;
        float tPick = t0 + 0.36f;   // seconds (PickPen at its nominal duration)
        t0 += Timing::PickPen;
        float path1 = t0 + Timing::WriteApproach;
        t0 += writeTaskDuration(wt[1]);
        float turn = t0;
        t0 += writeTaskDuration(wt[2]);
        float path2 = t0 + Timing::WriteApproach;
        t0 += writeTaskDuration(wt[3]);
        float tPut = t0 + 0.34f;
        t0 += Timing::PutPen + 0.2f;
        const float tEnd = t0;
        std::vector<float> downs, ups;   // expected PenDown / PenUp instants
        for (const auto* pp : {&p1, &p2}) {
            float base = pp == &p1 ? path1 : path2;
            for (size_t i = 0; i < pp->size(); ++i) {
                bool dn = i + 1 < pp->size() && (*pp)[i].down, db = i > 0 && (*pp)[i - 1].down;
                if (dn && !db) downs.push_back(base + (*pp)[i].t);
                if (!dn && db) ups.push_back(base + (*pp)[i].t);
            }
        }
        float tipErr = 0, belowPaper = 0, pinchErr = 0, pickJump = 0, putErr = 0, evErr = 0, fingerLow = 1e9f, lastS = -1, sBack = 0;
        float slidePerp = 0, axMin = 0, axMax = 0, lastTp = -1;
        vec3 lt0(0, 0, 0);
        int nDown = 0, nUp = 0, nDone = 0, nGrip = 0, nTurned = 0, nEmpty = 0, bad = 0;
        bool justPicked = false;
        mat4 lastPen;
        const float dt = 1.0f / 120.0f;
        std::vector<Event> ev;
        for (float t = 0; t < tEnd + 0.3f; t += dt) {
            ev.clear();
            an.update(dt, ev);
            for (const Event& e : ev) {
                auto expectAt = [&](float want) {
                    evErr = std::max(evErr, std::fabs(e.time - want));
                    if (std::getenv("SCACELITH_WRITE_TRACE")) LOGI("wtrace %s event %d at %.4f (expected %.4f)", a ? "B" : "W", int(e.type), e.time, want);
                };
                switch (e.type) {
                    case EventType::PieceGripped: ps[pawn].heldBy = 0; break;
                    case EventType::PieceReleased:
                        expectAt(tRelease);
                        ps[pawn].heldBy = -1;
                        ps[pawn].xf = e.transform;
                        break;
                    case EventType::ClockPressed: expectAt(tClock); break;
                    case EventType::PenPicked:
                        expectAt(tPick);
                        if (length(e.transform.translation() - pen0.translation()) > 1e-5f) ++bad;
                        justPicked = true;
                        break;
                    case EventType::PenPut: {
                        expectAt(tPut);
                        mat4 want = wt[4].frame;
                        if (length(e.transform.translation() - want.translation()) > 1e-5f) ++bad;
                        putErr = std::max(putErr, length(lastPen.translation() - want.translation()));
                        break;
                    }
                    case EventType::PenDown: expectAt(nDown < int(downs.size()) ? downs[size_t(nDown)] : -1.0f); ++nDown; break;
                    case EventType::PenUp: expectAt(nUp < int(ups.size()) ? ups[size_t(nUp)] : -1.0f); ++nUp; break;
                    case EventType::WritingDone: expectAt(nDone == 0 ? path1 + p1.back().t : path2 + p2.back().t); ++nDone; break;
                    case EventType::PageGripped: expectAt(turn + 0.33f * Timing::PageTurn); ++nGrip; break;
                    case EventType::PageTurned: expectAt(turn + 0.90f * Timing::PageTurn); ++nTurned; break;
                    case EventType::WritingQueueEmpty: expectAt(tEnd); ++nEmpty; break;
                    default: break;
                }
            }
            mat4 held;
            if (an.heldPieceTransform(pawn, held)) ps[pawn].xf = held;
            const mat4* g = an.globals();
            mat4 px;
            const bool hasPen = an.penTransform(px);
            if (hasPen) {
                if (justPicked) pickJump = std::max(pickJump, length(px.translation() - pen0.translation()));
                justPicked = false;
                lastPen = px;
                vec3 tip = px.translation();
                const float tp = an.writingPathTime();
                if (tp >= 0.0f) {
                    const auto& path = t < turn ? p1 : p2;
                    tipErr = std::max(tipErr, length(tip - penPathPoint(path, tp)));
                    if (std::getenv("SCACELITH_WRITE_TRACE")) {
                        vec3 w = penPathPoint(path, tp);
                        LOGI("wtrace %s t=%.3f tp=%.3f tip %.4f %.4f %.4f want %.4f %.4f %.4f", a ? "B" : "W", t + dt, tp, tip.x, tip.y, tip.z, w.x, w.y, w.z);
                    }
                }
                vec3 rel = tip - f.centre;
                if (std::fabs(dot(rel, f.right)) < layout::SCORESHEET_WIDTH * 0.5f && std::fabs(dot(rel, f.up)) < layout::SCORESHEET_LENGTH * 0.5f)
                    belowPaper = std::max(belowPaper, paper - tip.y);
                if (tp >= 0.0f) {
                    // The pen in the hand: it may move along its axis (the fingers push / draw it),
                    // anything else is the pen sliding through the fingers.
                    const Side ws = an.writingHand();
                    const mat4 inv = inverseAffine(g[onSide(HandR, ws)]);
                    vec3 lt = transformPoint(inv, tip), la = normalize(transformDir(inv, transformDir(px, vec3(0, 1, 0))));
                    if (tp < lastTp || lastTp < 0.0f) lt0 = lt;
                    lastTp = tp;
                    slidePerp = std::max(slidePerp, length((lt - lt0) - la * dot(lt - lt0, la)));
                    axMin = std::min(axMin, dot(lt - lt0, la));
                    axMax = std::max(axMax, dot(lt - lt0, la));
                    // Fingers of the writing hand above the paper (their pads may touch it, not sink in).
                    for (int fi = 0; fi < 5; ++fi) {
                        Bone b3 = onSide(Bone(ThumbR3 + fi * 3), ws);
                        vec3 ft = transformPoint(g[b3], normalize(sk.restOffset[b3]) * sk.boneLength[b3]);
                        fingerLow = std::min(fingerLow, ft.y - 0.0065f - paper);
                    }
                }
            }
            const float s = an.pageTurnProgress();
            if (s >= 0.0f) {
                if (s < lastS - 1e-6f) sBack = std::max(sBack, lastS - s);
                lastS = s;
                const float u = (t + dt - turn) / Timing::PageTurn;
                if (u > 0.34f && u < 0.69f) {
                    pinchErr = std::max(pinchErr, length(pinchOf(g, an.writingHand()) - pageCorner(a, s)));
                    if (std::getenv("SCACELITH_WRITE_TRACE")) {
                        vec3 pc = pinchOf(g, an.writingHand()), cc = pageCorner(a, s);
                        LOGI("wtrace %s turn u=%.3f s=%.3f pinch %.4f %.4f %.4f corner %.4f %.4f %.4f", a ? "B" : "W", u, s, pc.x, pc.y, pc.z, cc.x, cc.y, cc.z);
                    }
                }
            }
        }
        if (nDown != int(downs.size()) || nUp != int(ups.size()) || nDone != 2 || nGrip != 1 || nTurned != 1 || nEmpty != 1) ++bad;
        const bool fail = bad > 0 || evErr > 1e-4f || tipErr > 2e-4f || belowPaper > 3e-4f || putErr > 5e-4f || sBack > 0.0f || slidePerp > 0.002f ||
                          fingerLow < -0.001f || pinchErr > 0.002f;
        ::logx::write(fail ? ::logx::Level::Warn : ::logx::Level::Info,
                      "selftest writing %s (%s-handed): %d wrong, event err %.1e s, tip err %.3f mm, tip below paper %.2f mm, pen slide in the fingers "
                      "%.1f mm (along the pen %.1f..%.1f mm), pen jump at pick %.2f mm, put err %.2f mm, lowest writing fingertip pad %.1f mm over "
                      "the paper, page pinch off the corner %.1f mm",
                      a ? "Black" : "White", a ? "left" : "right", bad, evErr, tipErr * 1000.0f, belowPaper * 1000.0f, slidePerp * 1000.0f,
                      axMin * 1000.0f, axMax * 1000.0f, pickJump * 1000.0f, putErr * 1000.0f, fingerLow * 1000.0f, pinchErr * 1000.0f);
    }

    // Mirror image: Black left-handed with the clock at +X against Black right-handed in the
    // mirrored world (clock at -X, everything at -x): the same motion, bone for bone.
    {
        Piece psL[kPieces], psR[kPieces];
        initialPieces(psL);
        initialPieces(psR);
        const mat4 S = scale(vec3(-1, 1, 1));
        for (int i = 0; i < kPieces; ++i) psR[i].xf = S * psL[i].xf * S;
        Animator L, R;
        setup(L, 1, psL, true);
        setup(R, 1, psR, false);
        R.setRestHand(vec3(-0.24f, layout::TABLE_TOP_Y, -0.34f));
        auto mirrorV = [](vec3 v) { return vec3(-v.x, v.y, v.z); };
        PadFrame f = padFrame(1);
        std::vector<PenKey> path = handwriting("Nf6", rowBase(1, 2, 1), f.right, f.up), pathR = path;
        for (PenKey& k : pathR) k.tip = mirrorV(k.tip);
        auto writing = [&](Animator& an, const mat4& penF, const std::vector<PenKey>& p, bool mir) {
            std::vector<WriteTask> w(4);
            w[0].type = WriteTaskType::PickPen;
            w[0].frame = penF;
            w[1].type = WriteTaskType::Write;
            w[1].path = p;
            w[2].type = WriteTaskType::TurnPage;
            w[2].pageCorner = [mir](float s) {
                vec3 c = pageCorner(1, s);
                return mir ? vec3(-c.x, c.y, c.z) : c;
            };
            w[3].type = WriteTaskType::PutPen;
            w[3].frame = penF;
            an.enqueueWriting(w);
        };
        writing(L, penRestFrame(1), path, false);
        writing(R, S * penRestFrame(1) * S, pathR, true);
        auto moves = [&](Animator& an, bool mir) {
            auto P = [&](vec3 v) { return mir ? mirrorV(v) : v; };
            std::vector<Task> ts(8);
            ts[0].type = TaskType::Reach; ts[0].pieceId = 30;   // g8 knight
            ts[1].type = TaskType::Lift;
            ts[2].type = TaskType::Carry; ts[2].position = P(layout::squareCenter(sq("f6")));
            ts[3].type = TaskType::Place; ts[3].position = P(layout::squareCenter(sq("f6")));
            ts[4].type = TaskType::PressClock; ts[4].position = P(vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, -0.045f));
            ts[5].type = TaskType::Retract;
            ts[6].type = TaskType::Wait; ts[6].duration = 0.3f;
            ts[7].type = TaskType::Retract;
            an.enqueue(ts);
        };
        moves(L, false);
        moves(R, true);
        float worst = 0.0f, worstT = 0.0f, penDiff = 0.0f;
        int worstBone = 0;
        std::vector<Event> evL, evR;
        int evBad = 0;
        for (float t = 0; t < 4.5f; t += 1.0f / 120.0f) {
            evL.clear();
            evR.clear();
            L.update(1.0f / 120.0f, evL);
            R.update(1.0f / 120.0f, evR);
            if (evL.size() != evR.size()) ++evBad;
            for (size_t i = 0; i < std::min(evL.size(), evR.size()); ++i)
                if (evL[i].type != evR[i].type || std::fabs(evL[i].time - evR[i].time) > 1e-6f || length(evL[i].position - mirrorV(evR[i].position)) > 1e-4f) ++evBad;
            for (int i = 0; i < kPieces; ++i) {
                mat4 x;
                if (L.heldPieceTransform(i, x)) psL[i].xf = x;
                if (R.heldPieceTransform(i, x)) psR[i].xf = x;
            }
            for (const Event& e : evL)
                if (e.type == EventType::PieceReleased) psL[e.pieceId].xf = e.transform;
            for (const Event& e : evR)
                if (e.type == EventType::PieceReleased) psR[e.pieceId].xf = e.transform;
            for (int b = 0; b < BoneCount; ++b) {
                Bone m = Bone(b);
                if (b >= ClavicleL && b <= PinkyL3) m = Bone(b + (ClavicleR - ClavicleL));
                else if (b >= ClavicleR && b <= PinkyR3) m = Bone(b - (ClavicleR - ClavicleL));
                else if (b >= ThighL && b <= FootL) m = Bone(b + (ThighR - ThighL));
                else if (b >= ThighR && b <= FootR) m = Bone(b - (ThighR - ThighL));
                if (b == EyeL) m = EyeR;
                if (b == EyeR) m = EyeL;
                if (b == LidUpperL) m = LidUpperR;
                if (b == LidUpperR) m = LidUpperL;
                if (b == LidLowerL) m = LidLowerR;
                if (b == LidLowerR) m = LidLowerL;
                mat4 want = S * R.globals()[m] * S;
                const mat4& got = L.globals()[b];
                float d = length(got.translation() - want.translation());
                for (int c = 0; c < 3; ++c) d = std::max(d, 0.1f * length(vec3(got.c[c].x - want.c[c].x, got.c[c].y - want.c[c].y, got.c[c].z - want.c[c].z)));
                if (d > worst) { worst = d; worstT = t; worstBone = b; }
            }
            mat4 pl, pr;
            if (L.penTransform(pl) && R.penTransform(pr)) penDiff = std::max(penDiff, length(pl.translation() - mirrorV(pr.translation())));
        }
        const bool fail = worst > 1e-4f || evBad > 0 || penDiff > 1e-4f;
        ::logx::write(fail ? ::logx::Level::Warn : ::logx::Level::Info,
                      "selftest mirror: left-handed Black vs mirrored right-handed Black: worst bone %.4f mm (%s at t=%.2f), pen %.4f mm, %d event mismatches",
                      worst * 1000.0f, boneName(Bone(worstBone)), worstT, penDiff * 1000.0f, evBad);
    }

    // Handshake with a left-handed player still holding the pen: it lays the pen down first.
    {
        Piece ps[kPieces];
        initialPieces(ps);
        Animator W, B;
        setup(W, 0, ps, false);
        setup(B, 1, ps, true);
        WriteTask pick;
        pick.type = WriteTaskType::PickPen;
        pick.frame = penRestFrame(1);
        B.enqueueWriting(pick);
        Task h;
        h.type = TaskType::Handshake;
        Task wait;
        wait.type = TaskType::Wait;
        wait.duration = 1.0f;
        W.enqueue(wait);
        B.enqueue(wait);
        h.partner = &B;
        W.enqueue(h);
        h.partner = &W;
        B.enqueue(h);
        std::vector<Event> ev;
        float putAt = -1, claspW = -1, claspB = -1, palmGap = 0;
        bool heldAtClasp = true;
        for (float t = 0; t < 4.0f; t += 1.0f / 120.0f) {
            ev.clear();
            W.update(1.0f / 120.0f, ev);
            for (const Event& e : ev)
                if (e.type == EventType::HandshakeClasp) claspW = e.time;
            ev.clear();
            B.update(1.0f / 120.0f, ev);
            for (const Event& e : ev) {
                if (e.type == EventType::PenPut) putAt = e.time;
                if (e.type == EventType::HandshakeClasp) {
                    claspB = e.time;
                    heldAtClasp = B.holdsPen();
                    auto palm = [&](const mat4* g, Bone hand, float side) { return transformPoint(g[hand], vec3(side * 0.0135f, -0.052f, 0.003f)); };
                    palmGap = length(palm(W.globals(), HandR, 1.0f) - palm(B.globals(), HandR, 1.0f));
                }
            }
        }
        const bool fail = putAt < 0.0f || heldAtClasp || std::fabs(claspW - claspB) > 1e-5f || palmGap > 0.05f;
        ::logx::write(fail ? ::logx::Level::Warn : ::logx::Level::Info,
                      "selftest handshake with a left-handed player holding the pen: pen put down at t=%.3f, clasp %.3f / %.3f, right palms %.1f mm apart",
                      putAt, claspW, claspB, palmGap * 1000.0f);
    }
}

}  // namespace

SCACELITH_SCENE("anim", "Animation viewer: two debug robots, handshake, moves, captures, clock", AnimViewer);
