// Fake server and fake direct-match peer (see online_mock.h).
#include "online_mock.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include "../math/math.h"
#include "../net/protocol_gen.h"
#include "layout.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>

namespace net {
namespace mock {

namespace {

// ---- Clock ------------------------------------------------------------------------------------
bool g_virtual = false;
double g_virtualMs = 1790596800000.0;  // 2026-09-28 12:00 UTC
int g_opponentDrop = 0, g_connectionDrop = 0;  // pending developer requests (seconds)
bool g_manualClock = false;                    // --online-manual-clock: games with autoPress off

double wallMs() {
    using namespace std::chrono;
    return double(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// Protocol values (dedicated-server/src/protocol/schema.js).
enum Status { Ongoing = 0, WhiteWins = 1, BlackWins = 2, Draw = 3, Aborted = 4 };
enum Reason {
    RNone = 0, RResignation = 2, RTimeout = 3, RAgreement = 12, RThreefoldClaim = 10, RFiftyClaim = 11,
    RAbandonment = 20, RAbandonmentVsInsufficient = 21, RAborted = 22, RNoShow = 23
};
enum GameEventKind { DrawOffered = 1, DrawDeclined = 2, PlayerDisconnected = 3, PlayerReconnected = 4, RematchOffered = 5, RematchDeclined = 6 };
enum Err {
    ErrNotInGame = 100, ErrNotYourTurn = 101, ErrIllegalMove = 102, ErrStalePly = 103, ErrDesync = 104, ErrGameOver = 105,
    ErrAlreadyInGame = 106, ErrInvalidCategory = 107, ErrDrawOfferLimit = 108, ErrNothingToClaim = 109, ErrAbortNotAllowed = 110,
    ErrNoPendingOffer = 111, ErrFlagFell = 112, ErrUserUnavailable = 202, ErrCannotChallengeSelf = 204, ErrCodeInvalid = 205,
    ErrRatedRequiresOfficialTc = 206, ErrMatchmakingCooldown = 207, ErrInvalidTimeControl = 208, ErrRematchUnavailable = 209
};
enum MoveFlagBits { FCheck = 64, FMate = 128 };

constexpr double kHttpMin = 250.0, kHttpMax = 520.0;  // HTTPS round trip
constexpr double kOneWay = 17.0;                      // realtime one-way latency
constexpr double kFirstMoveMs = 30000.0;              // FIRST_MOVE_TIMEOUT_MS
constexpr double kIdEpochMs = 1767225600000.0;        // 2026-01-01 (ids.js)

const char* const kOpponents[] = {"Wilhelmina", "orlov_b", "Kasparilla", "MarieCurieux", "Tal_Returns",
                                  "quietbishop", "Nimzo_fan", "Aurelien_P", "sveta.k", "RookLift"};

std::vector<Category> officialCategories() {
    static const int tc[][2] = {{1, 0}, {3, 0}, {3, 2}, {5, 0}, {5, 3}, {10, 0}, {10, 5}, {15, 10}, {30, 0}, {30, 20}, {90, 30}};
    std::vector<Category> out;
    for (auto& t : tc) {
        Category c;
        c.id = std::to_string(t[0]) + "+" + std::to_string(t[1]);
        c.baseSec = t[0] * 60;
        c.incSec = t[1];
        out.push_back(c);
    }
    return out;
}
const Category* findCategory(const std::string& id) {
    static const std::vector<Category> cats = officialCategories();
    for (const Category& c : cats)
        if (c.id == id) return &c;
    return nullptr;
}
std::string categoryOf(int baseSec, int incSec) {
    for (const Category& c : officialCategories())
        if (c.baseSec == baseSec && c.incSec == incSec) return c.id;
    return "custom";
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool contains(const std::string& s, const char* what) { return lower(s).find(what) != std::string::npos; }
bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Events waiting for their delivery time (stable for equal times).
struct Outbox {
    std::deque<std::pair<double, Event>> q;
    void push(double due, Event e) {
        auto it = std::upper_bound(q.begin(), q.end(), due, [](double d, const std::pair<double, Event>& x) { return d < x.first; });
        q.insert(it, {due, std::move(e)});
    }
    bool pop(double now, Event& out) {
        if (q.empty() || q.front().first > now) return false;
        out = std::move(q.front().second);
        q.pop_front();
        return true;
    }
    void clear() { q.clear(); }
};

int pieceValue(chess::PieceType t) {
    static const int v[7] = {0, 1, 3, 3, 5, 9, 100};
    return v[t];
}

double eloExpected(int a, int b) { return 1.0 / (1.0 + std::pow(10.0, double(b - a) / 400.0)); }

// ---- A game with the fake as its authority ------------------------------------------------------
struct Room {
    OnlineGame g;
    chess::Game chess;
    int me = 0;                       // the local player's colour
    int oppRating = 1500;
    double firstDeadline = 0;         // first-move limit (plies 0 and 1)
    double oppMoveAt = -1;            // when the fake opponent plays
    double drawAnswerAt = -1;         // the fake answers our draw offer
    double rematchAnswerAt = -1;      // the fake answers our rematch offer
    double oppRematchAt = -1;         // the fake offers a rematch
    double rematchExpires = 0;
    bool meRematch = false, oppRematch = false;
    double oppAwayUntil = 0, oppGraceEnd = 0;
    bool oppAway = false;
    bool over = false;
    int myOffers = 0, lastDeclinePly = -100;
    m::Rng* rng = nullptr;
    std::function<void(Event, double)> emit;  // event, delay (ms)
    std::function<void(Room&)> onEnd;        // ratings, bookkeeping
    std::function<void(Room&)> onRematch;    // both players want a rematch

    // The fake's next move, chosen when its turn begins, and the way its hand goes there.
    struct Plan {
        uint16_t move = 0;
        double touchAt = -1, hesitateAt = -1, aimAt = -1, promoAt = -1;  // oppMoveAt = on the board
        int hesitateSq = Gesture::kNoSquare;  // aimed at first, sometimes
        double pressMs = 0;                   // autoPress off: from the board to the clock press
    } plan;
    double pressAt = -1;              // autoPress off: the move is on the board, pressed then
    // Its live gestures (net/gesture.h), from a random stream of their own so that they never
    // change the game's.
    m::Rng looks;
    Gesture sent;                     // the last one emitted
    bool sentAny = false;
    int idlePly = 0;                  // plies played when its hand became empty
    double nextLookAt = 0, nextFocusAt = 0, clockLookUntil = 0, glanceUntil = 0;
    int focus = 27;                   // the square its eyes rest on
    float headYaw = 0.0f, headPitch = -0.6f, headLean = 0.0f;

    int opp() const { return 1 - me; }
    int toMove() const { return int(chess.position().sideToMove()); }
    int64_t& ms(int c) { return c == 0 ? g.whiteMs : g.blackMs; }

    Event gameEvent(Event::Kind k) const {
        Event e;
        e.kind = k;
        e.ok = true;
        e.game = g;
        e.gameId = g.id;
        return e;
    }
    void sendSnapshot(double delay = kOneWay) { emit(gameEvent(Event::Kind::GameSnapshot), delay); }
    void event(int kind, int color, uint32_t arg, double delay = kOneWay) {
        Event e = gameEvent(Event::Kind::GameEvent);
        e.gameEventKind = kind;
        e.color = color;
        e.arg = arg;
        emit(e, delay);
    }
    void error(int code, double delay = kOneWay) {
        Event e;
        e.kind = Event::Kind::ServerError;
        e.code = code;
        e.gameId = g.id;
        emit(e, delay);
    }
    void reject(int ply, uint16_t move, int code) {
        Event e = gameEvent(Event::Kind::MoveRejected);
        e.ply = ply;
        e.move = move;
        e.code = code;
        emit(e, kOneWay);
    }

    void start(double now) {
        g.moves.clear();
        g.running = 2;
        g.whiteMs = g.blackMs = g.baseMs;
        g.serverTimeMs = now;
        g.drawOfferBy = 2;
        g.status = Ongoing;
        g.reason = RNone;
        g.whiteConnected = g.blackConnected = true;
        g.graceMs = uint32_t(std::clamp<int64_t>(g.baseMs / 10, 15000, 60000));
        g.firstMoveMs = uint32_t(kFirstMoveMs);
        g.rematchBy = 2;
        firstDeadline = now + kFirstMoveMs;
        chess.reset();
        looks.seedWith(g.id);
        sendSnapshot();
        schedule(now);
    }

    void schedule(double now) {
        oppMoveAt = pressAt = -1;
        plan = Plan();
        if (over || toMove() != opp()) return;
        int ply = int(g.moves.size());
        double t;
        if (ply < 2) {
            t = rng->range(900.0f, 2400.0f);
        } else {
            double left = double(ms(opp()));
            double est = left / 38.0 + double(g.incMs) * 0.7;
            t = est * rng->range(0.25f, 1.45f);
            if (ply < 12) t *= 0.45;
            t = std::clamp(t, 450.0, 14000.0);
            t = std::min(t, std::max(150.0, left * 0.5));
        }
        oppMoveAt = now + t;
        planMove(now, t);
    }

    // The move of the fake's turn and its gestures: the piece touched 0.4-1.3 s before the move
    // reaches the board, in 35% of the moves aimed at another square first, aimed at its own
    // 250-400 ms before (the promotion picker just before a promotion); autoPress off: the clock
    // is pressed 0.6-1.0 s after the move (never so late that the flag falls for it).
    void planMove(double now, double t) {
        plan.move = chooseMove();
        if (!plan.move) return;
        const int from = moveFrom(plan.move), to = moveTo(plan.move);
        const double touchLead = std::min(double(looks.range(400.0f, 1300.0f)), std::max(0.0, t - 100.0));
        const double aimLead = std::min(double(looks.range(250.0f, 400.0f)), touchLead * 0.5);
        plan.touchAt = oppMoveAt - touchLead;
        plan.aimAt = oppMoveAt - aimLead;
        if (movePromo(plan.move) != 0) plan.promoAt = oppMoveAt - aimLead * 0.4;
        if (touchLead - aimLead >= 250.0 && looks.uniform() < 0.35f) {
            std::vector<int> others;
            for (const chess::Move& mv : chess.position().legalMoves())
                if (int(mv.from) == from && int(mv.to) != to) others.push_back(int(mv.to));
            if (!others.empty()) {
                plan.hesitateSq = others[size_t(looks.rangeInt(0, int(others.size()) - 1))];
                plan.hesitateAt = plan.touchAt + (plan.aimAt - plan.touchAt) * double(looks.range(0.15f, 0.45f));
            }
        }
        plan.pressMs = looks.range(600.0f, 1000.0f);
        if (g.moves.size() >= 2) {
            const double left = double(ms(opp())) - std::max(0.0, now - g.serverTimeMs) - t;
            plan.pressMs = std::min(plan.pressMs, std::max(0.0, left * 0.5));
        }
    }

    // The planned move reaches the board (autoPress) or the clock is pressed (autoPress off).
    void oppMoves(double now) {
        const bool pressed = pressAt >= 0;
        pressAt = -1;
        play(opp(), int(g.moves.size()), plan.move, 0, false, now);
        idlePly = int(g.moves.size());
        clockLookUntil = pressed ? now + double(looks.range(450.0f, 750.0f)) : now;
        glanceUntil = clockLookUntil + double(looks.range(1800.0f, 2200.0f));
        nextLookAt = now;
    }

    // What the fake has in hand at 'now' (touch, aim, placed, Promoting) and the ply of that state.
    Gesture hand(double now) const {
        Gesture h;
        h.ply = idlePly;
        if (toMove() != opp() || plan.move == 0 || now < plan.touchAt) return h;
        h.ply = int(g.moves.size());
        h.touch = moveFrom(plan.move);
        if (pressAt >= 0) {
            h.aim = moveTo(plan.move);
            h.placed = plan.move;
            return h;
        }
        if (now >= plan.aimAt) h.aim = moveTo(plan.move);
        else if (plan.hesitateAt >= 0 && now >= plan.hesitateAt) h.aim = plan.hesitateSq;
        if (plan.promoAt >= 0 && now >= plan.promoAt) h.flags = proto::GestureFlag::Promoting;
        return h;
    }

    // A point of the table as the fake sees it from its seat: every player sees the table as
    // White does (seated at +Z, facing -Z), with its clock on its right (+X).
    m::vec3 seatSquare(int sq) const {
        m::vec3 p = layout::squareCenter(sq);
        return opp() == 0 ? p : m::vec3(-p.x, p.y, -p.z);
    }
    static void lookAt(const m::vec3& p, float& yaw, float& pitch) {
        const float dx = p.x, dy = p.y - layout::EYE_HEIGHT, dz = p.z - layout::PLAYER_PELVIS_Z;
        yaw = std::atan2(-dx, -dz);   // > 0 to the left
        pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    }
    // A square its eyes rest on for a while: a piece that can move, or where it could go (its own
    // while it thinks, the local player's while it waits), now and then its planned move.
    int pickFocus() {
        if (plan.move && looks.uniform() < 0.25f) return looks.uniform() < 0.5f ? moveFrom(plan.move) : moveTo(plan.move);
        const std::vector<chess::Move> legal = chess.position().legalMoves();
        if (legal.empty()) return focus;
        const chess::Move& mv = legal[size_t(looks.rangeInt(0, int(legal.size()) - 1))];
        return looks.uniform() < 0.6f ? int(mv.from) : int(mv.to);
    }

    // The fake's gesture: at once when its hand changes, otherwise at 4-6 Hz for its head, which
    // turns towards the piece in hand (then where it is aimed), its clock after pressing it, its
    // scoresheet after a move, or wanders over the board; it leans in while it thinks.
    void gestures(double now) {
        if (oppAway) return;
        Gesture h = hand(now);
        const bool changed = !sentAny || h.ply != sent.ply || h.touch != sent.touch || h.aim != sent.aim || h.placed != sent.placed ||
                             (h.flags & proto::GestureFlag::Promoting) != (sent.flags & proto::GestureFlag::Promoting);
        if (!changed && now < nextLookAt) return;
        const bool thinking = toMove() == opp();
        m::vec3 target;
        float leanTo = 0.2f;
        if (h.touch != Gesture::kNoSquare) {
            target = seatSquare(h.aim != Gesture::kNoSquare ? h.aim : h.touch);
            leanTo = 0.6f;
        } else if (now < clockLookUntil) {
            target = m::vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT, layout::CLOCK_Z);
            h.flags |= proto::GestureFlag::Side;
        } else if (now < glanceUntil) {
            target = m::vec3(-layout::SCORESHEET_X, layout::TABLE_TOP_Y, layout::SCORESHEET_Z);
            h.flags |= proto::GestureFlag::Glance | proto::GestureFlag::Side;
        } else {
            if (now >= nextFocusAt) {
                focus = pickFocus();
                nextFocusAt = now + double(looks.range(700.0f, 1600.0f));
            }
            target = seatSquare(focus);
            leanTo = thinking ? 0.6f : 0.25f;
        }
        float yaw, pitch;
        lookAt(target, yaw, pitch);
        headYaw += (yaw - headYaw) * 0.6f + looks.range(-0.015f, 0.015f);
        headPitch += (pitch - headPitch) * 0.6f + looks.range(-0.01f, 0.01f);
        headLean += (leanTo - headLean) * 0.12f;
        h.yaw = headYaw;
        h.pitch = headPitch;
        h.lean = headLean;
        Event e;
        e.kind = Event::Kind::OpponentGesture;
        e.ok = true;
        e.gameId = g.id;
        e.gesture = h;
        emit(e, kOneWay);
        sent = h;
        sentAny = true;
        nextLookAt = now + double(looks.range(167.0f, 250.0f));
    }

    uint16_t chooseMove() {
        const chess::Position& pos = chess.position();
        std::vector<chess::Move> legal = pos.legalMoves();
        if (legal.empty()) return 0;
        for (const chess::Move& mv : legal) {
            chess::Position p = pos;
            p.makeMove(mv);
            if (p.isCheckmate()) return packMove(mv.from, mv.to, mv.promotion);
        }
        if (rng->uniform() < 0.6f) {
            int best = 0;
            const chess::Move* pick = nullptr;
            for (const chess::Move& mv : legal) {
                chess::Piece victim = pos.at(mv.to);
                if (victim.empty()) continue;
                int gain = pieceValue(victim.type) - pieceValue(pos.at(mv.from).type);
                if (gain >= 0 && pieceValue(victim.type) > best) {
                    best = pieceValue(victim.type);
                    pick = &mv;
                }
            }
            if (pick) return packMove(pick->from, pick->to, pick->promotion);
        }
        chess::Move mv = legal[size_t(rng->rangeInt(0, int(legal.size()) - 1))];
        if (mv.promotion != chess::NoPiece && mv.promotion != chess::Queen) mv.promotion = chess::Queen;
        return packMove(mv.from, mv.to, mv.promotion);
    }

    // A move of 'color' received at 'now'. Returns false (and answers) when it is refused.
    bool play(int color, int ply, uint16_t move, uint32_t posHash, bool drawOffer, double now) {
        bool mine = color == me;
        int cur = int(g.moves.size());
        if (over) {
            if (mine) error(ErrGameOver);
            return false;
        }
        if (ply < cur) {
            if (mine && g.moves[size_t(ply)].move == move) {
                Event e = gameEvent(Event::Kind::MoveMade);  // idempotent: the original again
                e.ply = ply;
                e.move = move;
                e.mine = true;
                emit(e, kOneWay);
            } else if (mine) {
                reject(ply, move, ErrStalePly);
            }
            return false;
        }
        if (posHash != 0 && posHash != digest(chess.position().fen())) {
            reject(ply, move, ErrDesync);
            sendSnapshot(kOneWay + 1.0);
            return false;
        }
        if (ply > cur || toMove() != color) {
            reject(ply, move, ply > cur ? ErrDesync : ErrNotYourTurn);
            sendSnapshot(kOneWay + 1.0);
            return false;
        }
        chess::Move mv = chess.position().findLegal(chess::Square(moveFrom(move)), chess::Square(moveTo(move)),
                                                    chess::PieceType(movePromo(move)));
        if (!mv.valid()) {
            reject(ply, move, ErrIllegalMove);
            sendSnapshot(kOneWay + 1.0);
            return false;
        }
        uint32_t spent = 0;
        if (ply >= 2) {
            double charged = std::max(0.0, now - g.serverTimeMs);
            double left = double(ms(color)) - charged;
            if (left <= 0.0) {
                if (mine) reject(ply, move, ErrFlagFell);
                flag(color, now);
                return false;
            }
            spent = uint32_t(charged);
            ms(color) = int64_t(left) + g.incMs;
        }
        chess.play(mv);
        uint8_t flags = uint8_t(mv.flags & 63);
        if (chess.position().inCheck()) flags |= FCheck;
        if (chess.position().isCheckmate()) flags |= FMate;
        OnlineGame::MoveRec rec;
        rec.move = move;
        rec.spentMs = spent;
        rec.clockMs = uint32_t(std::max<int64_t>(0, ms(color)));
        g.moves.push_back(rec);
        int next = cur + 1;
        g.serverTimeMs = now;
        g.running = next >= 2 ? toMove() : 2;
        g.firstMoveMs = next < 2 ? uint32_t(kFirstMoveMs) : 0u;
        if (next < 2) firstDeadline = now + kFirstMoveMs;
        bool declined = g.drawOfferBy == 1 - color;
        g.drawOfferBy = drawOffer ? color : 2;
        if (drawOffer && !mine) drawAnswerAt = -1;
        Event e = gameEvent(Event::Kind::MoveMade);
        e.ply = cur;
        e.move = move;
        e.flags = flags;
        e.spentMs = spent;
        e.mine = mine;
        emit(e, kOneWay);
        if (declined) {
            if (!mine) lastDeclinePly = cur;
            event(DrawDeclined, color, 0);
        }
        if (drawOffer && mine) drawAnswerAt = now + 1500.0;
        if (chess.isOver()) {
            finish(int(chess.status()), int(chess.endReason()), now);
            return true;
        }
        schedule(now);
        // Now and then the fake offers a draw with its move in a long game.
        if (!mine && cur > 60 && g.drawOfferBy == 2 && rng->uniform() < 0.04f) {
            g.drawOfferBy = color;
            event(DrawOffered, color, 0, kOneWay + 1.0);
        }
        return true;
    }

    void flag(int color, double now) {
        chess.flagFall(chess::Color(color));
        finish(int(chess.status()), int(chess.endReason()), now);
    }

    void finish(int status, int reason, double now) {
        if (over) return;
        if (g.running != 2) ms(g.running) = std::max<int64_t>(0, ms(g.running) - int64_t(now - g.serverTimeMs));
        g.serverTimeMs = now;
        g.running = 2;
        g.drawOfferBy = 2;
        g.status = status;
        g.reason = reason;
        g.firstMoveMs = 0;
        over = true;
        oppMoveAt = drawAnswerAt = -1;
        rematchExpires = now + 60000.0;
        emit(gameEvent(Event::Kind::GameEnd), kOneWay);
        if (onEnd) onEnd(*this);
        // The fake sometimes wants a rematch (after a decisive game or a draw).
        if (status != Aborted && rng->uniform() < 0.5f) oppRematchAt = now + rng->range(2500.0f, 5000.0f);
    }

    void tick(double now) {
        if (!over) {
            if (g.moves.size() < 2 && now >= firstDeadline) {
                finish(Aborted, RNoShow, now);
                return;
            }
            if (g.running != 2 && double(ms(g.running)) - (now - g.serverTimeMs) <= 0.0) {
                flag(g.running, now);
                return;
            }
            if (oppAway && now >= oppGraceEnd) {
                if (g.moves.size() < 2) {
                    finish(Aborted, RNoShow, now);
                } else {
                    bool canMate = chess.position().canColorMate(chess::Color(me));
                    finish(canMate ? (me == 0 ? WhiteWins : BlackWins) : Draw, canMate ? RAbandonment : RAbandonmentVsInsufficient, now);
                }
                return;
            }
            if (oppAway && now >= oppAwayUntil) {
                oppAway = false;
                (opp() == 0 ? g.whiteConnected : g.blackConnected) = true;
                event(PlayerReconnected, opp(), 0);
                schedule(now);
            }
            if (!oppAway && oppMoveAt >= 0 && now >= oppMoveAt && toMove() == opp()) {
                oppMoveAt = -1;
                if (g.autoPress) {
                    oppMoves(now);
                    if (over) return;
                } else {
                    pressAt = now + plan.pressMs;
                }
            }
            if (!oppAway && pressAt >= 0 && now >= pressAt && toMove() == opp()) {
                oppMoves(now);
                if (over) return;
            }
            if (drawAnswerAt >= 0 && now >= drawAnswerAt) {
                drawAnswerAt = -1;
                if (g.drawOfferBy == me) {
                    const chess::Position& p = chess.position();
                    int bal = 0;
                    for (int s = 0; s < 64; ++s) {
                        chess::Piece pc = p.at(chess::Square(s));
                        if (!pc.empty() && pc.type != chess::King) bal += (pc.color == chess::White ? 1 : -1) * pieceValue(pc.type);
                    }
                    bool accept = (g.moves.size() > 50 && std::abs(bal) <= 1) || rng->uniform() < 0.15f;
                    if (accept) {
                        finish(Draw, RAgreement, now);
                        return;
                    }
                    g.drawOfferBy = 2;
                    lastDeclinePly = int(g.moves.size());
                    event(DrawDeclined, opp(), 0);
                }
            }
            gestures(now);
            return;
        }
        if (now > rematchExpires) {
            if (meRematch || oppRematch) event(RematchDeclined, 2, 0);
            meRematch = oppRematch = false;
            rematchAnswerAt = oppRematchAt = -1;
            rematchExpires = 1e300;
            return;
        }
        if (oppRematchAt >= 0 && now >= oppRematchAt) {
            oppRematchAt = -1;
            oppRematch = true;
            g.rematchBy = opp();
            if (meRematch) {
                if (onRematch) onRematch(*this);
                return;
            }
            event(RematchOffered, opp(), 0);
        }
        if (rematchAnswerAt >= 0 && now >= rematchAnswerAt) {
            rematchAnswerAt = -1;
            if (meRematch && onRematch) onRematch(*this);
        }
    }

    // ---- commands of the local player ----
    void resign(double now) {
        if (over) return error(ErrGameOver);
        chess.resign(chess::Color(me));
        finish(int(chess.status()), RResignation, now);
    }
    void offerDraw(double now) {
        if (over) return error(ErrGameOver);
        if (g.drawOfferBy == me || myOffers >= 3 || int(g.moves.size()) - lastDeclinePly < 10) return error(ErrDrawOfferLimit);
        if (g.drawOfferBy == opp()) return finish(Draw, RAgreement, now);  // crossing offers
        ++myOffers;
        g.drawOfferBy = me;
        event(DrawOffered, me, 0);
        drawAnswerAt = now + rng->range(1200.0f, 3000.0f);
    }
    void answerDraw(bool accept, double now) {
        if (over) return error(ErrGameOver);
        if (g.drawOfferBy != opp()) return error(ErrNoPendingOffer);
        if (accept) return finish(Draw, RAgreement, now);
        g.drawOfferBy = 2;
        event(DrawDeclined, me, 0);
    }
    void claimDraw(double now) {
        if (over) return error(ErrGameOver);
        if (chess.canClaimThreefold() || chess.canClaimFiftyMove()) {
            bool three = chess.canClaimThreefold();
            chess.claimDraw();
            finish(Draw, three ? RThreefoldClaim : RFiftyClaim, now);
        } else {
            error(ErrNothingToClaim);
        }
    }
    void abort(double now) {
        if (over) return error(ErrGameOver);
        bool myFirstMade = int(g.moves.size()) > me;
        if (myFirstMade) return error(ErrAbortNotAllowed);
        finish(Aborted, RAborted, now);
    }
    void rematch(bool accept, double now) {
        if (!over || now > rematchExpires) return error(ErrRematchUnavailable);
        if (!accept) {
            meRematch = false;
            event(RematchDeclined, me, 0);
            return;
        }
        meRematch = true;
        g.rematchBy = me;
        if (oppRematch) {
            if (onRematch) onRematch(*this);
            return;
        }
        event(RematchOffered, me, 0);
        rematchAnswerAt = now + rng->range(1200.0f, 2600.0f);
    }
    void opponentLeaves(int seconds, double now) {
        if (over || oppAway) return;
        oppAway = true;
        oppAwayUntil = now + seconds * 1000.0;
        oppGraceEnd = now + g.graceMs;
        (opp() == 0 ? g.whiteConnected : g.blackConnected) = false;
        event(PlayerDisconnected, opp(), g.graceMs);
    }
};

PlayerInfo player(uint32_t id, const std::string& name, int rating, bool provisional) {
    PlayerInfo p;
    p.userId = id;
    p.name = name;
    p.rating = rating;
    p.provisional = provisional;
    return p;
}

std::string groups(const std::string& s, size_t n, char sep) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && i % n == 0) out += sep;
        out += s[i];
    }
    return out;
}

}  // namespace

void useVirtualClock(bool on) { g_virtual = on; }
bool virtualClock() { return g_virtual; }
void advance(double ms) {
    if (g_virtual && ms > 0.0) g_virtualMs += ms;
}
double nowMs() { return g_virtual ? g_virtualMs : wallMs(); }

uint32_t digest(const std::string& fen) {
    // The first four fields: placement, side, castling, en passant.
    size_t end = 0;
    for (int spaces = 0; end < fen.size(); ++end)
        if (fen[end] == ' ' && ++spaces == 4) break;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < end; ++i) {
        h ^= static_cast<unsigned char>(fen[i]);
        h *= 16777619u;
    }
    return h;
}

