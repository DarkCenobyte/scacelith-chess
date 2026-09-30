#include "direct_authority.h"
#include "direct_crypto.h"
#include "online_client.h"   // packMove helpers
#include "protocol_gen.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace net {
namespace direct {

namespace P = net::proto;

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int kNone = 2;

uint32_t u32ms(int64_t ms) { return uint32_t(std::max<int64_t>(0, std::min<int64_t>(ms, 0xFFFFFFFFll))); }

// seq of an encoded client->server message (every one starts with it), 0 when too short.
uint32_t seqOf(const uint8_t* p, size_t n) {
    if (n < 5) return 0;
    return uint32_t(p[1]) | uint32_t(p[2]) << 8 | uint32_t(p[3]) << 16 | uint32_t(p[4]) << 24;
}
}  // namespace

uint32_t fenDigest(const std::string& fen) {
    // The first four fields: placement, side, castling, en passant (up to the fourth space).
    size_t end = fen.size();
    int spaces = 0;
    for (size_t i = 0; i < fen.size(); ++i)
        if (fen[i] == ' ' && ++spaces == 4) { end = i; break; }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < end; ++i) {
        h ^= (unsigned char)fen[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t positionHash(const chess::Position& pos) { return fenDigest(pos.fen()); }

std::string sanitizeName(const std::string& in, const char* fallback) {
    // Keep printable UTF-8; drop control characters and malformed sequences.
    std::string s;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len && i + len <= in.size();
        for (size_t k = 1; ok && k < len; ++k) ok = ((unsigned char)in[i + k] >> 6) == 2;
        if (!ok) { ++i; continue; }
        if (len == 1 && (c < 0x20 || c == 0x7F)) { ++i; continue; }
        s.append(in, i, len);
        i += len;
    }
    // Trim spaces, then cut to 24 bytes on a character boundary.
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    if (s.size() > 24) {
        size_t cut = 24;
        while (cut > 0 && ((unsigned char)s[cut] >> 6) == 2) --cut;
        s.resize(cut);
        size_t e = s.find_last_not_of(' ');
        s.resize(e == std::string::npos ? 0 : e + 1);
    }
    return s.empty() ? std::string(fallback) : s;
}

Authority::Authority(const AuthorityConfig& cfg, const std::string& hostName, const std::string& guestName, int hostColorPref,
                     uint64_t firstGameId)
    : cfg_(cfg), nextId_(firstGameId ? firstGameId : 1) {
    names_[HostSide] = sanitizeName(hostName, "Host");
    names_[GuestSide] = sanitizeName(guestName, "Guest");
    if (hostColorPref == int(P::ColorPref::White)) hostColor_ = 0;
    else if (hostColorPref == int(P::ColorPref::Black)) hostColor_ = 1;
    else {
        uint8_t b = 0;
        randomBytes(&b, 1);
        hostColor_ = b & 1;
    }
}

bool Authority::isOver() const { return started_ && status_ != uint8_t(P::GameStatus::Ongoing); }

template <class T> void Authority::emit(Output& out, Side side, const T& msg) {
    std::vector<uint8_t> buf;
    P::encode(msg, buf);
    (side == HostSide ? out.toHost : out.toGuest).push_back(std::move(buf));
}

template <class T> void Authority::emitBoth(Output& out, const T& msg) {
    std::vector<uint8_t> buf;
    P::encode(msg, buf);
    out.toHost.push_back(buf);
    out.toGuest.push_back(std::move(buf));
}

void Authority::error(Output& out, Side side, uint32_t ref, int code, uint64_t game) {
    P::Error e;
    e.ref = ref;
    e.code = P::ErrorCode(code);
    e.fatal = false;
    e.game = game < P::kId53Limit ? game : 0;
    emit(out, side, e);
}

void Authority::gameEvent(Output& out, int kind, int color, uint32_t arg, bool toGuest) {
    P::GameEvent e;
    e.game = id_;
    e.gseq = ++gseq_;
    e.kind = P::GameEventKind(kind);
    e.color = P::Color(color);
    e.arg = arg;
    if (toGuest) emitBoth(out, e);
    else emit(out, HostSide, e);
}

int64_t Authority::clockAt(int color, double now) const {
    if (color == running_ && !isOver()) return std::max<int64_t>(0, remaining_[color] - int64_t(std::floor(now - turnStart_)));
    return std::max<int64_t>(0, remaining_[color]);
}

double Authority::compBound() const {
    double rtt = rtt_[GuestSide] < 0 ? 0 : rtt_[GuestSide];
    return std::min(rtt / 2 + double(cfg_.lagCompRttMarginMs), double(cfg_.lagCompMaxMs));
}

double Authority::flagTime() const {
    if (running_ == kNone) return kInf;
    // The latest moment a move could still arrive in time: the guest may get its compensation.
    double allowance = running_ == colorOf(GuestSide) ? std::min(quota_[running_], compBound()) : 0.0;
    return turnStart_ + double(remaining_[running_]) + allowance;
}

bool Authority::canOffer(int color) const {
    return drawOffers_[color] < cfg_.drawOffersPerGame && plies() - declinedAtPly_[color] >= cfg_.drawOfferCooldownPlies;
}

void Authority::startGame(double now, Output& out) {
    started_ = true;
    id_ = nextId_++;
    gseq_ = 0;
    game_.reset();
    recs_.clear();
    made_.clear();
    remaining_[0] = remaining_[1] = cfg_.baseMs;
    running_ = kNone;
    turnStart_ = now;
    firstMoveDeadline_ = now + double(cfg_.firstMoveMs);
    graceDeadline_ = kInf;
    startedAt_ = now;
    quota_[0] = quota_[1] = double(cfg_.lagQuotaMs);
    drawOfferBy_ = kNone;
    drawOffers_[0] = drawOffers_[1] = 0;
    declinedAtPly_[0] = declinedAtPly_[1] = -1000;
    status_ = uint8_t(P::GameStatus::Ongoing);
    reason_ = uint8_t(P::EndReason::None);
    rematchBy_ = kNone;
    rematchOpen_ = false;
    rematchDeadline_ = kInf;
    if (!sideConnected_[GuestSide]) graceDeadline_ = now + double(cfg_.graceMs);
    sendSnapshot(HostSide, now, out);
    sendSnapshot(GuestSide, now, out);
}

void Authority::sendSnapshot(Side side, double now, Output& out) {
    if (!started_) return;
    P::GameSnapshot s;
    s.game = id_;
    s.gseq = gseq_;
    s.category = "custom";
    s.baseMs = u32ms(cfg_.baseMs);
    s.incMs = u32ms(cfg_.incMs);
    s.rated = false;
    for (int c = 0; c < 2; ++c) {
        Side who = c == hostColor_ ? HostSide : GuestSide;
        P::PlayerInfo pi;
        pi.userId = who == HostSide ? 1 : 2;
        pi.name = names_[who];
        pi.rating = 0;
        pi.provisional = false;
        (c == 0 ? s.white : s.black) = pi;
    }
    s.you = P::Color(colorOf(side));
    s.moves.reserve(recs_.size());
    for (auto& r : recs_) {
        P::MoveRec m;
        m.move = r.move;
        m.spentMs = r.spentMs;
        m.clockMs = r.clockMs;
        s.moves.push_back(m);
    }
    bool over = isOver();
    s.running = P::Color(over ? kNone : running_);
    s.whiteMs = u32ms(clockAt(0, now));
    s.blackMs = u32ms(clockAt(1, now));
    s.serverTime = now;
    s.drawOffer = P::Color(drawOfferBy_);
    s.status = P::GameStatus(status_);
    s.reason = P::EndReason(reason_);
    s.whiteConnected = sideConnected_[hostColor_ == 0 ? HostSide : GuestSide];
    s.blackConnected = sideConnected_[hostColor_ == 1 ? HostSide : GuestSide];
    s.graceMs = std::isfinite(graceDeadline_) && !over ? u32ms(int64_t(graceDeadline_ - now)) : u32ms(cfg_.graceMs);
    s.firstMoveMs = !over && plies() < 2 ? u32ms(int64_t(firstMoveDeadline_ - now)) : 0;
    s.startedAt = startedAt_;
    s.rematch = P::Color(rematchBy_);
    s.autoPress = cfg_.autoPress;
    emit(out, side, s);
}

void Authority::finish(int status, int reason, double now, Output& out) {
    if (running_ != kNone) remaining_[running_] = clockAt(running_, now);
    running_ = kNone;
    status_ = uint8_t(status);
    reason_ = uint8_t(reason);
    drawOfferBy_ = kNone;
    firstMoveDeadline_ = graceDeadline_ = kInf;
    P::GameEnd e;
    e.game = id_;
    e.gseq = ++gseq_;
    e.status = P::GameStatus(status);
    e.reason = P::EndReason(reason);
    e.whiteMs = u32ms(remaining_[0]);
    e.blackMs = u32ms(remaining_[1]);
    e.serverTime = now;
    emitBoth(out, e);
    rematchBy_ = kNone;
    rematchOpen_ = sideConnected_[GuestSide];
    rematchDeadline_ = now + double(cfg_.rematchWindowMs);
}

void Authority::finishFromChess(double now, Output& out) {
    // chess::GameStatus and GameEndReason share the protocol's numbering (schema.js EndReason).
    finish(int(game_.status()), int(game_.endReason()), now, out);
}

void Authority::flagFall(int color, double now, Output& out) {
    remaining_[color] = 0;
    running_ = kNone;
    game_.flagFall(chess::Color(color));   // Timeout, or a draw when the opponent cannot mate
    finishFromChess(now, out);
}

void Authority::onMessage(Side side, const uint8_t* p, size_t n, double now, Output& out) {
    P::MsgType t;
    const uint32_t seq = seqOf(p, n);
    if (!P::peekType(p, n, t)) { error(out, side, seq, int(P::ErrorCode::Malformed), 0); return; }
    const int c = colorOf(side);
    auto sameGame = [&](uint64_t g, int codeIfNot) {
        if (started_ && g == id_) return true;
        error(out, side, seq, codeIfNot, g);
        return false;
    };
    auto running = [&](uint64_t g) {
        if (isOver()) { error(out, side, seq, int(P::ErrorCode::GameOver), g); return false; }
        return true;
    };
    switch (t) {
    case P::MsgType::Move:
        onMove(side, p, n, now, out);
        return;
    case P::MsgType::Resign: {
        P::Resign m;
        if (!P::decode(p, n, m)) break;
        if (!sameGame(m.game, int(P::ErrorCode::NotInGame)) || !running(m.game)) return;
        game_.resign(chess::Color(c));
        finishFromChess(now, out);
        return;
    }
    case P::MsgType::DrawOffer: {
        P::DrawOffer m;
        if (!P::decode(p, n, m)) break;
        if (!sameGame(m.game, int(P::ErrorCode::NotInGame)) || !running(m.game)) return;
        if (drawOfferBy_ == 1 - c) {   // both want a draw
            game_.agreeDraw();
            finishFromChess(now, out);
        } else if (drawOfferBy_ == c || !canOffer(c)) {
            error(out, side, seq, int(P::ErrorCode::DrawOfferLimit), m.game);
        } else {
            drawOfferBy_ = c;
            ++drawOffers_[c];
            gameEvent(out, int(P::GameEventKind::DrawOffered), c, 0);
        }
        return;
    }
    case P::MsgType::DrawAnswer: {
        P::DrawAnswer m;
        if (!P::decode(p, n, m)) break;
        if (!sameGame(m.game, int(P::ErrorCode::NotInGame)) || !running(m.game)) return;
        if (drawOfferBy_ != 1 - c) { error(out, side, seq, int(P::ErrorCode::NoPendingOffer), m.game); return; }
        if (m.accept) {
            game_.agreeDraw();
            finishFromChess(now, out);
        } else {
            declinedAtPly_[1 - c] = plies();
            drawOfferBy_ = kNone;
            gameEvent(out, int(P::GameEventKind::DrawDeclined), c, 0);
        }
        return;
    }
    case P::MsgType::DrawClaim: {
        P::DrawClaim m;
        if (!P::decode(p, n, m)) break;
        if (!sameGame(m.game, int(P::ErrorCode::NotInGame)) || !running(m.game)) return;
        if (!game_.canClaimThreefold() && !game_.canClaimFiftyMove()) {
            error(out, side, seq, int(P::ErrorCode::NothingToClaim), m.game);
            return;
        }
        game_.claimDraw();
        finishFromChess(now, out);
        return;
    }
    case P::MsgType::Abort: {
        P::Abort m;
        if (!P::decode(p, n, m)) break;
        if (!sameGame(m.game, int(P::ErrorCode::NotInGame)) || !running(m.game)) return;
        // Only before one's own first move: White at ply 0, Black at plies 0 and 1.
        if (plies() > c) { error(out, side, seq, int(P::ErrorCode::AbortNotAllowed), m.game); return; }
        finish(int(P::GameStatus::Aborted), int(P::EndReason::Aborted), now, out);
        return;
    }
    case P::MsgType::Resync: {
        P::Resync m;
        if (!P::decode(p, n, m)) break;
        if (!sameGame(m.game, int(P::ErrorCode::NotInGame))) return;
        sendSnapshot(side, now, out);
        return;
    }
    case P::MsgType::Rematch: {
        P::Rematch m;
        if (!P::decode(p, n, m)) break;
        if (!started_ || m.game != id_ || !isOver() || !rematchOpen_ || now > rematchDeadline_) {
            error(out, side, seq, int(P::ErrorCode::RematchUnavailable), m.game);
            return;
        }
        if (!m.accept) {
            rematchOpen_ = false;
            rematchBy_ = kNone;
            gameEvent(out, int(P::GameEventKind::RematchDeclined), c, 0);
        } else if (rematchBy_ == 1 - c) {
            hostColor_ = 1 - hostColor_;   // colours swapped
            startGame(now, out);
        } else if (rematchBy_ == kNone) {
            rematchBy_ = c;
            gameEvent(out, int(P::GameEventKind::RematchOffered), c, 0);
        }
        return;
    }
    default:
        error(out, side, seq, int(P::ErrorCode::ProtocolViolation), 0);
        return;
    }
    error(out, side, seq, int(P::ErrorCode::Malformed), 0);
}

void Authority::onMove(Side side, const uint8_t* p, size_t n, double now, Output& out) {
    P::Move m;
    if (!P::decode(p, n, m)) { error(out, side, seqOf(p, n), int(P::ErrorCode::Malformed), 0); return; }
    const int c = colorOf(side);
    if (!started_ || m.game != id_) { error(out, side, m.seq, int(P::ErrorCode::NotInGame), m.game); return; }
    auto reject = [&](P::ErrorCode code) {
        P::MoveRejected r;
        r.game = id_;
        r.ply = m.ply;
        r.move = m.move;
        r.code = code;
        emit(out, side, r);
    };
    if (isOver()) { reject(P::ErrorCode::GameOver); return; }
    const int cur = plies();
    if (int(m.ply) < cur) {
        // The same move again (a resend after a reconnection): answer with the original MoveMade.
        if (recs_[m.ply].move == m.move && int(m.ply % 2) == c) (side == HostSide ? out.toHost : out.toGuest).push_back(made_[m.ply]);
        else reject(P::ErrorCode::StalePly);
        return;
    }
    const chess::Position& pos = game_.position();
    if (int(m.ply) > cur || m.posHash != positionHash(pos)) {
        reject(P::ErrorCode::Desync);
        sendSnapshot(side, now, out);
        return;
    }
    if (int(pos.sideToMove()) != c) { reject(P::ErrorCode::NotYourTurn); return; }
    const int promo = movePromo(m.move);
    chess::Move mv = pos.findLegal(chess::Square(moveFrom(m.move)), chess::Square(moveTo(m.move)), chess::PieceType(promo));
    const bool isPromotion = mv.valid() && (mv.flags & chess::MovePromotion);
    if (!mv.valid() || (!isPromotion && promo != 0)) { reject(P::ErrorCode::IllegalMove); return; }

    // Clock (DESIGN 6.1): plies 0 and 1 are free.
    uint32_t spent = 0;
    if (cur >= 2) {
        double elapsed = std::max(0.0, now - turnStart_);
        double comp = 0;
        if (side == GuestSide) {
            double think = std::min(std::max(0.0, double(m.thinkMs)), elapsed);
            double lag = elapsed - think;
            comp = std::max(0.0, std::min({lag, compBound(), quota_[c]}));
            quota_[c] = std::min(quota_[c] - comp + double(cfg_.lagQuotaGainMs), double(cfg_.lagQuotaMs));
        }
        int64_t charged = int64_t(std::floor(elapsed - comp));
        if (remaining_[c] - charged <= 0) {
            reject(P::ErrorCode::FlagFell);
            flagFall(c, now, out);
            return;
        }
        remaining_[c] -= charged;
        remaining_[c] += cfg_.incMs;
        spent = u32ms(charged);
    }

    uint8_t flags = mv.flags;   // chess::MoveFlags share the low bits of the protocol MoveFlag
    game_.play(mv);
    if (game_.position().inCheck()) flags |= P::MoveFlag::Check;
    if (game_.endReason() == chess::GameEndReason::Checkmate) flags |= P::MoveFlag::Mate;
    recs_.push_back({m.move, spent, u32ms(remaining_[c])});
    const int next = 1 - c;
    const int now_plies = cur + 1;
    if (now_plies == 1) {
        firstMoveDeadline_ = now + double(cfg_.firstMoveMs);   // Black's first move
        running_ = kNone;
    } else {
        firstMoveDeadline_ = kInf;
        running_ = next;
        turnStart_ = now;
    }
    // Draw offers: the move declines the opponent's pending offer; it may carry its own.
    bool declined = false;
    if (drawOfferBy_ == next) {
        declined = true;
        declinedAtPly_[next] = now_plies;
        drawOfferBy_ = kNone;
    }
    bool offered = false, offerRefused = false;
    if (m.drawOffer && drawOfferBy_ != c) {
        if (canOffer(c)) {
            drawOfferBy_ = c;
            ++drawOffers_[c];
            offered = true;
        } else {
            offerRefused = true;
        }
    }
    P::MoveMade mm;
    mm.game = id_;
    mm.gseq = ++gseq_;
    mm.ply = uint16_t(cur);
    mm.move = m.move;
    mm.flags = flags;
    mm.spentMs = spent;
    mm.whiteMs = u32ms(clockAt(0, now));
    mm.blackMs = u32ms(clockAt(1, now));
    mm.serverTime = now;
    mm.drawOffer = offered;
    mm.firstMoveMs = now_plies == 1 ? u32ms(cfg_.firstMoveMs) : 0;
    std::vector<uint8_t> buf;
    P::encode(mm, buf);
    made_.push_back(buf);
    out.toHost.push_back(buf);
    out.toGuest.push_back(std::move(buf));
    if (declined) gameEvent(out, int(P::GameEventKind::DrawDeclined), c, 0);
    if (offerRefused) error(out, side, m.seq, int(P::ErrorCode::DrawOfferLimit), id_);
    if (game_.isOver()) finishFromChess(now, out);
}

void Authority::onDisconnect(Side side, double now, Output& out) {
    if (side != GuestSide || !sideConnected_[GuestSide]) return;
    sideConnected_[GuestSide] = false;
    if (!started_) return;
    if (!isOver()) {
        graceDeadline_ = now + double(cfg_.graceMs);
        gameEvent(out, int(P::GameEventKind::PlayerDisconnected), colorOf(GuestSide), u32ms(cfg_.graceMs), false);
    } else if (rematchOpen_) {
        rematchOpen_ = false;
        rematchBy_ = kNone;
        gameEvent(out, int(P::GameEventKind::RematchDeclined), kNone, 0, false);
    }
}

void Authority::onReconnect(Side side, double now, Output& out) {
    if (side != GuestSide) return;
    bool was = sideConnected_[GuestSide];
    sideConnected_[GuestSide] = true;
    if (!started_) return;
    graceDeadline_ = kInf;
    sendSnapshot(GuestSide, now, out);
    if (!was && !isOver()) gameEvent(out, int(P::GameEventKind::PlayerReconnected), colorOf(GuestSide), 0, false);
}

void Authority::onRtt(Side side, double rttMs) {
    double v = std::min(std::max(rttMs, 0.0), 2000.0);
    rtt_[side] = rtt_[side] < 0 ? v : rtt_[side] * 0.8 + v * 0.2;
}

void Authority::tick(double now, Output& out) {
    if (!started_) return;
    if (!isOver()) {
        const int n = plies();
        if (n < 2 && now >= firstMoveDeadline_) {
            finish(int(P::GameStatus::Aborted), int(P::EndReason::NoShow), now, out);
            return;
        }
        if (running_ != kNone && now >= flagTime()) {
            flagFall(running_, now, out);
            return;
        }
        if (now >= graceDeadline_) {
            const int gc = colorOf(GuestSide);
            if (n < 2) finish(int(P::GameStatus::Aborted), int(P::EndReason::NoShow), now, out);
            else if (!game_.position().canColorMate(chess::Color(1 - gc)))
                finish(int(P::GameStatus::Draw), int(P::EndReason::AbandonmentVsInsufficient), now, out);
            else finish(int(gc == 0 ? P::GameStatus::BlackWins : P::GameStatus::WhiteWins), int(P::EndReason::Abandonment), now, out);
        }
    } else if (rematchOpen_ && now >= rematchDeadline_) {
        rematchOpen_ = false;
        rematchBy_ = kNone;
        gameEvent(out, int(P::GameEventKind::RematchDeclined), kNone, 0);
    }
}

double Authority::nextDeadline() const {
    if (!started_) return kInf;
    if (isOver()) return rematchOpen_ ? rematchDeadline_ : kInf;
    double d = graceDeadline_;
    if (plies() < 2) d = std::min(d, firstMoveDeadline_);
    return std::min(d, flagTime());
}

void Authority::hostLeaves(double now, Output& out) {
    if (!started_ || isOver()) return;
    game_.resign(chess::Color(hostColor_));
    finishFromChess(now, out);
}

}  // namespace direct
}  // namespace net
