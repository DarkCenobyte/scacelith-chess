// Online play: the game's side of the Scacelith dedicated server (dedicated-server/ in this
// repository; protocol in dedicated-server/src/protocol/schema.js, design in
// dedicated-server/docs/DESIGN.md).
//
// OnlineClient owns a network thread. Every command below returns at once and queues work for
// that thread; results and server pushes come back as Events that the game thread drains with
// poll() once per frame. No command blocks, no callback runs on the game thread by surprise.
//
// Trust boundary: each server is identified by its origin ServerEndpoint::origin()
// ("host:apiPort"). The session token, the pinned certificate and the remembered user name are
// stored per origin (CredentialStore) and are only ever sent to that origin: the WebSocket goes
// to the same host, HTTP redirects are never followed, and switching servers never carries a
// token over. Tokens are encrypted at rest with DPAPI on Windows.
//
// Engine-free (no GL, no UI): compiled into scacelith_core and unit-tested (tests/net_tests.cpp).
//
// Changes to the original contract (client-net):
//   - OnlineClient::setCredentialsFile(path) (additive): where the credential store lives. By
//     default Scacelith.credentials is next to the executable (like Scacelith.ini), or in the
//     user data directory when that directory is not writable; main.cpp may call it with the
//     directory of the --ini file. Tests use it to work in a temporary file.
//   - ServerEndpoint::origin() brackets IPv6 literals ("[::1]:8443") so an origin is unambiguous.
//   - ServerEndpoint::pinnedSha256 accepts "AB:CD:..." too (setServer normalises it to 64 lower-case
//     hex digits). When it is empty, the pin saved for the origin at the last login applies.
//   - Commands that need the realtime connection while it is not Online produce a ServerError
//     event with code 0 and error "offline" ("invalid_request" for out-of-range arguments).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace net {

// Where the server is. The official server (if the build defines SCACELITH_OFFICIAL_SERVER) is
// the default; players may enter a community server instead (Options > Online).
struct ServerEndpoint {
    std::string host;                 // DNS name or IP literal (IPv6 without brackets)
    uint16_t apiPort = 443;           // HTTPS API
    uint16_t wsPort = 443;            // WSS; 0 = the value announced by the server's /api/v1/info
    std::string pinnedSha256;         // optional: hex SHA-256 of the server's leaf certificate
                                      // (self-signed community servers); empty = OS trust store
    bool insecureDev = false;         // plain HTTP/WS for local development servers only
                                      // (refused for anything but localhost / 127.0.0.1 / ::1)
    std::string origin() const;       // "host:apiPort" (lower-case host) - the credential scope
    bool valid() const;
};

// The official server of this build ("" when none): CMake option SCACELITH_OFFICIAL_SERVER
// ("host[:apiPort[:wsPort]]").
ServerEndpoint officialServer();

struct Category {                     // an official (rated) time control
    std::string id;                   // "3+2"
    int baseSec = 0, incSec = 0;
};

struct ServerInfo {
    std::string name, serverId, motd;
    int protocolMin = 0, protocolMax = 0;
    uint32_t schemaHash = 0;
    bool compatible = false;          // our protocol version is within [min, max] and schema matches
    uint16_t wsPort = 0;
    bool registrationOpen = false, emailVerification = false, googleSso = false;
    int powRegisterBits = 0;
    std::vector<Category> categories;
};

struct RatingInfo {
    std::string category;             // "3+2"
    int rating = 1500, games = 0, wins = 0, draws = 0, losses = 0, peak = 1500;
    bool provisional = true;
};

struct AccountInfo {
    uint32_t userId = 0;
    std::string username, email;      // email as the server shows it to its owner
    bool emailVerified = false, mfaEnabled = false, googleLinked = false;
    std::vector<RatingInfo> ratings;
    int64_t bannedUntilMs = 0;        // 0 = not banned
};

struct PlayerInfo {
    uint32_t userId = 0;
    std::string name;
    int rating = 0;
    bool provisional = false;
};