void opponentDrop(int seconds) { g_opponentDrop = std::max(1, seconds); }
void connectionDrop(int seconds) { g_connectionDrop = std::max(1, seconds); }
void useManualClock(bool on) { g_manualClock = on; }
bool manualClock() { return g_manualClock; }

// =============================================================================================
// FakeServer
// =============================================================================================

struct FakeServer::Impl {
    ServerEndpoint ep;
    Outbox out;
    m::Rng rng{20260928};
    double lastNow = 0;
    // accounts
    std::string sessionUser;          // saved session of this origin ("" = none)
    AccountInfo account;
    bool signedIn = false;
    std::string mfaUser;              // waiting for loginMfa()
    bool ssoActive = false, ssoKnown = false;
    double ssoAt = -1;
    bool mfaEnabled = false;
    // realtime
    ConnState conn = ConnState::Offline;
    bool wantOnline = false;
    double connectAt = -1, dropUntil = 0;
    double pingPhase = 0;
    // matchmaking
    bool queued = false, queueRated = false;
    std::string queueCategory;
    double queueSince = 0, queueNext = 0, queueMatchAt = 0;
    // challenges
    struct Outgoing {
        bool active = false;
        uint32_t id = 0;
        std::string target, code;
        int baseSec = 0, incSec = 0, color = 0;
        bool rated = false;
        double acceptAt = 0;
    } outgoing;
    struct Incoming {
        bool active = false;
        uint32_t id = 0;
        PlayerInfo from;
        int baseSec = 0, incSec = 0, color = 0;
        bool rated = false;
        double expires = 0;
    } incoming;
    double idleSince = 0;
    bool demoChallengeSent = false;
    // game
    std::unique_ptr<Room> room;
    OnlineGame delivered;             // the client's view (last game event delivered)
    bool hasDelivered = false;
    uint32_t nextId = 100;
    uint32_t seq = 0;

