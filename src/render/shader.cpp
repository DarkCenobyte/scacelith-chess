#include "shader.h"
#include "../core/embedded.h"
#include "../core/files.h"
#include "../core/log.h"
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <system_error>

std::string ProgramDesc::key() const {
    std::string k = vs + "|" + tcs + "|" + tes + "|" + gs + "|" + fs + "|" + cs + "|" + material + "|" + displacement;
    for (auto& d : defines) k += "|" + d;
    return k;
}

namespace shaders {
namespace {
std::map<std::string, std::pair<ProgramDesc, std::unique_ptr<ShaderProgram>>> g_cache;

int fileIndex(std::vector<std::string>& table, const std::string& path) {
    for (size_t i = 0; i < table.size(); ++i)
        if (table[i] == path) return int(i);
    table.push_back(path);
    return int(table.size() - 1);
}

void expand(const std::string& path, const std::string& material, const std::string& displacement,
            std::vector<std::string>& table, std::set<std::string>& included, std::string& out, bool isRoot) {
    // Include-once is implemented with preprocessor guards (not by skipping text) so includes
    // inside #ifdef blocks behave like C: the guard only takes effect in active branches.
    if (included.count(path)) {  // cycle through the current include stack
        LOGW("recursive #include of %s ignored", path.c_str());
        return;
    }
    included.insert(path);
    std::string src = embedded::text(path.c_str());
    int idx = fileIndex(table, path);
    std::istringstream in(src);
    std::string line;
    int lineNo = 0;
    std::string guard = "SCACELITH_INC_" + std::to_string(std::hash<std::string>()(path) & 0xFFFFFFFFu);
    if (!isRoot) out += "#ifndef " + guard + "\n#define " + guard + "\n#line 1 " + std::to_string(idx) + "\n";
    while (std::getline(in, line)) {
        ++lineNo;
        size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line.compare(p, 8, "#include") == 0) {
            size_t a = line.find('"', p), b = line.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                expand(line.substr(a + 1, b - a - 1), material, displacement, table, included, out, false);
                out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(idx) + "\n";
                continue;
            }
        }
        if (p != std::string::npos && line.compare(p, 16, "#pragma material") == 0) {
            out += "\n";
            if (!material.empty()) {
                expand(material, material, displacement, table, included, out, false);
                out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(idx) + "\n";
            }
            continue;
        }
        if (p != std::string::npos && line.compare(p, 20, "#pragma displacement") == 0) {
            out += "\n";
            if (!displacement.empty()) {
                out += "#define MATERIAL_HAS_DISPLACEMENT 1\n";
                expand(displacement, material, displacement, table, included, out, false);
                out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(idx) + "\n";
            }
            continue;
        }
        if (p != std::string::npos && line.compare(p, 8, "#version") == 0) { out += "\n"; continue; }  // re-emitted by caller
        out += line;
        out += '\n';
    }
    if (!isRoot) out += "#endif\n";
    included.erase(path);
}

// SCACELITH_DUMP_SHADERS=<dir>: the folder (an existing one) the preprocessed sources are written
// to, resolved once to its canonical path (absolute, no "." or ".." parts, no symbolic link), the
// one the log names; "" = no dump.
const std::string& dumpDirectory() {
    static const std::string dir = [] {
        const char* env = std::getenv("SCACELITH_DUMP_SHADERS");
        if (!env || !*env) return std::string();
        std::error_code ec;
        std::filesystem::path p;
        try {
            p = std::filesystem::canonical(std::filesystem::u8path(env), ec);
        } catch (const std::exception&) {   // not UTF-8 (Windows)
            ec = std::make_error_code(std::errc::illegal_byte_sequence);
        }
        if (ec || !std::filesystem::is_directory(p, ec)) {
            LOGW("shaders: no dump, %s is not a folder", env);
            return std::string();
        }
        LOGI("shaders: dumping the preprocessed sources to %s", p.u8string().c_str());
        return p.u8string() + "/";
    }();
    return dir;
}

