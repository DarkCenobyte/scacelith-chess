// Fake server and fake direct-match peer (see online_mock.h).
#include "online_mock.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include "../math/math.h"
#include "../net/json.h"
#include "../net/protocol_gen.h"
#include "layout.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
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

// ---- The account API: a generated game history --------------------------------------------------
// The history of an account is made once from its name (the same games every time): ~45 games
// over the last weeks, newest first, of every kind the server stores: official categories rated
// or casual, custom time controls (casual), both colours, opponents with plausible ratings (one now
// and then a deleted account, "deleted#<id>"), and every way a game ends (checkmate, resignation,
// flag fall, agreement, threefold claim, stalemate, abandonment, forfeit, aborted, first move not
// played). The moves are legal (played by the rules of src/chess), the clocks consistent with the
// time control (no clock before each side's first move, increments, the loser of a flag fall at
// zero), and the ratings of rated games lead to the account's current ones.

const char* const kHistoryNames[] = {"Wilhelmina", "orlov_b", "Kasparilla", "MarieCurieux", "Tal_Returns", "quietbishop",
                                     "Nimzo_fan", "Aurelien_P", "sveta.k", "RookLift", "Eleonora_V", "hikaru_not",
                                     "pawnstorm77", "Lucia-M", "the_grob", "Bjorn.S", "caro_kann_carla", "FianchettoFox",
                                     "Ines_R", "zugzwang_z"};

// Lines that end by themselves, for the endings random play rarely reaches.
const char* const kMateLines[] = {
    "e2e4 e7e5 f1c4 b8c6 d1h5 g8f6 h5f7",                                              // White mates
    "f2f3 e7e5 g2g4 d8h4",                                                             // Black mates
    "e2e4 e7e5 g1f3 d7d6 f1c4 c8g4 b1c3 g7g6 f3e5 g4d1 c4f7 e8e7 c3d5",                // White mates
    "e2e4 e7e5 g1f3 b8c6 f1c4 c6d4 f3e5 d8g5 e5f7 g5g2 h1f1 g2e4 c4e2 d4f3",           // Black mates
    "d2d4 e7e5 d4e5 d7d6 e5d6 f8d6 g1f3 d8e7 c1g5 d6f2 e1d2 e7e3",                     // Black mates
};
const char* const kStalemateLine = "e2e3 a7a5 d1h5 a8a6 h5a5 h7h5 h2h4 a6h6 a5c7 f7f6 c7d7 e8f7 d7b7 d8d3 b7b8 d3h7 b8c8 f7g6 c8e6";
const char* const kRepetitionLines[] = {
    "e2e4 e7e5 g1f3 b8c6 f3g1 c6b8 g1f3 b8c6 f3g1 c6b8",
    "d2d4 d7d5 g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8",
    "c2c4 e7e5 b1c3 g8f6 c3b1 f6g8 b1c3 g8f6 c3b1 f6g8",
};

// The server's end reason texts (dedicated-server/src/chess/game.js REASON_TEXT), the comment after
// the last move of its PGN.
const char* serverReasonText(int reason) {
    switch (reason) {
    case 1: return "Checkmate";
    case 2: return "Resignation";
    case 3: return "Loss on time";
    case 4: return "Second illegal move (forfeit)";
    case 5: return "Stalemate";
    case 6: return "Dead position (insufficient material)";
    case 7: return "Flag fall, but the opponent cannot checkmate";
    case 8: return "Fivefold repetition";
    case 9: return "75-move rule";
    case 10: return "Threefold repetition (claimed)";
    case 11: return "50-move rule (claimed)";
    case 12: return "Draw by agreement";
    case 13: return "Second illegal move, but the opponent cannot checkmate";
    case 20: return "Abandoned (disconnected for too long)";
    case 21: return "Abandoned, but the opponent cannot checkmate";
    case 22: return "Game aborted";
    case 23: return "Aborted: first move not played in time";
    case 24: return "Forfeit (fair play violation)";
    case 25: return "Aborted by the server";
    case 26: return "Aborted: both players disconnected";
    default: return "";
    }
}

// The PGN Termination tag of the server (S2 of the account API work: aborted games "unterminated").
const char* serverTermination(int status, int reason) {
    if (status == Aborted || status == Ongoing) return "unterminated";
    switch (reason) {
    case 3:
    case 7: return "time forfeit";
    case 4:
    case 13:
    case 24: return "rules infraction";
    case 20:
    case 21:
    case 26: return "abandoned";
    default: return "normal";
    }
}

