// Position: bitboard move generation (pseudo-legal + fast legality filter), FEN, SAN/UCI, perft.
#include "chess/bitboard.h"
#include "chess/chess.h"

#include <cstdlib>
#include <cstring>

namespace chess {
using namespace bb;

namespace {

// Move buffers (generatePseudo writes them without a bound check). 256 is enough only because
// setFEN caps each side at 16 pieces and 8 pawns and rejects a side not to move in check, caps that
// makeMove and passTurn keep: searches over such positions found at most 242 pseudo-legal moves
// (15 queens and a king). That is a measured margin, not a proof: revisit it if setFEN is ever
// relaxed (a board editor, variants).
constexpr int kMaxMoves = 256;

inline Move mkMove(int from, int to, PieceType promo, int flags) {
    Move m;
    m.from = Square(from);
    m.to = Square(to);
    m.promotion = promo;
    m.flags = uint8_t(flags);
    return m;
}

inline U64 pieceAttacks(PieceType t, int s, U64 occ) {
    switch (t) {
    case Knight: return kTables.knight[s];
    case Bishop: return bishopAttacks(s, occ);
    case Rook: return rookAttacks(s, occ);
    case Queen: return bishopAttacks(s, occ) | rookAttacks(s, occ);
    case King: return kTables.king[s];
    default: return 0;
    }
}

inline bool isPromotionPiece(PieceType t) { return t == Knight || t == Bishop || t == Rook || t == Queen; }

inline bool inSet(const char* set, char c) { return c != '\0' && std::strchr(set, c) != nullptr; }

PieceType pieceFromChar(char c) {
    switch (c) {
    case 'p': case 'P': return Pawn;
    case 'n': case 'N': return Knight;
    case 'b': case 'B': return Bishop;
    case 'r': case 'R': return Rook;
    case 'q': case 'Q': return Queen;
    case 'k': case 'K': return King;
    default: return NoPiece;
    }
}

char pieceUpper(PieceType t) { return " PNBRQK"[t]; }

std::vector<std::string> splitSpaces(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool parseInt(const std::string& s, int& out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

}  // namespace

// ---- Squares --------------------------------------------------------------------------------

std::string squareName(Square s) {
    if (s < 0 || s > 63) return "-";
    std::string r;
    r += char('a' + fileOf(s));
    r += char('1' + rankOf(s));
    return r;
}

Square parseSquare(const std::string& s) {
    if (s.size() != 2) return NoSquare;
    char fc = s[0];
    if (fc >= 'A' && fc <= 'H') fc = char(fc - 'A' + 'a');
    const int f = fc - 'a', r = s[1] - '1';
    if (f < 0 || f > 7 || r < 0 || r > 7) return NoSquare;
    return makeSquare(f, r);
}

// ---- Board state ----------------------------------------------------------------------------

Position::Position() { setStart(); }

void Position::clear() {
    for (auto& p : board_) p = Piece{};
    colorBB_[0] = colorBB_[1] = 0;
    for (auto& b : typeBB_) b = 0;
    side_ = White;
    castling_ = 0;
    ep_ = NoSquare;
    halfmove_ = 0;
    fullmove_ = 1;
    hash_ = 0;
}

void Position::putPiece(Square s, Piece p) {
    board_[s] = p;
    colorBB_[p.color] |= bit(s);
    typeBB_[p.type] |= bit(s);
    hash_ ^= kZobrist.piece[p.color][p.type][s];
}

void Position::removePiece(Square s) {
    const Piece p = board_[s];
    if (p.empty()) return;
    colorBB_[p.color] &= ~bit(s);
    typeBB_[p.type] &= ~bit(s);
    hash_ ^= kZobrist.piece[p.color][p.type][s];
    board_[s] = Piece{};
}

void Position::recomputeHash() {
    U64 h = 0;
    for (int s = 0; s < 64; ++s)
        if (!board_[s].empty()) h ^= kZobrist.piece[board_[s].color][board_[s].type][s];
    h ^= kZobrist.castling[castling_ & 0xF];
    if (ep_ != NoSquare) h ^= kZobrist.epFile[fileOf(ep_)];
    if (side_ == Black) h ^= kZobrist.side;
    hash_ = h;
}

void Position::setStart() {
    clear();
    const PieceType back[8] = {Rook, Knight, Bishop, Queen, King, Bishop, Knight, Rook};
    for (int f = 0; f < 8; ++f) {
        putPiece(makeSquare(f, 0), Piece{back[f], White});
        putPiece(makeSquare(f, 1), Piece{Pawn, White});
        putPiece(makeSquare(f, 6), Piece{Pawn, Black});
        putPiece(makeSquare(f, 7), Piece{back[f], Black});
    }
    castling_ = 0xF;
    recomputeHash();
}

bool Position::setFEN(const std::string& fen) {
    const std::vector<std::string> f = splitSpaces(fen);
    if (f.size() < 4 || f.size() > 6) return false;
    Position p(*this);
    p.clear();
    int rank = 7, file = 0;
    for (char c : f[0]) {
        if (c == '/') {
            if (file != 8 || rank == 0) return false;
            --rank;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) return false;
        } else {
            const PieceType t = pieceFromChar(c);
            if (t == NoPiece || file > 7) return false;
            p.putPiece(makeSquare(file, rank), Piece{t, (c >= 'A' && c <= 'Z') ? White : Black});
            ++file;
        }
    }
    if (rank != 0 || file != 8) return false;
    for (int c = 0; c < 2; ++c) {
        if (popcount(p.typeBB_[King] & p.colorBB_[c]) != 1) return false;
        if (popcount(p.colorBB_[c]) > 16 || popcount(p.typeBB_[Pawn] & p.colorBB_[c]) > 8) return false;
    }
    if (p.typeBB_[Pawn] & (rankBB(0) | rankBB(7))) return false;

    if (f[1] == "w") p.side_ = White;
    else if (f[1] == "b") p.side_ = Black;
    else return false;

    uint8_t rights = 0;
    if (f[2] != "-") {
        for (char c : f[2]) {
            switch (c) {
            case 'K': rights |= WhiteKingSide; break;
            case 'Q': rights |= WhiteQueenSide; break;
            case 'k': rights |= BlackKingSide; break;
            case 'q': rights |= BlackQueenSide; break;
            default: return false;
            }
        }
    }
    // Keep only rights whose king and rook stand on their initial squares.
    const Piece wk{King, White}, bk{King, Black}, wr{Rook, White}, br{Rook, Black};
    if (p.board_[4] != wk) rights &= uint8_t(~(WhiteKingSide | WhiteQueenSide));
    if (p.board_[60] != bk) rights &= uint8_t(~(BlackKingSide | BlackQueenSide));
    if (p.board_[7] != wr) rights &= uint8_t(~WhiteKingSide);
    if (p.board_[0] != wr) rights &= uint8_t(~WhiteQueenSide);
    if (p.board_[63] != br) rights &= uint8_t(~BlackKingSide);
    if (p.board_[56] != br) rights &= uint8_t(~BlackQueenSide);
    p.castling_ = rights;

    if (f.size() >= 5 && !parseInt(f[4], p.halfmove_)) return false;
    if (f.size() >= 6 && !parseInt(f[5], p.fullmove_)) return false;
    if (p.fullmove_ < 1) p.fullmove_ = 1;

    // The side that just moved cannot be in check.
    const Color them = opposite(p.side_);
    if (p.attackedWith(p.kingSquare(them), p.side_, p.occupied(), 0)) return false;

    p.ep_ = NoSquare;
    p.recomputeHash();
    if (f[3] != "-") {
        const Square e = parseSquare(f[3]);
        if (e == NoSquare) return false;
        const int up = p.side_ == White ? 8 : -8;
        if (rankOf(e) == (p.side_ == White ? 5 : 2) && p.board_[e - up] == Piece{Pawn, them} && p.board_[e].empty() &&
            p.board_[e + up].empty())
            p.setEpIfCapturable(e);
    }
    *this = p;
    return true;
}

std::string Position::fen() const {
    std::string s;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const Piece p = board_[makeSquare(f, r)];
            if (p.empty()) {
                ++empty;
                continue;
            }
            if (empty) s += char('0' + empty), empty = 0;
            const char c = pieceUpper(p.type);
            s += p.color == White ? c : char(c - 'A' + 'a');
        }
        if (empty) s += char('0' + empty);
        if (r) s += '/';
    }
    s += side_ == White ? " w " : " b ";
    if (!castling_) s += '-';
    if (castling_ & WhiteKingSide) s += 'K';
    if (castling_ & WhiteQueenSide) s += 'Q';
    if (castling_ & BlackKingSide) s += 'k';
    if (castling_ & BlackQueenSide) s += 'q';
    s += ' ';
    s += ep_ == NoSquare ? std::string("-") : squareName(ep_);
    s += ' ' + std::to_string(halfmove_) + ' ' + std::to_string(fullmove_);
    return s;
}

