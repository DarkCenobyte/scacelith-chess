// Context bootstrap helpers usable without including the GL headers (the X11 platform layer
// cannot include glcorearb.h next to GL/glx.h).
#pragma once

namespace gl46 {
typedef void* (*GetProcFn)(const char* name);
int load(GetProcFn getProc, const char** firstMissing);
// Logs driver strings, reads the driver's capabilities (caps()), installs the KHR_debug callback
// when 'debug' is set, and applies global state every module relies on (reverse-Z clip control,
// seamless cubemaps).
void afterContextCreated(bool debug);

// Features of OpenGL 4.6 the engine can do without. A conforming 4.6 driver has them all, but the
// announced version is not proof: on macOS, Mesa's Zink runs on Apple's Vulkan drivers and reports
// 4.6 only because the game forces it (MESA_GL_VERSION_OVERRIDE), without tessellation on some
// versions and never with depth clamping. So each one is read from the extension list, then
// probed when the list leaves it out (some drivers do not list the extensions promoted to core).
// A driver known to list a feature it draws wrong does not get it either (gl_quirks.h:
// tessellation on Zink over KosmicKrisp), unless SCACELITH_GL_FORCE names it.
// SCACELITH_GL_DISABLE=tessellation,depth_clamp turns them off on any driver, to test the
// fallbacks. All true until afterContextCreated() has run.
struct Caps {
    bool tessellation = true;  // GL_ARB_tessellation_shader: Phong tessellation of the pieces and robots
    bool depthClamp = true;    // GL_ARB_depth_clamp: shadow casters in front of the cascade's near plane
};
const Caps& caps();
}  // namespace gl46
