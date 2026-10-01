// Unit tests for src/game/game_saving: the archive mode of each mode of the game scene (which ones
// are saved) and the record of a direct match rebuilt from the moves its authority reported.
#include "test.h"
#include "game/game_saving.h"

#include <string>

using namespace game;
namespace pgn = chess::pgn;

namespace {

// A direct match's moves as the authority reports them: UCI, the time charged, the mover's clock
// after the move (increment included).
struct M { const char* uci; uint32_t spent, clock; };

net::OnlineGame directGame(std::initializer_list<M> moves, int you, int status, int reason) {
    net::OnlineGame og;
    og.baseMs = 600000;
    og.incMs = 5000;
    og.you = you;
    og.status = status;
    og.reason = reason;
    chess::Position pos;
    for (const M& m : moves) {
        chess::Move mv = pos.parseUCI(m.uci);
        net::OnlineGame::MoveRec r;
        r.move = net::packMove(mv.from, mv.to, mv.promotion);
        r.spentMs = m.spent;
        r.clockMs = m.clock;
        og.moves.push_back(r);
        pos.makeMove(mv);
    }
    return og;
}

}  // namespace

// ---- Which games are saved -----------------------------------------------------------------------

TEST(saving_mode_table) {
    using archive::Mode;
    CHECK(saving::archiveMode(GameMode::Play, false) == Mode::Play);
    CHECK(saving::archiveMode(GameMode::Coach, false) == Mode::Coach);
    CHECK(saving::archiveMode(GameMode::HotSeat, false) == Mode::HotSeat);
    CHECK(saving::archiveMode(GameMode::Online, true) == Mode::Direct);
    CHECK(saving::archiveMode(GameMode::Online, false) == Mode::Server);
    CHECK(saving::archiveMode(GameMode::Watch, false) == Mode::Watch);
    CHECK(saving::archiveMode(GameMode::Replay, false) == Mode::Watch);
    // The direct flag only matters online.
    CHECK(saving::archiveMode(GameMode::Play, true) == Mode::Play);
    CHECK(saving::archiveMode(GameMode::Replay, true) == Mode::Watch);

    // With archive::shouldSave: what the scene saves, finished or left after a move.
    auto saved = [](GameMode m, bool direct, int coachLevel) {
        return archive::shouldSave(saving::archiveMode(m, direct), coachLevel, 10, true, true);
    };
    CHECK(saved(GameMode::Play, false, -1));
    CHECK(saved(GameMode::HotSeat, false, -1));
    CHECK(saved(GameMode::Online, true, -1));
    for (int level = 1; level <= 6; ++level) CHECK(saved(GameMode::Coach, false, level));
    CHECK(!saved(GameMode::Coach, false, 0));      // the rules lesson
    CHECK(!saved(GameMode::Online, false, -1));    // a server game: the server keeps it
    CHECK(!saved(GameMode::Watch, false, -1));     // the viewer mode
    CHECK(!saved(GameMode::Replay, false, -1));    // a replay is never saved again
    // Left before any move: only when it has a result (a resignation).
    CHECK(!archive::shouldSave(saving::archiveMode(GameMode::HotSeat, false), -1, 0, false, true));
    CHECK(archive::shouldSave(saving::archiveMode(GameMode::Play, false), -1, 0, true, true));
    // The player's setting turns it all off.
    CHECK(!archive::shouldSave(saving::archiveMode(GameMode::Play, false), -1, 10, true, false));
}

TEST(saving_online_results_and_reasons) {
    CHECK_EQ(saving::onlineResult(0), std::string("*"));
    CHECK_EQ(saving::onlineResult(1), std::string("1-0"));
    CHECK_EQ(saving::onlineResult(2), std::string("0-1"));
    CHECK_EQ(saving::onlineResult(3), std::string("1/2-1/2"));
    CHECK_EQ(saving::onlineResult(4), std::string("*"));
    CHECK_EQ(saving::onlineEndKey(int(chess::GameEndReason::Checkmate)), std::string("reason.checkmate"));
    CHECK_EQ(saving::onlineEndKey(int(chess::GameEndReason::Resignation)), std::string("reason.resignation"));
    CHECK_EQ(saving::onlineEndKey(int(chess::GameEndReason::Timeout)), std::string("reason.timeout"));
    CHECK_EQ(saving::onlineEndKey(20), std::string("reason.online.abandonment"));
    CHECK_EQ(saving::onlineEndKey(26), std::string("reason.online.both_disconnected"));
    CHECK_EQ(saving::onlineEndKey(0), std::string());
    CHECK_EQ(saving::onlineEndKey(19), std::string());
    CHECK_EQ(saving::onlineEndKey(99), std::string());
}

// ---- Direct matches ------------------------------------------------------------------------------