bool Position::samePosition(const Position& o) const {
    if (hash_ != o.hash_ || side_ != o.side_ || castling_ != o.castling_ || ep_ != o.ep_) return false;
    for (int s = 0; s < 64; ++s)
        if (board_[s] != o.board_[s]) return false;
    return true;
}

bool Position::isStandardStart() const { return samePosition(Position()) && halfmove_ == 0 && fullmove_ == 1; }

// ---- Attacks / legality ---------------------------------------------------------------------

Square Position::kingSquare(Color c) const {
    const U64 k = typeBB_[King] & colorBB_[c];
    return k ? Square(lsb(k)) : NoSquare;
}

bool Position::attackedWith(Square s, Color by, U64 occ, U64 removed) const {
    if (s < 0) return false;
    const U64 them = colorBB_[by] & ~removed;
    if (kTables.pawn[by ^ 1][s] & typeBB_[Pawn] & them) return true;
    if (kTables.knight[s] & typeBB_[Knight] & them) return true;
    if (kTables.king[s] & typeBB_[King] & them) return true;
    const U64 diag = (typeBB_[Bishop] | typeBB_[Queen]) & them;
    if (diag && (bishopAttacks(s, occ) & diag)) return true;
    const U64 orth = (typeBB_[Rook] | typeBB_[Queen]) & them;
    return orth && (rookAttacks(s, occ) & orth);
}