    bool online() const { return conn == ConnState::Online; }
    bool hostHas(const char* w) const { return contains(ep.host, w); }

    void http(Event e) {
        out.push(lastNow + double(rng.range(float(kHttpMin), float(kHttpMax))), std::move(e));
    }
    Event result(Event::Kind k, bool ok, const std::string& err = "") {
        Event e;
        e.kind = k;
        e.ok = ok;
        e.error = err;
        return e;
    }
    // Transport failures of the configured host.
    bool transportError(Event::Kind k) {
        const char* err = hostHas("offline") ? "network" : hostHas("badcert") ? "certificate" : nullptr;
        if (!err) return false;
        http(result(k, false, err));
        return true;
    }
    void rt(Event e, double delay = kOneWay) {
        if (!online()) return;  // lost while disconnected: a snapshot follows the reconnection
        out.push(lastNow + delay, std::move(e));
    }

    AccountInfo makeAccount(const std::string& name) {
        AccountInfo a;
        a.userId = 4242;
        a.username = name;
        a.email = lower(name) + "@example.com";
        a.emailVerified = true;
        a.mfaEnabled = mfaEnabled;
        uint32_t h = 2166136261u;
        for (char c : name) h = (h ^ uint8_t(c)) * 16777619u;
        for (const Category& c : officialCategories()) {
            RatingInfo r;
            r.category = c.id;
            uint32_t k = m::hash32(h ^ uint32_t(c.baseSec * 7 + c.incSec));
            int played = (c.id == "3+2" || c.id == "5+0" || c.id == "10+5") ? int(k % 90) + 4 : int(k % 3 == 0 ? k % 12 : 0);
            r.games = played;
            r.rating = played ? 1380 + int(k % 420) : 1500;
            r.peak = r.rating + int(k % 60);
            r.provisional = played < 30;
            r.wins = played * 45 / 100;
            r.draws = played * 12 / 100;
            r.losses = played - r.wins - r.draws;
            a.ratings.push_back(r);
        }
        return a;
    }
    RatingInfo* rating(const std::string& cat) {
        for (RatingInfo& r : account.ratings)
            if (r.category == cat) return &r;
        return nullptr;
    }
    void signIn(const std::string& name) {
        signedIn = true;
        sessionUser = name;
        account = makeAccount(name);
        Event e = result(Event::Kind::LoginResult, true);
        e.account = account;
        http(e);
    }

