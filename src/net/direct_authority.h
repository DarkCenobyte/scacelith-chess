// The host's game authority for a direct match: a small C++ counterpart of the dedicated
// server's GameRoom (dedicated-server/docs/DESIGN.md sections 6.1 to 6.4) in a simplified,
// unrated form. Deterministic: time is always passed in (epoch milliseconds that advance with
// the monotonic clock, sock::epochMs()), no timer inside; the caller asks nextDeadline() and
// calls tick().
//
// Input: the net::proto client->server game messages (Move, Resign, DrawOffer, DrawAnswer,
// DrawClaim, Abort, Resync, Rematch), encoded, from either side. The host's own UI actions are
// encoded into the same messages and go through the same path as the guest's.
// Output: encoded server->client messages (GameSnapshot, MoveMade, MoveRejected, GameEvent,
// GameEnd, Error) for the host (decoded locally into net::Event) and for the guest (sent over
// the secure channel).
//
// Policies:
//   - Plies 0 and 1 run no clock: each player has firstMoveMs (60 s) for their first move, else
//     the game is aborted (NoShow). As on the server, the guest's first move may arrive later by
//     the most its flag would allow (min(rtt/2 + 30 ms, 500 ms, quota), below); the firstMoveMs
//     sent to the players has no such margin. The clocks start with White's second move; no
//     increment for the first moves.
//   - Clocks on the host: elapsed = arrival - turn start. The guest's move gets a lag
//     compensation of at most min(lag, rtt/2 + 30 ms, 500 ms, quota) where lag = elapsed -
//     thinkMs and the quota starts at 2 s (+100 ms per move, capped at 2 s); the host's moves
//     get none. A move that arrives after the flag is refused (FlagFell) and the game is lost
//     on time (drawn when the opponent cannot mate). Fischer increment after every later move.
//   - Validation order of a Move as in DESIGN 6.2 (game, over, duplicate/stale ply, position
//     digest, turn, legality, clock).
//   - Draws: one pending offer; 3 offers per player per game; not again within 10 plies of a
//     decline; a move declines the opponent's pending offer; an offer while the opponent's is
//     pending is an agreement; claims (threefold, fifty moves) via chess::Game; automatic
//     endings (mate, stalemate, insufficient material, fivefold, 75 moves) via chess::Game.
//   - Resignation at any time while the game runs; abort only before one's own first move.
//   - A game that reaches 1200 plies (no Move can carry a later ply) ends aborted
//     (ServerAborted), as on the server.
//   - Guest disconnected: its clock keeps running; after graceMs (60 s) it loses by
//     Abandonment (draw when the host cannot mate; aborted NoShow before 2 plies).
//   - Rematch within 60 s after the end, both must accept, colours swapped; a decline, the
//     guest's disconnection or the window's end closes it.
//   - autoPress (the host's choice) only travels in every GameSnapshot, rematches included: the
//     clients then send a move when the robot presses the clock by itself, or when the player
//     presses it. The rules above do not depend on it.
#pragma once
#include "chess/chess.h"
#include <cstdint>
#include <string>
#include <vector>

namespace net {
namespace direct {

enum Side : int { HostSide = 0, GuestSide = 1 };

struct AuthorityConfig {
    int64_t baseMs = 600000, incMs = 5000;
    int64_t firstMoveMs = 60000;
    int64_t graceMs = 60000;
    int64_t lagCompMaxMs = 500;
    int64_t lagCompRttMarginMs = 30;
    int64_t lagQuotaMs = 2000;
    int64_t lagQuotaGainMs = 100;
    int drawOffersPerGame = 3;
    int drawOfferCooldownPlies = 10;
    int64_t rematchWindowMs = 60000;
    bool autoPress = true;            // GameSnapshot.autoPress (DirectHostOptions::autoPress)
};

// FNV-1a 32 of the first four FEN fields (the Move.posHash of schema.js): net::positionDigest,
// the digest of online play.
uint32_t fenDigest(const std::string& fen);
uint32_t positionHash(const chess::Position& pos);
// A player name for PlayerInfo: trimmed, ASCII control characters removed, at most maxBytes of
// UTF-8 the protocol accepts (no overlong forms, surrogates or code points above U+10FFFF),
// 'fallback' when empty. Also cleans the router's name before the hosting page shows it.
std::string sanitizeName(const std::string& name, const char* fallback, size_t maxBytes = 24);

class Authority {
public:
    struct Output {
        std::vector<std::vector<uint8_t>> toHost, toGuest;
        bool empty() const { return toHost.empty() && toGuest.empty(); }
        void clear() { toHost.clear(); toGuest.clear(); }
    };

