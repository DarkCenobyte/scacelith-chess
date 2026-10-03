// Online play: the game's side of the Scacelith dedicated server (dedicated-server/ in this
// repository; protocol in dedicated-server/src/protocol/schema.js, design in
// dedicated-server/docs/DESIGN.md).
//
// OnlineClient owns network threads. Every command below returns at once and queues work for
// them; results and server pushes come back as Events that the game thread drains with poll()
// once per frame. No command blocks, no callback runs on the game thread by surprise.
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
//     hex digits). When it is empty, the pin saved for the origin at the last login applies,
//     until forgetSavedPin() (Options: the pin field of that server emptied).
//   - Commands that need the realtime connection while it is not Online produce a ServerError
//     event with code 0 and error "offline" ("invalid_request" for out-of-range arguments).
//   - ServerEndpoint::wsPort defaults to 0 = the API port (one port for HTTPS and /ws, as on the
//     official server); effectiveWsPort() resolves it. /api/v1/info's wsPort is informative only.
//     TLS is always used unless insecureDev is set (and insecureDev is refused off loopback).
//   - RetryCause, reconnectDelayMs(), ReconnectBackoff and clientPingIntervalMs() (additive): the
//     reconnection and client Ping pacing rules, pure, so the tests can check them.
//   - Protocol v2 (additive): sendGesture() and Event::Kind::OpponentGesture relay the live
//     gestures of the two players (net/gesture.h), and OnlineGame::autoPress tells whether the
//     robots press the clock by themselves in the game.
//   - Account API (additive): the game history, a game's details and PGN, the signed-in devices,
//     the challenge preference, the e-mail change, the data export and the account deletion
//     (fetchMyGames ... deleteAccount below; dedicated-server/docs/API.md). A 401 answer to any
//     call that carried the session token erases that token (the session expired or was revoked)
//     and sets Event::sessionLost; such a call, or one that needs the session while none is saved,
//     fails with "unauthorized" whatever the server's code (it says invalid_token).
//   - Animated GIFs (additive): downloadGameGif() and renderPgnGif() ask the server for the GIF of a
//     game of its own (GET /games/:id/gif) or of any game given as PGN text (POST /gif); the file
//     comes back in Event::Kind::GifResult (signed-in players only: the renders count against the
//     account's quota; a refused or missing session is "unauthorized", as above).
//   - Event::origin (additive): the HTTPS results name the server their command went to.
//   - Google sign-in by loopback redirect (decisions 36a and 35A; dedicated-server/docs/API.md):
//     startGoogleSso(page) listens on 127.0.0.1 (net/loopback_redirect.h) before it starts, opens
//     Google's page only when it is Google's (its redirect URI names this listener and the origin
//     of the server in use), and Google sends the browser back to the game: no polling. The game
//     then finishes with the server (SsoCodeReceived, then LoginResult, SsoNeedsUsername or the new
//     SsoNeedsPassword). linkSso(password) adds Google sign-in to the existing account of that
//     address once its password (then its code, loginMfa()) is entered in the game.
//     setBrowserOpener() and setSsoMinWaitMs() are for the tests.
#pragma once
#include "gesture.h"
#include "loopback_redirect.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net {

namespace proto { struct GameSnapshot; }   // protocol_gen.h

// Where the server is. The build's official server (officialServer() below) is the default;
// players may enter a community server instead (Options > Online).
struct ServerEndpoint {
    std::string host;                 // DNS name or IP literal (IPv6 without brackets)
    uint16_t apiPort = 443;           // HTTPS API
    uint16_t wsPort = 0;              // WSS; 0 (left empty) = apiPort (API and /ws on one port)
    std::string pinnedSha256;         // optional: hex SHA-256 of the server's leaf certificate
                                      // (self-signed community servers); empty = OS trust store
    bool insecureDev = false;         // plain HTTP/WS for local development servers only
                                      // (refused for anything but localhost / 127.0.0.1 / ::1)
    std::string origin() const;       // "host:apiPort" (lower-case host) - the credential scope
    bool valid() const;
    uint16_t effectiveWsPort() const { return wsPort ? wsPort : apiPort; }
};