    // ---- games ----
    void startGame(int baseSec, int incSec, bool rated, int colorPref, const std::string& oppName, int oppRating, bool oppProv) {
        leaveQueueSilently();
        std::string cat = categoryOf(baseSec, incSec);
        if (cat == "custom") rated = false;
        int me = colorPref == 1 ? 0 : colorPref == 2 ? 1 : rng.rangeInt(0, 1);
        const RatingInfo* mine = rating(cat);
        PlayerInfo you = player(account.userId, account.username, mine ? mine->rating : 1500, mine ? mine->provisional : true);
        PlayerInfo them = player(9000 + rng.rangeInt(0, 999), oppName, oppRating, oppProv);
        room.reset(new Room());
        Room& r = *room;
        r.rng = &rng;
        r.me = me;
        r.oppRating = oppRating;
        r.emit = [this](Event e, double delay) { rt(std::move(e), delay); };
        r.onEnd = [this](Room& rm) { gameEnded(rm); };
        r.onRematch = [this](Room& rm) { startRematch(rm); };
        r.g.id = uint64_t(std::max(0.0, lastNow - kIdEpochMs)) * 4096u + 3u * 64u + (seq++ & 63u);
        r.g.category = cat;
        r.g.baseMs = int64_t(baseSec) * 1000;
        r.g.incMs = int64_t(incSec) * 1000;
        r.g.rated = rated;
        r.g.white = me == 0 ? you : them;
        r.g.black = me == 0 ? them : you;
        r.g.you = me;
        r.g.autoPress = !g_manualClock;
        r.start(lastNow);
        LOGI("mock server: game %llu, %s %s, you play %s against %s", (unsigned long long)r.g.id, cat.c_str(), rated ? "rated" : "casual",
             me == 0 ? "White" : "Black", oppName.c_str());
    }
    void startRematch(Room& old) {
        int base = int(old.g.baseMs / 1000), inc = int(old.g.incMs / 1000);
        const PlayerInfo& o = old.me == 0 ? old.g.black : old.g.white;
        std::string name = o.name;
        int rating = o.rating;
        bool prov = o.provisional, rated = old.g.rated;
        int color = old.me == 0 ? 2 : 1;  // colours swapped
        startGame(base, inc, rated, color, name, rating, prov);
    }
    void gameEnded(Room& r) {
        idleSince = lastNow;
        if (!r.g.rated || r.g.status == Aborted || r.g.status == Ongoing) return;
        RatingInfo* mine = rating(r.g.category);
        if (!mine) return;
        double score = r.g.status == Draw ? 0.5 : ((r.g.status == WhiteWins) == (r.me == 0) ? 1.0 : 0.0);
        int kMe = mine->games < 30 ? 40 : 20, kOpp = 20;
        int before = mine->rating, oppBefore = r.oppRating;
        int after = before + int(std::lround(kMe * (score - eloExpected(before, oppBefore))));
        int oppAfter = oppBefore + int(std::lround(kOpp * ((1.0 - score) - eloExpected(oppBefore, before))));
        mine->rating = after;
        mine->games += 1;
        mine->peak = std::max(mine->peak, after);
        mine->provisional = mine->games < 30;
        (score == 1.0 ? mine->wins : score == 0.0 ? mine->losses : mine->draws) += 1;
        Event e;
        e.kind = Event::Kind::RatingUpdate;
        e.gameId = r.g.id;
        e.game = r.g;
        Event::Rating& rm = r.me == 0 ? e.ratingWhite : e.ratingBlack;
        Event::Rating& ro = r.me == 0 ? e.ratingBlack : e.ratingWhite;
        rm.before = before;
        rm.after = after;
        rm.games = mine->games;
        rm.provisional = mine->provisional;
        ro.before = oppBefore;
        ro.after = oppAfter;
        ro.games = 57;
        rt(e, 700.0);
    }
    void leaveQueueSilently() { queued = false; }
    bool busyInGame() const { return room && !room->over; }

