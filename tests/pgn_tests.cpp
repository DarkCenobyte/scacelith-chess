// Unit tests for src/chess/pgn: reading real-world exports (lichess, chess.com, annotated files,
// set-up positions), broken games among good ones, the hard caps, the writer and round trips.
#include "test.h"
#include "chess/pgn.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace chess;

namespace {

// A lichess export: clocks and evaluations in the comments, "N..." for Black, a suffix annotation.
const char* kLichess =
    "[Event \"Rated Blitz game\"]\n"
    "[Site \"https://lichess.org/AbCd1234\"]\n"
    "[Date \"2024.03.15\"]\n"
    "[White \"Alice\"]\n"
    "[Black \"Bob\"]\n"
    "[Result \"1-0\"]\n"
    "[UTCDate \"2024.03.15\"]\n"
    "[UTCTime \"18:02:11\"]\n"
    "[WhiteElo \"1850\"]\n"
    "[BlackElo \"1790\"]\n"
    "[TimeControl \"180+2\"]\n"
    "[ECO \"C50\"]\n"
    "[Opening \"Italian Game: Giuoco Pianissimo\"]\n"
    "[Termination \"Normal\"]\n"
    "\n"
    "1. e4 { [%eval 0.36] [%clk 0:03:00] } 1... e5 { [%eval 0.3] [%clk 0:03:00] } 2. Nf3 { [%eval 0.31] [%clk 0:02:58] }\n"
    "2... Nc6 { [%clk 0:02:59] } 3. Bc4 { [%clk 0:02:57] } 3... Bc5 { [%clk 0:02:55] } 4. d3?! { [%clk 0:02:50] }\n"
    "4... Nf6 { [%clk 0:02:51] } 5. O-O $1 { [%clk 0:02:47] } 5... d6 { [%clk 0:02:40] } 1-0\n"
    "\n";

// A chess.com export: tenths in the clocks, a long tag list.
const char* kChessCom =
    "[Event \"Live Chess\"]\n"
    "[Site \"Chess.com\"]\n"
    "[Date \"2024.01.20\"]\n"
    "[Round \"-\"]\n"
    "[White \"Carol\"]\n"
    "[Black \"Dave\"]\n"
    "[Result \"0-1\"]\n"
    "[CurrentPosition \"rnb1kbnr/ppp1pppp/8/q7/8/2N5/PPPP1PPP/R1BQKBNR w KQkq -\"]\n"
    "[Timezone \"UTC\"]\n"
    "[ECO \"B01\"]\n"
    "[ECOUrl \"https://www.chess.com/openings/Scandinavian-Defense-Mieses-Kotrc-Variation\"]\n"
    "[UTCDate \"2024.01.20\"]\n"
    "[UTCTime \"10:15:00\"]\n"
    "[WhiteElo \"1200\"]\n"
    "[BlackElo \"1250\"]\n"
    "[TimeControl \"600\"]\n"
    "[Termination \"Dave won by resignation\"]\n"
    "[Link \"https://www.chess.com/game/live/123456789\"]\n"
    "\n"
    "1. e4 {[%clk 0:09:58.5]} 1... d5 {[%clk 0:09:57.1]} 2. exd5 {[%clk 0:09:50]} 2... Qxd5\n"
    "{[%clk 0:09:55.9]} 3. Nc3 {[%clk 0:09:40.2]} 3... Qa5 {[%clk 0:09:50]} 0-1\n";

// An annotated file: an escape line, a game comment with brackets, NAGs, nested variations,
// a ; comment, move numbers without spaces, a mate.
const char* kOpera =
    "% exported by an old program\n"
    "[Event \"Paris \\\"Opera\\\" game\"]\n"
    "[Site \"Paris FRA\"]\n"
    "[Date \"1858.??.??\"]\n"
    "[Round \"?\"]\n"
    "[White \"Morphy, Paul\"]\n"
    "[Black \"Duke Karl / Count Isouard\"]\n"
    "[Result \"1-0\"]\n"
    "\n"
    "{The Opera Game [famous], see [%csl Gd4]} 1.e4 e5 2.Nf3 d6 3.d4 Bg4 $2 {[a weak move]}\n"
    "(3...exd4 4.Nxd4 (4.Qxd4 Nc6 (4...Nf6 5.e5)) 4...Nf6) 4.dxe5 Bxf3 5.Qxf3 dxe5 6.Bc4 Nf6\n"
    "7.Qb3 Qe7 8.Nc3 c6 9.Bg5 b5?! 10.Nxb5! cxb5 11.Bxb5+ Nbd7 12.O-O-O Rd8 13.Rxd7 Rxd7\n"
    "14.Rd1 Qe6 15.Bxd7+ Nxd7 16.Qb8+!! Nxb8 17.Rd8# ; the queen sacrifice\n"
    "1-0\n";

// A set-up position (Black to move, move 50) and lenient notation.
const char* kFenGame =
    "[Event \"Endgame study\"]\n"
    "[SetUp \"1\"]\n"
    "[FEN \"8/8/8/4k3/8/8/4P3/4K3 b - - 0 50\"]\n"
    "[Result \"*\"]\n"
    "\n"
    "50... Kd5 51. kd2 Kd4 52. e3+ Ke4 *\n";

std::string threeGames() {
    return std::string(
               "[Event \"First\"]\n[White \"A\"]\n[Black \"B\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Qxf7# 1-0\n\n") +
           "[Event \"Broken\"]\n[White \"C\"]\n[Black \"D\"]\n[Result \"*\"]\n\n1. e4 e5 2. Qh5 Ke4 3. Qxe5# *\n\n" +
           "[Event \"Third\"]\n[White \"E\"]\n[Black \"F\"]\n[Result \"1/2-1/2\"]\n\n1. d4 d5 1/2-1/2\n";
}

const pgn::ParsedGame* only(const pgn::Result<pgn::ParsedGame>& r) {
    CHECK_EQ(int(r.games.size()), 1);
    return r.games.empty() ? nullptr : &r.games[0];
}

std::vector<std::string> sans(const pgn::Record& r) {
    std::vector<std::string> out;
    for (const pgn::Ply& p : r.plies) out.push_back(p.san);
    return out;
}

}  // namespace

