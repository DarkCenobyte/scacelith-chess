// Game record: automatic endings, draw claims, resignation, flag fall, PGN export.
#include "chess/chess.h"
#include "i18n/i18n.h"

#include <cstdio>
#include <ctime>

namespace chess {

const char* endReasonKey(GameEndReason r) {
    switch (r) {
    case GameEndReason::None: return "";
    case GameEndReason::Checkmate: return "reason.checkmate";
    case GameEndReason::Resignation: return "reason.resignation";
    case GameEndReason::Timeout: return "reason.timeout";
    case GameEndReason::IllegalMoves: return "reason.illegal_moves";
    case GameEndReason::Stalemate: return "reason.stalemate";
    case GameEndReason::InsufficientMaterial: return "reason.insufficient";
    case GameEndReason::TimeoutVsInsufficient: return "reason.timeout_vs_insufficient";
    case GameEndReason::FivefoldRepetition: return "reason.fivefold";
    case GameEndReason::SeventyFiveMoves: return "reason.75_moves";
    case GameEndReason::ThreefoldClaim: return "reason.threefold_claim";
    case GameEndReason::FiftyMoveClaim: return "reason.50_moves_claim";
    case GameEndReason::Agreement: return "reason.agreement";
    case GameEndReason::IllegalMovesVsInsufficient: return "reason.illegal_vs_insufficient";
    }
    return "";
}

const char* endReasonText(GameEndReason r) {
    const char* key = endReasonKey(r);
    return *key ? i18n::tr(key) : "";
}

Game::Game() { reset(); }

void Game::reset() {
    positions_.assign(1, Position());
    moves_.clear();
    san_.clear();
    status_ = GameStatus::Ongoing;
    reason_ = GameEndReason::None;
}

bool Game::resetFromFEN(const std::string& fen) {
    Position p;
    if (!p.setFEN(fen)) return false;
    positions_.assign(1, p);
    moves_.clear();
    san_.clear();
    status_ = GameStatus::Ongoing;
    reason_ = GameEndReason::None;
    updateStatus();
    return true;
}

std::vector<std::string> Game::uciMoves() const {
    std::vector<std::string> out;
    out.reserve(moves_.size());
    for (size_t i = 0; i < moves_.size(); ++i) out.push_back(positions_[i].toUCI(moves_[i]));
    return out;
}

bool Game::play(const Move& m) {
    if (status_ != GameStatus::Ongoing) return false;
    const Position& cur = position();
    const Move legal = cur.findLegal(m.from, m.to, m.promotion);
    if (!legal.valid()) return false;
    std::string san = cur.toSAN(legal);
    Position next(cur);
    next.makeMove(legal);
    positions_.push_back(next);
    moves_.push_back(legal);
    san_.push_back(std::move(san));
    updateStatus();
    return true;
}

bool Game::undo(int plies) {
    if (plies <= 0 || size_t(plies) > moves_.size()) return false;
    // positions_ holds the start position and one position per move: repetitions are counted from
    // it alone, so truncating the three records together keeps them consistent.
    positions_.resize(positions_.size() - size_t(plies));
    moves_.resize(moves_.size() - size_t(plies));
    san_.resize(san_.size() - size_t(plies));
    status_ = GameStatus::Ongoing;
    reason_ = GameEndReason::None;
    updateStatus();
    return true;
}

void Game::finish(GameStatus s, GameEndReason r) {
    if (status_ != GameStatus::Ongoing) return;
    status_ = s;
    reason_ = r;
}

void Game::updateStatus() {
    if (!endDetection_) return;
    const Position& p = position();
    if (!p.hasLegalMove()) {
        if (p.inCheck()) finish(p.sideToMove() == White ? GameStatus::BlackWins : GameStatus::WhiteWins, GameEndReason::Checkmate);
        else finish(GameStatus::Draw, GameEndReason::Stalemate);
        return;  // checkmate takes precedence over the 75-move rule (9.6.2)
    }
    if (p.hasInsufficientMaterial()) return finish(GameStatus::Draw, GameEndReason::InsufficientMaterial);
    if (repetitionCount() >= 5) return finish(GameStatus::Draw, GameEndReason::FivefoldRepetition);
    if (p.halfmoveClock() >= 150) return finish(GameStatus::Draw, GameEndReason::SeventyFiveMoves);
}

const char* Game::resultString() const {
    switch (status_) {
    case GameStatus::WhiteWins: return "1-0";
    case GameStatus::BlackWins: return "0-1";
    case GameStatus::Draw: return "1/2-1/2";
    default: return "*";
    }
}

int Game::repetitionCount() const {
    const Position& cur = position();
    const int last = int(positions_.size()) - 1;
    // Only positions since the last irreversible move (pawn move or capture) can repeat.
    const int window = cur.halfmoveClock();
    int count = 0;
    for (int i = last; i >= 0 && last - i <= window; i -= 2)
        if (positions_[size_t(i)].samePosition(cur)) ++count;
    return count;
}

bool Game::canClaimThreefold() const { return status_ == GameStatus::Ongoing && repetitionCount() >= 3; }

bool Game::canClaimFiftyMove() const { return status_ == GameStatus::Ongoing && position().halfmoveClock() >= 100; }

void Game::claimDraw() {
    if (canClaimThreefold()) finish(GameStatus::Draw, GameEndReason::ThreefoldClaim);
    else if (canClaimFiftyMove()) finish(GameStatus::Draw, GameEndReason::FiftyMoveClaim);
}

void Game::resign(Color loser) {
    finish(loser == White ? GameStatus::BlackWins : GameStatus::WhiteWins, GameEndReason::Resignation);
}

void Game::agreeDraw() { finish(GameStatus::Draw, GameEndReason::Agreement); }

void Game::flagFall(Color flagged) {
    const Color winner = opposite(flagged);
    if (!position().canColorMate(winner)) finish(GameStatus::Draw, GameEndReason::TimeoutVsInsufficient);
    else finish(winner == White ? GameStatus::WhiteWins : GameStatus::BlackWins, GameEndReason::Timeout);
}

void Game::forfeitIllegal(Color offender) {
    const Color winner = opposite(offender);
    if (!position().canColorMate(winner)) finish(GameStatus::Draw, GameEndReason::IllegalMovesVsInsufficient);
    else finish(winner == White ? GameStatus::WhiteWins : GameStatus::BlackWins, GameEndReason::IllegalMoves);
}

// ---- PGN ------------------------------------------------------------------------------------

namespace {

std::string pgnEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (c == '\n' || c == '\r') c = ' ';
        r += c;
    }
    return r;
}