    void queueStatus(int state) {
        Event e;
        e.kind = Event::Kind::QueueStatus;
        e.queueCategory = queueCategory;
        e.queueRated = queueRated;
        e.queueState = state;
        e.queueWaitMs = uint32_t(std::max(0.0, lastNow - queueSince));
        e.queueWindow = uint32_t(std::min(400.0, 75.0 + 25.0 * (lastNow - queueSince) / 1000.0));
        e.queued = uint32_t(3 + rng.rangeInt(0, 9));
        rt(e);
    }
    void challengeStatus(int state) {
        Event e;
        e.kind = Event::Kind::ChallengeStatus;
        e.challengeId = outgoing.id;
        e.challengeState = state;
        e.challengeTarget = outgoing.target;
        e.challengeCode = outgoing.code;
        e.challengeBaseSec = outgoing.baseSec;
        e.challengeIncSec = outgoing.incSec;
        e.challengeRated = outgoing.rated;
        rt(e);
    }
    void serverError(int code) {
        Event e;
        e.kind = Event::Kind::ServerError;
        e.code = code;
        rt(e);
    }

    void setConn(ConnState s, const std::string& err = "") {
        conn = s;
        Event e;
        e.kind = Event::Kind::ConnectionChanged;
        e.state = s;
        e.error = err;
        out.push(lastNow, e);
    }
    void welcome() {
        Event w;
        w.kind = Event::Kind::Welcome;
        w.ok = true;
        w.account = account;
        w.serverName = "Scacelith (mock server)";
        rt(w, 1.0);
        if (room && !room->over) room->sendSnapshot(2.0);
        idleSince = lastNow;
    }

    void tick(double now) {
        lastNow = now;
        pingPhase += 1.0;
        if (ssoActive && ssoAt >= 0 && now >= ssoAt) {
            ssoAt = -1;
            if (ssoKnown) {
                ssoActive = false;
                signIn("Guillaume_G");
            } else {
                http(result(Event::Kind::SsoNeedsUsername, true));
            }
        }
        if (g_connectionDrop > 0 && online()) {
            dropUntil = now + g_connectionDrop * 1000.0;
            g_connectionDrop = 0;
            setConn(ConnState::Reconnecting, "network");
        }
        if (wantOnline && conn == ConnState::Reconnecting && now >= dropUntil) {
            setConn(ConnState::Online);
            welcome();
            if (room && room->over && hasDelivered && delivered.status == Ongoing) room->sendSnapshot(2.0);
        }
        if (conn == ConnState::Connecting && connectAt >= 0 && now >= connectAt) {
            connectAt = -1;
            if (hostHas("old")) {
                setConn(ConnState::Incompatible, "incompatible");
                wantOnline = false;
            } else {
                setConn(ConnState::Online);
                welcome();
            }
        }
        if (room) {
            if (g_opponentDrop > 0) {
                room->opponentLeaves(g_opponentDrop, now);
                g_opponentDrop = 0;
            }
            room->tick(now);
        }
        if (!online()) return;
        if (queued) {
            if (now >= queueMatchAt) {
                queued = false;
                queueStatus(2);
                const RatingInfo* mine = rating(queueCategory);
                int base = mine ? mine->rating : 1500;
                const Category* c = findCategory(queueCategory);
                startGame(c ? c->baseSec : 300, c ? c->incSec : 0, queueRated, 0, kOpponents[rng.rangeInt(0, 9)],
                          base + rng.rangeInt(-120, 120), rng.uniform() < 0.3f);
            } else if (now >= queueNext) {
                queueNext = now + 1000.0;
                queueStatus(1);
            }
        }
        if (outgoing.active && outgoing.acceptAt > 0 && now >= outgoing.acceptAt) {
            outgoing.active = false;
            challengeStatus(1);
            std::string who = outgoing.target.empty() ? std::string("a_friend") : outgoing.target;
            startGame(outgoing.baseSec, outgoing.incSec, outgoing.rated, outgoing.color, who, 1450 + rng.rangeInt(0, 300), false);
        }
        if (incoming.active && now >= incoming.expires) {
            incoming.active = false;
            Event e;
            e.kind = Event::Kind::ChallengeStatus;
            e.challengeId = incoming.id;
            e.challengeState = 4;  // Expired
            rt(e);
        }
        // Once per session, a player challenges you while you idle in the menus.
        bool idle = !queued && !outgoing.active && !incoming.active && !busyInGame();
        if (!idle) idleSince = now;
        if (idle && !demoChallengeSent && now - idleSince > 25000.0) {
            demoChallengeSent = true;
            incoming.active = true;
            incoming.id = ++nextId;
            incoming.from = player(7310, "Eleonora_V", 1612, false);
            incoming.baseSec = 300;
            incoming.incSec = 3;
            incoming.rated = true;
            incoming.color = rng.rangeInt(1, 2);
            incoming.expires = now + 30000.0;
            Event e;
            e.kind = Event::Kind::ChallengeReceived;
            e.challengeId = incoming.id;
            e.challenger = incoming.from;
            e.challengeBaseSec = incoming.baseSec;
            e.challengeIncSec = incoming.incSec;
            e.challengeRated = incoming.rated;
            e.challengeColor = incoming.color;
            e.challengeExpiresMs = 30000;
            rt(e);
        }
    }
};

FakeServer::FakeServer() : impl_(new Impl()) {
    impl_->lastNow = nowMs();
    if (!g_virtual) impl_->rng.seedWith(uint64_t(impl_->lastNow));
}
FakeServer::~FakeServer() = default;