TEST(pgn_reads_lichess_export) {
    auto res = pgn::read(kLichess);
    const pgn::ParsedGame* g = only(res);
    if (!g) return;
    CHECK(g->ok());
    const pgn::Record& r = g->record;
    CHECK_EQ(r.tag("White"), std::string("Alice"));
    CHECK_EQ(r.tag("Opening"), std::string("Italian Game: Giuoco Pianissimo"));
    CHECK_EQ(r.result, std::string("1-0"));
    CHECK_EQ(int(r.plies.size()), 10);
    CHECK_EQ(r.plies[0].clockMs, int64_t(180000));
    CHECK_EQ(r.plies[2].clockMs, int64_t(178000));
    CHECK_EQ(r.plies[9].clockMs, int64_t(160000));
    CHECK_EQ(r.plies[0].elapsedMs, int64_t(-1));
    // The evaluation stays in the comment; the clock is taken out of it.
    CHECK_EQ(r.plies[0].comment, std::string("[%eval 0.36]"));
    CHECK_EQ(r.plies[3].comment, std::string(""));
    CHECK_EQ(r.plies[6].san, std::string("d3"));
    CHECK_EQ(int(r.plies[6].nags.size()), 1);
    if (!r.plies[6].nags.empty()) CHECK_EQ(r.plies[6].nags[0], 6);  // ?!
    CHECK_EQ(r.plies[8].san, std::string("O-O"));
    if (!r.plies[8].nags.empty()) CHECK_EQ(r.plies[8].nags[0], 1);
    CHECK(r.hasClocks());
    CHECK(!r.hasElapsed());
    CHECK(r.fen.empty());
}

TEST(pgn_reads_chesscom_export) {
    auto res = pgn::read(kChessCom);
    const pgn::ParsedGame* g = only(res);
    if (!g) return;
    CHECK(g->ok());
    const pgn::Record& r = g->record;
    CHECK_EQ(r.result, std::string("0-1"));
    CHECK_EQ(int(r.plies.size()), 6);
    CHECK_EQ(r.plies[0].clockMs, int64_t(598500));
    CHECK_EQ(r.plies[1].clockMs, int64_t(597100));
    CHECK_EQ(r.plies[3].clockMs, int64_t(595900));
    CHECK_EQ(r.plies[3].san, std::string("Qxd5"));
    CHECK_EQ(r.tag("Link"), std::string("https://www.chess.com/game/live/123456789"));
    // The game replays into a chess::Game.
    chess::Game game;
    CHECK(r.toGame(game));
    CHECK_EQ(int(game.moves().size()), 6);
    CHECK_EQ(game.position().fen(), std::string("rnb1kbnr/ppp1pppp/8/q7/8/2N5/PPPP1PPP/R1BQKBNR w KQkq - 2 4"));
}

TEST(pgn_reads_annotated_game_with_variations) {
    auto res = pgn::read(kOpera);
    const pgn::ParsedGame* g = only(res);
    if (!g) return;
    if (!g->ok()) std::fprintf(stderr, "  %s\n", g->error.text().c_str());
    CHECK(g->ok());
    const pgn::Record& r = g->record;
    CHECK_EQ(r.tag("Event"), std::string("Paris \"Opera\" game"));
    CHECK_EQ(r.comment, std::string("The Opera Game [famous], see [%csl Gd4]"));
    CHECK_EQ(int(r.plies.size()), 33);
    CHECK_EQ(r.plies[5].san, std::string("Bg4"));
    CHECK_EQ(r.plies[5].comment, std::string("[a weak move]"));
    if (!r.plies[5].nags.empty()) CHECK_EQ(r.plies[5].nags[0], 2);
    CHECK_EQ(r.plies[6].san, std::string("dxe5"));  // the variations were skipped
    CHECK_EQ(r.plies[30].san, std::string("Qb8+"));
    if (!r.plies[30].nags.empty()) CHECK_EQ(r.plies[30].nags[0], 3);  // !!
    CHECK_EQ(r.plies[32].san, std::string("Rd8#"));
    CHECK_EQ(r.plies[32].comment, std::string("the queen sacrifice"));
    CHECK_EQ(r.result, std::string("1-0"));
    std::vector<chess::Position> pos = r.positions();
    CHECK_EQ(int(pos.size()), 34);
    if (pos.size() == 34) CHECK(pos.back().isCheckmate());
}

