// "robot" viewer scene: the porcelain robot seated on a box seat in front of a table slab.
//
//   --pose rest|reach|hand   rest pose, right arm reaching to the table with a pinch, or the same
//                            pose framed like the first-person view (hand at ~30 cm)
//   --view full|face|side|back|hand|top   camera preset (hand = first-person-like close-up)
//   --blink <0..1>           eyelid closure (0 open, 1 closed)
//   --pupil <0..1>           pupil dilation
//   --gaze <yaw,pitch>       eye direction in degrees (+yaw = towards the robot's left)
//   --yaw/--pitch/--dist <v> orbit overrides (radians / meters), --fov <deg>
//   --target <x,y,z>         orbit centre override
//   --table                  show the table slab in the rest pose too
//   --only <substring>       debug: draw only parts whose name contains the substring
//   --wire                   debug: wireframe (triangle edges over the shells)
//   --fp                     first-person flags (hides the head, eyes, lids, neck top)
//   --sun <x,y,z>            direction towards the sun
//   --coach [text]           the coach's chest marking (ChestMarking; default text "COACH")
#include "robot.h"
#include "../app/orbit_camera.h"
#include "../app/scene.h"
#include "../core/log.h"
#include "../game/layout.h"
#include "../render/post/postfx.h"
#include <cstdio>
#include <cstdlib>

using namespace m;
using namespace character;

