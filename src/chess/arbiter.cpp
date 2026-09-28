// Tournament arbiter for the physical board: touch-move (FIDE Laws Art. 4) and completed
// illegal moves (Art. 7.5, Appendix A.4.2 / B for rapid and blitz).
#include "chess/chess.h"

#include <cstdlib>

namespace chess {

namespace {

bool isPromotionPiece(PieceType t) { return t == Knight || t == Bishop || t == Rook || t == Queen; }
const char* colorName(Color c) { return c == White ? "White" : "Black"; }

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
    case kNoMove: msg = "Arbiter: the clock was pressed without making a move, which counts as an illegal move."; break;
    case kUnpromoted: msg = "Arbiter: illegal move, the pawn was not replaced by a new piece before the clock was pressed."; break;
    default: msg = "Arbiter: illegal move."; break;
    }
    if (illegal_[mover] >= 2) {
        v.forfeit = true;
        if (pos_.canColorMate(opp))
            msg += std::string(" This is ") + colorName(mover) + "'s second illegal move: " + colorName(mover) + " loses the game.";
        else
            msg += std::string(" This is ") + colorName(mover) + "'s second illegal move, but " + colorName(opp) +
                   " cannot checkmate: the game is drawn.";
        v.message = msg;
        return v;
    }
    switch (tc.category()) {
    case TimeControl::Category::Standard: v.opponentBonusMs = 2 * 60000; break;  // Art. 7.5.5
    case TimeControl::Category::Rapid:
    case TimeControl::Category::Blitz: v.opponentBonusMs = 60000; break;         // Appendix A.4.2 / B
    case TimeControl::Category::Unlimited: v.opponentBonusMs = 0; break;
    }
    const std::string bonus = v.opponentBonusMs == 120000 ? "two extra minutes" : "one extra minute";
    const std::string opponent = colorName(opp);
    if (kind == kUnpromoted) {
        msg += " The pawn becomes a queen";
        msg += v.opponentBonusMs ? " and " + opponent + " receives " + bonus + "." : std::string(".");
    } else {
        if (kind == kIllegalMove) msg += " The position is restored";
        if (v.opponentBonusMs) msg += (kind == kIllegalMove ? " and " : " ") + opponent + " receives " + bonus + ".";
        else msg += kind == kIllegalMove ? "." : "";
    }
    if (!v.opponentBonusMs) msg += " A second illegal move loses the game.";
    if (kind == kIllegalMove && touched_ != NoSquare && !pos_.legalMovesFrom(touched_).empty())
        msg += std::string(" ") + colorName(mover) + " must move the touched piece.";
    v.message = msg;
    return v;
}

Arbiter::Verdict Arbiter::clockPressed(const Game& game, const TimeControl& tc) {
    sync(game);
    if (over_) {
        Verdict v;
        v.message = "The game is over.";
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
