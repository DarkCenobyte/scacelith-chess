// Scratch: handshake plan geometry of W (right-handed, rr): clasp, pre-contact, rest, directions.
#include "geo.h"
int main() {
    const Skeleton& sk = robotSkeleton();
    anim::Animator W, B;
    W.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    B.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f);
    anim::Task h;
    h.type = anim::TaskType::Handshake;
    h.partner = &B;
    W.enqueue(h);
    h.partner = &W;
    B.enqueue(h);
    std::vector<anim::Event> ev;
    W.update(1.0f / 120.0f, ev);
    auto& I = *W.impl_;
    const Motion& mo = I.shakeHand().motion;
    float t = 0;
    for (size_t i = 0; i < mo.segs.size(); ++i) {
        const Segment& s = mo.segs[i];
        HandSample a = s.sample(0), b = s.sample(s.T);
        std::printf("seg %zu: t %.3f..%.3f p0 (%.3f %.3f %.3f) p1 (%.3f %.3f %.3f) |d| %.3f v1 (%.2f %.2f %.2f) he %.2f arc %.3f locked %d elbow %.2f->%.2f\n", i, t, t + s.T,
                    a.p.x, a.p.y, a.p.z, b.p.x, b.p.y, b.p.z, length(b.p - a.p), s.v1.x, s.v1.y, s.v1.z, s.he, s.arcH, int(s.locked), s.elbow0, s.elbow1);
        t += s.T;
    }
    const HandSample r = I.shakeHand().rest;
    std::printf("rest (%.3f %.3f %.3f)\n", r.p.x, r.p.y, r.p.z);
    return 0;
}
