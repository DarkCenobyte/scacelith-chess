// Scratch: the cancel test's chest jerk, frame by frame (left- or right-handed B cut at CUT).
#include "geo.h"
int main(int argc, char** argv) {
    const Skeleton& sk = robotSkeleton();
    const bool left = argc > 1 && std::string(argv[1]) == "left";
    const float cut = argc > 2 ? std::atof(argv[2]) : 1.2f;
    anim::Animator W, B;
    W.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    B.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, left ? Side::Left : Side::Right);
    anim::Task h;
    h.type = anim::TaskType::Handshake;
    h.partner = &B;
    W.enqueue(h);
    h.partner = &W;
    B.enqueue(h);
    const float dt = 1.0f / 120.0f;
    std::vector<anim::Event> ev;
    bool cutDone = false;
    vec3 chest = B.globals()[Spine2].translation(), step(0.0f), prevHand = B.globals()[HandR].translation();
    while (B.time() < anim::Timing::Handshake + 0.3f) {
        if (!cutDone && B.time() >= cut - 1e-4f) {
            B.cancelTasks();
            anim::Task back;
            back.type = anim::TaskType::Retract;
            B.enqueue(back);
            cutDone = true;
        }
        ev.clear();
        W.update(dt, ev);
        ev.clear();
        B.update(dt, ev);
        const float t = B.time();
        const vec3 c = B.globals()[Spine2].translation();
        const float jerk = length((c - chest) - step);
        step = c - chest;
        chest = c;
        const vec3 hp = B.globals()[HandR].translation();
        const float hs = length(hp - prevHand) / dt;
        prevHand = hp;
        if (t > cut - 0.02f && t < cut + 0.5f)
            std::printf("t=%.3f chest jerk %.3f mm step %.2f mm  hand speed %.2f m/s  pelvis-chest (%.3f %.3f %.3f)\n", t, jerk * 1000, length(step) * 1000, hs, c.x, c.y, c.z);
    }
    return 0;
}