TEST(pgn_reads_set_up_positions) {
    auto res = pgn::read(kFenGame);
    const pgn::ParsedGame* g = only(res);
    if (!g) return;
    if (!g->ok()) std::fprintf(stderr, "  %s\n", g->error.text().c_str());
    CHECK(g->ok());
    const pgn::Record& r = g->record;
    CHECK_EQ(r.fen, std::string("8/8/8/4k3/8/8/4P3/4K3 b - - 0 50"));
    CHECK_EQ(sans(r), (std::vector<std::string>{"Kd5", "Kd2", "Kd4", "e3+", "Ke4"}));
    CHECK_EQ(r.startPosition().fullmoveNumber(), 50);
    // The writer numbers from the set-up position and writes SetUp / FEN.
    std::string text = pgn::write(r);
    CHECK(text.find("[SetUp \"1\"]\n[FEN \"8/8/8/4k3/8/8/4P3/4K3 b - - 0 50\"]") != std::string::npos);
    CHECK(text.find("50... Kd5 51. Kd2 Kd4 52. e3+ Ke4 *") != std::string::npos);
    // SetUp "0": the FEN is ignored.
    auto zero = pgn::read("[SetUp \"0\"]\n[FEN \"8/8/8/4k3/8/8/4P3/4K3 b - - 0 50\"]\n\n1. e4 *\n");
    if (const pgn::ParsedGame* z = only(zero)) {
        CHECK(z->ok());
        CHECK(z->record.fen.empty());
    }
}

TEST(pgn_variants_and_chess960) {
    auto atomic = pgn::read("[Variant \"Atomic\"]\n[Result \"*\"]\n\n1. e4 *\n");
    if (const pgn::ParsedGame* g = only(atomic)) {
        CHECK(!g->ok());
        CHECK(g->error.message.find("Atomic") != std::string::npos);
        CHECK_EQ(g->error.line, 1);
    }
    // Shredder castling letters: Position cannot castle from there.
    auto shredder = pgn::read(
        "[Event \"960\"]\n[Variant \"Chess960\"]\n[SetUp \"1\"]\n"
        "[FEN \"bqnb1rkr/pp3ppp/3ppn2/2p5/5P2/P2P4/NPP1P1PP/BQ1BNRKR w HFhf - 2 9\"]\n\n9. g3 *\n");
    if (const pgn::ParsedGame* g = only(shredder)) {
        CHECK(!g->ok());
        CHECK_EQ(g->error.line, 4);
        CHECK(g->error.message.find("Chess960") != std::string::npos);
    }
    // Chess960 with KQkq rights whose rooks are not on a1/h1: refused, not silently changed.
    auto moved = pgn::read(
        "[Variant \"Chess960\"]\n[FEN \"rkrnnqbb/pppppppp/8/8/8/8/PPPPPPPP/RKRNNQBB w KQkq - 0 1\"]\n\n1. e4 *\n");
    if (const pgn::ParsedGame* g = only(moved)) CHECK(!g->ok());
    // Chess960 from the standard setup (position 518) plays like chess.
    auto sp518 = pgn::read(
        "[Variant \"Chess960\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\"]\n\n1. e4 e5 2. Nf3 *\n");
    if (const pgn::ParsedGame* g = only(sp518)) {
        CHECK(g->ok());
        CHECK_EQ(int(g->record.plies.size()), 3);
        CHECK(g->record.fen.empty());
    }
    // "From Position" (lichess) is standard chess.
    auto fromPos = pgn::read("[Variant \"From Position\"]\n[FEN \"4k3/8/8/8/8/8/4P3/4K3 w - - 0 1\"]\n\n1. e4 *\n");
    if (const pgn::ParsedGame* g = only(fromPos)) CHECK(g->ok());
}