std::tm utcOf(int64_t ms) {
    std::time_t t = std::time_t(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

// "h:mm:ss.f", tenths truncated (the server's pgnClock).
std::string pgnClock(int64_t ms) {
    const int64_t t = std::max<int64_t>(0, ms) / 100;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld.%lld", (long long)(t / 36000), (long long)(t / 600 % 60), (long long)(t / 10 % 60),
                  (long long)(t % 10));
    return buf;
}

std::string ratingDiffText(int d) { return (d >= 0 ? "+" : "") + std::to_string(d); }

// The server's PGN of a stored game (GET /games/:id/pgn, dedicated-server/src/http/routes/players.js
// gamePgn): its tags in its order, a {[%clk] [%emt]} comment after each move, the end reason as a
// comment, movetext wrapped under 80 columns.
std::string serverPgn(const GameDetails& g, const std::string& serverName, const std::string& site) {
    std::string out;
    auto tag = [&](const char* name, const std::string& value) {
        std::string v;
        for (char c : value) {
            if (c == '"' || c == '\\') v += '\\';
            v += c;
        }
        out += std::string("[") + name + " \"" + v + "\"]\n";
    };
    const std::tm start = utcOf(g.startedAtMs);
    char date[48], time[48];
    std::snprintf(date, sizeof date, "%04d.%02d.%02d", start.tm_year + 1900, start.tm_mon + 1, start.tm_mday);
    std::snprintf(time, sizeof time, "%02d:%02d:%02d", start.tm_hour, start.tm_min, start.tm_sec);
    tag("Event", serverName + (g.rated ? " rated " : " casual ") + g.category);
    tag("Site", site);
    tag("Date", date);
    tag("Round", "-");
    tag("White", g.white.name);
    tag("Black", g.black.name);
    tag("Result", g.result);
    tag("UTCDate", date);
    tag("UTCTime", time);
    tag("WhiteElo", g.white.rating > 0 ? std::to_string(g.white.rating) : "-");
    tag("BlackElo", g.black.rating > 0 ? std::to_string(g.black.rating) : "-");
    if (g.white.ratingChanged || g.black.ratingChanged) {
        tag("WhiteRatingDiff", ratingDiffText(g.white.ratingDiff));
        tag("BlackRatingDiff", ratingDiffText(g.black.ratingDiff));
    }
    tag("TimeControl", std::to_string(g.baseMs / 1000) + "+" + std::to_string(g.incMs / 1000));
    tag("Termination", serverTermination(g.status, g.reason));
    tag("PlyCount", std::to_string(g.moves.size()));
    tag("ScacelithGameId", std::to_string(g.id));
    out += "\n";

    std::string line;
    auto emit = [&](const std::string& tok) {
        if (!line.empty() && line.size() + 1 + tok.size() > 79) {
            out += line + "\n";
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += tok;
    };
    chess::Position pos;
    for (size_t i = 0; i < g.moves.size(); ++i) {
        const GameDetails::Ply& p = g.moves[i];
        chess::Move mv = pos.findLegal(chess::Square(moveFrom(p.move)), chess::Square(moveTo(p.move)), chess::PieceType(movePromo(p.move)));
        if (!mv.valid()) break;
        emit(std::to_string(i / 2 + 1) + (i % 2 ? "..." : "."));
        emit(pos.toSAN(mv));
        pos.makeMove(mv);
        std::vector<std::string> words;
        if (p.clockMs >= 0) words.push_back("[%clk " + pgnClock(p.clockMs) + "]");
        if (p.spentMs >= 0) words.push_back("[%emt " + pgnClock(p.spentMs) + "]");
        for (size_t w = 0; w < words.size(); ++w)
            emit((w == 0 ? "{" : "") + words[w] + (w + 1 == words.size() ? "}" : ""));
    }
    const std::string text = serverReasonText(g.reason);
    if (g.status != Ongoing && !text.empty()) {
        std::string word;
        const std::string comment = "{" + text + "}";
        for (size_t i = 0; i <= comment.size(); ++i) {
            if (i == comment.size() || comment[i] == ' ') {
                if (!word.empty()) emit(word);
                word.clear();
            } else {
                word += comment[i];
            }
        }
    }
    emit(g.result);
    out += line + "\n";
    return out;
}

const char* resultOf(int status) {
    return status == WhiteWins ? "1-0" : status == BlackWins ? "0-1" : status == Draw ? "1/2-1/2" : "*";
}

// Plays the UCI moves of 'line' on 'game'; false (and the game as far as it went) when one is not legal.
bool playLine(chess::Game& game, const char* line) {
    std::string tok;
    for (const char* c = line;; ++c) {
        if (*c == ' ' || *c == 0) {
            if (!tok.empty()) {
                chess::Move mv = game.position().parseUCI(tok);
                if (!mv.valid() || !game.play(mv)) return false;
            }
            tok.clear();
            if (!*c) return true;
        } else {
            tok += *c;
        }
    }
}

// A move of the history's players: a mate in one when there is one, else the best of a quick
// look (material won or lost on the square, development, castling and the centre in the opening,
// checks) with some noise, so that the games look like club games rather than random moves.
chess::Move historyMove(const chess::Position& pos, m::Rng& rng, int ply) {
    std::vector<chess::Move> legal = pos.legalMoves();
    if (legal.empty()) return chess::Move();
    const chess::Color us = pos.sideToMove(), them = chess::Color(1 - int(us));
    const bool opening = ply < 20;
    float bestScore = -1e9f;
    chess::Move best = legal[0];
    for (const chess::Move& mv : legal) {
        chess::Position p = pos;
        p.makeMove(mv);
        if (p.isCheckmate()) return mv;
        const chess::Piece mover = pos.at(mv.from);
        float score = 0.0f;
        const chess::Piece victim = pos.at(mv.to);
        if (!victim.empty()) score += 10.0f * float(pieceValue(victim.type));
        if (mv.flags & chess::MoveEnPassant) score += 10.0f;
        if (mv.promotion != chess::NoPiece) score += mv.promotion == chess::Queen ? 80.0f : -20.0f;
        // The piece left where the opponent takes it.
        const int value = pieceValue(mv.promotion != chess::NoPiece ? mv.promotion : mover.type);
        if (mover.type != chess::King && p.isAttacked(mv.to, them)) {
            const bool defended = p.isAttacked(mv.to, us);
            score -= defended ? 10.0f * float(std::max(0, value - 3)) : 10.0f * float(value);
        }
        if (p.inCheck()) score += 3.0f;
        if (mv.flags & (chess::MoveCastleKing | chess::MoveCastleQueen)) score += 14.0f;
        const int file = int(mv.to) & 7, rank = int(mv.to) >> 3, fromRank = int(mv.from) >> 3;
        const bool backRank = fromRank == (us == chess::White ? 0 : 7);
        if (opening) {
            if (mover.type == chess::Pawn && (file == 3 || file == 4)) score += 6.0f;
            if (mover.type == chess::Pawn && (file == 0 || file == 7)) score -= 4.0f;
            if ((mover.type == chess::Knight || mover.type == chess::Bishop) && backRank) score += 7.0f;
            if (mover.type == chess::Queen) score -= 5.0f;
            if (mover.type == chess::Rook) score -= 4.0f;
            if (mover.type == chess::King && !(mv.flags & (chess::MoveCastleKing | chess::MoveCastleQueen))) score -= 12.0f;
            if ((mover.type == chess::Knight || mover.type == chess::Bishop) && !backRank) score -= 3.0f;  // moved twice
        } else if (mover.type == chess::Pawn) {
            score += 0.6f * float(us == chess::White ? rank : 7 - rank);  // passed pawns walk on
        }
        if (file >= 2 && file <= 5 && rank >= 2 && rank <= 5) score += 1.5f;
        if (mover.type == chess::Knight && (file == 0 || file == 7)) score -= 6.0f;  // "a knight on the rim is dim"
        score += rng.range(0.0f, 9.0f);
        if (score > bestScore) {
            bestScore = score;
            best = mv;
        }
    }
    return best;
}

int materialOf(const chess::Position& pos, int color) {
    int sum = 0;
    for (int sq = 0; sq < 64; ++sq) {
        chess::Piece p = pos.at(chess::Square(sq));
        if (!p.empty() && int(p.color) == color && p.type != chess::King) sum += pieceValue(p.type);
    }
    return sum;
}

enum class Ending { Natural, Mate, Resign, Flag, Agree, Threefold, Stalemate, Abandon, Forfeit, Abort, NoShow };

// One finished game ending 'endedAt' (moves, clocks, players, result; ratings by the caller).
GameDetails makePastGame(m::Rng& rng, Ending ending, int64_t endedAt, int baseSec, int incSec, int me) {
    GameDetails d;
    d.baseMs = int64_t(baseSec) * 1000;
    d.incMs = int64_t(incSec) * 1000;
    d.you = me;
    chess::Game game;
    bool lineEnded = false;
    if (ending == Ending::Mate) {
        const char* line = kMateLines[rng.rangeInt(0, int(sizeof kMateLines / sizeof *kMateLines) - 1)];
        lineEnded = playLine(game, line) && game.isOver();
        if (!lineEnded) game.reset();
    } else if (ending == Ending::Stalemate) {
        lineEnded = playLine(game, kStalemateLine) && game.isOver();
        if (!lineEnded) game.reset();
    } else if (ending == Ending::Threefold) {
        const char* line = kRepetitionLines[rng.rangeInt(0, int(sizeof kRepetitionLines / sizeof *kRepetitionLines) - 1)];
        if (!playLine(game, line) || !game.canClaimThreefold()) game.reset();
    }
    if (game.moves().empty()) {
        int target = ending == Ending::Abort ? rng.rangeInt(0, 1)
                     : ending == Ending::NoShow ? rng.rangeInt(0, 1)
                     : ending == Ending::Mate || ending == Ending::Natural ? 120
                                                                            : rng.rangeInt(24, 96);
        if (ending == Ending::Mate) ending = Ending::Natural;  // random play until it ends by itself (or resigns)
        if (ending == Ending::Threefold) ending = Ending::Agree;
        if (ending == Ending::Stalemate) ending = Ending::Agree;
        while (int(game.moves().size()) < target && !game.isOver()) {
            chess::Move mv = historyMove(game.position(), rng, int(game.moves().size()));
            if (!mv.valid() || !game.play(mv)) break;
        }
        if (ending == Ending::Natural && !game.isOver()) ending = Ending::Resign;
    }
    const int plies = int(game.moves().size());
    const int toMove = int(game.position().sideToMove());
    // The result.
    if (game.isOver()) {
        d.status = game.status() == chess::GameStatus::WhiteWins ? WhiteWins : game.status() == chess::GameStatus::BlackWins ? BlackWins : Draw;
        d.reason = int(game.endReason());
    } else {
        switch (ending) {
        case Ending::Resign: {
            const int mw = materialOf(game.position(), 0), mb = materialOf(game.position(), 1);
            const int loser = mw == mb ? toMove : (mw < mb ? 0 : 1);
            d.status = loser == 0 ? BlackWins : WhiteWins;
            d.reason = RResignation;
            break;
        }
        case Ending::Flag:
            d.status = toMove == 0 ? BlackWins : WhiteWins;
            d.reason = RTimeout;
            if (!game.position().canColorMate(chess::Color(1 - toMove))) {
                d.status = Draw;
                d.reason = 7;  // TimeoutVsInsufficient
            }
            break;
        case Ending::Threefold:
            d.status = Draw;
            d.reason = RThreefoldClaim;
            break;
        case Ending::Abandon:
            d.status = toMove == 0 ? BlackWins : WhiteWins;  // the side to move left
            d.reason = RAbandonment;
            if (!game.position().canColorMate(chess::Color(1 - toMove))) {
                d.status = Draw;
                d.reason = RAbandonmentVsInsufficient;
            }
            break;
        case Ending::Forfeit:
            d.status = me == 0 ? WhiteWins : BlackWins;  // the opponent's fair play violation
            d.reason = 24;
            break;
        case Ending::Abort:
            d.status = Aborted;
            d.reason = RAborted;
            break;
        case Ending::NoShow:
            d.status = Aborted;
            d.reason = RNoShow;
            break;
        default:
            d.status = Draw;
            d.reason = RAgreement;
            break;
        }
    }
    d.result = resultOf(d.status);
    d.plies = plies;

    // Clocks: no clock before each side's first move (plies 0 and 1), then the time spent,
    // increments included in the clock after the move.
    const double base = double(d.baseMs), inc = double(d.incMs);
    const double pace = base / 38.0 + inc * 0.85;  // average thinking time
    std::vector<double> spent(size_t(plies), 0.0);
    for (int i = 2; i < plies; ++i) {
        const double phase = i < 16 ? 0.45 : i < 70 ? 1.2 : 0.8;
        spent[size_t(i)] = std::max(300.0, pace * phase * double(rng.range(0.25f, 1.9f)));
    }
    const bool flagged = d.reason == RTimeout || d.reason == 7;
    const int loser = toMove;  // whose flag fell
    if (flagged) {
        // Scale the loser's thinking so that its clock ends with a few seconds or less.
        double sum = 0;
        int count = 0;
        for (int i = 2; i < plies; ++i)
            if (i % 2 == loser) {
                sum += spent[size_t(i)];
                ++count;
            }
        const double target = base + inc * count - double(rng.range(300.0f, 5000.0f));
        if (sum > 0 && target > 0)
            for (int i = 2; i < plies; ++i)
                if (i % 2 == loser) spent[size_t(i)] *= target / sum;
    }
    double clock[2] = {base, base};
    double total = 0;
    for (int i = 0; i < plies; ++i) {
        const int side = i % 2;
        GameDetails::Ply p;
        const chess::Move& mv = game.moves()[size_t(i)];
        p.move = packMove(mv.from, mv.to, mv.promotion);
        p.uci = game.positionAt(size_t(i)).toUCI(mv);
        if (i >= 2) {
            double s = spent[size_t(i)];
            if (!flagged || side != loser) s = std::min(s, std::max(200.0, clock[side] - 900.0));  // nobody else runs out
            s = std::floor(s / 100.0) * 100.0;
            clock[side] = clock[side] - s + inc;
            p.spentMs = int64_t(s);
            total += s;
        } else {
            p.spentMs = 0;
            total += double(rng.range(1500.0f, 9000.0f));
        }
        p.clockMs = int64_t(std::max(0.0, clock[side]));
        d.moves.push_back(p);
    }
    if (flagged) total += std::max(0.0, clock[loser]);
    if (d.reason == RAbandonment || d.reason == RAbandonmentVsInsufficient) total += 60000.0;
    if (d.reason == RNoShow) total += kFirstMoveMs;
    d.endedAtMs = endedAt;
    d.startedAtMs = endedAt - int64_t(total) - int64_t(plies) * 120 - 800;
    return d;
}

// Sets the ratings and changes of a past game: 'mine' my rating after it (rated) or at the time.
void setRatings(GameDetails& d, int mine, int opp, bool provisional) {
    GameSide& me = d.you == 0 ? d.white : d.black;
    GameSide& them = d.you == 0 ? d.black : d.white;
    if (d.category == "custom") {
        me.rating = them.rating = 0;
        return;
    }
    if (!d.rated || d.status == Aborted) {
        me.rating = mine;
        them.rating = opp;
        return;
    }
    const double score = d.status == Draw ? 0.5 : ((d.status == WhiteWins) == (d.you == 0) ? 1.0 : 0.0);
    const int k = provisional ? 40 : 20;
    const int diff = int(std::lround(k * (score - eloExpected(mine, opp))));
    const int oppDiff = int(std::lround(20 * ((1.0 - score) - eloExpected(opp, mine - diff))));
    me.rating = mine - diff;
    me.ratingAfter = mine;
    me.ratingDiff = diff;
    me.ratingChanged = true;
    them.rating = opp;
    them.ratingAfter = opp + oppDiff;
    them.ratingDiff = oppDiff;
    them.ratingChanged = true;
}

// The history of an account, newest first, the newest ending around 'now'.
std::vector<GameDetails> makeHistory(const AccountInfo& account, double now) {
    std::vector<GameDetails> out;
    if (contains(account.username, "newbie")) return out;
    uint32_t h = 2166136261u;
    for (char c : account.username) h = (h ^ uint8_t(c)) * 16777619u;
    m::Rng rng(uint64_t(h) * 2654435761u + 77u);

    // The endings, in a random order.
    std::vector<Ending> endings;
    auto add = [&](Ending e, int n) { endings.insert(endings.end(), size_t(n), e); };
    add(Ending::Resign, 13);
    add(Ending::Flag, 7);
    add(Ending::Mate, 8);
    add(Ending::Natural, 2);
    add(Ending::Agree, 4);
    add(Ending::Threefold, 2);
    add(Ending::Stalemate, 1);
    add(Ending::Abandon, 2);
    add(Ending::Forfeit, 1);
    add(Ending::Abort, 3);
    add(Ending::NoShow, 2);
    for (size_t i = endings.size(); i > 1; --i) std::swap(endings[i - 1], endings[size_t(rng.rangeInt(0, int(i) - 1))]);

    // My rating in each category after the game being made (going back in time).
    std::vector<std::pair<std::string, int>> cursor;
    for (const RatingInfo& r : account.ratings) cursor.push_back({r.category, r.rating});
    auto ratingOf = [&](const std::string& cat) -> int* {
        for (auto& c : cursor)
            if (c.first == cat) return &c.second;
        return nullptr;
    };
    static const char* const kCategories[] = {"3+2", "3+2", "3+2", "5+0", "5+0", "10+5", "10+5", "1+0", "5+3", "15+10", "10+0", "30+0"};
    static const int kCustom[][2] = {{420, 3}, {240, 2}, {90, 1}, {1200, 15}, {150, 0}};
    int64_t endedAt = int64_t(now) - int64_t(rng.range(20.0f, 90.0f) * 60000.0f);
    uint32_t seq = 0;
    size_t lastDeleted = 0;  // deleted accounts are rare: one in a dozen games at most
    for (size_t i = 0; i < endings.size(); ++i) {
        std::string cat;
        int baseSec = 0, incSec = 0;
        bool rated;
        if (rng.uniform() < 0.12f) {
            const int k = rng.rangeInt(0, 4);
            baseSec = kCustom[k][0];
            incSec = kCustom[k][1];
            cat = "custom";
            rated = false;
        } else {
            cat = kCategories[rng.rangeInt(0, int(sizeof kCategories / sizeof *kCategories) - 1)];
            const Category* c = findCategory(cat);
            baseSec = c ? c->baseSec : 180;
            incSec = c ? c->incSec : 2;
            // Rated only where the account has played rated games.
            const RatingInfo* info = nullptr;
            for (const RatingInfo& r : account.ratings)
                if (r.category == cat) info = &r;
            rated = info && info->games > 0 && rng.uniform() < 0.8f;
        }
        const int me = rng.rangeInt(0, 1);
        GameDetails d = makePastGame(rng, endings[i], endedAt, baseSec, incSec, me);
        d.category = cat;
        d.rated = rated;
        std::string opp = kHistoryNames[rng.rangeInt(0, int(sizeof kHistoryNames / sizeof *kHistoryNames) - 1)];
        if (rng.uniform() < 0.08f && (lastDeleted == 0 || i >= lastDeleted + 12)) {
            opp = "deleted#" + std::to_string(1000 + rng.rangeInt(0, 8999));
            lastDeleted = i + 1;
        }
        (me == 0 ? d.white : d.black).name = account.username;
        (me == 0 ? d.black : d.white).name = opp;
        int* mine = ratingOf(cat);
        const int my = mine ? *mine : 1500;
        const int theirs = std::clamp(my + rng.rangeInt(-160, 160), 800, 2700);
        const RatingInfo* info = nullptr;
        for (const RatingInfo& r : account.ratings)
            if (r.category == cat) info = &r;
        setRatings(d, my, theirs, info && info->provisional);
        if (mine && d.rated && d.status != Aborted) *mine = (me == 0 ? d.white : d.black).rating;  // before this game
        d.id = uint64_t(std::max<int64_t>(0, d.startedAtMs - int64_t(kIdEpochMs))) * 4096u + 3u * 64u + (seq++ & 63u);
        out.push_back(std::move(d));
        // The game before: a short pause or a long one (another day).
        const double gap = rng.uniform() < 0.6f ? double(rng.range(2.0f, 40.0f)) * 60000.0 : double(rng.range(5.0f, 60.0f)) * 3600000.0;
        endedAt = out.back().startedAtMs - int64_t(gap);
    }
    return out;
}

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
    // account API: the game history (made from the account's name, then the games played here),
    // the signed-in devices, the e-mail change waiting for its link, the data exports of the hour
    struct Past {
        GameDetails d;
        bool reported = false;
    };
    std::vector<Past> history;        // newest first
    std::string historyOf;
    bool historyMade = false;
    std::vector<SessionInfo> devices;
    double signedInAt = 0, pendingSince = 0;
    std::vector<double> exportTimes;
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
    // Transport failures of the configured host (e: the other fields of the answer, if any).
    bool transportError(Event::Kind k, Event e = Event()) {
        const char* err = hostHas("offline") ? "network" : hostHas("badcert") ? "certificate" : nullptr;
        if (!err) return false;
        e.kind = k;
        e.ok = false;
        e.error = err;
        http(std::move(e));
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
        a.hasPassword = true;
        a.acceptChallenges = true;
        a.createdAtMs = int64_t(lastNow) - int64_t(212.0 * 86400000.0);
        a.lastLoginAtMs = int64_t(lastNow);
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
        signedInAt = lastNow;
        devices.clear();  // this sign-in is a new session
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
        RatingInfo* mine = r.g.rated && r.g.status != Aborted && r.g.status != Ongoing ? rating(r.g.category) : nullptr;
        if (!mine) return recordGame(r, nullptr, nullptr);
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
        recordGame(r, &e.ratingWhite, &e.ratingBlack);
        rt(e, 700.0);
    }
    void recordGame(const Room& r, const Event::Rating* white, const Event::Rating* black);
    // account API (defined with FakeServer's account API methods)
    bool reauth(Event::Kind k, const std::string& password, const std::string& code);
    void ensureHistory();
    void ensureDevices();
    Past* findPast(uint64_t id);
    bool reportable(const Past& p) const;
    std::string exportDocument();
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
        if (idle && !demoChallengeSent && account.acceptChallenges && now - idleSince > 25000.0) {
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
    if (!I.account.pendingEmail.empty() && I.lastNow - I.pendingSince > 40000.0) {
        I.account.email = I.account.pendingEmail;  // the fake opened the link
        I.account.emailVerified = true;
        I.account.pendingEmail.clear();
    }
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
void FakeServer::report(uint64_t gameId, const std::string&, const std::string&, const std::string&) {
    impl_->lastNow = nowMs();
    if (Impl::Past* p = impl_->findPast(gameId)) p->reported = true;
    impl_->http(impl_->result(Event::Kind::ReportResult, true));
}

// ---- account API (dedicated-server/docs/API.md) ----
// Re-authentication of the account changes (e-mail, export, deletion): the password, and the
// second factor when two-factor is on (6 digits, or a recovery code). Answers the error itself.
bool FakeServer::Impl::reauth(Event::Kind k, const std::string& password, const std::string& code) {
    if (!account.hasPassword) { http(result(k, false, "password_not_set")); return false; }
    if (password == "wrong" || password.empty()) { http(result(k, false, "invalid_password")); return false; }
    if (mfaEnabled) {
        std::string c;
        for (char ch : code)
            if (ch != ' ' && ch != '-') c += ch;
        if (c.empty()) { http(result(k, false, "mfa_code_required")); return false; }
        const bool digits = c.size() == 6 && allDigits(c);
        if ((!digits && c.size() != 10) || c == "000000") { http(result(k, false, "invalid_code")); return false; }
    }
    return true;
}

void FakeServer::Impl::ensureHistory() {
    if (historyOf == account.username && historyMade) return;
    history.clear();
    for (GameDetails& d : makeHistory(account, lastNow)) {
        Past p;
        p.d = std::move(d);
        history.push_back(std::move(p));
    }
    historyOf = account.username;
    historyMade = true;
}

void FakeServer::Impl::ensureDevices() {
    if (!devices.empty()) return;
    const int64_t now = int64_t(lastNow);
#ifdef _WIN32
    const char* here = "Scacelith/0.1.0 win64";
    const char* other = "Scacelith/0.1.0 linux";
#else
    const char* here = "Scacelith/0.1.0 linux";
    const char* other = "Scacelith/0.1.0 win64";
#endif
    struct Seed { const char* label; double createdDays, seenHours; bool current; };
    const Seed seeds[] = {{here, 0.0, 0.0, true}, {other, 18.0, 31.0, false}, {"Scacelith/0.0.9 linux", 46.0, 290.0, false}, {"", 62.0, 1100.0, false}};
    int64_t id = 5120 + int64_t(rng.rangeInt(0, 400));
    for (const Seed& s : seeds) {
        SessionInfo info;
        info.id = id;
        id += 37 + rng.rangeInt(0, 90);
        info.clientLabel = s.label;
        info.current = s.current;
        info.createdAtMs = s.current ? int64_t(signedInAt) : now - int64_t(s.createdDays * 86400000.0);
        info.lastSeenAtMs = s.current ? now : now - int64_t(s.seenHours * 3600000.0);
        info.expiresAtMs = info.lastSeenAtMs + int64_t(30.0 * 86400000.0);
        devices.push_back(info);
    }
}

FakeServer::Impl::Past* FakeServer::Impl::findPast(uint64_t id) {
    for (Past& p : history)
        if (p.d.id == id) return &p;
    return nullptr;
}

bool FakeServer::Impl::reportable(const Past& p) const {
    const GameSide& them = p.d.you == 0 ? p.d.black : p.d.white;
    return !p.reported && p.d.status != Aborted && them.name.compare(0, 8, "deleted#") != 0 &&
           lastNow - double(p.d.endedAtMs) < 7.0 * 86400000.0;
}

// A game played against the fake goes into the history (newest first), like the server's store.
void FakeServer::Impl::recordGame(const Room& r, const Event::Rating* white, const Event::Rating* black) {
    if (r.g.status == Ongoing) return;
    ensureHistory();
    Past p;
    GameDetails& d = p.d;
    d.id = r.g.id;
    d.category = r.g.category;
    d.rated = r.g.rated;
    d.baseMs = r.g.baseMs;
    d.incMs = r.g.incMs;
    d.you = r.me;
    d.status = r.g.status;
    d.reason = r.g.reason;
    d.result = resultOf(r.g.status);
    d.plies = int(r.g.moves.size());
    d.endedAtMs = int64_t(lastNow);
    d.startedAtMs = int64_t(double(r.g.id / 4096u) + kIdEpochMs);
    auto side = [&](GameSide& s, const PlayerInfo& pi, const Event::Rating* change) {
        s.name = pi.name;
        s.rating = d.category == "custom" ? 0 : pi.rating;
        if (change) {
            s.ratingChanged = true;
            s.ratingAfter = change->after;
            s.ratingDiff = change->after - change->before;
        }
    };
    side(d.white, r.g.white, white);
    side(d.black, r.g.black, black);
    chess::Position pos;
    for (const OnlineGame::MoveRec& m : r.g.moves) {
        GameDetails::Ply ply;
        ply.move = m.move;
        chess::Move mv = pos.findLegal(chess::Square(moveFrom(m.move)), chess::Square(moveTo(m.move)), chess::PieceType(movePromo(m.move)));
        if (!mv.valid()) break;
        ply.uci = pos.toUCI(mv);
        ply.spentMs = int64_t(m.spentMs);
        ply.clockMs = int64_t(m.clockMs);
        pos.makeMove(mv);
        d.moves.push_back(ply);
    }
    history.insert(history.begin(), std::move(p));
}

std::string FakeServer::Impl::exportDocument() {
    ensureHistory();
    ensureDevices();
    using json::Value;
    Value doc = Value::object();
    doc.set("format", "scacelith-account-export");
    doc.set("version", 1);
    doc.set("exportedAt", int64_t(lastNow));
    Value server = Value::object();
    server.set("name", contains(ep.host, "official") ? "Scacelith" : "Scacelith (mock server)");
    server.set("host", ep.host);
    doc.set("server", server);
    Value acc = Value::object();
    acc.set("id", account.userId);
    acc.set("username", account.username);
    acc.set("email", account.email);
    acc.set("emailVerified", account.emailVerified);
    acc.set("pendingEmail", account.pendingEmail.empty() ? Value() : Value(account.pendingEmail));
    acc.set("mfaEnabled", mfaEnabled);
    acc.set("googleLinked", account.googleLinked);
    if (account.googleLinked) acc.set("googleEmail", account.email);
    acc.set("hasPassword", account.hasPassword);
    acc.set("acceptChallenges", account.acceptChallenges ? "all" : "none");
    acc.set("createdAt", account.createdAtMs);
    acc.set("lastLoginAt", account.lastLoginAtMs);
    doc.set("account", acc);
    Value ratings = Value::array();
    for (const RatingInfo& r : account.ratings) {
        Value v = Value::object();
        v.set("category", r.category);
        v.set("rating", r.rating);
        v.set("games", r.games);
        v.set("wins", r.wins);
        v.set("draws", r.draws);
        v.set("losses", r.losses);
        v.set("peak", r.peak);
        v.set("provisional", r.provisional);
        v.set("updatedAt", int64_t(lastNow) - int64_t(r.games) * 3600000);
        ratings.push(v);
    }
    doc.set("ratings", ratings);
    doc.set("ratingRefunds", Value::array());
    Value sessions = Value::array();
    for (const SessionInfo& s : devices) {
        Value v = Value::object();
        v.set("id", s.id);
        v.set("createdAt", s.createdAtMs);
        v.set("lastSeenAt", s.lastSeenAtMs);
        v.set("expiresAt", s.expiresAtMs);
        v.set("revokedAt", Value());
        v.set("clientLabel", s.clientLabel.empty() ? Value() : Value(s.clientLabel));
        v.set("ip", s.current ? "203.0.113.24" : "198.51.100.7");
        sessions.push(v);
    }
    doc.set("sessions", sessions);
    Value events = Value::array();
    Value login = Value::object();
    login.set("kind", "login");
    login.set("at", int64_t(signedInAt));
    login.set("ip", "203.0.113.24");
    login.set("detail", Value());
    events.push(login);
    doc.set("securityEvents", events);
    doc.set("sanctions", Value::array());
    doc.set("conduct", Value::array());
    doc.set("reportsFiled", Value::array());
    Value games = Value::object();
    games.set("total", int(history.size()));
    Value list = Value::array();
    for (const Past& p : history) {
        const GameDetails& d = p.d;
        Value g = Value::object();
        g.set("id", d.id);
        g.set("category", d.category);
        g.set("rated", d.rated);
        g.set("timeControl", std::to_string(d.baseMs / 1000) + "+" + std::to_string(d.incMs / 1000));
        auto side = [](const GameSide& s) {
            Value v = Value::object();
            v.set("name", s.name);
            v.set("rating", s.rating > 0 ? Value(s.rating) : Value());
            v.set("ratingAfter", s.ratingChanged ? Value(s.ratingAfter) : Value());
            v.set("ratingDiff", s.ratingChanged ? Value(s.ratingDiff) : Value());
            return v;
        };
        g.set("white", side(d.white));
        g.set("black", side(d.black));
        g.set("color", d.you == 0 ? "white" : "black");
        g.set("status", d.status);
        g.set("reason", d.reason);
        g.set("result", d.result);
        g.set("plies", d.plies);
        g.set("startedAt", d.startedAtMs);
        g.set("endedAt", d.endedAtMs);
        g.set("baseMs", d.baseMs);
        g.set("incMs", d.incMs);
        list.push(g);
    }
    games.set("list", list);
    doc.set("games", games);
    Value notes = Value::array();
    notes.push("Your password, two-factor secret, recovery codes and session tokens are never exported.");
    notes.push("Reports made against you and the fair play analysis of your games are not included.");
    doc.set("notes", notes);
    return doc.dump();
}

void FakeServer::fetchMyGames(uint64_t before, int limit, const GamesFilter& filter) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::GamesResult;
    // Every answer names its request (the cursor and the filter), as net::OnlineClient's do.
    auto answer = [&](bool ok, const char* error) {
        Event e = I.result(k, ok, error);
        e.gamesPage.before = before;
        e.gamesPage.filter = filter;
        return e;
    };
    if (I.transportError(k, answer(false, ""))) return;
    if (!I.signedIn) return I.http(answer(false, "unauthorized"));
    if (limit < 1 || limit > 50) return I.http(answer(false, "invalid_limit"));
    const bool catOk = filter.category.empty() || filter.category == "custom" || findCategory(filter.category);
    const bool resultOk = filter.result.empty() || filter.result == "win" || filter.result == "loss" || filter.result == "draw";
    if (!catOk || !resultOk || filter.rated < -1 || filter.rated > 1) return I.http(answer(false, "invalid_filter"));
    I.ensureHistory();
    Event e = answer(true, "");
    GamesPage& page = e.gamesPage;
    for (const Impl::Past& p : I.history) {
        const GameDetails& d = p.d;
        if (!filter.category.empty() && d.category != filter.category) continue;
        if (filter.rated >= 0 && d.rated != (filter.rated == 1)) continue;
        if (!filter.result.empty()) {
            if (d.status == Aborted || d.status == Ongoing) continue;
            const char* outcome = d.status == Draw ? "draw" : (d.status == WhiteWins) == (d.you == 0) ? "win" : "loss";
            if (filter.result != outcome) continue;
        }
        ++page.total;
        if (before && d.id >= before) continue;
        if (int(page.games.size()) < limit) {
            page.games.push_back(static_cast<const GameSummary&>(d));
        } else if (!page.next) {
            page.next = page.games.back().id;
        }
    }
    I.http(e);
}
void FakeServer::fetchGame(uint64_t gameId) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::GameDetailsResult;
    // Every answer names the game asked for, as net::OnlineClient's do.
    auto answer = [&](bool ok, const char* error) {
        Event e = I.result(k, ok, error);
        e.gameId = gameId;
        return e;
    };
    if (I.transportError(k, answer(false, ""))) return;
    if (!I.signedIn) return I.http(answer(false, "unauthorized"));
    I.ensureHistory();
    Impl::Past* p = I.findPast(gameId);
    if (!p) return I.http(answer(false, "not_found"));
    Event e = answer(true, "");
    e.gameDetails = p->d;
    e.gameDetails.reportable = I.reportable(*p);
    I.http(e);
}
void FakeServer::downloadPgn(uint64_t gameId) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::PgnResult;
    auto answer = [&](bool ok, const char* error) {
        Event e = I.result(k, ok, error);
        e.gameId = gameId;
        return e;
    };
    if (I.transportError(k, answer(false, ""))) return;
    I.ensureHistory();
    Impl::Past* p = I.signedIn ? I.findPast(gameId) : nullptr;
    if (!p) return I.http(answer(false, "not_found"));
    Event e = answer(true, "");
    e.text = serverPgn(p->d, contains(I.ep.host, "official") ? "Scacelith" : "Scacelith (mock server)", I.ep.host);
    I.http(e);
}
void FakeServer::fetchSessions() {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::SessionsResult;
    if (I.transportError(k)) return;
    if (!I.signedIn) return I.http(I.result(k, false, "unauthorized"));
    I.ensureDevices();
    for (SessionInfo& s : I.devices)
        if (s.current) s.lastSeenAtMs = int64_t(I.lastNow);
    Event e = I.result(k, true);
    e.sessions = I.devices;
    I.http(e);
}
void FakeServer::revokeSession(int64_t sessionId) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::SessionRevoked;
    if (I.transportError(k)) return;
    if (!I.signedIn) return I.http(I.result(k, false, "unauthorized"));
    I.ensureDevices();
    auto it = std::find_if(I.devices.begin(), I.devices.end(), [&](const SessionInfo& s) { return s.id == sessionId; });
    Event e = I.result(k, it != I.devices.end(), it != I.devices.end() ? "" : "not_found");
    e.sessionId = sessionId;
    if (it != I.devices.end()) {
        const bool current = it->current;
        I.devices.erase(it);
        if (current) {  // this very session: signed out like a logout
            disconnect();
            I.signedIn = false;
            I.sessionUser.clear();
        }
    }
    I.http(e);
}
void FakeServer::setAcceptChallenges(bool accept) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::PreferencesResult;
    if (I.transportError(k)) return;
    if (!I.signedIn) return I.http(I.result(k, false, "unauthorized"));
    I.account.acceptChallenges = accept;
    Event e = I.result(k, true);
    e.account.acceptChallenges = accept;
    I.http(e);
}
void FakeServer::changeEmail(const std::string& newEmail, const std::string& password, const std::string& codeOrRecovery) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::EmailChangeResult;
    if (I.transportError(k)) return;
    if (!I.signedIn) return I.http(I.result(k, false, "unauthorized"));
    if (!I.reauth(k, password, codeOrRecovery)) return;
    std::string email = lower(newEmail);
    while (!email.empty() && email.back() == ' ') email.pop_back();
    while (!email.empty() && email.front() == ' ') email.erase(email.begin());
    const size_t at = email.find('@');
    if (at == std::string::npos || at == 0 || email.find('.', at) == std::string::npos || email.back() == '.' || email.find(' ') != std::string::npos)
        return I.http(I.result(k, false, "invalid_email"));
    if (email == lower(I.account.email)) return I.http(I.result(k, false, "same_email"));
    Event e = I.result(k, true);
    if (I.hostHas("noverify")) {
        // A server without e-mail confirmation (REQUIRE_EMAIL_VERIFICATION=false): at once.
        if (contains(email, "taken")) return I.http(I.result(k, false, "email_taken"));
        I.account.email = email;
        I.account.emailVerified = true;
        I.account.pendingEmail.clear();
        e.status = "email_changed";
    } else {
        // A link goes to the new address (the same answer when the address is taken); the fake
        // "opens" it 40 s later (seen at the next fetchAccount).
        I.account.pendingEmail = email;
        I.pendingSince = I.lastNow;
        e.status = "verification_sent";
    }
    I.http(e);
}
void FakeServer::exportAccount(const std::string& password, const std::string& codeOrRecovery) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::AccountExportResult;
    if (I.transportError(k)) return;
    if (!I.signedIn) return I.http(I.result(k, false, "unauthorized"));
    if (!I.reauth(k, password, codeOrRecovery)) return;
    // Five an hour (account_export).
    I.exportTimes.erase(std::remove_if(I.exportTimes.begin(), I.exportTimes.end(), [&](double t) { return I.lastNow - t >= 3600000.0; }),
                        I.exportTimes.end());
    if (I.exportTimes.size() >= 5) {
        Event e = I.result(k, false, "rate_limited");
        e.retryAfterSec = int(std::ceil((I.exportTimes.front() + 3600000.0 - I.lastNow) / 1000.0));
        return I.http(e);
    }
    I.exportTimes.push_back(I.lastNow);
    Event e = I.result(k, true);
    e.text = I.exportDocument();
    I.http(e);
}
void FakeServer::deleteAccount(const std::string& password, const std::string& codeOrRecovery) {
    Impl& I = *impl_;
    I.lastNow = nowMs();
    const Event::Kind k = Event::Kind::AccountDeleted;
    if (I.transportError(k)) return;
    if (!I.signedIn) return I.http(I.result(k, false, "unauthorized"));
    if (!I.reauth(k, password, codeOrRecovery)) return;
    // The network layer erases the session and stops the realtime connection.
    disconnect();
    LOGI("mock server: account %s deleted", I.account.username.c_str());
    I.signedIn = false;
    I.sessionUser.clear();
    I.mfaEnabled = false;
    I.account = AccountInfo();
    I.history.clear();
    I.historyMade = false;
    I.historyOf.clear();
    I.devices.clear();
    I.http(I.result(k, true));
}

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
