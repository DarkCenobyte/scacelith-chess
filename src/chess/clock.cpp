// Time controls (presets, labels, FIDE category) and the digital chess clock.
#include "chess/chess.h"

#include <algorithm>
#include <cstdio>

namespace chess {

namespace {

TimeControl makeTC(bool unlimited, int minutes, int incrementSeconds) {
    TimeControl t;
    t.unlimited = unlimited;
    t.baseMs = unlimited ? 0 : int64_t(minutes) * 60000;
    t.incrementMs = int64_t(incrementSeconds) * 1000;
    t.delayMs = 0;
    return t;
}

// Seconds with at most one decimal: "2", "2.5".
std::string seconds(int64_t ms) {
    char buf[32];
    if (ms % 1000 == 0) std::snprintf(buf, sizeof buf, "%lld", (long long)(ms / 1000));
    else std::snprintf(buf, sizeof buf, "%.1f", double(ms) / 1000.0);
    return buf;
}

}  // namespace

const std::vector<TimeControl>& timeControlPresets() {
    static const std::vector<TimeControl> presets = {
        makeTC(true, 0, 0),   makeTC(false, 1, 0),  makeTC(false, 3, 0),   makeTC(false, 3, 2),
        makeTC(false, 5, 0),  makeTC(false, 5, 3),  makeTC(false, 10, 0),  makeTC(false, 10, 5),
        makeTC(false, 15, 10), makeTC(false, 30, 0), makeTC(false, 30, 20), makeTC(false, 90, 30),
    };
    return presets;
}

std::string TimeControl::label() const {
    if (unlimited) return "Unlimited";
    if (delayMs == 0) {
        for (const TimeControl& p : timeControlPresets())
            if (!p.unlimited && p.baseMs == baseMs && p.incrementMs == incrementMs)
                return std::to_string(baseMs / 60000) + "+" + std::to_string(incrementMs / 1000);
    }
    const long long totalSec = (long long)(baseMs / 1000);
    char base[32];
    if (totalSec % 60 == 0) std::snprintf(base, sizeof base, "%lld", totalSec / 60);
    else std::snprintf(base, sizeof base, "%lld:%02lld", totalSec / 60, totalSec % 60);
    std::string s = std::string("Custom ") + base + "+" + seconds(incrementMs);
    if (delayMs > 0) s += " d" + seconds(delayMs);
    return s;
}

std::string TimeControl::pgnTag() const {
    if (unlimited) return "-";
    std::string s = seconds(baseMs);
    if (incrementMs > 0) s += "+" + seconds(incrementMs);
    return s;
}

TimeControl::Category TimeControl::category() const {
    if (unlimited) return Category::Unlimited;
    const int64_t estimate = baseMs + 60 * incrementMs;  // time for 60 moves per player
    if (estimate <= 10 * 60000) return Category::Blitz;
    if (estimate < 60 * 60000) return Category::Rapid;
    return Category::Standard;
}

// ---- Clock ----------------------------------------------------------------------------------

void Clock::setup(const TimeControl& tc) {
    tc_ = tc;
    remaining_[0] = remaining_[1] = tc.unlimited ? 0 : tc.baseMs;
    delayLeft_ = 0;
    used_ = 0;
    running_ = White;
    active_ = false;
    turnBegun_ = false;
    flagged_[0] = flagged_[1] = false;
}

void Clock::start(Color running) {
    if (flagged_[0] || flagged_[1]) return;
    if (!turnBegun_ || running != running_) {
        used_ = 0;
        delayLeft_ = tc_.unlimited ? 0 : tc_.delayMs;
    }
    running_ = running;
    active_ = true;
    turnBegun_ = true;
}

void Clock::stop() { active_ = false; }

void Clock::press(Color mover) {
    if (!active_ || mover != running_ || flagged_[mover]) return;
    if (!tc_.unlimited) {
        if (tc_.delayMs > 0) remaining_[mover] += std::min(used_, tc_.delayMs);  // Bronstein
        remaining_[mover] += tc_.incrementMs;                                     // Fischer
    }
    running_ = opposite(mover);
    used_ = 0;
    delayLeft_ = tc_.unlimited ? 0 : tc_.delayMs;
}

void Clock::update(int64_t elapsedMs) {
    if (!active_ || elapsedMs <= 0) return;
    const Color c = running_;
    used_ += elapsedMs;
    if (tc_.unlimited) {
        remaining_[c] += elapsedMs;  // counts up: time used
        return;
    }
    delayLeft_ = std::max<int64_t>(0, tc_.delayMs - used_);
    remaining_[c] -= elapsedMs;
    if (remaining_[c] <= 0) {
        remaining_[c] = 0;
        flagged_[c] = true;
        active_ = false;
    }
}

int64_t Clock::remainingMs(Color c) const { return remaining_[c]; }

bool Clock::flagged(Color c) const { return flagged_[c]; }

void Clock::addTime(Color c, int64_t ms) {
    if (tc_.unlimited || flagged_[c]) return;
    remaining_[c] += ms;
}

}  // namespace chess
