// Replay timeline and clock (see replay.h).
#include "replay.h"

#include <algorithm>

namespace game {
namespace replay {

float speedFactor(Speed s) {
    switch (s) {
    case Speed::X1: return 1.0f;
    case Speed::X2: return 2.0f;
    case Speed::X4: return 4.0f;
    case Speed::X8: return 8.0f;
    case Speed::Instant: return 0.0f;
    }
    return 1.0f;
}

Speed faster(Speed s) { return s == Speed::Instant ? s : Speed(int(s) + 1); }
Speed slower(Speed s) { return s == Speed::X1 ? s : Speed(int(s) - 1); }

// ---- Timeline -------------------------------------------------------------------------------------

void Timeline::build(const chess::pgn::Record& record, const Options& options) {
    options_ = options;
    plies_.clear();
    int64_t base = -1, inc = 0;
    if (!chess::pgn::parseTimeControl(record.tag("TimeControl"), base, inc)) {
        base = -1;
        inc = 0;
    }
    const chess::Position start = record.startPosition();
    chess::Color side = start.sideToMove();
    int64_t cur[2] = {base, base};
    if (base < 0) {
        // No base time: each side starts from its first recorded clock (close enough to show).
        chess::Color s = side;
        for (const chess::pgn::Ply& p : record.plies) {
            if (cur[s] < 0 && p.clockMs >= 0) cur[s] = p.clockMs;
            s = chess::opposite(s);
        }
    }
    start_[0] = cur[0];
    start_[1] = cur[1];
    clocksKnown_ = record.hasClocks() || (record.hasElapsed() && base >= 0);
    for (size_t i = 0; i < record.plies.size(); ++i) {
        const chess::pgn::Ply& r = record.plies[i];
        PlyTiming p;
        p.mover = side;
        p.clockBefore = cur[side];
        int64_t think = -1, after = -1;
        if (r.elapsedMs >= 0) think = r.elapsedMs;
        else if (r.clockMs >= 0 && cur[side] >= 0) think = std::max<int64_t>(0, cur[side] - r.clockMs + inc);
        if (r.clockMs >= 0) after = r.clockMs;
        else if (r.elapsedMs >= 0 && cur[side] >= 0) after = std::max<int64_t>(0, cur[side] - r.elapsedMs + inc);
        if (think >= 0) {
            p.thinkMs = think;
            p.measured = true;
        } else {
            // The default pace: quicker in the opening, a different length every move.
            const uint32_t h = (uint32_t(i) + 1u) * 2654435761u;
            const double u = double((h >> 12) & 1023u) / 1023.0;
            double ms = double(options.defaultThinkMs) * (0.55 + 0.9 * u);
            if (i < 10) ms *= 0.5;
            p.thinkMs = int64_t(ms);
        }
        if (after >= 0) cur[side] = after;
        p.clockAfter[0] = cur[0];
        p.clockAfter[1] = cur[1];
        plies_.push_back(p);
        side = chess::opposite(side);
    }
}

int64_t Timeline::waitMs(int i, Speed s) const {
    if (s == Speed::Instant || i < 0 || i >= plies()) return 0;
    const double f = speedFactor(s);
    const double shown = double(std::min(plies_[size_t(i)].thinkMs, options_.thinkCapMs));
    const double w = std::max(double(options_.minWaitMs) / f, shown / f - double(options_.moveAnimationMs));
    return int64_t(w);
}

int64_t Timeline::turnMs(int i, Speed s) const { return waitMs(i, s) + options_.moveAnimationMs; }

// ---- Clock -----------------------------------------------------------------------------------------

void ReplayClock::load(const chess::pgn::Record& record, const Options& options) {
    timeline_.build(record, options);
    first_ = record.startPosition().sideToMove();
    events_.clear();
    next_ = 0;
    paused_ = false;
    speed_ = Speed::X1;
    push(Event::Kind::SetPosition, 0);
    settle(0);
}

void ReplayClock::push(Event::Kind kind, int ply) {
    if (next_ == events_.size()) {
        events_.clear();
        next_ = 0;
    }
    events_.push_back(Event{kind, ply});
}

bool ReplayClock::poll(Event& out) {
    if (next_ >= events_.size()) return false;
    out = events_[next_++];
    return true;
}

void ReplayClock::settle(int ply) {
    ply_ = std::clamp(ply, 0, plies());
    waitDone_ = 0.0f;
    moveAge_ = 0.0;
    if (ply_ >= plies()) {
        phase_ = Phase::Finished;
        push(Event::Kind::Finished, ply_);
    } else {
        phase_ = Phase::Thinking;
    }
}

void ReplayClock::play() {
    push(Event::Kind::Play, ply_);
    phase_ = Phase::Moving;
    waitDone_ = 1.0f;
    moveAge_ = 0.0;
}

void ReplayClock::update(float dtSeconds) {
    if (paused_ || dtSeconds <= 0.0f) return;
    const double dtMs = double(dtSeconds) * 1000.0;
    if (phase_ == Phase::Moving) {
        moveAge_ += dtMs;
        return;
    }
    if (phase_ != Phase::Thinking) return;
    const int64_t wait = timeline_.waitMs(ply_, speed_);
    if (wait > 0) waitDone_ += float(dtMs / double(wait));
    if (wait <= 0 || waitDone_ >= 1.0f) play();
}

void ReplayClock::moveDone() {
    if (phase_ != Phase::Moving) return;
    settle(ply_ + 1);
}

void ReplayClock::pause() { paused_ = true; }
void ReplayClock::resume() { paused_ = false; }
void ReplayClock::setSpeed(Speed s) { speed_ = s; }  // the fraction of the wait already done is kept

void ReplayClock::stepForward() {
    paused_ = true;
    if (phase_ == Phase::Thinking) play();
}

void ReplayClock::stepBack() {
    paused_ = true;
    // While a move is being played the board goes back to the position before it.
    const int target = phase_ == Phase::Moving ? ply_ : ply_ - 1;
    if (target < 0) return;
    push(Event::Kind::SetPosition, target);
    settle(target);
}

void ReplayClock::jumpTo(int ply) {
    const int target = std::clamp(ply, 0, plies());
    push(Event::Kind::SetPosition, target);
    settle(target);
}

chess::Color ReplayClock::toMove() const {
    if (ply_ < plies()) return timeline_.ply(ply_).mover;
    return (plies() % 2) ? chess::opposite(first_) : first_;
}

float ReplayClock::thinkProgress() const {
    switch (phase_) {
    case Phase::Thinking: return std::min(1.0f, waitDone_);
    case Phase::Moving: return 1.0f;
    default: return 0.0f;
    }
}

int64_t ReplayClock::clockOf(chess::Color c, int ply) const {
    if (ply <= 0 || plies() == 0) return timeline_.startClock(c);
    return timeline_.ply(std::min(ply, plies()) - 1).clockAfter[c];
}

ClockView ReplayClock::clocks() const {
    ClockView v;
    if (!timeline_.clocksKnown()) return v;
    v.known = true;
    for (int c = 0; c < 2; ++c) v.ms[c] = std::max<int64_t>(0, clockOf(chess::Color(c), ply_));
    if (phase_ == Phase::Finished || ply_ >= plies()) return v;
    // The mover's clock runs down over the turn (the wait, then the animation) by the time the
    // move took in the game.
    const PlyTiming& p = timeline_.ply(ply_);
    const double wait = double(timeline_.waitMs(ply_, speed_));
    const double turn = double(timeline_.turnMs(ply_, speed_));
    const double elapsed = phase_ == Phase::Thinking ? double(waitDone_) * wait : wait + moveAge_;
    const double frac = turn > 0.0 ? std::min(1.0, elapsed / turn) : 1.0;
    if (p.clockBefore >= 0) v.ms[p.mover] = std::max<int64_t>(0, p.clockBefore - int64_t(double(p.thinkMs) * frac));
    v.running = paused_ ? -1 : int(p.mover);
    return v;
}

// ---- Endings ------------------------------------------------------------------------------------------

std::string endReasonKey(const chess::pgn::Record& record) {
    if (record.result == "*") return std::string();
    const std::string own = record.tag("ScacelithEnd");
    bool clean = !own.empty() && own.size() < 48;
    for (char c : own) clean = clean && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.');
    if (clean) return "reason." + own;
    const std::vector<chess::Position> pos = record.positions();
    const chess::Position& last = pos.back();
    if (last.isCheckmate()) return "reason.checkmate";
    if (last.isStalemate()) return "reason.stalemate";
    if (record.result == "1/2-1/2" && last.hasInsufficientMaterial()) return "reason.insufficient";
    std::string term = record.tag("Termination");
    for (char& c : term)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (term == "time forfeit") return "reason.timeout";
    return std::string();
}

// ---- Move numbers ----------------------------------------------------------------------------------

int moveNumberAfter(const chess::Position& start, int plies) {
    const int blackFirst = start.sideToMove() == chess::Black ? 1 : 0;
    return std::max(1, start.fullmoveNumber()) + std::max(0, plies + blackFirst - 1) / 2;
}

int sheetRowsAfter(const chess::Position& start, int plies) {
    const int blackFirst = start.sideToMove() == chess::Black ? 1 : 0;
    return plies > 0 ? (plies + blackFirst + 1) / 2 : 0;
}

}  // namespace replay
}  // namespace game
