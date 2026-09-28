#include "gl_context.h"
#include "gl46.h"
#include "../core/log.h"

namespace gl46 {
static void APIENTRY debugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (id == 131185 || id == 131218 || id == 131204) return;  // NVIDIA buffer/shader recompile chatter
    if (type == GL_DEBUG_TYPE_ERROR) LOGE("GL: %s", message);
    else LOGW("GL: %s", message);
}

void afterContextCreated(bool debug) {
    LOGI("GL %s | %s | %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VENDOR));
    if (debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debugCallback, nullptr);
    }
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
}
}  // namespace gl46
