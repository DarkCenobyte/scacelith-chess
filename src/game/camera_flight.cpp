#include "camera_flight.h"
#include <algorithm>
#include <cmath>

using namespace m;

namespace game {

namespace {

vec3 bezier(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float s) {
    float r = 1.0f - s;
    return p0 * (r * r * r) + p1 * (3.0f * r * r * s) + p2 * (3.0f * r * s * s) + p3 * (s * s * s);
}

vec3 flatDir(vec3 v) {
    vec3 f(v.x, 0.0f, v.z);
    float l = length(f);
    return l > 1e-5f ? f / l : vec3(0.0f);
}

}  // namespace

quat CameraPose::orientation() const {
    return normalize(axisAngle(vec3(0, 1, 0), yaw) * axisAngle(vec3(1, 0, 0), pitch) * axisAngle(vec3(0, 0, 1), roll));
}

vec3 CameraPose::forward() const {
    float cp = std::cos(pitch);
    return vec3(-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp);
}

CameraPose CameraPose::looking(vec3 eye, vec3 target, float fov) {
    CameraPose p;
    p.position = eye;
    vec3 f = target - eye;
    float l = length(f);
    f = l > 1e-6f ? f / l : vec3(0, 0, -1);
    p.yaw = std::atan2(-f.x, -f.z);
    p.pitch = std::asin(clamp(f.y, -1.0f, 1.0f));
    p.roll = 0.0f;
    p.fovY = fov;
    return p;
}

CameraPose CameraPose::fromOrientation(vec3 position, quat q, float fov) {
    CameraPose p = looking(position, position + rotate(q, vec3(0, 0, -1)), fov);
    // Roll: angle of the real up vector around the view axis, from the level (no roll) up vector.
    quat level = p.orientation();
    vec3 up = rotate(q, vec3(0, 1, 0));
    vec3 r0 = rotate(level, vec3(1, 0, 0)), u0 = rotate(level, vec3(0, 1, 0));
    p.roll = std::atan2(-dot(up, r0), dot(up, u0));
    return p;
}

float angleDelta(float a, float b) {
    float d = std::fmod(b - a, 2.0f * PI);
    if (d > PI) d -= 2.0f * PI;
    if (d <= -PI) d += 2.0f * PI;
    return d;
}

void CameraFlight::start(const CameraPose& from, const CameraPose& to, float duration, const FlightShape& shape) {
    from_ = from;
    to_ = to;
    shape_ = shape;
    duration_ = duration > 0.0f ? duration : autoDuration(from, to);
    time_ = 0.0f;
    yawSpan_ = angleDelta(from.yaw, to.yaw);
    if (shape.yawTurn > 0 && yawSpan_ < 0.0f) yawSpan_ += 2.0f * PI;
    if (shape.yawTurn < 0 && yawSpan_ > 0.0f) yawSpan_ -= 2.0f * PI;
    active_ = true;
}

void CameraFlight::retarget(const CameraPose& to) {
    to_ = to;
    // Keep turning the same way: the new span is the one closest to the current span.
    float d = angleDelta(from_.yaw, to.yaw);
    while (d - yawSpan_ > PI) d -= 2.0f * PI;
    while (d - yawSpan_ < -PI) d += 2.0f * PI;
    yawSpan_ = d;
}

float CameraFlight::progress() const { return duration_ > 0.0f ? saturate(time_ / duration_) : 1.0f; }

CameraPose CameraFlight::sample(float u) const {
    float s = smootherstep(saturate(u));
    CameraPose p;
    p.position = bezier(from_.position, from_.position + shape_.departure, to_.position + shape_.arrival, to_.position, s);
    p.yaw = from_.yaw + yawSpan_ * s;
    p.pitch = lerp(from_.pitch, to_.pitch, s) - shape_.pitchDip * std::sin(PI * s);
    p.roll = from_.roll + angleDelta(from_.roll, to_.roll) * s;
    p.fovY = lerp(from_.fovY, to_.fovY, s);
    return p;
}

CameraPose CameraFlight::update(float dt) {
    if (!active_) return to_;
    time_ += std::max(0.0f, dt);
    if (time_ >= duration_) {
        active_ = false;
        return to_;
    }
    return sample(time_ / duration_);
}

float CameraFlight::autoDuration(const CameraPose& from, const CameraPose& to) {
    float dist = length(to.position - from.position);
    float turn = std::fabs(angleDelta(from.yaw, to.yaw)) + std::fabs(to.pitch - from.pitch);
    return clamp(0.55f + 0.42f * std::sqrt(dist) + 0.25f * turn, 0.7f, 2.6f);
}

FlightShape CameraFlight::handoverShape(const CameraPose& from, const CameraPose& to, vec3 centre) {
    FlightShape s;
    const float rise = 0.55f, reach = 0.22f;
    s.departure = flatDir(centre - from.position) * reach + vec3(0, rise, 0);
    s.arrival = flatDir(centre - to.position) * reach + vec3(0, rise, 0);
    s.pitchDip = 28.0f * DEG;
    s.yawTurn = 1;
    return s;
}

}  // namespace game
