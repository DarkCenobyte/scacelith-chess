// Unit tests for src/chess: move generation (perft), notation, game endings, clock, arbiter.
#include "test.h"
#include "chess/chess.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <vector>

using namespace chess;

namespace {

const char* kStartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const char* kKiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
const char* kPos3 = "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1";
const char* kPos4 = "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1";
const char* kPos4Mirror = "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1";
const char* kPos5 = "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8";

Square sq(const char* s) { return parseSquare(s); }

Position fromFEN(const char* fen) {
    Position p;
    bool ok = p.setFEN(fen);
    if (!ok) std::fprintf(stderr, "  invalid FEN in test: %s\n", fen);
    CHECK(ok);
    return p;
}

void checkPerft(const char* fen, const std::vector<uint64_t>& counts) {
    Position p = fromFEN(fen);
    for (size_t d = 0; d < counts.size(); ++d) {
        const uint64_t n = p.perft(int(d + 1));
        if (n != counts[d]) std::fprintf(stderr, "  perft(%d) of %s\n", int(d + 1), fen);
        CHECK_EQ(n, counts[d]);
    }
}

bool playSAN(Game& g, const char* san) {
    const Move m = g.position().parseSAN(san);
    if (!m.valid()) {
        std::fprintf(stderr, "  cannot parse SAN '%s' in %s\n", san, g.position().fen().c_str());
        return false;
    }
    return g.play(m);
}

bool playLine(Game& g, const std::vector<const char*>& sans) {
    for (const char* s : sans)
        if (!playSAN(g, s)) return false;
    return true;
}

bool playUCI(Game& g, const char* uci) { return g.play(g.position().parseUCI(uci)); }

std::string sanOf(const char* fen, const char* uci) {
    Position p = fromFEN(fen);
    const Move m = p.parseUCI(uci);
    CHECK(m.valid());
    return p.toSAN(m);
}

std::string parsedUCI(const char* fen, const char* san) {
    Position p = fromFEN(fen);
    const Move m = p.parseSAN(san);
    return m.valid() ? p.toUCI(m) : std::string("invalid");
}

struct Rng {
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

TimeControl preset(const char* label) {
    for (const TimeControl& t : timeControlPresets())
        if (t.label() == label) return t;
    CHECK(false);
    return TimeControl{};
}

}  // namespace

// ---- Squares / FEN ------------------------------------------------------------------------------

TEST(chess_square_names) {
    CHECK_EQ(squareName(0), "a1");
    CHECK_EQ(squareName(28), "e4");
    CHECK_EQ(squareName(63), "h8");
    CHECK_EQ(parseSquare("e4"), 28);
    CHECK_EQ(parseSquare("h8"), 63);
    CHECK_EQ(parseSquare("i1"), NoSquare);
    CHECK_EQ(parseSquare("a9"), NoSquare);
    CHECK_EQ(parseSquare("e"), NoSquare);
    CHECK_EQ(makeSquare(4, 3), 28);
    CHECK_EQ(fileOf(28), 4);
    CHECK_EQ(rankOf(28), 3);
}

TEST(chess_fen_roundtrip) {
    const char* fens[] = {
        kStartFEN, kKiwipete, kPos3, kPos4, kPos4Mirror, kPos5,
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
        "rnbqkbnr/pppp1ppp/8/8/3Pp3/8/PPP1PPPP/RNBQKBNR b KQkq d3 0 3",
        "8/8/8/8/8/8/8/K6k w - - 99 150",
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1",
        "r3k3/8/8/8/8/8/8/4K3 b q - 5 40",
    };
    for (const char* f : fens) {
        Position p = fromFEN(f);
        CHECK_EQ(p.fen(), std::string(f));
        Position q = fromFEN(p.fen().c_str());
        CHECK_EQ(q.hash(), p.hash());
        CHECK(q.samePosition(p));
    }
    Position start;
    CHECK_EQ(start.fen(), std::string(kStartFEN));
    CHECK_EQ(start.hash(), fromFEN(kStartFEN).hash());
    // Optional move counters.
    Position p;
    CHECK(p.setFEN("4k3/8/8/8/8/8/8/4K3 w - -"));
    CHECK_EQ(p.fen(), "4k3/8/8/8/8/8/8/4K3 w - - 0 1");
}

TEST(chess_fen_invalid_and_normalised) {
    Position p;
    const std::string before = p.fen();
    CHECK(!p.setFEN(""));
    CHECK(!p.setFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1"));          // 7 ranks
    CHECK(!p.setFEN("rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));  // bad digit
    CHECK(!p.setFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1"));  // bad side
    CHECK(!p.setFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkx - 0 1"));  // bad castling
    CHECK(!p.setFEN("8/8/8/8/8/8/8/K7 w - - 0 1"));                                // no black king
    CHECK(!p.setFEN("k7/8/8/8/8/8/8/KK6 w - - 0 1"));                              // two white kings
    CHECK(!p.setFEN("P3k3/8/8/8/8/8/8/4K3 w - - 0 1"));                            // pawn on the 8th rank
    CHECK(p.setFEN("4k3/8/8/8/8/8/8/4K2R b - - 0 1"));                             // legal (sanity)
    CHECK(!p.setFEN("4k2R/8/8/8/8/8/8/4K3 w - - 0 1"));                            // side not to move in check
    CHECK(!p.setFEN("4k3/8/8/8/8/8/8/4K3 w - - x 1"));                             // bad counter
    p.setStart();
    CHECK_EQ(p.fen(), before);
    // Castling rights without king/rook on their squares are dropped.
    CHECK(p.setFEN("4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1"));
    CHECK_EQ(p.castling(), 0);
    CHECK(p.setFEN("r3k3/8/8/8/8/8/8/4K2R w KQkq - 0 1"));
    CHECK_EQ(p.fen(), "r3k3/8/8/8/8/8/8/4K2R w Kq - 0 1");
    // En passant square kept only when a capture is legal.
    CHECK(p.setFEN("rnbqkbnr/ppp1pppp/8/3p4/8/8/PPPPPPPP/RNBQKBNR w KQkq d6 0 2"));
    CHECK_EQ(p.epSquare(), NoSquare);
    CHECK_EQ(p.fen(), "rnbqkbnr/ppp1pppp/8/3p4/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 2");
    // Pinned capturer (rank pin through both pawns): ep capture illegal -> not stored.
    CHECK(p.setFEN("4k3/8/8/KPp4r/8/8/8/8 w - c6 0 2"));
    CHECK_EQ(p.epSquare(), NoSquare);
    CHECK(p.setFEN("4k3/8/8/1Pp5/8/8/8/K7 w - c6 0 2"));
    CHECK_EQ(p.epSquare(), sq("c6"));
}

TEST(chess_ep_square_only_when_capturable) {
    Game g;
    CHECK(playSAN(g, "e4"));
    CHECK_EQ(g.position().epSquare(), NoSquare);  // no black pawn can capture
    CHECK_EQ(g.position().fen(), "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    CHECK(playLine(g, {"Nf6", "e5", "d5"}));
    CHECK_EQ(g.position().epSquare(), sq("d6"));
    CHECK_EQ(g.position().fen(), "rnbqkb1r/ppp1pppp/5n2/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");
    // A double push whose ep capture would expose the king is not an en passant possibility.
    CHECK(g.resetFromFEN("4k3/2p5/8/KP5r/8/8/8/8 b - - 0 1"));
    CHECK(playUCI(g, "c7c5"));
    CHECK_EQ(g.position().epSquare(), NoSquare);
    CHECK(!g.position().parseUCI("b5c6").valid());
}

// ---- Perft --------------------------------------------------------------------------------------

TEST(chess_perft_startpos) {
    const auto t0 = std::chrono::steady_clock::now();
    checkPerft(kStartFEN, {20, 400, 8902, 197281, 4865609});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000.0);  // typically ~50 ms in Release
    CHECK_EQ(Position().perft(0), 1u);
}

TEST(chess_perft_kiwipete) { checkPerft(kKiwipete, {48, 2039, 97862, 4085603}); }

TEST(chess_perft_position3) { checkPerft(kPos3, {14, 191, 2812, 43238, 674624}); }

TEST(chess_perft_position4) {
    checkPerft(kPos4, {6, 264, 9467, 422333});
    checkPerft(kPos4Mirror, {6, 264, 9467, 422333});
}

TEST(chess_perft_position5) { checkPerft(kPos5, {44, 1486, 62379, 2103487}); }

TEST(chess_perft_edge_cases) {
    struct Case { const char* fen; int depth; uint64_t nodes; };
    const Case cases[] = {
        {"3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1", 6, 1134888},        // illegal ep #1
        {"8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1", 6, 1015133},       // illegal ep #2
        {"8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", 6, 1440467},      // ep capture checks opponent
        {"5k2/8/8/8/8/8/8/4K2R w K - 0 1", 6, 661072},            // short castling gives check
        {"3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", 6, 803711},            // long castling gives check
        {"r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", 4, 1274206}, // castle rights
        {"r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1", 4, 1720476}, // castling prevented
        {"2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1", 6, 3821001},        // promote out of check
        {"8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1", 5, 1004658},      // discovered check
        {"4k3/1P6/8/8/8/8/K7/8 w - - 0 1", 6, 217342},            // promote to give check
        {"8/P1k5/K7/8/8/8/8/8 w - - 0 1", 6, 92683},              // under-promote to give check
        {"K1k5/8/P7/8/8/8/8/8 w - - 0 1", 6, 2217},               // self stalemate
        {"8/k1P5/8/1K6/8/8/8/8 w - - 0 1", 7, 567584},            // stalemate & checkmate
        {"8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1", 4, 23527},          // stalemate & checkmate
    };
    for (const Case& c : cases) {
        const uint64_t n = fromFEN(c.fen).perft(c.depth);
        if (n != c.nodes) std::fprintf(stderr, "  perft(%d) of %s\n", c.depth, c.fen);
        CHECK_EQ(n, c.nodes);
    }
}

// ---- Move queries -------------------------------------------------------------------------------

TEST(chess_legal_move_queries) {
    Position p;
    CHECK_EQ(p.legalMoves().size(), 20u);
    CHECK_EQ(p.legalMovesFrom(sq("g1")).size(), 2u);
    CHECK_EQ(p.legalMovesFrom(sq("a1")).size(), 0u);
    CHECK_EQ(p.legalMovesFrom(sq("e7")).size(), 0u);  // not the side to move
    const Move m = p.findLegal(sq("e2"), sq("e4"));
    CHECK(m.valid());
    CHECK(m.flags & MoveDoublePush);
    CHECK(p.isLegal(m));
    CHECK(!p.findLegal(sq("e2"), sq("e5")).valid());
    CHECK(!p.inCheck());
    CHECK_EQ(p.kingSquare(White), sq("e1"));
    CHECK_EQ(p.kingSquare(Black), sq("e8"));
    CHECK(p.isAttacked(sq("f3"), White));
    CHECK(!p.isAttacked(sq("e4"), White));
    // Castling generation: rights, path, check.
    Position c = fromFEN("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    CHECK(c.findLegal(sq("e1"), sq("g1")).flags & MoveCastleKing);
    CHECK(c.findLegal(sq("e1"), sq("c1")).flags & MoveCastleQueen);
    Position through = fromFEN("r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1");  // f1 attacked
    CHECK(!through.findLegal(sq("e1"), sq("g1")).valid());
    CHECK(through.findLegal(sq("e1"), sq("c1")).valid());
    Position b1attacked = fromFEN("r3k2r/8/8/8/8/8/1r6/R3K2R w KQkq - 0 1");  // b1 attacked: O-O-O still legal
    CHECK(b1attacked.findLegal(sq("e1"), sq("c1")).valid());
    Position inCheck = fromFEN("r3k2r/8/8/8/8/8/4r3/R3K2R w KQkq - 0 1");
    CHECK(inCheck.inCheck());
    CHECK(!inCheck.findLegal(sq("e1"), sq("g1")).valid());
    CHECK(!inCheck.findLegal(sq("e1"), sq("c1")).valid());
    Position blocked = fromFEN("r3k2r/8/8/8/8/8/8/RN2K1NR w KQkq - 0 1");
    CHECK(!blocked.findLegal(sq("e1"), sq("g1")).valid());
    CHECK(!blocked.findLegal(sq("e1"), sq("c1")).valid());
    // Promotions need a piece.
    Position promo = fromFEN("8/4P3/8/8/8/8/k7/4K3 w - - 0 1");
    CHECK(!promo.findLegal(sq("e7"), sq("e8")).valid());
    for (PieceType t : {Queen, Rook, Bishop, Knight}) {
        const Move pm = promo.findLegal(sq("e7"), sq("e8"), t);
        CHECK(pm.valid());
        CHECK_EQ(pm.promotion, t);
        CHECK(pm.flags & MovePromotion);
    }
    CHECK_EQ(promo.legalMovesFrom(sq("e7")).size(), 4u);
    // Checkmate / stalemate predicates.
    CHECK(fromFEN("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3").isCheckmate());
    CHECK(fromFEN("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1").isStalemate());
    // makeMove re-derives flags: a bare from/to Move works for castling and en passant.
    Position e = fromFEN("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
    Move bare;
    bare.from = sq("e5");
    bare.to = sq("f6");
    e.makeMove(bare);
    CHECK(e.at(sq("f5")).empty());
    CHECK_EQ(e.at(sq("f6")).type, Pawn);
}

// ---- SAN / UCI ----------------------------------------------------------------------------------

TEST(chess_san_output) {
    CHECK_EQ(sanOf(kStartFEN, "g1f3"), "Nf3");
    CHECK_EQ(sanOf(kStartFEN, "e2e4"), "e4");
    const char* knights = "rnbqkb1r/ppp1pppp/5n2/3p4/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1";
    CHECK_EQ(sanOf(knights, "b8d7"), "Nbd7");
    CHECK_EQ(sanOf(knights, "f6d7"), "Nfd7");
    CHECK_EQ(sanOf(knights, "b8c6"), "Nc6");
    const char* rooks = "4k3/8/8/R7/8/8/8/R3K3 w - - 0 1";
    CHECK_EQ(sanOf(rooks, "a1a3"), "R1a3");
    CHECK_EQ(sanOf(rooks, "a5a3"), "R5a3");
    CHECK_EQ(sanOf(rooks, "a1d1"), "Rd1");
    CHECK_EQ(sanOf(rooks, "a5a8"), "Ra8+");
    const char* queens = "1k6/8/8/8/4Q2Q/8/K7/7Q w - - 0 1";
    CHECK_EQ(sanOf(queens, "h4e1"), "Qh4e1");
    CHECK_EQ(sanOf(queens, "e4e1"), "Qee1");
    CHECK_EQ(sanOf(queens, "h1e1"), "Q1e1");
    const char* knightsFile = "4k3/8/8/8/8/N7/8/N3K3 w - - 0 1";
    CHECK_EQ(sanOf(knightsFile, "a1c2"), "N1c2");
    CHECK_EQ(sanOf(knightsFile, "a3c2"), "N3c2");
    const char* promo = "k2r4/4P3/8/8/8/8/8/4K3 w - - 0 1";
    CHECK_EQ(sanOf(promo, "e7d8q"), "exd8=Q+");
    CHECK_EQ(sanOf(promo, "e7d8r"), "exd8=R+");
    CHECK_EQ(sanOf(promo, "e7d8b"), "exd8=B");
    CHECK_EQ(sanOf(promo, "e7d8n"), "exd8=N");
    CHECK_EQ(sanOf(promo, "e7e8q"), "e8=Q");
    CHECK_EQ(sanOf("4rkr1/4p1p1/8/8/8/8/8/4K2R w K - 0 1", "e1g1"), "O-O#");
    CHECK_EQ(sanOf("r3k3/8/8/8/8/8/8/R3K3 w Qq - 0 1", "e1c1"), "O-O-O");
    CHECK_EQ(sanOf("r3k3/8/8/8/8/8/8/4K3 b q - 0 1", "e8c8"), "O-O-O");
    CHECK_EQ(sanOf("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3", "e5f6"), "exf6");
    CHECK_EQ(sanOf("rnbqkbnr/ppp2ppp/3p4/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3", "f1b5"), "Bb5+");
    CHECK_EQ(sanOf("4k3/8/8/2p1p3/3P4/8/8/4K3 w - - 0 1", "d4c5"), "dxc5");
    CHECK_EQ(sanOf("4k3/8/8/2p1p3/3P4/8/8/4K3 w - - 0 1", "d4e5"), "dxe5");
    CHECK_EQ(sanOf("rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2", "d8h4"), "Qh4#");
    // Pinned piece does not count for disambiguation (only legal moves do).
    CHECK_EQ(sanOf("4k3/8/8/8/1b6/8/3N4/4K1N1 w - - 0 1", "g1f3"), "Nf3");
    // Illegal move -> empty.
    Position p;
    Move bogus;
    bogus.from = sq("e2");
    bogus.to = sq("e5");
    CHECK_EQ(p.toSAN(bogus), "");
    CHECK_EQ(p.toUCI(p.findLegal(sq("e2"), sq("e4"))), "e2e4");
    CHECK_EQ(fromFEN(promo).toUCI(fromFEN(promo).findLegal(sq("e7"), sq("e8"), Knight)), "e7e8n");
}

TEST(chess_san_parse_tolerant) {
    CHECK_EQ(parsedUCI(kStartFEN, "Nf3"), "g1f3");
    CHECK_EQ(parsedUCI(kStartFEN, "nf3"), "g1f3");
    CHECK_EQ(parsedUCI(kStartFEN, "Ng1f3"), "g1f3");
    CHECK_EQ(parsedUCI(kStartFEN, "Ng1-f3"), "g1f3");
    CHECK_EQ(parsedUCI(kStartFEN, "g1f3"), "g1f3");
    CHECK_EQ(parsedUCI(kStartFEN, "Nf3+"), "g1f3");
    CHECK_EQ(parsedUCI(kStartFEN, "1.e4"), "e2e4");
    CHECK_EQ(parsedUCI(kStartFEN, " e4!? "), "e2e4");
    CHECK_EQ(parsedUCI(kStartFEN, "e2-e4"), "e2e4");
    CHECK_EQ(parsedUCI(kStartFEN, "e5"), "invalid");
    CHECK_EQ(parsedUCI(kStartFEN, "Nf6"), "invalid");
    CHECK_EQ(parsedUCI(kStartFEN, ""), "invalid");
    CHECK_EQ(parsedUCI(kStartFEN, "xyz"), "invalid");
    CHECK_EQ(parsedUCI(kStartFEN, "O-O"), "invalid");
    const char* castle = "4rkr1/4p1p1/8/8/8/8/8/4K2R w K - 0 1";
    CHECK_EQ(parsedUCI(castle, "O-O"), "e1g1");
    CHECK_EQ(parsedUCI(castle, "0-0"), "e1g1");
    CHECK_EQ(parsedUCI(castle, "O-O#"), "e1g1");
    CHECK_EQ(parsedUCI(castle, "0-0+"), "e1g1");
    CHECK_EQ(parsedUCI(castle, "o-o"), "e1g1");
    CHECK_EQ(parsedUCI(castle, "O-O-O"), "invalid");
    const char* promo = "k2r4/4P3/8/8/8/8/8/4K3 w - - 0 1";
    CHECK_EQ(parsedUCI(promo, "e8Q"), "e7e8q");
    CHECK_EQ(parsedUCI(promo, "e8=Q"), "e7e8q");
    CHECK_EQ(parsedUCI(promo, "e8(Q)"), "e7e8q");
    CHECK_EQ(parsedUCI(promo, "e8q"), "e7e8q");
    CHECK_EQ(parsedUCI(promo, "e8=Q+"), "e7e8q");
    CHECK_EQ(parsedUCI(promo, "exd8=N"), "e7d8n");
    CHECK_EQ(parsedUCI(promo, "exd8Q+"), "e7d8q");
    CHECK_EQ(parsedUCI(promo, "ed8R"), "e7d8r");
    CHECK_EQ(parsedUCI(promo, "e7e8n"), "e7e8n");
    CHECK_EQ(parsedUCI(promo, "e8"), "invalid");
    CHECK_EQ(parsedUCI(promo, "e8=K"), "invalid");
    const char* ep = "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3";
    CHECK_EQ(parsedUCI(ep, "exf6"), "e5f6");
    CHECK_EQ(parsedUCI(ep, "exf6e.p."), "e5f6");
    CHECK_EQ(parsedUCI(ep, "exf6 e.p."), "e5f6");
    CHECK_EQ(parsedUCI(ep, "ef6"), "e5f6");
    const char* knights = "rnbqkb1r/ppp1pppp/5n2/3p4/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1";
    CHECK_EQ(parsedUCI(knights, "Nd7"), "invalid");  // ambiguous
    CHECK_EQ(parsedUCI(knights, "Nbd7"), "b8d7");
    CHECK_EQ(parsedUCI(knights, "N8d7"), "b8d7");
    CHECK_EQ(parsedUCI(knights, "Nfd7"), "f6d7");
    const char* queens = "1k6/8/8/8/4Q2Q/8/K7/7Q w - - 0 1";
    CHECK_EQ(parsedUCI(queens, "Qh4e1"), "h4e1");
    CHECK_EQ(parsedUCI(queens, "Qh4xe1"), "h4e1");
    CHECK_EQ(parsedUCI(queens, "Qe1"), "invalid");
    CHECK_EQ(parsedUCI(queens, "Qhe1"), "invalid");  // h4 and h1
    CHECK_EQ(parsedUCI(queens, "Q1e1"), "h1e1");
    // UCI parsing strictness.
    Position p;
    CHECK(p.parseUCI("e2e4").valid());
    CHECK(!p.parseUCI("e2e5").valid());
    CHECK(!p.parseUCI("e2e4q").valid());
    CHECK(!p.parseUCI("e2").valid());
    CHECK(!p.parseUCI("z9e4").valid());
}

TEST(chess_san_uci_fen_roundtrip_random_games) {
    const char* starts[] = {kStartFEN, kKiwipete, kPos3, kPos4, kPos5, "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1",
                            "8/P1k5/K7/8/8/8/8/8 w - - 0 1", "4k3/1P6/8/8/8/8/K7/8 w - - 0 1"};
    Rng rng{0x9E3779B97F4A7C15ULL};
    int checkedMoves = 0, games = 0;
    for (int round = 0; round < 6; ++round) {
        for (const char* start : starts) {
            Game g;
            CHECK(g.resetFromFEN(start));
            ++games;
            for (int ply = 0; ply < 160 && !g.isOver(); ++ply) {
                const Position& p = g.position();
                // FEN round trip and incremental hash vs recomputed hash.
                Position q;
                CHECK(q.setFEN(p.fen()));
                CHECK_EQ(q.fen(), p.fen());
                CHECK_EQ(q.hash(), p.hash());
                const std::vector<Move> moves = p.legalMoves();
                std::set<std::string> sans;
                for (const Move& m : moves) {
                    const std::string san = p.toSAN(m);
                    sans.insert(san);
                    const Move back = p.parseSAN(san);
                    CHECK(back == m);
                    CHECK_EQ(back.flags, m.flags);
                    const Move u = p.parseUCI(p.toUCI(m));
                    CHECK(u == m);
                    // Check / mate suffix agrees with the resulting position.
                    Position n(p);
                    n.makeMove(m);
                    const char last = san.back();
                    if (n.inCheck()) CHECK(last == (n.hasLegalMove() ? '+' : '#'));
                    else CHECK(last != '+' && last != '#');
                    ++checkedMoves;
                }
                CHECK_EQ(sans.size(), moves.size());  // SAN is unambiguous
                if (moves.empty()) break;
                CHECK(g.play(moves[size_t(rng.next() % moves.size())]));
            }
            // Replaying the UCI / SAN records reproduces the game.
            Game replay;
            CHECK(replay.resetFromFEN(start));
            for (const std::string& u : g.uciMoves()) CHECK(replay.play(replay.position().parseUCI(u)));
            CHECK_EQ(replay.position().fen(), g.position().fen());
            Position sanReplay = fromFEN(start);
            for (const std::string& s : g.sanMoves()) {
                const Move m = sanReplay.parseSAN(s);
                CHECK(m.valid());
                sanReplay.makeMove(m);
            }
            CHECK_EQ(sanReplay.fen(), g.position().fen());
        }
    }
    CHECK(games == 48);
    CHECK(checkedMoves > 10000);
}

// ---- Game endings -------------------------------------------------------------------------------

TEST(chess_game_checkmate_and_record) {
    Game g;
    CHECK(playLine(g, {"f3", "e5", "g4", "Qh4#"}));
    CHECK(g.status() == GameStatus::BlackWins);
    CHECK(g.endReason() == GameEndReason::Checkmate);
    CHECK_EQ(std::string(g.resultString()), "0-1");
    CHECK_EQ(g.sanMoves().size(), 4u);
    CHECK_EQ(g.sanMoves()[3], "Qh4#");
    CHECK_EQ(g.uciMoves()[3], "d8h4");
    CHECK(!playUCI(g, "a2a3"));  // game over
    CHECK_EQ(g.moves().size(), 4u);
    CHECK_EQ(std::string(endReasonText(GameEndReason::Checkmate)), "Checkmate");
    CHECK_EQ(std::string(endReasonText(GameEndReason::ThreefoldClaim)), "Threefold repetition (claimed)");
    g.reset();
    CHECK(g.status() == GameStatus::Ongoing);
    CHECK(g.moves().empty());
    CHECK(!playUCI(g, "e2e5"));  // illegal
    CHECK(g.moves().empty());
    CHECK(!g.play(Move{}));
}

TEST(chess_game_stalemate) {
    Game g;
    CHECK(g.resetFromFEN("7k/8/6K1/8/8/8/8/5Q2 w - - 0 1"));
    CHECK(playUCI(g, "f1f7"));
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::Stalemate);
    CHECK_EQ(std::string(g.resultString()), "1/2-1/2");
}

TEST(chess_game_dead_positions) {
    Game g;
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/3q4/4K3 w - - 0 1"));
    CHECK(playSAN(g, "Kxd2"));  // K v K
    CHECK(g.endReason() == GameEndReason::InsufficientMaterial);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/3n4/2B1K3 w - - 0 1"));
    CHECK(playSAN(g, "Bxd2"));  // K+B v K
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::InsufficientMaterial);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/3b4/1N2K3 w - - 0 1"));
    CHECK(playSAN(g, "Nxd2"));  // K+N v K
    CHECK(g.endReason() == GameEndReason::InsufficientMaterial);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/2b5/3n4/2B1K3 w - - 0 1"));
    CHECK(playSAN(g, "Bxd2"));  // K+B v K+B, both bishops on dark squares
    CHECK(g.endReason() == GameEndReason::InsufficientMaterial);
    // Dead from the start position of the game.
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/8/4K3 w - - 0 1"));
    CHECK(g.isOver());
    // Not dead: opposite-coloured bishops, knight v knight, two knights, bishop v pawn.
    CHECK(!fromFEN("4k3/8/8/8/8/8/2b5/2B1K3 w - - 0 1").hasInsufficientMaterial());
    CHECK(!fromFEN("4k3/8/8/8/8/8/2n5/2N1K3 w - - 0 1").hasInsufficientMaterial());
    CHECK(!fromFEN("4k3/8/8/8/8/8/8/1NN1K3 w - - 0 1").hasInsufficientMaterial());
    CHECK(!fromFEN("4k3/7p/8/8/8/8/8/2B1K3 w - - 0 1").hasInsufficientMaterial());
    CHECK(!fromFEN("4k3/8/8/8/8/8/2n5/2B1K3 w - - 0 1").hasInsufficientMaterial());
    CHECK(fromFEN("4k3/8/8/8/8/8/8/2B1KB2 w - - 0 1").hasInsufficientMaterial() == false);  // c1 dark, f1 light
    CHECK(fromFEN("4k3/8/8/8/8/8/8/B1B1K3 w - - 0 1").hasInsufficientMaterial());           // a1, c1 both dark
}

TEST(chess_can_color_mate) {
    Position p = fromFEN("4k3/8/8/8/8/8/8/4K2N w - - 0 1");
    CHECK(!p.canColorMate(White));  // lone knight v bare king
    CHECK(!p.canColorMate(Black));  // bare king
    p = fromFEN("4k3/7p/8/8/8/8/8/4K2N w - - 0 1");
    CHECK(p.canColorMate(White));   // helpmate exists: the pawn can block
    CHECK(p.canColorMate(Black));
    p = fromFEN("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(p.canColorMate(White));
    CHECK(!p.canColorMate(Black));
    p = fromFEN("4k3/8/8/8/8/8/8/1NN1K3 w - - 0 1");
    CHECK(p.canColorMate(White));
    p = fromFEN("4k3/8/8/8/8/8/2b5/2B1K3 w - - 0 1");
    CHECK(p.canColorMate(White));
    CHECK(p.canColorMate(Black));
}

TEST(chess_game_repetitions) {
    Game g;
    const std::vector<const char*> cycle = {"Nf3", "Nf6", "Ng1", "Ng8"};
    CHECK_EQ(g.repetitionCount(), 1);
    CHECK(playLine(g, cycle));
    CHECK_EQ(g.repetitionCount(), 2);
    CHECK(!g.canClaimThreefold());
    CHECK(playLine(g, cycle));
    CHECK_EQ(g.repetitionCount(), 3);
    CHECK(g.canClaimThreefold());
    CHECK(g.status() == GameStatus::Ongoing);  // threefold must be claimed
    CHECK(playLine(g, cycle));
    CHECK_EQ(g.repetitionCount(), 4);
    CHECK(g.status() == GameStatus::Ongoing);
    CHECK(playLine(g, {"Nf3", "Nf6", "Ng1"}));
    CHECK(g.status() == GameStatus::Ongoing);
    CHECK(playSAN(g, "Ng8"));
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::FivefoldRepetition);

    // Claiming the threefold repetition.
    Game c;
    CHECK(playLine(c, cycle));
    c.claimDraw();  // nothing to claim yet
    CHECK(c.status() == GameStatus::Ongoing);
    CHECK(playLine(c, cycle));
    c.claimDraw();
    CHECK(c.status() == GameStatus::Draw);
    CHECK(c.endReason() == GameEndReason::ThreefoldClaim);
    CHECK(!c.canClaimThreefold());  // game over

    // En passant possibility makes positions different (FIDE 9.2.3.2).
    Game e;
    CHECK(playLine(e, {"e4", "Nc6", "e5", "d5"}));  // exd6 e.p. possible
    CHECK(playLine(e, {"Nf3", "Nb8", "Ng1", "Nc6"}));
    CHECK_EQ(e.repetitionCount(), 1);
    CHECK(playLine(e, {"Nf3", "Nb8", "Ng1", "Nc6"}));
    CHECK_EQ(e.repetitionCount(), 2);

    // Transpositions reach the same position (and hash).
    Game t1, t2;
    CHECK(playLine(t1, {"Nf3", "Nf6", "Nc3"}));
    CHECK(playLine(t2, {"Nc3", "Nf6", "Nf3"}));
    CHECK(t1.position().samePosition(t2.position()));
    CHECK_EQ(t1.position().hash(), t2.position().hash());
    CHECK(t1.position().hash() != Position().hash());

    // Castling rights make positions different.
    Game r;
    CHECK(playLine(r, {"Nf3", "Nf6", "Rg1", "Rg8", "Rh1", "Rh8"}));
    CHECK_EQ(r.repetitionCount(), 1);  // same placement as after 1.Nf3 Nf6 but no castling rights
    CHECK(playLine(r, {"Rg1", "Rg8", "Rh1", "Rh8"}));
    CHECK_EQ(r.repetitionCount(), 2);
}

TEST(chess_game_move_rules) {
    Game g;
    CHECK(g.resetFromFEN("7k/8/6K1/8/8/8/8/R7 w - - 149 100"));
    CHECK(playSAN(g, "Ra2"));
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::SeventyFiveMoves);
    // Checkmate on the 75th move takes precedence.
    CHECK(g.resetFromFEN("7k/8/6K1/8/8/8/8/R7 w - - 149 100"));
    CHECK(playSAN(g, "Ra8#"));
    CHECK(g.status() == GameStatus::WhiteWins);
    CHECK(g.endReason() == GameEndReason::Checkmate);
    // Fifty-move claim.
    CHECK(g.resetFromFEN("7k/8/6K1/8/8/8/8/R7 w - - 98 60"));
    CHECK(!g.canClaimFiftyMove());
    CHECK(playSAN(g, "Ra2"));
    CHECK(!g.canClaimFiftyMove());
    CHECK(playSAN(g, "Kg8"));
    CHECK(g.canClaimFiftyMove());
    CHECK(g.status() == GameStatus::Ongoing);
    g.claimDraw();
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::FiftyMoveClaim);
    // A pawn move resets the counter.
    CHECK(g.resetFromFEN("7k/8/6K1/8/8/8/P7/R7 w - - 99 60"));
    CHECK(playSAN(g, "a3"));
    CHECK_EQ(g.position().halfmoveClock(), 0);
    CHECK(!g.canClaimFiftyMove());
}

TEST(chess_game_flag_fall) {
    Game g;
    g.flagFall(White);
    CHECK(g.status() == GameStatus::BlackWins);
    CHECK(g.endReason() == GameEndReason::Timeout);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/8/R3K3 w - - 0 1"));
    g.flagFall(White);  // Black has a bare king
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::TimeoutVsInsufficient);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/8/R3K3 b - - 0 1"));
    g.flagFall(Black);
    CHECK(g.status() == GameStatus::WhiteWins);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/8/4KN2 b - - 0 1"));
    CHECK(g.isOver());  // K+N v K is already a dead position
    g.flagFall(Black);  // no effect
    CHECK(g.endReason() == GameEndReason::InsufficientMaterial);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/1p6/4K3 w - - 0 1"));
    g.flagFall(Black);  // White has a bare king
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::TimeoutVsInsufficient);
    CHECK(g.resetFromFEN("4k3/7p/8/8/8/8/8/4KN2 b - - 0 1"));
    g.flagFall(Black);  // K+N v K+P: a mate is possible
    CHECK(g.status() == GameStatus::WhiteWins);
    CHECK(g.endReason() == GameEndReason::Timeout);
}

TEST(chess_game_resign_agree_forfeit) {
    Game g;
    g.resign(White);
    CHECK(g.status() == GameStatus::BlackWins);
    CHECK(g.endReason() == GameEndReason::Resignation);
    g.agreeDraw();  // no effect after the end
    CHECK(g.status() == GameStatus::BlackWins);
    g.reset();
    g.agreeDraw();
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::Agreement);
    g.reset();
    g.forfeitIllegal(Black);
    CHECK(g.status() == GameStatus::WhiteWins);
    CHECK(g.endReason() == GameEndReason::IllegalMoves);
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/8/R3K3 w - - 0 1"));
    g.forfeitIllegal(White);  // Black cannot checkmate
    CHECK(g.status() == GameStatus::Draw);
    CHECK(g.endReason() == GameEndReason::IllegalMovesVsInsufficient);
}

// ---- Taking moves back --------------------------------------------------------------------------

namespace {

// The whole record of two games agrees: positions (FEN, repetition identity), moves, notation,
// status, repetitions, PGN.
bool sameRecord(const Game& a, const Game& b) {
    if (a.moves().size() != b.moves().size() || a.sanMoves() != b.sanMoves() || a.uciMoves() != b.uciMoves()) return false;
    for (size_t i = 0; i < a.moves().size(); ++i)
        if (a.moves()[i] != b.moves()[i] || a.moves()[i].flags != b.moves()[i].flags) return false;
    const PgnTags tags{"Test", "Here", "2026.09.30", "1", "-"};
    return a.position().fen() == b.position().fen() && a.position().samePosition(b.position()) &&
           a.position().hash() == b.position().hash() && a.startPosition().fen() == b.startPosition().fen() &&
           a.status() == b.status() && a.endReason() == b.endReason() && a.repetitionCount() == b.repetitionCount() &&
           a.canClaimThreefold() == b.canClaimThreefold() && a.canClaimFiftyMove() == b.canClaimFiftyMove() &&
           a.pgn("W", "B", tags) == b.pgn("W", "B", tags);
}

// A game started from 'fen' (the standard position when null) with the first 'n' moves of 'g'.
Game prefixOf(const Game& g, size_t n, const char* fen = nullptr) {
    Game p;
    if (fen) CHECK(p.resetFromFEN(fen));
    for (size_t i = 0; i < n; ++i) CHECK(p.play(g.moves()[i]));
    return p;
}

}  // namespace

TEST(chess_game_undo_record) {
    Game g;
    CHECK(!g.undo());      // nothing to take back
    CHECK(!g.undo(0));
    CHECK(playLine(g, {"e4", "d5", "exd5", "Qxd5", "Nc3"}));
    const Game full = g;
    CHECK(!g.undo(-1));
    CHECK(!g.undo(0));
    CHECK(!g.undo(6));     // more than were played: nothing changes
    CHECK(sameRecord(g, full));
    // One move, then the capture and its recapture.
    CHECK(g.undo());
    CHECK(sameRecord(g, prefixOf(full, 4)));
    CHECK_EQ(g.position().fen(), std::string("rnb1kbnr/ppp1pppp/8/3q4/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3"));
    CHECK(g.undo(2));
    CHECK(sameRecord(g, prefixOf(full, 2)));
    CHECK(g.position().at(sq("d5")) == (Piece{Pawn, Black}));
    CHECK_EQ(g.sanMoves().back(), std::string("d5"));
    // Another move in their place.
    CHECK(playSAN(g, "Nf3"));
    CHECK_EQ(g.sanMoves().size(), size_t(3));
    CHECK_EQ(g.uciMoves()[2], std::string("g1f3"));
    // Everything, back to the start.
    CHECK(g.undo(3));
    CHECK(sameRecord(g, Game()));
    CHECK(!g.undo());
    // The arbiter follows a game that went back (reset, as GameScene does, or its own sync).
    Arbiter arb;
    Game h;
    CHECK(playLine(h, {"e4", "e5"}));
    arb.reset(h);
    CHECK(h.undo());
    CHECK(!arb.touch(h, sq("g1")));    // Black to move again
    CHECK(arb.touch(h, sq("e7")));
    CHECK(arb.place(h, sq("e6"), NoPiece));
    CHECK(arb.clockPressed(h, TimeControl{}).legal);
}

TEST(chess_game_undo_special_moves) {
    struct Case {
        const char* fen;
        const char* san;
    };
    const Case cases[] = {
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "O-O"},        // the castling rights come back
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "O-O-O"},
        {"r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "O-O"},
        {"r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "O-O-O"},
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "Rxa8+"},      // a rook capture takes rights from both
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", "exd6"},         // en passant: the ep square comes back
        {"4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 2", "exd3"},
        {"4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", "b8=Q+"},           // promotions
        {"4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", "b8=N"},
        {"r3k3/1P6/8/8/8/8/8/4K3 w q - 0 1", "bxa8=R+"},        // a capturing under-promotion, rights lost
        {"4k3/8/8/8/8/8/6p1/4K2R b K - 0 1", "gxh1=B"},
        {"4k3/8/8/8/8/8/6p1/4K2R b K - 7 30", "Kd7"},          // the move counters come back too
    };
    for (const Case& c : cases) {
        Game g;
        CHECK(g.resetFromFEN(c.fen));
        const Game before = g;
        CHECK(playSAN(g, c.san));
        CHECK(!g.position().samePosition(before.position()));
        CHECK(g.undo());
        CHECK(sameRecord(g, before));
        // The same move again gives the same record.
        Game again = before;
        CHECK(playSAN(g, c.san));
        CHECK(playSAN(again, c.san));
        CHECK(sameRecord(g, again));
    }
    // A double push and the en passant capture after it, both taken back: the capture is possible
    // again, then no more.
    Game e;
    CHECK(playLine(e, {"e4", "a6", "e5", "d5", "exd6"}));
    CHECK(e.undo());
    CHECK_EQ(e.position().epSquare(), sq("d6"));
    CHECK((e.position().findLegal(sq("e5"), sq("d6")).flags & MoveEnPassant) != 0);
    CHECK(e.undo());
    CHECK_EQ(e.position().epSquare(), NoSquare);
    CHECK(playSAN(e, "d6"));
    CHECK_EQ(e.position().epSquare(), NoSquare);
}