TEST(saving_direct_match_finished) {
    // Fool's mate: Black mates; the first moves are not charged (first-move timers).
    net::OnlineGame og = directGame({{"f2f3", 0, 600000}, {"e7e5", 0, 600000}, {"g2g4", 2400, 602600}, {"d8h4", 1100, 603900}},
                                    1, 2, int(chess::GameEndReason::Checkmate));
    saving::DirectRecord d;
    CHECK(saving::directMatchRecord(og, d));
    CHECK(d.finished);
    CHECK_EQ(int(d.game.moves().size()), 4);
    CHECK(d.game.position().isCheckmate());
    CHECK(d.info.mode == archive::Mode::Direct);
    CHECK_EQ(d.info.result, std::string("0-1"));
    CHECK_EQ(d.info.endKey, std::string("reason.checkmate"));
    CHECK_EQ(d.info.timeControl, std::string("600+5"));
    CHECK_EQ(int(d.info.elapsedMs.size()), 4);
    CHECK_EQ(int(d.info.clockMs.size()), 4);
    CHECK_EQ((long long)d.info.elapsedMs[2], 2400LL);
    CHECK_EQ((long long)d.info.clockMs[3], 603900LL);

    // The saved record: clocks and times on every move, the ending as the authority said.
    d.info.white = "Camille";
    d.info.black = "Human";
    d.info.started = 1790000000;
    pgn::Record rec = archive::makeRecord(d.game, d.info);
    CHECK_EQ(rec.result, std::string("0-1"));
    CHECK_EQ(rec.tag("ScacelithMode"), std::string("direct"));
    CHECK_EQ(rec.tag("TimeControl"), std::string("600+5"));
    CHECK_EQ(rec.tag("Termination"), std::string("normal"));
    CHECK(rec.tag("WhiteElo").empty());
    const std::string text = pgn::write(rec);
    CHECK(text.find("1. f3 {[%clk 0:10:00] [%emt 0:00:00]}") != std::string::npos);
    CHECK(text.find("g4 {[%clk 0:10:02.6] [%emt 0:00:02.4]}") != std::string::npos);
    CHECK(text.find("Qh4# {[%clk 0:10:03.9]") != std::string::npos);
    CHECK(text.find("0-1") != std::string::npos);
}

TEST(saving_direct_match_left) {
    // Aborted by the authority: nothing to save.
    net::OnlineGame aborted = directGame({{"e2e4", 0, 600000}}, 1, 4, 22);
    saving::DirectRecord d;
    CHECK(!saving::directMatchRecord(aborted, d));

    // Left while ongoing before my first move (I am Black, White has moved): an abort, not saved.
    net::OnlineGame early = directGame({{"e2e4", 0, 600000}}, 1, 0, 0);
    CHECK(!saving::directMatchRecord(early, d));
    // ... and as White before any move.
    net::OnlineGame none = directGame({}, 0, 0, 0);
    CHECK(!saving::directMatchRecord(none, d));

    // Left after my first move: saved as my resignation.
    net::OnlineGame left = directGame({{"e2e4", 0, 600000}, {"e7e6", 0, 600000}, {"d2d4", 3000, 602000}}, 1, 0, 0);
    CHECK(saving::directMatchRecord(left, d));
    CHECK(d.finished);
    CHECK_EQ(d.info.result, std::string("1-0"));
    CHECK_EQ(d.info.endKey, std::string("reason.resignation"));
    net::OnlineGame leftWhite = directGame({{"e2e4", 0, 600000}}, 0, 0, 0);
    CHECK(saving::directMatchRecord(leftWhite, d));
    CHECK_EQ(d.info.result, std::string("0-1"));

    // A spectator never saves.
    net::OnlineGame watched = directGame({{"e2e4", 0, 600000}}, 2, 1, int(chess::GameEndReason::Resignation));
    CHECK(!saving::directMatchRecord(watched, d));
}

TEST(saving_direct_match_online_reason_and_bad_moves) {
    // Ended by the authority for an online reason (the opponent left): its key is kept.
    net::OnlineGame left = directGame({{"e2e4", 0, 600000}, {"e7e5", 0, 600000}}, 0, 1, 20);
    saving::DirectRecord d;
    CHECK(saving::directMatchRecord(left, d));
    CHECK_EQ(d.info.result, std::string("1-0"));
    CHECK_EQ(d.info.endKey, std::string("reason.online.abandonment"));
    pgn::Record rec = archive::makeRecord(d.game, d.info);
    CHECK_EQ(rec.tag("Termination"), std::string("abandoned"));

    // Moves that do not follow: never saved.
    net::OnlineGame bad = directGame({{"e2e4", 0, 600000}}, 0, 1, int(chess::GameEndReason::Resignation));
    net::OnlineGame::MoveRec r;
    r.move = net::packMove(12, 28, 0);   // e2e4 again: e2 is empty now
    bad.moves.push_back(r);
    CHECK(!saving::directMatchRecord(bad, d));
}