bool Position::isAttacked(Square s, Color by) const { return attackedWith(s, by, occupied(), 0); }

// ---- Board queries (explanations) -----------------------------------------------------------

std::vector<Square> squaresOf(U64 set) {
    std::vector<Square> out;
    out.reserve(size_t(popcount(set)));
    while (set) out.push_back(Square(popLsb(set)));
    return out;
}

int squareCount(U64 set) { return popcount(set); }

U64 attacksOf(PieceType t, Color c, Square s, U64 occ) {
    if (s < 0 || s > 63) return 0;
    return t == Pawn ? kTables.pawn[c][s] : pieceAttacks(t, s, occ);
}

U64 squaresBetween(Square a, Square b) {
    if (a < 0 || a > 63 || b < 0 || b > 63) return 0;
    return kTables.between[a][b];
}

U64 Position::attackersTo(Square s, U64 occ) const {
    if (s < 0 || s > 63) return 0;
    const U64 pawns = (kTables.pawn[Black][s] & typeBB_[Pawn] & colorBB_[White]) |
                      (kTables.pawn[White][s] & typeBB_[Pawn] & colorBB_[Black]);
    return (pawns | (kTables.knight[s] & typeBB_[Knight]) | (kTables.king[s] & typeBB_[King]) |
            (bishopAttacks(s, occ) & (typeBB_[Bishop] | typeBB_[Queen])) |
            (rookAttacks(s, occ) & (typeBB_[Rook] | typeBB_[Queen]))) &
           occ;
}

U64 Position::attacksFrom(Square s) const {
    if (s < 0 || s > 63 || board_[s].empty()) return 0;
    return attacksOf(board_[s].type, board_[s].color, s, occupied());
}

U64 Position::checkers() const {
    const Square k = kingSquare(side_);
    return k == NoSquare ? 0 : attackersTo(k, opposite(side_));
}

U64 Position::pinned(Color c) const {
    const Square k = kingSquare(c);
    return k == NoSquare ? 0 : pinnedPieces(c, k);
}

bool Position::passTurn() {
    if (inCheck()) return false;  // the side passing would stay in check: never a legal position
    if (ep_ != NoSquare) {
        hash_ ^= kZobrist.epFile[fileOf(ep_)];
        ep_ = NoSquare;
    }
    side_ = opposite(side_);
    hash_ ^= kZobrist.side;
    ++halfmove_;  // the move number is left alone (analysis only, never recorded)
    return true;
}

bool Position::inCheck() const { return attackedWith(kingSquare(side_), opposite(side_), occupied(), 0); }

