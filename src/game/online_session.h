// Online play on the game's side. One OnlineSession per process (onlineSession()) sits between
// the network layer (net::OnlineClient for the Scacelith server, net::DirectMatch for a direct
// match) and the game, and is the only code that polls them:
//   - the menus (ui/ui_screens_online*.cpp, ui/ui_screens_account.cpp) read its state (server
//     info, account, connection, matchmaking, challenges, the account API's data: game history,
//     the game opened from it, signed-in devices) and send commands through api() / direct();
//     the history's game page and the saved games (ui/ui_library.cpp) ask for animated GIFs here
//     (saveGameGif / savePgnGif), whose files the session writes when the server answers;
//   - the 3D scene plays the games it announces (gameReady() / takeGame()) through a GameLink and
//     drains their events with nextGameEvent(), the opponent's live gestures (OpponentGesture)
//     included: only those of the game being played, the latest one replacing one still queued.
// With --online-mock the in-process fakes of online_mock.h replace the network layer;
// --online-manual-clock then makes their games autoPress = false (the moves wait for a clock
// press). Tokens never pass through here: the network layer stores them per server.
#pragma once
#include "../net/direct_match.h"
#include "../net/online_client.h"
#include "game_link.h"
#include "online_account.h"
#include "online_live.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace game {

// The commands of net::OnlineClient, for the real client or the fake server.
class ServerApi {
public:
    virtual ~ServerApi() = default;
    virtual void setServer(const net::ServerEndpoint& ep) = 0;
    virtual void forgetSavedPin() = 0;
    virtual void fetchServerInfo() = 0;
    virtual bool hasSavedSession() const = 0;
    virtual std::string savedUsername() const = 0;
    virtual void registerAccount(const std::string& username, const std::string& email, const std::string& password) = 0;
    virtual void login(const std::string& usernameOrEmail, const std::string& password) = 0;
    virtual void loginMfa(const std::string& code) = 0;
    virtual void startGoogleSso() = 0;
    virtual void completeSso(const std::string& username) = 0;
    virtual void cancelSso() = 0;
    virtual void logout(bool allSessions) = 0;
    virtual void fetchAccount() = 0;
    virtual void resendVerification(const std::string& email) = 0;
    virtual void forgotPassword(const std::string& email) = 0;
    virtual void changePassword(const std::string& current, const std::string& next) = 0;
    virtual void mfaSetup(const std::string& password) = 0;
    virtual void mfaEnable(const std::string& code) = 0;
    virtual void mfaDisable(const std::string& password, const std::string& codeOrRecovery) = 0;
    virtual void regenerateRecoveryCodes(const std::string& password, const std::string& code) = 0;
    virtual void report(uint64_t gameId, const std::string& username, const std::string& category, const std::string& comment) = 0;
    virtual void fetchMyGames(uint64_t before, int limit, const net::GamesFilter& filter) = 0;
    virtual void fetchGame(uint64_t gameId) = 0;
    virtual void downloadPgn(uint64_t gameId) = 0;
    virtual void fetchSessions() = 0;
    virtual void revokeSession(int64_t sessionId) = 0;
    virtual void setAcceptChallenges(bool accept) = 0;
    virtual void changeEmail(const std::string& newEmail, const std::string& password, const std::string& codeOrRecovery) = 0;
    virtual void exportAccount(const std::string& password, const std::string& codeOrRecovery) = 0;
    virtual void deleteAccount(const std::string& password, const std::string& codeOrRecovery) = 0;
    virtual void downloadGameGif(uint64_t gameId, const net::GifOptions& options) = 0;
    virtual void renderPgnGif(const std::string& pgn, const net::GifOptions& options) = 0;
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual net::ConnState state() const = 0;
    virtual int pingMs() const = 0;
    virtual double serverNowMs() const = 0;
    virtual void joinQueue(const std::string& category, bool rated) = 0;
    virtual void leaveQueue() = 0;
    virtual void challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref) = 0;
    virtual void createPrivateGame(int baseSec, int incSec, bool rated, int colorPref) = 0;
    virtual void joinPrivateGame(const std::string& code) = 0;
    virtual void acceptChallenge(uint32_t id) = 0;
    virtual void declineChallenge(uint32_t id) = 0;
    virtual void cancelChallenge(uint32_t id) = 0;
    virtual void sendMove(uint64_t gameId, int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) = 0;
    virtual void resign(uint64_t gameId) = 0;
    virtual void offerDraw(uint64_t gameId) = 0;
    virtual void answerDraw(uint64_t gameId, bool accept) = 0;
    virtual void claimDraw(uint64_t gameId) = 0;
    virtual void abortGame(uint64_t gameId) = 0;
    virtual void requestResync(uint64_t gameId) = 0;
    virtual void rematch(uint64_t gameId, bool accept) = 0;
    virtual void sendGesture(uint64_t gameId, const net::Gesture& g) = 0;
    virtual bool poll(net::Event& out) = 0;
};

