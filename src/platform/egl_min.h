// Internal to the macOS layer (macos_gl.cpp): the few EGL types, tokens and entry points it uses,
// as the Khronos registry defines them (EGL/egl.h, EGL/eglext.h). Mesa's libEGL is loaded at run
// time from the application bundle: the game is neither linked with it nor built with its headers.
#pragma once
#include <cstdint>

using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
using EGLint = int32_t;
using EGLAttrib = intptr_t;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLNativeDisplayType = void*;
using EGLProc = void (*)();   // __eglMustCastToProperFunctionPointerType

#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)

constexpr EGLBoolean EGL_FALSE = 0;
constexpr EGLBoolean EGL_TRUE = 1;

constexpr EGLint EGL_SUCCESS = 0x3000;
constexpr EGLint EGL_ALPHA_SIZE = 0x3021;
constexpr EGLint EGL_BLUE_SIZE = 0x3022;
constexpr EGLint EGL_GREEN_SIZE = 0x3023;
constexpr EGLint EGL_RED_SIZE = 0x3024;
constexpr EGLint EGL_DEPTH_SIZE = 0x3025;
constexpr EGLint EGL_STENCIL_SIZE = 0x3026;
constexpr EGLint EGL_SAMPLES = 0x3031;
constexpr EGLint EGL_SAMPLE_BUFFERS = 0x3032;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_COLOR_BUFFER_TYPE = 0x303F;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040;
constexpr EGLint EGL_VENDOR = 0x3053;
constexpr EGLint EGL_VERSION = 0x3054;
constexpr EGLint EGL_EXTENSIONS = 0x3055;
constexpr EGLint EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_CLIENT_APIS = 0x308D;
constexpr EGLint EGL_RGB_BUFFER = 0x308E;
constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_OPENGL_BIT = 0x0008;
constexpr EGLenum EGL_OPENGL_API = 0x30A2;

// EGL 1.5 (EGL_KHR_create_context in 1.4, whose flags carry the debug bit).
constexpr EGLint EGL_CONTEXT_MAJOR_VERSION = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VERSION = 0x30FB;
constexpr EGLint EGL_CONTEXT_FLAGS_KHR = 0x30FC;
constexpr EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD;
constexpr EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x00000001;
constexpr EGLint EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR = 0x00000001;
constexpr EGLint EGL_CONTEXT_OPENGL_DEBUG = 0x31B0;

// EGL_MESA_platform_surfaceless.
constexpr EGLenum EGL_PLATFORM_SURFACELESS_MESA = 0x31DD;

using PFN_eglGetProcAddress = EGLProc (*)(const char* procname);
using PFN_eglGetError = EGLint (*)();
using PFN_eglQueryString = const char* (*)(EGLDisplay dpy, EGLint name);
// EGL_EXT_platform_base (EGLint attributes), and EGL 1.5's own (EGLAttrib attributes).
using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLenum platform, void* nativeDisplay, const EGLint* attribList);
using PFN_eglGetPlatformDisplay = EGLDisplay (*)(EGLenum platform, void* nativeDisplay, const EGLAttrib* attribList);
using PFN_eglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType displayId);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay dpy, EGLint* major, EGLint* minor);
using PFN_eglTerminate = EGLBoolean (*)(EGLDisplay dpy);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum api);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay dpy, const EGLint* attribList, EGLConfig* configs, EGLint configSize,
                                           EGLint* numConfig);
using PFN_eglGetConfigAttrib = EGLBoolean (*)(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay dpy, EGLConfig config, EGLContext shareContext, const EGLint* attribList);
using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay dpy, EGLContext ctx);
using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay dpy, EGLConfig config, const EGLint* attribList);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surface);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
using PFN_eglReleaseThread = EGLBoolean (*)();
