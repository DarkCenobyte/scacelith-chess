// The moves each scoresheet owes and which of them it may write now: the engine-free part of
// Scorekeeper, header-only so that the unit tests (tests/scoresheet_tests.cpp) reach it.
//
// Every completed move is recorded; each sheet writes the recorded moves in order, one entry per
// ply, and next(seat) is the ply it writes next. The plies [next(seat), due(seat)) may be begun
// now. They wait while the sheet is held (hot-seat: the player to move writes the opponent's move
// at the start of their turn) and from the write limit on (Coach mode: the human's move waits
// until the coach has said whether to take it back). Moves that neither sheet has begun can be
// dropped (a takeback), and the move played instead is recorded at the same ply.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace game {

class ScoreLedger {
public:
    // A new game: nothing recorded, no hold, no limit.
    void reset() {
        moves_.clear();
        next_[0] = next_[1] = 0;
        hold_[0] = hold_[1] = false;
        limit_ = -1;
    }

    // Move 'ply' (0 = White's first) completed with 'san'.
    void record(int ply, const std::string& san) {
        if (ply < 0) return;
        if (int(moves_.size()) <= ply) moves_.resize(size_t(ply) + 1);
        moves_[size_t(ply)] = san;
    }
    // Moves on the sheets already (written instantly): the record is 'san', and each sheet goes
    // on after it.
    void writtenInstantly(const std::vector<std::string>& san) {
        moves_ = san;
        for (int s = 0; s < 2; ++s) next_[s] = std::max(next_[s], int(san.size()));
    }
    int recorded() const { return int(moves_.size()); }
    const std::string& san(int ply) const { return moves_[size_t(ply)]; }   // 0 <= ply < recorded()

    int next(int seat) const { return next_[seat & 1]; }
    // End of the plies sheet 'seat' may begin now (next(seat) when it may begin none).
    int due(int seat) const {
        seat &= 1;
        if (hold_[seat]) return next_[seat];
        int end = recorded();
        if (limit_ >= 0) end = std::min(end, limit_);
        return std::max(end, next_[seat]);
    }
    // The writing hand of 'seat' was given the entry of 'ply' (= next(seat)).
    void begin(int seat, int ply) { next_[seat & 1] = ply + 1; }

    void setHold(int seat, bool hold) { hold_[seat & 1] = hold; }
    bool held(int seat) const { return hold_[seat & 1]; }

    // Plies >= 'plies' are recorded but not written yet; -1: no limit.
    void setWriteLimit(int plies) { limit_ = plies < 0 ? -1 : plies; }
    int writeLimit() const { return limit_; }

    // Forgets the moves from 'fromPly' on, when neither sheet has begun any of them. The move
    // then recorded at 'fromPly' is written as any other. False (nothing changes) otherwise.
    bool dropMoves(int fromPly) {
        if (fromPly < 0 || next_[0] > fromPly || next_[1] > fromPly) return false;
        if (recorded() > fromPly) moves_.resize(size_t(fromPly));
        return true;
    }

private:
    std::vector<std::string> moves_;   // SAN of every recorded move (for late starts)
    int next_[2] = {0, 0};             // next ply each sheet will write
    bool hold_[2] = {false, false};
    int limit_ = -1;
};

}  // namespace game