// The commands of net::DirectMatch, for the real one or the fake peer.
class DirectApi {
public:
    virtual ~DirectApi() = default;
    virtual void host(const net::DirectHostOptions& opt) = 0;
    virtual void join(const std::string& address, uint16_t port, const std::string& code, const std::string& playerName) = 0;
    virtual void close() = 0;
    virtual net::DirectMatch::State state() const = 0;
    virtual std::string lastError() const = 0;
    virtual net::DirectInvite invite() const = 0;
    virtual net::UpnpStatus upnp() const = 0;
    virtual bool isHost() const = 0;
    virtual void sendMove(int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) = 0;
    virtual void resign() = 0;
    virtual void offerDraw() = 0;
    virtual void answerDraw(bool accept) = 0;
    virtual void claimDraw() = 0;
    virtual void abortGame() = 0;
    virtual void requestResync() = 0;
    virtual void rematch(bool accept) = 0;
    virtual void sendGesture(const net::Gesture& g) = 0;
    virtual int pingMs() const = 0;
    virtual double serverNowMs() const = 0;
    virtual bool poll(net::Event& out) = 0;
};

class OnlineSession {
public:
    OnlineSession();
    ~OnlineSession();

    // Chooses the backends (call once, before anything else): mock = the fakes of online_mock.h.
    // virtualClock: the fakes' time only moves with update(dt) (screenshots, --warp).
    void init(bool mock, bool virtualClock);
    bool mock() const { return mock_; }
    // Once per frame (menus and games alike): polls the network layer, advances the virtual
    // clock. Game events wait in a queue for the scene.
    void update(float dt);
    // The scene says, before update(), whether a game is being played (any kind: online, direct,
    // or on this PC while connected). Some toasts wait until none is: the RatingRestored notice
    // shows in the menus, on the game over card, or at once when no game is going on.
    void setInGame(bool inGame) { inGame_ = inGame; }

    ServerApi& api();
    DirectApi& direct();

    // ---- Server ---------------------------------------------------------------------------------
    net::ServerEndpoint endpoint() const;      // from game::settings() [online]
    bool serverConfigured() const { return endpoint().valid(); }
    bool officialAvailable() const;            // this build has an official server
    // After the [online] settings changed: selects the server (each keeps its own session) and
    // forgets what was known of the previous one. Its answers still on the way are dropped when
    // they arrive (ServerAnswers), except a GIF (written) and a PGN (saved by the game page), each
    // as the game it was asked for.
    void applyServer();
    const net::ServerInfo& info() const { return info_; }
    bool infoKnown() const { return infoKnown_; }
    const std::string& infoError() const { return infoError_; }
    void refreshInfo();                         // fetchServerInfo(), result in info()
    std::string serverName() const;             // info().name, or the host
    // Options > Online "Test connection": fetches the info of 'ep' (the values being edited,
    // not applied yet) and comes back to the current server. The result: takeTest().
    void testServer(const net::ServerEndpoint& ep);
    bool testing() const { return testing_; }
    bool takeTest(net::Event& out);

