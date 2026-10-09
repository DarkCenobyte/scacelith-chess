// What the game scene saves in the folder of saved games (game_archive.h), apart from the scene so
// that it is unit-tested (tests/game_saving_tests.cpp): which archive mode a game of the scene is,
// and the record of a direct match as its authority reports it. Engine-free (no GL, no Stockfish).
//
// The scene (GameScene::archiveGame, game_scene.cpp) saves a game once: when it ends, when the
// player leaves it (the Esc menu; leaving a game against Stockfish resigns it, a hot-seat or coach
// game left is saved unfinished, "*"), and as a safety net when the menu comes back or the window
// is closed in the middle of a game. archive::shouldSave decides with the mode found here.
#pragma once
#include "../chess/chess.h"
#include "../net/online_client.h"
#include "game_archive.h"
#include "game_mode.h"
#include <string>

namespace game {
namespace saving {

// The archive mode of a game of the scene: Play, Coach (shouldSave refuses the rules lesson),
// HotSeat as themselves; Online: Direct for a direct match, else Server (the server keeps its
// games: never saved here); Watch and Replay: Watch (never saved).
archive::Mode archiveMode(GameMode mode, bool directMatch);

// The ending of an online game as its authority reports it (net::OnlineGame status and reason, the
// values of protocol/scacelith-v1.json): "1-0", "0-1", "1/2-1/2", or "*" while it is
// ongoing and for an aborted game; the i18n key of the reason ("reason.checkmate" for the chess
// reasons 1..14, "reason.online.abandonment"... from 20), "" for none and for a value of a later
// protocol minor.
std::string onlineResult(int status);
std::string onlineEndKey(int reason);

// What is saved of a direct match: its moves as the authority recorded them (the local game can be
// behind: an opponent's move still on its way to their robot), each with the clock time charged
// for it ([%emt]) and the mover's clock after it, increment included ([%clk]); its time control;
// its ending. The names, ratings and start time are the scene's.
struct DirectRecord {
    chess::Game game;
    archive::GameInfo info;      // mode, timeControl, elapsedMs, clockMs, result, endKey
    bool finished = false;       // it has a result
};
// 'og.you' is the local player's colour (0 White, 1 Black). When the authority still reports the
// game as ongoing, the player is leaving it (the Esc menu, the window closed): the resignation or
// the abort they just sent is not answered yet. Before their first move the game is aborted and
// nothing is saved (nor while that move is sent but not confirmed: the scene resigns then, but
// og.moves lacks the move); after it, it is saved as their resignation, the result the opponent's
// copy gets (a draw, ResignationVsInsufficient, when the opponent cannot mate). A game the guest ended itself when the host was gone for good (status Aborted, reason
// ServerAborted) is saved unfinished ("*") once both players have moved. False when nothing is to
// be saved: a game aborted by the authority, a spectator, or moves that do not follow one another
// (never expected from an authority; logged).
bool directMatchRecord(const net::OnlineGame& og, DirectRecord& out);

}  // namespace saving
}  // namespace game
