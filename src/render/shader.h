// GLSL program builder.
//
// Sources come from the embedded file table (core/embedded.h) so the shipped exe has no loose
// files; pass --data-dir <repo> to read them from disk instead (live editing + F5 reload).
//
// Preprocessing done here (before the driver sees the code):
//  * `#include "path"` — path relative to the repository root (e.g. "shaders/include/common.glsl").
//    Each file is included at most once per stage (implicit include guard).
//  * `#pragma material` — replaced by the material surface file given in ProgramDesc::material.
//  * `#pragma displacement` — replaced by ProgramDesc::displacement (vertex / tess-eval stages).
//  * ProgramDesc::defines are inserted right after the #version line as `#define X`.
//  * Every stage must start with `#version 460 core` (added automatically when missing).
#pragma once
#include "../gl/gl46.h"
#include <string>
#include <vector>

struct ProgramDesc {
    std::string vs, tcs, tes, gs, fs, cs;  // embedded paths, empty = stage unused
    std::string material;                  // surface file substituted at `#pragma material`
    std::string displacement;              // displacement file substituted at `#pragma displacement`
    std::vector<std::string> defines;      // "NAME" or "NAME VALUE"
    std::string key() const;
};

class ShaderProgram {
public:
    GLuint id = 0;
    bool valid() const { return id != 0; }
    void use() const { glUseProgram(id); }
    GLint loc(const char* name) const { return glGetUniformLocation(id, name); }
    void set(const char* n, int v) const { glProgramUniform1i(id, loc(n), v); }
    void set(const char* n, float v) const { glProgramUniform1f(id, loc(n), v); }
    void set(const char* n, float x, float y) const { glProgramUniform2f(id, loc(n), x, y); }
    void set(const char* n, float x, float y, float z) const { glProgramUniform3f(id, loc(n), x, y, z); }
    void set(const char* n, float x, float y, float z, float w) const { glProgramUniform4f(id, loc(n), x, y, z, w); }
    void setMat4(const char* n, const float* m) const { glProgramUniformMatrix4fv(id, loc(n), 1, GL_FALSE, m); }
};

namespace shaders {
// Compiles (or returns the cached) program. On failure logs the error with file:line and returns
// a program with id == 0 (callers must skip the draw).
const ShaderProgram& get(const ProgramDesc& desc);
// Convenience for single-stage compute and fullscreen passes.
const ShaderProgram& compute(const std::string& cs, const std::vector<std::string>& defines = {});
const ShaderProgram& fullscreen(const std::string& fs, const std::vector<std::string>& defines = {});
// Rebuilds every cached program from source (keeps the old program when a rebuild fails).
void reloadAll();
// Expands includes / pragma material for one stage (exposed for tests).
std::string preprocess(const std::string& path, const std::string& material, const std::string& displacement,
                       const std::vector<std::string>& defines, std::vector<std::string>* fileTable);
void shutdown();
}  // namespace shaders