// Authoritative game state as last received from the server (GameSnapshot + later events).
struct OnlineGame {
    uint64_t id = 0;
    std::string category;             // "3+2" or "custom"
    int64_t baseMs = 0, incMs = 0;
    bool rated = false;
    PlayerInfo white, black;
    int you = 0;                      // 0 White, 1 Black, 2 spectator
    struct MoveRec { uint16_t move = 0; uint32_t spentMs = 0, clockMs = 0; };
    std::vector<MoveRec> moves;
    int running = 2;                  // colour whose clock runs, 2 = none
    int64_t whiteMs = 0, blackMs = 0; // remaining at serverTimeMs
    double serverTimeMs = 0;          // server clock of the clock values
    int drawOfferBy = 2;              // 2 = none
    int status = 0, reason = 0;       // net::proto GameStatus / EndReason values
    bool whiteConnected = true, blackConnected = true;
    uint32_t graceMs = 0, firstMoveMs = 0;
    int rematchBy = 2;
};

// Move as sent on the wire: from | to << 6 | promo << 12 (chess::PieceType promo numbering).
inline uint16_t packMove(int from, int to, int promo) { return uint16_t((from & 63) | ((to & 63) << 6) | ((promo & 7) << 12)); }
inline int moveFrom(uint16_t m) { return m & 63; }
inline int moveTo(uint16_t m) { return (m >> 6) & 63; }
inline int movePromo(uint16_t m) { return (m >> 12) & 7; }
// FNV-1a 32 of the first four FEN fields (chess::Position::fen() prefix): the posHash of Move.
uint32_t positionDigest(const std::string& fen);

enum class ConnState {
    Offline,        // no realtime connection
    Connecting,     // TCP/TLS/WebSocket handshake or Hello in progress
    Online,         // Welcome received
    Reconnecting,   // lost; retrying with backoff (a game in progress continues on the server)
    Incompatible,   // protocol/schema mismatch: update the game or the server
    Unauthorized,   // token refused: log in again
    Banned
};

struct Event {
    enum class Kind {
        // ---- HTTPS results (ok = request succeeded; otherwise error holds the server code,
        //      e.g. "invalid_credentials", "rate_limited", "network", "tls", "certificate") ----
        ServerInfoResult,     // info
        RegisterResult,       // ok: verification e-mail sent (or account ready when not required)
        LoginResult,          // ok: logged in (account); mfaRequired: ask for a code, then loginMfa()
        LogoutResult,
        AccountResult,        // account (fetchAccount, and after any account change)
        PasswordResetRequested, VerificationResent, PasswordChanged,
        MfaSetupResult,       // mfaSecret (base32), mfaUri (otpauth://...)
        MfaEnableResult,      // recoveryCodes
        MfaDisableResult, RecoveryCodesResult /* recoveryCodes */,
        SsoBrowserOpened,     // the system browser shows the provider's page; polling
        SsoNeedsUsername,     // first Google login: choose a username, then completeSso()
        ReportResult,
        // ---- realtime ----
        ConnectionChanged,    // state (and error for Incompatible/Unauthorized/Banned)
        Welcome,              // account.username/userId, serverName
        QueueStatus,          // queue*
        ChallengeReceived,    // challenge*
        ChallengeStatus,      // challenge*
        GameSnapshot,         // game: full state; the scene rebuilds the board from it
        MoveMade,             // game (updated), ply, move, flags, spentMs; mine = the confirmation of my move
        MoveRejected,         // ply, move, code; a GameSnapshot follows
        GameEvent,            // gameEventKind, color, arg (draw offer, disconnection...)
        GameEnd,              // game (status, reason, clocks)
        RatingUpdate,         // ratingWhite/ratingBlack before/after
        Notice,               // noticeCode, arg (shutdown, ban, cooldown...)
        ServerError           // code (net::proto::ErrorCode), fatal, gameId
    };
    Kind kind = Kind::ServerInfoResult;
    bool ok = false;
    std::string error;                // HTTPS error code / transport error
    int retryAfterSec = 0;
    ServerInfo info;
    AccountInfo account;
    std::string serverName;
    bool mfaRequired = false;
    std::string mfaSecret, mfaUri;
    std::vector<std::string> recoveryCodes;
    ConnState state = ConnState::Offline;
    // queue
    std::string queueCategory; bool queueRated = false; int queueState = 0; uint32_t queueWaitMs = 0, queueWindow = 0, queued = 0;
    // challenges
    uint32_t challengeId = 0; int challengeState = 0; PlayerInfo challenger; std::string challengeTarget, challengeCode;
    int challengeBaseSec = 0, challengeIncSec = 0; bool challengeRated = false; int challengeColor = 0; uint32_t challengeExpiresMs = 0;
    // game
    OnlineGame game;                  // copy of the state after this event
    int ply = 0; uint16_t move = 0; uint8_t flags = 0; uint32_t spentMs = 0; bool mine = false;
    int code = 0; bool fatal = false; uint64_t gameId = 0;
    int gameEventKind = 0, color = 2; uint32_t arg = 0;
    struct Rating { int before = 0, after = 0, games = 0; bool provisional = false; } ratingWhite, ratingBlack;
    int noticeCode = 0; double noticeArg = 0;
};