    // ---- Account --------------------------------------------------------------------------------
    // Signed in: a login this run, or a session saved for this server (resumed with resume()).
    bool signedIn() const { return signedIn_; }
    void resume();                              // saved session: connect and fetch the account
    // A session of the configured server saved on this computer, resumed or not (the Online menu
    // not opened yet this run): the requests that carry the token (the GIFs) work without
    // resume(). False before init() (nothing is started for it). Its user name, or "".
    bool hasSavedSession() const;
    std::string savedUsername() const;
    const net::AccountInfo& account() const { return account_; }
    const net::RatingInfo* rating(const std::string& category) const;
    void signOut(bool everywhere);

    // ---- HTTPS requests -------------------------------------------------------------------------
    // The menus send a command with api() and call expect() with the kind of its result:
    // busy() is true until it arrives, take() hands it over once.
    void expect(net::Event::Kind k);
    bool busy(net::Event::Kind k) const;
    bool take(net::Event::Kind k, net::Event& out);

    // ---- Account API (game history, devices, preferences) -----------------------------------------
    // What the account pages show, kept from the answers (AccountData::apply). The requests below
    // send the command and expect() its result, so busy() and take() work as for the others; an
    // account deleted (AccountDeleted) signs out here, a changed e-mail fetches the account again.
    const AccountData& accountData() const { return data_; }
    void loadHistory(const net::GamesFilter& filter);   // its first page
    void historyNext();
    void historyPrevious();
    void historyReload();                               // the page shown, again (after an error)
    void openGame(uint64_t gameId);                     // accountData().game
    void loadSessions();
    void revokeSession(int64_t sessionId);
    void setAcceptChallenges(bool accept);

    // ---- Animated GIFs (a game of the history, a game of the saved games) -----------------------
    // The GIF of a game of this server (GET /games/:id/gif) or of any game as PGN text (POST /gif),
    // written to 'folder' as 'fileName' (gifFileName) when the server's answer arrives, whatever
    // page shows then (GifSaver; owner = what the page shows its state for). One at a time: false,
    // nothing sent, while another one is being made. A GIF that ends while no page shows its state
    // (gifShown() not called for its owner lately) is announced by a notification.
    bool saveGameGif(const std::string& owner, uint64_t gameId, const net::GifOptions& options, const std::string& folder,
                     const std::string& fileName);
    bool savePgnGif(const std::string& owner, const std::string& pgn, const net::GifOptions& options, const std::string& folder,
                    const std::string& fileName);
    const GifSaver& gif() const { return gif_; }
    void gifShown(const std::string& owner);   // a page shows the state of this owner's GIF this frame
    void clearGif();                           // forget the last GIF's state (not one being made)

    // ---- Realtime -------------------------------------------------------------------------------
    net::ConnState conn() const { return conn_; }
    int pingMs() const;                         // of the connection in use (server, or the direct peer)
    struct Queue {
        bool searching = false;
        std::string category;
        bool rated = false;
        double sinceMs = 0;                     // nowMs() when the search began (elapsed time)
        uint32_t window = 0, queued = 0;
    };
    const Queue& queue() const { return queue_; }
    void findOpponent(const std::string& category, bool rated);
    void cancelSearch();
    struct Outgoing {                           // our challenge or private game
        bool active = false;
        uint32_t id = 0;
        std::string target, code;
        int baseSec = 0, incSec = 0;
        bool rated = false;
    };
    const Outgoing& outgoing() const { return outgoing_; }
    void challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref);
    void createPrivateGame(int baseSec, int incSec, bool rated, int colorPref);
    void joinPrivateGame(const std::string& code);
    bool joining() const { return joining_; }   // a join sent: neither its game nor an error came yet
    void cancelOutgoing();
    struct Incoming {                           // a challenge received
        uint32_t id = 0;
        net::PlayerInfo from;
        int baseSec = 0, incSec = 0;
        bool rated = false;
        int yourColor = 0;                      // net ColorPref offered to us
        double expiresMs = 0;                   // nowMs() of the expiry
    };
    const std::vector<Incoming>& incoming() const { return incoming_; }
    void answerChallenge(uint32_t id, bool accept);
    double cooldownUntilMs() const { return cooldownUntilMs_; }   // matchmaking cooldown (epoch ms)
    double bannedUntilMs() const { return bannedUntilMs_; }
    double nowMs() const;                       // the clock of these values

    // ---- Direct match ---------------------------------------------------------------------------
    void hostDirect(const net::DirectHostOptions& opt);
    void joinDirect(const std::string& address, uint16_t port, const std::string& code);
    void closeDirect();
    bool directActive() const { return directUsed_; }

    // ---- Games ----------------------------------------------------------------------------------
    // A game started (matchmaking, challenge, rematch, direct match): the scene takes it.
    bool gameReady() const { return gameReady_; }
    GameLink* takeGame(net::OnlineGame& snapshot);
    GameLink* link() { return link_.get(); }
    bool nextGameEvent(net::Event& e);
    // The scene left the game (back to the menu): a direct match is closed.
    void leaveGame();
    // --start-online: signs in (the saved session; with the fakes, any name) and looks for an
    // opponent in 'category' as soon as the connection is up. With the fakes and a virtual clock
    // the game is ready on return.
    void quickStart(const std::string& category, const std::string& username);
    // Fakes with a virtual clock (UI viewer, screenshots): runs them for 'ms' of virtual time, then
    // waits for a GIF file being written.
    void runMock(double ms);