// The official server of this build: caissa.scacelith.com, HTTPS API and WSS on port 443
// (https://caissa.scacelith.com/api/v1, wss://caissa.scacelith.com/ws), trust store, no pin. The
// CMake option SCACELITH_OFFICIAL_SERVER ("host[:apiPort[:wsPort]]", apiPort 443 and wsPort =
// apiPort when omitted) replaces it; "none" builds without one (host ""). The official server
// used port 44664 before: a session saved for "<official host>:44664" moves once to the official
// origin (CredentialStore::addOriginMove), so its players stay signed in.
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
    // Account API additions (GET /account/me):
    bool hasPassword = true;          // false: a Google-only account (no password set yet)
    bool acceptChallenges = true;     // preferences.acceptChallenges == "all"
    std::string pendingEmail;         // a requested e-mail change waiting for its confirmation link
    int64_t createdAtMs = 0, lastLoginAtMs = 0;
};

// ---- Account API: game history, game details, sessions (HTTPS) -----------------------------------
struct GameSide {                     // one player of a finished server game
    std::string name;                 // "deleted#123" once that account was deleted
    int rating = 0;                   // at the start of the game (0 = unknown)
    int ratingAfter = 0, ratingDiff = 0;
    bool ratingChanged = false;       // a rated game that changed the ratings: ratingAfter / ratingDiff hold
};

// A finished game of a server: GET /account/games (the player's history) and the head of GameDetails.
struct GameSummary {
    uint64_t id = 0;
    std::string category;             // "3+2", or "custom"
    bool rated = false;
    int64_t baseMs = 0, incMs = 0;
    GameSide white, black;
    int you = 2;                      // 0 White, 1 Black, 2 not one of the players
    int status = 0, reason = 0;       // net::proto GameStatus / EndReason
    std::string result = "*";         // "1-0", "0-1", "1/2-1/2", "*" (aborted)
    int plies = 0;
    int64_t startedAtMs = 0, endedAtMs = 0;
};

// One game in full: GET /games/:id.
struct GameDetails : GameSummary {
    struct Ply {
        uint16_t move = 0;            // packMove() form, from the UCI text
        std::string uci;              // "e2e4", "e7e8q"
        int64_t spentMs = -1;         // time the mover was charged for it, -1 = unknown
        int64_t clockMs = -1;         // the mover's clock after it (increment included), -1 = unknown
    };
    std::vector<Ply> moves;
    uint64_t rematchOf = 0;
    bool reportable = false;          // the caller may report the opponent (POST /reports)
};

// Filter of the history (empty / -1 = everything).
struct GamesFilter {
    std::string category;             // "", "custom" or an official category id ("3+2")
    int rated = -1;                   // -1 all, 0 casual only, 1 rated only
    std::string result;               // "", "win", "loss", "draw"
};

struct GamesPage {
    uint64_t before = 0;              // the cursor of the request (0 = the first page)
    GamesFilter filter;               // the filter of the request (with 'before', on errors too)
    std::vector<GameSummary> games;   // newest first
    uint64_t next = 0;                // 'before' of the next page, 0 = this was the last page
    int total = 0;                    // games matching the filter, all pages together
};

struct SessionInfo {                  // a signed-in device (GET /auth/sessions)
    int64_t id = 0;
    int64_t createdAtMs = 0, lastSeenAtMs = 0, expiresAtMs = 0;
    std::string clientLabel;          // as the signing-in client gave it, e.g. "Scacelith/0.1.0 win64"
                                      // (clientString() of this game); "" when it gave none
    bool current = false;             // the session of this game
};

