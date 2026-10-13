// The null OpenGL driver of the unit tests (null_gl.h): the defaults of every entry point
// (null_gl_defaults.inc, tools/gen_null_gl.py), and here the ones whose answers the game reads.
#include "null_gl.h"
#include "gl/gl46.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace {

struct NullGlEntry {
    const char* name;
    void* fn;
};
#include "null_gl_defaults.inc"

struct Buffer {
    uint64_t size = 0;
    std::vector<uint8_t> data;   // allocated when first written or mapped
    uint8_t* bytes() {
        if (data.size() < size) data.resize(size_t(size));
        return data.data();
    }
};
struct Texture {
    GLint w = 1, h = 1, d = 1;
};

struct State {
    std::mutex mu;
    uint32_t nextName = 1;
    uint64_t nextSync = 1;
    std::map<GLuint, Buffer> buffers;
    std::map<GLuint, Texture> textures;
    std::set<GLenum> enabled;
    GLenum error = GL_NO_ERROR;   // the next glGetError()
    GLint packAlignment = 4;
    GLuint packBuffer = 0;
    GLint viewport[4] = {0, 0, 1, 1};
    nullgl::Stats stats;
    nullgl::Driver driver;
};
State& st() {
    static State s;
    return s;
}

void names(GLsizei n, GLuint* out, int64_t* live) {
    std::lock_guard<std::mutex> lk(st().mu);
    for (GLsizei i = 0; i < n; ++i) out[i] = st().nextName++;
    if (live) *live += n;
}
void unnamed(GLsizei n, const GLuint* in, int64_t* live) {
    std::lock_guard<std::mutex> lk(st().mu);
    for (GLsizei i = 0; i < n; ++i) {
        if (!in[i]) continue;
        if (live) --*live;
        auto b = st().buffers.find(in[i]);
        if (b != st().buffers.end()) {
            st().stats.bufferBytes -= b->second.size;
            st().buffers.erase(b);
        }
        st().textures.erase(in[i]);
    }
}

// ---- Objects -------------------------------------------------------------------------------------
void APIENTRY createBuffers(GLsizei n, GLuint* b) { names(n, b, &st().stats.liveBuffers); }
void APIENTRY deleteBuffers(GLsizei n, const GLuint* b) { unnamed(n, b, &st().stats.liveBuffers); }
void APIENTRY createTextures(GLenum, GLsizei n, GLuint* t) { names(n, t, &st().stats.liveTextures); }
void APIENTRY genTextures(GLsizei n, GLuint* t) { names(n, t, &st().stats.liveTextures); }
void APIENTRY deleteTextures(GLsizei n, const GLuint* t) { unnamed(n, t, &st().stats.liveTextures); }
void APIENTRY createFramebuffers(GLsizei n, GLuint* f) { names(n, f, &st().stats.liveFramebuffers); }
void APIENTRY deleteFramebuffers(GLsizei n, const GLuint* f) { unnamed(n, f, &st().stats.liveFramebuffers); }
void APIENTRY createVertexArrays(GLsizei n, GLuint* v) { names(n, v, &st().stats.liveVertexArrays); }
void APIENTRY deleteVertexArrays(GLsizei n, const GLuint* v) { unnamed(n, v, &st().stats.liveVertexArrays); }
void APIENTRY createOther(GLsizei n, GLuint* o) { names(n, o, nullptr); }
void APIENTRY createOtherTyped(GLenum, GLsizei n, GLuint* o) { names(n, o, nullptr); }
GLuint APIENTRY createShader(GLenum) {
    GLuint n;
    names(1, &n, nullptr);
    return n;
}
GLuint APIENTRY createProgram() {
    GLuint n;
    names(1, &n, &st().stats.livePrograms);
    return n;
}
void APIENTRY deleteProgram(GLuint p) {
    if (p) --st().stats.livePrograms;
}
void APIENTRY compileShader(GLuint) { ++st().stats.shadersCompiled; }
void APIENTRY linkProgram(GLuint) { ++st().stats.programsLinked; }
void APIENTRY objectLabel(GLenum, GLuint, GLsizei length, const GLchar* label) {
    if (!label) return;
    std::lock_guard<std::mutex> lk(st().mu);
    st().stats.labels.push_back(length < 0 ? std::string(label) : std::string(label, size_t(length)));
}

