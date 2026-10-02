// Unit tests for the shader preprocessor (shaders::preprocess, src/render/shader.cpp): the
// #version and defines header, the include guards, #include inside #ifdef, #pragma material and
// #pragma displacement, the #line mapping and its file table, and recursive includes.
#include "test.h"
#include "core/embedded.h"
#include "net/net_sys.h"
#include "render/shader.h"

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

// Small shader files beside the test executable, read through embedded::setOverrideDir (which
// takes them before the embedded table). The destructor removes them and clears the override.
struct ShaderFiles {
    std::string dir, prefix;
    std::map<std::string, std::string> text;  // shader path -> contents
    std::vector<std::string> written;
    ShaderFiles() {
#ifdef _WIN32
        const unsigned pid = unsigned(GetCurrentProcessId());
#else
        const unsigned pid = unsigned(getpid());
#endif
        dir = net::sys::exeDirectory();
        if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
        prefix = "shader-pp-test-" + std::to_string(pid) + "-";
        embedded::setOverrideDir(dir);
    }
    ~ShaderFiles() {
        for (const std::string& p : written) std::remove(p.c_str());
        embedded::setOverrideDir(std::string());
    }
    // Writes a file and returns its shader path (what #include and preprocess() take).
    std::string add(const std::string& name, const std::string& contents) {
        const std::string path = prefix + name, full = dir + "/" + path;
        if (FILE* f = std::fopen(full.c_str(), "wb")) {
            std::fwrite(contents.data(), 1, contents.size(), f);
            std::fclose(f);
            written.push_back(full);
        }
        text[path] = contents;
        return path;
    }
};

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

int count(const std::vector<std::string>& v, const std::string& line) {
    int n = 0;
    for (const std::string& l : v) n += l == line ? 1 : 0;
    return n;
}

int indexOf(const std::vector<std::string>& v, const std::string& line, int from = 0) {
    for (int i = from; i < int(v.size()); ++i)
        if (v[size_t(i)] == line) return i;
    return -1;
}

// Every emitted code line (neither blank nor a directive) must be the source line that the last
// "#line N file" names (in GLSL the line after the directive is line N of that file). Returns how
// many lines were checked.
int checkLineMapping(const std::vector<std::string>& out, const std::vector<std::string>& table, const ShaderFiles& fs) {
    std::vector<std::vector<std::string>> sources;
    for (const std::string& path : table) {
        auto it = fs.text.find(path);
        sources.push_back(it != fs.text.end() ? lines(it->second) : std::vector<std::string>());
    }
    int file = -1, line = 0, checked = 0;
    for (const std::string& l : out) {
        int n = 0, idx = 0;
        if (std::sscanf(l.c_str(), "#line %d %d", &n, &idx) == 2) {
            file = idx;
            line = n;
            continue;
        }
        if (!l.empty() && l[0] != '#') {
            const bool inRange = file >= 0 && file < int(sources.size()) && line >= 1 && line <= int(sources[size_t(file)].size());
            CHECK(inRange);
            if (inRange) CHECK_EQ(l, sources[size_t(file)][size_t(line - 1)]);
            ++checked;
        }
        ++line;
    }
    return checked;
}

}  // namespace

// The #version line comes first (the file's own is blanked, a missing one is added), then the
// defines with their first '=' turned into a space, then "#line 1 0".
TEST(shader_preprocess_header_and_defines) {
    ShaderFiles fs;
    const std::string root = fs.add("header.glsl", "#version 460 core\nvoid main() {}\n");
    std::vector<std::string> table;
    const std::string out = shaders::preprocess(root, "", "", {"FEATURE", "COUNT=3", "PAIR=a=b"}, &table);
    CHECK_EQ(out, std::string("#version 460 core\n#define FEATURE\n#define COUNT 3\n#define PAIR a=b\n#line 1 0\n"
                              "\nvoid main() {}\n"));
    CHECK_EQ(table.size(), size_t(1));
    CHECK(!table.empty() && table[0] == root);

    const std::string bare = fs.add("bare.glsl", "void main() {}\n");
    CHECK_EQ(shaders::preprocess(bare, "", "", {}, nullptr), std::string("#version 460 core\n#line 1 0\nvoid main() {}\n"));
}