// How the server draws the animated GIF of a game (GET /games/:id/gif, POST /gif): the size of
// the board ("small", "medium", "large"), the side at the bottom ("white", "black"), the time each
// move stays on screen in milliseconds (100..3000; the server refuses other values) and the
// coordinates on the border.
struct GifOptions {
    std::string size = "medium";
    std::string orientation = "white";
    int delayMs = 500;
    bool coords = true;
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
    // The robots press the clock by themselves once a move is on the board (the server's
    // AUTO_PRESS_CLOCK, or the host's choice in a direct match). When false, the move is sent
    // only when the player presses the clock, so their clock runs until then.
    bool autoPress = true;
};

// Move as sent on the wire: from | to << 6 | promo << 12 (chess::PieceType promo numbering).
inline uint16_t packMove(int from, int to, int promo) { return uint16_t((from & 63) | ((to & 63) << 6) | ((promo & 7) << 12)); }
inline int moveFrom(uint16_t m) { return m & 63; }
inline int moveTo(uint16_t m) { return (m >> 6) & 63; }
inline int movePromo(uint16_t m) { return (m >> 12) & 7; }
// FNV-1a 32 of the first four FEN fields (chess::Position::fen() prefix): the posHash of Move.
uint32_t positionDigest(const std::string& fen);
// The game of a decoded GameSnapshot (OnlineClient; DirectMatch with the host's messages).
OnlineGame onlineGameFromSnapshot(const proto::GameSnapshot& s);

enum class ConnState {
    Offline,        // no realtime connection
    Connecting,     // TCP/TLS/WebSocket handshake or Hello in progress
    Online,         // Welcome received
    Reconnecting,   // lost; retrying with backoff (a game in progress continues on the server)
    Incompatible,   // protocol/schema mismatch: update the game or the server
    Unauthorized,   // token refused: log in again
    Banned
};

// ---- Reconnection and ping pacing (pure; the network thread uses them, the tests check them) ----

// Why the realtime connection is being established again.
enum class RetryCause {
    Failure,        // network error, timeout, dropped connection, upgrade refused (not 503), the
                    // first 503 at the upgrade after a shutdown (the server is restarting)
    ServerFull,     // HTTP 503 at the WebSocket upgrade, close 4006 or a fatal Error{ServerFull}
    Shutdown        // close 4008, a fatal Error{ShuttingDown} or a Notice{ServerShutdown} before the drop
};

// Delay before automatic reconnection attempt number `attempt` (0 = the first one, counted by
// ReconnectBackoff below). u is a uniform random number in [0, 1).
//   Failure     full jitter: uniform in [0.5 s, min(30 s, 2 s x 2^attempt)]
//   ServerFull  uniform in [60 s, 120 s]
//   Shutdown    attempt 0: uniform in [5 s, 35 s], which spreads the reconnection wave of a
//               restart; later attempts as Failure
// gameInProgress: the player has a game running on the server, which gives them only its
// reconnection grace to come back before they lose by abandonment: 10 % of the base time within
// RECONNECT_GRACE_MIN_MS and RECONNECT_GRACE_MAX_MS (15 s to 60 s by default, so 15 s for the fast
// games), and RECOVERY_GRACE_MS (90 s by default) for a game the server restored after a restart.
// Every cause then waits uniform in [0.5 s, min(8 s, 2 s x 2^attempt)], and a shutdown's first
// attempt uniform in [1 s, 8 s].
// retryAfterMs is a Retry-After the server gave (0 = none): the delay is then at least that, plus
// up to half of it so that the clients it was given to do not come back together (10 minutes at
// most), even during a game (the 8 s bound above does not apply then). User-initiated
// connections (connect(), a server change) never wait for any of this.
uint32_t reconnectDelayMs(int attempt, RetryCause cause, double u, bool gameInProgress, uint32_t retryAfterMs);