const char kLog[] = "0(1) : error C0000: the null driver fails it (nullgl::driver())";
void APIENTRY getShaderiv(GLuint, GLenum pname, GLint* p) {
    if (!p) return;
    if (pname == GL_COMPILE_STATUS) *p = st().driver.compileFails ? GL_FALSE : GL_TRUE;
    else if (pname == GL_INFO_LOG_LENGTH) *p = st().driver.compileFails ? GLint(sizeof kLog) : 0;
    else *p = 0;
}
void APIENTRY getProgramiv(GLuint, GLenum pname, GLint* p) {
    if (!p) return;
    if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) *p = st().driver.linkFails ? GL_FALSE : GL_TRUE;
    else if (pname == GL_INFO_LOG_LENGTH) *p = st().driver.linkFails ? GLint(sizeof kLog) : 0;
    else *p = 0;
}
void infoLog(bool fails, GLsizei bufSize, GLsizei* length, GLchar* log) {
    GLsizei n = 0;
    if (log && bufSize > 0) {
        n = fails ? std::min<GLsizei>(bufSize - 1, GLsizei(sizeof kLog) - 1) : 0;
        std::memcpy(log, kLog, size_t(n));
        log[n] = 0;
    }
    if (length) *length = n;
}
void APIENTRY getShaderInfoLog(GLuint, GLsizei b, GLsizei* l, GLchar* log) { infoLog(st().driver.compileFails, b, l, log); }
void APIENTRY getProgramInfoLog(GLuint, GLsizei b, GLsizei* l, GLchar* log) { infoLog(st().driver.linkFails, b, l, log); }
GLint APIENTRY getUniformLocation(GLuint, const GLchar* name) {
    // A location per name (the game only passes it back).
    GLint h = 0;
    for (const char* c = name; c && *c; ++c) h = (h * 31 + *c) & 0x3fff;
    return h;
}

// ---- Buffers -------------------------------------------------------------------------------------
void storage(GLuint b, GLsizeiptr size, const void* data) {
    std::lock_guard<std::mutex> lk(st().mu);
    Buffer& buf = st().buffers[b];
    st().stats.bufferBytes += uint64_t(size) - buf.size;
    buf.size = uint64_t(size);
    buf.data.clear();
    if (data) std::memcpy(buf.bytes(), data, size_t(size));
}
void APIENTRY namedBufferStorage(GLuint b, GLsizeiptr size, const void* data, GLbitfield) { storage(b, size, data); }
void APIENTRY namedBufferData(GLuint b, GLsizeiptr size, const void* data, GLenum) { storage(b, size, data); }
void APIENTRY namedBufferSubData(GLuint b, GLintptr offset, GLsizeiptr size, const void* data) {
    std::lock_guard<std::mutex> lk(st().mu);
    auto it = st().buffers.find(b);
    if (!data || it == st().buffers.end() || uint64_t(offset) + uint64_t(size) > it->second.size) return;
    std::memcpy(it->second.bytes() + offset, data, size_t(size));
}
void* APIENTRY mapNamedBufferRange(GLuint b, GLintptr offset, GLsizeiptr length, GLbitfield) {
    std::lock_guard<std::mutex> lk(st().mu);
    auto it = st().buffers.find(b);
    if (it == st().buffers.end() || uint64_t(offset) + uint64_t(length) > it->second.size) {
        st().error = GL_INVALID_VALUE;
        return nullptr;
    }
    return it->second.bytes() + offset;
}
GLboolean APIENTRY unmapNamedBuffer(GLuint) { return GL_TRUE; }
void APIENTRY copyNamedBufferSubData(GLuint from, GLuint to, GLintptr src, GLintptr dst, GLsizeiptr size) {
    std::lock_guard<std::mutex> lk(st().mu);
    auto a = st().buffers.find(from), b = st().buffers.find(to);
    if (a == st().buffers.end() || b == st().buffers.end() || uint64_t(src) + uint64_t(size) > a->second.size ||
        uint64_t(dst) + uint64_t(size) > b->second.size)
        return;
    std::memmove(b->second.bytes() + dst, a->second.bytes() + src, size_t(size));
}
void APIENTRY bindBuffer(GLenum target, GLuint b) {
    if (target == GL_PIXEL_PACK_BUFFER) st().packBuffer = b;
}