namespace {

vec3 parseVec3(const std::string& s, vec3 def) {
    vec3 v = def;
    if (!s.empty()) std::sscanf(s.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z);
    return v;
}

// ---- tiny posing helper (the animation package owns the real IK) ------------------------------
struct Poser {
    const Skeleton& sk = robotSkeleton();
    Pose pose;
    mat4 g[BoneCount];
    void update() { computeGlobal(sk, pose, g); }
    quat globalRot(Bone b) const { return fromMat3(g[b].upper3()); }
    vec3 pos(Bone b) const { return g[b].translation(); }
    // Sets the local rotation of b so that its global orientation becomes 'world'.
    void setGlobalRot(Bone b, quat world) {
        int p = sk.parent[b];
        quat parent = p < 0 ? pose.rootRotation : globalRot(Bone(p));
        pose.local[b] = normalize(conjugate(parent) * world);
        update();
    }
    // Two-bone IK for an arm: wrist to 'target', elbow hint direction 'pole' (world).
    void armIK(Side side, vec3 target, vec3 pole) {
        Bone ua = sideBone(UpperArmL, side), fa = sideBone(ForeArmL, side), hand = sideBone(HandL, side);
        update();
        vec3 s = pos(ua);
        float l1 = length(sk.restOffset[fa]), l2 = length(sk.restOffset[hand]);
        vec3 d = target - s;
        float dist = clamp(length(d), 0.05f, l1 + l2 - 1e-4f);
        vec3 dir = normalize(d);
        float cosA = clamp((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1.0f, 1.0f);
        vec3 bend = normalize(pole - dir * dot(pole, dir));
        vec3 elbow = s + (dir * cosA + bend * std::sqrt(1 - cosA * cosA)) * l1;
        vec3 upperDir = normalize(elbow - s), foreDir = normalize(target - elbow);
        // Upper arm: bone -Y onto upperDir, keep the elbow hinge axis (X) perpendicular to the plane.
        vec3 hinge = normalize(cross(upperDir, foreDir));
        if (length2(cross(upperDir, foreDir)) < 1e-8f) hinge = normalize(cross(upperDir, bend));
        // For the arm frames: -Y = bone direction, X = hinge (flexion about X brings the forearm to +Z).
        auto frame = [](vec3 boneDir, vec3 xAxis) {
            vec3 y = -boneDir;
            vec3 x = normalize(xAxis - y * dot(xAxis, y));
            vec3 z = cross(x, y);
            return fromMat3(mat3(x, y, z));
        };
        // Flexion about +X maps -Y to... forearm must bend towards the front of the upper arm (+Z).
        // Choose the hinge sign so the forearm lies on the +Z side of the upper arm frame.
        quat qUa = frame(upperDir, hinge);
        vec3 zUa = rotate(qUa, vec3(0, 0, 1));
        if (dot(zUa, foreDir) < 0) qUa = frame(upperDir, -hinge);
        setGlobalRot(ua, qUa);
        vec3 xUa = rotate(globalRot(ua), vec3(1, 0, 0));
        setGlobalRot(fa, frame(foreDir, xUa));
    }
    void curl(Side side, Bone leftFinger1, float a1, float a2, float a3, float spread = 0.0f) {
        vec3 ax = fingerCurlAxis(side);
        Bone b = sideBone(leftFinger1, side);
        float sp = side == Side::Right ? spread : -spread;
        pose.local[b] = axisAngle(vec3(1, 0, 0), sp) * axisAngle(ax, a1);
        pose.local[Bone(b + 1)] = axisAngle(ax, a2);
        pose.local[Bone(b + 2)] = axisAngle(ax, a3);
    }
};

class RobotViewer : public Scene {
public:
    bool init(AppContext& ctx) override {
        materials::init();
        setupRobotMaterials();
        parts_ = buildRobot();
        std::string only = ctx.argValue("--only");
        if (!only.empty()) {
            std::vector<RobotPart> keep;
            for (auto& p : parts_)
                if (std::string(p.name).find(only) != std::string::npos) keep.push_back(p);
            parts_.swap(keep);
        }
        if (ctx.hasArg("--wire")) {
            // Debug: unshared vertices with barycentrics in uv, drawn with the wire material.
            for (auto& p : parts_) {
                MeshData w;
                for (size_t k = 0; k < p.mesh.indices.size(); ++k) {
                    Vertex v = p.mesh.vertices[p.mesh.indices[k]];
                    v.uv = k % 3 == 0 ? vec2(1, 0) : (k % 3 == 1 ? vec2(0, 1) : vec2(0, 0));
                    w.vertices.push_back(v);
                    w.indices.push_back(uint32_t(k));
                }
                p.mesh = std::move(w);
            }
            wire_ = true;
            wireMat_[0].surface = wireMat_[1].surface = "shaders/materials/robot_wire.glsl";
            wireMat_[0].params[0] = vec4(0.85f, 0.85f, 0.83f, 0);
            wireMat_[1].params[0] = vec4(0.15f, 0.15f, 0.17f, 0);
        }
        gpu_.upload(parts_);
        LOGI("robot viewer: %d draws, %u triangles", int(gpu_.parts.size()), unsigned(gpu_.triangleCount));
        if (ctx.hasArg("--coach")) {
            std::string text = ctx.argValue("--coach");
            if (text.empty() || text[0] == '-') text = "COACH";
            marking_.create(text);
        }

        // Props: floor, seat block, table slab.
        floor_.upload(prim::plane(8, 8, 1, 1, 1.0f), "floor");
        seat_.upload(prim::roundedBox(vec3(0.16f, layout::SEAT_HEIGHT * 0.5f, 0.21f), 0.012f, 3), "seat");
        table_.upload(prim::roundedBox(vec3(0.60f, layout::TABLE_TOP_THICKNESS * 0.5f, 0.43f), 0.006f, 3), "table");
        floorMat_.params[0] = vec4(0.42f, 0.40f, 0.38f, 0.35f);
        floorMat_.params[1] = vec4(0, 0.5f, 1.0f, 0.08f);
        floorMat_.planarReflector = 0;
        seatMat_.params[0] = vec4(0.05f, 0.035f, 0.03f, 0.55f);
        seatMat_.params[1] = vec4(0, 0.5f, 0.3f, 0.2f);
        tableMat_.params[0] = vec4(0.10f, 0.045f, 0.025f, 0.3f);
        tableMat_.params[1] = vec4(0, 0.5f, 1.0f, 0.04f);
        render::PlanarReflector pr;
        render::renderer().addPlanarReflector(pr);

        poseName_ = ctx.argValue("--pose", "rest");
        view_ = ctx.argValue("--view", poseName_ == "hand" ? "hand" : "full");
        blink_ = float(std::atof(ctx.argValue("--blink", "0").c_str()));
        pupil_ = float(std::atof(ctx.argValue("--pupil", "0.35").c_str()));
        vec3 gz = parseVec3(ctx.argValue("--gaze"), vec3(0));
        gazeYaw_ = gz.x * DEG;
        gazePitch_ = gz.y * DEG;
        firstPerson_ = ctx.hasArg("--fp");
        sunDir_ = normalize(parseVec3(ctx.argValue("--sun"), vec3(0.45f, 0.75f, 0.65f)));
        showTable_ = poseName_ != "rest" || ctx.hasArg("--table");
        buildPose();
        setupCamera(ctx);
        return true;
    }

