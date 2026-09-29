#include "hotseat.h"
#include <algorithm>
#include <utility>

namespace game {
namespace hotseat {

Players Players::swapped() const {
    Players p = *this;
    std::swap(p.names[0], p.names[1]);
    std::swap(p.hands[0], p.hands[1]);
    p.clockRightOf = 1 - (clockRightOf & 1);
    return p;
}

void advanceClock(chess::Clock& clock, double& accumMs, float dt, bool frozen) {
    if (frozen || !clock.isRunning()) return;
    accumMs += double(std::max(0.0f, dt)) * 1000.0;
    int64_t ms = int64_t(accumMs);
    accumMs -= double(ms);
    clock.update(ms);
}

void Handover::start(int fromSeat, int toSeat, float duration, const CameraPose& fromPose, const CameraPose& toPose,
                     m::vec3 centre) {
    from_ = fromSeat & 1;
    to_ = toSeat & 1;
    elapsed_ = phaseTime_ = 0.0f;
    pose_ = fromPose;
    if (duration > 0.0f) {
        flightDuration_ = std::clamp(duration, kMinFlight, kMaxFlight);
        flight_.start(fromPose, toPose, flightDuration_, CameraFlight::handoverShape(fromPose, toPose, centre));
        phase_ = Phase::Flight;
    } else {
        flightDuration_ = 0.0f;
        flight_.cancel();
        phase_ = Phase::FadeOut;
    }
}

Handover::Step Handover::update(float dt, const CameraPose& toPose) {
    Step s;
    if (phase_ == Phase::Idle) return s;
    dt = std::max(0.0f, dt);
    elapsed_ += dt;
    phaseTime_ += dt;
    switch (phase_) {
    case Phase::Flight:
        flight_.retarget(toPose);
        pose_ = flight_.update(dt);
        if (!flight_.active()) {
            pose_ = toPose;
            phase_ = Phase::Idle;
            s.landed = true;
        }
        break;
    case Phase::FadeOut:
        if (phaseTime_ >= kFadeOut) {
            phase_ = Phase::FadeIn;
            phaseTime_ -= kFadeOut;
            pose_ = toPose;
            s.cut = true;
            // A very long frame may cross both fades at once.
            if (phaseTime_ >= kFadeIn) {
                phase_ = Phase::Idle;
                s.landed = true;
            }
        }
        break;
    case Phase::FadeIn:
        pose_ = toPose;
        if (phaseTime_ >= kFadeIn) {
            phase_ = Phase::Idle;
            s.landed = true;
        }
        break;
    default: break;
    }
    return s;
}

int Handover::viewSeat() const {
    switch (phase_) {
    case Phase::Flight: return -1;
    case Phase::FadeOut: return from_;
    default: return to_;
    }
}

float Handover::fade() const {
    switch (phase_) {
    case Phase::FadeOut: return m::smoothstep(0.0f, 1.0f, std::min(1.0f, phaseTime_ / kFadeOut));
    case Phase::FadeIn: return 1.0f - m::smoothstep(0.0f, 1.0f, std::min(1.0f, phaseTime_ / kFadeIn));
    default: return 0.0f;
    }
}

float Handover::totalTime() const { return flightDuration_ > 0.0f ? flightDuration_ : kFadeOut + kFadeIn; }

float Handover::progress() const {
    if (phase_ == Phase::Idle) return elapsed_ > 0.0f ? 1.0f : 0.0f;
    return std::min(1.0f, elapsed_ / totalTime());
}

}  // namespace hotseat
}  // namespace game
