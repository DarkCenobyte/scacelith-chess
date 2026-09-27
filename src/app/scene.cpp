#include "scene.h"
#include <map>

bool AppContext::hasArg(const std::string& a) const {
    for (auto& s : args) if (s == a) return true;
    return false;
}
std::string AppContext::argValue(const std::string& a, const std::string& def) const {
    for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == a) return args[i + 1];
    return def;
}

namespace {
struct Entry { std::string desc; SceneFactory f; };
std::map<std::string, Entry>& registry() { static std::map<std::string, Entry> r; return r; }
}

void registerScene(const char* name, const char* desc, SceneFactory f) { registry()[name] = {desc, std::move(f)}; }
std::unique_ptr<Scene> createScene(const std::string& name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second.f();
}
std::vector<std::pair<std::string, std::string>> listScenes() {
    std::vector<std::pair<std::string, std::string>> r;
    for (auto& kv : registry()) r.push_back({kv.first, kv.second.desc});
    return r;
}
