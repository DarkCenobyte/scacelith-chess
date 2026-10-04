// Black right-handed vs Black left-handed (same White): the real right hand's bones (hand + fingers),
// largest position / rotation difference per phase.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "math/math.h"
#include "anim/animator.cpp"
#include "anim/animator_gesture.cpp"
#include "anim/animator_writing.cpp"
using namespace m;
using namespace character;
int main() {
    const Skeleton& sk = robotSkeleton();
    anim::Animator W1, B1, W2, B2;
    W1.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    B1.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, Side::Right);
    W2.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    B2.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, Side::Left);
    for (auto pr : {std::make_pair(&W1, &B1), std::make_pair(&W2, &B2)}) {
        anim::Task h;
        h.type = anim::TaskType::Handshake;
        h.partner = pr.second;
        pr.first->enqueue(h);
        h.partner = pr.first;
        pr.second->enqueue(h);
    }
    std::vector<anim::Event> ev;
    std::map<std::string, std::pair<float, float>> worst;
    while (W1.time() < anim::Timing::Handshake + 0.05f) {
        for (anim::Animator* a : {&W1, &B1, &W2, &B2}) {
            ev.clear();
            a->update(1.0f / 120.0f, ev);
        }
        const float u = W1.time();
        const char* ph = u < 0.68f ? "approach" : u < 2.18f ? "contact (slide-in .. withdraw)" : "retract";
        auto& w = worst[ph];
        for (int b = HandR; b <= PinkyR3; ++b) {
            const mat4 &x = B1.globals()[b], &y = B2.globals()[b];
            w.first = std::max(w.first, length(x.translation() - y.translation()));
            quat qa = fromMat3(x.upper3()), qb = fromMat3(y.upper3());
            w.second = std::max(w.second, 2.0f * std::acos(clamp(std::fabs(dot(qa, qb)), 0.0f, 1.0f)));
        }
    }
    for (auto& kv : worst) std::printf("%-32s Black's right hand, right- vs left-handed: %.3f mm, %.4f rad\n", kv.first.c_str(), kv.second.first * 1000, kv.second.second);
}
