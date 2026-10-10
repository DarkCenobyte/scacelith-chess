// macOS OpenGL (see macos_gl.h): Mesa's EGL on its surfaceless platform, Zink over KosmicKrisp,
// all loaded from the application bundle, and the asynchronous readback of each finished frame.
#ifdef __APPLE__
#include "macos_gl.h"
#include "egl_min.h"
#include "../core/log.h"
#include "../gl/gl46.h"
#include "../gl/gl_context.h"
#include "../net/net_sys.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace plat {
namespace macgl {
namespace {

// ---- EGL, resolved from Mesa's libEGL -------------------------------------------------------------
struct Egl {
    void* lib = nullptr;
    PFN_eglGetProcAddress getProcAddress = nullptr;
    PFN_eglGetError getError = nullptr;
    PFN_eglQueryString queryString = nullptr;
    PFN_eglGetPlatformDisplayEXT getPlatformDisplayEXT = nullptr;
    PFN_eglGetPlatformDisplay getPlatformDisplay = nullptr;
    PFN_eglGetDisplay getDisplay = nullptr;
    PFN_eglInitialize initialize = nullptr;
    PFN_eglTerminate terminate = nullptr;
    PFN_eglBindAPI bindAPI = nullptr;
    PFN_eglChooseConfig chooseConfig = nullptr;
    PFN_eglGetConfigAttrib getConfigAttrib = nullptr;
    PFN_eglCreateContext createContext = nullptr;
    PFN_eglDestroyContext destroyContext = nullptr;
    PFN_eglCreatePbufferSurface createPbufferSurface = nullptr;
    PFN_eglDestroySurface destroySurface = nullptr;
    PFN_eglMakeCurrent makeCurrent = nullptr;
    PFN_eglReleaseThread releaseThread = nullptr;
};
Egl g_egl;
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLConfig g_config = nullptr;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
int g_surfaceW = 0, g_surfaceH = 0;

bool regularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The bundle's Contents folder ("<...>.app/Contents/"), from the executable's folder (Contents/MacOS/);
// the executable's parent folder when it runs outside a bundle (a development build).
std::string contentsDirectory() {
    const std::string up = net::sys::exeDirectory() + "..";
    char buf[PATH_MAX];
    if (realpath(up.c_str(), buf)) return std::string(buf) + "/";
    return up + "/";
}

// The variables Mesa and the Vulkan loader read: those the user has set are left as they are.
void prepareEnvironment(const std::string& contents) {
    setenv("EGL_PLATFORM", "surfaceless", 0);
    setenv("GALLIUM_DRIVER", "zink", 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
    setenv("MESA_GL_VERSION_OVERRIDE", "4.6", 0);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "460", 0);
    // The bundle's Vulkan driver (KosmicKrisp), the only one the loader then reads.
    const std::string icd = contents + "Resources/vulkan/icd.d/kosmickrisp_icd.json";
    if (regularFile(icd)) setenv("VK_DRIVER_FILES", icd.c_str(), 0);
    const char* driverFiles = std::getenv("VK_DRIVER_FILES");
    LOGI("gl: Vulkan driver files %s", driverFiles ? driverFiles : "(the loader's defaults)");
}

// Zink opens the Vulkan loader by its name alone ("libvulkan.1.dylib"): loaded first from the
// bundle by its full path, the loader that Zink finds is this one.
void preloadVulkanLoader(const std::string& contents) {
    const std::string path = contents + "Frameworks/libvulkan.1.dylib";
    if (!regularFile(path)) return;
    if (!dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
        const char* err = dlerror();
        LOGW("gl: could not load %s (%s)", path.c_str(), err ? err : "?");
    }
}

template <typename F>
bool resolve(F& fn, const char* name) {
    fn = reinterpret_cast<F>(dlsym(g_egl.lib, name));
    if (!fn) LOGE("gl: libEGL has no %s", name);
    return fn != nullptr;
}

// libEGL: the bundle's (Contents/Frameworks), else $SCACELITH_EGL_LIBRARY, else the one dyld finds.
bool loadEgl(const std::string& contents, std::string& why) {
    std::string candidates[3] = {contents + "Frameworks/libEGL.1.dylib", "", "libEGL.1.dylib"};
    if (const char* env = std::getenv("SCACELITH_EGL_LIBRARY")) candidates[1] = env;
    for (const std::string& path : candidates) {
        if (path.empty()) continue;
        g_egl.lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (g_egl.lib) {
            LOGI("gl: EGL from %s", path.c_str());
            break;
        }
        const char* err = dlerror();
        LOGW("gl: could not load %s (%s)", path.c_str(), err ? err : "?");
    }
    if (!g_egl.lib) {
        why = "Mesa's libEGL could not be loaded";
        return false;
    }
    bool ok = resolve(g_egl.getProcAddress, "eglGetProcAddress");
    ok = resolve(g_egl.getError, "eglGetError") && ok;
    ok = resolve(g_egl.queryString, "eglQueryString") && ok;
    ok = resolve(g_egl.getDisplay, "eglGetDisplay") && ok;
    ok = resolve(g_egl.initialize, "eglInitialize") && ok;
    ok = resolve(g_egl.terminate, "eglTerminate") && ok;
    ok = resolve(g_egl.bindAPI, "eglBindAPI") && ok;
    ok = resolve(g_egl.chooseConfig, "eglChooseConfig") && ok;
    ok = resolve(g_egl.getConfigAttrib, "eglGetConfigAttrib") && ok;
    ok = resolve(g_egl.createContext, "eglCreateContext") && ok;
    ok = resolve(g_egl.destroyContext, "eglDestroyContext") && ok;
    ok = resolve(g_egl.createPbufferSurface, "eglCreatePbufferSurface") && ok;
    ok = resolve(g_egl.destroySurface, "eglDestroySurface") && ok;
    ok = resolve(g_egl.makeCurrent, "eglMakeCurrent") && ok;
    ok = resolve(g_egl.releaseThread, "eglReleaseThread") && ok;
    if (!ok) {
        why = "Mesa's libEGL lacks entry points";
        return false;
    }
    // EGL_EXT_platform_base's entry point, else EGL 1.5's own.
    g_egl.getPlatformDisplayEXT =
        reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(g_egl.getProcAddress("eglGetPlatformDisplayEXT"));
    g_egl.getPlatformDisplay = reinterpret_cast<PFN_eglGetPlatformDisplay>(dlsym(g_egl.lib, "eglGetPlatformDisplay"));
    return true;
}

int eglError() { return g_egl.getError ? int(g_egl.getError()) : 0; }

void* getProc(const char* name) { return reinterpret_cast<void*>(g_egl.getProcAddress(name)); }

EGLDisplay openDisplay() {
    const char* clientExtensions = g_egl.queryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    LOGI("gl: EGL client extensions: %s", clientExtensions ? clientExtensions : "(none)");
    EGLDisplay display = EGL_NO_DISPLAY;
    if (g_egl.getPlatformDisplayEXT)
        display = g_egl.getPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY && g_egl.getPlatformDisplay)
        display = g_egl.getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    // EGL_PLATFORM=surfaceless (prepareEnvironment) makes the default display a surfaceless one too.
    if (display == EGL_NO_DISPLAY) display = g_egl.getDisplay(EGL_DEFAULT_DISPLAY);
    return display;
}

EGLint configAttrib(EGLConfig config, EGLint attribute) {
    EGLint value = 0;
    return g_egl.getConfigAttrib(g_display, config, attribute, &value) ? value : -1;
}

// A pbuffer configuration for desktop OpenGL with 8-bit RGBA, single-sampled, with depth and stencil
// (24 and 8 bits at least) when one has them.
bool chooseConfig() {
    for (int withDepth = 1; withDepth >= 0; --withDepth) {
        const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                  EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                  EGL_DEPTH_SIZE, withDepth ? 24 : 0, EGL_STENCIL_SIZE, withDepth ? 8 : 0,
                                  EGL_SAMPLE_BUFFERS, 0, EGL_NONE};
        EGLConfig configs[64];
        EGLint n = 0;
        if (!g_egl.chooseConfig(g_display, attribs, configs, 64, &n) || n <= 0) continue;
        // The list is sorted deepest colour first: the first with exactly 8 bits per channel.
        for (EGLint i = 0; i < n; ++i) {
            if (configAttrib(configs[i], EGL_RED_SIZE) == 8 && configAttrib(configs[i], EGL_GREEN_SIZE) == 8 &&
                configAttrib(configs[i], EGL_BLUE_SIZE) == 8 && configAttrib(configs[i], EGL_ALPHA_SIZE) == 8) {
                g_config = configs[i];
                break;
            }
        }
        if (!g_config) g_config = configs[0];
        LOGI("gl: EGL config RGBA %d%d%d%d, depth %d, stencil %d", configAttrib(g_config, EGL_RED_SIZE),
             configAttrib(g_config, EGL_GREEN_SIZE), configAttrib(g_config, EGL_BLUE_SIZE),
             configAttrib(g_config, EGL_ALPHA_SIZE), configAttrib(g_config, EGL_DEPTH_SIZE),
             configAttrib(g_config, EGL_STENCIL_SIZE));
        if (!withDepth) LOGW("gl: no pbuffer configuration with depth and stencil, the default framebuffer has none");
        return true;
    }
    return false;
}