// ---- Textures ------------------------------------------------------------------------------------
void APIENTRY textureStorage2D(GLuint t, GLsizei, GLenum, GLsizei w, GLsizei h) {
    std::lock_guard<std::mutex> lk(st().mu);
    st().textures[t] = Texture{w, h, 1};
}
void APIENTRY textureStorage3D(GLuint t, GLsizei, GLenum, GLsizei w, GLsizei h, GLsizei d) {
    std::lock_guard<std::mutex> lk(st().mu);
    st().textures[t] = Texture{w, h, d};
}
void APIENTRY getTextureLevelParameteriv(GLuint t, GLint level, GLenum pname, GLint* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(st().mu);
    auto it = st().textures.find(t);
    const Texture tex = it == st().textures.end() ? Texture() : it->second;
    if (pname == GL_TEXTURE_WIDTH) *p = std::max(1, tex.w >> level);
    else if (pname == GL_TEXTURE_HEIGHT) *p = std::max(1, tex.h >> level);
    else if (pname == GL_TEXTURE_DEPTH) *p = std::max(1, tex.d >> level);
    else *p = 0;
}

// ---- State and queries ---------------------------------------------------------------------------
void APIENTRY enable(GLenum cap) {
    if (cap == GL_DEPTH_CLAMP && !st().driver.depthClamp) {
        st().error = GL_INVALID_ENUM;
        return;
    }
    std::lock_guard<std::mutex> lk(st().mu);
    st().enabled.insert(cap);
}
void APIENTRY disable(GLenum cap) {
    std::lock_guard<std::mutex> lk(st().mu);
    st().enabled.erase(cap);
}
GLboolean APIENTRY isEnabled(GLenum cap) {
    std::lock_guard<std::mutex> lk(st().mu);
    return st().enabled.count(cap) ? GL_TRUE : GL_FALSE;
}
GLenum APIENTRY getError() {
    GLenum e = st().error;
    st().error = GL_NO_ERROR;
    return e;
}
void APIENTRY viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    GLint* v = st().viewport;
    v[0] = x, v[1] = y, v[2] = w, v[3] = h;
}
void APIENTRY pixelStorei(GLenum pname, GLint value) {
    if (pname == GL_PACK_ALIGNMENT) st().packAlignment = value;
}
void APIENTRY getIntegerv(GLenum pname, GLint* p) {
    if (!p) return;
    switch (pname) {
    case GL_MAJOR_VERSION: *p = 4; break;
    case GL_MINOR_VERSION: *p = 6; break;
    case GL_NUM_EXTENSIONS: *p = GLint(st().driver.extensions.size()); break;
    case GL_MAX_TESS_GEN_LEVEL:
        if (st().driver.tessellation) *p = 64;
        else st().error = GL_INVALID_ENUM;
        break;
    case GL_VIEWPORT: std::memcpy(p, st().viewport, sizeof st().viewport); break;
    case GL_PACK_ALIGNMENT: *p = st().packAlignment; break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *p = GLint(st().packBuffer); break;
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *p = 16384; break;
    case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_ARRAY_TEXTURE_LAYERS: *p = 2048; break;
    case GL_MAX_SAMPLES: case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS: *p = 8; break;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *p = 192; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: *p = 32; break;
    case GL_MAX_UNIFORM_BLOCK_SIZE: *p = 65536; break;
    case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: *p = 1 << 27; break;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: *p = 256; break;
    case GL_DEPTH_WRITEMASK: *p = GL_TRUE; break;
    default: *p = 0; break;
    }
}
void APIENTRY getFloatv(GLenum pname, GLfloat* p) {
    if (p) *p = pname == GL_MAX_TEXTURE_MAX_ANISOTROPY ? 16.0f : 0.0f;
}
void APIENTRY getBooleanv(GLenum pname, GLboolean* p) {
    if (p) *p = pname == GL_DEPTH_WRITEMASK ? GL_TRUE : GL_FALSE;
}
const GLubyte* APIENTRY getString(GLenum name) {
    static const char* const vendor = "Scacelith tests";
    static const char* const version = "4.6.0 null";
    static const char* const glsl = "4.60 null";
    switch (name) {
    case GL_VENDOR: return reinterpret_cast<const GLubyte*>(vendor);
    case GL_RENDERER: return reinterpret_cast<const GLubyte*>(st().driver.renderer.c_str());
    case GL_VERSION: return reinterpret_cast<const GLubyte*>(version);
    case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>(glsl);
    default: st().error = GL_INVALID_ENUM; return nullptr;
    }
}
const GLubyte* APIENTRY getStringi(GLenum name, GLuint i) {
    const auto& e = st().driver.extensions;
    if (name != GL_EXTENSIONS || i >= e.size()) {
        st().error = GL_INVALID_VALUE;
        return nullptr;
    }
    return reinterpret_cast<const GLubyte*>(e[i].c_str());
}
GLenum APIENTRY checkNamedFramebufferStatus(GLuint, GLenum) {
    return st().driver.framebufferIncomplete ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT : GL_FRAMEBUFFER_COMPLETE;
}
GLenum APIENTRY checkFramebufferStatus(GLenum t) { return checkNamedFramebufferStatus(0, t); }

