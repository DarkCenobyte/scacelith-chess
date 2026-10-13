// Helpers of the tests of the viewer scenes, the renderer and the 3D scene (tests/viewer_*.cpp,
// render_*.cpp, scene_*.cpp): the game's log captured while a scene runs (the viewer scenes report
// their timelines and self tests there), vectors and cameras compared with a tolerance, mouse drags.
#pragma once
#include "fake_platform.h"
#include "headless.h"
#include "core/log.h"
#include "math/math.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace viewer_test {

// The log lines (core/log.h) written while it lives go to a file of a TempDir as well as to stderr;
// text() stops the capture and answers them. No other log file may be open meanwhile (the tests
// never open one: only main.cpp does).
class LogCapture {
public:
    LogCapture() : dir_("viewer-log") { open_ = logx::init(dir_.file("log.txt").c_str()); }
    ~LogCapture() { stop(); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    const std::string& text() {
        stop();
        return text_;
    }
    bool has(const std::string& s) { return text().find(s) != std::string::npos; }
    // How many times 's' appears.
    int count(const std::string& s) {
        int n = 0;
        for (size_t at = text().find(s); at != std::string::npos; at = text_.find(s, at + s.size())) ++n;
        return n;
    }
    // Whether every part appears, each one after the previous one.
    bool inOrder(std::initializer_list<const char*> parts) {
        size_t at = 0;
        for (const char* p : parts) {
            at = text().find(p, at);
            if (at == std::string::npos) return false;
            at += std::string(p).size();
        }
        return true;
    }
    // The lines that contain 's'.
    std::vector<std::string> lines(const std::string& s) {
        std::vector<std::string> out;
        size_t at = 0;
        const std::string& t = text();
        while (at < t.size()) {
            size_t end = t.find('\n', at);
            if (end == std::string::npos) end = t.size();
            std::string line = t.substr(at, end - at);
            if (line.find(s) != std::string::npos) out.push_back(line);
            at = end + 1;
        }
        return out;
    }

private:
    void stop() {
        if (!open_) return;
        logx::shutdown();
        open_ = false;
        std::ifstream in(dir_.file("log.txt"), std::ios::binary);
        text_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    headless::TempDir dir_;
    bool open_ = false;
    std::string text_;
};

inline bool nearly(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
inline bool nearly(m::vec3 a, m::vec3 b, float eps = 1e-3f) { return m::length(a - b) <= eps; }

// Where OrbitCamera (src/app/orbit_camera.h) puts the eye for these angles (radians) and distance.
inline m::vec3 orbitEye(m::vec3 target, float yaw, float pitch, float distance) {
    return target + m::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)) * distance;
}

// Whether the renderer's last camera stood at 'eye', looked at 'target' and was 'fovDeg' wide.
inline bool sees(const render::Camera& c, m::vec3 eye, m::vec3 target, float fovDeg) {
    return nearly(c.position, eye) && nearly(c.forward(), m::normalize(target - eye), 1e-4f) && nearly(c.fovY / m::DEG, fovDeg, 0.01f);
}
// The same for an orbit camera's angles (radians) and distance.
inline bool orbits(const render::Camera& c, m::vec3 target, float yaw, float pitch, float distance, float fovDeg) {
    return sees(c, orbitEye(target, yaw, pitch, distance), target, fovDeg);
}

// A drag with a mouse button held (plat::MOUSE_RIGHT orbits the viewers' cameras, MOUSE_MIDDLE
// pans them): three frames.
inline void drag(headless::App& app, int button, float x0, float y0, float x1, float y1) {
    app.moveMouse(x0, y0);
    plat::Input& in = fakeplat::next();
    in.mouseDown[button] = true;
    app.moveMouse(x1, y1);
    in.mouseDown[button] = false;
    app.frame();
}

// A key held for one frame while the frame's result (false: the scene asks to quit) is answered.
inline bool frameWithKey(headless::App& app, int key) {
    plat::Input& in = fakeplat::next();
    in.keyDown[key] = in.keyPressed[key] = true;
    const bool running = app.frame();
    in.keyDown[key] = false;
    return running;
}

}  // namespace viewer_test
