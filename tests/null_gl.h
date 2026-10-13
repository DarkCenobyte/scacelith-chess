// A null OpenGL 4.6 driver for the unit tests: every entry point gl46::load() resolves does
// nothing, except what the game reads back. Object names are unique, shaders compile and programs
// link, framebuffers are complete, the limits are generous, a mapped buffer is memory of its size,
// fences are signalled at once, and a read-back gets zeros. Nothing is drawn: the menus, the
// renderer and the scenes run their CPU side (layout, state, draw lists) without a GPU.
//
//   nullgl::load();            // gl46::load() on the null driver (the fake platform's plat::init does it)
//   nullgl::stats().draws      // what the game asked of it since the last reset()
// Defaults: tools/gen_null_gl.py (tests/null_gl_defaults.inc); behaviours: tests/null_gl.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nullgl {

struct Stats {
    uint64_t draws = 0;            // glDraw* and glMultiDraw* calls
    uint64_t dispatches = 0;       // glDispatchCompute
    uint64_t programsLinked = 0;
    uint64_t shadersCompiled = 0;
    uint64_t clears = 0;
    int64_t liveTextures = 0;      // created minus deleted
    int64_t liveBuffers = 0;
    int64_t liveFramebuffers = 0;
    int64_t liveVertexArrays = 0;
    int64_t livePrograms = 0;
    uint64_t bufferBytes = 0;      // storage given to the buffers alive
    std::vector<std::string> labels;  // glObjectLabel names, in order (reset() clears them)
};
Stats& stats();
void resetStats();

struct Driver {
    std::string renderer = "Scacelith null GL";
    // Listed by glGetStringi: those gl46::afterContextCreated() looks for. Without
    // GL_ARB_tessellation_shader or GL_ARB_depth_clamp the feature is probed (the two below).
    std::vector<std::string> extensions = {
        "GL_ARB_clip_control", "GL_ARB_compute_shader", "GL_ARB_copy_image", "GL_ARB_direct_state_access",
        "GL_ARB_shader_image_load_store", "GL_ARB_shader_storage_buffer_object", "GL_ARB_texture_cube_map_array",
        "GL_ARB_texture_gather", "GL_ARB_tessellation_shader", "GL_ARB_depth_clamp"};
    bool tessellation = true;              // probed: GL_MAX_TESS_GEN_LEVEL answers 64 (else GL_INVALID_ENUM)
    bool depthClamp = true;                // probed: glEnable(GL_DEPTH_CLAMP) takes (else GL_INVALID_ENUM)
    bool compileFails = false;             // shaders fail to compile, with a log
    bool linkFails = false;                // programs fail to link, with a log
    bool framebufferIncomplete = false;    // glCheckNamedFramebufferStatus answers incomplete
};
// The driver's answers; change them before nullgl::load() (or gl46::afterContextCreated).
Driver& driver();

// Loads every GL entry point with the null driver's. Returns gl46::load()'s count of missing ones.
int load();
// The address gl46::load() gets for 'name' (nullptr for a name the driver does not know).
void* getProc(const char* name);

}  // namespace nullgl