// ---- Sync and read-backs -------------------------------------------------------------------------
GLsync APIENTRY fenceSync(GLenum, GLbitfield) { return reinterpret_cast<GLsync>(uintptr_t(st().nextSync++)); }
GLenum APIENTRY clientWaitSync(GLsync, GLbitfield, GLuint64) { return GL_ALREADY_SIGNALED; }
void APIENTRY getSynciv(GLsync, GLenum pname, GLsizei count, GLsizei* length, GLint* values) {
    if (values && count > 0) *values = pname == GL_SYNC_STATUS ? GL_SIGNALED : 0;
    if (length) *length = count > 0 ? 1 : 0;
}
size_t pixelBytes(GLenum format, GLenum type) {
    size_t c = 4;
    switch (format) {
    case GL_RED: case GL_DEPTH_COMPONENT: case GL_RED_INTEGER: c = 1; break;
    case GL_RG: case GL_RG_INTEGER: c = 2; break;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: c = 3; break;
    default: c = 4; break;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return c;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2 * c;
    case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
    default: return 4 * c;
    }
}
void APIENTRY readPixels(GLint, GLint, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    if (st().packBuffer || !pixels || w <= 0 || h <= 0) return;   // into a buffer object: nothing to write
    const size_t a = size_t(std::max(1, st().packAlignment));
    const size_t row = (size_t(w) * pixelBytes(format, type) + a - 1) / a * a;
    // The last row is not padded (the pack rules of the GL specification).
    std::memset(pixels, 0, row * size_t(h - 1) + size_t(w) * pixelBytes(format, type));
}

// ---- Counted work --------------------------------------------------------------------------------
void APIENTRY clear(GLbitfield) { ++st().stats.clears; }
void APIENTRY clearNamedFramebufferfv(GLuint, GLenum, GLint, const GLfloat*) { ++st().stats.clears; }
void APIENTRY drawArrays(GLenum, GLint, GLsizei) { ++st().stats.draws; }
void APIENTRY drawArraysInstanced(GLenum, GLint, GLsizei, GLsizei) { ++st().stats.draws; }
void APIENTRY drawElements(GLenum, GLsizei, GLenum, const void*) { ++st().stats.draws; }
void APIENTRY drawElementsInstanced(GLenum, GLsizei, GLenum, const void*, GLsizei) { ++st().stats.draws; }
void APIENTRY drawElementsBaseVertex(GLenum, GLsizei, GLenum, const void*, GLint) { ++st().stats.draws; }
void APIENTRY drawElementsInstancedBaseVertexBaseInstance(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint) {
    ++st().stats.draws;
}
void APIENTRY multiDrawElementsIndirect(GLenum, GLenum, const void*, GLsizei, GLsizei) { ++st().stats.draws; }
void APIENTRY dispatchCompute(GLuint, GLuint, GLuint) { ++st().stats.dispatches; }