GLuint compileStage(GLenum type, const std::string& path, const ProgramDesc& d) {
    std::vector<std::string> table;
    std::string src = preprocess(path, d.material, d.displacement, d.defines, &table);
    const std::string& dump = dumpDirectory();
    if (!dump.empty()) {
        std::string name = path;
        for (char& c : name) if (c == '/') c = '_';
        if (FILE* f = files::create((dump + name + "." + std::to_string(std::hash<std::string>()(d.key())) + ".glsl").c_str())) {
            std::fputs(src.c_str(), f);
            std::fclose(f);
        }
    }
    GLuint sh = glCreateShader(type);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 1 ? len : 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, &log[0]);
        std::string files;
        for (size_t i = 0; i < table.size(); ++i) files += "  " + std::to_string(i) + " = " + table[i] + "\n";
        LOGE("shader compile failed: %s (material '%s')\n%s\nsource index table:\n%s", path.c_str(), d.material.c_str(),
             log.c_str(), files.c_str());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint link(const ProgramDesc& d) {
    struct St { GLenum type; const std::string* path; } stages[] = {
        {GL_VERTEX_SHADER, &d.vs}, {GL_TESS_CONTROL_SHADER, &d.tcs}, {GL_TESS_EVALUATION_SHADER, &d.tes},
        {GL_GEOMETRY_SHADER, &d.gs}, {GL_FRAGMENT_SHADER, &d.fs}, {GL_COMPUTE_SHADER, &d.cs}};
    GLuint prog = glCreateProgram();
    std::vector<GLuint> shs;
    bool ok = true;
    for (auto& st : stages) {
        if (st.path->empty()) continue;
        GLuint sh = compileStage(st.type, *st.path, d);
        if (!sh) { ok = false; break; }
        glAttachShader(prog, sh);
        shs.push_back(sh);
    }
    if (ok) {
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLint len = 0;
            glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
            std::string log(size_t(len > 1 ? len : 1), '\0');
            glGetProgramInfoLog(prog, len, nullptr, &log[0]);
            LOGE("program link failed (%s):\n%s", d.key().c_str(), log.c_str());
            ok = false;
        }
    }
    for (GLuint sh : shs) { glDetachShader(prog, sh); glDeleteShader(sh); }
    if (!ok) { glDeleteProgram(prog); return 0; }
    return prog;
}
}  // namespace

std::string preprocess(const std::string& path, const std::string& material, const std::string& displacement,
                       const std::vector<std::string>& defines, std::vector<std::string>* fileTable) {
    std::vector<std::string> localTable;
    std::vector<std::string>& table = fileTable ? *fileTable : localTable;
    std::string out = "#version 460 core\n";
    for (auto& d : defines) {
        std::string def = d;
        size_t eq = def.find('=');
        if (eq != std::string::npos) def[eq] = ' ';
        out += "#define " + def + "\n";
    }
    out += "#line 1 0\n";
    std::set<std::string> included;
    expand(path, material, displacement, table, included, out, true);
    return out;
}

const ShaderProgram& get(const ProgramDesc& d) {
    std::string k = d.key();
    auto it = g_cache.find(k);
    if (it != g_cache.end()) return *it->second.second;
    auto p = std::make_unique<ShaderProgram>();
    p->id = link(d);
    auto& slot = g_cache[k];
    slot.first = d;
    slot.second = std::move(p);
    return *slot.second;
}

const ShaderProgram& compute(const std::string& cs, const std::vector<std::string>& defines) {
    ProgramDesc d;
    d.cs = cs;
    d.defines = defines;
    return get(d);
}

const ShaderProgram& fullscreen(const std::string& fs, const std::vector<std::string>& defines) {
    ProgramDesc d;
    d.vs = "shaders/passes/fullscreen.vert";
    d.fs = fs;
    d.defines = defines;
    return get(d);
}

void reloadAll() {
    int ok = 0, failed = 0;
    for (auto& kv : g_cache) {
        GLuint np = link(kv.second.first);
        if (np) {
            if (kv.second.second->id) glDeleteProgram(kv.second.second->id);
            kv.second.second->id = np;
            ++ok;
        } else {
            ++failed;
        }
    }
    LOGI("shader reload: %d ok, %d failed", ok, failed);
}

void shutdown() {
    for (auto& kv : g_cache)
        if (kv.second.second->id) glDeleteProgram(kv.second.second->id);
    g_cache.clear();
}
}  // namespace shaders
