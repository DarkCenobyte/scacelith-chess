// Scacelith realtime protocol codec (C++17): generated from dedicated-server/src/protocol/schema.js
// by dedicated-server/tools/gen-protocol-cpp.js, do not edit.
#include "protocol_gen.h"
#include <cmath>
#include <cstring>

#pragma push_macro("None")
#undef None

namespace net {
namespace proto {

// ---- enums ----
bool isValid(Color v) {
    switch (v) {
    case Color::White:
    case Color::Black:
    case Color::None:
        return true;
    }
    return false;
}
const char* enumName(Color v) {
    switch (v) {
    case Color::White: return "White";
    case Color::Black: return "Black";
    case Color::None: return "None";
    }
    return "?";
}
bool isValid(ColorPref v) {
    switch (v) {
    case ColorPref::Random:
    case ColorPref::White:
    case ColorPref::Black:
        return true;
    }
    return false;
}
const char* enumName(ColorPref v) {
    switch (v) {
    case ColorPref::Random: return "Random";
    case ColorPref::White: return "White";
    case ColorPref::Black: return "Black";
    }
    return "?";
}
bool isValid(GameStatus v) {
    switch (v) {
    case GameStatus::Ongoing:
    case GameStatus::WhiteWins:
    case GameStatus::BlackWins:
    case GameStatus::Draw:
    case GameStatus::Aborted:
        return true;
    }
    return false;
}
const char* enumName(GameStatus v) {
    switch (v) {
    case GameStatus::Ongoing: return "Ongoing";
    case GameStatus::WhiteWins: return "WhiteWins";
    case GameStatus::BlackWins: return "BlackWins";
    case GameStatus::Draw: return "Draw";
    case GameStatus::Aborted: return "Aborted";
    }
    return "?";
}
bool isValid(EndReason v) {
    switch (v) {
    case EndReason::None:
    case EndReason::Checkmate:
    case EndReason::Resignation:
    case EndReason::Timeout:
    case EndReason::IllegalMoves:
    case EndReason::Stalemate:
    case EndReason::InsufficientMaterial:
    case EndReason::TimeoutVsInsufficient:
    case EndReason::FivefoldRepetition:
    case EndReason::SeventyFiveMoves:
    case EndReason::ThreefoldClaim:
    case EndReason::FiftyMoveClaim:
    case EndReason::Agreement:
    case EndReason::IllegalMovesVsInsufficient:
    case EndReason::Abandonment:
    case EndReason::AbandonmentVsInsufficient:
    case EndReason::Aborted:
    case EndReason::NoShow:
    case EndReason::Forfeit:
    case EndReason::ServerAborted:
    case EndReason::BothDisconnected:
        return true;
    }
    return false;
}
const char* enumName(EndReason v) {
    switch (v) {
    case EndReason::None: return "None";
    case EndReason::Checkmate: return "Checkmate";
    case EndReason::Resignation: return "Resignation";
    case EndReason::Timeout: return "Timeout";
    case EndReason::IllegalMoves: return "IllegalMoves";
    case EndReason::Stalemate: return "Stalemate";
    case EndReason::InsufficientMaterial: return "InsufficientMaterial";
    case EndReason::TimeoutVsInsufficient: return "TimeoutVsInsufficient";
    case EndReason::FivefoldRepetition: return "FivefoldRepetition";
    case EndReason::SeventyFiveMoves: return "SeventyFiveMoves";
    case EndReason::ThreefoldClaim: return "ThreefoldClaim";
    case EndReason::FiftyMoveClaim: return "FiftyMoveClaim";
    case EndReason::Agreement: return "Agreement";
    case EndReason::IllegalMovesVsInsufficient: return "IllegalMovesVsInsufficient";
    case EndReason::Abandonment: return "Abandonment";
    case EndReason::AbandonmentVsInsufficient: return "AbandonmentVsInsufficient";
    case EndReason::Aborted: return "Aborted";
    case EndReason::NoShow: return "NoShow";
    case EndReason::Forfeit: return "Forfeit";
    case EndReason::ServerAborted: return "ServerAborted";
    case EndReason::BothDisconnected: return "BothDisconnected";
    }
    return "?";
}
bool isValid(GameEventKind v) {
    switch (v) {
    case GameEventKind::DrawOffered:
    case GameEventKind::DrawDeclined:
    case GameEventKind::PlayerDisconnected:
    case GameEventKind::PlayerReconnected:
    case GameEventKind::RematchOffered:
    case GameEventKind::RematchDeclined:
    case GameEventKind::AbortAvailable:
        return true;
    }
    return false;
}
const char* enumName(GameEventKind v) {
    switch (v) {
    case GameEventKind::DrawOffered: return "DrawOffered";
    case GameEventKind::DrawDeclined: return "DrawDeclined";
    case GameEventKind::PlayerDisconnected: return "PlayerDisconnected";
    case GameEventKind::PlayerReconnected: return "PlayerReconnected";
    case GameEventKind::RematchOffered: return "RematchOffered";
    case GameEventKind::RematchDeclined: return "RematchDeclined";
    case GameEventKind::AbortAvailable: return "AbortAvailable";
    }
    return "?";
}
bool isValid(QueueState v) {
    switch (v) {
    case QueueState::Left:
    case QueueState::Searching:
    case QueueState::Matched:
        return true;
    }
    return false;
}
const char* enumName(QueueState v) {
    switch (v) {
    case QueueState::Left: return "Left";
    case QueueState::Searching: return "Searching";
    case QueueState::Matched: return "Matched";
    }
    return "?";
}
bool isValid(ChallengeState v) {
    switch (v) {
    case ChallengeState::Pending:
    case ChallengeState::Accepted:
    case ChallengeState::Declined:
    case ChallengeState::Cancelled:
    case ChallengeState::Expired:
    case ChallengeState::Unavailable:
        return true;
    }
    return false;
}
const char* enumName(ChallengeState v) {
    switch (v) {
    case ChallengeState::Pending: return "Pending";
    case ChallengeState::Accepted: return "Accepted";
    case ChallengeState::Declined: return "Declined";
    case ChallengeState::Cancelled: return "Cancelled";
    case ChallengeState::Expired: return "Expired";
    case ChallengeState::Unavailable: return "Unavailable";
    }
    return "?";
}
bool isValid(NoticeCode v) {
    switch (v) {
    case NoticeCode::ServerShutdown:
    case NoticeCode::Banned:
    case NoticeCode::SessionRevoked:
    case NoticeCode::MatchmakingCooldown:
    case NoticeCode::ReplacedByNewConnection:
    case NoticeCode::Motd:
    case NoticeCode::RatingRestored:
        return true;
    }
    return false;
}
const char* enumName(NoticeCode v) {
    switch (v) {
    case NoticeCode::ServerShutdown: return "ServerShutdown";
    case NoticeCode::Banned: return "Banned";
    case NoticeCode::SessionRevoked: return "SessionRevoked";
    case NoticeCode::MatchmakingCooldown: return "MatchmakingCooldown";
    case NoticeCode::ReplacedByNewConnection: return "ReplacedByNewConnection";
    case NoticeCode::Motd: return "Motd";
    case NoticeCode::RatingRestored: return "RatingRestored";
    }
    return "?";
}
bool isValid(ErrorCode v) {
    switch (v) {
    case ErrorCode::Malformed:
    case ErrorCode::UnsupportedProtocol:
    case ErrorCode::Unauthorized:
    case ErrorCode::Banned:
    case ErrorCode::RateLimited:
    case ErrorCode::ServerFull:
    case ErrorCode::Replaced:
    case ErrorCode::ShuttingDown:
    case ErrorCode::Internal:
    case ErrorCode::HelloRequired:
    case ErrorCode::EmailUnverified:
    case ErrorCode::NotInGame:
    case ErrorCode::NotYourTurn:
    case ErrorCode::IllegalMove:
    case ErrorCode::StalePly:
    case ErrorCode::Desync:
    case ErrorCode::GameOver:
    case ErrorCode::AlreadyInGame:
    case ErrorCode::InvalidCategory:
    case ErrorCode::DrawOfferLimit:
    case ErrorCode::NothingToClaim:
    case ErrorCode::AbortNotAllowed:
    case ErrorCode::NoPendingOffer:
    case ErrorCode::FlagFell:
    case ErrorCode::QueueNotAllowed:
    case ErrorCode::ChallengeNotFound:
    case ErrorCode::UserUnavailable:
    case ErrorCode::ChallengeLimit:
    case ErrorCode::CannotChallengeSelf:
    case ErrorCode::CodeInvalid:
    case ErrorCode::RatedRequiresOfficialTc:
    case ErrorCode::MatchmakingCooldown:
    case ErrorCode::InvalidTimeControl:
    case ErrorCode::RematchUnavailable:
    case ErrorCode::RatedRepeatLimit:
    case ErrorCode::ProtocolViolation:
    case ErrorCode::Flood:
    case ErrorCode::CheatDetected:
    case ErrorCode::SlowConsumer:
        return true;
    }
    return false;
}
const char* enumName(ErrorCode v) {
    switch (v) {
    case ErrorCode::Malformed: return "Malformed";
    case ErrorCode::UnsupportedProtocol: return "UnsupportedProtocol";
    case ErrorCode::Unauthorized: return "Unauthorized";
    case ErrorCode::Banned: return "Banned";
    case ErrorCode::RateLimited: return "RateLimited";
    case ErrorCode::ServerFull: return "ServerFull";
    case ErrorCode::Replaced: return "Replaced";
    case ErrorCode::ShuttingDown: return "ShuttingDown";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::HelloRequired: return "HelloRequired";
    case ErrorCode::EmailUnverified: return "EmailUnverified";
    case ErrorCode::NotInGame: return "NotInGame";
    case ErrorCode::NotYourTurn: return "NotYourTurn";
    case ErrorCode::IllegalMove: return "IllegalMove";
    case ErrorCode::StalePly: return "StalePly";
    case ErrorCode::Desync: return "Desync";
    case ErrorCode::GameOver: return "GameOver";
    case ErrorCode::AlreadyInGame: return "AlreadyInGame";
    case ErrorCode::InvalidCategory: return "InvalidCategory";
    case ErrorCode::DrawOfferLimit: return "DrawOfferLimit";
    case ErrorCode::NothingToClaim: return "NothingToClaim";
    case ErrorCode::AbortNotAllowed: return "AbortNotAllowed";
    case ErrorCode::NoPendingOffer: return "NoPendingOffer";
    case ErrorCode::FlagFell: return "FlagFell";
    case ErrorCode::QueueNotAllowed: return "QueueNotAllowed";
    case ErrorCode::ChallengeNotFound: return "ChallengeNotFound";
    case ErrorCode::UserUnavailable: return "UserUnavailable";
    case ErrorCode::ChallengeLimit: return "ChallengeLimit";
    case ErrorCode::CannotChallengeSelf: return "CannotChallengeSelf";
    case ErrorCode::CodeInvalid: return "CodeInvalid";
    case ErrorCode::RatedRequiresOfficialTc: return "RatedRequiresOfficialTc";
    case ErrorCode::MatchmakingCooldown: return "MatchmakingCooldown";
    case ErrorCode::InvalidTimeControl: return "InvalidTimeControl";
    case ErrorCode::RematchUnavailable: return "RematchUnavailable";
    case ErrorCode::RatedRepeatLimit: return "RatedRepeatLimit";
    case ErrorCode::ProtocolViolation: return "ProtocolViolation";
    case ErrorCode::Flood: return "Flood";
    case ErrorCode::CheatDetected: return "CheatDetected";
    case ErrorCode::SlowConsumer: return "SlowConsumer";
    }
    return "?";
}

const char* messageName(MsgType t) {
    switch (t) {
    case MsgType::Hello: return "Hello";
    case MsgType::C_Ping: return "C_Ping";
    case MsgType::C_Pong: return "C_Pong";
    case MsgType::QueueJoin: return "QueueJoin";
    case MsgType::QueueLeave: return "QueueLeave";
    case MsgType::ChallengeCreate: return "ChallengeCreate";
    case MsgType::ChallengeAccept: return "ChallengeAccept";
    case MsgType::ChallengeDecline: return "ChallengeDecline";
    case MsgType::ChallengeCancel: return "ChallengeCancel";
    case MsgType::ChallengeJoinCode: return "ChallengeJoinCode";
    case MsgType::Move: return "Move";
    case MsgType::Resign: return "Resign";
    case MsgType::DrawOffer: return "DrawOffer";
    case MsgType::DrawAnswer: return "DrawAnswer";
    case MsgType::DrawClaim: return "DrawClaim";
    case MsgType::Abort: return "Abort";
    case MsgType::Resync: return "Resync";
    case MsgType::Rematch: return "Rematch";
    case MsgType::C_Gesture: return "C_Gesture";
    case MsgType::Welcome: return "Welcome";
    case MsgType::Error: return "Error";
    case MsgType::S_Ping: return "S_Ping";
    case MsgType::S_Pong: return "S_Pong";
    case MsgType::Ack: return "Ack";
    case MsgType::Notice: return "Notice";
    case MsgType::QueueStatus: return "QueueStatus";
    case MsgType::ChallengeReceived: return "ChallengeReceived";
    case MsgType::ChallengeStatus: return "ChallengeStatus";
    case MsgType::GameSnapshot: return "GameSnapshot";
    case MsgType::MoveMade: return "MoveMade";
    case MsgType::MoveRejected: return "MoveRejected";
    case MsgType::GameEvent: return "GameEvent";
    case MsgType::GameEnd: return "GameEnd";
    case MsgType::RatingUpdate: return "RatingUpdate";
    case MsgType::S_Gesture: return "S_Gesture";
    }
    return nullptr;
}

bool peekType(const uint8_t* p, size_t n, MsgType& t) {
    if (!p || n < 1 || !messageName(MsgType(p[0]))) return false;
    t = MsgType(p[0]);
    return true;
}

namespace {

// Appends little-endian values.
class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : o_(out) {}
    void u8(uint8_t v) { o_.push_back(v); }
    void u16(uint16_t v) { o_.push_back(uint8_t(v)); o_.push_back(uint8_t(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) o_.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) o_.push_back(uint8_t(v >> (8 * i))); }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        u64(bits);
    }
    // Cut to 'max' bytes without splitting a UTF-8 sequence (only reached with invalid input).
    void str8(const std::string& s, size_t max) {
        size_t n = s.size();
        if (n > max) {
            n = max;
            while (n > 0 && (uint8_t(s[n]) & 0xC0) == 0x80) --n;
        }
        u8(uint8_t(n));
        o_.insert(o_.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
    }

private:
    std::vector<uint8_t>& o_;
};

// Strict UTF-8 (no overlong forms, no surrogates, <= U+10FFFF), and no NUL byte.
bool utf8NoNul(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c == 0) return false;
        if (c < 0x80) { ++i; continue; }
        size_t len;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

bool validStr(const std::string& s, size_t min, size_t max) {
    return s.size() >= min && s.size() <= max && utf8NoNul(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

template <class T, class P> bool allOf(const std::vector<T>& v, P pred) {
    for (const T& x : v)
        if (!pred(x)) return false;
    return true;
}

// Bounds-checked little-endian reader; every accessor fails on truncation or a bad value.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool type(MsgType t) {
        if (n_ < 1 || p_[0] != uint8_t(t)) return false;
        o_ = 1;
        return true;
    }
    bool end() const { return o_ == n_; }
    bool u8(uint8_t& v, uint32_t lo, uint32_t hi) {
        if (n_ - o_ < 1) return false;
        v = p_[o_++];
        return v >= lo && v <= hi;
    }
    bool u16(uint16_t& v, uint32_t lo, uint32_t hi) {
        if (n_ - o_ < 2) return false;
        v = uint16_t(p_[o_] | (p_[o_ + 1] << 8));
        o_ += 2;
        return v >= lo && v <= hi;
    }
    bool u32(uint32_t& v, uint32_t lo, uint32_t hi) {
        if (!raw32(v)) return false;
        return v >= lo && v <= hi;
    }
    bool i32(int32_t& v, int32_t lo, int32_t hi) {
        uint32_t u;
        if (!raw32(u)) return false;
        v = int32_t(u);
        return v >= lo && v <= hi;
    }
    bool id53(uint64_t& v) {
        if (!raw64(v)) return false;
        return v < kId53Limit;
    }
    bool f64(double& v) {
        uint64_t bits;
        if (!raw64(bits)) return false;
        std::memcpy(&v, &bits, 8);
        return std::isfinite(v);
    }
    bool boolean(bool& v) {
        if (n_ - o_ < 1) return false;
        uint8_t b = p_[o_++];
        v = b == 1;
        return b <= 1;
    }
    template <class E> bool enumeration(E& v) {
        if (n_ - o_ < 1) return false;
        v = E(p_[o_++]);
        return isValid(v);
    }
    bool str8(std::string& v, size_t min, size_t max) {
        if (n_ - o_ < 1) return false;
        size_t len = p_[o_++];
        if (len < min || len > max || n_ - o_ < len) return false;
        if (!utf8NoNul(p_ + o_, len)) return false;
        v.assign(reinterpret_cast<const char*>(p_ + o_), len);
        o_ += len;
        return true;
    }
    template <class T, class F> bool list(std::vector<T>& v, size_t max, F item) {
        uint16_t count;
        if (!u16(count, 0, 0xFFFF) || count > max) return false;
        v.clear();
        v.resize(count);
        for (T& x : v)
            if (!item(*this, x)) return false;
        return true;
    }

private:
    bool raw32(uint32_t& v) {
        if (n_ - o_ < 4) return false;
        v = uint32_t(p_[o_]) | uint32_t(p_[o_ + 1]) << 8 | uint32_t(p_[o_ + 2]) << 16 | uint32_t(p_[o_ + 3]) << 24;
        o_ += 4;
        return true;
    }
    bool raw64(uint64_t& v) {
        uint32_t lo, hi;
        if (!raw32(lo) || !raw32(hi)) return false;
        v = uint64_t(hi) << 32 | lo;
        return true;
    }
    const uint8_t* p_;
    size_t n_, o_ = 0;
};

}  // namespace

// ---- structs ----
namespace {
void put(Writer& w, const PlayerInfo& s);
bool get(Reader& r, PlayerInfo& s);
void put(Writer& w, const MoveRec& s);
bool get(Reader& r, MoveRec& s);
void put(Writer& w, const RatingChange& s);
bool get(Reader& r, RatingChange& s);

void put(Writer& w, const PlayerInfo& s) {
    w.u32(s.userId);
    w.str8(s.name, 24);
    w.u16(s.rating);
    w.u8(s.provisional ? 1 : 0);
}
bool get(Reader& r, PlayerInfo& s) {
    return r.u32(s.userId, 0u, 0xffffffffu) &&
           r.str8(s.name, 1, 24) &&
           r.u16(s.rating, 0u, 65535u) &&
           r.boolean(s.provisional);
}
void put(Writer& w, const MoveRec& s) {
    w.u16(s.move);
    w.u32(s.spentMs);
    w.u32(s.clockMs);
}
bool get(Reader& r, MoveRec& s) {
    return r.u16(s.move, 0u, 32767u) &&
           r.u32(s.spentMs, 0u, 0xffffffffu) &&
           r.u32(s.clockMs, 0u, 0xffffffffu);
}
void put(Writer& w, const RatingChange& s) {
    w.u16(s.before);
    w.u16(s.after);
    w.u32(s.games);
    w.u8(s.provisional ? 1 : 0);
}
bool get(Reader& r, RatingChange& s) {
    return r.u16(s.before, 0u, 65535u) &&
           r.u16(s.after, 0u, 65535u) &&
           r.u32(s.games, 0u, 0xffffffffu) &&
           r.boolean(s.provisional);
}
}  // namespace

bool valid(const PlayerInfo& s) {
    return validStr(s.name, 1, 24);
}
bool valid(const MoveRec& s) {
    return s.move <= 32767u;
}
bool valid(const RatingChange& s) {
    (void)s;
    return true;
}

// ---- messages ----
void encode(const Hello& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Hello));
    w.u32(m.seq);
    w.u16(m.proto);
    w.u32(m.schema);
    w.str8(m.client, 48);
    w.str8(m.token, 160);
}
bool decode(const uint8_t* p, size_t n, Hello& out) {
    Reader r(p, n);
    return r.type(MsgType::Hello) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.u16(out.proto, 0u, 65535u) &&
           r.u32(out.schema, 0u, 0xffffffffu) &&
           r.str8(out.client, 0, 48) &&
           r.str8(out.token, 16, 160) &&
           r.end();
}
bool valid(const Hello& m) {
    return validStr(m.client, 0, 48) &&
           validStr(m.token, 16, 160);
}
void encode(const C_Ping& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::C_Ping));
    w.u32(m.seq);
    w.u32(m.nonce);
}
bool decode(const uint8_t* p, size_t n, C_Ping& out) {
    Reader r(p, n);
    return r.type(MsgType::C_Ping) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.u32(out.nonce, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const C_Ping& m) {
    (void)m;
    return true;
}
void encode(const C_Pong& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::C_Pong));
    w.u32(m.seq);
    w.u32(m.nonce);
}
bool decode(const uint8_t* p, size_t n, C_Pong& out) {
    Reader r(p, n);
    return r.type(MsgType::C_Pong) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.u32(out.nonce, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const C_Pong& m) {
    (void)m;
    return true;
}
void encode(const QueueJoin& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::QueueJoin));
    w.u32(m.seq);
    w.str8(m.category, 7);
    w.u8(m.rated ? 1 : 0);
}
bool decode(const uint8_t* p, size_t n, QueueJoin& out) {
    Reader r(p, n);
    return r.type(MsgType::QueueJoin) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.str8(out.category, 3, 7) &&
           r.boolean(out.rated) &&
           r.end();
}
bool valid(const QueueJoin& m) {
    return validStr(m.category, 3, 7);
}
void encode(const QueueLeave& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::QueueLeave));
    w.u32(m.seq);
}
bool decode(const uint8_t* p, size_t n, QueueLeave& out) {
    Reader r(p, n);
    return r.type(MsgType::QueueLeave) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const QueueLeave& m) {
    (void)m;
    return true;
}
void encode(const ChallengeCreate& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeCreate));
    w.u32(m.seq);
    w.str8(m.target, 24);
    w.u16(m.baseSec);
    w.u8(m.incSec);
    w.u8(m.rated ? 1 : 0);
    w.u8(uint8_t(m.color));
}
bool decode(const uint8_t* p, size_t n, ChallengeCreate& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeCreate) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.str8(out.target, 0, 24) &&
           r.u16(out.baseSec, 15u, 10800u) &&
           r.u8(out.incSec, 0u, 180u) &&
           r.boolean(out.rated) &&
           r.enumeration(out.color) &&
           r.end();
}
bool valid(const ChallengeCreate& m) {
    return validStr(m.target, 0, 24) &&
           m.baseSec >= 15u && m.baseSec <= 10800u &&
           m.incSec <= 180u &&
           isValid(m.color);
}
void encode(const ChallengeAccept& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeAccept));
    w.u32(m.seq);
    w.u32(m.id);
}
bool decode(const uint8_t* p, size_t n, ChallengeAccept& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeAccept) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.u32(out.id, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const ChallengeAccept& m) {
    (void)m;
    return true;
}
void encode(const ChallengeDecline& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeDecline));
    w.u32(m.seq);
    w.u32(m.id);
}
bool decode(const uint8_t* p, size_t n, ChallengeDecline& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeDecline) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.u32(out.id, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const ChallengeDecline& m) {
    (void)m;
    return true;
}
void encode(const ChallengeCancel& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeCancel));
    w.u32(m.seq);
    w.u32(m.id);
}
bool decode(const uint8_t* p, size_t n, ChallengeCancel& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeCancel) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.u32(out.id, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const ChallengeCancel& m) {
    (void)m;
    return true;
}
void encode(const ChallengeJoinCode& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeJoinCode));
    w.u32(m.seq);
    w.str8(m.code, 12);
}
bool decode(const uint8_t* p, size_t n, ChallengeJoinCode& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeJoinCode) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.str8(out.code, 4, 12) &&
           r.end();
}
bool valid(const ChallengeJoinCode& m) {
    return validStr(m.code, 4, 12);
}
void encode(const Move& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Move));
    w.u32(m.seq);
    w.u64(m.game);
    w.u16(m.ply);
    w.u16(m.move);
    w.u32(m.posHash);
    w.u32(m.thinkMs);
    w.u8(m.drawOffer ? 1 : 0);
}
bool decode(const uint8_t* p, size_t n, Move& out) {
    Reader r(p, n);
    return r.type(MsgType::Move) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.u16(out.ply, 0u, 1199u) &&
           r.u16(out.move, 0u, 32767u) &&
           r.u32(out.posHash, 0u, 0xffffffffu) &&
           r.u32(out.thinkMs, 0u, 0xffffffffu) &&
           r.boolean(out.drawOffer) &&
           r.end();
}
bool valid(const Move& m) {
    return m.game < kId53Limit &&
           m.ply <= 1199u &&
           m.move <= 32767u;
}
void encode(const Resign& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Resign));
    w.u32(m.seq);
    w.u64(m.game);
}
bool decode(const uint8_t* p, size_t n, Resign& out) {
    Reader r(p, n);
    return r.type(MsgType::Resign) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.end();
}
bool valid(const Resign& m) {
    return m.game < kId53Limit;
}
void encode(const DrawOffer& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::DrawOffer));
    w.u32(m.seq);
    w.u64(m.game);
}
bool decode(const uint8_t* p, size_t n, DrawOffer& out) {
    Reader r(p, n);
    return r.type(MsgType::DrawOffer) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.end();
}
bool valid(const DrawOffer& m) {
    return m.game < kId53Limit;
}
void encode(const DrawAnswer& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::DrawAnswer));
    w.u32(m.seq);
    w.u64(m.game);
    w.u8(m.accept ? 1 : 0);
}
bool decode(const uint8_t* p, size_t n, DrawAnswer& out) {
    Reader r(p, n);
    return r.type(MsgType::DrawAnswer) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.boolean(out.accept) &&
           r.end();
}
bool valid(const DrawAnswer& m) {
    return m.game < kId53Limit;
}
void encode(const DrawClaim& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::DrawClaim));
    w.u32(m.seq);
    w.u64(m.game);
}
bool decode(const uint8_t* p, size_t n, DrawClaim& out) {
    Reader r(p, n);
    return r.type(MsgType::DrawClaim) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.end();
}
bool valid(const DrawClaim& m) {
    return m.game < kId53Limit;
}
void encode(const Abort& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Abort));
    w.u32(m.seq);
    w.u64(m.game);
}
bool decode(const uint8_t* p, size_t n, Abort& out) {
    Reader r(p, n);
    return r.type(MsgType::Abort) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.end();
}
bool valid(const Abort& m) {
    return m.game < kId53Limit;
}
void encode(const Resync& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Resync));
    w.u32(m.seq);
    w.u64(m.game);
}
bool decode(const uint8_t* p, size_t n, Resync& out) {
    Reader r(p, n);
    return r.type(MsgType::Resync) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.end();
}
bool valid(const Resync& m) {
    return m.game < kId53Limit;
}
void encode(const Rematch& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Rematch));
    w.u32(m.seq);
    w.u64(m.game);
    w.u8(m.accept ? 1 : 0);
}
bool decode(const uint8_t* p, size_t n, Rematch& out) {
    Reader r(p, n);
    return r.type(MsgType::Rematch) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.boolean(out.accept) &&
           r.end();
}
bool valid(const Rematch& m) {
    return m.game < kId53Limit;
}
void encode(const C_Gesture& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::C_Gesture));
    w.u32(m.seq);
    w.u64(m.game);
    w.u16(m.ply);
    w.u8(m.touch);
    w.u8(m.aim);
    w.u16(m.placed);
    w.u8(m.flags);
    w.u32(uint32_t(m.yaw));
    w.u32(uint32_t(m.pitch));
    w.u8(m.lean);
}
bool decode(const uint8_t* p, size_t n, C_Gesture& out) {
    Reader r(p, n);
    return r.type(MsgType::C_Gesture) &&
           r.u32(out.seq, 0u, 0xffffffffu) &&
           r.id53(out.game) &&
           r.u16(out.ply, 0u, 1199u) &&
           r.u8(out.touch, 0u, 64u) &&
           r.u8(out.aim, 0u, 64u) &&
           r.u16(out.placed, 0u, 32767u) &&
           r.u8(out.flags, 0u, 7u) &&
           r.i32(out.yaw, -3142, 3142) &&
           r.i32(out.pitch, -1571, 1571) &&
           r.u8(out.lean, 0u, 100u) &&
           r.end();
}
bool valid(const C_Gesture& m) {
    return m.game < kId53Limit &&
           m.ply <= 1199u &&
           m.touch <= 64u &&
           m.aim <= 64u &&
           m.placed <= 32767u &&
           m.flags <= 7u &&
           m.yaw >= -3142 && m.yaw <= 3142 &&
           m.pitch >= -1571 && m.pitch <= 1571 &&
           m.lean <= 100u;
}
void encode(const Welcome& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Welcome));
    w.u16(m.proto);
    w.f64(m.serverTime);
    w.u32(m.userId);
    w.str8(m.username, 24);
    w.str8(m.serverName, 64);
    w.u32(m.heartbeatMs);
    w.u32(m.clientPingMs);
    w.u16(m.maxMsgPerSec);
    w.u64(m.activeGame);
    w.u16(m.gestureRate);
    w.u16(m.gestureBurst);
}
bool decode(const uint8_t* p, size_t n, Welcome& out) {
    Reader r(p, n);
    return r.type(MsgType::Welcome) &&
           r.u16(out.proto, 0u, 65535u) &&
           r.f64(out.serverTime) &&
           r.u32(out.userId, 0u, 0xffffffffu) &&
           r.str8(out.username, 0, 24) &&
           r.str8(out.serverName, 0, 64) &&
           r.u32(out.heartbeatMs, 0u, 0xffffffffu) &&
           r.u32(out.clientPingMs, 0u, 0xffffffffu) &&
           r.u16(out.maxMsgPerSec, 0u, 65535u) &&
           r.id53(out.activeGame) &&
           r.u16(out.gestureRate, 0u, 60u) &&
           r.u16(out.gestureBurst, 0u, 120u) &&
           r.end();
}
bool valid(const Welcome& m) {
    return std::isfinite(m.serverTime) &&
           validStr(m.username, 0, 24) &&
           validStr(m.serverName, 0, 64) &&
           m.activeGame < kId53Limit &&
           m.gestureRate <= 60u &&
           m.gestureBurst <= 120u;
}
void encode(const Error& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Error));
    w.u32(m.ref);
    w.u8(uint8_t(m.code));
    w.u8(m.fatal ? 1 : 0);
    w.u64(m.game);
}
bool decode(const uint8_t* p, size_t n, Error& out) {
    Reader r(p, n);
    return r.type(MsgType::Error) &&
           r.u32(out.ref, 0u, 0xffffffffu) &&
           r.enumeration(out.code) &&
           r.boolean(out.fatal) &&
           r.id53(out.game) &&
           r.end();
}
bool valid(const Error& m) {
    return isValid(m.code) &&
           m.game < kId53Limit;
}
void encode(const S_Ping& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::S_Ping));
    w.u32(m.nonce);
    w.f64(m.serverTime);
}
bool decode(const uint8_t* p, size_t n, S_Ping& out) {
    Reader r(p, n);
    return r.type(MsgType::S_Ping) &&
           r.u32(out.nonce, 0u, 0xffffffffu) &&
           r.f64(out.serverTime) &&
           r.end();
}
bool valid(const S_Ping& m) {
    return std::isfinite(m.serverTime);
}
void encode(const S_Pong& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::S_Pong));
    w.u32(m.nonce);
    w.f64(m.serverTime);
}
bool decode(const uint8_t* p, size_t n, S_Pong& out) {
    Reader r(p, n);
    return r.type(MsgType::S_Pong) &&
           r.u32(out.nonce, 0u, 0xffffffffu) &&
           r.f64(out.serverTime) &&
           r.end();
}
bool valid(const S_Pong& m) {
    return std::isfinite(m.serverTime);
}
void encode(const Ack& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Ack));
    w.u32(m.ref);
}
bool decode(const uint8_t* p, size_t n, Ack& out) {
    Reader r(p, n);
    return r.type(MsgType::Ack) &&
           r.u32(out.ref, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const Ack& m) {
    (void)m;
    return true;
}
void encode(const Notice& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::Notice));
    w.u8(uint8_t(m.code));
    w.f64(m.arg);
}
bool decode(const uint8_t* p, size_t n, Notice& out) {
    Reader r(p, n);
    return r.type(MsgType::Notice) &&
           r.enumeration(out.code) &&
           r.f64(out.arg) &&
           r.end();
}
bool valid(const Notice& m) {
    return isValid(m.code) &&
           std::isfinite(m.arg);
}
void encode(const QueueStatus& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::QueueStatus));
    w.str8(m.category, 7);
    w.u8(m.rated ? 1 : 0);
    w.u8(uint8_t(m.state));
    w.u32(m.waitMs);
    w.u16(m.window);
    w.u32(m.queued);
}
bool decode(const uint8_t* p, size_t n, QueueStatus& out) {
    Reader r(p, n);
    return r.type(MsgType::QueueStatus) &&
           r.str8(out.category, 0, 7) &&
           r.boolean(out.rated) &&
           r.enumeration(out.state) &&
           r.u32(out.waitMs, 0u, 0xffffffffu) &&
           r.u16(out.window, 0u, 65535u) &&
           r.u32(out.queued, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const QueueStatus& m) {
    return validStr(m.category, 0, 7) &&
           isValid(m.state);
}
void encode(const ChallengeReceived& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeReceived));
    w.u32(m.id);
    put(w, m.from);
    w.u16(m.baseSec);
    w.u8(m.incSec);
    w.u8(m.rated ? 1 : 0);
    w.u8(uint8_t(m.yourColor));
    w.u32(m.expiresMs);
}
bool decode(const uint8_t* p, size_t n, ChallengeReceived& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeReceived) &&
           r.u32(out.id, 0u, 0xffffffffu) &&
           get(r, out.from) &&
           r.u16(out.baseSec, 0u, 65535u) &&
           r.u8(out.incSec, 0u, 255u) &&
           r.boolean(out.rated) &&
           r.enumeration(out.yourColor) &&
           r.u32(out.expiresMs, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const ChallengeReceived& m) {
    return valid(m.from) &&
           isValid(m.yourColor);
}
void encode(const ChallengeStatus& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::ChallengeStatus));
    w.u32(m.id);
    w.u8(uint8_t(m.state));
    w.str8(m.target, 24);
    w.str8(m.code, 12);
    w.u16(m.baseSec);
    w.u8(m.incSec);
    w.u8(m.rated ? 1 : 0);
}
bool decode(const uint8_t* p, size_t n, ChallengeStatus& out) {
    Reader r(p, n);
    return r.type(MsgType::ChallengeStatus) &&
           r.u32(out.id, 0u, 0xffffffffu) &&
           r.enumeration(out.state) &&
           r.str8(out.target, 0, 24) &&
           r.str8(out.code, 0, 12) &&
           r.u16(out.baseSec, 0u, 65535u) &&
           r.u8(out.incSec, 0u, 255u) &&
           r.boolean(out.rated) &&
           r.end();
}
bool valid(const ChallengeStatus& m) {
    return isValid(m.state) &&
           validStr(m.target, 0, 24) &&
           validStr(m.code, 0, 12);
}
void encode(const GameSnapshot& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::GameSnapshot));
    w.u64(m.game);
    w.u32(m.gseq);
    w.str8(m.category, 7);
    w.u32(m.baseMs);
    w.u32(m.incMs);
    w.u8(m.rated ? 1 : 0);
    put(w, m.white);
    put(w, m.black);
    w.u8(uint8_t(m.you));
    {
        const size_t count = m.moves.size() < 1200 ? m.moves.size() : 1200;
        w.u16(uint16_t(count));
        for (size_t i = 0; i < count; ++i) {
            put(w, m.moves[i]);
        }
    }
    w.u8(uint8_t(m.running));
    w.u32(m.whiteMs);
    w.u32(m.blackMs);
    w.f64(m.serverTime);
    w.u8(uint8_t(m.drawOffer));
    w.u8(uint8_t(m.status));
    w.u8(uint8_t(m.reason));
    w.u8(m.whiteConnected ? 1 : 0);
    w.u8(m.blackConnected ? 1 : 0);
    w.u32(m.graceMs);
    w.u32(m.firstMoveMs);
    w.f64(m.startedAt);
    w.u8(uint8_t(m.rematch));
    w.u8(m.autoPress ? 1 : 0);
}
bool decode(const uint8_t* p, size_t n, GameSnapshot& out) {
    Reader r(p, n);
    return r.type(MsgType::GameSnapshot) &&
           r.id53(out.game) &&
           r.u32(out.gseq, 0u, 0xffffffffu) &&
           r.str8(out.category, 0, 7) &&
           r.u32(out.baseMs, 0u, 0xffffffffu) &&
           r.u32(out.incMs, 0u, 0xffffffffu) &&
           r.boolean(out.rated) &&
           get(r, out.white) &&
           get(r, out.black) &&
           r.enumeration(out.you) &&
           r.list(out.moves, 1200, [](Reader& r, MoveRec& x) { return get(r, x); }) &&
           r.enumeration(out.running) &&
           r.u32(out.whiteMs, 0u, 0xffffffffu) &&
           r.u32(out.blackMs, 0u, 0xffffffffu) &&
           r.f64(out.serverTime) &&
           r.enumeration(out.drawOffer) &&
           r.enumeration(out.status) &&
           r.enumeration(out.reason) &&
           r.boolean(out.whiteConnected) &&
           r.boolean(out.blackConnected) &&
           r.u32(out.graceMs, 0u, 0xffffffffu) &&
           r.u32(out.firstMoveMs, 0u, 0xffffffffu) &&
           r.f64(out.startedAt) &&
           r.enumeration(out.rematch) &&
           r.boolean(out.autoPress) &&
           r.end();
}
bool valid(const GameSnapshot& m) {
    return m.game < kId53Limit &&
           validStr(m.category, 0, 7) &&
           valid(m.white) &&
           valid(m.black) &&
           isValid(m.you) &&
           m.moves.size() <= 1200 && allOf(m.moves, [](const MoveRec& x) { return valid(x); }) &&
           isValid(m.running) &&
           std::isfinite(m.serverTime) &&
           isValid(m.drawOffer) &&
           isValid(m.status) &&
           isValid(m.reason) &&
           std::isfinite(m.startedAt) &&
           isValid(m.rematch);
}
void encode(const MoveMade& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::MoveMade));
    w.u64(m.game);
    w.u32(m.gseq);
    w.u16(m.ply);
    w.u16(m.move);
    w.u8(m.flags);
    w.u32(m.spentMs);
    w.u32(m.whiteMs);
    w.u32(m.blackMs);
    w.f64(m.serverTime);
    w.u8(m.drawOffer ? 1 : 0);
    w.u32(m.firstMoveMs);
}
bool decode(const uint8_t* p, size_t n, MoveMade& out) {
    Reader r(p, n);
    return r.type(MsgType::MoveMade) &&
           r.id53(out.game) &&
           r.u32(out.gseq, 0u, 0xffffffffu) &&
           r.u16(out.ply, 0u, 65535u) &&
           r.u16(out.move, 0u, 65535u) &&
           r.u8(out.flags, 0u, 255u) &&
           r.u32(out.spentMs, 0u, 0xffffffffu) &&
           r.u32(out.whiteMs, 0u, 0xffffffffu) &&
           r.u32(out.blackMs, 0u, 0xffffffffu) &&
           r.f64(out.serverTime) &&
           r.boolean(out.drawOffer) &&
           r.u32(out.firstMoveMs, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const MoveMade& m) {
    return m.game < kId53Limit &&
           std::isfinite(m.serverTime);
}
void encode(const MoveRejected& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::MoveRejected));
    w.u64(m.game);
    w.u16(m.ply);
    w.u16(m.move);
    w.u8(uint8_t(m.code));
}
bool decode(const uint8_t* p, size_t n, MoveRejected& out) {
    Reader r(p, n);
    return r.type(MsgType::MoveRejected) &&
           r.id53(out.game) &&
           r.u16(out.ply, 0u, 65535u) &&
           r.u16(out.move, 0u, 65535u) &&
           r.enumeration(out.code) &&
           r.end();
}
bool valid(const MoveRejected& m) {
    return m.game < kId53Limit &&
           isValid(m.code);
}
void encode(const GameEvent& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::GameEvent));
    w.u64(m.game);
    w.u32(m.gseq);
    w.u8(uint8_t(m.kind));
    w.u8(uint8_t(m.color));
    w.u32(m.arg);
}
bool decode(const uint8_t* p, size_t n, GameEvent& out) {
    Reader r(p, n);
    return r.type(MsgType::GameEvent) &&
           r.id53(out.game) &&
           r.u32(out.gseq, 0u, 0xffffffffu) &&
           r.enumeration(out.kind) &&
           r.enumeration(out.color) &&
           r.u32(out.arg, 0u, 0xffffffffu) &&
           r.end();
}
bool valid(const GameEvent& m) {
    return m.game < kId53Limit &&
           isValid(m.kind) &&
           isValid(m.color);
}
void encode(const GameEnd& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::GameEnd));
    w.u64(m.game);
    w.u32(m.gseq);
    w.u8(uint8_t(m.status));
    w.u8(uint8_t(m.reason));
    w.u32(m.whiteMs);
    w.u32(m.blackMs);
    w.f64(m.serverTime);
}
bool decode(const uint8_t* p, size_t n, GameEnd& out) {
    Reader r(p, n);
    return r.type(MsgType::GameEnd) &&
           r.id53(out.game) &&
           r.u32(out.gseq, 0u, 0xffffffffu) &&
           r.enumeration(out.status) &&
           r.enumeration(out.reason) &&
           r.u32(out.whiteMs, 0u, 0xffffffffu) &&
           r.u32(out.blackMs, 0u, 0xffffffffu) &&
           r.f64(out.serverTime) &&
           r.end();
}
bool valid(const GameEnd& m) {
    return m.game < kId53Limit &&
           isValid(m.status) &&
           isValid(m.reason) &&
           std::isfinite(m.serverTime);
}
void encode(const RatingUpdate& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::RatingUpdate));
    w.u64(m.game);
    w.str8(m.category, 7);
    put(w, m.white);
    put(w, m.black);
}
bool decode(const uint8_t* p, size_t n, RatingUpdate& out) {
    Reader r(p, n);
    return r.type(MsgType::RatingUpdate) &&
           r.id53(out.game) &&
           r.str8(out.category, 0, 7) &&
           get(r, out.white) &&
           get(r, out.black) &&
           r.end();
}
bool valid(const RatingUpdate& m) {
    return m.game < kId53Limit &&
           validStr(m.category, 0, 7) &&
           valid(m.white) &&
           valid(m.black);
}
void encode(const S_Gesture& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u8(uint8_t(MsgType::S_Gesture));
    w.u64(m.game);
    w.u16(m.ply);
    w.u8(m.touch);
    w.u8(m.aim);
    w.u16(m.placed);
    w.u8(m.flags);
    w.u32(uint32_t(m.yaw));
    w.u32(uint32_t(m.pitch));
    w.u8(m.lean);
}
bool decode(const uint8_t* p, size_t n, S_Gesture& out) {
    Reader r(p, n);
    return r.type(MsgType::S_Gesture) &&
           r.id53(out.game) &&
           r.u16(out.ply, 0u, 1199u) &&
           r.u8(out.touch, 0u, 64u) &&
           r.u8(out.aim, 0u, 64u) &&
           r.u16(out.placed, 0u, 32767u) &&
           r.u8(out.flags, 0u, 7u) &&
           r.i32(out.yaw, -3142, 3142) &&
           r.i32(out.pitch, -1571, 1571) &&
           r.u8(out.lean, 0u, 100u) &&
           r.end();
}
bool valid(const S_Gesture& m) {
    return m.game < kId53Limit &&
           m.ply <= 1199u &&
           m.touch <= 64u &&
           m.aim <= 64u &&
           m.placed <= 32767u &&
           m.flags <= 7u &&
           m.yaw >= -3142 && m.yaw <= 3142 &&
           m.pitch >= -1571 && m.pitch <= 1571 &&
           m.lean <= 100u;
}

}  // namespace proto
}  // namespace net

#pragma pop_macro("None")