EGLSurface createPbuffer(int width, int height) {
    const EGLint attribs[] = {EGL_WIDTH, width > 0 ? width : 1, EGL_HEIGHT, height > 0 ? height : 1, EGL_NONE};
    return g_egl.createPbufferSurface(g_display, g_config, attribs);
}

// ---- Readback -------------------------------------------------------------------------------------
constexpr int kSlots = 3;
// How long takeFrame() waits for the previous frame, and endFrame() for the frame three frames old
// whose buffer it reuses (the GPU is that far behind: the CPU waits for it instead of queuing more).
constexpr GLuint64 kPreviousFrameWaitNs = 4000000;      // 4 ms
constexpr GLuint64 kOldestFrameWaitNs = 1000000000;     // 1 s

struct Slot {
    GLuint pbo = 0;
    size_t capacity = 0;
    GLsync fence = nullptr;
    int width = 0, height = 0;
    uint64_t serial = 0;   // order of the frames
};
Slot g_slots[kSlots];
int g_nextSlot = 0;
int g_lastSlot = -1;       // the frame endFrame() started last
uint64_t g_serial = 0;
Slot* g_mapped = nullptr;
GLuint g_flipFbo = 0, g_flipTexture = 0;
int g_flipW = 0, g_flipH = 0;
bool g_readbackFailed = false;