TEST(chess_game_undo_endings) {
    // Checkmate taken back: the game goes on and the mating side may play another move.
    Game m;
    CHECK(playLine(m, {"f3", "e5", "g4", "Qh4#"}));
    CHECK(m.isOver());
    CHECK(m.undo());
    CHECK(m.status() == GameStatus::Ongoing);
    CHECK(m.endReason() == GameEndReason::None);
    CHECK_EQ(std::string(m.resultString()), std::string("*"));
    CHECK(playSAN(m, "Qg5"));
    CHECK(!m.isOver());
    // Stalemate, dead position.
    Game s;
    CHECK(s.resetFromFEN("7k/8/6K1/8/8/8/8/5Q2 w - - 0 1"));
    CHECK(playUCI(s, "f1f7"));
    CHECK(s.endReason() == GameEndReason::Stalemate);
    CHECK(s.undo());
    CHECK(!s.isOver());
    CHECK(s.resetFromFEN("4k3/8/8/8/8/8/3q4/4K3 w - - 0 1"));
    CHECK(playSAN(s, "Kxd2"));
    CHECK(s.endReason() == GameEndReason::InsufficientMaterial);
    CHECK(s.undo());
    CHECK(!s.isOver());
    CHECK(s.position().at(sq("d2")) == (Piece{Queen, Black}));
    // Fivefold repetition: the occurrences are counted again from the positions left.
    Game r;
    const std::vector<const char*> cycle = {"Nf3", "Nf6", "Ng1", "Ng8"};
    for (int i = 0; i < 4; ++i) CHECK(playLine(r, cycle));
    CHECK(r.endReason() == GameEndReason::FivefoldRepetition);
    CHECK(r.undo());
    CHECK(!r.isOver());
    CHECK_EQ(r.repetitionCount(), 4);  // the position after ...Ng1 (Nf3 Nf6 Ng1), seen four times
    CHECK(r.undo(3));
    CHECK_EQ(r.repetitionCount(), 4);  // the start position, four times
    CHECK(r.canClaimThreefold());
    CHECK(r.undo(4));
    CHECK_EQ(r.repetitionCount(), 3);
    CHECK(r.undo(4));
    CHECK_EQ(r.repetitionCount(), 2);
    CHECK(!r.canClaimThreefold());
    CHECK(playLine(r, cycle));
    CHECK_EQ(r.repetitionCount(), 3);
    r.claimDraw();
    CHECK(r.endReason() == GameEndReason::ThreefoldClaim);
    // A claimed draw, a resignation or a flag fall is lifted as well: the game is where the
    // position puts it.
    CHECK(r.undo());
    CHECK(!r.isOver());
    Game q;
    CHECK(playLine(q, {"e4", "e5"}));
    q.resign(White);
    CHECK(q.isOver());
    CHECK(q.undo());
    CHECK(!q.isOver());
    q.flagFall(Black);
    CHECK(q.isOver());
    CHECK(q.undo());
    CHECK(!q.isOver());
    CHECK(q.moves().empty());
    // The 75-move rule, and the fifty-move claim that follows the counter.
    Game f;
    CHECK(f.resetFromFEN("7k/8/6K1/8/8/8/8/R7 w - - 148 100"));
    CHECK(playLine(f, {"Ra2", "Kg8"}));
    CHECK(f.endReason() == GameEndReason::SeventyFiveMoves);
    CHECK(f.undo());
    CHECK(!f.isOver());
    CHECK(f.canClaimFiftyMove());
    CHECK_EQ(f.position().halfmoveClock(), 149);
    // A game over from its start position (no move to take back) stays over.
    Game d;
    CHECK(d.resetFromFEN("4k3/8/8/8/8/8/8/4K3 w - - 0 1"));
    CHECK(!d.undo());
    CHECK(d.isOver());
}

