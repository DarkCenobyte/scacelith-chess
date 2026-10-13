// Pieces of the 3D scene and of the characters that need no GPU: the registry of the scenes
// (src/app/scene.cpp), the chandelier's candle flames (src/scene/hall_decor.cpp), the chess
// pieces (src/scene/pieces.cpp), the robot's mirrored bones and eyelids (src/character/), and
// the mesh helpers (src/render/mesh.cpp).
#include "test.h"
#include "viewer_helpers.h"
#include "app/scene.h"
#include "character/robot.h"
#include "character/skeleton.h"
#include "game/layout.h"
#include "render/mesh.h"
#include "scene/hall.h"
#include "scene/pieces.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using viewer_test::LogCapture;
using viewer_test::nearly;

// Every viewer scene of the command line (--scene) is listed with a description, and made by name;
// an unknown name makes nothing.
TEST(scene_registry_lists_and_makes_the_scenes) {
    const std::vector<std::pair<std::string, std::string>> scenes = listScenes();
    for (const char* name : {"game", "anim", "robot", "hall", "tabletop", "materials", "lightbox", "testbed", "posttest", "scoresheet"}) {
        auto it = std::find_if(scenes.begin(), scenes.end(), [&](const std::pair<std::string, std::string>& s) { return s.first == name; });
        CHECK(it != scenes.end());
        if (it != scenes.end()) CHECK(!it->second.empty());
    }
    CHECK(std::is_sorted(scenes.begin(), scenes.end()));
    CHECK(createScene("robot") != nullptr);
    CHECK(createScene("no-such-scene") == nullptr);
    AppContext ctx;
    ctx.args = {"--scene", "hall", "--fov", "30", "--view"};
    CHECK(ctx.hasArg("--fov") && !ctx.hasArg("30x"));
    CHECK_EQ(ctx.argValue("--fov"), std::string("30"));
    CHECK_EQ(ctx.argValue("--view", "orbit"), std::string("orbit"));   // no value after it
    CHECK_EQ(ctx.argValue("--sun", "default"), std::string("default"));
}

// The chandelier's 18 flames: 12 on the lower ring (1.2 m round the axis, the first towards +X)
// and 6 on the upper ring between them, above their candles, scaled with the body about the ring.
TEST(scene_chandelier_candle_flames) {
    std::vector<m::vec3> flames = {m::vec3(1, 2, 3)};
    hall::chandelierCandles(flames);
    REQUIRE(flames.size() == 18u);
    const float s = hall::CHANDELIER_SCALE, above = 0.066f + 0.19f;
    for (size_t i = 0; i < flames.size(); ++i) {
        const bool lower = i < 12;
        const float r = std::hypot(flames[i].x, flames[i].z);
        CHECK(nearly(r, (lower ? 0.86f : 0.5f) * s));
        const float cupY = lower ? hall::CHANDELIER_Y : 6.36f;
        CHECK(nearly(flames[i].y, hall::CHANDELIER_Y + (cupY + above - hall::CHANDELIER_Y) * s));
    }
    CHECK(nearly(flames[0], m::vec3(0.86f * s, flames[0].y, 0.0f)));
    // The upper ring is turned half a step: its first flame at 30 degrees.
    CHECK(nearly(std::atan2(flames[12].z, flames[12].x), m::PI / 6.0f));
}

// A pawn stands on its felt and is as tall as the layout says; a type that is no piece makes
// nothing and is reported.
TEST(scene_pieces_built_by_type) {
    const PieceMeshes pawn = buildPiece(1);
    REQUIRE(!pawn.body.vertices.empty() && !pawn.felt.vertices.empty());
    float lo = 1e9f, hi = -1e9f;
    for (const Vertex& v : pawn.body.vertices) {
        lo = std::min(lo, v.pos.y);
        hi = std::max(hi, v.pos.y);
    }
    CHECK(lo > -0.002f && lo < 0.002f);
    CHECK(nearly(hi, layout::PIECE_HEIGHT[1], 0.002f));
    LogCapture log;
    CHECK(buildPiece(0).body.vertices.empty());
    CHECK(buildPiece(7).felt.vertices.empty());
    CHECK(log.has("buildPiece: invalid piece type 0"));
    CHECK(log.has("buildPiece: invalid piece type 7"));
}

