// In-process stand-ins for the Scacelith dedicated server and for a direct-match peer: the whole
// online game can be played, and every menu tried, without any network.
//
// FakeServer answers every net::OnlineClient command with the events the real server would send
// (same net::Event values, realistic delays: ~0.3 s for the HTTPS API, ~17 ms one way and a
// ~35 ms ping for the realtime connection). Accounts accept any password; matchmaking pairs you
// after 2 s with a fake opponent who plays legal moves (a mate in one when it sees one, a good
// capture now and then, otherwise at random) with plausible thinking times; the fake keeps the
// authoritative clocks (first-move timers, increments, flags) and the result like the server.
// FakeDirect does the same for net::DirectMatch (a fake UPnP router and a fake friend).
//
// The fake opponent also sends its live gestures (OpponentGesture, the rules of net/gesture.h):
// its head at 4-6 Hz (towards the piece in hand or its square, wandering over the board and
// leaning in while it thinks, a look at its clock after pressing it, a glance at its scoresheet
// after each move), the piece it touches 0.4-1.3 s before moving, sometimes aimed elsewhere
// first, aimed at its square 250-400 ms before; when its game has autoPress off, the move is
// placed first and pressed 0.6-1.0 s later. Silent while it is away and once the game is over.
// The local player's gestures are accepted and ignored.
//
// The account API (dedicated-server/docs/API.md) answers like the server: a game history made
// from the account's name (the same ~45 games every time: legal moves, every kind of ending,
// clocks that follow the time control, ratings that lead to the account's ones; the games played
// against the fake are added at the top), served page by page with its filters; the details and
// the PGN of each game (the server's format: its tags, [%clk]/[%emt] comments, the end reason);
// four signed-in devices (this one, two others, one without a label); the challenge preference
// (no demo challenge when it is off); an e-mail change waiting for its link, which the fake
// "opens" 40 s later; the data export (five attempts an hour, failed ones included, checked
// before the password as the server's router does) and the deletion of the account. Animated
// GIFs of the history's games and of PGN texts: a small but valid GIF89a drawn by the fake (a tiny
// board, a frame per move; the tests decode it), after a render time that grows with the game,
// with the server's quota per account (four renders a minute, thirty an hour; a GIF asked again
// costs nothing) and its checks (options, unreadable PGN, more than 600 moves).
//
// Special inputs to try the error paths: user name "banned", "unverified" or "ratelimited",
// password "wrong", a user name containing "mfa" (asks for a code), a custom server host
// containing "offline", "badcert" or "old" (network, certificate, incompatible version); direct
// match: address "refused.test", "timeout.test" or "unknown.test", a code that is not 12
// characters, a code starting with "2222" (wrong code), host port 47199 (no UPnP router) or
// 47198 (carrier-grade NAT). Account API: a user name containing "newbie" (no games yet),
// password "wrong" when changing the e-mail, exporting or deleting (invalid_password), with
// two-factor on no code (mfa_code_required) or "000000" (invalid_code), a new address equal to
// the current one (same_email) or without "@" and a dot (invalid_email), a host containing
// "noverify" (a server without e-mail confirmation: the address changes at once; one containing
// "taken" is refused, email_taken). GIFs: game id 429 (rate_limited, retry after 2 min 30 s) or
// 503 (server_busy, retry after 8 s), a PGN whose White or Black is "ratelimited" or "serverbusy"
// (the same), a host containing "nogif" (gif_disabled).
//
// Used by src/game/online_stub.cpp (the OnlineClient / DirectMatch implementation of builds
// without the real network layer, i.e. without SCACELITH_NET_REAL) and meant to back a
// --online-mock developer mode next to the real implementation. Engine-free, single-threaded:
// everything happens inside poll().
#pragma once
#include "../net/direct_match.h"
#include "../net/online_client.h"
#include <cstdint>
#include <memory>
#include <string>