void FakeServer::setServer(const ServerEndpoint& ep) {
    Impl& I = *impl_;
    if (ep.origin() != I.ep.origin()) {
        if (I.conn != ConnState::Offline) disconnect();
        I.signedIn = false;
        I.sessionUser.clear();  // each origin keeps its own session: the fake starts signed out
    }
    I.ep = ep;
}
const ServerEndpoint& FakeServer::server() const { return impl_->ep; }

void FakeServer::fetchServerInfo() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.transportError(Event::Kind::ServerInfoResult)) return;
    Event e = I.result(Event::Kind::ServerInfoResult, true);
    ServerInfo& s = e.info;
    s.name = contains(I.ep.host, "official") ? "Scacelith" : "Scacelith (mock server)";
    s.serverId = "mock-" + lower(I.ep.host);
    s.motd = "A local stand-in for the real server: every password works, and your opponents play at random.";
    s.protocolMin = proto::kProtocolMin;
    s.protocolMax = I.hostHas("old") ? proto::kProtocolMin - 1 : proto::kProtocolVersion;
    s.compatible = !I.hostHas("old");
    s.wsPort = I.ep.wsPort ? I.ep.wsPort : I.ep.apiPort;
    s.registrationOpen = true;
    s.emailVerification = true;
    s.googleSso = true;
    s.categories = officialCategories();
    I.http(e);
}
bool FakeServer::hasSavedSession() const { return !impl_->sessionUser.empty(); }
std::string FakeServer::savedUsername() const { return impl_->sessionUser; }

void FakeServer::registerAccount(const std::string& username, const std::string& email, const std::string& password) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.transportError(Event::Kind::RegisterResult)) return;
    bool nameOk = username.size() >= 3 && username.size() <= 24 &&
                  std::all_of(username.begin(), username.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'; });
    std::string err = !nameOk ? "invalid_username"
                      : contains(username, "taken") ? "username_taken"
                      : email.find('@') == std::string::npos ? "invalid_email"
                      : password.size() < 10 ? "weak_password"
                                             : "";
    I.http(I.result(Event::Kind::RegisterResult, err.empty(), err));
}

void FakeServer::login(const std::string& user, const std::string& password) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.transportError(Event::Kind::LoginResult)) return;
    std::string name = user.substr(0, user.find('@'));
    if (name.empty()) name = "player";
    if (password == "wrong") return I.http(I.result(Event::Kind::LoginResult, false, "invalid_credentials"));
    if (contains(name, "ratelimited")) {
        Event e = I.result(Event::Kind::LoginResult, false, "rate_limited");
        e.retryAfterSec = 45;
        return I.http(e);
    }
    if (contains(name, "unverified")) return I.http(I.result(Event::Kind::LoginResult, false, "email_unverified"));
    if (contains(name, "banned")) {
        Event e = I.result(Event::Kind::LoginResult, false, "banned");
        e.account.bannedUntilMs = int64_t(I.lastNow + 3.0 * 86400000.0);
        return I.http(e);
    }
    if (contains(name, "mfa") || I.mfaEnabled) {
        I.mfaUser = name;
        Event e = I.result(Event::Kind::LoginResult, false);
        e.mfaRequired = true;
        return I.http(e);
    }
    I.signIn(name);
}
void FakeServer::loginMfa(const std::string& code) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    std::string c;
    for (char ch : code)
        if (ch != ' ' && ch != '-') c += ch;
    bool ok = !I.mfaUser.empty() && ((c.size() == 6 && allDigits(c)) || c.size() == 10);
    if (!ok) return I.http(I.result(Event::Kind::LoginResult, false, I.mfaUser.empty() ? "expired" : "invalid_code"));
    std::string name = I.mfaUser;
    I.mfaUser.clear();
    I.mfaEnabled = true;
    I.signIn(name);
}
void FakeServer::startGoogleSso() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    I.ssoActive = true;
    I.ssoAt = I.lastNow + 3500.0;
    I.http(I.result(Event::Kind::SsoBrowserOpened, true));
}
void FakeServer::completeSso(const std::string& username) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.ssoActive) return I.http(I.result(Event::Kind::LoginResult, false, "expired"));
    if (contains(username, "taken") || username.size() < 3) {
        return I.http(I.result(Event::Kind::LoginResult, false, username.size() < 3 ? "invalid_username" : "username_taken"));
    }
    I.ssoActive = false;
    I.ssoKnown = true;
    I.signIn(username);
    I.account.googleLinked = true;
}
void FakeServer::cancelSso() {
    impl_->ssoActive = false;
    impl_->ssoAt = -1;
}
void FakeServer::logout(bool) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    disconnect();
    I.signedIn = false;
    I.sessionUser.clear();
    I.http(I.result(Event::Kind::LogoutResult, true));
}
void FakeServer::fetchAccount() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.signedIn) return I.http(I.result(Event::Kind::AccountResult, false, "unauthorized"));
    I.account.mfaEnabled = I.mfaEnabled;
    Event e = I.result(Event::Kind::AccountResult, true);
    e.account = I.account;
    I.http(e);
}
void FakeServer::resendVerification(const std::string&) { impl_->lastNow = nowMs(); impl_->http(impl_->result(Event::Kind::VerificationResent, true)); }
void FakeServer::forgotPassword(const std::string&) { impl_->lastNow = nowMs(); impl_->http(impl_->result(Event::Kind::PasswordResetRequested, true)); }
void FakeServer::changePassword(const std::string& current, const std::string& next) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    std::string err = current == "wrong" ? "invalid_credentials" : next.size() < 10 ? "weak_password" : "";
    I.http(I.result(Event::Kind::PasswordChanged, err.empty(), err));
}
void FakeServer::mfaSetup(const std::string& password) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (password == "wrong") return I.http(I.result(Event::Kind::MfaSetupResult, false, "invalid_credentials"));
    static const char* b32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string secret;
    for (int i = 0; i < 32; ++i) secret += b32[I.rng.rangeInt(0, 31)];
    Event e = I.result(Event::Kind::MfaSetupResult, true);
    e.mfaSecret = secret;
    e.mfaUri = "otpauth://totp/Scacelith:" + I.account.username + "?secret=" + secret + "&issuer=Scacelith&digits=6&period=30";
    I.http(e);
}
void FakeServer::mfaEnable(const std::string& code) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (code.size() != 6 || !allDigits(code)) return I.http(I.result(Event::Kind::MfaEnableResult, false, "invalid_code"));
    I.mfaEnabled = true;
    I.account.mfaEnabled = true;
    Event e = I.result(Event::Kind::MfaEnableResult, true);
    static const char* alpha = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 10; ++i) {
        std::string c;
        for (int k = 0; k < 10; ++k) c += alpha[I.rng.rangeInt(0, 30)];
        e.recoveryCodes.push_back(c.substr(0, 4) + "-" + c.substr(4, 4) + "-" + c.substr(8, 2));
    }
    I.http(e);
}
void FakeServer::mfaDisable(const std::string& password, const std::string& code) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    std::string err = password == "wrong" ? "invalid_credentials" : code.size() < 6 ? "invalid_code" : "";
    if (err.empty()) {
        I.mfaEnabled = false;
        I.account.mfaEnabled = false;
    }
    I.http(I.result(Event::Kind::MfaDisableResult, err.empty(), err));
}
void FakeServer::regenerateRecoveryCodes(const std::string& password, const std::string& code) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    std::string err = password == "wrong" ? "invalid_credentials" : code.size() != 6 ? "invalid_code" : "";
    Event e = I.result(Event::Kind::RecoveryCodesResult, err.empty(), err);
    if (err.empty()) {
        static const char* alpha = "abcdefghjkmnpqrstuvwxyz23456789";
        for (int i = 0; i < 10; ++i) {
            std::string c;
            for (int k = 0; k < 10; ++k) c += alpha[I.rng.rangeInt(0, 30)];
            e.recoveryCodes.push_back(c.substr(0, 4) + "-" + c.substr(4, 4) + "-" + c.substr(8, 2));
        }
    }
    I.http(e);
}
void FakeServer::report(uint64_t, const std::string&, const std::string&, const std::string&) {
    impl_->lastNow = nowMs();
    impl_->http(impl_->result(Event::Kind::ReportResult, true));
}