U64 Position::pinnedPieces(Color c, Square ksq) const {
    const U64 them = colorBB_[opposite(c)];
    U64 snipers = ((rookRays(ksq) & (typeBB_[Rook] | typeBB_[Queen])) | (bishopRays(ksq) & (typeBB_[Bishop] | typeBB_[Queen]))) & them;
    const U64 occ = occupied();
    U64 pinned = 0;
    while (snipers) {
        const int s = popLsb(snipers);
        const U64 b = kTables.between[ksq][s] & occ;
        if (b && !several(b) && (b & colorBB_[c])) pinned |= b;
    }
    return pinned;
}

// Full legality test of a pseudo-legal, non-castling move of the side to move.
bool Position::legalFull(const Move& m, Square ksq) const {
    const Color us = side_, them = opposite(us);
    const U64 occ = occupied();
    if (m.from == ksq) return !attackedWith(m.to, them, occ ^ bit(m.from), bit(m.to));
    U64 occ2 = (occ ^ bit(m.from)) | bit(m.to);
    U64 removed = bit(m.to);
    if (m.flags & MoveEnPassant) {
        const int cap = m.to + (us == White ? -8 : 8);
        occ2 ^= bit(cap);
        removed |= bit(cap);
    }
    return !attackedWith(ksq, them, occ2, removed);
}

// ---- Move generation ------------------------------------------------------------------------

int Position::generatePseudo(Move* out) const {
    int n = 0;
    const Color us = side_, them = opposite(us);
    const U64 own = colorBB_[us], enemy = colorBB_[them], occ = own | enemy;

    // Pawns
    const int up = us == White ? 8 : -8;
    const int lastRank = us == White ? 7 : 0, startRank = us == White ? 1 : 6;
    U64 pawns = typeBB_[Pawn] & own;
    while (pawns) {
        const int s = popLsb(pawns);
        const int to = s + up;
        if (!(occ & bit(to))) {
            if (rankOf(Square(to)) == lastRank) {
                for (PieceType t : {Queen, Rook, Bishop, Knight}) out[n++] = mkMove(s, to, t, MovePromotion);
            } else {
                out[n++] = mkMove(s, to, NoPiece, MoveQuiet);
                if (rankOf(Square(s)) == startRank && !(occ & bit(to + up))) out[n++] = mkMove(s, to + up, NoPiece, MoveDoublePush);
            }
        }
        U64 caps = kTables.pawn[us][s] & enemy;
        while (caps) {
            const int t = popLsb(caps);
            if (rankOf(Square(t)) == lastRank) {
                for (PieceType pt : {Queen, Rook, Bishop, Knight}) out[n++] = mkMove(s, t, pt, MovePromotion | MoveCapture);
            } else {
                out[n++] = mkMove(s, t, NoPiece, MoveCapture);
            }
        }
        if (ep_ != NoSquare && (kTables.pawn[us][s] & bit(ep_))) out[n++] = mkMove(s, ep_, NoPiece, MoveCapture | MoveEnPassant);
    }

    // Pieces
    for (PieceType t : {Knight, Bishop, Rook, Queen, King}) {
        U64 b = typeBB_[t] & own;
        while (b) {
            const int s = popLsb(b);
            U64 att = pieceAttacks(t, s, occ) & ~own;
            while (att) {
                const int to = popLsb(att);
                out[n++] = mkMove(s, to, NoPiece, (enemy & bit(to)) ? MoveCapture : MoveQuiet);
            }
        }
    }

    // Castling (fully checked here: rights, empty squares, not out of / through / into check)
    const uint8_t ks = us == White ? WhiteKingSide : BlackKingSide;
    const uint8_t qs = us == White ? WhiteQueenSide : BlackQueenSide;
    if (castling_ & (ks | qs)) {
        const int k = us == White ? 4 : 60;
        if (!attackedWith(Square(k), them, occ, 0)) {
            if ((castling_ & ks) && !(occ & (bit(k + 1) | bit(k + 2))) && !attackedWith(Square(k + 1), them, occ, 0) &&
                !attackedWith(Square(k + 2), them, occ, 0))
                out[n++] = mkMove(k, k + 2, NoPiece, MoveCastleKing);
            if ((castling_ & qs) && !(occ & (bit(k - 1) | bit(k - 2) | bit(k - 3))) && !attackedWith(Square(k - 1), them, occ, 0) &&
                !attackedWith(Square(k - 2), them, occ, 0))
                out[n++] = mkMove(k, k - 2, NoPiece, MoveCastleQueen);
        }
    }
    return n;
}