// Left-side bones mirrored to the right (arms, fingers, legs, eyes, lids); the others unchanged.
TEST(scene_skeleton_mirrored_bones) {
    using namespace character;
    CHECK_EQ(sideBone(ThighL, Side::Right), ThighR);
    CHECK_EQ(sideBone(FootL, Side::Right), FootR);
    CHECK_EQ(sideBone(IndexL1, Side::Right), IndexR1);
    CHECK_EQ(sideBone(EyeL, Side::Right), EyeR);
    CHECK_EQ(sideBone(LidUpperL, Side::Right), LidUpperR);
    CHECK_EQ(sideBone(LidLowerL, Side::Right), LidLowerR);
    CHECK_EQ(sideBone(Head, Side::Right), Head);
    CHECK_EQ(sideBone(ThighL, Side::Left), ThighL);
    CHECK_EQ(std::string(boneName(BoneCount)), std::string("?"));
    CHECK(!std::string(boneName(Head)).empty());
}

// The eyelids: identity when open and looking ahead; closed, both margins meet just below the
// eye's axis; the upper lid follows the gaze up and down (at most half a radian), the lower one
// less; a closure beyond 1 is a closed eye.
TEST(scene_robot_eyelids_close_and_follow_the_gaze) {
    using namespace character;
    // The angle about +X of a lid's rotation, from where it takes +Y.
    auto angle = [](m::quat q) {
        const m::vec3 y = m::rotate(q, m::vec3(0, 1, 0));
        return std::atan2(y.z, y.y);
    };
    CHECK(nearly(angle(lidRotation(LidUpperL, 0.0f)), 0.0f, 1e-5f));
    CHECK(nearly(angle(lidRotation(LidLowerR, 0.0f)), 0.0f, 1e-5f));
    const float upperClosed = eye::LID_UPPER_OPEN - eye::LID_CLOSED, lowerClosed = -eye::LID_LOWER_OPEN - eye::LID_CLOSED;
    CHECK(nearly(angle(lidRotation(LidUpperR, 1.0f)), upperClosed, 1e-5f));
    CHECK(upperClosed > 0.0f);   // closing: positive for the upper lid
    CHECK(nearly(angle(lidRotation(LidLowerL, 1.0f)), lowerClosed, 1e-5f));
    CHECK(lowerClosed < 0.0f);   // negative for the lower one
    CHECK(nearly(angle(lidRotation(LidUpperL, 3.0f)), upperClosed, 1e-5f));
    CHECK(nearly(angle(lidRotation(LidUpperL, 0.0f, 0.3f)), -0.8f * 0.3f, 1e-5f));
    CHECK(nearly(angle(lidRotation(LidUpperL, 0.0f, 2.0f)), -0.8f * 0.5f, 1e-5f));
    CHECK(nearly(angle(lidRotation(LidLowerL, 0.0f, -0.3f)), 0.35f * 0.3f, 1e-5f));
    // Half closed: half way between open and closed.
    CHECK(nearly(angle(lidRotation(LidUpperL, 0.5f)), upperClosed * 0.5f, 1e-5f));
}

// Mesh helpers: smooth normals weighted by area, tangents along the texture's u, the faceted
// normals of unshared triangles, and an open cylinder (no caps) that is only its side.
TEST(scene_mesh_normals_tangents_and_open_cylinder) {
    MeshData quad;
    const m::vec3 corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 0, -1}, {0, 0, -1}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.pos = corners[i];
        v.uv = m::vec2(corners[i].x, -corners[i].z);
        quad.vertices.push_back(v);
    }
    quad.indices = {0, 1, 2, 0, 2, 3};
    quad.computeNormals();
    for (const Vertex& v : quad.vertices) CHECK(nearly(v.normal, m::vec3(0, 1, 0), 1e-5f));
    quad.computeTangents();
    for (const Vertex& v : quad.vertices) {
        CHECK(nearly(v.tangent.xyz(), m::vec3(1, 0, 0), 1e-5f));
        CHECK(v.tangent.w == 1.0f || v.tangent.w == -1.0f);
    }
    MeshData faceted = quad;
    faceted.computeNormals(false);
    CHECK_EQ(faceted.vertices.size(), size_t(6));
    CHECK_EQ(faceted.indices.size(), size_t(6));

    const MeshData open = prim::cylinder(0.1f, 0.3f, 16, false);
    const MeshData closed = prim::cylinder(0.1f, 0.3f, 16, true);
    REQUIRE(!open.vertices.empty());
    CHECK(open.indices.size() < closed.indices.size());
    for (const Vertex& v : open.vertices) {
        CHECK(nearly(std::hypot(v.pos.x, v.pos.z), 0.1f, 1e-4f));
        CHECK(nearly(v.normal.y, 0.0f, 1e-4f));   // the side faces outwards
    }
}
