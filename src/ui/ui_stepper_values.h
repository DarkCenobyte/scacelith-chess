// Internal to src/ui: the values of the steppers of the custom time control and of the custom
// engine limits (New Game, Watch a Game, challenges, direct match). Header-only, for the unit tests.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace ui {
namespace detail {

// The base times offered (15 s to 3 h).
inline const std::vector<int>& baseTimeValues() {
    static const std::vector<int> v = [] {
        std::vector<int> r;
        for (int s = 15; s < 180; s += 15) r.push_back(s);
        for (int s = 180; s < 600; s += 30) r.push_back(s);
        for (int s = 600; s < 3600; s += 60) r.push_back(s);
        for (int s = 3600; s <= 10800; s += 300) r.push_back(s);
        return r;
    }();
    return v;
}
// Stockfish's time per move in ms and nodes per move (0 = no limit).
inline const std::vector<int>& moveTimeValues() {
    static const std::vector<int> v = {0, 100, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000, 7500, 10000, 15000, 20000, 30000};
    return v;
}
inline const std::vector<int>& nodeValues() {
    static const std::vector<int> v = {0, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
                                       1000000, 2000000, 5000000, 10000000, 20000000, 50000000};
    return v;
}
// The index of the value nearest to 'value' (the first of two as near).
inline int nearestIndex(const std::vector<int>& v, int value) {
    int best = 0;
    for (int i = 0; i < int(v.size()); ++i)
        if (std::abs(v[size_t(i)] - value) < std::abs(v[size_t(best)] - value)) best = i;
    return best;
}
// The value a stepper over 'v' shows for 'value', whatever the int (a hand-edited .ini): the pages
// snap the values they read from the settings with it when they open, so that a game uses what
// they show.
inline int nearestValue(const std::vector<int>& v, int value) {
    return v[size_t(nearestIndex(v, std::clamp(value, v.front(), v.back())))];
}

}  // namespace detail
}  // namespace ui