// The attempt number of reconnectDelayMs() across connections. A connection that reached Welcome
// sets it back to 0 only once it has stayed up for kStableMs: a server that closes right after
// Welcome (a crash loop, a refusal of every connection) is tried again with growing delays, not
// every 0.5 s to 2 s for ever. The spread first attempt of a shutdown stays apart from the count:
// it follows any connection that reached Welcome. Times are steady-clock milliseconds (the tests
// give their own).
class ReconnectBackoff {
public:
    static constexpr double kStableMs = 60000.0;
    void reset() { attempt_ = 0; welcomedAtMs_ = -1.0; }   // connect(): from the shortest delay
    void welcomed(double nowMs) { welcomedAtMs_ = nowMs; }  // the connection reached Welcome
    // The delay before the next attempt, after a failed attempt or a lost connection at nowMs.
    uint32_t next(double nowMs, RetryCause cause, double u, bool gameInProgress, uint32_t retryAfterMs);

private:
    int attempt_ = 0;
    double welcomedAtMs_ = -1.0;   // Welcome of the connection lost next; -1 = none since the last next()
};

// Interval of the client's own Ping for Welcome.clientPingMs (the server's
// CLIENT_PING_INTERVAL_MS): 0 (not announced) = 10 s, otherwise clamped to 1 s .. 60 s.
uint32_t clientPingIntervalMs(uint32_t announced);

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
        SsoBrowserOpened,     // the system browser shows Google's page, which sends it back to the
                              // game's loopback redirect on 127.0.0.1 (no polling)
        SsoNeedsUsername,     // first Google login: choose a username, then completeSso()
        SsoCodeReceived,      // Google sent the browser back: the game finishes with the server
        SsoNeedsPassword,     // the Google address is the one of an account with a password
                              // (account.username): enter that password with linkSso()
        ReportResult,
        // ---- HTTPS, account API ----
        GamesResult,          // gamesPage (fetchMyGames)
        GameDetailsResult,    // gameDetails (fetchGame)
        PgnResult,            // gameId, text = the PGN (downloadPgn)
        SessionsResult,       // sessions (fetchSessions)
        SessionRevoked,       // sessionId (revokeSession)
        PreferencesResult,    // ok: account.acceptChallenges updated (setAcceptChallenges)
        EmailChangeResult,    // status: "verification_sent" (link mailed to the new address) or
                              // "email_changed" (servers without e-mail confirmation) (changeEmail)
        AccountExportResult,  // text = the JSON document (exportAccount)
        AccountDeleted,       // ok: the account is gone and the local session erased (deleteAccount)
        GifResult,            // text = the GIF file, gameId = the game (0 for a PGN text)
                              // (downloadGameGif, renderPgnGif); rate_limited (the account's quota)
                              // and server_busy (the renderer is full) come with retryAfterSec
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
        ServerError,          // code (net::proto::ErrorCode), fatal, gameId
        OpponentGesture       // gesture, gameId: the opponent's live gestures in the current game
                              // (cosmetic; 'game' is not filled in, a newer one replaces one still
                              // queued)
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
    Gesture gesture;
    // account API
    GamesPage gamesPage;
    GameDetails gameDetails;
    std::vector<SessionInfo> sessions;
    int64_t sessionId = 0;
    std::string status;               // EmailChangeResult
    std::string text;                 // PgnResult, AccountExportResult, GifResult (the file's bytes)
    // HTTPS: the saved session was refused (401) during this call, and its token erased: the player
    // is signed out. The error is then "unauthorized", or none when a public read was asked again
    // without the token and answered (fetchGame, downloadPgn).
    bool sessionLost = false;
    // HTTPS results: the origin (ServerEndpoint::origin()) of the server the command went to, the
    // one in use when it was given; "" for the realtime events. An answer that arrives after
    // setServer() chose another server names the previous one.
    std::string origin;
};

class OnlineClient {
public:
    OnlineClient();
    ~OnlineClient();                  // closes the connection and joins the network threads
    OnlineClient(const OnlineClient&) = delete;
    OnlineClient& operator=(const OnlineClient&) = delete;