// ---- account API: CONTRACT STUBS (replaced by the UI work package with realistic fakes) ----
namespace {
Event notImplemented(Event::Kind kind) {
    Event ev;
    ev.kind = kind;
    ev.error = "not_implemented";
    return ev;
}
}  // namespace
void FakeServer::fetchMyGames(uint64_t, int, const GamesFilter&) { impl_->http(notImplemented(Event::Kind::GamesResult)); }
void FakeServer::fetchGame(uint64_t) { impl_->http(notImplemented(Event::Kind::GameDetailsResult)); }
void FakeServer::downloadPgn(uint64_t) { impl_->http(notImplemented(Event::Kind::PgnResult)); }
void FakeServer::fetchSessions() { impl_->http(notImplemented(Event::Kind::SessionsResult)); }
void FakeServer::revokeSession(int64_t) { impl_->http(notImplemented(Event::Kind::SessionRevoked)); }
void FakeServer::setAcceptChallenges(bool) { impl_->http(notImplemented(Event::Kind::PreferencesResult)); }
void FakeServer::changeEmail(const std::string&, const std::string&, const std::string&) {
    impl_->http(notImplemented(Event::Kind::EmailChangeResult));
}
void FakeServer::exportAccount(const std::string&, const std::string&) { impl_->http(notImplemented(Event::Kind::AccountExportResult)); }
void FakeServer::deleteAccount(const std::string&, const std::string&) { impl_->http(notImplemented(Event::Kind::AccountDeleted)); }

void FakeServer::connect() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.signedIn || I.conn == ConnState::Online || I.conn == ConnState::Connecting) return;
    I.wantOnline = true;
    if (I.hostHas("offline")) {
        I.setConn(ConnState::Reconnecting, "network");
        I.dropUntil = 1e300;
        return;
    }
    I.setConn(ConnState::Connecting);
    I.connectAt = I.lastNow + 350.0;
}
void FakeServer::disconnect() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    I.wantOnline = false;
    I.queued = false;
    I.outgoing.active = false;
    if (I.room && !I.room->over) I.room->resign(I.lastNow);
    if (I.conn != ConnState::Offline) I.setConn(ConnState::Offline);
}
ConnState FakeServer::state() const { return impl_->conn; }
int FakeServer::pingMs() const {
    if (impl_->conn != ConnState::Online) return -1;
    return 33 + int(std::lround(2.5 * std::sin(impl_->pingPhase * 0.013)));
}
double FakeServer::serverNowMs() const { return nowMs(); }

void FakeServer::joinQueue(const std::string& category, bool rated) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.online()) return;
    if (I.busyInGame()) return I.serverError(ErrAlreadyInGame);
    if (!findCategory(category)) return I.serverError(ErrInvalidCategory);
    I.queued = true;
    I.queueCategory = category;
    I.queueRated = rated;
    I.queueSince = I.lastNow;
    I.queueNext = I.lastNow + 1000.0;
    I.queueMatchAt = I.lastNow + 2000.0 + I.rng.range(0.0f, 600.0f);
    I.queueStatus(1);
}
void FakeServer::leaveQueue() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.queued) return;
    I.queued = false;
    I.queueStatus(0);
}
void FakeServer::challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.online()) return;
    if (lower(username) == lower(I.account.username)) return I.serverError(ErrCannotChallengeSelf);
    if (baseSec < 15 || baseSec > 10800 || incSec < 0 || incSec > 180) return I.serverError(ErrInvalidTimeControl);
    if (rated && categoryOf(baseSec, incSec) == "custom") return I.serverError(ErrRatedRequiresOfficialTc);
    I.outgoing = Impl::Outgoing();
    I.outgoing.active = true;
    I.outgoing.id = ++I.nextId;
    I.outgoing.target = username;
    I.outgoing.baseSec = baseSec;
    I.outgoing.incSec = incSec;
    I.outgoing.rated = rated;
    I.outgoing.color = colorPref;
    bool away = contains(username, "away") || contains(username, "nobody");
    I.outgoing.acceptAt = away ? 0.0 : I.lastNow + 2500.0;
    I.challengeStatus(away ? 5 : 0);
    if (away) I.outgoing.active = false;
}
void FakeServer::createPrivateGame(int baseSec, int incSec, bool rated, int colorPref) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.online()) return;
    if (rated && categoryOf(baseSec, incSec) == "custom") return I.serverError(ErrRatedRequiresOfficialTc);
    static const char* alpha = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    std::string code;
    for (int i = 0; i < 6; ++i) code += alpha[I.rng.rangeInt(0, 30)];
    I.outgoing = Impl::Outgoing();
    I.outgoing.active = true;
    I.outgoing.id = ++I.nextId;
    I.outgoing.code = code;
    I.outgoing.baseSec = baseSec;
    I.outgoing.incSec = incSec;
    I.outgoing.rated = rated;
    I.outgoing.color = colorPref;
    I.outgoing.acceptAt = I.lastNow + 6000.0;
    I.challengeStatus(0);
}
void FakeServer::joinPrivateGame(const std::string& code) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.online()) return;
    std::string c;
    for (char ch : code)
        if (ch != '-' && ch != ' ') c += char(std::toupper(static_cast<unsigned char>(ch)));
    if (c.size() < 4 || c.size() > 12) return I.serverError(ErrCodeInvalid);
    I.outgoing = Impl::Outgoing();
    I.outgoing.active = true;
    I.outgoing.id = ++I.nextId;
    I.outgoing.target = "Camille_D";
    I.outgoing.baseSec = 600;
    I.outgoing.incSec = 5;
    I.outgoing.acceptAt = I.lastNow + 700.0;
}
void FakeServer::acceptChallenge(uint32_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.incoming.active || I.incoming.id != id) return I.serverError(201);
    I.incoming.active = false;
    int color = I.incoming.color == 1 ? 1 : 2;
    I.startGame(I.incoming.baseSec, I.incoming.incSec, I.incoming.rated, color, I.incoming.from.name, I.incoming.from.rating,
                I.incoming.from.provisional);
}
void FakeServer::declineChallenge(uint32_t id) {
    Impl& I = *impl_;
    if (I.incoming.active && I.incoming.id == id) I.incoming.active = false;
}
void FakeServer::cancelChallenge(uint32_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.outgoing.active || I.outgoing.id != id) return;
    I.outgoing.active = false;
    I.challengeStatus(3);
}

void FakeServer::sendMove(uint64_t gameId, int ply, uint16_t move, const std::string& fen, uint32_t, bool drawOffer) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (!I.online()) return;  // lost: the reconnection's snapshot tells the truth
    if (!I.room || I.room->g.id != gameId) return I.serverError(ErrNotInGame);
    // The intent reaches the server one latency later.
    I.room->play(I.room->me, ply, move, digest(fen), drawOffer, I.lastNow + kOneWay);
}
void FakeServer::resign(uint64_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->resign(I.lastNow);
}
void FakeServer::offerDraw(uint64_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->offerDraw(I.lastNow);
}
void FakeServer::answerDraw(uint64_t id, bool accept) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->answerDraw(accept, I.lastNow);
}
void FakeServer::claimDraw(uint64_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->claimDraw(I.lastNow);
}
void FakeServer::abortGame(uint64_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->abort(I.lastNow);
}
void FakeServer::requestResync(uint64_t id) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->sendSnapshot();
}
void FakeServer::rematch(uint64_t id, bool accept) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && I.room->g.id == id) I.room->rematch(accept, I.lastNow);
}
void FakeServer::sendGesture(uint64_t, const Gesture&) {}   // the fake opponent does not watch
const OnlineGame* FakeServer::currentGame() const { return impl_->hasDelivered ? &impl_->delivered : nullptr; }

bool FakeServer::poll(Event& out) {
    Impl& I = *impl_;
    I.tick(nowMs());
    if (!I.out.pop(I.lastNow, out)) return false;
    switch (out.kind) {
    case Event::Kind::GameSnapshot:
    case Event::Kind::MoveMade:
    case Event::Kind::MoveRejected:
    case Event::Kind::GameEvent:
    case Event::Kind::GameEnd:
        I.delivered = out.game;
        I.hasDelivered = true;
        break;
    default: break;
    }
    return true;
}

// =============================================================================================
// FakeDirect
// =============================================================================================

struct FakeDirect::Impl {
    Outbox out;
    m::Rng rng{47100};
    double lastNow = 0;
    DirectMatch::State state = DirectMatch::State::Idle;
    std::string error;
    DirectInvite invite;
    UpnpStatus upnp;
    bool host = false;
    DirectHostOptions opt;
    std::string myName, friendName;
    double nextStepAt = -1;
    std::unique_ptr<Room> room;
    OnlineGame delivered;
    bool hasDelivered = false;
    uint32_t seq = 0;