namespace net {
namespace mock {

// Clock of the fakes (server clock, epoch milliseconds). Wall clock by default; with the virtual
// clock time only moves through advance() (deterministic screenshots, --warp).
void useVirtualClock(bool on);
bool virtualClock();
void advance(double ms);
double nowMs();

// FNV-1a 32 of the first four FEN fields (the protocol's position digest).
uint32_t digest(const std::string& fen);

// Developer switches (keys F9 / F10 in the game with --online-mock): the fake opponent leaves for
// 'seconds', or our own realtime connection drops for 'seconds'.
void opponentDrop(int seconds);
void connectionDrop(int seconds);
// --online-manual-clock (with --online-mock): the games of the fakes, direct matches included,
// have autoPress = false, so a move goes only when its player presses the clock. Off by default.
void useManualClock(bool on);
bool manualClock();

class FakeServer {
public:
    FakeServer();
    ~FakeServer();
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    void setServer(const ServerEndpoint& ep);
    void forgetSavedPin() {}          // the fakes have no certificates
    const ServerEndpoint& server() const;
    void fetchServerInfo();
    bool hasSavedSession() const;
    std::string savedUsername() const;
    void registerAccount(const std::string& username, const std::string& email, const std::string& password);
    void login(const std::string& usernameOrEmail, const std::string& password);
    void loginMfa(const std::string& code);
    void startGoogleSso();
    void completeSso(const std::string& username);
    void cancelSso();
    void logout(bool allSessions);
    void fetchAccount();
    void resendVerification(const std::string& email);
    void forgotPassword(const std::string& email);
    void changePassword(const std::string& current, const std::string& next);
    void mfaSetup(const std::string& password);
    void mfaEnable(const std::string& code);
    void mfaDisable(const std::string& password, const std::string& codeOrRecovery);
    void regenerateRecoveryCodes(const std::string& password, const std::string& code);
    void report(uint64_t gameId, const std::string& username, const std::string& category, const std::string& comment);
    void fetchMyGames(uint64_t before, int limit, const GamesFilter& filter);
    void fetchGame(uint64_t gameId);
    void downloadPgn(uint64_t gameId);
    void fetchSessions();
    void revokeSession(int64_t sessionId);
    void setAcceptChallenges(bool accept);
    void changeEmail(const std::string& newEmail, const std::string& password, const std::string& codeOrRecovery);
    void exportAccount(const std::string& password, const std::string& codeOrRecovery);
    void deleteAccount(const std::string& password, const std::string& codeOrRecovery);
    void downloadGameGif(uint64_t gameId, const GifOptions& options);
    void renderPgnGif(const std::string& pgn, const GifOptions& options);

    void connect();
    void disconnect();
    ConnState state() const;
    int pingMs() const;
    double serverNowMs() const;
    void joinQueue(const std::string& category, bool rated);
    void leaveQueue();
    void challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref);
    void createPrivateGame(int baseSec, int incSec, bool rated, int colorPref);
    void joinPrivateGame(const std::string& code);
    void acceptChallenge(uint32_t id);
    void declineChallenge(uint32_t id);
    void cancelChallenge(uint32_t id);
    void sendMove(uint64_t gameId, int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer);
    void resign(uint64_t gameId);
    void offerDraw(uint64_t gameId);
    void answerDraw(uint64_t gameId, bool accept);
    void claimDraw(uint64_t gameId);
    void abortGame(uint64_t gameId);
    void requestResync(uint64_t gameId);
    void rematch(uint64_t gameId, bool accept);
    void sendGesture(uint64_t gameId, const Gesture& g);
    bool poll(Event& out);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

class FakeDirect {
public:
    FakeDirect();
    ~FakeDirect();
    FakeDirect(const FakeDirect&) = delete;
    FakeDirect& operator=(const FakeDirect&) = delete;

    void host(const DirectHostOptions& opt);
    void join(const std::string& address, uint16_t port, const std::string& code, const std::string& playerName);
    void close();
    DirectMatch::State state() const;
    std::string lastError() const;
    DirectInvite invite() const;
    UpnpStatus upnp() const;
    bool isHost() const;
    void sendMove(int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer);
    void resign();
    void offerDraw();
    void answerDraw(bool accept);
    void claimDraw();
    void abortGame();
    void requestResync();
    void rematch(bool accept);
    void sendGesture(const Gesture& g);
    const OnlineGame* currentGame() const;
    int pingMs() const;
    double serverNowMs() const;
    bool poll(Event& out);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mock
}  // namespace net