private:
    void handleServer(net::Event& e);   // a GIF's bytes are moved out of e (GifSaver::finish)
    void handleDirect(const net::Event& e);
    void routeGame(const net::Event& e, LinkKind from);
    std::unique_ptr<GameLink> makeLink(LinkKind kind, uint64_t id);
    void resetAccountState();

    bool mock_ = false, virtual_ = false, ready_ = false;
    std::unique_ptr<ServerApi> api_;
    std::unique_ptr<DirectApi> direct_;
    bool directUsed_ = false;
    net::ConnState directConn_ = net::ConnState::Offline;   // of the direct match (a guest reconnects)

    bool infoKnown_ = false;
    bool testing_ = false, testSwitched_ = false, testDone_ = false;
    std::string testOrigin_;                    // of the server being tested
    net::Event testResult_;
    std::string infoError_;
    net::ServerInfo info_;
    bool signedIn_ = false;
    net::AccountInfo account_;
    AccountData data_;
    GifSaver gif_;
    double gifShownAt_ = -1e9;                  // steady seconds of the last gifShown() of gif_'s owner
    std::string serverNameRt_;
    ServerAnswers answers_;                     // for expect(), busy(), take(); the server in use
    net::ServerEndpoint applied_;               // the last applyServer()'s

    net::ConnState conn_ = net::ConnState::Offline;
    Queue queue_;
    Outgoing outgoing_;
    std::vector<Outgoing> cancelledEarly_;      // cancelled before the server named them: cancelled then
    bool joining_ = false;
    std::vector<Incoming> incoming_;
    bool quietNotFound_ = false;                // the next ChallengeNotFound answers a reconnection's cancel
    double cooldownUntilMs_ = 0, bannedUntilMs_ = 0;
    std::string autoQueue_;                     // --start-online
    bool inGame_ = false;
    live::HeldNotice ratingRestored_;           // RatingRestored points waiting for the end of the game

    bool gameReady_ = false;
    uint64_t gameId_ = 0;
    LinkKind gameKind_ = LinkKind::Server;
    net::OnlineGame snapshot_;
    std::unique_ptr<GameLink> link_;
    std::deque<net::Event> gameEvents_;
};

OnlineSession& onlineSession();

// Friendly texts (i18n) of the network layer's errors: an HTTPS error code is onlineErrorText()
// (online_account.h); a realtime net::proto ErrorCode, a direct match error ("refused",
// "timeout", "wrong_code", "incompatible", "port_in_use"... see net::DirectMatch::lastError()).
// The error of a GIF in words (GifSaver::error()): the account's quota used up with the wait in
// minutes and seconds, the renderer busy, signed out, a game too long, a PGN the server cannot
// read, a render that failed, a server without GIFs, the file not written; the other codes as
// onlineErrorText().
std::string gifErrorText(const std::string& code, int retryAfterSec = 0);
std::string serverErrorText(int code);
// Text of a ServerError event: its ErrorCode, or its transport error ("offline": a command sent
// while not connected, which the network layer drops).
std::string eventErrorText(const net::Event& e);
std::string directErrorText(const std::string& code);

}  // namespace game
