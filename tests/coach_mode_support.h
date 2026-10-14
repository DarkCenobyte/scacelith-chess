// Helpers of the Coach mode and Analysis mode tests (tests/coach_mode_*.cpp,
// tests/analysis_mode_*.cpp) around the headless game (headless.h):
//   - LogCapture: the game's log lines written to a file of the test while it lives (logx::init),
//     for the scene's transitions that nothing on screen shows (a demonstration on the table, a
//     takeback by hand, a comment decided) and to wait for them;
//   - the coach's voice model folder: an empty folder of the run (--coach-dir), so that the coach
//     speaks through subtitles whatever the machine has in its application data, and the download
//     is never started;
//   - finding the widgets of the last frame by their id (ui::im::makeId under the screen's id
//     stack) or by the last item showing a label (a confirmation button that repeats the label of
//     the menu entry it confirms).
#pragma once
#include "headless.h"
#include "core/log.h"
#include "tts/model_store.h"
#include "ui/ui_widgets.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace coachtest {

// The log of the game while it lives (stderr still gets every line).
class LogCapture {
public:
    LogCapture() : dir_("log") {
        path_ = dir_.file("game.log");
        logx::init(path_.c_str());
    }
    ~LogCapture() { logx::shutdown(); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::string text() const {
        std::ifstream in(std::filesystem::u8path(path_), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    bool has(const std::string& needle) const { return text().find(needle) != std::string::npos; }
    int count(const std::string& needle) const {
        const std::string t = text();
        int n = 0;
        for (size_t at = t.find(needle); at != std::string::npos; at = t.find(needle, at + needle.size())) ++n;
        return n;
    }
    // The text logged after 'mark' (a length of text() taken earlier).
    std::string since(size_t mark) const {
        const std::string t = text();
        return mark < t.size() ? t.substr(mark) : std::string();
    }
    size_t mark() const { return text().size(); }

private:
    headless::TempDir dir_;
    std::string path_;
};

// An empty folder for the coach's voice model, the same for the whole run (the shared App's
// arguments name it), and the model store pointed at it while a test runs (VoiceFolder restores
// the default when the test ends).
inline const std::string& voiceFolder() {
    static headless::TempDir dir("coach-voice");
    return dir.path;
}
struct VoiceFolder {
    VoiceFolder() { tts::setModelFolder(voiceFolder()); }
    ~VoiceFolder() { tts::setModelFolder(std::string()); }
};

// The id an item of the screen gets under these id scopes (ui::im::pushId), as makeId gives it
// between two frames (the id stack is empty then).
template <class Key>
ui::im::Id idIn(std::initializer_list<const char*> scopes, const Key& key) {
    for (const char* s : scopes) ui::im::pushId(s);
    const ui::im::Id id = ui::im::makeId(key);
    for (size_t i = 0; i < scopes.size(); ++i) ui::im::popId();
    return id;
}

inline const ui::im::TracedItem* findId(const headless::App& app, ui::im::Id id) {
    for (const auto& it : app.items())
        if (it.id == id) return &it;
    return nullptr;
}

inline const ui::im::TracedItem* findLast(const headless::App& app, const std::string& label) {
    const ui::im::TracedItem* found = nullptr;
    for (const auto& it : app.items())
        if (it.label == label) found = &it;
    return found;
}

inline bool clickLast(headless::App& app, const std::string& label) {
    const ui::im::TracedItem* it = findLast(app, label);
    if (!it) return false;
    app.clickItem(*it);
    return true;
}

inline bool clickId(headless::App& app, ui::im::Id id) {
    const ui::im::TracedItem* it = findId(app, id);
    if (!it) return false;
    app.clickItem(*it);
    return true;
}

inline bool disabled(const ui::im::TracedItem* it) { return it && (it->flags & ui::im::ITEM_DISABLED) != 0; }

// Frames of 'dt' until the log holds 'needle' (at most maxFrames).
inline bool waitLog(headless::App& app, const LogCapture& log, const std::string& needle, int maxFrames,
                    float dt = 1.0f / 20.0f) {
    return app.runUntil([&] { return log.has(needle); }, maxFrames, dt);
}

// Frames until 'done', for what the engine's thread decides: bounded by the wall clock (the
// engine starts and searches at its own pace, however fast the frames go), never by frames.
inline bool waitEngine(headless::App& app, const std::function<bool()>& done, double seconds = 30.0,
                       float dt = 1.0f / 20.0f) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) return false;
        if (!app.frame(dt)) return done();
    }
    return true;
}

// Frames of 'dt' until an item shows 'label', pressing Space every 'spaceEvery' frames meanwhile
// (Space skips what the coach says).
inline bool waitLabelSkipping(headless::App& app, const std::string& label, int maxFrames, int spaceEvery = 12,
                              float dt = 1.0f / 20.0f) {
    for (int i = 0; i < maxFrames; ++i) {
        if (app.hasLabel(label)) return true;
        if (spaceEvery > 0 && i % spaceEvery == spaceEvery - 1) app.key(plat::KEY_SPACE);
        else if (!app.frame(dt)) return app.hasLabel(label);
    }
    return app.hasLabel(label);
}

// The files of a folder whose name ends with 'ext'.
inline std::vector<std::string> filesIn(const std::string& folder, const std::string& ext) {
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
            out.push_back(it->path().u8string());
    }
    return out;
}

inline std::string readFile(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    out << text;
    return bool(out);
}

}  // namespace coachtest