    // hostColorPref: net::proto::ColorPref (0 random, 1 White, 2 Black).
    Authority(const AuthorityConfig& cfg, const std::string& hostName, const std::string& guestName, int hostColorPref,
              uint64_t firstGameId = 1);

    void startGame(double now, Output& out);                 // snapshots to both sides
    void onMessage(Side side, const uint8_t* p, size_t n, double now, Output& out);
    void onDisconnect(Side side, double now, Output& out);   // the guest's link was lost
    void onReconnect(Side side, double now, Output& out);    // back (a snapshot goes to that side)
    void onRtt(Side side, double rttMs);                     // smoothed round trip to that side
    void tick(double now, Output& out);                      // flags, first-move timeouts, grace, rematch window
    double nextDeadline() const;                             // absolute ms, +infinity when none
    void sendSnapshot(Side side, double now, Output& out);
    void hostLeaves(double now, Output& out);                // resigns the host's running game

    bool isOver() const;
    uint64_t gameId() const { return id_; }
    int colorOf(Side side) const { return side == HostSide ? hostColor_ : 1 - hostColor_; }
    int plies() const { return int(recs_.size()); }
    const std::string& guestName() const { return names_[GuestSide]; }

private:
    AuthorityConfig cfg_;
    std::string names_[2];
    uint64_t nextId_;
    uint64_t id_ = 0;
    uint32_t gseq_ = 0;
    bool started_ = false;
    int hostColor_ = 0;
    chess::Game game_;
    struct Rec { uint16_t move; uint32_t spentMs, clockMs; };
    std::vector<Rec> recs_;
    std::vector<std::vector<uint8_t>> made_;   // encoded MoveMade of every ply (idempotent duplicates)
    int64_t remaining_[2] = {0, 0};
    int running_ = 2;
    double turnStart_ = 0, firstMoveDeadline_ = 0, graceDeadline_ = 0, startedAt_ = 0;
    double quota_[2] = {0, 0};
    double rtt_[2] = {-1, -1};                 // by side
    int drawOfferBy_ = 2;
    int drawOffers_[2] = {0, 0};
    int declinedAtPly_[2] = {-1000, -1000};
    uint8_t status_ = 0, reason_ = 0;          // net::proto GameStatus / EndReason
    bool sideConnected_[2] = {true, true};
    int rematchBy_ = 2;
    bool rematchOpen_ = false;
    double rematchDeadline_ = 0;

    template <class T> void emit(Output& out, Side side, const T& msg);
    template <class T> void emitBoth(Output& out, const T& msg);
    void error(Output& out, Side side, uint32_t ref, int code, uint64_t game);
    void gameEvent(Output& out, int kind, int color, uint32_t arg, bool toGuest = true);
    int64_t clockAt(int color, double now) const;
    double compBound() const;
    double flagTime() const;
    double firstMoveTime() const;   // the latest arrival of the first move of the side to move
    bool canOffer(int color) const;
    void finish(int status, int reason, double now, Output& out);
    void finishFromChess(double now, Output& out);
    void flagFall(int color, double now, Output& out);
    void onMove(Side side, const uint8_t* p, size_t n, double now, Output& out);
};

}  // namespace direct
}  // namespace net