class OnlineClient {
public:
    OnlineClient();
    ~OnlineClient();                  // closes the connection and joins the network thread
    OnlineClient(const OnlineClient&) = delete;
    OnlineClient& operator=(const OnlineClient&) = delete;

    // ---- server and account (HTTPS) ----
    void setCredentialsFile(const std::string& path);  // optional; see the note at the top
    void setServer(const ServerEndpoint& ep);    // disconnects if the origin changes
    const ServerEndpoint& server() const;
    void fetchServerInfo();
    bool hasSavedSession() const;                // a token is stored for the current origin
    std::string savedUsername() const;           // last user name used on this origin
    void registerAccount(const std::string& username, const std::string& email, const std::string& password);
    void login(const std::string& usernameOrEmail, const std::string& password);
    void loginMfa(const std::string& code);      // 6 digits, or a recovery code (xxxx-xxxx-xx)
    void startGoogleSso();                       // PKCE + system browser + polling
    void completeSso(const std::string& username);
    void cancelSso();
    void logout(bool allSessions = false);       // server-side revocation + local token erase
    void fetchAccount();
    void resendVerification(const std::string& email);
    void forgotPassword(const std::string& email);
    void changePassword(const std::string& current, const std::string& next);
    void mfaSetup(const std::string& password);
    void mfaEnable(const std::string& code);
    void mfaDisable(const std::string& password, const std::string& codeOrRecovery);
    void regenerateRecoveryCodes(const std::string& password, const std::string& code);
    void report(uint64_t gameId, const std::string& username, const std::string& category, const std::string& comment);

    // ---- realtime (WSS) ----
    void connect();                              // uses the saved session; reconnects automatically until disconnect()
    void disconnect();
    ConnState state() const;
    int pingMs() const;                          // smoothed round trip, -1 when unknown
    double serverNowMs() const;                  // estimate of the server clock (for clock display)
    void joinQueue(const std::string& category, bool rated);
    void leaveQueue();
    void challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref);
    void createPrivateGame(int baseSec, int incSec, bool rated, int colorPref);  // code in ChallengeStatus
    void joinPrivateGame(const std::string& code);
    void acceptChallenge(uint32_t id);
    void declineChallenge(uint32_t id);
    void cancelChallenge(uint32_t id);
    // Sends the move intent at once (call it when the destination is chosen, before animating).
    // fen = the position the move is played in (for posHash); thinkMs = local time since the turn began.
    void sendMove(uint64_t gameId, int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer);
    void resign(uint64_t gameId);
    void offerDraw(uint64_t gameId);
    void answerDraw(uint64_t gameId, bool accept);
    void claimDraw(uint64_t gameId);
    void abortGame(uint64_t gameId);
    void requestResync(uint64_t gameId);
    void rematch(uint64_t gameId, bool accept);
    const OnlineGame* currentGame() const;       // game thread view (updated by poll())

    // Drains one event; call until it returns false, once per frame.
    bool poll(Event& out);

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// The process-wide client used by the game (created on first use).
OnlineClient& onlineClient();

}  // namespace net