// A file included twice is emitted twice, each copy inside the same guard (include-once works
// like C, so an include inside #ifdef still counts only where the branch is active). Nested
// includes, the file table and every #line are checked as well.
TEST(shader_preprocess_includes) {
    ShaderFiles fs;
    const std::string inc2 = fs.add("inc2.glsl", "const float INC2 = 3.0;\n");
    const std::string inc = fs.add("inc.glsl", "const float INC = 2.0;\n#include \"" + inc2 + "\"\nfloat b = INC2;\n");
    const std::string root = fs.add("root.glsl",
                                    "#version 460 core\n"
                                    "// root\n"
                                    "#include \"" + inc + "\"\n"
                                    "float a = INC;\n"
                                    "#ifdef FEATURE\n"
                                    "    #include \"" + inc + "\"\n"
                                    "#endif\n"
                                    "void main() {}\n");
    std::vector<std::string> table;
    const std::vector<std::string> out = lines(shaders::preprocess(root, "", "", {}, &table));
    CHECK_EQ(table.size(), size_t(3));
    if (table.size() == 3) {
        CHECK(table[0] == root);
        CHECK(table[1] == inc);
        CHECK(table[2] == inc2);
    }
    CHECK_EQ(count(out, "#version 460 core"), 1);
    CHECK_EQ(count(out, "const float INC = 2.0;"), 2);
    CHECK_EQ(count(out, "const float INC2 = 3.0;"), 2);
    CHECK_EQ(count(out, "void main() {}"), 1);

    // Each copy: "#ifndef G", "#define G", "#line 1 <file>"; one guard name per file.
    std::map<std::string, std::vector<std::string>> guards;  // "#line 1 <file>" -> guard names
    for (size_t i = 0; i + 2 < out.size(); ++i) {
        if (out[i].compare(0, 22, "#ifndef SCACELITH_INC_") != 0) continue;
        const std::string g = out[i].substr(8);
        CHECK_EQ(out[i + 1], "#define " + g);
        guards[out[i + 2]].push_back(g);
    }
    CHECK_EQ(guards.size(), size_t(2));
    CHECK_EQ(guards["#line 1 1"].size(), size_t(2));
    CHECK_EQ(guards["#line 1 2"].size(), size_t(2));
    if (guards["#line 1 1"].size() == 2) CHECK(guards["#line 1 1"][0] == guards["#line 1 1"][1]);
    if (guards["#line 1 2"].size() == 2) CHECK(guards["#line 1 2"][0] == guards["#line 1 2"][1]);
    if (!guards["#line 1 1"].empty() && !guards["#line 1 2"].empty()) CHECK(guards["#line 1 1"][0] != guards["#line 1 2"][0]);

    // The second copy sits inside the #ifdef, before the #line that resumes the root at line 7.
    const int ifdef = indexOf(out, "#ifdef FEATURE");
    const int second = indexOf(out, "const float INC = 2.0;", indexOf(out, "const float INC = 2.0;") + 1);
    const int resume = indexOf(out, "#line 7 0");
    CHECK(ifdef >= 0 && ifdef < second && second < resume);
    CHECK(resume >= 0 && resume + 1 < int(out.size()) && out[size_t(resume + 1)] == "#endif");
    CHECK(indexOf(out, "#line 4 0") >= 0);
    CHECK_EQ(count(out, "#line 3 1"), 2);  // after the nested include, back in inc.glsl

    // root: 3 code lines; inc: 2 code lines and inc2: 1, each emitted twice.
    CHECK_EQ(checkLineMapping(out, table, fs), 9);
}

// #pragma displacement and #pragma material are replaced by the given files (blank when none);
// MATERIAL_HAS_DISPLACEMENT is defined only with a displacement file.
TEST(shader_preprocess_material_pragmas) {
    ShaderFiles fs;
    const std::string mat = fs.add("mat.glsl", "vec3 surface() { return vec3(1.0); }\n");
    const std::string disp = fs.add("disp.glsl", "vec3 displace(vec3 p) { return p; }\n");
    const std::string root = fs.add("stage.glsl", "#version 460 core\n#pragma displacement\n#pragma material\nvoid main() {}\n");

    std::vector<std::string> table;
    std::vector<std::string> out = lines(shaders::preprocess(root, mat, disp, {}, &table));
    CHECK_EQ(table.size(), size_t(3));
    if (table.size() == 3) {
        CHECK(table[1] == disp);
        CHECK(table[2] == mat);
    }
    const int def = indexOf(out, "#define MATERIAL_HAS_DISPLACEMENT 1");
    const int dispBody = indexOf(out, "vec3 displace(vec3 p) { return p; }");
    const int matBody = indexOf(out, "vec3 surface() { return vec3(1.0); }");
    CHECK(def >= 0 && def < dispBody && dispBody < matBody);
    CHECK_EQ(count(out, "#define MATERIAL_HAS_DISPLACEMENT 1"), 1);
    for (const std::string& l : out) CHECK(l.find("#pragma") == std::string::npos);
    CHECK_EQ(checkLineMapping(out, table, fs), 3);

    // A material without displacement: no define, no displacement body.
    table.clear();
    out = lines(shaders::preprocess(root, mat, "", {}, &table));
    CHECK_EQ(table.size(), size_t(2));
    CHECK_EQ(count(out, "#define MATERIAL_HAS_DISPLACEMENT 1"), 0);
    CHECK_EQ(count(out, "vec3 displace(vec3 p) { return p; }"), 0);
    CHECK_EQ(count(out, "vec3 surface() { return vec3(1.0); }"), 1);
    CHECK_EQ(checkLineMapping(out, table, fs), 2);

    // Neither: both pragmas become blank lines, so the root keeps its line numbers.
    CHECK_EQ(shaders::preprocess(root, "", "", {}, nullptr), std::string("#version 460 core\n#line 1 0\n\n\n\nvoid main() {}\n"));
}

// An include of a file already on the include stack is ignored (with a warning).
TEST(shader_preprocess_recursive_include) {
    ShaderFiles fs;
    const std::string a = fs.prefix + "a.glsl", b = fs.prefix + "b.glsl";
    fs.add("a.glsl", "#include \"" + b + "\"\nfloat a;\n");
    fs.add("b.glsl", "#include \"" + a + "\"\nfloat b;\n");
    std::vector<std::string> table;
    const std::vector<std::string> out = lines(shaders::preprocess(a, "", "", {}, &table));
    CHECK_EQ(table.size(), size_t(2));
    CHECK_EQ(count(out, "float a;"), 1);
    CHECK_EQ(count(out, "float b;"), 1);
    CHECK(indexOf(out, "float b;") < indexOf(out, "float a;"));
    CHECK_EQ(checkLineMapping(out, table, fs), 2);

    const std::string self = fs.add("self.glsl", "float s;\n#include \"" + fs.prefix + "self.glsl\"\nvoid main() {}\n");
    CHECK_EQ(shaders::preprocess(self, "", "", {}, nullptr), std::string("#version 460 core\n#line 1 0\nfloat s;\n#line 3 0\nvoid main() {}\n"));
}
