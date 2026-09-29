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
// Special inputs to try the error paths: user name "banned", "unverified" or "ratelimited",
// password "wrong", a user name containing "mfa" (asks for a code), a custom server host
// containing "offline", "badcert" or "old" (network, certificate, incompatible version); direct
// match: address "refused.test", "timeout.test" or "unknown.test", a code that is not 12
// characters, a code starting with "2222" (wrong code), host port 47199 (no UPnP router) or
// 47198 (carrier-grade NAT).
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

class FakeServer {
public:
    FakeServer();
    ~FakeServer();
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    void setServer(const ServerEndpoint& ep);
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
    const OnlineGame* currentGame() const;
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