int Position::generateLegal(Move* out) const {
    const int n = generatePseudo(out);
    const Color them = opposite(side_);
    const Square ksq = kingSquare(side_);
    const U64 occ = occupied();
    const bool check = attackedWith(ksq, them, occ, 0);
    const U64 pinned = pinnedPieces(side_, ksq);
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const Move& mv = out[i];
        bool ok;
        if (mv.flags & (MoveCastleKing | MoveCastleQueen)) ok = true;
        else if (mv.from == ksq) ok = !attackedWith(mv.to, them, occ ^ bit(ksq), bit(mv.to));
        else if (!check && !(pinned & bit(mv.from)) && !(mv.flags & MoveEnPassant)) ok = true;
        else ok = legalFull(mv, ksq);
        if (ok) out[m++] = mv;
    }
    return m;
}

std::vector<Move> Position::legalMoves() const {
    Move buf[kMaxMoves];
    const int n = generateLegal(buf);
    return std::vector<Move>(buf, buf + n);
}

std::vector<Move> Position::legalMovesFrom(Square from) const {
    std::vector<Move> out;
    if (from < 0 || from > 63 || board_[from].empty() || board_[from].color != side_) return out;
    Move buf[kMaxMoves];
    const int n = generateLegal(buf);
    for (int i = 0; i < n; ++i)
        if (buf[i].from == from) out.push_back(buf[i]);
    return out;
}

bool Position::hasLegalMove() const {
    Move buf[kMaxMoves];
    return generateLegal(buf) > 0;
}

Move Position::findLegal(Square from, Square to, PieceType promo) const {
    if (from < 0 || from > 63 || to < 0 || to > 63) return Move{};
    if (board_[from].empty() || board_[from].color != side_) return Move{};
    Move buf[kMaxMoves];
    const int n = generateLegal(buf);
    for (int i = 0; i < n; ++i) {
        const Move& m = buf[i];
        if (m.from != from || m.to != to) continue;
        if ((m.flags & MovePromotion) && m.promotion != promo) continue;
        return m;
    }
    return Move{};
}

bool Position::isLegal(const Move& m) const {
    const Move l = findLegal(m.from, m.to, m.promotion);
    return l.valid() && l.promotion == m.promotion;
}

// ---- Making moves ---------------------------------------------------------------------------

void Position::setEpIfCapturable(Square e) {
    if (ep_ != NoSquare) hash_ ^= kZobrist.epFile[fileOf(ep_)];
    ep_ = NoSquare;
    const Square ksq = kingSquare(side_);
    U64 capturers = kTables.pawn[opposite(side_)][e] & typeBB_[Pawn] & colorBB_[side_];
    while (capturers) {
        const int c = popLsb(capturers);
        if (legalFull(mkMove(c, e, NoPiece, MoveCapture | MoveEnPassant), ksq)) {
            ep_ = e;
            hash_ ^= kZobrist.epFile[fileOf(e)];
            return;
        }
    }
}

void Position::makeMove(const Move& m) {
    const Color us = side_, them = opposite(us);
    const Square from = m.from, to = m.to;
    const Piece p = board_[from];
    const Piece cap = board_[to];
    const bool isPawn = p.type == Pawn;
    const bool isEp = isPawn && to == ep_ && cap.empty() && fileOf(from) != fileOf(to);

    if (ep_ != NoSquare) {
        hash_ ^= kZobrist.epFile[fileOf(ep_)];
        ep_ = NoSquare;
    }
    hash_ ^= kZobrist.castling[castling_];

    if (!cap.empty()) removePiece(to);
    if (isEp) removePiece(Square(to + (us == White ? -8 : 8)));
    removePiece(from);
    PieceType placed = p.type;
    if (isPawn && (rankOf(to) == 7 || rankOf(to) == 0)) placed = isPromotionPiece(m.promotion) ? m.promotion : Queen;
    putPiece(to, Piece{placed, us});
    if (p.type == King && std::abs(fileOf(to) - fileOf(from)) == 2) {
        const int r = rankOf(from);
        const Square rf = makeSquare(fileOf(to) == 6 ? 7 : 0, r), rt = makeSquare(fileOf(to) == 6 ? 5 : 3, r);
        const Piece rook = board_[rf];
        removePiece(rf);
        putPiece(rt, rook);
    }

    castling_ &= uint8_t(kTables.castleMask[from] & kTables.castleMask[to]);
    hash_ ^= kZobrist.castling[castling_];
    halfmove_ = (isPawn || !cap.empty()) ? 0 : halfmove_ + 1;
    if (us == Black) ++fullmove_;
    side_ = them;
    hash_ ^= kZobrist.side;
    if (isPawn && std::abs(to - from) == 16) setEpIfCapturable(Square((from + to) / 2));
}

