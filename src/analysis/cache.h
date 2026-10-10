// Analysis mode: the evaluations of the games already reviewed, kept in <application data>/analysis/
// (one small text file per game, named after GameReview::key()), so that a game opened again shows
// its bar, symbols and comments at once. Engine-free; the scene loads before the review starts and
// saves when it leaves the game (or the review completes). Files are untrusted input: a damaged,
// foreign or oversized file is ignored (and replaced at the next save). The folder keeps the 200
// most recently used games: older files are deleted when a new one is written.
#pragma once
#include "review.h"
#include <string>
#include <vector>

namespace analysis {

// "<folder><key>.txt"
std::string cachePath(const std::string& folder, const std::string& key);
// The evaluations saved for 'key', or false (no file, unreadable, another game, another number of
// positions). 'deepDepth' (optional) receives the deep pass's depth of the review that saved them
// (0 when the file does not say), for GameReview::restore().
bool loadCache(const std::string& folder, const std::string& key, int positions, std::vector<PositionEval>& out,
               int* deepDepth = nullptr);
// Writes them (atomically: a temporary file renamed) and trims the folder to the newest 200 files.
// 'deepDepth': the review's Settings::deepDepth (0: not written).
bool saveCache(const std::string& folder, const std::string& key, const std::vector<PositionEval>& evals,
               int deepDepth = 0);

}  // namespace analysis
