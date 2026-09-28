// Tournament arbiter for the physical board: touch-move (FIDE Laws Art. 4) and completed
// illegal moves (Art. 7.5, Appendix A.4.2 / B for rapid and blitz).
#include "chess/chess.h"
#include "i18n/i18n.h"

#include <cstdlib>

namespace chess {

namespace {

bool isPromotionPiece(PieceType t) { return t == Knight || t == Bishop || t == Rook || t == Queen; }
// Arbiter messages are whole translated sentences (assets/i18n, section "Arbiter"), one key per
// colour so that every language can inflect the colour names; join() chains them.
std::string said(const std::string& key, Color c) { return i18n::tr(key + (c == White ? ".white" : ".black")); }
void join(std::string& msg, const std::string& sentence) {
    msg = msg.empty() ? sentence : i18n::trf("arbiter.join", {msg, sentence});
}

enum IllegalKind { kIllegalMove = 0, kNoMove = 1, kUnpromoted = 2 };

}  // namespace

bool Arbiter::inSync(const Game& game) const {
    return ply_ == game.moves().size() && pos_.samePosition(game.position());
}

void Arbiter::reset(const Game& game) {
    touched_ = NoSquare;
    pending_ = false;
    placement_ = Placement{};
    illegal_[0] = illegal_[1] = 0;
    pos_ = game.position();
    ply_ = game.moves().size();
    seenPly_ = ply_;
    rawEp_ = pos_.epSquare();
    if (!game.moves().empty() && (game.moves().back().flags & MoveDoublePush))
        rawEp_ = Square((game.moves().back().from + game.moves().back().to) / 2);
    over_ = game.isOver();
}

void Arbiter::sync(const Game& game) {
    if (game.moves().size() < seenPly_) {  // a new game was started without reset()
        reset(game);
        return;
    }
    seenPly_ = game.moves().size();
    if (!inSync(game)) {
        const int illegalW = illegal_[White], illegalB = illegal_[Black];
        reset(game);
        illegal_[White] = illegalW;
        illegal_[Black] = illegalB;
    }
    over_ = game.isOver();
}

bool Arbiter::touch(const Game& game, Square sq) {
    sync(game);
    return touch(sq);
}

bool Arbiter::touch(Square sq) {
    if (over_ || sq < 0 || sq > 63) return false;
    if (touched_ != NoSquare) {
        if (sq == touched_) return true;
        // Art. 4.5: when the touched piece cannot be moved the player may make any legal move.
        if (pending_ || !pos_.legalMovesFrom(touched_).empty()) return false;
        touched_ = NoSquare;
    }
    const Piece p = pos_.at(sq);
    if (p.empty() || p.color != pos_.sideToMove()) return false;
    touched_ = sq;
    return true;
}

bool Arbiter::touchedHasLegalMove(const Game& game) const {
    if (touched_ == NoSquare || !inSync(game)) return false;
    return !pos_.legalMovesFrom(touched_).empty();
}

void Arbiter::cancelTouch() {
    if (touched_ == NoSquare || !pos_.legalMovesFrom(touched_).empty()) return;
    touched_ = NoSquare;
    pending_ = false;
    placement_ = Placement{};
}

bool Arbiter::needsPromotion() const {
    if (!pending_) return false;
    const Piece p = pos_.at(placement_.from);
    return p.type == Pawn && rankOf(placement_.to) == (p.color == White ? 7 : 0) && placement_.promotion == NoPiece;
}

bool Arbiter::pendingIsLegal() const {
    if (!pending_) return false;
    const PieceType promo = needsPromotion() ? Queen : placement_.promotion;
    return pos_.findLegal(placement_.from, placement_.to, promo).valid();
}

bool Arbiter::place(const Game& game, Square to, PieceType promotion) {
    sync(game);
    if (over_ || touched_ == NoSquare || to < 0 || to > 63) return false;
    if (pending_) {
        // Art. 4.6: the square is final; only the missing promotion piece may still be supplied.
        if (to == placement_.to && needsPromotion() && isPromotionPiece(promotion)) {
            placement_.promotion = promotion;
            return true;
        }
        return false;
    }
    const Square from = touched_;
    if (to == from) return false;
    const Piece p = pos_.at(from), target = pos_.at(to);
    if (!target.empty() && target.color == p.color) return false;  // physically impossible

    Placement pl;
    pl.from = from;
    pl.to = to;
    if (!target.empty()) pl.captured = to;
    const Color us = p.color;
    if (p.type == Pawn) {
        const int dir = us == White ? 1 : -1;
        if (rankOf(to) == (us == White ? 7 : 0)) {
            pl.promotion = isPromotionPiece(promotion) ? promotion : NoPiece;
        } else if (target.empty() && to == rawEp_ && std::abs(fileOf(to) - fileOf(from)) == 1 && rankOf(to) - rankOf(from) == dir) {
            const Square passed = Square(to - 8 * dir);
            if (pos_.at(passed) == Piece{Pawn, opposite(us)}) pl.captured = passed;  // en passant
        }
    }
    if (p.type == King && from == (us == White ? 4 : 60) && rankOf(to) == rankOf(from) && std::abs(fileOf(to) - fileOf(from)) == 2) {
        const bool kingSide = fileOf(to) > fileOf(from);
        const uint8_t right = kingSide ? (us == White ? WhiteKingSide : BlackKingSide) : (us == White ? WhiteQueenSide : BlackQueenSide);
        const Square rookTo = makeSquare(kingSide ? 5 : 3, rankOf(from));
        if ((pos_.castling() & right) && pos_.at(rookTo).empty()) pl.castlingRookMove = true;
    }
    placement_ = pl;
    pending_ = true;
    return true;
}

bool Arbiter::pendingNeedsPromotion(const Game& game) const { return inSync(game) && needsPromotion(); }

bool Arbiter::choosePromotion(const Game& game, PieceType promotion) {
    sync(game);
    if (!needsPromotion() || !isPromotionPiece(promotion)) return false;
    placement_.promotion = promotion;
    return true;
}

bool Arbiter::retractPlacement(const Game& game) {
    sync(game);
    if (!pending_ || pendingIsLegal()) return false;
    pending_ = false;
    placement_ = Placement{};
    return true;
}

void Arbiter::advance(const Move& m) {
    rawEp_ = (m.flags & MoveDoublePush) ? Square((m.from + m.to) / 2) : NoSquare;
    pos_.makeMove(m);
    ++ply_;
    touched_ = NoSquare;
    pending_ = false;
    placement_ = Placement{};
}

Arbiter::Verdict Arbiter::illegal(Color mover, const TimeControl& tc, int kind) {
    Verdict v;
    pending_ = false;
    placement_ = Placement{};
    ++illegal_[mover];
    const Color opp = opposite(mover);
    std::string msg;
    switch (kind) {
    case kNoMove: msg = i18n::tr("arbiter.no_move"); break;
    case kUnpromoted: msg = i18n::tr("arbiter.unpromoted"); break;
    default: msg = i18n::tr("arbiter.illegal"); break;
    }
    if (illegal_[mover] >= 2) {
        v.forfeit = true;
        if (pos_.canColorMate(opp)) join(msg, said("arbiter.second_loses", mover));
        else join(msg, said("arbiter.second_draw", mover));
        v.message = msg;
        return v;
    }
    switch (tc.category()) {
    case TimeControl::Category::Standard: v.opponentBonusMs = 2 * 60000; break;  // Art. 7.5.5
    case TimeControl::Category::Rapid:
    case TimeControl::Category::Blitz: v.opponentBonusMs = 60000; break;         // Appendix A.4.2 / B
    case TimeControl::Category::Unlimited: v.opponentBonusMs = 0; break;
    }
    const std::string bonus = v.opponentBonusMs == 120000 ? "two_minutes" : "one_minute";
    if (kind == kUnpromoted) {
        join(msg, v.opponentBonusMs ? said("arbiter.queen_" + bonus, opp) : std::string(i18n::tr("arbiter.queen")));
    } else if (kind == kIllegalMove) {
        join(msg, v.opponentBonusMs ? said("arbiter.restored_" + bonus, opp) : std::string(i18n::tr("arbiter.restored")));
    } else if (v.opponentBonusMs) {
        join(msg, said("arbiter." + bonus, opp));
    }
    if (!v.opponentBonusMs) join(msg, i18n::tr("arbiter.second_loses"));
    if (kind == kIllegalMove && touched_ != NoSquare && !pos_.legalMovesFrom(touched_).empty())
        join(msg, said("arbiter.touched", mover));
    v.message = msg;
    return v;
}

Arbiter::Verdict Arbiter::clockPressed(const Game& game, const TimeControl& tc) {
    sync(game);
    if (over_) {
        Verdict v;
        v.message = i18n::tr("arbiter.game_over");
        return v;
    }
    const Color mover = pos_.sideToMove();
    if (!pending_) return illegal(mover, tc, kNoMove);  // Art. 7.5.3
    if (needsPromotion()) {
        const Move q = pos_.findLegal(placement_.from, placement_.to, Queen);
        if (!q.valid()) return illegal(mover, tc, kIllegalMove);
        Verdict v = illegal(mover, tc, kUnpromoted);  // Art. 7.5.2: penalised, replaced by a queen
        if (!v.forfeit) {
            v.moveStands = true;
            v.move = q;
            advance(q);
        }
        return v;
    }
    const Move m = pos_.findLegal(placement_.from, placement_.to, placement_.promotion);
    if (!m.valid()) return illegal(mover, tc, kIllegalMove);  // Art. 7.5.1: same piece must move (touched_ kept)
    Verdict v;
    v.legal = true;
    v.move = m;
    advance(m);
    return v;
}

}  // namespace chess
