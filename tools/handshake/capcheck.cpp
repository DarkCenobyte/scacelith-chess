// Calibrates the capsule check on the real two-robot handshake: per phase, the smallest margins.
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
#include "capsules.h"
using namespace m;
using namespace character;
int main(int argc, char** argv) {
    const Skeleton& sk = robotSkeleton();
    for (Side bs : {Side::Right, Side::Left}) {
        anim::Animator W, B;
        W.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
        B.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, bs);
        anim::Task h;
        h.type = anim::TaskType::Handshake;
        h.partner = &B;
        W.enqueue(h);
        h.partner = &W;
        B.enqueue(h);
        std::vector<anim::Event> ev;
        std::map<std::string, shakecheck::Margins> worst;
        auto mn = [](shakecheck::Margins& w, const shakecheck::Margins& m) {
            w.palm = std::min(w.palm, m.palm);
            w.cuff = std::min(w.cuff, m.cuff);
            w.forearm = std::min(w.forearm, m.forearm);
            w.fingers = std::min(w.fingers, m.fingers);
            w.thumbs = std::min(w.thumbs, m.thumbs);
            w.behind = std::min(w.behind, m.behind);
            w.fromWrist = std::min(w.fromWrist, m.fromWrist);
            w.toKnuckles = std::min(w.toKnuckles, m.toKnuckles);
            w.cross = std::max(w.cross, m.cross);
        };
        while (W.time() < anim::Timing::Handshake + 0.05f) {
            ev.clear();
            W.update(1.0f / 120.0f, ev);
            ev.clear();
            B.update(1.0f / 120.0f, ev);
            const float u = W.time();
            const char* ph = u < 0.5f ? "0far" : u < 0.68f ? "1approach" : u < 0.76f ? "2slide" : u < 0.92f ? "3close" : u < 1.90f ? "4pumps" : u < 2.08f ? "5open" : u < 2.18f ? "6withdraw" : "7retract";
            for (int k = 0; k < 2; ++k) {
                shakecheck::Margins m = k ? shakecheck::margins(sk, B.globals(), W.globals()) : shakecheck::margins(sk, W.globals(), B.globals());
                auto it = worst.find(ph);
                if (it == worst.end()) worst[ph] = m;
                else mn(it->second, m);
            }
        }
        std::printf("partner %s-handed (mm; capsule clearances, < 0 overlap)\n", bs == Side::Right ? "right" : "left");
        for (auto& kv : worst) {
            const auto& m = kv.second;
            std::printf("  %-10s palm %6.1f cuff %6.1f forearm %6.1f fingers %6.1f | thumbs %5.1f | pads behind %5.1f fromWrist %5.1f toKnuckles %5.1f | cross<= %.1f deg\n",
                        kv.first.c_str(), m.palm * 1000, m.cuff * 1000, m.forearm * 1000, m.fingers * 1000, m.thumbs * 1000, m.behind * 1000,
                        m.fromWrist * 1000, m.toKnuckles * 1000, m.cross / DEG);
        }
    }
    return 0;
}