TEST(pgn_broken_game_between_good_ones) {
    const std::string text = threeGames();
    auto res = pgn::read(text);
    CHECK_EQ(int(res.games.size()), 3);
    if (res.games.size() != 3) return;
    CHECK(res.games[0].ok());
    CHECK_EQ(int(res.games[0].record.plies.size()), 7);
    CHECK_EQ(res.games[0].record.result, std::string("1-0"));
    CHECK(!res.games[1].ok());
    CHECK_EQ(res.games[1].error.line, 13);
    CHECK_EQ(res.games[1].error.column, 17);
    CHECK_EQ(res.games[1].error.message, std::string("illegal move 'Ke4'"));
    CHECK_EQ(res.games[1].error.text(), std::string("line 13, column 17: illegal move 'Ke4'"));
    CHECK_EQ(res.games[1].record.tag("Event"), std::string("Broken"));
    CHECK_EQ(int(res.games[1].record.plies.size()), 3);  // what came before the error
    CHECK(res.games[2].ok());
    CHECK_EQ(res.games[2].record.result, std::string("1/2-1/2"));
    CHECK_EQ(res.games[2].record.tag("Event"), std::string("Third"));
    // The scan finds the same games at the same places, with the move counts of the tokens.
    auto sc = pgn::scan(text);
    CHECK_EQ(int(sc.games.size()), 3);
    if (sc.games.size() != 3) return;
    CHECK_EQ(sc.games[0].plies, 7);
    CHECK_EQ(sc.games[1].plies, 5);
    CHECK(sc.games[1].error.empty());  // the scan does not play the moves
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(sc.games[size_t(i)].offset, res.games[size_t(i)].offset);
        CHECK_EQ(sc.games[size_t(i)].length, res.games[size_t(i)].length);
        CHECK_EQ(sc.games[size_t(i)].line, res.games[size_t(i)].line);
    }
    // A slice read alone, with its origin, reports the same error position.
    const pgn::Summary& s1 = sc.games[1];
    CHECK_EQ(text.substr(s1.offset, 7), std::string("[Event "));
    auto slice = pgn::read(text.substr(s1.offset, s1.length), pgn::Limits(), pgn::Origin{s1.line, s1.column});
    if (const pgn::ParsedGame* g = only(slice)) {
        CHECK_EQ(g->error.line, 13);
        CHECK_EQ(g->error.column, 17);
    }
}

TEST(pgn_lenient_notation_and_recovery) {
    // 0-0, e8Q, figurines, "e.p.", lowercase pieces, trailing annotations, text evaluations.
    auto res = pgn::read(
        "[FEN \"4k3/P7/8/3pP3/8/8/8/R3K2R w KQ d6 0 1\"]\n\n"
        "1. exd6 e.p. Kd7 2. 0-0 Kxd6 3. a8Q +- Ke5 4. \xE2\x99\x96" "a5+ Kd4 5. qd8+ +/- *\n");
    if (const pgn::ParsedGame* g = only(res)) {
        if (!g->ok()) std::fprintf(stderr, "  %s\n", g->error.text().c_str());
        CHECK(g->ok());
        CHECK_EQ(sans(g->record), (std::vector<std::string>{"exd6", "Kd7", "O-O", "Kxd6", "a8=Q", "Ke5", "Ra5+", "Kd4", "Qd8+"}));
    }
    // A game without a result runs into the next game's tags; a game without tags.
    auto two = pgn::read("[Event \"A\"]\n\n1. e4 e5\n\n[Event \"B\"]\n\n1. d4 *\n\n1. c4 c5 *\n");
    CHECK_EQ(int(two.games.size()), 3);
    if (two.games.size() == 3) {
        CHECK(two.games[0].ok());
        CHECK_EQ(two.games[0].record.result, std::string("*"));
        CHECK_EQ(int(two.games[0].record.plies.size()), 2);
        CHECK_EQ(two.games[1].record.tag("Event"), std::string("B"));
        CHECK_EQ(int(two.games[2].record.plies.size()), 2);
    }
    // An unterminated comment: the error is at its brace and the next game still loads.
    auto open = pgn::read("[Event \"A\"]\n\n1. e4 {never closed\n2. Nf3\n[Event \"B\"]\n\n1. d4 *\n");
    CHECK_EQ(int(open.games.size()), 2);
    if (open.games.size() == 2) {
        CHECK(!open.games[0].ok());
        CHECK_EQ(open.games[0].error.line, 3);
        CHECK_EQ(open.games[0].error.column, 7);
        CHECK(open.games[1].ok());
        CHECK_EQ(open.games[1].record.tag("Event"), std::string("B"));
    }
    // Null moves are refused with their position.
    auto null = pgn::read("1. e4 -- 2. d4 *\n");
    if (const pgn::ParsedGame* g = only(null)) {
        CHECK(!g->ok());
        CHECK_EQ(g->error.column, 7);
    }
    // A Windows-1252 name becomes UTF-8; an unescaped quote inside a value survives.
    auto latin = pgn::read("[White \"Andr\xE9 M\xFCller\"]\n[Event \"The \"big\" one\"]\n\n1. e4 *\n");
    if (const pgn::ParsedGame* g = only(latin)) {
        CHECK(g->ok());
        CHECK_EQ(g->record.tag("White"), std::string("Andr\xC3\xA9 M\xC3\xBC" "ller"));
        CHECK_EQ(g->record.tag("Event"), std::string("The \"big\" one"));
    }
    // Garbage never crashes and yields an error with a position.
    auto junk = pgn::read(std::string("\x01\x02 )) ]] [[ {{ $$$ 1. ?? \0 \xFF\xFE", 30));
    for (const auto& g : junk.games) CHECK(!g.error.empty() || g.record.plies.empty());
    auto utf16 = pgn::read(std::string("\xFF\xFE[\0E\0", 6));
    CHECK(!utf16.error.empty());
}