TEST(chess_game_undo_random_games) {
    // Random games from several start positions: taking k moves back gives the record of the game
    // replayed to that point, and the game goes on from there.
    const char* starts[] = {nullptr, kKiwipete, kPos3, kPos4, kPos5, "8/P1k5/K7/8/8/8/8/8 w - - 0 1",
                            "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1"};
    Rng rng{0xD1B54A32D192ED03ULL};
    int undone = 0, endingsLifted = 0;
    for (int round = 0; round < 8; ++round) {
        for (const char* start : starts) {
            Game g;
            if (start) CHECK(g.resetFromFEN(start));
            for (int step = 0; step < 200; ++step) {
                if (!g.moves().empty() && (g.isOver() || rng.next() % 6 == 0)) {
                    const size_t n = g.moves().size();
                    const int k = 1 + int(rng.next() % std::min<size_t>(n, 6));
                    if (g.isOver()) ++endingsLifted;
                    const Game full = g;
                    CHECK(g.undo(k));
                    CHECK(sameRecord(g, prefixOf(full, n - size_t(k), start)));
                    undone += k;
                    continue;
                }
                const std::vector<Move> moves = g.position().legalMoves();
                if (moves.empty()) break;
                CHECK(g.play(moves[size_t(rng.next() % moves.size())]));
            }
        }
    }
    CHECK(undone > 1000);
    CHECK(endingsLifted > 3);
}

