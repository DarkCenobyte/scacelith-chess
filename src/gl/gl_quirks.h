// Drivers that list an OpenGL feature but draw it wrong, recognised by their GL_RENDERER string.
// gl_context.cpp turns such a feature off as if the driver had not listed it (caps());
// SCACELITH_GL_FORCE=<feature> keeps it on, to try a newer driver. Free of GL calls, for the tests.
#pragma once
#include <cstring>

namespace gl46 {

// Zink over KosmicKrisp (macOS): tessellation is new in KosmicKrisp with Mesa 26.2, emulated in
// compute shaders, and the Phong-tessellated pieces and robots come out with no triangles at all
// (Mesa 26.2.4, Apple M3 Pro, macOS 26, 2026-10-10). Zink names its Vulkan driver in the renderer
// string: "zink Vulkan 1.4(Apple M3 Pro (MESA_KOSMICKRISP))".
inline bool tessellationBroken(const char* renderer) {
    return renderer && std::strstr(renderer, "(MESA_KOSMICKRISP)") != nullptr;
}

}  // namespace gl46