    void buildPose() {
        Poser& P = poser_;
        P.pose = Pose();
        P.pose.rootPosition = vec3(0, layout::PLAYER_PELVIS_Y, 0);
        P.update();
        if (poseName_ == "reach" || poseName_ == "hand") {
            // Lean slightly towards the table, look at the hand.
            P.pose.local[Spine1] = axisAngle(vec3(1, 0, 0), 0.06f);
            P.pose.local[Spine2] = axisAngle(vec3(1, 0, 0), 0.05f);
            P.pose.local[Neck] = axisAngle(vec3(1, 0, 0), 0.20f);
            P.pose.local[Head] = axisAngle(vec3(1, 0, 0), 0.25f) * axisAngle(vec3(0, 1, 0), -0.12f);
            P.update();
            // Right hand pinching a piece on the table in front of the robot.
            vec3 pinch(-0.05f, layout::TABLE_TOP_Y + 0.045f, 0.36f);
            vec3 palmDir = normalize(vec3(0.30f, -0.80f, -0.30f));
            vec3 fingerDir = normalize(vec3(0.10f, -0.45f, 0.88f));
            vec3 yAx = -fingerDir;
            vec3 xAx = normalize(palmDir - yAx * dot(palmDir, yAx));
            vec3 zAx = cross(xAx, yAx);
            quat handRot = fromMat3(mat3(xAx, yAx, zAx));
            // Wrist position so that the fingertips land near the pinch point.
            vec3 wrist = pinch - rotate(handRot, vec3(0.018f, -0.125f, 0.018f));
            P.armIK(Side::Right, wrist, normalize(vec3(-0.8f, -0.5f, -0.1f)));
            P.setGlobalRot(HandR, handRot);
            P.curl(Side::Right, IndexL1, 0.55f, 0.55f, 0.30f, 0.02f);
            P.curl(Side::Right, MiddleL1, 0.75f, 0.85f, 0.45f, 0.0f);
            P.curl(Side::Right, RingL1, 0.95f, 1.05f, 0.55f, -0.05f);
            P.curl(Side::Right, PinkyL1, 1.10f, 1.10f, 0.60f, -0.10f);
            P.pose.local[ThumbR1] = axisAngle(vec3(0, 1, 0), -0.35f) * axisAngle(vec3(1, 0, 0), 0.25f) * axisAngle(vec3(0, 0, 1), 0.35f);
            P.pose.local[ThumbR2] = axisAngle(vec3(1, 0, 0), 0.25f);
            P.pose.local[ThumbR3] = axisAngle(vec3(1, 0, 0), 0.35f);
            // Left hand resting on the thigh.
            P.armIK(Side::Left, vec3(0.16f, layout::PLAYER_PELVIS_Y + 0.02f, 0.25f), normalize(vec3(0.8f, -0.4f, -0.3f)));
            P.pose.local[HandL] = axisAngle(vec3(1, 0, 0), -0.9f) * axisAngle(vec3(0, 0, 1), -0.3f);
            P.curl(Side::Left, IndexL1, 0.30f, 0.35f, 0.20f);
            P.curl(Side::Left, MiddleL1, 0.35f, 0.40f, 0.22f);
            P.curl(Side::Left, RingL1, 0.40f, 0.45f, 0.25f);
            P.curl(Side::Left, PinkyL1, 0.45f, 0.50f, 0.28f);
            P.update();
        }
        // Eyes and lids.
        quat eye = axisAngle(vec3(0, 1, 0), gazeYaw_) * axisAngle(vec3(1, 0, 0), -gazePitch_);
        P.pose.local[EyeL] = eye;
        P.pose.local[EyeR] = eye;
        for (Bone b : {LidUpperL, LidUpperR, LidLowerL, LidLowerR}) P.pose.local[b] = lidRotation(b, blink_, gazePitch_);
        P.update();
    }

    void setupCamera(AppContext& ctx) {
        vec3 eyeMid = (poser_.pos(EyeL) + poser_.pos(EyeR)) * 0.5f;
        cam_.fovY = 35.0f * DEG;
        if (view_ == "face") {
            cam_.target = eyeMid - vec3(0, 0.03f, 0.0f);
            cam_.distance = 0.42f;
            cam_.yaw = 0.30f;
            cam_.pitch = 0.02f;
            cam_.fovY = 28.0f * DEG;
        } else if (view_ == "side") {
            cam_.target = vec3(0, 0.85f, 0.15f);
            cam_.distance = 2.6f;
            cam_.yaw = -1.5708f;
            cam_.pitch = 0.08f;
        } else if (view_ == "back") {
            cam_.target = vec3(0, 0.9f, 0.0f);
            cam_.distance = 2.4f;
            cam_.yaw = 3.4f;
            cam_.pitch = 0.2f;
        } else if (view_ == "top") {
            cam_.target = vec3(0, 0.9f, 0.1f);
            cam_.distance = 2.4f;
            cam_.yaw = 0.3f;
            cam_.pitch = 1.1f;
        } else if (view_ == "hand") {
            // First-person-like framing: from the eyes towards the right hand, 30 cm away.
            vec3 h = transformPoint(poser_.g[HandR], vec3(0.004f, -0.070f, 0.012f));
            vec3 toEye = normalize(eyeMid - h);
            cam_.target = h;
            cam_.distance = 0.30f;
            cam_.yaw = std::atan2(toEye.x, toEye.z);
            cam_.pitch = std::asin(clamp(toEye.y, -1.0f, 1.0f));
            cam_.fovY = 50.0f * DEG;
        } else {
            cam_.target = vec3(0, 0.88f, 0.12f);
            cam_.distance = 2.5f;
            cam_.yaw = 0.55f;
            cam_.pitch = 0.12f;
        }
        std::string v;
        if (!(v = ctx.argValue("--yaw")).empty()) cam_.yaw = float(std::atof(v.c_str()));
        if (!(v = ctx.argValue("--pitch")).empty()) cam_.pitch = float(std::atof(v.c_str()));
        if (!(v = ctx.argValue("--dist")).empty()) cam_.distance = float(std::atof(v.c_str()));
        if (!(v = ctx.argValue("--fov")).empty()) cam_.fovY = float(std::atof(v.c_str())) * DEG;
        if (!(v = ctx.argValue("--target")).empty()) cam_.target = parseVec3(v, cam_.target);
    }