TEST(chess_game_pgn) {
    Game g;
    CHECK(playLine(g, {"f3", "e5", "g4", "Qh4#"}));
    const std::string pgn = g.pgn("Alice", "Robot \"B\"");
    for (const char* tag : {"[Event \"", "[Site \"", "[Date \"", "[Round \"", "[White \"Alice\"]", "[Black \"Robot \\\"B\\\"\"]",
                            "[Result \"0-1\"]", "[TimeControl \"", "[Termination \"normal\"]"})
        CHECK(pgn.find(tag) != std::string::npos);
    CHECK(pgn.find("1. f3 e5 2. g4 Qh4# {Checkmate} 0-1") != std::string::npos);
    CHECK(pgn.find("[FEN") == std::string::npos);
    // Seven Tag Roster order.
    CHECK(pgn.find("[Event") < pgn.find("[Site"));
    CHECK(pgn.find("[Site") < pgn.find("[Date"));
    CHECK(pgn.find("[Date") < pgn.find("[Round"));
    CHECK(pgn.find("[Round") < pgn.find("[White"));
    CHECK(pgn.find("[White") < pgn.find("[Black"));
    CHECK(pgn.find("[Black") < pgn.find("[Result"));

    PgnTags tags;
    tags.date = "2026.09.27";
    tags.timeControl = preset("5+3").pgnTag();
    Game t;
    t.flagFall(White);
    const std::string p2 = t.pgn("A", "B", tags);
    CHECK(p2.find("[Date \"2026.09.27\"]") != std::string::npos);
    CHECK(p2.find("[TimeControl \"300+3\"]") != std::string::npos);
    CHECK(p2.find("[Termination \"time forfeit\"]") != std::string::npos);
    CHECK(p2.find("0-1") != std::string::npos);

    // Custom start position with Black to move, ongoing game.
    Game f;
    CHECK(f.resetFromFEN("4k3/8/8/8/8/8/4P3/4K3 b - - 0 30"));
    CHECK(playLine(f, {"Kd7", "e4"}));
    const std::string p3 = f.pgn("A", "B");
    CHECK(p3.find("[SetUp \"1\"]") != std::string::npos);
    CHECK(p3.find("[FEN \"4k3/8/8/8/8/8/4P3/4K3 b - - 0 30\"]") != std::string::npos);
    CHECK(p3.find("30... Kd7 31. e4 *") != std::string::npos);
    CHECK(p3.find("[Result \"*\"]") != std::string::npos);
    CHECK(p3.find("[Termination \"unterminated\"]") != std::string::npos);

    // Long games wrap at 80 columns.
    Game l;
    Rng rng{12345};
    for (int i = 0; i < 120 && !l.isOver(); ++i) {
        const std::vector<Move> moves = l.position().legalMoves();
        CHECK(l.play(moves[size_t(rng.next() % moves.size())]));
    }
    const std::string p4 = l.pgn("A", "B");
    CHECK(p4.size() > 600);
    size_t start = 0;
    while (start < p4.size()) {
        size_t end = p4.find('\n', start);
        if (end == std::string::npos) end = p4.size();
        CHECK(end - start <= 80);
        start = end + 1;
    }
}