void dropFence(Slot& s) {
    if (s.fence) glDeleteSync(s.fence);
    s.fence = nullptr;
}

bool signaled(GLsync fence, GLbitfield flags, GLuint64 timeoutNs) {
    const GLenum r = glClientWaitSync(fence, flags, timeoutNs);
    if (r == GL_WAIT_FAILED && !g_readbackFailed) {
        g_readbackFailed = true;
        LOGW("gl: glClientWaitSync failed (GL error 0x%x)", unsigned(glGetError()));
    }
    // A failed wait does not hold the frame back for ever.
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED || r == GL_WAIT_FAILED;
}

// The texture FB 0 is flipped into, at the pbuffer's size.
bool ensureFlipTarget(int w, int h) {
    if (g_flipFbo && g_flipW == w && g_flipH == h) return true;
    if (!g_flipFbo) glCreateFramebuffers(1, &g_flipFbo);
    if (g_flipTexture) glDeleteTextures(1, &g_flipTexture);
    glCreateTextures(GL_TEXTURE_2D, 1, &g_flipTexture);
    glTextureStorage2D(g_flipTexture, 1, GL_RGBA8, w, h);
    glNamedFramebufferTexture(g_flipFbo, GL_COLOR_ATTACHMENT0, g_flipTexture, 0);
    g_flipW = w;
    g_flipH = h;
    if (glCheckNamedFramebufferStatus(g_flipFbo, GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("gl: the readback framebuffer (%dx%d) is incomplete", w, h);
        return false;
    }
    return true;
}

bool ensureBuffer(Slot& s, size_t bytes) {
    if (s.pbo && s.capacity >= bytes) return true;
    if (s.pbo) glDeleteBuffers(1, &s.pbo);
    glCreateBuffers(1, &s.pbo);
    // Read by the CPU: memory it reads quickly (cached).
    glNamedBufferStorage(s.pbo, GLsizeiptr(bytes), nullptr, GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT);
    s.capacity = bytes;
    return true;
}

void destroyReadback() {
    if (g_mapped) releaseFrame();
    for (Slot& s : g_slots) {
        dropFence(s);
        if (s.pbo) glDeleteBuffers(1, &s.pbo);
        s = Slot();
    }
    if (g_flipTexture) glDeleteTextures(1, &g_flipTexture);
    if (g_flipFbo) glDeleteFramebuffers(1, &g_flipFbo);
    g_flipTexture = g_flipFbo = 0;
    g_flipW = g_flipH = 0;
    g_nextSlot = 0;
    g_lastSlot = -1;
}

}  // namespace