// ---- Material / dead positions --------------------------------------------------------------

bool Position::hasInsufficientMaterial() const {
    const U64 others = occupied() & ~typeBB_[King];
    if (!others) return true;  // K v K
    if (others & (typeBB_[Pawn] | typeBB_[Rook] | typeBB_[Queen])) return false;
    const U64 knights = typeBB_[Knight], bishops = typeBB_[Bishop];
    if (!bishops) return !several(knights);  // K+N v K
    if (knights) return false;
    // Only bishops left: dead when they all stand on squares of the same colour.
    return !(bishops & kLightSquares) || !(bishops & kDarkSquares);
}

bool Position::canColorMate(Color c) const {
    const U64 mine = colorBB_[c] & ~typeBB_[King];
    if (mine & (typeBB_[Pawn] | typeBB_[Rook] | typeBB_[Queen])) return true;
    if (mine & typeBB_[Knight]) {
        // A lone knight mates only past a blocker of the opponent that cannot take it: not a queen.
        return several(mine) || (colorBB_[opposite(c)] & ~typeBB_[King] & ~typeBB_[Queen]);
    }
    if (!mine) return false;  // bare king
    // Bishops alone: an opposing pawn, knight or bishop of the other square colour can block a
    // flight square without being able to take or block the checking bishop.
    if (typeBB_[Pawn] | typeBB_[Knight]) return true;
    const U64 bishops = typeBB_[Bishop];
    return (bishops & kLightSquares) && (bishops & kDarkSquares);
}

// ---- Notation -------------------------------------------------------------------------------

std::string Position::toUCI(const Move& m) const {
    if (!m.valid()) return "0000";
    std::string s = squareName(m.from) + squareName(m.to);
    if (m.promotion != NoPiece) s += " pnbrqk"[m.promotion];
    return s;
}

Move Position::parseUCI(const std::string& in) const {
    if (in.size() != 4 && in.size() != 5) return Move{};
    const Square from = parseSquare(in.substr(0, 2)), to = parseSquare(in.substr(2, 2));
    if (from == NoSquare || to == NoSquare) return Move{};
    PieceType promo = NoPiece;
    if (in.size() == 5) {
        promo = pieceFromChar(in[4]);
        if (!isPromotionPiece(promo)) return Move{};
    }
    const Move m = findLegal(from, to, promo);
    if (m.valid() && promo != NoPiece && !(m.flags & MovePromotion)) return Move{};
    return m;
}

std::string Position::toSAN(const Move& in) const {
    const Move m = findLegal(in.from, in.to, in.promotion);
    if (!m.valid()) return "";
    std::string s;
    const Piece p = board_[m.from];
    if (m.flags & MoveCastleKing) {
        s = "O-O";
    } else if (m.flags & MoveCastleQueen) {
        s = "O-O-O";
    } else if (p.type == Pawn) {
        if (m.flags & MoveCapture) {
            s += char('a' + fileOf(m.from));
            s += 'x';
        }
        s += squareName(m.to);
        if (m.flags & MovePromotion) {
            s += '=';
            s += pieceUpper(m.promotion);
        }
    } else {
        s += pieceUpper(p.type);
        Move buf[kMaxMoves];
        const int n = generateLegal(buf);
        bool ambiguous = false, sameFile = false, sameRank = false;
        for (int i = 0; i < n; ++i) {
            const Move& o = buf[i];
            if (o.to != m.to || o.from == m.from || board_[o.from].type != p.type) continue;
            ambiguous = true;
            if (fileOf(o.from) == fileOf(m.from)) sameFile = true;
            if (rankOf(o.from) == rankOf(m.from)) sameRank = true;
        }
        if (ambiguous) {
            if (!sameFile) {
                s += char('a' + fileOf(m.from));
            } else if (!sameRank) {
                s += char('1' + rankOf(m.from));
            } else {
                s += squareName(m.from);
            }
        }
        if (m.flags & MoveCapture) s += 'x';
        s += squareName(m.to);
    }
    Position next(*this);
    next.makeMove(m);
    if (next.inCheck()) s += next.hasLegalMove() ? '+' : '#';
    return s;
}