// ---- Clock --------------------------------------------------------------------------------------

TEST(chess_clock_presets) {
    const std::vector<TimeControl>& p = timeControlPresets();
    const char* labels[] = {"Unlimited", "1+0", "3+0", "3+2", "5+0", "5+3", "10+0", "10+5", "15+10", "30+0", "30+20", "90+30"};
    using C = TimeControl::Category;
    const C cats[] = {C::Unlimited, C::Blitz, C::Blitz, C::Blitz, C::Blitz, C::Blitz, C::Blitz, C::Rapid, C::Rapid, C::Rapid, C::Rapid, C::Standard};
    CHECK_EQ(p.size(), 12u);
    for (size_t i = 0; i < p.size() && i < 12; ++i) {
        CHECK_EQ(p[i].label(), labels[i]);
        CHECK(p[i].category() == cats[i]);
        CHECK_EQ(p[i].delayMs, 0);
    }
    CHECK(p[0].unlimited);
    CHECK_EQ(p[5].baseMs, 300000);
    CHECK_EQ(p[5].incrementMs, 3000);
    CHECK_EQ(p[11].baseMs, 90 * 60000);
    CHECK_EQ(p[11].incrementMs, 30000);

    TimeControl c;
    c.baseMs = 450000;
    c.incrementMs = 2000;
    c.delayMs = 3000;
    CHECK_EQ(c.label(), "Custom 7:30+2 d3");
    c.baseMs = 420000;
    CHECK_EQ(c.label(), "Custom 7+2 d3");
    c.delayMs = 0;
    CHECK_EQ(c.label(), "Custom 7+2");
    c.baseMs = 300000;
    c.incrementMs = 3000;
    CHECK_EQ(c.label(), "5+3");  // same as a preset
    c.baseMs = 3600000;
    c.incrementMs = 0;
    CHECK_EQ(c.label(), "Custom 60+0");
    CHECK(c.category() == C::Standard);  // exactly 60 min: standard (rapid is < 60 min)
    c.baseMs = 50 * 60000;
    c.incrementMs = 10000;  // 50 + 10 = 60 min
    CHECK(c.category() == C::Standard);
    c.incrementMs = 9000;  // 59 min
    CHECK(c.category() == C::Rapid);
    c.baseMs = 5 * 60000;
    c.incrementMs = 5000;  // exactly 10 min: blitz
    CHECK(c.category() == C::Blitz);
    CHECK_EQ(preset("5+3").pgnTag(), "300+3");
    CHECK_EQ(preset("5+0").pgnTag(), "300");
    CHECK_EQ(preset("Unlimited").pgnTag(), "-");
}