TEST(pgn_caps) {
    pgn::Limits lim;
    // Input size.
    lim.maxBytes = 64;
    auto big = pgn::read(std::string(65, ' '), lim);
    CHECK(!big.error.empty());
    CHECK(big.games.empty());
    // Games per file.
    lim = pgn::Limits();
    lim.maxGames = 2;
    auto many = pgn::read("1. e4 *\n1. d4 *\n1. c4 *\n", lim);
    CHECK_EQ(int(many.games.size()), 2);
    CHECK(many.truncated);
    // Plies.
    lim = pgn::Limits();
    lim.maxPlies = 4;
    auto longGame = pgn::read("1. Nf3 Nf6 2. Ng1 Ng8 3. Nf3 *\n", lim);
    if (const pgn::ParsedGame* g = only(longGame)) {
        CHECK(!g->ok());
        CHECK(g->error.message.find("too many moves") != std::string::npos);
        CHECK_EQ(g->error.column, 26);
    }
    // Tag value and name lengths, number of tags.
    lim = pgn::Limits();
    lim.maxTagValue = 8;
    auto longTag = pgn::read("[Event \"123456789\"]\n\n1. e4 *\n", lim);
    if (const pgn::ParsedGame* g = only(longTag)) CHECK(g->error.message.find("too long") != std::string::npos);
    auto longName = pgn::read("[" + std::string(80, 'A') + " \"x\"]\n\n1. e4 *\n");
    if (const pgn::ParsedGame* g = only(longName)) CHECK(!g->ok());
    lim = pgn::Limits();
    lim.maxTags = 2;
    auto tags = pgn::read("[A \"1\"]\n[B \"2\"]\n[C \"3\"]\n\n1. e4 *\n", lim);
    if (const pgn::ParsedGame* g = only(tags)) {
        CHECK(!g->ok());
        CHECK_EQ(g->error.line, 3);
    }
    // Nesting depth (and a very deep hostile nesting does not recurse).
    lim = pgn::Limits();
    lim.maxDepth = 3;
    auto deep = pgn::read("1. e4 (1. d4 (1. c4 (1. Nf3 (1. g3))))) e5 *\n", lim);
    if (const pgn::ParsedGame* g = only(deep)) {
        CHECK(!g->ok());
        CHECK(g->error.message.find("nested") != std::string::npos);
    }
    auto hostile = pgn::read("1. e4 " + std::string(200000, '(') + " *\n");
    CHECK_EQ(int(hostile.games.size()), 1);
    // Comments are cut, not refused.
    lim = pgn::Limits();
    lim.maxComment = 10;
    auto chatty = pgn::read("1. e4 {" + std::string(50, 'x') + "} *\n", lim);
    if (const pgn::ParsedGame* g = only(chatty)) {
        CHECK(g->ok());
        if (!g->record.plies.empty()) CHECK_EQ(g->record.plies[0].comment.size(), size_t(10));
    }
}

TEST(pgn_writer_order_and_wrapping) {
    pgn::Record r;
    r.setTag("CoachLevel", "3");
    r.setTag("ScacelithMode", "coach");
    r.setTag("BlackElo", "1300");
    r.setTag("White", "Olivier");
    r.setTag("Black", "Coach");
    r.setTag("Event", "Coach game");
    r.setTag("Opening", "Sicilian Defense");
    r.setTag("TimeControl", "-");
    r.setTag("Termination", "normal");
    r.setTag("PlyCount", "999");  // computed by the writer
    chess::Game g;
    for (const char* m : {"e4", "c5", "Nf3", "d6", "d4", "cxd4", "Nxd4", "Nf6", "Nc3", "a6"})
        g.play(g.position().parseSAN(m));
    pgn::Record moves = pgn::Record::fromGame(g);
    r.plies = moves.plies;
    r.result = "*";
    for (size_t i = 0; i < r.plies.size(); ++i) r.plies[i].elapsedMs = int64_t(i) * 1500 + 300;
    r.plies[3].comment = "A {braced} remark that is long enough to need wrapping across the eighty column limit";
    std::string text = pgn::write(r);
    const char* expectHead =
        "[Event \"Coach game\"]\n[Site \"?\"]\n[Date \"????.??.??\"]\n[Round \"?\"]\n[White \"Olivier\"]\n"
        "[Black \"Coach\"]\n[Result \"*\"]\n[TimeControl \"-\"]\n[Termination \"normal\"]\n[Opening \"Sicilian Defense\"]\n"
        "[BlackElo \"1300\"]\n[PlyCount \"10\"]\n[CoachLevel \"3\"]\n[ScacelithMode \"coach\"]\n\n";
    CHECK_EQ(text.substr(0, std::string(expectHead).size()), std::string(expectHead));
    size_t start = std::string(expectHead).size(), maxLine = 0;
    for (size_t i = start; i < text.size();) {
        size_t e = text.find('\n', i);
        maxLine = std::max(maxLine, e - i);
        i = e + 1;
    }
    CHECK(maxLine <= 80);
    std::string flat = text.substr(start);
    for (char& c : flat)
        if (c == '\n') c = ' ';
    CHECK(flat.find("1. e4 {[%emt 0:00:00.3]} 1... c5 {[%emt 0:00:01.8]}") != std::string::npos);
    CHECK(flat.find("2... d6 {[%emt 0:00:04.8] A (braced) remark") != std::string::npos);
    CHECK(flat.find("5... a6 {[%emt 0:00:13.8]} *") != std::string::npos);
    CHECK(text.back() == '\n');
}

