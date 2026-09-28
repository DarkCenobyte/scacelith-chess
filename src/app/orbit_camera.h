// Orbit camera for viewer/test scenes: right-drag orbits, middle-drag pans, wheel zooms.
#pragma once
#include "../platform/platform.h"
#include "../render/renderer.h"

struct OrbitCamera {
    m::vec3 target{0, 0.8f, 0};
    float yaw = 0.0f, pitch = 0.35f, distance = 1.2f;
    float fovY = 40.0f * m::DEG;
    void update(const plat::Input& in) {
        if (in.mouseDown[plat::MOUSE_RIGHT]) { yaw -= in.mouseDX * 0.005f; pitch = m::clamp(pitch + in.mouseDY * 0.005f, -1.5f, 1.5f); }
        distance = m::clamp(distance * std::pow(0.9f, in.wheel), 0.05f, 60.0f);
        if (in.mouseDown[plat::MOUSE_MIDDLE]) {
            render::Camera c = camera();
            target += (c.right() * -in.mouseDX + c.up() * in.mouseDY) * (0.0015f * distance);
        }
    }
    render::Camera camera() const {
        render::Camera c;
        c.fovY = fovY;
        c.nearZ = std::min(0.03f, distance * 0.05f);
        c.position = target + m::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)) * distance;
        c.lookAt(target);
        return c;
    }
};