TEST(chess_clock_increment) {
    Clock c;
    c.setup(preset("3+2"));
    CHECK(!c.isRunning());
    CHECK_EQ(c.remainingMs(White), 180000);
    c.update(1000);  // not started
    CHECK_EQ(c.remainingMs(White), 180000);
    c.start(White);
    CHECK(c.isRunning());
    CHECK(c.running() == White);
    c.update(10000);
    CHECK_EQ(c.remainingMs(White), 170000);
    CHECK_EQ(c.remainingMs(Black), 180000);
    c.press(White);
    CHECK_EQ(c.remainingMs(White), 172000);
    CHECK(c.running() == Black);
    c.press(White);  // not White's clock: ignored
    CHECK_EQ(c.remainingMs(White), 172000);
    CHECK(c.running() == Black);
    c.update(5000);
    CHECK_EQ(c.remainingMs(Black), 175000);
    c.press(Black);
    CHECK_EQ(c.remainingMs(Black), 177000);
    CHECK(c.running() == White);
    c.addTime(Black, 60000);  // arbiter bonus
    CHECK_EQ(c.remainingMs(Black), 237000);
    // Pause / resume.
    c.stop();
    c.update(4000);
    CHECK_EQ(c.remainingMs(White), 172000);
    c.start(c.running());
    c.update(2000);
    CHECK_EQ(c.remainingMs(White), 170000);
}

TEST(chess_clock_bronstein_delay) {
    TimeControl tc;
    tc.baseMs = 60000;
    tc.incrementMs = 0;
    tc.delayMs = 3000;
    Clock c;
    c.setup(tc);
    c.start(White);
    CHECK_EQ(c.delayLeftMs(), 3000);
    c.update(2000);
    CHECK_EQ(c.remainingMs(White), 58000);
    CHECK_EQ(c.delayLeftMs(), 1000);
    // Pause/resume keeps the delay window.
    c.stop();
    c.start(White);
    CHECK_EQ(c.delayLeftMs(), 1000);
    c.press(White);  // used 2 s < delay: all given back
    CHECK_EQ(c.remainingMs(White), 60000);
    CHECK_EQ(c.delayLeftMs(), 3000);
    c.update(5000);
    CHECK_EQ(c.delayLeftMs(), 0);
    CHECK_EQ(c.remainingMs(Black), 55000);
    c.press(Black);  // used 5 s: 3 s given back
    CHECK_EQ(c.remainingMs(Black), 58000);
    // Delay + increment.
    tc.incrementMs = 1000;
    c.setup(tc);
    c.start(White);
    c.update(4000);
    c.press(White);
    CHECK_EQ(c.remainingMs(White), 60000);  // 60 - 4 + 3 + 1
}

TEST(chess_clock_flag) {
    Clock c;
    c.setup(preset("1+0"));
    c.start(White);
    c.update(59000);
    CHECK(!c.flagged(White));
    CHECK_EQ(c.remainingMs(White), 1000);
    c.update(2000);
    CHECK(c.flagged(White));
    CHECK(!c.flagged(Black));
    CHECK_EQ(c.remainingMs(White), 0);
    CHECK(!c.isRunning());
    c.press(White);  // too late
    CHECK(c.running() == White);
    CHECK_EQ(c.remainingMs(Black), 60000);
    c.start(Black);  // a flagged clock does not restart
    CHECK(!c.isRunning());
    // Black flags too.
    c.setup(preset("1+0"));
    c.start(White);
    c.press(White);
    c.update(61000);
    CHECK(c.flagged(Black));
    CHECK(!c.flagged(White));
}

TEST(chess_clock_unlimited) {
    Clock c;
    c.setup(preset("Unlimited"));
    c.start(White);
    c.update(5000);
    CHECK_EQ(c.remainingMs(White), 5000);  // counts up: elapsed time
    c.update(100000000);
    CHECK(!c.flagged(White));
    CHECK(c.isRunning());
    c.press(White);
    CHECK(c.running() == Black);
    c.update(3000);
    CHECK_EQ(c.remainingMs(Black), 3000);
    c.addTime(Black, 60000);  // ignored
    CHECK_EQ(c.remainingMs(Black), 3000);
    CHECK_EQ(c.delayLeftMs(), 0);
}

// ---- Arbiter ------------------------------------------------------------------------------------

TEST(chess_arbiter_touch_move) {
    Game g;
    Arbiter a;
    a.reset(g);
    const TimeControl tc = preset("5+3");
    CHECK(!a.touch(g, sq("e7")));  // opponent's piece
    CHECK(!a.touch(g, sq("e4")));  // empty square
    CHECK(!a.place(g, sq("e4"), NoPiece));  // nothing touched
    CHECK(a.touch(g, sq("e2")));
    CHECK_EQ(a.touchedSquare(), sq("e2"));
    CHECK(!a.touch(g, sq("d2")));  // committed to e2
    CHECK(a.touch(g, sq("e2")));
    CHECK(a.touchedHasLegalMove(g));
    a.cancelTouch();  // not allowed: e2 can move
    CHECK_EQ(a.touchedSquare(), sq("e2"));
    CHECK(!a.place(g, sq("e2"), NoPiece));  // put back on its square: nothing happens
    CHECK(!a.hasPendingMove());
    CHECK(!a.place(g, sq("d2"), NoPiece));  // own piece there
    CHECK(a.place(g, sq("e4"), NoPiece));
    CHECK(a.hasPendingMove());
    CHECK_EQ(a.pendingMove().from, sq("e2"));
    CHECK_EQ(a.pendingMove().to, sq("e4"));
    CHECK_EQ(a.pendingMove().captured, NoSquare);
    CHECK(!a.pendingMove().castlingRookMove);
    CHECK(!a.place(g, sq("e3"), NoPiece));  // Art. 4.6: released, final
    CHECK(!a.retractPlacement(g));          // legal placement cannot be taken back
    CHECK(!a.touch(g, sq("d2")));
    Arbiter::Verdict v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(v.move == g.position().findLegal(sq("e2"), sq("e4")));
    CHECK(v.move.flags & MoveDoublePush);
    CHECK_EQ(v.message, "");
    CHECK_EQ(v.opponentBonusMs, 0);
    CHECK(!v.forfeit);
    CHECK_EQ(a.touchedSquare(), NoSquare);
    CHECK(!a.hasPendingMove());
    CHECK(g.play(v.move));
    // Black's turn: the arbiter follows the game.
    CHECK(!a.touch(g, sq("d2")));
    CHECK(a.touch(g, sq("e7")));
    CHECK(a.place(g, sq("e5"), NoPiece));
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(g.play(v.move));
    // The opponent's move played directly on the Game (e.g. the engine) resyncs the arbiter.
    CHECK(playSAN(g, "Nf3"));
    CHECK(a.touch(g, sq("b8")));
    CHECK(!a.touch(g, sq("g8")));
    CHECK_EQ(a.illegalCount(White), 0);
    CHECK_EQ(a.illegalCount(Black), 0);
}

TEST(chess_arbiter_piece_without_legal_move) {
    Game g;
    Arbiter a;
    a.reset(g);
    CHECK(a.touch(g, sq("a1")));
    CHECK(!a.touchedHasLegalMove(g));
    a.cancelTouch();  // Art. 4.5: free to choose another piece
    CHECK_EQ(a.touchedSquare(), NoSquare);
    CHECK(a.touch(g, sq("g1")));
    CHECK(a.touchedHasLegalMove(g));
    // Touching another piece when the touched one cannot move releases it automatically.
    Arbiter b;
    b.reset(g);
    CHECK(b.touch(g, sq("c1")));
    CHECK(b.touch(g, sq("b1")));
    CHECK_EQ(b.touchedSquare(), sq("b1"));
    CHECK(!b.touch(g, sq("g1")));  // b1 can move: committed
    // Touch without the game uses the last synchronised position.
    Arbiter c;
    c.reset(g);
    CHECK(c.touch(sq("d1")));
    CHECK(!c.touchedHasLegalMove(g));
    CHECK(c.touch(sq("d2")));
    CHECK(c.place(g, sq("d4"), NoPiece));
    CHECK(c.clockPressed(g, preset("5+3")).legal);
}

