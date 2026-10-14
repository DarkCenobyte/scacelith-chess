// Helpers of the Analysis mode tests in the game (tests/analysis_mode_*_tests.cpp), beside those of
// tests/coach_mode_support.h and tests/game_scene_testkit.h:
//   - the controls and the moves of the analysis overlay (ui_analysis.cpp) found by their ids;
//   - the settings file these tests start the game with: the coach's voice already offered, so
//     that no analysis opens the voice model's prompt (answering it "Not now" turns the coach's
//     voice off, and the voice model's state, one for the whole process, would then offer the
//     model again to the next App of the run, whose settings have the voice on);
//   - an earlier review of a game written in the evaluation cache (analysis/cache.h) as the game
//     leaves it, its evaluations made up by hand, so that the game opens with its review complete
//     without the engine;
//   - the settings and the interface language put back when a test ends.
#pragma once
#include "coach_mode_support.h"
#include "game_scene_testkit.h"
#include "ai/analysis.h"
#include "analysis/cache.h"
#include "analysis/review.h"
#include "chess/chess.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace analysismode {

// ---- The overlay ----------------------------------------------------------------------------------
// Its controls ("start", "back", "play", "forward", "end", "comments", "voice", "arrows") and the
// cell of each move (by ply).
inline ui::im::Id control(const char* name) { return coachtest::idIn({"analysis.controls"}, name); }
inline ui::im::Id cell(int ply) { return coachtest::idIn({"analysis.moves"}, ply); }

inline bool shown(const headless::App& app, const char* name) { return coachtest::findId(app, control(name)) != nullptr; }
inline bool enabled(const headless::App& app, const char* name) {
    const ui::im::TracedItem* it = coachtest::findId(app, control(name));
    return it && !coachtest::disabled(it);
}
// A click on a control (then one frame): false when the last frame did not draw it.
inline bool press(headless::App& app, const char* name) { return coachtest::clickId(app, control(name)); }

// ---- Settings ---------------------------------------------------------------------------------------
// The settings file of an App of these tests (written again before each start; the game saves its
// changes in it).
inline std::string writeIni(const headless::TempDir& dir) {
    const std::string path = dir.file("analysis.ini");
    coachtest::writeFile(path, "[graphics]\nbrightness_calibrated = true\n[coach]\nvoice_offered = true\n");
    return path;
}

// game::settings() and the interface language as a test found them, put back at its end.
struct Restore {
    game::Settings settings = game::settings();
    ~Restore() {
        game::settings() = settings;
        i18n::setLanguage("en");
    }
};

// ---- Files ------------------------------------------------------------------------------------------
// Removes the files of a folder whose name ends with 'ext' (the folder stays).
inline void clearFolder(const std::string& folder, const std::string& ext) {
    std::error_code ec;
    for (const std::string& f : coachtest::filesIn(folder, ext)) std::filesystem::remove(std::filesystem::u8path(f), ec);
}

inline void makeFolder(const std::string& folder) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
}

inline std::string analysisFolder(const headless::App& app) { return app.folder().path + "analysis/"; }

// ---- An earlier review --------------------------------------------------------------------------------
// A line of an engine's report on a position: its score from the side to move ('mate' > 0: it
// mates in that many moves) and its moves ("e2e4 e7e5").
struct EngineLine {
    int cp = 0;
    const char* pv = "";
    int mate = 0;
};

// The review of a game from 'fen' (nullptr: the start position) with these moves (SAN), as the
// scene sets it up.
inline analysis::GameReview reviewOf(const char* fen, std::initializer_list<const char*> sans) {
    chess::Position start;
    if (fen) start.setFEN(fen);
    chess::Position p = start;
    std::vector<chess::Move> moves;
    for (const char* s : sans) {
        const chess::Move m = p.parseSAN(s);
        if (!m.valid()) break;
        moves.push_back(m);
        p.makeMove(m);
    }
    analysis::GameReview r;
    r.reset(start, moves);
    return r;
}

// The review given these engine reports, one list of lines per position (an empty list: none;
// a position without moves needs none), each at the deep pass's depth, written in 'folder' as the
// game writes a review it completed. Returns the review's key ("" when it could not be written).
inline std::string writeReview(const std::string& folder, analysis::GameReview r,
                               const std::vector<std::vector<EngineLine>>& reports) {
    const int depth = r.settings().deepDepth;
    for (int i = 0; i < r.positions() && i < int(reports.size()); ++i) {
        if (reports[size_t(i)].empty()) continue;
        ai::Analysis a;
        a.ok = true;
        a.whiteToMove = r.positionAt(i).sideToMove() == chess::White;
        a.depth = depth;
        for (const EngineLine& l : reports[size_t(i)]) {
            ai::PvLine line;
            line.multipv = int(a.lines.size()) + 1;
            line.depth = depth;
            line.score.cp = l.cp;
            line.score.mate = l.mate;
            std::istringstream in(l.pv);
            for (std::string m; in >> m;) line.pv.push_back(m);
            a.lines.push_back(line);
        }
        a.bestMove = a.lines.front().pv.empty() ? std::string() : a.lines.front().pv.front();
        r.accept(i, a);
    }
    if (!r.complete()) return std::string();
    return analysis::saveCache(folder, r.key(), r.evaluations(), depth) ? r.key() : std::string();
}

}  // namespace analysismode