    // ---- server and account (HTTPS) ----
    void setCredentialsFile(const std::string& path);  // optional; see the note at the top
    void setServer(const ServerEndpoint& ep);    // disconnects if the origin changes
    // Forgets the pin saved for the current origin at sign-in (the session stays): with no pin in
    // the endpoint, its requests trust the system's certificates again. Done on net-http, after the
    // commands already queued (a sign-in queued before would save the pin again), or as the client
    // ends if it ends first.
    void forgetSavedPin();
    const ServerEndpoint& server() const;
    // ignoreSavedPin: this request goes without the pin saved for the origin (the endpoint's own
    // still applies), as every request will after forgetSavedPin() (Options' "Test connection").
    void fetchServerInfo(bool ignoreSavedPin = false);
    bool hasSavedSession() const;                // a token is stored for the current origin and decrypts here
    std::string savedUsername() const;           // last user name used on this origin
    void registerAccount(const std::string& username, const std::string& email, const std::string& password);
    void login(const std::string& usernameOrEmail, const std::string& password);
    void loginMfa(const std::string& code);      // 6 digits, or a recovery code (xxxx-xxxx-xx)
    // PKCE + the loopback redirect on 127.0.0.1 + the system browser; page = the texts of the page
    // the browser shows when Google sends it back (made on the game thread).
    void startGoogleSso(const SsoBrowserPage& page);
    void completeSso(const std::string& username);
    // After SsoNeedsPassword: the account's password (POST /auth/sso/google/link). A wrong password
    // ("invalid_credentials") or too many ("too_many_attempts") keep the step for another try;
    // mfaRequired continues with loginMfa(), which adds Google sign-in once the code is accepted.
    void linkSso(const std::string& password);
    // Stops the Google sign-in under way (its listener, a password step): LoginResult "cancelled"
    // when there was one. A start still waiting for the server opens no browser and answers
    // SsoBrowserOpened "cancelled".
    void cancelSso();
    // Tests: what opens Google's page (default net::sys::openBrowser; false = it could not), and the
    // shortest wait for Google's redirect (30 s by default, whatever expiresIn the server gives).
    void setBrowserOpener(std::function<bool(const std::string& url)> opener);
    void setSsoMinWaitMs(int ms);
    // Server-side revocation + local token erase. This session: erased whatever the server says
    // (LogoutResult ok, a refused token included). Every session (allSessions): ok only when the
    // server did it; the token is kept when it failed, except a refused one (401, "unauthorized").
    void logout(bool allSessions = false);
    void fetchAccount();
    void resendVerification(const std::string& email);
    void forgotPassword(const std::string& email);
    void changePassword(const std::string& current, const std::string& next);
    void mfaSetup(const std::string& password);
    void mfaEnable(const std::string& code);
    void mfaDisable(const std::string& password, const std::string& codeOrRecovery);
    void regenerateRecoveryCodes(const std::string& password, const std::string& code);
    void report(uint64_t gameId, const std::string& username, const std::string& category, const std::string& comment);