TEST(chess_arbiter_illegal_move_penalties) {
    struct Case { const char* tc; int64_t bonus; const char* text; };
    const Case cases[] = {
        {"3+2", 60000, "Black receives one extra minute."},
        {"10+0", 60000, "Black receives one extra minute."},
        {"15+10", 60000, "Black receives one extra minute."},
        {"30+20", 60000, "Black receives one extra minute."},
        {"90+30", 120000, "Black receives two extra minutes."},
        {"Unlimited", 0, "A second illegal move loses the game."},
    };
    for (const Case& k : cases) {
        const TimeControl tc = preset(k.tc);
        Game g;
        Arbiter a;
        a.reset(g);
        CHECK(a.touch(g, sq("e2")));
        CHECK(a.place(g, sq("e5"), NoPiece));  // hints off: physically possible, illegal
        Arbiter::Verdict v = a.clockPressed(g, tc);
        CHECK(!v.legal);
        CHECK(!v.move.valid());
        CHECK(!v.forfeit);
        CHECK(!v.moveStands);
        CHECK_EQ(v.opponentBonusMs, k.bonus);
        CHECK(v.message.find("Arbiter: illegal move. The position is restored") == 0);
        if (v.message.find(k.text) == std::string::npos) std::fprintf(stderr, "  message: %s\n", v.message.c_str());
        CHECK(v.message.find(k.text) != std::string::npos);
        CHECK(v.message.find("White must move the touched piece.") != std::string::npos);
        CHECK_EQ(a.illegalCount(White), 1);
        CHECK_EQ(a.illegalCount(Black), 0);
        // Art. 7.5.1 + 4.3: the same piece must be moved.
        CHECK_EQ(a.touchedSquare(), sq("e2"));
        CHECK(!a.hasPendingMove());
        CHECK(!a.touch(g, sq("d2")));
        CHECK(a.place(g, sq("e4"), NoPiece));
        v = a.clockPressed(g, tc);
        CHECK(v.legal);
        CHECK(g.play(v.move));
    }
    // Exact message from the specification.
    Game g;
    CHECK(playSAN(g, "e4"));
    Arbiter a;
    a.reset(g);
    CHECK(a.touch(g, sq("a8")));  // rook without legal move
    CHECK(a.place(g, sq("a6"), NoPiece));
    const Arbiter::Verdict v = a.clockPressed(g, preset("5+3"));
    CHECK_EQ(v.message, "Arbiter: illegal move. The position is restored and White receives one extra minute.");
}

TEST(chess_arbiter_second_illegal_forfeits) {
    const TimeControl tc = preset("5+3");
    Game g;
    Arbiter a;
    a.reset(g);
    CHECK(a.touch(g, sq("e2")));
    CHECK(a.place(g, sq("e5"), NoPiece));
    CHECK(!a.clockPressed(g, tc).legal);
    CHECK(a.place(g, sq("e4"), NoPiece));
    Arbiter::Verdict v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(g.play(v.move));
    // Black's first illegal move does not forfeit (counts are per player).
    CHECK(a.touch(g, sq("g8")));
    CHECK(a.place(g, sq("g6"), NoPiece));
    v = a.clockPressed(g, tc);
    CHECK(!v.legal);
    CHECK(!v.forfeit);
    CHECK_EQ(v.opponentBonusMs, 60000);
    CHECK(v.message.find("White receives one extra minute") != std::string::npos);
    CHECK(a.place(g, sq("f6"), NoPiece));
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(g.play(v.move));
    // White's second illegal move: forfeit.
    CHECK(a.touch(g, sq("f1")));
    CHECK(a.place(g, sq("f3"), NoPiece));
    v = a.clockPressed(g, tc);
    CHECK(!v.legal);
    CHECK(v.forfeit);
    CHECK_EQ(v.opponentBonusMs, 0);
    CHECK_EQ(v.message, "Arbiter: illegal move. This is White's second illegal move: White loses the game.");
    CHECK_EQ(a.illegalCount(White), 2);
    g.forfeitIllegal(White);
    CHECK(g.status() == GameStatus::BlackWins);
    CHECK(g.endReason() == GameEndReason::IllegalMoves);
    // After the end nothing is accepted.
    CHECK(!a.touch(g, sq("e1")));
    v = a.clockPressed(g, tc);
    CHECK(!v.legal);
    CHECK(!v.forfeit);
    CHECK_EQ(v.message, "The game is over.");
    CHECK_EQ(a.illegalCount(White), 2);

    // Second illegal move when the opponent cannot checkmate: draw message.
    Game d;
    CHECK(d.resetFromFEN("4k3/8/8/8/8/8/8/R3K3 w - - 0 1"));
    Arbiter b;
    b.reset(d);
    for (int i = 0; i < 2; ++i) {
        CHECK(b.touch(d, sq("a1")));
        CHECK(b.place(d, sq("b2"), NoPiece));
        v = b.clockPressed(d, tc);
        CHECK(!v.legal);
    }
    CHECK(v.forfeit);
    CHECK(v.message.find("Black cannot checkmate: the game is drawn") != std::string::npos);
    d.forfeitIllegal(White);
    CHECK(d.status() == GameStatus::Draw);
    // A new game resets the counts.
    d.reset();
    b.reset(d);
    CHECK_EQ(b.illegalCount(White), 0);
}