// UTC date "YYYY.MM.DD" without gmtime (not thread-safe on every platform).
std::string todayUtc() {
    const long long secs = (long long)std::time(nullptr);
    if (secs <= 0) return "????.??.??";
    long long z = secs / 86400 + 719468;  // days since 0000-03-01 (H. Hinnant's civil_from_days)
    const long long era = z / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long m = mp < 10 ? mp + 3 : mp - 9;
    const long long y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    if (y < 1970 || y > 9999) return "????.??.??";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d.%02d.%02d", int(y), int(m) % 100, int(d) % 100);
    return buf;
}

const char* terminationTag(GameStatus st, GameEndReason r) {
    if (st == GameStatus::Ongoing) return "unterminated";
    switch (r) {
    case GameEndReason::Timeout:
    case GameEndReason::TimeoutVsInsufficient: return "time forfeit";
    case GameEndReason::IllegalMoves:
    case GameEndReason::IllegalMovesVsInsufficient: return "rules infraction";
    default: return "normal";
    }
}

}  // namespace

std::string Game::pgn(const std::string& whiteName, const std::string& blackName) const {
    return pgn(whiteName, blackName, PgnTags{});
}

std::string Game::pgn(const std::string& whiteName, const std::string& blackName, const PgnTags& tags) const {
    const std::string result = resultString();
    std::string out;
    auto tag = [&out](const char* name, const std::string& value) { out += std::string("[") + name + " \"" + pgnEscape(value) + "\"]\n"; };
    tag("Event", tags.event);
    tag("Site", tags.site);
    tag("Date", tags.date.empty() ? todayUtc() : tags.date);
    tag("Round", tags.round);
    tag("White", whiteName);
    tag("Black", blackName);
    tag("Result", result);
    const Position& start = startPosition();
    const Position standard;
    if (!start.samePosition(standard) || start.halfmoveClock() != 0 || start.fullmoveNumber() != 1) {
        tag("SetUp", "1");
        tag("FEN", start.fen());
    }
    tag("TimeControl", tags.timeControl.empty() ? std::string("?") : tags.timeControl);
    tag("Termination", terminationTag(status_, reason_));
    out += '\n';

    // Movetext, wrapped at 80 columns.
    std::string line;
    auto emit = [&](const std::string& tok) {
        if (!line.empty() && line.size() + 1 + tok.size() > 79) {
            out += line + '\n';
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += tok;
    };
    int moveNo = start.fullmoveNumber();
    Color side = start.sideToMove();
    for (size_t i = 0; i < san_.size(); ++i) {
        if (side == White) emit(std::to_string(moveNo) + ".");
        else if (i == 0) emit(std::to_string(moveNo) + "...");
        emit(san_[i]);
        if (side == Black) ++moveNo;
        side = opposite(side);
    }
    if (status_ != GameStatus::Ongoing) {
        // PGN is an interchange format: the comment stays in English whatever the UI language.
        const std::string words = std::string("{") + i18n::english(endReasonKey(reason_)) + "}";
        // Comments may be wrapped anywhere: emit word by word.
        std::string word;
        for (char c : words) {
            if (c == ' ') {
                emit(word);
                word.clear();
            } else {
                word += c;
            }
        }
        if (!word.empty()) emit(word);
    }
    emit(result);
    out += line + '\n';
    return out;
}

}  // namespace chess