TEST(pgn_round_trip) {
    pgn::Record r;
    r.setTag("Event", "Casual game");
    r.setTag("Site", "Scacelith");
    r.setTag("Date", "2026.10.01");
    r.setTag("Round", "4");
    r.setTag("White", "\xC3\x89lodie \"Lef\xC3\xA8vre\" \\o/");
    r.setTag("Black", "\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E");
    r.setTag("TimeControl", "300+3");
    r.setTag("Termination", "time forfeit");
    r.setTag("WhiteElo", "1512");
    r.setTag("Time", "21:04:09");
    r.setTag("ScacelithMode", "hotseat");
    r.fen = "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 3 30";
    r.comment = "Both kings in the middle";
    chess::Position pos = r.startPosition();
    int64_t clk[2] = {300000, 300000};
    const char* line[] = {"O-O-O", "O-O", "Rd7", "Ra8+", "Kb7"};
    for (int i = 0; i < 5; ++i) {
        pgn::Ply p;
        p.move = pos.parseSAN(line[i]);
        CHECK(p.move.valid());
        p.san = pos.toSAN(p.move);
        const int side = int(pos.sideToMove());
        p.elapsedMs = 1000 + i * 2350;
        clk[side] += 3000 - p.elapsedMs;
        p.clockMs = clk[side];
        if (i == 2) p.nags = {1, 14};
        if (i == 3) p.comment = "check [x]";
        pos.makeMove(p.move);
        r.plies.push_back(p);
    }
    r.result = "0-1";
    const std::string text = pgn::write(r);
    auto back = pgn::read(text);
    const pgn::ParsedGame* g = only(back);
    if (!g) return;
    if (!g->ok()) std::fprintf(stderr, "  %s\n%s\n", g->error.text().c_str(), text.c_str());
    CHECK(g->ok());
    const pgn::Record& b = g->record;
    CHECK_EQ(b.fen, r.fen);
    CHECK_EQ(b.result, r.result);
    CHECK_EQ(b.comment, r.comment);
    CHECK_EQ(int(b.plies.size()), int(r.plies.size()));
    for (size_t i = 0; i < b.plies.size() && i < r.plies.size(); ++i) {
        CHECK(b.plies[i].move == r.plies[i].move);
        CHECK_EQ(b.plies[i].san, r.plies[i].san);
        CHECK_EQ(b.plies[i].clockMs, r.plies[i].clockMs);
        CHECK_EQ(b.plies[i].elapsedMs, r.plies[i].elapsedMs);
        CHECK_EQ(b.plies[i].comment, r.plies[i].comment);
        CHECK(b.plies[i].nags == r.plies[i].nags);
    }
    // Every tag comes back, plus the computed ones.
    for (const pgn::Tag& t : r.tags) CHECK_EQ(b.tag(t.name, "<missing>"), t.value);
    CHECK_EQ(b.tag("PlyCount"), std::string("5"));
    CHECK_EQ(b.tag("SetUp"), std::string("1"));
    CHECK_EQ(b.tag("Result"), std::string("0-1"));
    // Writing the read record again gives the same text.
    CHECK_EQ(pgn::write(b), text);
    // A file of several written games reads back game by game.
    pgn::Record second;
    second.plies = pgn::Record::fromGame(chess::Game()).plies;
    auto both = pgn::read(pgn::writeAll({r, second}));
    CHECK_EQ(int(both.games.size()), 2);
}

TEST(pgn_times_and_helpers) {
    CHECK_EQ(pgn::formatTime(0), std::string("0:00:00"));
    CHECK_EQ(pgn::formatTime(298000), std::string("0:04:58"));
    CHECK_EQ(pgn::formatTime(2500), std::string("0:00:02.5"));
    CHECK_EQ(pgn::formatTime(5400250), std::string("1:30:00.25"));
    CHECK_EQ(pgn::formatTime(61001), std::string("0:01:01.001"));
    int64_t ms = 0;
    CHECK(pgn::parseTime("0:04:58", ms));
    CHECK_EQ(ms, int64_t(298000));
    CHECK(pgn::parseTime("0:02:59.9", ms));
    CHECK_EQ(ms, int64_t(179900));
    CHECK(pgn::parseTime("4:05", ms));
    CHECK_EQ(ms, int64_t(245000));
    CHECK(pgn::parseTime("12.25", ms));
    CHECK_EQ(ms, int64_t(12250));
    CHECK(pgn::parseTime("1:00:00.12345", ms));
    CHECK_EQ(ms, int64_t(3600123));
    CHECK(!pgn::parseTime("", ms));
    CHECK(!pgn::parseTime("0:61:00", ms));
    CHECK(!pgn::parseTime("a:00", ms));
    CHECK(!pgn::parseTime("1:2:3:4", ms));
    int64_t base = 0, inc = 0;
    CHECK(pgn::parseTimeControl("300+3", base, inc));
    CHECK_EQ(base, int64_t(300000));
    CHECK_EQ(inc, int64_t(3000));
    CHECK(pgn::parseTimeControl("600", base, inc));
    CHECK_EQ(inc, int64_t(0));
    CHECK(!pgn::parseTimeControl("-", base, inc));
    CHECK(!pgn::parseTimeControl("?", base, inc));
    CHECK(!pgn::parseTimeControl("40/7200:3600", base, inc));
    CHECK_EQ(pgn::normalizeResult("\xC2\xBD-\xC2\xBD"), std::string("1/2-1/2"));
    CHECK_EQ(pgn::normalizeResult("0-0"), std::string(""));
    CHECK_EQ(std::string(pgn::terminationValue(GameStatus::Ongoing, GameEndReason::None)), std::string("unterminated"));
    CHECK_EQ(std::string(pgn::terminationValue(GameStatus::WhiteWins, GameEndReason::Timeout)), std::string("time forfeit"));
    CHECK(pgn::tagRank("Event") < pgn::tagRank("Result"));
    CHECK(pgn::tagRank("Result") < pgn::tagRank("TimeControl"));
    CHECK(pgn::tagRank("BlackElo") < pgn::tagRank("PlyCount"));
    CHECK(pgn::tagRank("FEN") < pgn::tagRank("ScacelithMode"));
}