#define NULLGL(name, fn) {#name, reinterpret_cast<void*>(&fn)}
const NullGlEntry kBehaviours[] = {
    NULLGL(glCreateBuffers, createBuffers), NULLGL(glGenBuffers, createBuffers), NULLGL(glDeleteBuffers, deleteBuffers),
    NULLGL(glCreateTextures, createTextures), NULLGL(glGenTextures, genTextures), NULLGL(glDeleteTextures, deleteTextures),
    NULLGL(glCreateFramebuffers, createFramebuffers), NULLGL(glGenFramebuffers, createFramebuffers),
    NULLGL(glDeleteFramebuffers, deleteFramebuffers), NULLGL(glCreateVertexArrays, createVertexArrays),
    NULLGL(glGenVertexArrays, createVertexArrays), NULLGL(glDeleteVertexArrays, deleteVertexArrays),
    NULLGL(glCreateSamplers, createOther), NULLGL(glGenSamplers, createOther), NULLGL(glCreateRenderbuffers, createOther),
    NULLGL(glGenRenderbuffers, createOther), NULLGL(glCreateQueries, createOtherTyped), NULLGL(glGenQueries, createOther),
    NULLGL(glCreateProgramPipelines, createOther), NULLGL(glCreateTransformFeedbacks, createOther),
    NULLGL(glCreateShader, createShader), NULLGL(glCreateProgram, createProgram), NULLGL(glDeleteProgram, deleteProgram),
    NULLGL(glCompileShader, compileShader), NULLGL(glLinkProgram, linkProgram), NULLGL(glObjectLabel, objectLabel),
    NULLGL(glGetShaderiv, getShaderiv), NULLGL(glGetProgramiv, getProgramiv), NULLGL(glGetShaderInfoLog, getShaderInfoLog),
    NULLGL(glGetProgramInfoLog, getProgramInfoLog), NULLGL(glGetUniformLocation, getUniformLocation),
    NULLGL(glNamedBufferStorage, namedBufferStorage), NULLGL(glNamedBufferData, namedBufferData),
    NULLGL(glNamedBufferSubData, namedBufferSubData), NULLGL(glMapNamedBufferRange, mapNamedBufferRange),
    NULLGL(glUnmapNamedBuffer, unmapNamedBuffer), NULLGL(glCopyNamedBufferSubData, copyNamedBufferSubData),
    NULLGL(glBindBuffer, bindBuffer), NULLGL(glTextureStorage2D, textureStorage2D),
    NULLGL(glTextureStorage3D, textureStorage3D), NULLGL(glGetTextureLevelParameteriv, getTextureLevelParameteriv),
    NULLGL(glEnable, enable), NULLGL(glDisable, disable), NULLGL(glIsEnabled, isEnabled), NULLGL(glGetError, getError),
    NULLGL(glViewport, viewport), NULLGL(glPixelStorei, pixelStorei), NULLGL(glGetIntegerv, getIntegerv),
    NULLGL(glGetFloatv, getFloatv), NULLGL(glGetBooleanv, getBooleanv), NULLGL(glGetString, getString),
    NULLGL(glGetStringi, getStringi), NULLGL(glCheckNamedFramebufferStatus, checkNamedFramebufferStatus),
    NULLGL(glCheckFramebufferStatus, checkFramebufferStatus), NULLGL(glFenceSync, fenceSync),
    NULLGL(glClientWaitSync, clientWaitSync), NULLGL(glGetSynciv, getSynciv), NULLGL(glReadPixels, readPixels),
    NULLGL(glClear, clear), NULLGL(glClearNamedFramebufferfv, clearNamedFramebufferfv), NULLGL(glDrawArrays, drawArrays),
    NULLGL(glDrawArraysInstanced, drawArraysInstanced), NULLGL(glDrawElements, drawElements),
    NULLGL(glDrawElementsInstanced, drawElementsInstanced), NULLGL(glDrawElementsBaseVertex, drawElementsBaseVertex),
    NULLGL(glDrawElementsInstancedBaseVertexBaseInstance, drawElementsInstancedBaseVertexBaseInstance),
    NULLGL(glMultiDrawElementsIndirect, multiDrawElementsIndirect), NULLGL(glDispatchCompute, dispatchCompute),
};
#undef NULLGL

}  // namespace

namespace nullgl {

Stats& stats() { return st().stats; }
void resetStats() {
    Stats& s = st().stats;
    s.draws = s.dispatches = s.programsLinked = s.shadersCompiled = s.clears = 0;
    s.labels.clear();
}
Driver& driver() { return st().driver; }

void* getProc(const char* name) {
    for (const NullGlEntry& e : kBehaviours)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    for (const NullGlEntry& e : kNullGlDefaults)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return nullptr;
}

int load() {
    const char* missing = nullptr;
    return gl46::load(&getProc, &missing);
}

}  // namespace nullgl