    // ---- account API (HTTPS; dedicated-server/docs/API.md) ----
    // Answers that do not have the documented shape (a move that is not UCI text, a PGN that does
    // not start with its tags, an export that is not the export document...) come back with error
    // "invalid_response", like a PGN over 4 MiB or an export over 64 MiB.
    // The signed-in player's finished games, newest first (GET /account/games): before = 0 for the
    // first page, then GamesPage::next; limit 1..50 (0 or less: the server's 20, more: 50).
    void fetchMyGames(uint64_t before, int limit, const GamesFilter& filter);
    // GET /games/:id, with the session token when one is saved (the players then get `you` and
    // `reportable`); a refused token is erased and the public answer asked for instead.
    void fetchGame(uint64_t gameId);
    void downloadPgn(uint64_t gameId);                                  // GET /games/:id/pgn (text)
    void fetchSessions();                                               // GET /auth/sessions
    // DELETE /auth/sessions/:id. Revoking the session marked current in the last fetchSessions()
    // signs this game out (token erased, realtime connection stopped), as logout() would: its
    // realtime connection closes first, and opens again as deleteAccount()'s on a failure.
    void revokeSession(int64_t sessionId);
    void setAcceptChallenges(bool accept);                              // PUT /account/preferences
    // Re-authenticated changes. codeOrRecovery: "" when two-factor is off (no field sent), a
    // 6-digit code ("code") or a recovery code ("recoveryCode").
    void changeEmail(const std::string& newEmail, const std::string& password, const std::string& codeOrRecovery);
    void exportAccount(const std::string& password, const std::string& codeOrRecovery);   // text = the JSON
    // The realtime connection closes first (the server closes every connection of the account it
    // deletes). On success the token and the user name saved for the origin are erased and it stays
    // closed; AccountDeleted then comes with ok. A failure other than a refused session
    // ("unauthorized") opens it again if it was open.
    void deleteAccount(const std::string& password, const std::string& codeOrRecovery);

    // ---- animated GIFs (HTTPS; dedicated-server/docs/API.md) ----
    // The server draws the game (a 2D board seen from above, a frame per move) and answers with the
    // .gif file: GifResult, text = the file (16 MiB at most; an answer that does not start with
    // "GIF87a" or "GIF89a" is "invalid_response"). Signed-in players only ("unauthorized" without
    // a saved token, nothing sent; "unauthorized" with sessionLost when the server refuses the
    // saved session, its token erased): each render counts against the account's quota (429
    // "rate_limited" with retryAfterSec; a GIF the server rendered before costs nothing), and a
    // busy renderer answers 503 "server_busy" with retryAfterSec (so does the 503 "busy" of a
    // locked database). "game_too_long" over the server's limit of moves (GIF_MAX_PLIES),
    // "render_failed" when the server could not make it, "gif_disabled" on a server without GIFs:
    // turned off, or an older server without these routes (its 404 "not_found" for POST /gif, and
    // for GET /games/:id/gif when GET /games/:id finds the game). The GIFs have a thread of their
    // own (one at a time, the other calls never wait for them) and kGifTimeoutMs: the server may
    // hold the request for 45 s with its default settings before it answers 503 "timeout".
    // A game of the server: GET /games/:id/gif?size=&orientation=&delay=&coords=0|1 (gameId 0:
    // "invalid_game_id", nothing sent).
    void downloadGameGif(uint64_t gameId, const GifOptions& options);
    // Any game: POST /gif { pgn, size, orientation, delayMs, coords }, the first game of the text
    // (at most kGifMaxPgnBytes: "pgn_too_large" above, nothing sent; "invalid_pgn" when the server
    // cannot read it). GifResult's gameId is 0.
    void renderPgnGif(const std::string& pgn, const GifOptions& options);
    static constexpr size_t kGifMaxPgnBytes = 65536;   // the server's limit of the pgn field
    static constexpr size_t kGifMaxBytes = size_t(16) << 20;
    // How long a GIF request may take, answer included: twice the server's own bound (queue
    // GIF_QUEUE_TIMEOUT_MS 10 s + render GIF_RENDER_TIMEOUT_MS 30 s + 5 s), against the 15 s of the
    // other calls ("timeout" after it).
    static constexpr int kGifTimeoutMs = 90000;

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
    // Live gestures (cosmetic, net/gesture.h): only the latest state is kept, and it is sent at
    // the rate the server announced in Welcome (gestureRate / gestureBurst; nothing when 0).
    // Dropped while not Online (never queued for a reconnection) and when gameId is not the game
    // of the last GameSnapshot. Cheap enough to call every frame.
    void sendGesture(uint64_t gameId, const Gesture& g);

    // Drains one event; call until it returns false, once per frame.
    bool poll(Event& out);

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// The process-wide client used by the game (created on first use).
OnlineClient& onlineClient();

}  // namespace net