    void push(Event e, double delay) { out.push(lastNow + delay, std::move(e)); }
    void fail(const std::string& err) {
        state = DirectMatch::State::Failed;
        error = err;
        nextStepAt = -1;
        Event e;
        e.kind = Event::Kind::ConnectionChanged;
        e.state = ConnState::Offline;
        e.error = err;
        push(e, 0.0);
    }
    void startGame(int baseSec, int incSec, int hostColor, bool swap) {
        int me;
        if (swap && room) {
            me = 1 - room->me;
        } else {
            int hc = hostColor == 1 ? 0 : hostColor == 2 ? 1 : rng.rangeInt(0, 1);
            me = host ? hc : 1 - hc;
        }
        room.reset(new Room());
        Room& r = *room;
        r.rng = &rng;
        r.me = me;
        r.emit = [this](Event e, double delay) { push(std::move(e), delay); };
        r.onRematch = [this](Room& rm) {
            int b = int(rm.g.baseMs / 1000), i = int(rm.g.incMs / 1000);
            startGame(b, i, 0, true);
        };
        r.g.id = uint64_t(std::max(0.0, lastNow - kIdEpochMs)) * 4096u + (seq++ & 63u);
        r.g.category = "custom";
        r.g.baseMs = int64_t(baseSec) * 1000;
        r.g.incMs = int64_t(incSec) * 1000;
        r.g.rated = false;
        PlayerInfo you = player(1, myName.empty() ? std::string("Player") : myName, 0, false);
        PlayerInfo them = player(2, friendName, 0, false);
        r.g.white = me == 0 ? you : them;
        r.g.black = me == 0 ? them : you;
        r.g.you = me;
        r.g.autoPress = !g_manualClock && (!host || opt.autoPress);   // the host's choice
        r.start(lastNow);
    }
    void tick(double now) {
        lastNow = now;
        if (room) {
            if (g_opponentDrop > 0) {
                room->opponentLeaves(g_opponentDrop, now);
                g_opponentDrop = 0;
            }
            room->tick(now);
        }
        if (nextStepAt < 0 || now < nextStepAt) return;
        nextStepAt = -1;
        switch (state) {
        case DirectMatch::State::OpeningPort:
            if (opt.upnp) {
                if (opt.port == 47199) {
                    upnp.state = UpnpStatus::State::NoGateway;
                } else {
                    upnp.state = UpnpStatus::State::Mapped;
                    upnp.gatewayName = "Livebox 6";
                    upnp.externalIp = opt.port == 47198 ? "100.72.14.9" : "203.0.113.47";
                    upnp.externalPort = opt.port;
                    upnp.cgnatSuspected = opt.port == 47198;
                }
            }
            invite.port = opt.port ? opt.port : 47100;
            // Empty when carrier-grade NAT is suspected (as the real DirectMatch does).
            invite.publicAddress = upnp.state == UpnpStatus::State::Mapped && !upnp.cgnatSuspected ? upnp.externalIp : std::string();
            invite.lanAddresses = {"192.168.1.23", "fd12:3456:789a::1c"};
            {
                static const char* alpha = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
                std::string c;
                for (int i = 0; i < 12; ++i) c += alpha[rng.rangeInt(0, 30)];
                invite.code = groups(c, 4, '-');
            }
            state = DirectMatch::State::WaitingForGuest;
            nextStepAt = now + 3000.0;
            break;
        case DirectMatch::State::WaitingForGuest:
        case DirectMatch::State::Connecting:
            state = DirectMatch::State::Handshake;
            nextStepAt = now + 400.0;
            break;
        case DirectMatch::State::Handshake: {
            state = DirectMatch::State::Playing;
            Event e;
            e.kind = Event::Kind::ConnectionChanged;
            e.state = ConnState::Online;
            push(e, 0.0);
            startGame(host ? opt.baseSec : 600, host ? opt.incSec : 5, host ? opt.hostColor : 0, false);
            break;
        }
        default: break;
        }
    }
};

FakeDirect::FakeDirect() : impl_(new Impl()) { impl_->lastNow = nowMs(); }
FakeDirect::~FakeDirect() = default;

void FakeDirect::host(const DirectHostOptions& opt) {
    Impl& I = *impl_;
    close();
    I.lastNow = nowMs();
    I.host = true;
    I.opt = opt;
    I.myName = opt.playerName;
    I.friendName = "Aurelien";
    I.error.clear();
    I.invite = DirectInvite();
    I.upnp = UpnpStatus();
    I.upnp.state = opt.upnp ? UpnpStatus::State::Searching : UpnpStatus::State::NotTried;
    I.state = DirectMatch::State::OpeningPort;
    I.nextStepAt = I.lastNow + 1000.0;
}
void FakeDirect::join(const std::string& address, uint16_t port, const std::string& code, const std::string& playerName) {
    Impl& I = *impl_;
    close();
    I.lastNow = nowMs();
    I.host = false;
    I.myName = playerName;
    I.friendName = "Camille";
    I.error.clear();
    std::string c;
    for (char ch : code)
        if (ch != '-' && ch != ' ') c += ch;
    I.state = DirectMatch::State::Connecting;
    I.nextStepAt = I.lastNow + 1000.0;
    if (address.empty() || port == 0) return I.fail("bad_address");
    if (c.size() != 12) return I.fail("invalid_code");
    if (contains(address, "refused")) return I.fail("refused");
    if (contains(address, "timeout")) return I.fail("timeout");
    if (contains(address, "unknown")) return I.fail("not_found");
    if (contains(address, "old")) return I.fail("incompatible");
    if (c.compare(0, 4, "2222") == 0) return I.fail("wrong_code");
}
void FakeDirect::close() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room && !I.room->over) I.room->resign(I.lastNow);
    I.room.reset();
    I.out.clear();
    I.hasDelivered = false;
    I.state = DirectMatch::State::Idle;
    I.nextStepAt = -1;
}
DirectMatch::State FakeDirect::state() const { return impl_->state; }
std::string FakeDirect::lastError() const { return impl_->error; }
DirectInvite FakeDirect::invite() const { return impl_->invite; }
UpnpStatus FakeDirect::upnp() const { return impl_->upnp; }
bool FakeDirect::isHost() const { return impl_->host; }
void FakeDirect::sendMove(int ply, uint16_t move, const std::string& fen, uint32_t, bool drawOffer) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    if (I.room) I.room->play(I.room->me, ply, move, digest(fen), drawOffer, I.lastNow + 6.0);
}
void FakeDirect::resign() { if (impl_->room) impl_->room->resign(nowMs()); }
void FakeDirect::offerDraw() { if (impl_->room) impl_->room->offerDraw(nowMs()); }
void FakeDirect::answerDraw(bool accept) { if (impl_->room) impl_->room->answerDraw(accept, nowMs()); }
void FakeDirect::claimDraw() { if (impl_->room) impl_->room->claimDraw(nowMs()); }
void FakeDirect::abortGame() { if (impl_->room) impl_->room->abort(nowMs()); }
void FakeDirect::requestResync() { if (impl_->room) impl_->room->sendSnapshot(); }
void FakeDirect::rematch(bool accept) { if (impl_->room) impl_->room->rematch(accept, nowMs()); }
void FakeDirect::sendGesture(const Gesture&) {}   // the fake friend does not watch
const OnlineGame* FakeDirect::currentGame() const { return impl_->hasDelivered ? &impl_->delivered : nullptr; }
int FakeDirect::pingMs() const { return impl_->state == DirectMatch::State::Playing ? 12 : -1; }
double FakeDirect::serverNowMs() const { return nowMs(); }
bool FakeDirect::poll(Event& out) {
    Impl& I = *impl_;
    I.tick(nowMs());
    if (!I.out.pop(I.lastNow, out)) return false;
    switch (out.kind) {
    case Event::Kind::GameSnapshot:
    case Event::Kind::MoveMade:
    case Event::Kind::MoveRejected:
    case Event::Kind::GameEvent:
    case Event::Kind::GameEnd:
        I.delivered = out.game;
        I.hasDelivered = true;
        break;
    default: break;
    }
    return true;
}

}  // namespace mock
}  // namespace net
