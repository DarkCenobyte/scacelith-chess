// What the game scene saves (see game_saving.h).
#include "game_saving.h"
#include "../core/log.h"
#include "../net/protocol_gen.h"

namespace game {
namespace saving {

namespace {

// Protocol values (realtime protocol v1: protocol/scacelith-v1.json), as
// game_scene_online.cpp names them.
enum Status { StOngoing = 0, StWhiteWins = 1, StBlackWins = 2, StDraw = 3, StAborted = 4 };
constexpr int kReasonServerAborted = 25;
// The last chess::GameEndReason (1..14 are the chess reasons, 20 and above the online ones).
constexpr int kLastChessReason = int(chess::GameEndReason::ResignationVsInsufficient);
// The names above are the generated ones (net/protocol_gen.h): a schema change fails here.
static_assert(StOngoing == int(net::proto::GameStatus::Ongoing) && StWhiteWins == int(net::proto::GameStatus::WhiteWins) &&
                  StBlackWins == int(net::proto::GameStatus::BlackWins) && StDraw == int(net::proto::GameStatus::Draw) &&
                  StAborted == int(net::proto::GameStatus::Aborted),
              "GameStatus");
static_assert(kReasonServerAborted == int(net::proto::EndReason::ServerAborted) &&
                  kLastChessReason == int(net::proto::EndReason::ResignationVsInsufficient),
              "EndReason");

}  // namespace

archive::Mode archiveMode(GameMode mode, bool directMatch) {
    switch (mode) {
    case GameMode::Play: return archive::Mode::Play;
    case GameMode::Coach: return archive::Mode::Coach;
    case GameMode::HotSeat: return archive::Mode::HotSeat;
    case GameMode::Online: return directMatch ? archive::Mode::Direct : archive::Mode::Server;
    case GameMode::Watch:
    case GameMode::Replay:
    case GameMode::Analysis: return archive::Mode::Watch;
    }
    return archive::Mode::Watch;
}

std::string onlineResult(int status) {
    switch (status) {
    case StWhiteWins: return "1-0";
    case StBlackWins: return "0-1";
    case StDraw: return "1/2-1/2";
    default: return "*";
    }
}

std::string onlineEndKey(int reason) {
    // The reasons of the game over card (reasonText in game_scene_online.cpp), as keys.
    switch (reason) {
    case 20: return "reason.online.abandonment";
    case 21: return "reason.online.abandonment_vs_insufficient";
    case 22: return "reason.online.aborted";
    case 23: return "reason.online.no_show";
    case 24: return "reason.online.forfeit";
    case 25: return "reason.online.server_aborted";
    case 26: return "reason.online.both_disconnected";
    default: return reason > 0 && reason <= kLastChessReason ? chess::endReasonKey(chess::GameEndReason(reason)) : "";
    }
}

bool directMatchRecord(const net::OnlineGame& og, DirectRecord& out) {
    out = DirectRecord();
    if (og.you < 0 || og.you > 1) return false;
    // An abort by the authority (before the first moves) is not a game. The guest that lost the
    // host for good ends the game itself as ServerAborted (direct_match.cpp, endLocally) at any
    // point, and the authority ends a game that reaches 1200 plies as ServerAborted too
    // (direct_authority.cpp, kMaxPlies): kept unfinished ("*") once both players have moved, the
    // point before which the authority aborts a game that loses a player (NoShow).
    if (og.status == StAborted && (og.reason != kReasonServerAborted || og.moves.size() < 2)) return false;
    out.game.reset();
    for (const net::OnlineGame::MoveRec& m : og.moves) {
        const chess::Position& pos = out.game.position();
        chess::Move mv = pos.findLegal(chess::Square(net::moveFrom(m.move)), chess::Square(net::moveTo(m.move)),
                                       chess::PieceType(net::movePromo(m.move)));
        if (!mv.valid() || !out.game.play(mv)) {
            LOGW("saved games: move %d of the direct match does not follow: not saved", int(out.game.moves().size()) + 1);
            return false;
        }
        out.info.elapsedMs.push_back(int64_t(m.spentMs));
        out.info.clockMs.push_back(int64_t(m.clockMs));   // direct matches are always timed (clock_rules.h)
    }
    archive::GameInfo& info = out.info;
    info.mode = archive::Mode::Direct;
    chess::TimeControl tc;
    tc.unlimited = false;
    tc.baseMs = og.baseMs;
    tc.incrementMs = og.incMs;
    info.timeControl = tc.pgnTag();
    if (og.status == StOngoing) {
        // Left before the authority answered: aborted before the player's first move, otherwise
        // resigned (a draw when the opponent cannot mate, as the authority decides it). A first
        // move sent but not confirmed yet is not in og.moves: the scene resigns then
        // (GameScene::myFirstMoveMade), but nothing is saved rather than a record without the
        // move the authority may have applied before the resignation.
        const bool firstMoveMade = int(og.moves.size()) > og.you;
        if (!firstMoveMade) return false;
        chess::Game ended = out.game;
        ended.resign(chess::Color(og.you));
        info.result = ended.resultString();
        info.endKey = chess::endReasonKey(ended.endReason());
        out.finished = true;
        return true;
    }
    info.result = onlineResult(og.status);
    info.endKey = onlineEndKey(og.reason);
    out.finished = info.result != "*";
    return true;
}

}  // namespace saving
}  // namespace game