Move Position::parseSANStrict(const std::string& s) const {
    if (s.empty()) return Move{};
    Move buf[kMaxMoves];
    const int n = generateLegal(buf);

    // Castling: O-O, 0-0, o-o, OO, O-O-O ...
    {
        std::string c;
        for (char ch : s) c += (ch == '0' || ch == 'o') ? 'O' : ch;
        const bool king = c == "O-O" || c == "OO", queen = c == "O-O-O" || c == "OOO";
        if (king || queen) {
            for (int i = 0; i < n; ++i)
                if (buf[i].flags & (king ? MoveCastleKing : MoveCastleQueen)) return buf[i];
            return Move{};
        }
    }

    PieceType piece = Pawn;
    std::string body = s;
    if (inSet("NBRQK", body[0])) {
        piece = pieceFromChar(body[0]);
        body.erase(0, 1);
    }
    PieceType promo = NoPiece;
    if (piece == Pawn) {
        if (!body.empty() && body.back() == ')') body.pop_back();
        if (body.size() >= 3) {
            const PieceType pt = pieceFromChar(body.back());
            const char prev = body[body.size() - 2];
            if (isPromotionPiece(pt) && ((prev >= '1' && prev <= '8') || prev == '=' || prev == '(' || prev == '/')) {
                promo = pt;
                body.pop_back();
                while (!body.empty() && (body.back() == '=' || body.back() == '(' || body.back() == '/')) body.pop_back();
            }
        }
    }
    std::string core;
    for (char ch : body)
        if (ch != 'x' && ch != 'X' && ch != ':' && ch != '-') core += ch;
    if (core.size() < 2 || core.size() > 4) return Move{};
    const Square to = parseSquare(core.substr(core.size() - 2));
    if (to == NoSquare) return Move{};
    int dFile = -1, dRank = -1;
    for (size_t i = 0; i + 2 < core.size(); ++i) {
        const char ch = core[i];
        if (ch >= 'a' && ch <= 'h' && dFile < 0) dFile = ch - 'a';
        else if (ch >= '1' && ch <= '8' && dRank < 0) dRank = ch - '1';
        else return Move{};
    }
    Move found;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const Move& m = buf[i];
        if (m.to != to || board_[m.from].type != piece) continue;
        if (dFile >= 0 && fileOf(m.from) != dFile) continue;
        if (dRank >= 0 && rankOf(m.from) != dRank) continue;
        if (m.promotion != promo) continue;
        found = m;
        ++count;
    }
    return count == 1 ? found : Move{};
}

Move Position::parseSAN(const std::string& in) const {
    std::string s;
    for (char c : in)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') s += c;
    // Leading move number: "12.", "12...".
    {
        size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i > 0 && i < s.size() && s[i] == '.') {
            while (i < s.size() && s[i] == '.') ++i;
            s.erase(0, i);
        }
    }
    // Trailing annotations.
    for (bool again = true; again && !s.empty();) {
        again = false;
        if (inSet("+#!?", s.back())) {
            s.pop_back();
            again = true;
        } else if (s.size() > 4 && s.compare(s.size() - 4, 4, "e.p.") == 0) {
            s.erase(s.size() - 4);
            again = true;
        }
    }
    if (s.empty()) return Move{};
    Move m = parseSANStrict(s);
    if (m.valid()) return m;
    // Plain UCI before the lowercase piece letter, so that "b1d2" is the knight and not "B1d2".
    m = parseUCI(s);
    if (m.valid()) return m;
    // Lowercase piece letter ("nf3", "bb5" when no pawn move matches).
    if (inSet("nbrqk", s[0])) {
        std::string u = s;
        u[0] = char(u[0] - 'a' + 'A');
        return parseSANStrict(u);
    }
    return Move{};
}

// ---- Perft ----------------------------------------------------------------------------------

uint64_t Position::perftRec(int depth) const {
    Move buf[kMaxMoves];
    const int n = generateLegal(buf);
    if (depth <= 1) return uint64_t(n);
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) {
        Position next(*this);
        next.makeMove(buf[i]);
        total += next.perftRec(depth - 1);
    }
    return total;
}

uint64_t Position::perft(int depth) const { return depth <= 0 ? 1 : perftRec(depth); }

}  // namespace chess