    bool update(AppContext& ctx, float dt) override {
        time_ = ctx.fixedTime >= 0 && ctx.screenshotMode ? ctx.fixedTime : time_ + dt;
        cam_.update(plat::input());
        const plat::Input& in = plat::input();
        if (in.keyPressed[int('B')]) { blink_ = blink_ > 0.5f ? 0.0f : 1.0f; buildPose(); }
        return !in.keyPressed[plat::KEY_ESCAPE];
    }

    void render(AppContext& ctx, float dt) override {
        render::Renderer& r = *ctx.renderer;
        render::Environment env;
        env.time = time_;
        env.sunDirection = sunDir_;
        r.post().settings.dofFocusDistance = cam_.distance;  // in focus at the orbit target
        r.beginFrame(cam_.camera(), env, dt);
        render::DrawItem d;
        d.mesh = &floor_;
        d.material = &floorMat_;
        d.flags = render::DRAW_STATIC;
        r.submit(d);
        render::DrawItem seat;
        seat.mesh = &seat_;
        seat.material = &seatMat_;
        seat.model = translate(vec3(0, layout::SEAT_HEIGHT * 0.5f, -0.02f));
        seat.flags = render::DRAW_STATIC | render::DRAW_CAST_SHADOW;
        r.submit(seat);
        if (showTable_) {
            render::DrawItem t;
            t.mesh = &table_;
            t.material = &tableMat_;
            t.model = translate(vec3(0, layout::TABLE_TOP_Y - layout::TABLE_TOP_THICKNESS * 0.5f, 0.60f));
            t.flags = render::DRAW_STATIC | render::DRAW_CAST_SHADOW;
            r.submit(t);
        }
        render::PointLight rim;
        rim.position = vec3(-1.2f, 1.9f, -1.4f);
        rim.radius = 8.0f;
        rim.color = vec3(0.85f, 0.9f, 1.0f);
        rim.intensity = 2.5e4f;
        r.addLight(rim);
        if (wire_) {
            for (size_t i = 0; i < gpu_.parts.size(); ++i) {
                render::DrawItem w;
                w.mesh = &gpu_.parts[i].mesh;
                w.material = &wireMat_[gpu_.parts[i].material == MaterialId::RobotJoint ? 1 : 0];
                w.model = poser_.g[gpu_.parts[i].bone];
                r.submit(w);
            }
        } else {
            submitRobot(r, gpu_, poser_.g, firstPerson_, 100, nullptr, pupil_, 1.0f, Side::Right,
                        marking_.valid() ? &marking_.material : nullptr);
        }
        r.endFrame();
    }

    void shutdown(AppContext&) override {
        gpu_.destroy();
        marking_.destroy();
        floor_.destroy();
        seat_.destroy();
        table_.destroy();
    }

private:
    std::vector<RobotPart> parts_;
    GpuRobot gpu_;
    ChestMarking marking_;
    Poser poser_;
    Mesh floor_, seat_, table_;
    Material floorMat_, seatMat_, tableMat_, wireMat_[2];
    bool wire_ = false;
    OrbitCamera cam_;
    std::string poseName_, view_;
    float blink_ = 0, pupil_ = 0.35f, gazeYaw_ = 0, gazePitch_ = 0, time_ = 0;
    bool firstPerson_ = false, showTable_ = false;
    vec3 sunDir_;
};

}  // namespace

SCACELITH_SCENE("robot", "Porcelain robot viewer (--pose rest|reach|hand, --view face|hand|side, --blink)", RobotViewer);