// A game whose movetext is only "*" (a study chapter, a position) keeps its set-up position, and
// its tags are checked as those of any game.
TEST(pgn_result_only_game_keeps_its_position) {
    const char* text = "[SetUp \"1\"]\n[FEN \"8/8/8/8/8/8/k7/7K b - - 0 40\"]\n\n*";
    auto res = pgn::read(text);
    if (const pgn::ParsedGame* g = only(res)) {
        CHECK(g->ok());
        CHECK_EQ(g->record.fen, std::string("8/8/8/8/8/8/k7/7K b - - 0 40"));
        CHECK(g->record.plies.empty());
        // Written and read again, the position is still there.
        auto back = pgn::read(pgn::write(g->record));
        if (const pgn::ParsedGame* b = only(back)) CHECK_EQ(b->record.fen, g->record.fen);
    }
    auto zh = pgn::read("[Variant \"Crazyhouse\"]\n\n*");
    if (const pgn::ParsedGame* g = only(zh)) CHECK(!g->ok());
    auto badFen = pgn::read("[SetUp \"1\"]\n[FEN \"not a position\"]\n\n*\n");
    if (const pgn::ParsedGame* g = only(badFen)) {
        CHECK(!g->ok());
        CHECK_EQ(g->error.line, 2);
    }
    // Tags and no movetext at all, at the end of a file: checked too.
    auto tagsOnly = pgn::read("[Event \"A\"]\n\n1. e4 *\n\n[Variant \"Atomic\"]\n");
    CHECK_EQ(int(tagsOnly.games.size()), 2);
    if (tagsOnly.games.size() == 2) CHECK(!tagsOnly.games[1].ok());
    CHECK_EQ(int(pgn::scan(text).games.size()), 1);
}

// Chess960 from the standard setup (position 518, as lichess exports it): what the reader
// accepts, the writer writes so that it reads back.
TEST(pgn_chess960_standard_setup_round_trip) {
    auto res = pgn::read(
        "[Variant \"Chess960\"]\n[SetUp \"1\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\"]\n\n"
        "1. e4 e5 2. Nf3 *\n");
    const pgn::ParsedGame* g = only(res);
    if (!g) return;
    CHECK(g->ok());
    const std::string text = pgn::write(g->record);
    CHECK(text.find("[SetUp \"1\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\"]") != std::string::npos);
    auto back = pgn::read(text);
    if (const pgn::ParsedGame* b = only(back)) {
        if (!b->ok()) std::fprintf(stderr, "  %s\n", b->error.text().c_str());
        CHECK(b->ok());
        CHECK_EQ(int(b->record.plies.size()), 3);
        CHECK_EQ(pgn::write(b->record), text);
    }
    // A standard game still gets no FEN.
    CHECK(pgn::write(pgn::Record()).find("[FEN") == std::string::npos);
}

// A tag value full of unescaped quotes is read in one pass: a small hostile file must not stall
// the listing (it took seconds per 160 KB when every quote looked ahead to the end of the line).
TEST(pgn_quotes_in_tag_values_take_linear_time) {
    std::string small = "[Event \"";
    for (int i = 0; i < 500; ++i) small += "\"x";
    small += "\"]\n\n1. e4 *\n";
    auto res = pgn::read(small);
    if (const pgn::ParsedGame* g = only(res)) {
        CHECK(g->ok());
        CHECK_EQ(g->record.tag("Event").size(), size_t(1000));
        CHECK_EQ(g->record.tag("Event").substr(0, 4), std::string("\"x\"x"));
    }
    std::string big = "[Event \"";
    for (int i = 0; i < 80000; ++i) big += "\"x";
    big += "\"]\n\n1. e4 *\n\n[Event \"Next\"]\n\n1. d4 *\n";
    auto t0 = std::chrono::steady_clock::now();
    auto sc = pgn::scan(big);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  %zu bytes of quotes scanned in %.1f ms\n", big.size(), ms);
    CHECK(ms < 1000.0);
    CHECK_EQ(int(sc.games.size()), 2);
    if (sc.games.size() == 2) {
        CHECK(sc.games[0].error.message.find("too long") != std::string::npos);
        CHECK_EQ(sc.games[1].tag("Event"), std::string("Next"));
    }
}