TEST(chess_arbiter_castling_placement) {
    const TimeControl tc = preset("15+10");
    Game g;
    CHECK(g.resetFromFEN("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
    Arbiter a;
    a.reset(g);
    CHECK(a.touch(g, sq("e1")));
    CHECK(a.place(g, sq("g1"), NoPiece));
    CHECK(a.pendingMove().castlingRookMove);
    Arbiter::Verdict v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(v.move.flags & MoveCastleKing);
    CHECK(g.play(v.move));
    CHECK_EQ(g.position().at(sq("f1")).type, Rook);
    CHECK(a.touch(g, sq("e8")));
    CHECK(a.place(g, sq("c8"), NoPiece));
    CHECK(a.pendingMove().castlingRookMove);
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(v.move.flags & MoveCastleQueen);
    CHECK(g.play(v.move));
    CHECK_EQ(g.sanMoves()[0], "O-O");
    CHECK_EQ(g.sanMoves()[1], "O-O-O");

    // Castling through an attacked square is illegal; the king stays touched (Art. 4.4.3).
    CHECK(g.resetFromFEN("r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1"));
    a.reset(g);
    CHECK(a.touch(g, sq("e1")));
    CHECK(a.place(g, sq("g1"), NoPiece));
    CHECK(a.pendingMove().castlingRookMove);
    v = a.clockPressed(g, tc);
    CHECK(!v.legal);
    CHECK_EQ(v.opponentBonusMs, 60000);
    CHECK_EQ(a.touchedSquare(), sq("e1"));
    CHECK(!a.touch(g, sq("h1")));
    CHECK(a.place(g, sq("c1"), NoPiece));  // O-O-O is legal
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(v.move.flags & MoveCastleQueen);

    // King two squares without castling rights: no rook move, illegal.
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/8/R3K2R w - - 0 1"));
    a.reset(g);
    CHECK(a.touch(g, sq("e1")));
    CHECK(a.place(g, sq("g1"), NoPiece));
    CHECK(!a.pendingMove().castlingRookMove);
    CHECK(!a.clockPressed(g, tc).legal);
}

TEST(chess_arbiter_en_passant_placement) {
    const TimeControl tc = preset("5+3");
    Game g;
    CHECK(playLine(g, {"e4", "a6", "e5", "d5"}));
    Arbiter a;
    a.reset(g);
    CHECK(a.touch(g, sq("e5")));
    CHECK(a.place(g, sq("d6"), NoPiece));
    CHECK_EQ(a.pendingMove().captured, sq("d5"));
    Arbiter::Verdict v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(v.move.flags & MoveEnPassant);
    CHECK(g.play(v.move));
    CHECK(g.position().at(sq("d5")).empty());
    CHECK_EQ(g.sanMoves().back(), "exd6");

    // From a FEN with an en passant square.
    Game f;
    CHECK(f.resetFromFEN("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3"));
    a.reset(f);
    CHECK(a.touch(f, sq("e5")));
    CHECK(a.place(f, sq("f6"), NoPiece));
    CHECK_EQ(a.pendingMove().captured, sq("f5"));
    CHECK(a.clockPressed(f, tc).legal);

    // Pinned pawn: the physical en passant capture is recorded but illegal.
    Game p;
    CHECK(p.resetFromFEN("4k3/2p5/8/KP5r/8/8/8/8 b - - 0 1"));
    CHECK(playUCI(p, "c7c5"));
    a.reset(p);
    CHECK(a.touch(p, sq("b5")));
    CHECK(a.place(p, sq("c6"), NoPiece));
    CHECK_EQ(a.pendingMove().captured, sq("c5"));
    v = a.clockPressed(p, tc);
    CHECK(!v.legal);
    CHECK(a.place(p, sq("b6"), NoPiece));
    CHECK(a.clockPressed(p, tc).legal);

    // A diagonal pawn move to an empty square that is not the en passant square captures nothing.
    Game q;
    a.reset(q);
    CHECK(a.touch(q, sq("e2")));
    CHECK(a.place(q, sq("d3"), NoPiece));
    CHECK_EQ(a.pendingMove().captured, NoSquare);
    CHECK(!a.clockPressed(q, tc).legal);
}

TEST(chess_arbiter_promotion) {
    const TimeControl tc = preset("5+3");
    const char* fen = "8/4P3/8/8/8/8/k7/4K3 w - - 0 1";
    Game g;
    Arbiter a;
    // Choice supplied after the pawn was released.
    CHECK(g.resetFromFEN(fen));
    a.reset(g);
    CHECK(a.touch(g, sq("e7")));
    CHECK(a.place(g, sq("e8"), NoPiece));
    CHECK(a.pendingNeedsPromotion(g));
    CHECK(!a.choosePromotion(g, King));
    CHECK(a.choosePromotion(g, Knight));
    CHECK(!a.pendingNeedsPromotion(g));
    Arbiter::Verdict v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK_EQ(v.move.promotion, Knight);
    CHECK(g.play(v.move));
    CHECK_EQ(g.position().at(sq("e8")).type, Knight);
    // Choice given with the placement.
    CHECK(g.resetFromFEN(fen));
    a.reset(g);
    CHECK(a.touch(g, sq("e7")));
    CHECK(a.place(g, sq("e8"), Rook));
    CHECK(!a.pendingNeedsPromotion(g));
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK_EQ(v.move.promotion, Rook);
    // Choice given by a second place() on the same square.
    CHECK(g.resetFromFEN(fen));
    a.reset(g);
    CHECK(a.touch(g, sq("e7")));
    CHECK(a.place(g, sq("e8"), NoPiece));
    CHECK(!a.place(g, sq("d8"), Bishop));  // square is final
    CHECK(a.place(g, sq("e8"), Bishop));
    CHECK(!a.place(g, sq("e8"), Queen));   // piece is final too
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK_EQ(v.move.promotion, Bishop);
    // Art. 7.5.2: clock pressed without replacing the pawn -> illegal, pawn becomes a queen.
    CHECK(g.resetFromFEN(fen));
    a.reset(g);
    CHECK(a.touch(g, sq("e7")));
    CHECK(a.place(g, sq("e8"), NoPiece));
    v = a.clockPressed(g, tc);
    CHECK(!v.legal);
    CHECK(v.moveStands);
    CHECK(!v.forfeit);
    CHECK_EQ(v.move.promotion, Queen);
    CHECK_EQ(v.opponentBonusMs, 60000);
    CHECK(v.message.find("The pawn becomes a queen and Black receives one extra minute.") != std::string::npos);
    CHECK_EQ(a.illegalCount(White), 1);
    CHECK_EQ(a.touchedSquare(), NoSquare);
    CHECK(g.play(v.move));
    CHECK_EQ(g.position().at(sq("e8")).type, Queen);
    CHECK(a.touch(g, sq("a2")));  // Black's turn now
    // Black pawn promotion with capture.
    CHECK(g.resetFromFEN("4k3/8/8/8/8/8/K5p1/5N2 b - - 0 1"));
    a.reset(g);
    CHECK(a.touch(g, sq("g2")));
    CHECK(a.place(g, sq("f1"), Queen));
    CHECK_EQ(a.pendingMove().captured, sq("f1"));
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(v.move.flags & MovePromotion);
    CHECK(v.move.flags & MoveCapture);
}

TEST(chess_arbiter_misc) {
    const TimeControl tc = preset("30+0");
    Game g;
    Arbiter a;
    a.reset(g);
    // Art. 7.5.3: pressing the clock without making a move.
    Arbiter::Verdict v = a.clockPressed(g, tc);
    CHECK(!v.legal);
    CHECK_EQ(v.opponentBonusMs, 60000);
    CHECK(v.message.find("without making a move") != std::string::npos);
    CHECK_EQ(a.illegalCount(White), 1);
    // Retracting an illegal placement before pressing the clock (not completed yet).
    CHECK(a.touch(g, sq("g1")));
    CHECK(a.place(g, sq("g3"), NoPiece));
    CHECK(a.retractPlacement(g));
    CHECK(!a.hasPendingMove());
    CHECK_EQ(a.touchedSquare(), sq("g1"));
    CHECK(!a.touch(g, sq("b1")));
    CHECK(a.place(g, sq("f3"), NoPiece));
    v = a.clockPressed(g, tc);
    CHECK(v.legal);
    CHECK(g.play(v.move));
    // A capture records the captured square.
    Game c;
    CHECK(playLine(c, {"e4", "d5"}));
    a.reset(c);
    CHECK(a.touch(c, sq("e4")));
    CHECK(a.place(c, sq("d5"), NoPiece));
    CHECK_EQ(a.pendingMove().captured, sq("d5"));
    v = a.clockPressed(c, tc);
    CHECK(v.legal);
    CHECK(v.move.flags & MoveCapture);
    // Out-of-range squares.
    Game d;
    a.reset(d);
    CHECK(!a.touch(d, -1));
    CHECK(!a.touch(d, 64));
    CHECK(a.touch(d, sq("e2")));
    CHECK(!a.place(d, 64, NoPiece));
    CHECK(!a.place(d, NoSquare, NoPiece));
}

// ---- Board queries for the coach's explanations -------------------------------------------------

TEST(chess_board_queries) {
    // Italian after 1.e4 e5 2.Nf3 Nc6 3.Bc4 Nd4 4.Nxe5 Qg5 (the coach's worked fork example).
    Position p = fromFEN("r1b1kbnr/pppp1ppp/8/4N1q1/2BnP3/8/PPPP1PPP/RNBQK2R w KQkq - 1 5");
    CHECK_EQ(p.pieces(White, Knight), squareBit(sq("b1")) | squareBit(sq("e5")));
    CHECK_EQ(squareCount(p.pieces(Pawn)), 15);
    CHECK_EQ(p.occupancy(), p.pieces(White) | p.pieces(Black));
    // g2 and e5 are attacked by the queen on g5 and not defended.
    CHECK_EQ(p.attackersTo(sq("e5"), Black), squareBit(sq("g5")));
    CHECK_EQ(p.attackersTo(sq("e5"), White), uint64_t(0));
    CHECK_EQ(p.attackersTo(sq("g2"), Black), squareBit(sq("g5")));
    CHECK_EQ(p.attackersTo(sq("g2"), White), uint64_t(0));  // the king on e1 is too far
    // Attacks from a square; a pawn attacks its two capture squares.
    CHECK(p.attacksFrom(sq("g5")) & squareBit(sq("g2")));
    CHECK(p.attacksFrom(sq("g5")) & squareBit(sq("e5")));
    CHECK_EQ(p.attacksFrom(sq("e4")), squareBit(sq("d5")) | squareBit(sq("f5")));
    CHECK_EQ(p.attacksFrom(sq("e3")), uint64_t(0));
    CHECK_EQ(attacksOf(Pawn, Black, sq("e5"), 0), squareBit(sq("d4")) | squareBit(sq("f4")));
    // X-rays: the rook a8 and the queen d8 line up on the 8th rank; with the queen out of the
    // occupancy, the rook reaches e8's neighbour d8.
    Position x = fromFEN("r2qk3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_EQ(x.attackersTo(sq("c8"), Black), squareBit(sq("a8")) | squareBit(sq("d8")));
    CHECK_EQ(x.attackersTo(sq("e8"), x.occupancy()) & x.pieces(Black, Rook), uint64_t(0));
    CHECK_EQ(x.attackersTo(sq("e8"), x.occupancy() & ~squareBit(sq("d8"))) & x.pieces(Black, Rook), squareBit(sq("a8")));
    const std::vector<Square> knights = squaresOf(p.pieces(White, Knight));
    CHECK_EQ(knights.size(), size_t(2));
    CHECK_EQ(knights[0], sq("b1"));
    CHECK_EQ(knights[1], sq("e5"));
    CHECK_EQ(squaresBetween(sq("a1"), sq("d4")), squareBit(sq("b2")) | squareBit(sq("c3")));
    CHECK_EQ(squaresBetween(sq("a1"), sq("b3")), uint64_t(0));
    CHECK_EQ(squaresBetween(sq("e1"), sq("e2")), uint64_t(0));
    CHECK_EQ(squareCount(squaresBetween(sq("h8"), sq("h1"))), 6);
}

TEST(chess_checkers_pins_pass_turn) {
    // Double check: rook e1 and bishop b5 against the king on e8.
    Position d = fromFEN("4k3/8/8/1B6/8/8/8/4RK2 b - - 0 1");
    CHECK_EQ(d.checkers(), squareBit(sq("e1")) | squareBit(sq("b5")));
    Position quiet = fromFEN("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_EQ(quiet.checkers(), uint64_t(0));
    // Absolute pin: Bb5 pins the knight c6 to the king e8.
    Position pin = fromFEN("r1bqkbnr/ppp2ppp/2np4/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 4");
    CHECK_EQ(pin.pinned(Black), squareBit(sq("c6")));
    CHECK_EQ(pin.pinned(White), uint64_t(0));
    // Pass the turn: White to move afterwards, en passant cleared, same pieces.
    Position ep = fromFEN("4k3/8/8/8/3pP3/8/8/4K3 b - e3 0 1");
    CHECK_EQ(ep.epSquare(), sq("e3"));
    Position q = ep;
    CHECK(q.passTurn());
    CHECK_EQ(q.sideToMove(), White);
    CHECK_EQ(q.epSquare(), NoSquare);
    CHECK_EQ(q.halfmoveClock(), 1);
    CHECK(q.pieces(White) == ep.pieces(White) && q.pieces(Black) == ep.pieces(Black));
    // The hash equals the one of the same position set up with White to move.
    Position same = fromFEN("4k3/8/8/8/3pP3/8/8/4K3 w - - 1 1");
    CHECK_EQ(q.hash(), same.hash());
    CHECK(q.samePosition(same));
    // In check: no pass, nothing changes.
    Position c = d;
    CHECK(!c.passTurn());
    CHECK_EQ(c.sideToMove(), Black);
}

TEST(chess_game_position_at) {
    Game g;
    CHECK(playLine(g, {"e4", "e5", "Nf3"}));
    CHECK(g.positionAt(0).samePosition(g.startPosition()));
    CHECK_EQ(g.positionAt(1).at(sq("e4")).type, Pawn);
    CHECK_EQ(g.positionAt(1).sideToMove(), Black);
    CHECK(g.positionAt(3).samePosition(g.position()));
    CHECK(g.positionAt(99).samePosition(g.position()));  // clamped
}
