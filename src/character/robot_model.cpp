// Robot assembly: runs the part builders, mirrors the right side onto the left, merges parts that
// share bone + material + parameters (fewer draw calls) and reports the triangle budget.
#include "robot_build.h"
#include "../core/log.h"
#include "../platform/platform.h"
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

using namespace m;

namespace character {
namespace build {

vec4 seamPlane(vec3 n, vec3 p) {
    n = normalize(n);
    return vec4(n, -dot(n, p));
}

Bone mirrorBone(Bone b) {
    if (b >= ClavicleR && b <= PinkyR3) return Bone(b - (ClavicleR - ClavicleL));
    if (b >= ThighR && b <= FootR) return Bone(b - (ThighR - ThighL));
    if (b == EyeR) return EyeL;
    if (b == LidUpperR) return LidUpperL;
    if (b == LidLowerR) return LidLowerL;
    return b;
}

void Sink::add(const char* name, Bone b, MaterialId mat, MeshJob job, bool fpHidden, const vec4* inst) {
    RobotPart p;
    p.bone = b;
    p.material = mat;
    p.firstPersonHidden = fpHidden;
    p.name = name;
    if (inst)
        for (int k = 0; k < 4; ++k) p.inst[k] = inst[k];
    parts.push_back(std::move(p));
    jobs.push_back(std::move(job));
    mirrorOf.push_back(-1);
}

void Sink::addPorcelain(const char* name, Bone b, MeshJob job, bool fpHidden, const PorcelainLook& look, MaterialId mat) {
    vec4 inst[4] = {vec4(look.variant, look.coat, look.roughOffset, look.seamWidth), look.seams[0], look.seams[1], look.seams[2]};
    add(name, b, mat, std::move(job), fpHidden, inst);
}

void Sink::addJoint(const char* name, Bone b, MeshJob job, bool fpHidden, float variant) {
    vec4 inst[4] = {vec4(variant, 0, 0, 0), vec4(0), vec4(0), vec4(0)};
    add(name, b, MaterialId::RobotJoint, std::move(job), fpHidden, inst);
}

void Sink::mirrorFrom(size_t from) {
    size_t end = parts.size();
    for (size_t i = from; i < end; ++i) {
        RobotPart p = parts[i];
        p.bone = mirrorBone(p.bone);
        if (p.material == MaterialId::RobotPorcelain || p.material == MaterialId::RobotLid)
            for (int k = 1; k < 4; ++k) p.inst[k].x = -p.inst[k].x;
        parts.push_back(std::move(p));
        jobs.push_back(nullptr);
        mirrorOf.push_back(int(i));
    }
}

void Sink::run(int threads) {
    // A shared counter keeps every worker busy. Jobs are handed out from the last one: the head is
    // declared last and its face and skull jobs are by far the longest (the face alone is about a
    // third of the whole build), so they start first instead of running alone at the end.
    std::atomic<size_t> next{0};
    const size_t n = parts.size();
    auto worker = [&]() {
        for (;;) {
            size_t k = next.fetch_add(1);
            if (k >= n) break;
            size_t i = n - 1 - k;
            if (jobs[i]) parts[i].mesh = jobs[i]();
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (mirrorOf[i] < 0) continue;
        MeshData m = parts[size_t(mirrorOf[i])].mesh;
        m.transform(scale(vec3(-1, 1, 1)));
        parts[i].mesh = std::move(m);
    }
    jobs.clear();
}

}  // namespace build

std::vector<RobotPart> buildRobot() {
    double t0 = plat::time();
    build::Sink s;
    build::buildHand(s);
    build::buildArm(s);
    build::buildTorso(s);
    build::buildLegs(s);
    build::buildHead(s);
    int threads = int(std::thread::hardware_concurrency());
    if (const char* e = std::getenv("SCACELITH_ROBOT_THREADS")) threads = std::atoi(e);
    s.run(std::max(1, std::min(threads, 16)));
    if (std::getenv("SCACELITH_ROBOT_STATS"))
        for (auto& p : s.parts) LOGI("  part %-12s %-10s %7d tris", p.name, boneName(p.bone), int(p.mesh.indices.size() / 3));
    // Merge parts sharing bone, material, visibility and instance parameters.
    std::vector<RobotPart> out;
    for (auto& p : s.parts) {
        RobotPart* dst = nullptr;
        for (auto& o : out)
            if (o.bone == p.bone && o.material == p.material && o.firstPersonHidden == p.firstPersonHidden &&
                std::memcmp(o.inst, p.inst, sizeof(p.inst)) == 0) {
                dst = &o;
                break;
            }
        if (dst) dst->mesh.append(p.mesh);
        else out.push_back(std::move(p));
    }
    size_t tris = 0, verts = 0;
    for (auto& p : out) {
        tris += p.mesh.indices.size() / 3;
        verts += p.mesh.vertices.size();
    }
    LOGI("robot: %d parts (%d draws), %d triangles, %d vertices, built in %.0f ms", int(s.parts.size()), int(out.size()), int(tris), int(verts),
         (plat::time() - t0) * 1000.0);
    return out;
}

}  // namespace character