// Nothing swallows the games after it: a '<' without its '>' stops at the end of its line, and a
// file with CR line ends (old Mac programs) has lines too.
TEST(pgn_stray_bracket_and_cr_line_ends) {
    const std::string three =
        "[Event \"One\"]\n\n1. e4 e5 2. Nf3 < 2... Nc6 1-0\n\n[Event \"Two\"]\n\n1. d4 *\n\n[Event \"Three\"]\n\n1. c4 *\n";
    auto res = pgn::read(three);
    CHECK_EQ(int(res.games.size()), 3);
    if (res.games.size() == 3) {
        CHECK(!res.games[0].ok());
        CHECK_EQ(res.games[0].error.line, 3);
        CHECK_EQ(res.games[0].error.column, 17);
        CHECK(res.games[1].ok());
        CHECK_EQ(res.games[1].record.tag("Event"), std::string("Two"));
        CHECK(res.games[2].ok());
    }
    CHECK_EQ(int(pgn::scan(three).games.size()), 3);
    // A reserved <...> on one line is still skipped.
    auto reserved = pgn::read("1. e4 <reserved> e5 *\n");
    if (const pgn::ParsedGame* g = only(reserved)) {
        CHECK(g->ok());
        CHECK_EQ(int(g->record.plies.size()), 2);
    }
    // CR line ends: the ; comment ends with its line, the next game is a game of its own, an
    // escape line is one line, and positions count the lines.
    const std::string cr = "[Event \"One\"]\r\r1. e4 e5 ; note\r2. Nf3 1-0\r\r[Event \"Two\"]\r%escaped\r\r1. d4 Ke4 *\r";
    auto mac = pgn::read(cr);
    CHECK_EQ(int(mac.games.size()), 2);
    if (mac.games.size() == 2) {
        CHECK(mac.games[0].ok());
        CHECK_EQ(int(mac.games[0].record.plies.size()), 3);
        if (mac.games[0].record.plies.size() == 3) CHECK_EQ(mac.games[0].record.plies[1].comment, std::string("note"));
        CHECK_EQ(mac.games[1].record.tag("Event"), std::string("Two"));
        CHECK(!mac.games[1].ok());
        CHECK_EQ(mac.games[1].error.line, 9);
        CHECK_EQ(mac.games[1].error.column, 7);
    }
    CHECK_EQ(int(pgn::scan(cr).games.size()), 2);
    // An unterminated comment in a CR file is found at the next tag line.
    auto open = pgn::read("[Event \"A\"]\r\r1. e4 {never closed\r[Event \"B\"]\r\r1. d4 *\r");
    CHECK_EQ(int(open.games.size()), 2);
    if (open.games.size() == 2) CHECK(open.games[1].ok());
}

// Mutated real files (bytes replaced, inserted, deleted, cut): the reader never crashes, and the
// scan of the listing finds the same games, at the same places, as the full read.
TEST(pgn_mutations_never_crash_and_scan_agrees) {
    const std::string base = std::string(kLichess) + kChessCom + "\n" + kOpera + "\n" + kFenGame + "\n" + threeGames();
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    static const char alphabet[] = "[]{}()<>\";%$!?.*-+#=/\\\n\r 0123456789abcdefghKQRBNOxe\xE2\x99\x98\xC3\xA9\xFF";
    int checked = 0;
    for (int round = 0; round < 600; ++round) {
        std::string s = base;
        const int edits = 1 + int(rnd() % 6);
        for (int e = 0; e < edits && !s.empty(); ++e) {
            const size_t at = rnd() % s.size();
            const char c = alphabet[rnd() % (sizeof(alphabet) - 1)];
            switch (rnd() % 4) {
            case 0: s[at] = c; break;
            case 1: s.insert(s.begin() + long(at), c); break;
            case 2: s.erase(at, 1 + rnd() % 8); break;
            default: s.resize(at); break;
            }
        }
        auto full = pgn::read(s);
        auto quick = pgn::scan(s);
        CHECK_EQ(full.games.size(), quick.games.size());
        if (full.games.size() != quick.games.size()) continue;
        for (size_t i = 0; i < full.games.size(); ++i) {
            CHECK_EQ(full.games[i].offset, quick.games[i].offset);
            CHECK_EQ(full.games[i].length, quick.games[i].length);
            CHECK(full.games[i].offset + full.games[i].length <= s.size());
            // The written form of whatever was read reads back to the same moves.
            if (full.games[i].ok()) {
                auto again = pgn::read(pgn::write(full.games[i].record));
                CHECK_EQ(int(again.games.size()), 1);
                if (again.games.size() == 1) CHECK_EQ(again.games[0].record.plies.size(), full.games[i].record.plies.size());
                ++checked;
            }
        }
    }
    CHECK(checked > 1000);
}
