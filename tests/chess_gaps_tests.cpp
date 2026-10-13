// The chess rules module, the paths tests/chess_tests.cpp and tests/pgn_tests.cpp leave out: the
// translation key of every end reason and the PGN Termination of the rarer ones, the end detection
// switch, time control labels with tenths of seconds, a PGN file starting with a byte order mark,
// a Chess960 game without its FEN, a record whose FEN does not load, and a record written from
// moves without their SAN (and one whose move does not play).
#include "test.h"
#include "chess/chess.h"
#include "chess/pgn.h"
#include "i18n/i18n.h"

#include <set>
#include <string>

using namespace chess;

TEST(chess_gaps_end_reason_keys_and_termination) {
    const GameEndReason all[] = {
        GameEndReason::Checkmate,          GameEndReason::Resignation,         GameEndReason::Timeout,
        GameEndReason::IllegalMoves,       GameEndReason::Stalemate,           GameEndReason::InsufficientMaterial,
        GameEndReason::TimeoutVsInsufficient, GameEndReason::FivefoldRepetition, GameEndReason::SeventyFiveMoves,
        GameEndReason::ThreefoldClaim,     GameEndReason::FiftyMoveClaim,      GameEndReason::Agreement,
        GameEndReason::IllegalMovesVsInsufficient, GameEndReason::ResignationVsInsufficient,
    };
    std::set<std::string> keys;
    for (GameEndReason r : all) {
        const std::string key = endReasonKey(r);
        CHECK(key.compare(0, 7, "reason.") == 0);
        CHECK(i18n::has(key.c_str()));   // every key has its text in the English table
        keys.insert(key);
    }
    CHECK_EQ(keys.size(), sizeof all / sizeof all[0]);
    CHECK_EQ(std::string(endReasonKey(GameEndReason::None)), std::string());
    CHECK_EQ(std::string(terminationTag(GameStatus::Ongoing, GameEndReason::IllegalMoves)), std::string("unterminated"));
    CHECK_EQ(std::string(terminationTag(GameStatus::BlackWins, GameEndReason::IllegalMoves)), std::string("rules infraction"));
    CHECK_EQ(std::string(terminationTag(GameStatus::Draw, GameEndReason::IllegalMovesVsInsufficient)),
             std::string("rules infraction"));
    CHECK_EQ(std::string(terminationTag(GameStatus::Draw, GameEndReason::TimeoutVsInsufficient)), std::string("time forfeit"));
    CHECK_EQ(std::string(terminationTag(GameStatus::Draw, GameEndReason::SeventyFiveMoves)), std::string("normal"));
}

TEST(chess_gaps_end_detection_switch) {
    Game g;
    CHECK(g.endDetection());
    g.setEndDetection(false);
    CHECK(!g.endDetection());
    g.setEndDetection(true);
    CHECK(g.endDetection());
}

TEST(chess_gaps_time_control_tenths) {
    TimeControl t;
    t.baseMs = 150000;
    t.incrementMs = 2500;
    t.delayMs = 500;
    CHECK_EQ(t.label(), std::string("Custom 2:30+2.5 d0.5"));
    CHECK_EQ(t.pgnTag(), std::string("150+2.5"));
    t.baseMs = 90500;   // a base of tenths: the PGN tag keeps them
    t.incrementMs = 0;
    t.delayMs = 0;
    CHECK_EQ(t.pgnTag(), std::string("90.5"));
}

TEST(chess_gaps_pgn_byte_order_mark_and_chess960_without_fen) {
    pgn::Result<pgn::ParsedGame> r = pgn::read("\xEF\xBB\xBF[Event \"With a BOM\"]\n\n1. e4 e5 2. Nf3 *\n");
    REQUIRE(r.games.size() == 1);
    CHECK(r.games[0].ok());
    CHECK_EQ(r.games[0].record.tag("Event"), std::string("With a BOM"));
    CHECK_EQ(r.games[0].record.plies.size(), size_t(3));
    CHECK_EQ(r.games[0].line, 1);
    r = pgn::read("[Variant \"Chess960\"]\n[Result \"*\"]\n\n1. e4 *\n");
    REQUIRE(r.games.size() == 1);
    CHECK(!r.games[0].ok());
    CHECK(r.games[0].error.message.find("Chess960 game without a FEN tag") != std::string::npos);
    CHECK(r.games[0].record.plies.empty());
    // SetUp "0" with a FEN: the FEN is not used, the same refusal.
    r = pgn::read("[Variant \"Fischer Random\"]\n[SetUp \"0\"]\n[FEN \"bbqnnrkr/pppppppp/8/8/8/8/PPPPPPPP/BBQNNRKR w HFhf - 0 1\"]\n\n*\n");
    REQUIRE(r.games.size() == 1);
    CHECK(r.games[0].error.message.find("Chess960 game without a FEN tag") != std::string::npos);
}

TEST(chess_gaps_pgn_record_edge_cases) {
    // A FEN that does not load: no game.
    pgn::Record bad;
    bad.fen = "not a position";
    Game g;
    CHECK(!bad.toGame(g));
    // Moves without their SAN: written from the positions; the first one that does not play there
    // ends the movetext.
    pgn::Record rec;
    for (const char* uci : {"e2e4", "e7e5", "g1f3"}) {
        pgn::Ply p;
        p.move = rec.positions().back().parseUCI(uci);
        CHECK(p.move.valid());
        rec.plies.push_back(p);
    }
    std::string text = pgn::write(rec);
    CHECK(text.find("1. e4 e5 2. Nf3 *") != std::string::npos);
    pgn::Ply again = rec.plies[0];   // e2e4 once more: no pawn on e2 any longer
    rec.plies.push_back(again);
    rec.plies.push_back(rec.plies[1]);
    text = pgn::write(rec);
    CHECK(text.find("1. e4 e5 2. Nf3 *") != std::string::npos);
    CHECK(text.find("Nf3 e4") == std::string::npos);
}