bool create(int width, int height, bool debug, std::string& why) {
    const std::string contents = contentsDirectory();
    prepareEnvironment(contents);
    preloadVulkanLoader(contents);
    if (!loadEgl(contents, why)) return false;

    g_display = openDisplay();
    if (g_display == EGL_NO_DISPLAY) {
        char code[16];
        std::snprintf(code, sizeof code, "0x%x", unsigned(eglError()));
        why = std::string("no EGL display (eglGetPlatformDisplay, error ") + code + ")";
        LOGE("gl: no surfaceless EGL display (EGL error %s)", code);
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!g_egl.initialize(g_display, &major, &minor)) {
        LOGE("gl: eglInitialize failed (EGL error 0x%x)", eglError());
        why = "eglInitialize failed";
        g_display = EGL_NO_DISPLAY;
        return false;
    }
    const char* vendor = g_egl.queryString(g_display, EGL_VENDOR);
    const char* version = g_egl.queryString(g_display, EGL_VERSION);
    const char* apis = g_egl.queryString(g_display, EGL_CLIENT_APIS);
    LOGI("gl: EGL %d.%d, %s, %s, APIs: %s", int(major), int(minor), vendor ? vendor : "?", version ? version : "?",
         apis ? apis : "?");
    if (!g_egl.bindAPI(EGL_OPENGL_API)) {
        LOGE("gl: eglBindAPI(EGL_OPENGL_API) failed (EGL error 0x%x)", eglError());
        why = "no desktop OpenGL in EGL";
        return false;
    }
    if (!chooseConfig()) {
        LOGE("gl: no EGL configuration for an OpenGL pbuffer (EGL error 0x%x)", eglError());
        why = "no EGL configuration";
        return false;
    }
    // EGL 1.5 takes the debug flag as an attribute of its own; 1.4 with EGL_KHR_create_context as a flag.
    const bool egl15 = major > 1 || (major == 1 && minor >= 5);
    const EGLint ctxAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 egl15 ? EGL_CONTEXT_OPENGL_DEBUG : EGL_CONTEXT_FLAGS_KHR,
                                 egl15 ? EGLint(debug ? EGL_TRUE : EGL_FALSE) : (debug ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0),
                                 EGL_NONE};
    g_context = g_egl.createContext(g_display, g_config, EGL_NO_CONTEXT, ctxAttribs);
    if (g_context == EGL_NO_CONTEXT) {
        LOGE("gl: cannot create a GL 4.6 core context (EGL error 0x%x)", eglError());
        why = "no OpenGL 4.6 core context";
        return false;
    }
    g_surface = createPbuffer(width, height);
    if (g_surface == EGL_NO_SURFACE) {
        LOGE("gl: cannot create a %dx%d pbuffer (EGL error 0x%x)", width, height, eglError());
        why = "no pbuffer";
        return false;
    }
    g_surfaceW = width > 0 ? width : 1;
    g_surfaceH = height > 0 ? height : 1;
    if (!g_egl.makeCurrent(g_display, g_surface, g_surface, g_context)) {
        LOGE("gl: eglMakeCurrent failed (EGL error 0x%x)", eglError());
        why = "eglMakeCurrent failed";
        return false;
    }
    const char* missing = nullptr;
    const int nMissing = gl46::load(getProc, &missing);
    if (nMissing) LOGW("%d GL entry points missing (first: %s)", nMissing, missing);
    gl46::afterContextCreated(debug);
    return true;
}

void destroy() {
    if (g_display == EGL_NO_DISPLAY) return;
    if (g_context != EGL_NO_CONTEXT && g_surface != EGL_NO_SURFACE) destroyReadback();
    g_egl.makeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_surface != EGL_NO_SURFACE) g_egl.destroySurface(g_display, g_surface);
    if (g_context != EGL_NO_CONTEXT) g_egl.destroyContext(g_display, g_context);
    g_egl.terminate(g_display);
    g_egl.releaseThread();
    g_surface = EGL_NO_SURFACE;
    g_context = EGL_NO_CONTEXT;
    g_display = EGL_NO_DISPLAY;
    g_config = nullptr;
    g_surfaceW = g_surfaceH = 0;
}

