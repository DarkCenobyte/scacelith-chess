#include "gl_context.h"
#include "gl46.h"
#include "../core/log.h"
#include <cstdlib>
#include <cstring>
#include <string>

namespace gl46 {
namespace {
Caps g_caps;

void APIENTRY debugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (id == 131185 || id == 131218 || id == 131204) return;  // NVIDIA buffer/shader recompile chatter
    if (type == GL_DEBUG_TYPE_ERROR) LOGE("GL: %s", message);
    else LOGW("GL: %s", message);
}

bool hasExtension(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, GLuint(i));
        if (e && std::strcmp(e, name) == 0) return true;
    }
    return false;
}

// Whether the comma-separated list in SCACELITH_GL_DISABLE names this feature.
bool disabledByEnv(const char* feature) {
    const char* env = std::getenv("SCACELITH_GL_DISABLE");
    if (!env) return false;
    const std::string list = std::string(",") + env + ",";
    return list.find(std::string(",") + feature + ",") != std::string::npos;
}

void drainErrors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
}

// Runs before the debug callback is installed: the probes raise GL_INVALID_ENUM on a driver
// without the feature, and that is the answer, not an error to report.
void detectCaps() {
    drainErrors();
    g_caps = Caps();
    // Tessellation: GL_MAX_TESS_GEN_LEVEL is only a valid query when the feature is there (Mesa
    // rejects it without GL_ARB_tessellation_shader whatever the version override says).
    g_caps.tessellation = hasExtension("GL_ARB_tessellation_shader");
    if (!g_caps.tessellation) {
        GLint level = 0;
        glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &level);
        g_caps.tessellation = glGetError() == GL_NO_ERROR && level >= 64;
    }
    g_caps.depthClamp = hasExtension("GL_ARB_depth_clamp");
    if (!g_caps.depthClamp) {
        glEnable(GL_DEPTH_CLAMP);
        g_caps.depthClamp = glGetError() == GL_NO_ERROR && glIsEnabled(GL_DEPTH_CLAMP);
        glDisable(GL_DEPTH_CLAMP);
    }
    drainErrors();
    if (disabledByEnv("tessellation")) g_caps.tessellation = false;
    if (disabledByEnv("depth_clamp")) g_caps.depthClamp = false;
    if (!g_caps.tessellation) LOGW("GL: no tessellation on this driver: turned off whatever the graphics settings say");
    if (!g_caps.depthClamp) LOGW("GL: no depth clamping on this driver: sun shadow cascades are fitted to every caster");

    // Extensions behind the core features the engine uses: a driver whose 4.6 is forced may
    // lack some. Logged to explain a broken image; the probes above are the only ones acted on.
    static const char* const kUsed[] = {
        "GL_ARB_clip_control", "GL_ARB_compute_shader", "GL_ARB_copy_image", "GL_ARB_direct_state_access",
        "GL_ARB_shader_image_load_store", "GL_ARB_shader_storage_buffer_object", "GL_ARB_texture_cube_map_array",
        "GL_ARB_texture_gather",
    };
    std::string missing;
    for (const char* e : kUsed)
        if (!hasExtension(e)) missing += std::string(missing.empty() ? "" : ", ") + e;
    if (!missing.empty()) LOGW("GL: extensions not listed by the driver: %s", missing.c_str());
}
}  // namespace

const Caps& caps() { return g_caps; }

void afterContextCreated(bool debug) {
    LOGI("GL %s | %s | %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VENDOR));
    detectCaps();
    if (debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debugCallback, nullptr);
    }
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
}
}  // namespace gl46
