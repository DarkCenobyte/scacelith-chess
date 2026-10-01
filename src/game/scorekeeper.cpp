#include "scorekeeper.h"
#include "../audio/audio.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "layout.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

using namespace m;

namespace game {

namespace {
constexpr uint32_t kSheetObjectId = 3000;   // + 16 per seat (Scoresheet::submit uses 16 ids)
constexpr uint32_t kPenObjectId = 3100;     // + 2 per seat
const vec3 kBlackInk(0.010f, 0.010f, 0.013f);

anim::WriteTask writeTask(anim::WriteTaskType type) {
    anim::WriteTask t;
    t.type = type;
    return t;
}
}  // namespace

sheet::PieceLetters localizedPieceLetters() {
    sheet::PieceLetters l;
    std::istringstream in(i18n::tr("scoresheet.pieces"));
    std::string k, q, r, b, n;
    if (in >> k >> q >> r >> b >> n) {
        l.king = k;
        l.queen = q;
        l.rook = r;
        l.bishop = b;
        l.knight = n;
    }
    return l;
}

std::string scoresheetDate(bool fixedForScreenshots) {
    int d = 28, mo = 9, y = 2026;
    if (!fixedForScreenshots) {
        std::time_t now = std::time(nullptr);
        if (const std::tm* tm = std::localtime(&now)) {
            d = tm->tm_mday;
            mo = tm->tm_mon + 1;
            y = tm->tm_year + 1900;
        }
    }
    char dd[16], mm[16], yy[16];
    std::snprintf(dd, sizeof dd, "%02d", d);
    std::snprintf(mm, sizeof mm, "%02d", mo);
    std::snprintf(yy, sizeof yy, "%04d", y);
    return i18n::trf("scoresheet.date_format", {dd, mm, yy});
}

bool Scorekeeper::init(bool clockOnPositiveX) {
    for (int s = 0; s < 2; ++s) {
        Scoresheet::Config c;
        c.owner = s;
        c.clockOnPositiveX = clockOnPositiveX;
        c.seed = 1u + uint32_t(s) * 7919u;
        if (!sheets_[s].init(c)) {
            LOGW("scoresheet %d unavailable", s);
            return false;
        }
    }
    ready_ = true;
    return true;
}

void Scorekeeper::shutdown() {
    for (auto& s : sheets_) s.shutdown();
    Scoresheet::releaseShared();
    ready_ = false;
}

void Scorekeeper::newGame(anim::Animator* anim, bool clockOnPositiveX, const Player players[2], int round,
                          const std::string& date) {
    anim_ = anim;
    round_ = round;
    date_ = date;
    recording_ = headerWritten_ = finished_ = false;
    hasDetails_ = false;
    details_ = Details();
    ledger_.reset();
    sheet::PieceLetters letters = localizedPieceLetters();
    for (int s = 0; s < 2; ++s) {
        players_[s] = players[s];
        handAside_[s] = false;
        hasPrevPen_[s] = false;
        if (!ready_) continue;
        Scoresheet& sh = sheets_[s];
        sh.setClockSide(clockOnPositiveX);
        sh.reset();
        sh.setHandStyle(players[s].handStyle);
        sh.setLetters(letters);
        if (players[s].blueInk) sh.setInkColor(Scoresheet::Config().inkColor);
        else sh.setInkColor(kBlackInk);
        refreshRest(s);
    }
}

void Scorekeeper::clear() {
    recording_ = headerWritten_ = finished_ = false;
    ledger_.reset();
    for (int s = 0; s < 2; ++s) {
        hasPrevPen_[s] = false;
        if (ready_) sheets_[s].reset();
    }
}

Scoresheet::Header Scorekeeper::header() const {
    Scoresheet::Header h;
    h.date = date_;
    h.round = std::to_string(round_);
    h.white = players_[0].name;
    h.black = players_[1].name;
    h.whiteElo = players_[0].elo > 0 ? std::to_string(players_[0].elo) : "";
    h.blackElo = players_[1].elo > 0 ? std::to_string(players_[1].elo) : "";
    if (!players_[0].rating.empty()) h.whiteElo = players_[0].rating;
    if (!players_[1].rating.empty()) h.blackElo = players_[1].rating;
    if (hasDetails_) {
        if (!details_.event.empty()) h.event = details_.event;
        if (!details_.round.empty()) h.round = details_.round;
        if (details_.noBoard) h.board.clear();
        h.note = details_.note;
        h.reference = details_.reference;
    }
    return h;
}

void Scorekeeper::writeHeaderInstantly() {
    if (!ready_ || headerWritten_) return;
    Scoresheet::Header h = header();
    for (auto& sh : sheets_) sh.writeHeaderInstant(h);
    headerWritten_ = true;
}

void Scorekeeper::writeMovesInstantly(const std::vector<std::string>& san) {
    if (!ready_) return;
    writeHeaderInstantly();
    for (int s = 0; s < 2; ++s)
        for (size_t i = size_t(ledger_.next(s)); i < san.size(); ++i) sheets_[s].writeMoveInstant(int(i), san[i]);
    ledger_.writtenInstantly(san);
    for (int s = 0; s < 2; ++s) refreshRest(s);
}

void Scorekeeper::startRecording() {
    if (recording_ || !ready_ || !anim_) return;
    recording_ = true;
    Scoresheet::Header h = header();
    for (int s = 0; s < 2; ++s) {
        anim::WriteTask pick = writeTask(anim::WriteTaskType::PickPen);
        pick.frame = sheets_[s].penRestTransform();
        anim_[s].enqueueWriting(pick);
        if (!headerWritten_) {
            anim::WriteTask w = writeTask(anim::WriteTaskType::Write);
            w.path = sheets_[s].beginHeader(h);
            anim_[s].enqueueWriting(w);
        }
    }
    headerWritten_ = true;
}

void Scorekeeper::recordMove(int ply, const std::string& san) {
    ledger_.record(ply, san);
    if (!ready_ || !anim_ || finished_) return;
    if (!recording_) startRecording();
    for (int s = 0; s < 2; ++s) {
        // A held sheet (hot-seat) and the plies from the write limit on wait. Moves completed
        // before this sheet caught up (a hold, a limit, else never in normal play) come first.
        const int end = std::min(ply + 1, ledger_.due(s));
        for (int p = ledger_.next(s); p < end; ++p) beginMoveEntry(s, p, ledger_.san(p));
    }
    LOGD("scoresheet: move %d queued, writing backlog %.1f s / %.1f s", ply + 1,
         anim_[0].writingRemainingTime(), anim_[1].writingRemainingTime());
}

void Scorekeeper::beginMoveEntry(int seat, int ply, const std::string& san) {
    Scoresheet& sh = sheets_[seat];
    if (sh.pageTurnNeeded(ply)) {
        sh.beginPageTurn();
        anim::WriteTask turn = writeTask(anim::WriteTaskType::TurnPage);
        Scoresheet* sp = &sh;
        turn.pageCorner = [sp](float s) { return sp->pageCorner(s); };
        anim_[seat].enqueueWriting(turn);
    }
    anim::WriteTask w = writeTask(anim::WriteTaskType::Write);
    w.path = sh.beginMove(ply, san);
    if (w.path.empty()) {
        // Nothing drawable (a glyph no font has): keep the entry queue in step with the tasks.
        anim::PenKey k;
        k.tip = sh.writingRest(ply) + vec3(0, 0.004f, 0);
        w.path.push_back(k);
    }
    anim_[seat].enqueueWriting(w);
    ledger_.begin(seat, ply);
}

void Scorekeeper::setHold(int seat, bool hold) {
    seat &= 1;
    if (ledger_.held(seat) == hold) return;
    ledger_.setHold(seat, hold);
    if (!hold) catchUp(seat);
}

void Scorekeeper::setWriteLimit(int plies) {
    ledger_.setWriteLimit(plies);
    for (int s = 0; s < 2; ++s) catchUp(s);
}

bool Scorekeeper::dropMoves(int fromPly) {
    if (!ledger_.dropMoves(fromPly)) return false;
    LOGD("scoresheet: moves from %d on dropped before being written", fromPly + 1);
    return true;
}

void Scorekeeper::catchUp(int seat) {
    if (!ready_ || !anim_ || finished_ || !recording_) return;
    const int end = ledger_.due(seat);
    for (int p = ledger_.next(seat); p < end; ++p) beginMoveEntry(seat, p, ledger_.san(p));
}

void Scorekeeper::finishGame(const std::string& result) {
    if (finished_) return;
    // The moves a held or limited sheet still owes come before the result.
    ledger_.setWriteLimit(-1);
    for (int s = 0; s < 2; ++s) {
        ledger_.setHold(s, false);
        catchUp(s);
    }
    finished_ = true;
    if (!ready_ || !anim_) return;
    for (int s = 0; s < 2; ++s) {
        if (!recording_) {
            sheets_[s].writeResultInstant(result);
            continue;
        }
        anim::WriteTask w = writeTask(anim::WriteTaskType::Write);
        w.path = sheets_[s].beginResult(result);
        if (w.path.empty()) {
            anim::PenKey k;
            k.tip = sheets_[s].writingRest(ledger_.next(s)) + vec3(0, 0.004f, 0);
            w.path.push_back(k);
        }
        anim_[s].enqueueWriting(w);
        anim::WriteTask put = writeTask(anim::WriteTaskType::PutPen);
        put.frame = sheets_[s].penRestTransform();
        anim_[s].enqueueWriting(put);
    }
}

void Scorekeeper::refreshRest(int seat) {
    if (!anim_ || !ready_) return;
    vec3 rest = sheets_[seat].writingRest(ledger_.next(seat));
    if (handAside_[seat]) {
        // Beside the pad's outer long edge (where the rest point already lies), near the bottom
        // of the page: the page stays in sight.
        mat4 pad = sheets_[seat].padTransform();
        vec3 c = pad.c[3].xyz(), right = pad.c[0].xyz(), down = pad.c[2].xyz();
        float side = dot(rest - c, right) >= 0.0f ? 1.0f : -1.0f;
        vec3 aside = c + right * (side * (0.5f * layout::SCORESHEET_WIDTH + 0.045f)) + down * (0.5f * layout::SCORESHEET_LENGTH - 0.035f);
        rest = vec3(aside.x, rest.y, aside.z);
    }
    anim_[seat].setWritingRest(rest);
}

void Scorekeeper::setHandAside(int seat, bool aside) {
    seat &= 1;
    if (handAside_[seat] == aside) return;
    handAside_[seat] = aside;
    refreshRest(seat);
}

void Scorekeeper::onEvent(int seat, const anim::Event& e) {
    if (!ready_ || !anim_) return;
    Scoresheet& sh = sheets_[seat];
    switch (e.type) {
    case anim::EventType::PenDown: {
        // The event comes out at the end of the animator's update, 'late' after the tip touched.
        const float late = std::max(0.0f, anim_[seat].time() - e.time), now = anim_[seat].writingPathTime();
        const sheet::PenStrokeSound s =
            sheet::penStrokeSound(sh.writingPath(), now >= 0.0f ? now - late : -1.0f, late, e.position,
                                  anim_[seat].eyeCameraTransform().translation(), audio::listenerPosition());
        audio::playPenStroke(s.position, s.seconds, s.gain);
        break;
    }
    case anim::EventType::WritingDone:
        sh.finishEntry();
        refreshRest(seat);
        break;
    case anim::EventType::PageGripped: audio::play(audio::Sfx::PageTurn, sh.pageCorner(0.0f), 0.9f); break;
    case anim::EventType::PageTurned:
        audio::play(audio::Sfx::PageFlap, sh.pageCorner(1.0f), 0.8f);
        sh.finishPageTurn();
        LOGD("scoresheet %d: page turned", seat);
        break;
    case anim::EventType::PenPut: audio::play(audio::Sfx::PenTap, e.transform.c[3].xyz(), 0.5f, 0.8f); break;
    default: break;
    }
}

void Scorekeeper::update() {
    if (!ready_) return;
    for (int s = 0; s < 2; ++s) {
        if (anim_) {
            float t = anim_[s].writingPathTime();
            if (t >= 0.0f) sheets_[s].setWritingTime(t);
            float p = anim_[s].pageTurnProgress();
            if (p >= 0.0f) sheets_[s].setTurnProgress(p);
        }
        sheets_[s].update();
    }
}

void Scorekeeper::submit(render::Renderer& r) {
    if (!ready_) return;
    for (int s = 0; s < 2; ++s) {
        sheets_[s].submit(r, kSheetObjectId + 16u * uint32_t(s));
        mat4 pen;
        if (!anim_ || !anim_[s].penTransform(pen)) pen = sheets_[s].penRestTransform();
        Scoresheet::submitPen(r, pen, kPenObjectId + 2u * uint32_t(s), hasPrevPen_[s] ? &prevPen_[s] : nullptr);
        prevPen_[s] = pen;
        hasPrevPen_[s] = true;
    }
}

bool Scorekeeper::writing(int seat) const {
    return (anim_ && anim_[seat & 1].writingBusy()) || (ready_ && sheets_[seat & 1].pendingEntries() > 0);
}

int Scorekeeper::backlog(int seat) const { return ready_ ? sheets_[seat & 1].pendingEntries() : 0; }

}  // namespace game