bool resize(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (g_surface == EGL_NO_SURFACE || (width == g_surfaceW && height == g_surfaceH)) return true;
    EGLSurface s = createPbuffer(width, height);
    if (s == EGL_NO_SURFACE) {
        LOGW("gl: cannot create a %dx%d pbuffer (EGL error 0x%x), keeping %dx%d", width, height, eglError(), g_surfaceW,
             g_surfaceH);
        return false;
    }
    if (!g_egl.makeCurrent(g_display, s, s, g_context)) {
        LOGW("gl: eglMakeCurrent on the %dx%d pbuffer failed (EGL error 0x%x)", width, height, eglError());
        g_egl.destroySurface(g_display, s);
        return false;
    }
    g_egl.destroySurface(g_display, g_surface);
    g_surface = s;
    g_surfaceW = width;
    g_surfaceH = height;
    return true;
}

int surfaceWidth() { return g_surfaceW; }
int surfaceHeight() { return g_surfaceH; }

void endFrame() {
    if (g_surface == EGL_NO_SURFACE) return;
    const int w = g_surfaceW, h = g_surfaceH;
    Slot& s = g_slots[g_nextSlot];
    // Three frames are on their way already: the CPU waits for the oldest, never shown.
    if (s.fence) {
        signaled(s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, kOldestFrameWaitNs);
        dropFence(s);
    }
    if (!ensureFlipTarget(w, h)) return;
    const size_t bytes = size_t(w) * size_t(h) * 4;
    ensureBuffer(s, bytes);

    // The state this changes, restored below (the blit's own framebuffers are named: no binding).
    GLint readFb = 0, packBuffer = 0, packAlignment = 4, packRowLength = 0, packSkipPixels = 0, packSkipRows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &packSkipPixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &packSkipRows);
    // A blit is cut by the scissor test, converted with GL_FRAMEBUFFER_SRGB and dropped under
    // GL_RASTERIZER_DISCARD.
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    const GLboolean discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);
    if (discard) glDisable(GL_RASTERIZER_DISCARD);

    // Upside down: the first row read is the top of the picture, as Metal's textures have it.
    glBlitNamedFramebuffer(0, g_flipFbo, 0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_flipFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    // Bytes B, G, R, A in memory: Metal's BGRA8Unorm.
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, packRowLength);
    glPixelStorei(GL_PACK_SKIP_PIXELS, packSkipPixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, packSkipRows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(packBuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFb));
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (discard) glEnable(GL_RASTERIZER_DISCARD);
    // No eglSwapBuffers to submit the frame: the GPU starts it now.
    glFlush();

    s.width = w;
    s.height = h;
    s.serial = ++g_serial;
    g_lastSlot = g_nextSlot;
    g_nextSlot = (g_nextSlot + 1) % kSlots;
}

bool takeFrame(Frame& frame) {
    frame = Frame();
    if (g_mapped) releaseFrame();
    // The frames on their way, newest first, the one just started excepted.
    Slot* pending[kSlots];
    int n = 0;
    for (int i = 0; i < kSlots; ++i)
        if (i != g_lastSlot && g_slots[i].fence) pending[n++] = &g_slots[i];
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && pending[j]->serial > pending[j - 1]->serial; --j) std::swap(pending[j], pending[j - 1]);
    // The GPU finishes them in order: the newest that has arrived, and those before it are dropped.
    Slot* best = nullptr;
    for (int i = 0; i < n && !best; ++i)
        if (signaled(pending[i]->fence, 0, i == 0 ? kPreviousFrameWaitNs : 0)) best = pending[i];
    if (!best) return false;
    for (int i = 0; i < n; ++i)
        if (pending[i]->serial < best->serial) dropFence(*pending[i]);
    const size_t bytes = size_t(best->width) * size_t(best->height) * 4;
    void* p = glMapNamedBufferRange(best->pbo, 0, GLsizeiptr(bytes), GL_MAP_READ_BIT);
    if (!p) {
        if (!g_readbackFailed) {
            g_readbackFailed = true;
            LOGW("gl: the readback buffer could not be mapped (GL error 0x%x)", unsigned(glGetError()));
        }
        dropFence(*best);
        return false;
    }
    g_mapped = best;
    frame.pixels = static_cast<const unsigned char*>(p);
    frame.width = best->width;
    frame.height = best->height;
    frame.stride = size_t(best->width) * 4;
    return true;
}

void releaseFrame() {
    if (!g_mapped) return;
    glUnmapNamedBuffer(g_mapped->pbo);
    dropFence(*g_mapped);
    g_mapped = nullptr;
}

void finish() {
    if (g_surface == EGL_NO_SURFACE) return;
    if (g_mapped) releaseFrame();
    glFinish();
    for (Slot& s : g_slots) dropFence(s);
    g_lastSlot = -1;
}

}  // namespace macgl
}  // namespace plat
#endif
