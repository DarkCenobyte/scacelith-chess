#include "observer_camera.h"
#include "layout.h"
#include <cmath>

using namespace m;

namespace game {

namespace {
constexpr float kPitchLimit = 88.0f * DEG;
constexpr float kLookRadiansPerPixel = 0.0022f;  // same feel as the seated player's mouse look
constexpr float kMargin = 0.35f;                 // from the walls and the ceiling
constexpr float kMinHeight = 0.25f;              // above the floor
}  // namespace

bool ObserverCamera::Controls::any() const {
    return length2(move) > 0.0f || lookX != 0.0f || lookY != 0.0f;
}

ObserverCamera::Controls ObserverCamera::read(const plat::Input& in, bool keys, bool looking) {
    Controls c;
    if (keys) {
        auto down = [&in](int k) { return in.keyDown[k]; };
        float fwd = (down('W') || down('Z') || down(plat::KEY_UP)) ? 1.0f : 0.0f;
        float back = (down('S') || down(plat::KEY_DOWN)) ? 1.0f : 0.0f;
        float left = (down('A') || down('Q') || down(plat::KEY_LEFT)) ? 1.0f : 0.0f;
        float right = (down('D') || down(plat::KEY_RIGHT)) ? 1.0f : 0.0f;
        float up = (down('E') || down(plat::KEY_SPACE) || down(plat::KEY_PAGEUP)) ? 1.0f : 0.0f;
        float dn = (down('C') || down(plat::KEY_LCTRL) || down(plat::KEY_RCTRL) || down(plat::KEY_PAGEDOWN)) ? 1.0f : 0.0f;
        c.move = vec3(right - left, up - dn, fwd - back);
        c.fast = down(plat::KEY_LSHIFT) || down(plat::KEY_RSHIFT);
    }
    if (looking) {
        c.lookX = in.mouseDX;
        c.lookY = in.mouseDY;
    }
    c.wheel = in.wheel;
    return c;
}

ObserverCamera::ObserverCamera()
    : lo_(layout::HALL_MIN_X + kMargin, kMinHeight, layout::HALL_MIN_Z + kMargin),
      hi_(layout::HALL_MAX_X - kMargin, layout::HALL_HEIGHT - kMargin, layout::HALL_MAX_Z - kMargin) {}

void ObserverCamera::setBounds(vec3 lo, vec3 hi) {
    lo_ = lo;
    hi_ = hi;
    clampToBounds();
}

void ObserverCamera::setPose(const CameraPose& p) {
    pose_ = p;
    flight_.cancel();
    velocity_ = vec3(0.0f);
    clampToBounds();
}

void ObserverCamera::flyTo(const CameraPose& p, float duration, const FlightShape& shape) {
    flight_.start(pose_, p, duration, shape);
    velocity_ = vec3(0.0f);
}

void ObserverCamera::retarget(const CameraPose& p) {
    if (flight_.active()) flight_.retarget(p);
    else pose_ = p;
}

void ObserverCamera::setSpeed(float s) { speed_ = clamp(s, kMinSpeed, kMaxSpeed); }

void ObserverCamera::update(float dt, const Controls& c, float sensitivity, bool invertY) {
    if (c.wheel != 0.0f) setSpeed(speed_ * std::pow(1.25f, c.wheel));
    if (flight_.active()) {
        if (!c.any()) {
            pose_ = flight_.update(dt);
            clampToBounds();
            return;
        }
        flight_.cancel();  // the observer takes over where the flight is
    }
    float k = kLookRadiansPerPixel * sensitivity;
    pose_.yaw -= c.lookX * k;
    pose_.pitch = clamp(pose_.pitch - c.lookY * k * (invertY ? -1.0f : 1.0f), -kPitchLimit, kPitchLimit);
    pose_.yaw = std::remainder(pose_.yaw, 2.0f * PI);
    // A manual look straightens a tilted first-person pose.
    if (c.lookX != 0.0f || c.lookY != 0.0f) pose_.roll *= std::exp(-dt * 8.0f);

    // Fly-through: forward follows the view (pitch included), strafing stays level, up/down is
    // the world vertical. The velocity eases in and out so the view never jerks.
    vec3 fwd = pose_.forward();
    vec3 right(std::cos(pose_.yaw), 0.0f, -std::sin(pose_.yaw));
    vec3 dir = fwd * c.move.z + right * c.move.x + vec3(0, 1, 0) * c.move.y;
    float len = length(dir);
    if (len > 1.0f) dir = dir / len;
    vec3 target = dir * (speed_ * (c.fast ? 3.0f : 1.0f));
    velocity_ = velocity_ + (target - velocity_) * (1.0f - std::exp(-dt * 10.0f));
    if (length2(velocity_) < 1e-8f) velocity_ = vec3(0.0f);
    pose_.position = pose_.position + velocity_ * dt;
    clampToBounds();
}

void ObserverCamera::clampToBounds() {
    vec3& p = pose_.position;
    p = vec3(clamp(p.x, lo_.x, hi_.x), clamp(p.y, lo_.y, hi_.y), clamp(p.z, lo_.z, hi_.z));
}

}  // namespace game
