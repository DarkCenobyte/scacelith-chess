// Scacelith realtime protocol codec (C++17): generated from dedicated-server/src/protocol/schema.js
// by dedicated-server/tools/gen-protocol-cpp.js, do not edit. Regenerate with
// `node dedicated-server/tools/gen-protocol-cpp.js` after changing the schema.
//
// Wire format and field meanings: schema.js; API: dedicated-server/docs/DESIGN.md section 5.1.
//   encode(m, out)       appends the message (type byte + fields, little-endian) to out. Strings
//                        longer than their bound are cut and lists longer than theirs are
//                        shortened so the frame stays well formed; check valid(m) first when
//                        the values come from user input.
//   decode(p, n, out)    false on any malformed input (wrong type byte, truncation, trailing
//                        bytes, value out of range or opts bounds, unknown enum value, bool not
//                        0/1, non-finite f64, id53 >= 2^53, string not UTF-8 or with NUL or
//                        outside its length bounds, list too long). out may be partly written.
//   valid(m)             true when decode(encode(m)) would succeed and give m back.
//   peekType(p, n, t)    type of a message (false when empty or unknown).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// X11 defines None as a macro; enum values below are called None.
#pragma push_macro("None")
#undef None

namespace net {
namespace proto {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kProtocolMin = 1;
constexpr uint32_t kSchemaHash = 0x992093ccu;
constexpr const char* kWsSubprotocol = "scacelith.v1";
constexpr uint64_t kId53Limit = 1ull << 53;   // id53 values are below 2^53

// ---- enums (u8 on the wire) ----
enum class Color : uint8_t { White = 0, Black = 1, None = 2 };
enum class ColorPref : uint8_t { Random = 0, White = 1, Black = 2 };
enum class GameStatus : uint8_t { Ongoing = 0, WhiteWins = 1, BlackWins = 2, Draw = 3, Aborted = 4 };
enum class EndReason : uint8_t {
    None = 0, Checkmate = 1, Resignation = 2, Timeout = 3, IllegalMoves = 4, Stalemate = 5,
    InsufficientMaterial = 6, TimeoutVsInsufficient = 7, FivefoldRepetition = 8, SeventyFiveMoves = 9,
    ThreefoldClaim = 10, FiftyMoveClaim = 11, Agreement = 12, IllegalMovesVsInsufficient = 13,
    Abandonment = 20, AbandonmentVsInsufficient = 21, Aborted = 22, NoShow = 23, Forfeit = 24,
    ServerAborted = 25, BothDisconnected = 26,
};
enum class GameEventKind : uint8_t {
    DrawOffered = 1, DrawDeclined = 2, PlayerDisconnected = 3, PlayerReconnected = 4,
    RematchOffered = 5, RematchDeclined = 6, AbortAvailable = 7,
};
enum class QueueState : uint8_t { Left = 0, Searching = 1, Matched = 2 };
enum class ChallengeState : uint8_t {
    Pending = 0, Accepted = 1, Declined = 2, Cancelled = 3, Expired = 4, Unavailable = 5,
};
enum class NoticeCode : uint8_t {
    ServerShutdown = 1, Banned = 2, SessionRevoked = 3, MatchmakingCooldown = 4,
    ReplacedByNewConnection = 5, Motd = 6,
};
// SCHEMA BUG: ErrorCode has values above 255 (ProtocolViolation=300, Flood=301, CheatDetected=302, SlowConsumer=303)
// but enums are u8 on the wire: those values cannot be sent (the frame carries value & 0xFF,
// which decode refuses). The C++ type is wider only so that this header compiles.
enum class ErrorCode : uint16_t {
    Malformed = 1, UnsupportedProtocol = 2, Unauthorized = 3, Banned = 4, RateLimited = 5,
    ServerFull = 6, Replaced = 7, ShuttingDown = 8, Internal = 9, HelloRequired = 10,
    EmailUnverified = 11, NotInGame = 100, NotYourTurn = 101, IllegalMove = 102, StalePly = 103,
    Desync = 104, GameOver = 105, AlreadyInGame = 106, InvalidCategory = 107, DrawOfferLimit = 108,
    NothingToClaim = 109, AbortNotAllowed = 110, NoPendingOffer = 111, FlagFell = 112,
    QueueNotAllowed = 200, ChallengeNotFound = 201, UserUnavailable = 202, ChallengeLimit = 203,
    CannotChallengeSelf = 204, CodeInvalid = 205, RatedRequiresOfficialTc = 206,
    MatchmakingCooldown = 207, InvalidTimeControl = 208, RematchUnavailable = 209,
    ProtocolViolation = 300, Flood = 301, CheatDetected = 302, SlowConsumer = 303,
};

// Membership of the schema enums, and their value names ("?" when not a member).
bool isValid(Color v);
bool isValid(ColorPref v);
bool isValid(GameStatus v);
bool isValid(EndReason v);
bool isValid(GameEventKind v);
bool isValid(QueueState v);
bool isValid(ChallengeState v);
bool isValid(NoticeCode v);
bool isValid(ErrorCode v);
const char* enumName(Color v);
const char* enumName(ColorPref v);
const char* enumName(GameStatus v);
const char* enumName(EndReason v);
const char* enumName(GameEventKind v);
const char* enumName(QueueState v);
const char* enumName(ChallengeState v);
const char* enumName(NoticeCode v);
const char* enumName(ErrorCode v);

// Move flags (MoveMade.flags, bit set).
namespace MoveFlag {
constexpr uint8_t Capture = 1;
constexpr uint8_t EnPassant = 2;
constexpr uint8_t CastleKing = 4;
constexpr uint8_t CastleQueen = 8;
constexpr uint8_t DoublePush = 16;
constexpr uint8_t Promotion = 32;
constexpr uint8_t Check = 64;
constexpr uint8_t Mate = 128;
}  // namespace MoveFlag

// WebSocket close codes used by the server (4000 + ErrorCode where one applies).
namespace CloseCode {
constexpr uint16_t Normal = 1000;
constexpr uint16_t GoingAway = 1001;
constexpr uint16_t ProtocolError = 1002;
constexpr uint16_t Unsupported = 1003;
constexpr uint16_t Policy = 1008;
constexpr uint16_t TooBig = 1009;
constexpr uint16_t Internal = 1011;
constexpr uint16_t UnsupportedProtocol = 4002;
constexpr uint16_t Unauthorized = 4003;
constexpr uint16_t Banned = 4004;
constexpr uint16_t Replaced = 4007;
constexpr uint16_t ShuttingDown = 4008;
constexpr uint16_t HelloTimeout = 4010;
constexpr uint16_t ProtocolViolation = 4300;
constexpr uint16_t Flood = 4301;
constexpr uint16_t CheatDetected = 4302;
constexpr uint16_t SlowConsumer = 4303;
}  // namespace CloseCode

// ---- message types (0x01-0x7F client -> server, 0x80-0xFF server -> client) ----
enum class MsgType : uint8_t {
    Hello = 0x01,
    C_Ping = 0x02,
    C_Pong = 0x03,
    QueueJoin = 0x10,
    QueueLeave = 0x11,
    ChallengeCreate = 0x12,
    ChallengeAccept = 0x13,
    ChallengeDecline = 0x14,
    ChallengeCancel = 0x15,
    ChallengeJoinCode = 0x16,
    Move = 0x20,
    Resign = 0x21,
    DrawOffer = 0x22,
    DrawAnswer = 0x23,
    DrawClaim = 0x24,
    Abort = 0x25,
    Resync = 0x26,
    Rematch = 0x27,
    Welcome = 0x80,
    Error = 0x81,
    S_Ping = 0x82,
    S_Pong = 0x83,
    Ack = 0x84,
    Notice = 0x85,
    QueueStatus = 0x90,
    ChallengeReceived = 0x91,
    ChallengeStatus = 0x92,
    GameSnapshot = 0xA0,
    MoveMade = 0xA1,
    MoveRejected = 0xA2,
    GameEvent = 0xA3,
    GameEnd = 0xA4,
    RatingUpdate = 0xA5,
};
const char* messageName(MsgType t);     // "Move", "S_Ping"...; nullptr when unknown
inline bool isClientType(uint8_t t) { return t >= 0x01 && t <= 0x7F; }
bool peekType(const uint8_t* p, size_t n, MsgType& t);

// ---- structs ----
struct PlayerInfo {
    uint32_t userId = 0;
    std::string name;  // min 1, max 24
    uint16_t rating = 0;
    bool provisional = false;
};
struct MoveRec {
    uint16_t move = 0;  // max 32767
    uint32_t spentMs = 0;
    uint32_t clockMs = 0;
};
struct RatingChange {
    uint16_t before = 0;
    uint16_t after = 0;
    uint32_t games = 0;
    bool provisional = false;
};

// ---- messages ----
// First message of a connection. The token is the session token from the HTTPS login of *this*
// server.
struct Hello {
    static constexpr MsgType kType = MsgType::Hello;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint16_t proto = 0;
    uint32_t schema = 0;
    std::string client;  // max 48
    std::string token;  // min 16, max 160
};
// Client round-trip measurement (at most one per second); answered by Pong with the server clock.
struct C_Ping {
    static constexpr MsgType kType = MsgType::C_Ping;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint32_t nonce = 0;
};
// Answer to the server Ping (echo its nonce at once).
struct C_Pong {
    static constexpr MsgType kType = MsgType::C_Pong;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint32_t nonce = 0;
};
// Join the matchmaking queue of an official category ("3+2"). rated=false: casual queue.
struct QueueJoin {
    static constexpr MsgType kType = MsgType::QueueJoin;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    std::string category;  // min 3, max 7
    bool rated = false;
};
struct QueueLeave {
    static constexpr MsgType kType = MsgType::QueueLeave;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
};
// Challenge a player by name, or (empty target) create a private game joined with a code. Rated
// only with an official category.
struct ChallengeCreate {
    static constexpr MsgType kType = MsgType::ChallengeCreate;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    std::string target;  // max 24
    uint16_t baseSec = 0;  // min 15, max 10800
    uint8_t incSec = 0;  // max 180
    bool rated = false;
    ColorPref color = ColorPref::Random;
};
struct ChallengeAccept {
    static constexpr MsgType kType = MsgType::ChallengeAccept;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint32_t id = 0;
};
struct ChallengeDecline {
    static constexpr MsgType kType = MsgType::ChallengeDecline;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint32_t id = 0;
};
struct ChallengeCancel {
    static constexpr MsgType kType = MsgType::ChallengeCancel;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint32_t id = 0;
};
struct ChallengeJoinCode {
    static constexpr MsgType kType = MsgType::ChallengeJoinCode;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    std::string code;  // min 4, max 12
};
// Move intent for ply `ply` of game `game`. posHash is the digest of the position the client
// played in; thinkMs the client-measured time since the turn began (used only for bounded lag
// compensation). drawOffer: the move comes with a draw offer.
struct Move {
    static constexpr MsgType kType = MsgType::Move;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
    uint16_t ply = 0;  // max 1199
    uint16_t move = 0;  // max 32767
    uint32_t posHash = 0;
    uint32_t thinkMs = 0;
    bool drawOffer = false;
};
struct Resign {
    static constexpr MsgType kType = MsgType::Resign;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
};
struct DrawOffer {
    static constexpr MsgType kType = MsgType::DrawOffer;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
};
struct DrawAnswer {
    static constexpr MsgType kType = MsgType::DrawAnswer;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
    bool accept = false;
};
// Claim a draw by threefold repetition or the fifty-move rule in the current position.
struct DrawClaim {
    static constexpr MsgType kType = MsgType::DrawClaim;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
};
// Abort before one's own first move (no rating change).
struct Abort {
    static constexpr MsgType kType = MsgType::Abort;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
};
// Ask for a full GameSnapshot.
struct Resync {
    static constexpr MsgType kType = MsgType::Resync;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
};
// After the end: accept=true offers (or accepts) a rematch with colours swapped, accept=false
// declines or withdraws.
struct Rematch {
    static constexpr MsgType kType = MsgType::Rematch;
    static constexpr bool kClientToServer = true;
    uint32_t seq = 0;
    uint64_t game = 0;
    bool accept = false;
};
// Hello accepted. When activeGame != 0 a GameSnapshot follows.
struct Welcome {
    static constexpr MsgType kType = MsgType::Welcome;
    static constexpr bool kClientToServer = false;
    uint16_t proto = 0;
    double serverTime = 0.0;
    uint32_t userId = 0;
    std::string username;  // max 24
    std::string serverName;  // max 64
    uint32_t heartbeatMs = 0;
    uint16_t maxMsgPerSec = 0;
    uint64_t activeGame = 0;
};
// A request was refused. fatal: the server closes the connection after it.
struct Error {
    static constexpr MsgType kType = MsgType::Error;
    static constexpr bool kClientToServer = false;
    uint32_t ref = 0;
    ErrorCode code = ErrorCode::Malformed;
    bool fatal = false;
    uint64_t game = 0;
};
// Heartbeat; answer with Pong at once (the server measures the latency with it).
struct S_Ping {
    static constexpr MsgType kType = MsgType::S_Ping;
    static constexpr bool kClientToServer = false;
    uint32_t nonce = 0;
    double serverTime = 0.0;
};
struct S_Pong {
    static constexpr MsgType kType = MsgType::S_Pong;
    static constexpr bool kClientToServer = false;
    uint32_t nonce = 0;
    double serverTime = 0.0;
};
// Request accepted (for requests without another answer).
struct Ack {
    static constexpr MsgType kType = MsgType::Ack;
    static constexpr bool kClientToServer = false;
    uint32_t ref = 0;
};
struct Notice {
    static constexpr MsgType kType = MsgType::Notice;
    static constexpr bool kClientToServer = false;
    NoticeCode code = NoticeCode::ServerShutdown;
    double arg = 0.0;
};
// Sent on join, every few seconds while searching, and on leave/match.
struct QueueStatus {
    static constexpr MsgType kType = MsgType::QueueStatus;
    static constexpr bool kClientToServer = false;
    std::string category;  // max 7
    bool rated = false;
    QueueState state = QueueState::Left;
    uint32_t waitMs = 0;
    uint16_t window = 0;
    uint32_t queued = 0;
};
// yourColor is the colour offered to the receiver.
struct ChallengeReceived {
    static constexpr MsgType kType = MsgType::ChallengeReceived;
    static constexpr bool kClientToServer = false;
    uint32_t id = 0;
    PlayerInfo from;
    uint16_t baseSec = 0;
    uint8_t incSec = 0;
    bool rated = false;
    ColorPref yourColor = ColorPref::Random;
    uint32_t expiresMs = 0;
};
// State of a challenge for its creator (and for the receiver when it is cancelled or expires).
// code: private game code.
struct ChallengeStatus {
    static constexpr MsgType kType = MsgType::ChallengeStatus;
    static constexpr bool kClientToServer = false;
    uint32_t id = 0;
    ChallengeState state = ChallengeState::Pending;
    std::string target;  // max 24
    std::string code;  // max 12
    uint16_t baseSec = 0;
    uint8_t incSec = 0;
    bool rated = false;
};
// Complete authoritative state of a game: sent when it starts, after a (re)connection and on
// Resync. Clocks are the remaining times at serverTime; the `running` side keeps counting from
// there.
struct GameSnapshot {
    static constexpr MsgType kType = MsgType::GameSnapshot;
    static constexpr bool kClientToServer = false;
    uint64_t game = 0;
    uint32_t gseq = 0;
    std::string category;  // max 7
    uint32_t baseMs = 0;
    uint32_t incMs = 0;
    bool rated = false;
    PlayerInfo white;
    PlayerInfo black;
    Color you = Color::White;
    std::vector<MoveRec> moves;  // max 1200
    Color running = Color::White;
    uint32_t whiteMs = 0;
    uint32_t blackMs = 0;
    double serverTime = 0.0;
    Color drawOffer = Color::White;
    GameStatus status = GameStatus::Ongoing;
    EndReason reason = EndReason::None;
    bool whiteConnected = false;
    bool blackConnected = false;
    uint32_t graceMs = 0;
    uint32_t firstMoveMs = 0;
    double startedAt = 0.0;
    Color rematch = Color::White;
};
// A move accepted by the server, sent to both players (for the mover it is the confirmation).
// Clocks as in GameSnapshot; firstMoveMs: time the next player has for their first move (0 when
// not applicable).
struct MoveMade {
    static constexpr MsgType kType = MsgType::MoveMade;
    static constexpr bool kClientToServer = false;
    uint64_t game = 0;
    uint32_t gseq = 0;
    uint16_t ply = 0;
    uint16_t move = 0;
    uint8_t flags = 0;
    uint32_t spentMs = 0;
    uint32_t whiteMs = 0;
    uint32_t blackMs = 0;
    double serverTime = 0.0;
    bool drawOffer = false;
    uint32_t firstMoveMs = 0;
};
// The move intent was refused (never shown to the opponent). A GameSnapshot follows when the
// client must resynchronise.
struct MoveRejected {
    static constexpr MsgType kType = MsgType::MoveRejected;
    static constexpr bool kClientToServer = false;
    uint64_t game = 0;
    uint16_t ply = 0;
    uint16_t move = 0;
    ErrorCode code = ErrorCode::Malformed;
};
struct GameEvent {
    static constexpr MsgType kType = MsgType::GameEvent;
    static constexpr bool kClientToServer = false;
    uint64_t game = 0;
    uint32_t gseq = 0;
    GameEventKind kind = GameEventKind::DrawOffered;
    Color color = Color::White;
    uint32_t arg = 0;
};
// Final result. Ratings follow in RatingUpdate once committed (rated games).
struct GameEnd {
    static constexpr MsgType kType = MsgType::GameEnd;
    static constexpr bool kClientToServer = false;
    uint64_t game = 0;
    uint32_t gseq = 0;
    GameStatus status = GameStatus::Ongoing;
    EndReason reason = EndReason::None;
    uint32_t whiteMs = 0;
    uint32_t blackMs = 0;
    double serverTime = 0.0;
};
// Rating changes of a finished rated game, sent after the database transaction committed.
struct RatingUpdate {
    static constexpr MsgType kType = MsgType::RatingUpdate;
    static constexpr bool kClientToServer = false;
    uint64_t game = 0;
    std::string category;  // max 7
    RatingChange white;
    RatingChange black;
};

// ---- codec ----
bool valid(const PlayerInfo& s);
bool valid(const MoveRec& s);
bool valid(const RatingChange& s);
void encode(const Hello& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Hello& out);
bool valid(const Hello& m);
void encode(const C_Ping& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, C_Ping& out);
bool valid(const C_Ping& m);
void encode(const C_Pong& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, C_Pong& out);
bool valid(const C_Pong& m);
void encode(const QueueJoin& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, QueueJoin& out);
bool valid(const QueueJoin& m);
void encode(const QueueLeave& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, QueueLeave& out);
bool valid(const QueueLeave& m);
void encode(const ChallengeCreate& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeCreate& out);
bool valid(const ChallengeCreate& m);
void encode(const ChallengeAccept& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeAccept& out);
bool valid(const ChallengeAccept& m);
void encode(const ChallengeDecline& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeDecline& out);
bool valid(const ChallengeDecline& m);
void encode(const ChallengeCancel& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeCancel& out);
bool valid(const ChallengeCancel& m);
void encode(const ChallengeJoinCode& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeJoinCode& out);
bool valid(const ChallengeJoinCode& m);
void encode(const Move& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Move& out);
bool valid(const Move& m);
void encode(const Resign& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Resign& out);
bool valid(const Resign& m);
void encode(const DrawOffer& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, DrawOffer& out);
bool valid(const DrawOffer& m);
void encode(const DrawAnswer& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, DrawAnswer& out);
bool valid(const DrawAnswer& m);
void encode(const DrawClaim& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, DrawClaim& out);
bool valid(const DrawClaim& m);
void encode(const Abort& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Abort& out);
bool valid(const Abort& m);
void encode(const Resync& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Resync& out);
bool valid(const Resync& m);
void encode(const Rematch& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Rematch& out);
bool valid(const Rematch& m);
void encode(const Welcome& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Welcome& out);
bool valid(const Welcome& m);
void encode(const Error& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Error& out);
bool valid(const Error& m);
void encode(const S_Ping& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, S_Ping& out);
bool valid(const S_Ping& m);
void encode(const S_Pong& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, S_Pong& out);
bool valid(const S_Pong& m);
void encode(const Ack& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Ack& out);
bool valid(const Ack& m);
void encode(const Notice& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, Notice& out);
bool valid(const Notice& m);
void encode(const QueueStatus& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, QueueStatus& out);
bool valid(const QueueStatus& m);
void encode(const ChallengeReceived& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeReceived& out);
bool valid(const ChallengeReceived& m);
void encode(const ChallengeStatus& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, ChallengeStatus& out);
bool valid(const ChallengeStatus& m);
void encode(const GameSnapshot& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, GameSnapshot& out);
bool valid(const GameSnapshot& m);
void encode(const MoveMade& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, MoveMade& out);
bool valid(const MoveMade& m);
void encode(const MoveRejected& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, MoveRejected& out);
bool valid(const MoveRejected& m);
void encode(const GameEvent& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, GameEvent& out);
bool valid(const GameEvent& m);
void encode(const GameEnd& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, GameEnd& out);
bool valid(const GameEnd& m);
void encode(const RatingUpdate& m, std::vector<uint8_t>& out);
bool decode(const uint8_t* p, size_t n, RatingUpdate& out);
bool valid(const RatingUpdate& m);

// ---- reflection (tests, logs): v(name, field) for every field, in wire order ----
template <class V> void visitFields(PlayerInfo& m, V&& v) {
    v("userId", m.userId);
    v("name", m.name);
    v("rating", m.rating);
    v("provisional", m.provisional);
}
template <class V> void visitFields(const PlayerInfo& m, V&& v) {
    v("userId", m.userId);
    v("name", m.name);
    v("rating", m.rating);
    v("provisional", m.provisional);
}
template <class V> void visitFields(MoveRec& m, V&& v) {
    v("move", m.move);
    v("spentMs", m.spentMs);
    v("clockMs", m.clockMs);
}
template <class V> void visitFields(const MoveRec& m, V&& v) {
    v("move", m.move);
    v("spentMs", m.spentMs);
    v("clockMs", m.clockMs);
}
template <class V> void visitFields(RatingChange& m, V&& v) {
    v("before", m.before);
    v("after", m.after);
    v("games", m.games);
    v("provisional", m.provisional);
}
template <class V> void visitFields(const RatingChange& m, V&& v) {
    v("before", m.before);
    v("after", m.after);
    v("games", m.games);
    v("provisional", m.provisional);
}
template <class V> void visitFields(Hello& m, V&& v) {
    v("seq", m.seq);
    v("proto", m.proto);
    v("schema", m.schema);
    v("client", m.client);
    v("token", m.token);
}
template <class V> void visitFields(const Hello& m, V&& v) {
    v("seq", m.seq);
    v("proto", m.proto);
    v("schema", m.schema);
    v("client", m.client);
    v("token", m.token);
}
template <class V> void visitFields(C_Ping& m, V&& v) {
    v("seq", m.seq);
    v("nonce", m.nonce);
}
template <class V> void visitFields(const C_Ping& m, V&& v) {
    v("seq", m.seq);
    v("nonce", m.nonce);
}
template <class V> void visitFields(C_Pong& m, V&& v) {
    v("seq", m.seq);
    v("nonce", m.nonce);
}
template <class V> void visitFields(const C_Pong& m, V&& v) {
    v("seq", m.seq);
    v("nonce", m.nonce);
}
template <class V> void visitFields(QueueJoin& m, V&& v) {
    v("seq", m.seq);
    v("category", m.category);
    v("rated", m.rated);
}
template <class V> void visitFields(const QueueJoin& m, V&& v) {
    v("seq", m.seq);
    v("category", m.category);
    v("rated", m.rated);
}
template <class V> void visitFields(QueueLeave& m, V&& v) {
    v("seq", m.seq);
}
template <class V> void visitFields(const QueueLeave& m, V&& v) {
    v("seq", m.seq);
}
template <class V> void visitFields(ChallengeCreate& m, V&& v) {
    v("seq", m.seq);
    v("target", m.target);
    v("baseSec", m.baseSec);
    v("incSec", m.incSec);
    v("rated", m.rated);
    v("color", m.color);
}
template <class V> void visitFields(const ChallengeCreate& m, V&& v) {
    v("seq", m.seq);
    v("target", m.target);
    v("baseSec", m.baseSec);
    v("incSec", m.incSec);
    v("rated", m.rated);
    v("color", m.color);
}
template <class V> void visitFields(ChallengeAccept& m, V&& v) {
    v("seq", m.seq);
    v("id", m.id);
}
template <class V> void visitFields(const ChallengeAccept& m, V&& v) {
    v("seq", m.seq);
    v("id", m.id);
}
template <class V> void visitFields(ChallengeDecline& m, V&& v) {
    v("seq", m.seq);
    v("id", m.id);
}
template <class V> void visitFields(const ChallengeDecline& m, V&& v) {
    v("seq", m.seq);
    v("id", m.id);
}
template <class V> void visitFields(ChallengeCancel& m, V&& v) {
    v("seq", m.seq);
    v("id", m.id);
}
template <class V> void visitFields(const ChallengeCancel& m, V&& v) {
    v("seq", m.seq);
    v("id", m.id);
}
template <class V> void visitFields(ChallengeJoinCode& m, V&& v) {
    v("seq", m.seq);
    v("code", m.code);
}
template <class V> void visitFields(const ChallengeJoinCode& m, V&& v) {
    v("seq", m.seq);
    v("code", m.code);
}
template <class V> void visitFields(Move& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
    v("ply", m.ply);
    v("move", m.move);
    v("posHash", m.posHash);
    v("thinkMs", m.thinkMs);
    v("drawOffer", m.drawOffer);
}
template <class V> void visitFields(const Move& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
    v("ply", m.ply);
    v("move", m.move);
    v("posHash", m.posHash);
    v("thinkMs", m.thinkMs);
    v("drawOffer", m.drawOffer);
}
template <class V> void visitFields(Resign& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(const Resign& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(DrawOffer& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(const DrawOffer& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(DrawAnswer& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
    v("accept", m.accept);
}
template <class V> void visitFields(const DrawAnswer& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
    v("accept", m.accept);
}
template <class V> void visitFields(DrawClaim& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(const DrawClaim& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(Abort& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(const Abort& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(Resync& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(const Resync& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
}
template <class V> void visitFields(Rematch& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
    v("accept", m.accept);
}
template <class V> void visitFields(const Rematch& m, V&& v) {
    v("seq", m.seq);
    v("game", m.game);
    v("accept", m.accept);
}
template <class V> void visitFields(Welcome& m, V&& v) {
    v("proto", m.proto);
    v("serverTime", m.serverTime);
    v("userId", m.userId);
    v("username", m.username);
    v("serverName", m.serverName);
    v("heartbeatMs", m.heartbeatMs);
    v("maxMsgPerSec", m.maxMsgPerSec);
    v("activeGame", m.activeGame);
}
template <class V> void visitFields(const Welcome& m, V&& v) {
    v("proto", m.proto);
    v("serverTime", m.serverTime);
    v("userId", m.userId);
    v("username", m.username);
    v("serverName", m.serverName);
    v("heartbeatMs", m.heartbeatMs);
    v("maxMsgPerSec", m.maxMsgPerSec);
    v("activeGame", m.activeGame);
}
template <class V> void visitFields(Error& m, V&& v) {
    v("ref", m.ref);
    v("code", m.code);
    v("fatal", m.fatal);
    v("game", m.game);
}
template <class V> void visitFields(const Error& m, V&& v) {
    v("ref", m.ref);
    v("code", m.code);
    v("fatal", m.fatal);
    v("game", m.game);
}
template <class V> void visitFields(S_Ping& m, V&& v) {
    v("nonce", m.nonce);
    v("serverTime", m.serverTime);
}
template <class V> void visitFields(const S_Ping& m, V&& v) {
    v("nonce", m.nonce);
    v("serverTime", m.serverTime);
}
template <class V> void visitFields(S_Pong& m, V&& v) {
    v("nonce", m.nonce);
    v("serverTime", m.serverTime);
}
template <class V> void visitFields(const S_Pong& m, V&& v) {
    v("nonce", m.nonce);
    v("serverTime", m.serverTime);
}
template <class V> void visitFields(Ack& m, V&& v) {
    v("ref", m.ref);
}
template <class V> void visitFields(const Ack& m, V&& v) {
    v("ref", m.ref);
}
template <class V> void visitFields(Notice& m, V&& v) {
    v("code", m.code);
    v("arg", m.arg);
}
template <class V> void visitFields(const Notice& m, V&& v) {
    v("code", m.code);
    v("arg", m.arg);
}
template <class V> void visitFields(QueueStatus& m, V&& v) {
    v("category", m.category);
    v("rated", m.rated);
    v("state", m.state);
    v("waitMs", m.waitMs);
    v("window", m.window);
    v("queued", m.queued);
}
template <class V> void visitFields(const QueueStatus& m, V&& v) {
    v("category", m.category);
    v("rated", m.rated);
    v("state", m.state);
    v("waitMs", m.waitMs);
    v("window", m.window);
    v("queued", m.queued);
}
template <class V> void visitFields(ChallengeReceived& m, V&& v) {
    v("id", m.id);
    v("from", m.from);
    v("baseSec", m.baseSec);
    v("incSec", m.incSec);
    v("rated", m.rated);
    v("yourColor", m.yourColor);
    v("expiresMs", m.expiresMs);
}
template <class V> void visitFields(const ChallengeReceived& m, V&& v) {
    v("id", m.id);
    v("from", m.from);
    v("baseSec", m.baseSec);
    v("incSec", m.incSec);
    v("rated", m.rated);
    v("yourColor", m.yourColor);
    v("expiresMs", m.expiresMs);
}
template <class V> void visitFields(ChallengeStatus& m, V&& v) {
    v("id", m.id);
    v("state", m.state);
    v("target", m.target);
    v("code", m.code);
    v("baseSec", m.baseSec);
    v("incSec", m.incSec);
    v("rated", m.rated);
}
template <class V> void visitFields(const ChallengeStatus& m, V&& v) {
    v("id", m.id);
    v("state", m.state);
    v("target", m.target);
    v("code", m.code);
    v("baseSec", m.baseSec);
    v("incSec", m.incSec);
    v("rated", m.rated);
}
template <class V> void visitFields(GameSnapshot& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("category", m.category);
    v("baseMs", m.baseMs);
    v("incMs", m.incMs);
    v("rated", m.rated);
    v("white", m.white);
    v("black", m.black);
    v("you", m.you);
    v("moves", m.moves);
    v("running", m.running);
    v("whiteMs", m.whiteMs);
    v("blackMs", m.blackMs);
    v("serverTime", m.serverTime);
    v("drawOffer", m.drawOffer);
    v("status", m.status);
    v("reason", m.reason);
    v("whiteConnected", m.whiteConnected);
    v("blackConnected", m.blackConnected);
    v("graceMs", m.graceMs);
    v("firstMoveMs", m.firstMoveMs);
    v("startedAt", m.startedAt);
    v("rematch", m.rematch);
}
template <class V> void visitFields(const GameSnapshot& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("category", m.category);
    v("baseMs", m.baseMs);
    v("incMs", m.incMs);
    v("rated", m.rated);
    v("white", m.white);
    v("black", m.black);
    v("you", m.you);
    v("moves", m.moves);
    v("running", m.running);
    v("whiteMs", m.whiteMs);
    v("blackMs", m.blackMs);
    v("serverTime", m.serverTime);
    v("drawOffer", m.drawOffer);
    v("status", m.status);
    v("reason", m.reason);
    v("whiteConnected", m.whiteConnected);
    v("blackConnected", m.blackConnected);
    v("graceMs", m.graceMs);
    v("firstMoveMs", m.firstMoveMs);
    v("startedAt", m.startedAt);
    v("rematch", m.rematch);
}
template <class V> void visitFields(MoveMade& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("ply", m.ply);
    v("move", m.move);
    v("flags", m.flags);
    v("spentMs", m.spentMs);
    v("whiteMs", m.whiteMs);
    v("blackMs", m.blackMs);
    v("serverTime", m.serverTime);
    v("drawOffer", m.drawOffer);
    v("firstMoveMs", m.firstMoveMs);
}
template <class V> void visitFields(const MoveMade& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("ply", m.ply);
    v("move", m.move);
    v("flags", m.flags);
    v("spentMs", m.spentMs);
    v("whiteMs", m.whiteMs);
    v("blackMs", m.blackMs);
    v("serverTime", m.serverTime);
    v("drawOffer", m.drawOffer);
    v("firstMoveMs", m.firstMoveMs);
}
template <class V> void visitFields(MoveRejected& m, V&& v) {
    v("game", m.game);
    v("ply", m.ply);
    v("move", m.move);
    v("code", m.code);
}
template <class V> void visitFields(const MoveRejected& m, V&& v) {
    v("game", m.game);
    v("ply", m.ply);
    v("move", m.move);
    v("code", m.code);
}
template <class V> void visitFields(GameEvent& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("kind", m.kind);
    v("color", m.color);
    v("arg", m.arg);
}
template <class V> void visitFields(const GameEvent& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("kind", m.kind);
    v("color", m.color);
    v("arg", m.arg);
}
template <class V> void visitFields(GameEnd& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("status", m.status);
    v("reason", m.reason);
    v("whiteMs", m.whiteMs);
    v("blackMs", m.blackMs);
    v("serverTime", m.serverTime);
}
template <class V> void visitFields(const GameEnd& m, V&& v) {
    v("game", m.game);
    v("gseq", m.gseq);
    v("status", m.status);
    v("reason", m.reason);
    v("whiteMs", m.whiteMs);
    v("blackMs", m.blackMs);
    v("serverTime", m.serverTime);
}
template <class V> void visitFields(RatingUpdate& m, V&& v) {
    v("game", m.game);
    v("category", m.category);
    v("white", m.white);
    v("black", m.black);
}
template <class V> void visitFields(const RatingUpdate& m, V&& v) {
    v("game", m.game);
    v("category", m.category);
    v("white", m.white);
    v("black", m.black);
}

// Calls f(msg) with a default-constructed message of type t; false when t is unknown.
template <class F> bool withMessage(MsgType t, F&& f) {
    switch (t) {
    case MsgType::Hello: { Hello m; f(m); return true; }
    case MsgType::C_Ping: { C_Ping m; f(m); return true; }
    case MsgType::C_Pong: { C_Pong m; f(m); return true; }
    case MsgType::QueueJoin: { QueueJoin m; f(m); return true; }
    case MsgType::QueueLeave: { QueueLeave m; f(m); return true; }
    case MsgType::ChallengeCreate: { ChallengeCreate m; f(m); return true; }
    case MsgType::ChallengeAccept: { ChallengeAccept m; f(m); return true; }
    case MsgType::ChallengeDecline: { ChallengeDecline m; f(m); return true; }
    case MsgType::ChallengeCancel: { ChallengeCancel m; f(m); return true; }
    case MsgType::ChallengeJoinCode: { ChallengeJoinCode m; f(m); return true; }
    case MsgType::Move: { Move m; f(m); return true; }
    case MsgType::Resign: { Resign m; f(m); return true; }
    case MsgType::DrawOffer: { DrawOffer m; f(m); return true; }
    case MsgType::DrawAnswer: { DrawAnswer m; f(m); return true; }
    case MsgType::DrawClaim: { DrawClaim m; f(m); return true; }
    case MsgType::Abort: { Abort m; f(m); return true; }
    case MsgType::Resync: { Resync m; f(m); return true; }
    case MsgType::Rematch: { Rematch m; f(m); return true; }
    case MsgType::Welcome: { Welcome m; f(m); return true; }
    case MsgType::Error: { Error m; f(m); return true; }
    case MsgType::S_Ping: { S_Ping m; f(m); return true; }
    case MsgType::S_Pong: { S_Pong m; f(m); return true; }
    case MsgType::Ack: { Ack m; f(m); return true; }
    case MsgType::Notice: { Notice m; f(m); return true; }
    case MsgType::QueueStatus: { QueueStatus m; f(m); return true; }
    case MsgType::ChallengeReceived: { ChallengeReceived m; f(m); return true; }
    case MsgType::ChallengeStatus: { ChallengeStatus m; f(m); return true; }
    case MsgType::GameSnapshot: { GameSnapshot m; f(m); return true; }
    case MsgType::MoveMade: { MoveMade m; f(m); return true; }
    case MsgType::MoveRejected: { MoveRejected m; f(m); return true; }
    case MsgType::GameEvent: { GameEvent m; f(m); return true; }
    case MsgType::GameEnd: { GameEnd m; f(m); return true; }
    case MsgType::RatingUpdate: { RatingUpdate m; f(m); return true; }
    }
    return false;
}

}  // namespace proto
}  // namespace net

#pragma pop_macro("None")
