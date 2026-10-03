// Online client tests: protocol codec against the JavaScript codec's vectors, position digest,
// JSON, crypto (hash / base64 / PKCE / proof of work), the folders of net::sys (also the platform
// layer's exeDirectory, userDataDirectory and appDataDirectory), credential store isolation,
// endpoint validation, and OnlineClient end to end against a fake server on the loopback interface
// (plain HTTP + WebSocket, the insecureDev mode): login with a proof of work, account, Hello /
// Welcome, ping and clock offset, queue, moves, reconnection, 4003 and logout; the pacing of the
// client Ping (Welcome.clientPingMs) and of the reconnections (full server, shutdown, /info reuse);
// live gestures (wire units, pacing, the opponent's); the account API (history, game details, PGN,
// signed-in devices, preferences, e-mail change, export, deletion, animated GIFs) against a
// scripted server (tests/http_fake.h), a refused session signing the game out (game::AccountData
// fed the client's events), and the move of the official server's saved session from port 44664
// to 443.
//
// Vectors: tests/data/net-protocol-vectors.json (dedicated-server/tools/gen-cpp-test-vectors.js)
// and, when present, dedicated-server/test/fixtures/protocol-vectors.json. The files are looked
// up from the current directory (run from the repository root), $SCACELITH_SOURCE_DIR and the
// executable's parent directories.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "test.h"
#include "alloc_fail.h"
#include "http_fake.h"
#include "repo_files.h"
#include "chess/chess.h"
#include "core/log.h"
#include "game/online_account.h"
#include "net/credential_store.h"
#include "net/crypto.h"
#include "net/json.h"
#include "net/net_sys.h"
#include "net/online_client.h"
#include "net/protocol_gen.h"
#include "net/transport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace pr = net::proto;
using net::json::Value;

namespace {

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v;
    net::crypto::hexDecode(s, v);
    return v;
}

// ---- JSON <-> generated structs through visitFields ----

struct NullVisitor {
    template <class F> void operator()(const char*, F&) {}
};
template <class T, class = void> struct HasFields : std::false_type {};
template <class T>
struct HasFields<T, std::void_t<decltype(pr::visitFields(std::declval<T&>(), std::declval<NullVisitor&>()))>> : std::true_type {};
template <class T> struct IsVector : std::false_type {};
template <class T> struct IsVector<std::vector<T>> : std::true_type {};

template <class T> bool fromJson(const Value& v, T& out);

struct FromJsonVisitor {
    const Value& o;
    bool ok = true;
    template <class F> void operator()(const char* name, F& f) {
        if (!o.has(name) || !fromJson(o[name], f)) ok = false;
    }
};

template <class T> bool fromJson(const Value& v, T& out) {
    if constexpr (std::is_same_v<T, bool>) {
        if (!v.isBool()) return false;
        out = v.asBool();
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!v.isString()) return false;
        out = v.asString();
    } else if constexpr (std::is_same_v<T, double>) {
        if (!v.isNumber()) return false;
        out = v.asNumber();
    } else if constexpr (std::is_enum_v<T> || std::is_integral_v<T>) {
        if (!v.isNumber()) return false;
        out = T(v.asInt());
    } else if constexpr (IsVector<T>::value) {
        if (!v.isArray()) return false;
        out.clear();
        for (const Value& it : v.items()) {
            typename T::value_type x;
            if (!fromJson(it, x)) return false;
            out.push_back(x);
        }
    } else {
        static_assert(HasFields<T>::value, "unsupported field type");
        if (!v.isObject()) return false;
        FromJsonVisitor fv{v};
        pr::visitFields(out, fv);
        return fv.ok;
    }
    return true;
}

template <class T> Value toJson(const T& v);

struct ToJsonVisitor {
    Value& o;
    template <class F> void operator()(const char* name, const F& f) { o.set(name, toJson(f)); }
};

template <class T> Value toJson(const T& v) {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string> || std::is_same_v<T, double>) {
        return Value(v);
    } else if constexpr (std::is_enum_v<T>) {
        return Value(int64_t(v));
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return Value(int64_t(v));
    } else if constexpr (std::is_integral_v<T>) {
        return Value(uint64_t(v));
    } else if constexpr (IsVector<T>::value) {
        Value a = Value::array();
        for (auto& x : v) a.push(toJson(x));
        return a;
    } else {
        Value o = Value::object();
        ToJsonVisitor tv{o};
        pr::visitFields(v, tv);
        return o;
    }
}

bool sameJson(const Value& a, const Value& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Value::Type::Null: return true;
    case Value::Type::Bool: return a.asBool() == b.asBool();
    case Value::Type::Number: return a.asNumber() == b.asNumber();
    case Value::Type::String: return a.asString() == b.asString();
    case Value::Type::Array:
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!sameJson(a[i], b[i])) return false;
        return true;
    case Value::Type::Object:
        if (a.size() != b.size()) return false;
        for (auto& m : a.members())
            if (!b.has(m.first) || !sameJson(m.second, b[m.first])) return false;
        return true;
    }
    return false;
}

std::vector<pr::MsgType> allTypes() {
    std::vector<pr::MsgType> v;
    for (int t = 0; t < 256; ++t)
        if (pr::messageName(pr::MsgType(t))) v.push_back(pr::MsgType(t));
    return v;
}

// Checks one valid vector: encode byte-exact, decode round trip, valid(), other decoders refuse.
bool checkValidVector(const std::string& name, const Value* msg, const std::vector<uint8_t>& bytes) {
    pr::MsgType t;
    if (!pr::peekType(bytes.data(), bytes.size(), t)) {
        std::fprintf(stderr, "  vector %s: unknown type\n", name.c_str());
        return false;
    }
    bool ok = true;
    pr::withMessage(t, [&](auto& m) {
        using M = std::decay_t<decltype(m)>;
        if (msg) {
            if (!fromJson(*msg, m)) { std::fprintf(stderr, "  vector %s: fields do not map\n", name.c_str()); ok = false; return; }
            std::vector<uint8_t> enc;
            pr::encode(m, enc);
            if (enc != bytes) { std::fprintf(stderr, "  vector %s: encoding differs\n", name.c_str()); ok = false; }
            if (!pr::valid(m)) { std::fprintf(stderr, "  vector %s: valid() is false\n", name.c_str()); ok = false; }
        }
        M d;
        if (!pr::decode(bytes.data(), bytes.size(), d)) { std::fprintf(stderr, "  vector %s: decode failed\n", name.c_str()); ok = false; return; }
        std::vector<uint8_t> again;
        pr::encode(d, again);
        if (again != bytes) { std::fprintf(stderr, "  vector %s: decode/encode differs\n", name.c_str()); ok = false; }
        if (msg && !sameJson(toJson(d), *msg)) {
            std::fprintf(stderr, "  vector %s: decoded %s\n    expected %s\n", name.c_str(), toJson(d).dump().c_str(), msg->dump().c_str());
            ok = false;
        }
    });
    for (pr::MsgType other : allTypes()) {
        if (other == t) continue;
        pr::withMessage(other, [&](auto& m) {
            if (pr::decode(bytes.data(), bytes.size(), m)) { std::fprintf(stderr, "  vector %s decodes as another type\n", name.c_str()); ok = false; }
        });
    }
    return ok;
}

// A malformed frame must be refused by every typed decoder.
// dir: "c2s" (bytes received by a server or a direct-match host: only client message decoders
// apply), "s2c" (received by a client: only server message decoders), "" (every decoder). A
// "wrong direction" vector is a valid message of the other direction, refused by dispatch.
bool checkMalformed(const std::string& name, const std::vector<uint8_t>& bytes, const std::string& dir = "") {
    bool ok = true;
    for (pr::MsgType t : allTypes()) {
        bool s2c = int(t) >= 0x80;
        if ((dir == "c2s" && s2c) || (dir == "s2c" && !s2c)) continue;
        pr::withMessage(t, [&](auto& m) {
            if (pr::decode(bytes.data(), bytes.size(), m)) {
                std::fprintf(stderr, "  malformed vector accepted as %s: %s\n", pr::messageName(t), name.c_str());
                ok = false;
            }
        });
    }
    return ok;
}

}  // namespace

// =============================================================================================
// Protocol codec
// =============================================================================================

TEST(net_protocol_constants) {
    CHECK_EQ(pr::kProtocolVersion, 2);
    CHECK_EQ(pr::kProtocolMin, 2);
    CHECK_EQ(std::string(pr::kWsSubprotocol), std::string("scacelith.v1"));
    CHECK_EQ(int(pr::MsgType::Move), 0x20);
    CHECK_EQ(int(pr::MsgType::C_Ping), 0x02);
    CHECK_EQ(int(pr::MsgType::S_Ping), 0x82);
    CHECK_EQ(int(pr::MsgType::C_Gesture), 0x28);
    CHECK_EQ(int(pr::MsgType::S_Gesture), 0xA6);
    CHECK_EQ(int(pr::GestureFlag::Glance), 1);
    CHECK_EQ(int(pr::GestureFlag::Promoting), 2);
    CHECK_EQ(int(pr::GestureFlag::Side), 4);
    CHECK_EQ(int(pr::NoticeCode::RatingRestored), 7);
    CHECK_EQ(std::string(pr::messageName(pr::MsgType::S_Pong)), std::string("S_Pong"));
    CHECK(pr::messageName(pr::MsgType(0x7F)) == nullptr);
    CHECK(pr::isValid(pr::EndReason::BothDisconnected));
    CHECK(!pr::isValid(pr::EndReason(14)));
    CHECK_EQ(std::string(pr::enumName(pr::ErrorCode::IllegalMove)), std::string("IllegalMove"));
    CHECK_EQ(pr::CloseCode::Unauthorized, 4003);
    CHECK_EQ(pr::CloseCode::ServerFull, 4000 + int(pr::ErrorCode::ServerFull));
}

TEST(net_protocol_vectors) {
    std::string path;
    std::string text = readRepoFile("tests/data/net-protocol-vectors.json", size_t(64) << 20, &path);
    CHECK(!text.empty());
    if (text.empty()) {
        std::fprintf(stderr, "  tests/data/net-protocol-vectors.json not found (run from the repository root)\n");
        return;
    }
    Value doc;
    std::string err;
    net::json::Limits lim;
    lim.maxBytes = 64 << 20;
    lim.maxElements = 10000000;
    CHECK(net::json::parse(text, doc, &err, lim));
    CHECK_EQ(uint32_t(doc["schemaHash"].asInt()), pr::kSchemaHash);   // regenerate the vectors after a schema change
    int valid = 0, malformed = 0;
    for (const Value& v : doc["valid"].items()) {
        CHECK(checkValidVector(v["name"].asString(), &v["msg"], unhex(v["hex"].asString())));
        ++valid;
    }
    for (const Value& v : doc["malformed"].items()) {
        CHECK(checkMalformed(v["name"].asString(), unhex(v["hex"].asString())));
        ++malformed;
    }
    for (const Value& v : doc["fnv1a32"].items()) {
        // positionDigest of a FEN prefix is FNV-1a of that very text.
        std::string t = v["text"].asString();
        if (!t.empty()) CHECK_EQ(net::positionDigest(t + " 0 1"), uint32_t(v["hash"].asInt()));
    }
    CHECK(valid >= 40);
    CHECK(malformed >= 150);
    std::fprintf(stderr, "  %s: %d valid, %d malformed\n", path.c_str(), valid, malformed);
}

// Golden vectors of the protocol owner, when that file exists (format read tolerantly).
TEST(net_protocol_shared_fixture) {
    std::string path;
    std::string text = readRepoFile("dedicated-server/test/fixtures/protocol-vectors.json", size_t(64) << 20, &path);
    if (text.empty()) SKIP("dedicated-server/test/fixtures/protocol-vectors.json not present");
    Value doc;
    net::json::Limits lim;
    lim.maxBytes = 64 << 20;
    lim.maxElements = 10000000;
    CHECK(net::json::parse(text, doc, nullptr, lim));
    for (const char* k : {"schemaHash", "schema_hash", "SCHEMA_HASH"})
        if (doc[k].isNumber() && uint32_t(doc[k].asInt()) != pr::kSchemaHash)
            SKIP("the shared fixture has another schema hash");
    auto hexOf = [](const Value& e) {
        for (const char* k : {"hex", "bytes", "encoded", "frame"})
            if (e[k].isString()) return e[k].asString();
        return std::string();
    };
    auto msgOf = [](const Value& e) -> const Value* {
        for (const char* k : {"msg", "message", "object", "value", "fields", "decoded"})
            if (e[k].isObject()) return &e[k];
        return nullptr;
    };
    int valid = 0, malformed = 0;
    auto validList = [&](const Value& list) {
        for (const Value& e : list.items()) {
            std::string h = hexOf(e);
            if (h.empty()) continue;
            const Value* m = msgOf(e);
            Value stripped;
            if (m) {   // drop keys that are not fields ("type")
                stripped = Value::object();
                for (auto& kv : m->members())
                    if (kv.first != "type") stripped.set(kv.first, kv.second);
            }
            CHECK(checkValidVector("shared:" + e["name"].asString(), m ? &stripped : nullptr, unhex(h)));
            ++valid;
        }
    };
    auto badList = [&](const Value& list) {
        for (const Value& e : list.items()) {
            std::string h = e.isString() ? e.asString() : hexOf(e);
            if (e.isObject() && h.empty()) continue;
            CHECK(checkMalformed("shared:" + e["name"].asString(), unhex(h), e["dir"].isString() ? e["dir"].asString() : ""));
            ++malformed;
        }
    };
    if (doc.isArray()) validList(doc);
    for (const char* k : {"valid", "vectors", "messages", "golden"})
        if (doc[k].isArray()) validList(doc[k]);
    for (const char* k : {"malformed", "invalid", "bad", "reject", "rejected"})
        if (doc[k].isArray()) badList(doc[k]);
    std::fprintf(stderr, "  %s: %d valid, %d malformed\n", path.c_str(), valid, malformed);
}

TEST(net_protocol_valid_checks) {
    pr::Move m;
    m.game = 1;
    CHECK(pr::valid(m));
    m.ply = 1200;
    CHECK(!pr::valid(m));
    m.ply = 3;
    m.move = 0x8000;
    CHECK(!pr::valid(m));
    m.move = 1;
    m.game = pr::kId53Limit;
    CHECK(!pr::valid(m));
    pr::Hello h;
    h.token = "short";
    CHECK(!pr::valid(h));
    h.token = std::string(20, 'a');
    CHECK(pr::valid(h));
    h.client = "bad\xC3";
    CHECK(!pr::valid(h));
    pr::MoveMade mm;
    mm.serverTime = std::nan("");
    CHECK(!pr::valid(mm));
    pr::Error e;
    e.code = pr::ErrorCode(12);
    CHECK(!pr::valid(e));
    pr::ChallengeCreate c;
    c.baseSec = 14;
    CHECK(!pr::valid(c));
    c.baseSec = 15;
    CHECK(pr::valid(c));
    // Oversized strings are cut on encode so the frame stays well formed (and valid() says no).
    pr::QueueJoin q;
    q.category = "123456789";
    CHECK(!pr::valid(q));
    std::vector<uint8_t> buf;
    pr::encode(q, buf);
    CHECK_EQ(buf.size(), size_t(1 + 4 + 1 + 7 + 1));
    pr::MsgType t;
    CHECK(pr::peekType(buf.data(), buf.size(), t) && t == pr::MsgType::QueueJoin);
    CHECK(!pr::peekType(nullptr, 0, t));
}

TEST(net_position_digest) {
    // Values of fnv1a32() in dedicated-server/src/protocol/index.js.
    const std::string start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    CHECK_EQ(net::positionDigest(start), 923150620u);
    CHECK_EQ(net::positionDigest("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"), 1150555523u);
    CHECK_EQ(net::positionDigest("r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq d6 0 3"), 4101590597u);
    CHECK_EQ(net::positionDigest("8/8/8/8/8/8/8/K6k w - - 12 40"), 132864131u);
    CHECK_EQ(net::positionDigest(""), 2166136261u);
    // The game's own FEN gives the same digests (en passant only when capturable).
    chess::Position p;
    CHECK_EQ(net::positionDigest(p.fen()), 923150620u);
    p.makeMove(p.findLegal(chess::makeSquare(4, 1), chess::makeSquare(4, 3)));
    CHECK_EQ(net::positionDigest(p.fen()), 1150555523u);
    chess::Position q;
    CHECK(q.setFEN("r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq d6 0 3"));
    CHECK_EQ(net::positionDigest(q.fen()), 4101590597u);
}

TEST(net_pack_move) {
    CHECK_EQ(net::packMove(12, 28, 0), uint16_t(1804));
    CHECK_EQ(net::packMove(52, 60, 5), uint16_t(24372));
    uint16_t m = net::packMove(63, 0, 4);
    CHECK_EQ(net::moveFrom(m), 63);
    CHECK_EQ(net::moveTo(m), 0);
    CHECK_EQ(net::movePromo(m), 4);
    CHECK((net::packMove(63, 63, 7) & 0x8000) == 0);
}

// Gesture wire units and bounds (net/gesture.h), and the token bucket that paces them.
TEST(net_gesture_wire) {
    // Out-of-range values are brought within the schema's bounds.
    net::Gesture g;
    g.ply = 1500;
    g.touch = -3;
    g.aim = 70;
    g.placed = 0xffff;
    g.flags = 0xff;
    g.yaw = 4.0f;
    g.pitch = -2.0f;
    g.lean = 1.7f;
    pr::C_Gesture c;
    c.seq = 1;
    c.game = 5;
    net::gestureToWire(g, c);
    CHECK(pr::valid(c));
    CHECK_EQ(int(c.ply), 1199);
    CHECK_EQ(int(c.touch), 64);
    CHECK_EQ(int(c.aim), 64);
    CHECK_EQ(int(c.placed), 0x7fff);
    CHECK_EQ(int(c.flags), 7);
    CHECK_EQ(c.yaw, 3142);
    CHECK_EQ(c.pitch, -1571);
    CHECK_EQ(int(c.lean), 100);
    g.yaw = -9.0f;
    g.pitch = 2.0f;
    g.lean = -0.5f;
    g.ply = -1;
    net::gestureToWire(g, c);
    CHECK_EQ(c.yaw, -3142);
    CHECK_EQ(c.pitch, 1571);
    CHECK_EQ(int(c.lean), 0);
    CHECK_EQ(int(c.ply), 0);
    g.yaw = std::nanf("");
    g.pitch = INFINITY;
    g.lean = std::nanf("");
    net::gestureToWire(g, c);
    CHECK(c.yaw == 0 && c.pitch == 0 && c.lean == 0);

    // Round trip through an encoded S_Gesture: milliradians and percent.
    net::Gesture h;
    h.ply = 42;
    h.touch = 12;
    h.aim = 28;
    h.placed = net::packMove(12, 28, 0);
    h.flags = uint8_t(pr::GestureFlag::Glance | pr::GestureFlag::Side);
    h.yaw = -0.5f;
    h.pitch = -0.3141f;
    h.lean = 0.42f;
    pr::S_Gesture s;
    s.game = 77;
    net::gestureToWire(h, s);
    std::vector<uint8_t> buf;
    pr::encode(s, buf);
    pr::S_Gesture d;
    CHECK(pr::decode(buf.data(), buf.size(), d));
    CHECK_EQ(d.game, uint64_t(77));
    CHECK_EQ(d.pitch, -314);
    net::Gesture r = net::gestureFromWire(d);
    CHECK(r.sameState(h));
    CHECK(std::fabs(r.yaw - h.yaw) < 1e-6f);
    CHECK(std::fabs(r.pitch + 0.314f) < 1e-6f);
    CHECK(std::fabs(r.lean - h.lean) < 1e-6f);
    net::Gesture moved = r;
    moved.yaw += 0.1f;
    CHECK(moved.sameState(r));
    moved.aim = 36;
    CHECK(!moved.sameState(r));

    // The bucket: full after reset, then 'rate' per second; rate 0 lets nothing through.
    net::GestureBucket b;
    b.reset(1000.0, 10, 3);
    CHECK(b.enabled());
    CHECK(b.take(1000.0) && b.take(1000.0) && b.take(1000.0));
    CHECK(!b.take(1000.0));
    CHECK_EQ(b.readyAtMs(1000.0), 1100.0);
    CHECK(!b.take(1050.0));
    CHECK_EQ(b.readyAtMs(1050.0), 1100.0);
    CHECK(b.take(1100.0));
    CHECK(!b.take(1100.0));
    CHECK_EQ(b.readyAtMs(10000.0), 10000.0);
    CHECK(b.take(10000.0) && b.take(10000.0) && b.take(10000.0));   // never more than the capacity
    CHECK(!b.take(10000.0));
    CHECK(!b.take(9000.0));                                           // time never goes back
    b.reset(0.0, 0, 20);
    CHECK(!b.enabled());
    CHECK(!b.take(0.0) && !b.take(60000.0));
    CHECK_EQ(b.readyAtMs(5.0), 5.0);
    CHECK_EQ(net::gestureSendCapacity(8), 7);
    CHECK_EQ(net::gestureSendCapacity(1), 1);
    CHECK_EQ(net::gestureSendCapacity(0), 1);
}

namespace {

// A sender paced at gestureSendCapacity(burst) against the receiver's bucket (rate, burst), both
// full at time 0. The sender has a new Gesture in every 16 ms frame where moving(t) holds and
// sends the latest one when its bucket allows; delayMs(t) is the network delay of a message sent
// at t, and the order is kept (TCP): a message never arrives before the one sent before it.
// Returns every message sent: its send time and whether the receiver kept it.
template <class Moving, class Delay>
std::vector<std::pair<double, bool>> paceAgainstReceiver(int rate, int burst, double spanMs, Moving moving, Delay delayMs) {
    net::GestureBucket out, in;
    out.reset(0.0, rate, net::gestureSendCapacity(burst));
    in.reset(0.0, rate, burst);
    std::vector<std::pair<double, bool>> sent;
    bool pending = false;
    double arrival = 0.0;
    for (double t = 0.0; t < spanMs; t += 16.0) {
        pending = pending || moving(t);
        if (!pending || !out.take(t)) continue;
        pending = false;
        arrival = std::max(arrival, t + delayMs(t));
        sent.push_back({t, in.take(arrival)});
    }
    return sent;
}

}  // namespace

TEST(net_gesture_pacing_against_the_receiver_bucket) {
    // What the spare token of gestureSendCapacity() covers: delays that vary by up to one
    // interval (1000 / rate ms) never make the receiver drop a Gesture, whatever the moves.
    uint32_t seed = 12345;
    auto rnd = [&seed] {   // xorshift32, 0..1
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return double(seed) / 4294967296.0;
    };
    const int buckets[][2] = {{4, 8}, {10, 20}, {2, 4}, {1, 2}};
    for (auto& b : buckets) {
        const double interval = 1000.0 / b[0];
        double phaseEnd = 0.0;
        bool movingNow = false;
        auto moving = [&](double t) {   // moves and rests of 0 to 3 s each
            if (t >= phaseEnd) {
                movingNow = !movingNow;
                phaseEnd = t + 3000.0 * rnd();
            }
            return movingNow;
        };
        auto jitter = [&](double) { return 20.0 + 0.95 * interval * rnd(); };
        auto sent = paceAgainstReceiver(b[0], b[1], 120000.0, moving, jitter);
        int dropped = 0;
        for (auto& m : sent) dropped += !m.second;
        CHECK(sent.size() > size_t(20 * b[0]));
        CHECK_EQ(dropped, 0);
    }
    // A 1 s stall of the link at the server's defaults (4, 8) while the player moves: what was
    // sent meanwhile arrives at once, and the receiver drops the part beyond its burst, the latest
    // state of the bunch included. The Gestures after it get through again.
    auto always = [](double) { return true; };
    auto stall = [](double t) { return t < 1000.0 ? 1000.0 - t + 20.0 : 20.0; };
    auto sent = paceAgainstReceiver(4, 8, 3000.0, always, stall);
    int inBunch = 0, droppedInBunch = 0;
    bool latestDropped = false, keptAfter = true;
    double lastDrop = 0.0;
    for (auto& m : sent) {
        if (m.first < 1000.0) {
            ++inBunch;
            droppedInBunch += !m.second;
            latestDropped = !m.second;
        }
        if (!m.second) lastDrop = m.first;
    }
    for (auto& m : sent)
        if (m.first > lastDrop) keptAfter = keptAfter && m.second;
    CHECK(inBunch > 8);
    CHECK_EQ(droppedInBunch, inBunch - 8);
    CHECK(latestDropped);
    CHECK(lastDrop < 1500.0 && keptAfter && sent.back().first > 2500.0);
}

// =============================================================================================
// JSON
// =============================================================================================

TEST(net_json_parse) {
    Value v;
    CHECK(net::json::parse(" {\"a\": [1, -2.5, 3e2, true, false, null], \"s\": \"x\\n\\u00e9\\ud83d\\ude00\\/\", \"o\": {}} ", v));
    CHECK(v.isObject());
    CHECK_EQ(v["a"].size(), size_t(6));
    CHECK_EQ(v["a"][1].asNumber(), -2.5);
    CHECK_EQ(v["a"][2].asInt(), int64_t(300));
    CHECK(v["a"][3].asBool());
    CHECK(v["a"][5].isNull());
    CHECK_EQ(v["s"].asString(), std::string("x\n\xC3\xA9\xF0\x9F\x98\x80/"));
    CHECK(v["o"].isObject());
    CHECK(v["missing"].isNull());
    CHECK(v["a"][99].isNull());
    CHECK(net::json::parse("9007199254740991", v) && v.asInt() == 9007199254740991LL);
    CHECK(net::json::parse("1727000000123.25", v) && v.asNumber() == 1727000000123.25);
    CHECK(net::json::parse("-0", v) && v.asNumber() == 0);
    CHECK(net::json::parse("1e400", v) && std::isinf(v.asNumber()));
    CHECK(net::json::parse("\"raw \xE6\xBC\xA2\"", v) && v.asString() == "raw \xE6\xBC\xA2");
    // duplicate names: the last one wins
    CHECK(net::json::parse("{\"k\":1,\"k\":2}", v) && v["k"].asInt() == 2 && v.size() == 1);
}

// A large object is read in time proportional to its size (a hostile server's answer of 1 MiB,
// up to 100000 members, must not keep a network thread busy), with the same rule for duplicate
// names however many members come before them: the last value, at the position of the first.
TEST(net_json_large_object) {
    std::string doc = "{";
    for (int i = 0; i < 90000; ++i) {
        char member[16];
        std::snprintf(member, sizeof member, "%s\"%05x\":%d", i ? "," : "", i, i % 10);
        doc += member;
    }
    doc += "}";
    Value v;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(net::json::parse(doc, v));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  %zu bytes, 90000 members: %.1f ms\n", doc.size(), ms);
    CHECK(ms < 2000);   // seconds before (a scan of the kept members per member)
    CHECK_EQ(v.size(), size_t(90000));
    CHECK_EQ(v["15f8f"].asInt(), int64_t(89999 % 10));

    std::string dup = "{";
    for (int i = 0; i < 30; ++i) dup += (i ? ",\"a" : "\"a") + std::to_string(i) + "\":" + std::to_string(i);
    dup += ",\"a3\":\"x\",\"a20\":\"y\",\"a29\":null,\"a3\":\"z\",\"b\":1}";
    CHECK(net::json::parse(dup, v));
    CHECK_EQ(v.size(), size_t(31));
    CHECK_EQ(v.members()[3].first, std::string("a3"));
    CHECK_EQ(v.members()[3].second.asString(), std::string("z"));
    CHECK_EQ(v.members()[20].second.asString(), std::string("y"));
    CHECK(v.members()[29].second.isNull());
    CHECK_EQ(v.members()[30].first, std::string("b"));
    CHECK_EQ(v.dump().substr(0, 32), std::string("{\"a0\":0,\"a1\":1,\"a2\":2,\"a3\":\"z\",\""));
}

TEST(net_json_rejects) {
    const char* bad[] = {"", "{", "[1,]", "{\"a\":1,}", "01", "1.", ".5", "+1", "-", "1e", "tru", "nul", "NaN", "Infinity",
                         "\"abc", "\"a\\x\"", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"a\tb\"", "\"\xC3\x28\"",
                         "\"\xED\xA0\x80\"", "\"\xC0\xAF\"", "{\"a\" 1}", "{1:2}", "[1 2]", "1 2", "\"\\u12\"", "[", "]"};
    for (const char* b : bad) {
        Value v;
        std::string err;
        bool ok = net::json::parse(b, v, &err);
        if (ok) std::fprintf(stderr, "  accepted: %s\n", b);
        CHECK(!ok);
    }
    // limits
    net::json::Limits lim;
    lim.maxDepth = 4;
    Value v;
    CHECK(net::json::parse("[[[[1]]]]", v, nullptr, lim));
    CHECK(!net::json::parse("[[[[[1]]]]]", v, nullptr, lim));
    lim = net::json::Limits();
    lim.maxBytes = 10;
    CHECK(!net::json::parse("\"0123456789\"", v, nullptr, lim));
    lim = net::json::Limits();
    lim.maxElements = 3;
    CHECK(!net::json::parse("[1,2,3]", v, nullptr, lim));
    std::string deep(100000, '[');
    CHECK(!net::json::parse(deep, v));
}

// Limits::keepDepth: the whole document is checked, only its head is kept (the account export).
TEST(net_json_keep_depth) {
    net::json::Limits lim;
    lim.keepDepth = 1;
    Value v;
    CHECK(net::json::parse(R"({"format":"x","version":1,"list":[1,{"a":[2]}],"obj":{"k":"v"}})", v, nullptr, lim));
    CHECK_EQ(v["format"].asString(), std::string("x"));
    CHECK_EQ(v["version"].asInt(), int64_t(1));
    CHECK(v["list"].isArray());
    CHECK_EQ(v["list"].size(), size_t(0));
    CHECK(v["obj"].isObject());
    CHECK_EQ(v["obj"].size(), size_t(0));
    CHECK(!net::json::parse(R"({"format":"x","list":[1,{"a":[2,]}]})", v, nullptr, lim));   // still checked
    CHECK(!net::json::parse(R"({"list":["\ud800"]})", v, nullptr, lim));
    lim.keepDepth = 2;
    CHECK(net::json::parse(R"({"list":[1,{"a":[2]}]})", v, nullptr, lim));
    CHECK_EQ(v["list"].size(), size_t(2));
    CHECK_EQ(v["list"][1].size(), size_t(0));
    CHECK(net::json::parse("[[1],[2]]", v, nullptr, lim));
    CHECK_EQ(v[0][0].asInt(), int64_t(1));

    // Limits::maxKept: the kept containers may have that many members or items, the deeper ones any
    // number; the reader stops at the first one too many (not at the end of the document).
    lim = net::json::Limits();
    lim.keepDepth = 1;
    lim.maxKept = 3;
    CHECK(net::json::parse(R"({"a":1,"b":[1,2,3,4,5],"c":{"d":1,"e":2,"f":3,"g":4}})", v, nullptr, lim));
    CHECK_EQ(v.size(), size_t(3));
    std::string err;
    CHECK(!net::json::parse(R"({"a":1,"b":2,"c":3,"d":4})", v, &err, lim));
    CHECK(err.find("too many members") != std::string::npos);
    std::string big = "[0";
    for (int i = 1; i < 100000; ++i) big += ",0";
    big += "]";
    CHECK(!net::json::parse(big, v, &err, lim));
    CHECK_EQ(err, std::string("too many items at byte 7"));
    lim.keepDepth = 2;
    CHECK(!net::json::parse(R"({"b":[1,2,3,4]})", v, nullptr, lim));
}

TEST(net_json_write) {
    Value o = Value::object();
    o.set("s", "q\"\\\n\x01\xC3\xA9");
    o.set("n", 42);
    o.set("f", 0.5);
    o.set("big", uint64_t(9007199254740991ULL));
    o.set("neg", -3);
    o.set("b", true);
    o.set("z", nullptr);
    Value& a = o.set("a", Value::array());
    a.push(1);
    a.push("x");
    o.set("n", 43);   // replaces
    std::string s = o.dump();
    CHECK_EQ(s, std::string("{\"s\":\"q\\\"\\\\\\n\\u0001\xC3\xA9\",\"n\":43,\"f\":0.5,\"big\":9007199254740991,\"neg\":-3,\"b\":true,\"z\":null,\"a\":[1,\"x\"]}"));
    Value back;
    CHECK(net::json::parse(s, back));
    CHECK(sameJson(back, o));
    CHECK_EQ(Value(std::nan("")).dump(), std::string("null"));
    CHECK_EQ(Value(1e300).dump(), std::string("1e+300"));
}

// =============================================================================================
// Crypto
// =============================================================================================

TEST(net_crypto_hashes) {
    using namespace net::crypto;
    CHECK_EQ(hex(sha256("")), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hex(sha256("abc")), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    std::string million(1000000, 'a');
    CHECK_EQ(hex(sha256(million)), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    Sha1 s1 = sha1("abc", 3);
    CHECK_EQ(hex(s1.data(), s1.size()), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    // RFC 6455 section 1.3 example
    std::string key = std::string("dGhlIHNhbXBsZSBub25jZQ==") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 acc = sha1(key.data(), key.size());
    CHECK_EQ(base64(acc.data(), acc.size()), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    std::fprintf(stderr, "  crypto backend: %s\n", backendName());
}

TEST(net_crypto_base64_hex) {
    using namespace net::crypto;
    const char* in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* b64[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    const char* b64u[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        CHECK_EQ(base64(in[i], std::strlen(in[i])), std::string(b64[i]));
        CHECK_EQ(base64url(in[i], std::strlen(in[i])), std::string(b64u[i]));
        std::vector<uint8_t> out;
        CHECK(base64Decode(b64[i], out) && std::string(out.begin(), out.end()) == in[i]);
        CHECK(base64urlDecode(b64u[i], out) && std::string(out.begin(), out.end()) == in[i]);
        CHECK(base64urlDecode(b64[i], out) && std::string(out.begin(), out.end()) == in[i]);   // padding accepted
    }
    const uint8_t bin[] = {0xFB, 0xFF, 0xBF};
    CHECK_EQ(base64(bin, 3), std::string("+/+/"));
    CHECK_EQ(base64url(bin, 3), std::string("-_-_"));
    std::vector<uint8_t> out;
    CHECK(!base64Decode("Zm9vYg", out));      // padding required
    CHECK(!base64Decode("Zh==", out));        // non-zero trailing bits
    CHECK(!base64urlDecode("Zm9v+", out));
    CHECK(!base64urlDecode("Z", out));
    CHECK(!base64Decode("Zm9v===", out));
    CHECK_EQ(hex("\x01\xAB", 2), std::string("01ab"));
    CHECK(hexDecode("01AbfF", out) && out.size() == 3 && out[1] == 0xAB && out[2] == 0xFF);
    CHECK(!hexDecode("abc", out));
    CHECK(!hexDecode("zz", out));
    CHECK(constantTimeEqual("abc", "abc"));
    CHECK(!constantTimeEqual("abc", "abd"));
    CHECK(!constantTimeEqual("abc", "ab"));
}

TEST(net_crypto_pkce_random) {
    using namespace net::crypto;
    // RFC 7636 appendix B
    CHECK_EQ(pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), std::string("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
    Pkce a, b;
    CHECK(makePkce(a) && makePkce(b));
    CHECK_EQ(a.verifier.size(), size_t(43));
    CHECK(a.verifier != b.verifier);
    CHECK_EQ(a.challenge, pkceChallenge(a.verifier));
    uint8_t r1[32] = {}, r2[32] = {};
    CHECK(randomBytes(r1, 32) && randomBytes(r2, 32));
    CHECK(std::memcmp(r1, r2, 32) != 0);
}

TEST(net_crypto_pow) {
    using namespace net::crypto;
    Sha256 d{};
    CHECK_EQ(leadingZeroBits(d), 256);
    d[0] = 0x01;
    CHECK_EQ(leadingZeroBits(d), 7);
    d[0] = 0;
    d[1] = 0x40;
    CHECK_EQ(leadingZeroBits(d), 9);
    // The solver counts from 0, so it finds the smallest nonce; these come from a brute force
    // with node:crypto over SHA-256(challenge + ":" + nonce).
    std::string nonce;
    CHECK(powSolve("scacelith-test-challenge", 12, nonce));
    CHECK_EQ(nonce, std::string("2388"));
    CHECK(powSolve("scacelith-test-challenge", 16, nonce));
    CHECK_EQ(nonce, std::string("103817"));
    CHECK(powCheck("scacelith-test-challenge", "103817", 16));
    CHECK(!powCheck("scacelith-test-challenge", "103816", 16));
    CHECK(!powCheck("scacelith-test-challenge", "103817x", 0));
    CHECK(!powCheck("scacelith-test-challenge", "", 0));
    PowStats st;
    CHECK(powSolve("c2", 18, nonce, nullptr, &st));
    CHECK_EQ(nonce, std::string("49917"));
    CHECK(powCheck("c2", nonce, 18));
    // Throughput and cancellation.
    std::atomic<bool> cancel{false};
    CHECK(!powSolve("bench", kPowMaxBits, nonce, &cancel, &st, 300000));
    CHECK_EQ(st.hashes, uint64_t(300000));
    std::fprintf(stderr, "  proof of work (%s): %.2f M hashes/s\n", backendName(), st.hashesPerSecond() / 1e6);
    cancel.store(true);
    CHECK(!powSolve("bench", 20, nonce, &cancel, &st));
    CHECK(st.hashes < 10000);
    CHECK(!powSolve("x", kPowMaxBits + 1, nonce));
    CHECK(!powSolve("x", -1, nonce));
}

// =============================================================================================
// Folders (net::sys; plat::exeDirectory, userDataDirectory and appDataDirectory return them)
// =============================================================================================

// The executable's folder is the absolute path of the folder that holds this program (a long exe
// path on Windows: net_sys_module_file_name_long_paths).
TEST(net_sys_exe_directory) {
    std::string d = net::sys::exeDirectory();
    REQUIRE(d.size() > 1);
#ifdef _WIN32
    CHECK((d[1] == ':' && d.size() >= 3) || d.compare(0, 2, "\\\\") == 0);
    CHECK_EQ(d.back(), '\\');
    CHECK(net::sys::fileExists(d + "scacelith_tests.exe"));
#else
    CHECK_EQ(d[0], '/');
    CHECK_EQ(d.back(), '/');
    CHECK(net::sys::fileExists(d + "scacelith_tests"));
#endif
}

#ifdef _WIN32
// An exe path of MAX_PATH characters or more (long paths enabled) is read whole into a larger
// buffer instead of giving ".\\" (the working directory, where the settings and the log would then
// go). Wine cannot start an exe from such a path: a fake GetModuleFileNameW cuts the path as
// Windows does (the buffer size returned, the copy cut and terminated).
TEST(net_sys_module_file_name_long_paths) {
    int calls = 0;
    std::wstring path;
    auto get = [&](wchar_t* buffer, unsigned long size) -> unsigned long {
        ++calls;
        if (path.empty()) return 0;
        size_t n = std::min<size_t>(path.size(), size - 1);
        std::copy(path.begin(), path.begin() + n, buffer);
        buffer[n] = L'\0';
        return path.size() < size ? (unsigned long)path.size() : size;
    };
    // A short path: one read.
    path = L"C:\\Games\\Scacelith\\Scacelith.exe";
    CHECK(net::sys::moduleFileName(get) == path);
    CHECK_EQ(calls, 1);
    // 300, 1000 and 32767 characters (the longest path): read again until the buffer holds it.
    for (size_t length : {size_t(300), size_t(1000), size_t(32767)}) {
        path = L"C:\\" + std::wstring(length - 17, L'a') + L"\\Scacelith.exe";
        REQUIRE(path.size() == length);
        calls = 0;
        CHECK(net::sys::moduleFileName(get) == path);
        CHECK(calls > 1);
    }
    // Exactly MAX_PATH characters fill the first buffer with no room for the terminator: cut.
    path = L"C:\\" + std::wstring(260 - 17, L'b') + L"\\Scacelith.exe";
    calls = 0;
    CHECK(net::sys::moduleFileName(get) == path);
    CHECK_EQ(calls, 2);
    // A failure, and a path no buffer holds (the reads stop past 32767 characters): "".
    path.clear();
    CHECK(net::sys::moduleFileName(get).empty());
    path = std::wstring(40000, L'c');
    calls = 0;
    CHECK(net::sys::moduleFileName(get).empty());
    CHECK(calls < 10);
}
#endif

#ifndef _WIN32
// The user data folder (the settings and log fallback, the default place of the logins) is
// created private, 0700, under $HOME/.config.
TEST(net_sys_user_data_directory_private) {
    const std::string home = net::sys::exeDirectory() + "net-test-home-" + std::to_string(getpid());
    REQUIRE(mkdir(home.c_str(), 0755) == 0 || errno == EEXIST);
    const char* was = std::getenv("HOME");
    const std::string saved = was ? was : "";
    setenv("HOME", home.c_str(), 1);
    std::string d = net::sys::userDataDirectory();
    if (was) setenv("HOME", saved.c_str(), 1);
    else unsetenv("HOME");
    CHECK_EQ(d, home + "/.config/scacelith/");
    struct stat st {};
    CHECK(stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    CHECK_EQ(int(st.st_mode & 0777), 0700);
    rmdir(d.c_str());
    rmdir((home + "/.config").c_str());
    rmdir(home.c_str());
}
#endif

// =============================================================================================
// Credential store
// =============================================================================================

namespace {
std::string tempCredentialPath(const char* tag) {
    std::string p = net::sys::exeDirectory() + "net-test-" + tag + ".credentials";
    net::sys::removeFile(p);
    return p;
}

// Removes the file when the test ends, also when a REQUIRE ends it early.
struct RemovedAtEnd {
    std::string path;
    ~RemovedAtEnd() { net::sys::removeFile(path); }
};
}  // namespace

TEST(net_credentials_isolation) {
    std::string path = tempCredentialPath("iso");
    const std::string A = "a.example.org:443", B = "b.example.org:443";
    const std::string tokenA = "sct_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    {
        net::CredentialStore s(path);
        CHECK(s.origins().empty());
        net::Credential c;
        c.origin = A;
        c.username = "alice";
        c.token = tokenA;
        c.serverId = "srv-a";
        c.pinnedSha256 = std::string(64, 'a');
        CHECK(s.put(c));
        net::Credential out;
        CHECK(!s.get(B, out));
        CHECK(!s.hasToken(B));
        CHECK(s.username(B).empty());
        CHECK(s.get(A, out));
        CHECK_EQ(out.token, tokenA);
        CHECK_EQ(out.pinnedSha256, std::string(64, 'a'));
        CHECK_EQ(s.pin(A), std::string(64, 'a'));
        CHECK(s.pin(B).empty());
    }
    std::string text;
    CHECK(net::sys::readFile(path, text, 1 << 20));
#ifdef _WIN32
    CHECK(text.find(tokenA) == std::string::npos);   // DPAPI: never in clear on disk
    CHECK(text.find("dpapi:") != std::string::npos);
#endif
    // A token blob copied into another origin's record never decrypts there.
    Value doc;
    CHECK(net::json::parse(text, doc));
    Value forged = Value::object();
    forged.set("origin", B);
    forged.set("username", "mallory");
    forged.set("token", doc["records"][0]["token"].asString());
    Value records = Value::array();
    records.push(doc["records"][0]);
    records.push(forged);
    doc.set("records", records);
    CHECK(net::sys::writeFileAtomic(path, doc.dump(), true));
    {
        net::CredentialStore s(path);
        net::Credential out;
        CHECK(s.get(B, out));
        CHECK(out.token.empty());
        CHECK_EQ(out.username, std::string("mallory"));
        CHECK(s.get(A, out));
        CHECK_EQ(out.token, tokenA);
        // Logout keeps the name, erase forgets the origin.
        CHECK(s.clearToken(A));
        CHECK(!s.hasToken(A));
        CHECK_EQ(s.username(A), std::string("alice"));
        CHECK(s.erase(A));
        CHECK(!s.get(A, out));
        CHECK_EQ(s.origins().size(), size_t(1));
    }
    std::string blob = net::protectToken(A, tokenA);
    std::string back;
    CHECK(!blob.empty());
    CHECK(net::unprotectToken(A, blob, back) && back == tokenA);
    CHECK(!net::unprotectToken(B, blob, back));
    CHECK(back.empty());
    CHECK(!net::unprotectToken(A, "garbage", back));
    net::sys::removeFile(path);
}

// A saved token that cannot be decrypted here (a portable install copied to another PC or Windows
// account) is no saved session, from the first look of the run: the game offers to sign in instead
// of resuming a session that fails at every opening. The file keeps it (it may be another account's).
TEST(net_credentials_undecryptable_token) {
    std::string path = tempCredentialPath("undecryptable");
    const std::string A = "a.example.org:443", B = "b.example.org:443";
    const std::string tokenA = "sct_" + std::string(43, 'A'), tokenB = "sct_" + std::string(43, 'B');
    const std::string blob = net::protectToken(A, tokenA);   // bound to A: never decrypts for B
    Value rec = Value::object();
    rec.set("origin", B);
    rec.set("username", "bob");
    rec.set("token", blob);
    Value good = Value::object();
    good.set("origin", A);
    good.set("username", "alice");
    good.set("token", blob);
    Value doc = Value::object();
    doc.set("version", 1);
    Value& records = doc.set("records", Value::array());
    records.push(rec);
    records.push(good);
    CHECK(net::sys::writeFileAtomic(path, doc.dump(), true));
    {
        net::CredentialStore s(path);
        CHECK(!s.hasToken(B));                      // before any get()
        CHECK(s.hasToken(A));
        net::Credential out;
        CHECK(s.get(B, out));
        CHECK(out.token.empty());
        CHECK_EQ(out.username, std::string("bob"));
        CHECK(!s.hasToken(B));                      // no saved session there from now on
        CHECK(s.get(B, out) && out.token.empty());
        CHECK(!s.hasToken(B));
        std::string text;
        CHECK(net::sys::readFile(path, text, 1 << 20));
        CHECK(text.find(blob) != std::string::npos);   // the file is left as it is
        // A new sign-in there replaces it.
        out.token = tokenB;
        CHECK(s.put(out));
        CHECK(s.hasToken(B));
        CHECK(s.get(B, out) && out.token == tokenB);
    }
    net::sys::removeFile(path);
}

// put() tells a record it could not keep at all (no origin, or a token the OS could not protect:
// a sign-in must not look successful then) from a file it could not write (the record holds for
// this run, the session works until the game quits).
TEST(net_credentials_put_reports_what_it_kept) {
    const std::string A = "a.example.org:443", token = "sct_" + std::string(43, 'K');
    net::CredentialStore s(net::sys::exeDirectory() + "net-test-no-such-folder/x.credentials");
    net::Credential c;
    c.origin = A;
    c.username = "alice";
    c.token = token;
    bool stored = false;
    CHECK(!s.put(c, &stored));                      // the file cannot be written...
    CHECK(stored);                                  // ...the record is kept all the same
    CHECK(s.hasToken(A));
    net::Credential out;
    CHECK(s.get(A, out) && out.token == token);
    c.origin.clear();
    CHECK(!s.put(c, &stored));
    CHECK(!stored);
}

// The pin saved at sign-in applies while the endpoint gives none; forgetSavedPin() (the pin field
// of Options emptied for that server) removes it, so that the system's certificates are trusted
// again, and keeps the session.
TEST(net_credentials_forget_saved_pin) {
    std::string path = tempCredentialPath("forget-pin");
    const std::string token = "sct_" + std::string(43, 'P'), pin(64, 'b');
    net::ServerEndpoint ep;
    ep.host = "chess.example.org";
    ep.apiPort = 8443;
    {
        net::CredentialStore s(path);
        net::Credential c;
        c.origin = ep.origin();
        c.username = "alice";
        c.token = token;
        c.serverId = "srv-1";
        c.pinnedSha256 = pin;
        CHECK(s.put(c));
        c.origin = "other.example.org:8443";       // another server's pin stays
        CHECK(s.put(c));
    }
    {
        net::OnlineClient c;
        c.setCredentialsFile(path);
        c.setServer(ep);
        CHECK(c.hasSavedSession());
        c.forgetSavedPin();   // on net-http
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!net::CredentialStore(path).pin(ep.origin()).empty() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(c.hasSavedSession());
    }
    net::CredentialStore s(path);
    net::Credential out;
    CHECK(s.get(ep.origin(), out));
    CHECK(out.pinnedSha256.empty());
    CHECK_EQ(out.token, token);
    CHECK_EQ(out.username, std::string("alice"));
    CHECK_EQ(out.serverId, std::string("srv-1"));
    CHECK_EQ(s.pin("other.example.org:8443"), pin);
    CHECK(s.clearPin("nowhere.example.org:443"));   // nothing saved there: nothing to do
    net::sys::removeFile(path);
}

// A token refused by the server is erased only while it is still the one saved: a GIF (on its own
// thread) may be refused while a new sign-in saves another token.
TEST(net_credentials_clear_that_token) {
    std::string path = tempCredentialPath("clear-that");
    const std::string A = "a.example.org:443";
    const std::string oldToken = "sct_" + std::string(43, 'O'), newToken = "sct_" + std::string(43, 'N');
    net::CredentialStore s(path);
    net::Credential c;
    c.origin = A;
    c.username = "alice";
    c.token = newToken;
    CHECK(s.put(c));
    CHECK(s.clearToken(A, oldToken));   // another one since: kept
    net::Credential out;
    CHECK(s.get(A, out));
    CHECK_EQ(out.token, newToken);
    CHECK(s.clearToken(A, newToken));   // that one: erased, the name kept
    CHECK(!s.hasToken(A));
    CHECK_EQ(s.username(A), std::string("alice"));
    CHECK(s.clearToken("b.example.org:443", oldToken));   // nothing saved there: nothing to do
    net::sys::removeFile(path);
}

// =============================================================================================
// Endpoints and transport rules
// =============================================================================================

TEST(net_endpoint_validation) {
    net::ServerEndpoint e;
    e.host = "Play.Example.ORG";
    e.apiPort = 8443;
    CHECK(e.valid());
    CHECK_EQ(e.origin(), std::string("play.example.org:8443"));
    e.insecureDev = true;
    CHECK(!e.valid());                              // plain HTTP only for loopback
    for (const char* h : {"localhost", "LOCALHOST", "127.0.0.1", "::1"}) {
        e.host = h;
        CHECK(e.valid());
    }
    e.host = "127.0.0.2";
    CHECK(!e.valid());
    e.host = "::1";
    CHECK_EQ(e.origin(), std::string("[::1]:8443"));
    e.insecureDev = false;
    for (const char* h : {"", "exa mple.com", "https://x.org", "a/b", "-a.com", "a-.com", "a..b", "x.org:443", "user@x.org", "a.b.", "x_y.org"}) {
        e.host = h;
        if (e.valid()) std::fprintf(stderr, "  accepted host '%s'\n", h);
        CHECK(!e.valid());
    }
    for (const char* h : {"x.org", "10.0.0.1", "2001:db8::1", "fe80::1:2", "::ffff:192.0.2.1", "a-b.c-d.example"}) {
        e.host = h;
        CHECK(e.valid());
    }
    e.host = "x.org";
    e.apiPort = 0;
    CHECK(!e.valid());
    e.apiPort = 443;
    e.pinnedSha256 = std::string(64, 'F');
    CHECK(e.valid());
    e.pinnedSha256 = "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";
    CHECK(e.valid());
    e.pinnedSha256 = std::string(63, 'a');
    CHECK(!e.valid());
    e.pinnedSha256 = std::string(64, 'g');
    CHECK(!e.valid());
    CHECK(net::isIpLiteral("192.168.0.1"));
    CHECK(!net::isIpLiteral("256.1.1.1"));
    CHECK(!net::isIpLiteral("1.2.3"));
    CHECK(net::isIpLiteral("::"));
    CHECK(!net::isIpLiteral("1:2:3:4:5:6:7:8:9"));
    CHECK(!net::isIpLiteral("example.com"));
    CHECK_EQ(net::hostHeader("x.org", 443, true), std::string("x.org"));
    CHECK_EQ(net::hostHeader("x.org", 80, true), std::string("x.org:80"));
    CHECK_EQ(net::hostHeader("::1", 8080, false), std::string("[::1]:8080"));
    // wsPort left empty = the API's port (HTTPS API and /ws share it).
    net::ServerEndpoint custom;
    custom.host = "chess.example.org";
    custom.apiPort = 8443;
    CHECK_EQ(custom.wsPort, uint16_t(0));
    CHECK_EQ(custom.effectiveWsPort(), uint16_t(8443));
    CHECK(!custom.insecureDev);
    custom.wsPort = 9443;
    CHECK_EQ(custom.effectiveWsPort(), uint16_t(9443));
    // The official server: https://caissa.scacelith.com/api/v1 and wss://caissa.scacelith.com/ws,
    // one port, 443.
    net::ServerEndpoint off = net::officialServer();
#ifndef SCACELITH_OFFICIAL_SERVER
#define SCACELITH_OFFICIAL_SERVER ""
#endif
    if (std::string(SCACELITH_OFFICIAL_SERVER).empty()) {
        CHECK_EQ(off.host, std::string("caissa.scacelith.com"));
        CHECK_EQ(off.apiPort, uint16_t(443));
        CHECK_EQ(off.wsPort, uint16_t(443));
        CHECK(off.pinnedSha256.empty());
        CHECK(!off.insecureDev);
        CHECK(off.valid());
        CHECK_EQ(off.origin(), std::string("caissa.scacelith.com:443"));
    } else {
        CHECK(off.host.empty() || (off.valid() && off.wsPort != 0));   // configured build
    }
}

TEST(net_transport_refuses_insecure) {
    net::HttpRequest r;
    r.host = "example.org";
    r.port = 80;
    r.tls = false;
    net::HttpResponse resp;
    net::httpRequest(r, resp);
    const std::string expect = net::transportAvailable() ? "insecure" : "unavailable";
    CHECK_EQ(resp.error, expect);
    CHECK_EQ(resp.status, 0);
    net::WsParams p;
    p.host = "10.1.2.3";
    p.tls = false;
    std::string err;
    int status = 0;
    CHECK(net::wsConnect(p, err, status) == nullptr);
    CHECK_EQ(err, expect);
    // A cancelled token aborts before anything happens.
    net::CancelToken tok;
    tok.cancel();
    bool ran = false;
    tok.setAbort([&] { ran = true; });
    CHECK(ran);
}

// A server that accepts the request and says nothing: the request and the WebSocket upgrade end
// at their timeout (Wine's WinHTTP waits for the response headers with a timeout of its own).
TEST(net_transport_silent_server_times_out) {
    if (!net::transportAvailable()) return;
    fakehttp::Server srv(
        [](const fakehttp::Request&) {
            fakehttp::Reply rep;
            rep.silenceMs = 6000;
            return rep;
        },
        true);
    CHECK(srv.ok());
    net::HttpRequest req;
    req.host = "127.0.0.1";
    req.port = srv.port();
    req.tls = false;
    req.path = "/api/v1/info";
    req.timeoutMs = 1000;
    net::HttpResponse resp;
    auto t0 = std::chrono::steady_clock::now();
    net::httpRequest(req, resp);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  request: %s after %.0f ms\n", resp.error.c_str(), ms);
    CHECK_EQ(resp.error, std::string("timeout"));
    CHECK(ms < 4000);
    net::WsParams p;
    p.host = "127.0.0.1";
    p.port = srv.port();
    p.tls = false;
    p.subprotocol = "scacelith.v1";
    p.timeoutMs = 1000;
    std::string error;
    int status = 0;
    t0 = std::chrono::steady_clock::now();
    std::unique_ptr<net::WebSocket> ws = net::wsConnect(p, error, status, nullptr);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  upgrade: %s after %.0f ms\n", error.c_str(), ms);
    CHECK(!ws);
    CHECK(ms < 4000);
}

// Chunked answers to httpRequest: chunk sizes that end the reads anywhere (in a size line, in the
// data, between CR and LF), a large body in linear time, malformed and truncated codings (the
// OpenSSL transport's own decoder; WinHTTP decodes the coding itself).
TEST(net_transport_chunked_answers) {
    if (!net::transportAvailable()) return;
    std::string body, coded;
    for (size_t i = 0, k = 1; body.size() < (size_t(1) << 20); ++i, k = k % 997 + 1) {
        std::string part(k, char('a' + i % 26));
        char size[16];
        std::snprintf(size, sizeof size, "%zx\r\n", k);
        coded += size + part + "\r\n";
        body += part;
    }
    coded += "0\r\n\r\n";
    std::string large(size_t(16) << 20, '\0');
    for (size_t i = 0; i < large.size(); ++i) large[i] = char('A' + (i * 7) % 61);
    fakehttp::Server srv([&](const fakehttp::Request& q) {
        fakehttp::Reply rep;
        if (q.path == "/large") {
            rep.chunked = true;   // chunks of 16 KiB
            rep.body = large;
            return rep;
        }
        rep.noLength = true;
        rep.headers.emplace_back("Transfer-Encoding", "chunked");
        if (q.path == "/pieces") {
            rep.body = coded;
            rep.pieceDelayMs = 10;   // 16 KiB pieces, a read each
        } else if (q.path == "/bad-line-end") {
            rep.body = "5\r\nhelloXX0\r\n\r\n";
        } else if (q.path == "/bad-size") {
            rep.body = "000000005\r\nhello\r\n0\r\n\r\n";
        } else if (q.path == "/bare-lf") {
            rep.body = "5\nhello\r\n0\r\n\r\n";
        } else {
            rep.body = coded.substr(0, coded.size() / 2);   // the connection ends there
        }
        return rep;
    });
    CHECK(srv.ok());
    auto get = [&](const char* path, net::HttpResponse& resp) {
        net::HttpRequest req;
        req.host = "127.0.0.1";
        req.port = srv.port();
        req.tls = false;
        req.path = path;
        req.timeoutMs = 60000;
        req.maxResponseBytes = size_t(32) << 20;
        auto t0 = std::chrono::steady_clock::now();
        net::httpRequest(req, resp);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    net::HttpResponse resp;
    get("/pieces", resp);
    CHECK(resp.error.empty());
    CHECK(resp.body == body);
    double ms = get("/large", resp);
    std::fprintf(stderr, "  16 MiB chunked: %.0f ms\n", ms);
    CHECK(resp.error.empty());
    CHECK(resp.body == large);
    CHECK(ms < 5000);
#ifndef _WIN32
    for (const char* bad : {"/bad-line-end", "/bad-size", "/bare-lf", "/truncated"}) {
        get(bad, resp);
        CHECK_EQ(resp.error, std::string("network"));
        CHECK(resp.body.empty());
    }
#endif
}

// An operation that ends while another thread runs its abort action waits for that action: what
// the action uses (the operation's socket or handle) is still there.
TEST(net_transport_cancel_waits_for_a_running_abort) {
    struct Target { int closed = 0; };
    auto* target = new Target;
    std::atomic<bool> inside{false};
    net::CancelToken tok;
    tok.setAbort([&] {
        inside.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        target->closed = 1;
        inside.store(false);
    });
    std::thread canceller([&] { tok.cancel(); });
    while (!inside.load()) std::this_thread::yield();
    tok.setAbort(nullptr);   // the operation ends
    CHECK(!inside.load());
    CHECK_EQ(target->closed, 1);
    canceller.join();
    delete target;
    CHECK(tok.cancelled());
    CHECK(!tok.hasAbort());
}

// A request that runs out of memory (a large answer while the system has none left) throws
// std::bad_alloc out of the transport. The abort action it gave its CancelToken (closing its
// socket or handle, locals of the call) is taken back all the same, so that a later cancel() (the
// client's shutdown) never reaches a socket or handle that is gone; the token serves again.
TEST(net_transport_cancel_cleared_when_out_of_memory) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    if (!allocfail::available()) SKIP("AddressSanitizer build: no simulated out of memory");
    allocfail::Reset reset;
    const std::string big(size_t(12) << 20, 'x');
    fakehttp::Server srv([&](const fakehttp::Request& q) {
        allocfail::spareThisThread();   // the server has the memory it needs
        fakehttp::Reply rep;
        rep.headers.emplace_back("Content-Type", "application/octet-stream");
        rep.body = q.path == "/big" ? big : std::string("small");
        return rep;
    });
    CHECK(srv.ok());
    net::HttpRequest req;
    req.host = "127.0.0.1";
    req.port = srv.port();
    req.tls = false;
    req.path = "/big";
    req.maxResponseBytes = size_t(16) << 20;
    net::CancelToken tok;
    net::HttpResponse resp;
    bool threw = false;
    allocfail::failFrom(size_t(8) << 20);
    try {
        net::httpRequest(req, resp, &tok);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    allocfail::failFrom(0);
    CHECK(threw);
    CHECK(!tok.hasAbort());
    // A streamed answer whose reader throws: the same.
    threw = false;
    try {
        net::httpStream(
            req, [](const net::HttpHead&) { return true; }, [](const char*, size_t) -> bool { throw std::bad_alloc(); }, resp, &tok);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!tok.hasAbort());
    req.path = "/small";
    net::httpRequest(req, resp, &tok);
    CHECK(resp.error.empty());
    CHECK_EQ(resp.body, std::string("small"));
    CHECK(!tok.hasAbort());
}

// =============================================================================================
// OnlineClient against a loopback fake server
// =============================================================================================

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kBadSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
void shutSock(Sock s) { shutdown(s, SD_BOTH); }
struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
#else
using Sock = int;
const Sock kBadSock = -1;
void closeSock(Sock s) { ::close(s); }
void shutSock(Sock s) { ::shutdown(s, SHUT_RDWR); }
#endif

bool sendAll(Sock s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n) {
#ifdef _WIN32
        int r = ::send(s, p, int(n), 0);
#else
        ssize_t r = ::send(s, p, n, MSG_NOSIGNAL);
#endif
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

double epochMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Plays the dedicated server on 127.0.0.1: the HTTP API subset the client uses, and /ws.
class FakeServer {
public:
    uint16_t port = 0;
    const std::string token = "sct_" + std::string(43, 'T');
    const std::string token2 = "sct_" + std::string(43, 'U');
    const std::string powChallenge = "fake-challenge-0123456789abcdef";
    static constexpr double kSkewMs = 5000;         // the server clock runs 5 s ahead
    std::atomic<int> loginAttempts{0}, powAccepted{0}, hellos{0}, pings{0}, moves{0}, logouts{0};
    std::atomic<int> deletes{0};                    // POST /api/v1/account/delete accepted
    std::atomic<int> infos{0}, upgrades{0};         // GET /api/v1/info, WebSocket upgrade requests
    std::atomic<int> upgradeStatus{0};              // != 0: upgrades are refused with this HTTP status
    std::atomic<uint32_t> clientPingMs{0};          // Welcome.clientPingMs
    std::atomic<uint32_t> heartbeatMs{15000};       // Welcome.heartbeatMs (no S_Ping follows the first one)
    std::atomic<bool> answerPings{true};            // false: C_Ping gets no S_Pong
    std::atomic<int> serverNo{1};                   // serverId "srv-<n>" in /info and in the 101 answer
    std::atomic<bool> helloTokenOk{false};
    std::atomic<uint64_t> activeGame{0};
    std::atomic<uint16_t> gestureRate{0}, gestureBurst{0};   // Welcome.gestureRate / gestureBurst
    std::atomic<bool> autoPress{true};                        // GameSnapshot.autoPress
    std::atomic<bool> seqOk{true};                            // every client message came numbered in order
    std::atomic<bool> loginToken2{false};                     // sign-ins answer token2 (another session)

    // The C_Gesture frames received, with their arrival time.
    struct GestureIn {
        pr::C_Gesture m;
        std::chrono::steady_clock::time_point at;
    };
    std::vector<GestureIn> gestures() {
        std::lock_guard<std::mutex> lk(gestureMu_);
        return gestures_;
    }

    // What the next upgrades get, in order, before upgradeStatus applies again: an HTTP status
    // that refuses the upgrade, kUpgradeOk, or kShutdownAtHello (101, then Error{ShuttingDown} +
    // close 4008 in answer to Hello, like a draining server).
    static constexpr int kUpgradeOk = 0, kShutdownAtHello = -1;
    void scriptUpgrades(std::initializer_list<int> steps) {
        std::lock_guard<std::mutex> lk(scriptMu_);
        script_.assign(steps.begin(), steps.end());
    }
    std::string serverId() const { return "srv-" + std::to_string(serverNo.load()); }

    bool start() {
#ifdef _WIN32
        static WsaInit wsa;
#endif
        ls_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (ls_ == kBadSock) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(ls_, 16) != 0) return false;
        socklen_t len = sizeof(a);
        getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        acceptor_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    ~FakeServer() {
        stop_.store(true);
        shutSock(ls_);
        closeSock(ls_);
        if (acceptor_.joinable()) acceptor_.join();
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (Sock s : open_) shutSock(s);
        }
        for (auto& t : workers_) t.join();
    }

    // Drops every WebSocket without a close frame (network failure).
    void dropWebSockets() {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sock s : ws_) shutSock(s);
    }

    // Sends a close frame with this code on every WebSocket (after an optional Notice).
    void kick(uint16_t code, bool banNotice = false) {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sock s : ws_) {
            if (banNotice) {
                pr::Notice n;
                n.code = pr::NoticeCode::Banned;
                n.arg = 4102444800000.0;
                sendMsg(s, n);
            }
            uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code)};
            sendFrame(s, 0x8, payload, 2);
        }
    }

    // Notice{ServerShutdown} on every WebSocket (a restart is coming).
    void noticeShutdown() {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sock s : ws_) {
            pr::Notice n;
            n.code = pr::NoticeCode::ServerShutdown;
            n.arg = 3000;
            sendMsg(s, n);
        }
    }

    // Notice{SessionRevoked} on every WebSocket (the session of that connection was revoked).
    void noticeRevoked() {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sock s : ws_) {
            pr::Notice n;
            n.code = pr::NoticeCode::SessionRevoked;
            sendMsg(s, n);
        }
    }

    // What the server does to the connections of the sessions it revokes (all of an account deleted):
    // Notice{SessionRevoked}, a fatal Error{Unauthorized} and close 4003, on every WebSocket.
    void revokeSessions() {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sock s : ws_) {
            pr::Notice n;
            n.code = pr::NoticeCode::SessionRevoked;
            sendMsg(s, n);
            pr::Error e;
            e.code = pr::ErrorCode::Unauthorized;
            e.fatal = true;
            sendMsg(s, e);
            uint8_t payload[2] = {uint8_t(pr::CloseCode::Unauthorized >> 8), uint8_t(pr::CloseCode::Unauthorized & 0xFF)};
            sendFrame(s, 0x8, payload, 2);
        }
    }

    // The opponent's gesture (S_Gesture) on every WebSocket.
    void sendGesture(const pr::S_Gesture& g) {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sock s : ws_) sendMsg(s, g);
    }

    // Frames the client ignores on every WebSocket, count of each kind: a client message type,
    // and a Welcome without its fields.
    void sendBadFrames(int count) {
        std::lock_guard<std::mutex> lk(mu_);
        const uint8_t clientType[1] = {0x01}, shortWelcome[1] = {uint8_t(pr::MsgType::Welcome)};
        for (Sock s : ws_)
            for (int i = 0; i < count; ++i) {
                sendFrame(s, 0x2, clientType, 1);
                sendFrame(s, 0x2, shortWelcome, 1);
            }
    }

private:
    Sock ls_ = kBadSock;
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::vector<Sock> open_, ws_;
    std::atomic<bool> stop_{false};
    std::mutex gameMu_;
    std::mutex gestureMu_;
    std::vector<GestureIn> gestures_;
    std::mutex scriptMu_;
    std::deque<int> script_;
    std::atomic<uint32_t> gseq_{1};
    std::vector<pr::MoveRec> gameMoves_;
    std::string gameCategory_;
    bool gameRated_ = false;

    void acceptLoop() {
        for (;;) {
            Sock s = ::accept(ls_, nullptr, nullptr);
            if (s == kBadSock || stop_.load()) {
                if (s != kBadSock) closeSock(s);
                return;
            }
            std::lock_guard<std::mutex> lk(mu_);
            open_.push_back(s);
            workers_.emplace_back([this, s] {
                serve(s);
                std::lock_guard<std::mutex> lk2(mu_);
                open_.erase(std::find(open_.begin(), open_.end(), s));
                auto w = std::find(ws_.begin(), ws_.end(), s);
                if (w != ws_.end()) ws_.erase(w);
                closeSock(s);
            });
        }
    }

    // Frames go out whole: the test thread (kick, sendGesture...) and the connection's worker may
    // send on the same socket at once.
    static void sendFrame(Sock s, int opcode, const uint8_t* data, size_t n) {
        static std::mutex sendMu;
        std::string f;
        f += char(0x80 | opcode);
        if (n < 126) {
            f += char(n);
        } else {
            f += char(126);
            f += char(n >> 8);
            f += char(n);
        }
        f.append(reinterpret_cast<const char*>(data), n);
        std::lock_guard<std::mutex> lk(sendMu);
        sendAll(s, f.data(), f.size());
    }

    template <class M> static void sendMsg(Sock s, const M& m) {
        std::vector<uint8_t> b;
        pr::encode(m, b);
        sendFrame(s, 0x2, b.data(), b.size());
    }

    void respond(Sock s, int status, const std::string& body) {
        std::string r = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        sendAll(s, r.data(), r.size());
    }

    void serve(Sock s) {
        std::string buf;
        char tmp[8192];
        size_t end;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            int r = int(::recv(s, tmp, sizeof(tmp), 0));
            if (r <= 0) return;
            buf.append(tmp, size_t(r));
        }
        std::string head = buf.substr(0, end);
        std::string rest = buf.substr(end + 4);
        std::string line = head.substr(0, head.find("\r\n"));
        std::string method = line.substr(0, line.find(' '));
        std::string path = line.substr(line.find(' ') + 1);
        path = path.substr(0, path.find(' '));
        std::map<std::string, std::string> h;
        size_t pos = head.find("\r\n");
        while (pos != std::string::npos && pos + 2 < head.size()) {
            size_t e = head.find("\r\n", pos + 2);
            std::string l = head.substr(pos + 2, e == std::string::npos ? std::string::npos : e - pos - 2);
            size_t c = l.find(':');
            if (c != std::string::npos) {
                std::string v = l.substr(c + 1);
                v.erase(0, v.find_first_not_of(' '));
                h[lowerStr(l.substr(0, c))] = v;
            }
            pos = e;
        }
        if (path == "/ws" && lowerStr(h["upgrade"]) == "websocket") return websocket(s, h, rest);
        size_t want = size_t(std::atoi(h["content-length"].c_str()));
        while (rest.size() < want) {
            int r = int(::recv(s, tmp, sizeof(tmp), 0));
            if (r <= 0) return;
            rest.append(tmp, size_t(r));
        }
        Value body;
        net::json::parse(rest, body);
        std::string auth = h["authorization"];
        bool authed = auth == "Bearer " + token;
        if (method == "GET" && path == "/api/v1/info") {
            ++infos;
            char info[512];
            std::snprintf(info, sizeof(info),
                          "{\"name\":\"Fake\",\"serverId\":\"%s\",\"motd\":\"hi\",\"protocol\":{\"min\":%u,\"max\":%u,\"schema\":%u,"
                          "\"subprotocol\":\"scacelith.v1\"},\"wsPort\":%u,\"wsPath\":\"/ws\",\"registration\":\"open\","
                          "\"emailVerification\":true,\"sso\":{\"google\":false},\"mfa\":true,\"pow\":{\"register\":10},"
                          "\"categories\":[{\"id\":\"3+2\",\"baseSec\":180,\"incSec\":2}]}",
                          serverId().c_str(), unsigned(pr::kProtocolMin), unsigned(pr::kProtocolVersion), unsigned(pr::kSchemaHash),
                          unsigned(port));
            respond(s, 200, info);
        } else if (method == "POST" && path == "/api/v1/auth/login") {
            ++loginAttempts;
            if (body["login"].asString() != "alice" || body["password"].asString() != "pw") {
                respond(s, 401, "{\"error\":\"invalid_credentials\",\"message\":\"no\"}");
            } else if (!body["pow"].isObject()) {
                respond(s, 428, "{\"error\":\"pow_required\",\"message\":\"pow\",\"pow\":{\"challenge\":\"" + powChallenge +
                                    "\",\"bits\":10,\"expiresAt\":1}}");
            } else if (body["pow"]["challenge"].asString() == powChallenge &&
                       net::crypto::powCheck(powChallenge, body["pow"]["nonce"].asString(), 10)) {
                ++powAccepted;
                respond(s, 200, "{\"token\":\"" + (loginToken2.load() ? token2 : token) + "\",\"expiresAt\":1,\"user\":{\"id\":7,\"username\":\"alice\","
                                    "\"email\":\"a@example.org\",\"emailVerified\":true,\"mfaEnabled\":false,\"googleLinked\":false}}");
            } else {
                respond(s, 428, "{\"error\":\"pow_required\",\"pow\":{\"challenge\":\"x\",\"bits\":40}}");
            }
        } else if (method == "GET" && path == "/api/v1/account/me") {
            if (!authed) return respond(s, 401, "{\"error\":\"unauthorized\"}");
            respond(s, 200, "{\"user\":{\"id\":7,\"username\":\"alice\",\"email\":\"a@example.org\",\"emailVerified\":true,"
                            "\"mfaEnabled\":true,\"googleLinked\":false},\"ratings\":[{\"category\":\"3+2\",\"rating\":1520,"
                            "\"games\":3,\"wins\":2,\"draws\":0,\"losses\":1,\"peak\":1530,\"provisional\":true}],"
                            "\"sanctions\":[],\"ban\":null}");
        } else if (method == "POST" && path == "/api/v1/auth/logout") {
            ++logouts;
            respond(s, authed ? 200 : 401, authed ? "{\"status\":\"logged_out\"}" : "{\"error\":\"unauthorized\"}");
        } else if (method == "POST" && path == "/api/v1/account/delete") {
            if (!authed) return respond(s, 401, "{\"error\":\"unauthorized\"}");
            if (body["password"].asString() != "pw") return respond(s, 403, "{\"error\":\"invalid_password\"}");
            ++deletes;
            // As the server, the account's connections are closed once it is gone (after its
            // database work: 100 ms here) and before the answer, which comes later still: their
            // frames reach the client first.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            revokeSessions();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            respond(s, 200, "{\"status\":\"deleted\"}");
        } else {
            respond(s, 404, "{\"error\":\"not_found\"}");
        }
    }

    void websocket(Sock s, std::map<std::string, std::string>& h, std::string in) {
        ++upgrades;
        int step = upgradeStatus.load();
        {
            std::lock_guard<std::mutex> lk(scriptMu_);
            if (!script_.empty()) {
                step = script_.front();
                script_.pop_front();
            }
        }
        if (step > 0) {
            respond(s, step, "{\"error\":\"server_full\"}");
            return;
        }
        std::string key = h["sec-websocket-key"];
        std::string proto = h["sec-websocket-protocol"];
        if (proto.find(pr::kWsSubprotocol) == std::string::npos || !h.count("sec-websocket-version")) {
            respond(s, 400, "{}");
            return;
        }
        std::string k = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        net::crypto::Sha1 d = net::crypto::sha1(k.data(), k.size());
        std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                        net::crypto::base64(d.data(), d.size()) + "\r\nSec-WebSocket-Protocol: " + pr::kWsSubprotocol +
                        "\r\nScacelith-Server-Id: " + serverId() + "\r\n\r\n";
        sendAll(s, r.data(), r.size());
        {
            std::lock_guard<std::mutex> lk(mu_);
            ws_.push_back(s);
        }
        std::atomic<uint32_t>& gseq = gseq_;   // game sequence, kept across connections
        uint32_t lastSeq = 0;                   // the client numbers its messages from Hello = 1
        char tmp[8192];
        for (;;) {
            // One frame (client frames are masked).
            while (in.size() < 2 || in.size() < frameSize(in)) {
                int n = int(::recv(s, tmp, sizeof(tmp), 0));
                if (n <= 0) return;
                in.append(tmp, size_t(n));
            }
            size_t total = frameSize(in);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(in.data());
            int op = b[0] & 0x0F;
            size_t len = b[1] & 0x7F, hdr = 2;
            if (len == 126) { len = size_t(b[2]) << 8 | b[3]; hdr = 4; }
            if (!(b[1] & 0x80)) return;   // unmasked client frame: protocol error
            const uint8_t* mask = b + hdr;
            std::vector<uint8_t> payload(len);
            for (size_t i = 0; i < len; ++i) payload[i] = b[hdr + 4 + i] ^ mask[i & 3];
            in.erase(0, total);
            if (op == 0x8) {
                sendFrame(s, 0x8, payload.data(), std::min<size_t>(payload.size(), 2));
                return;
            }
            if (op == 0x9) { sendFrame(s, 0xA, payload.data(), payload.size()); continue; }
            if (op != 0x2) continue;
            pr::MsgType t;
            if (!pr::peekType(payload.data(), payload.size(), t)) return;
            const uint8_t* p = payload.data();
            size_t n = payload.size();
            if (n >= 5) {
                uint32_t seq = uint32_t(p[1]) | uint32_t(p[2]) << 8 | uint32_t(p[3]) << 16 | uint32_t(p[4]) << 24;
                if (seq != lastSeq + 1) seqOk.store(false);
                lastSeq = seq;
            }
            if (t == pr::MsgType::Hello) {
                pr::Hello m;
                if (!pr::decode(p, n, m)) return;
                ++hellos;
                helloTokenOk.store(m.seq == 1 && m.token == token && m.schema == pr::kSchemaHash && m.proto == pr::kProtocolVersion &&
                                   m.client.compare(0, 10, "Scacelith/") == 0);
                if (step == kShutdownAtHello) {
                    pr::Error e;
                    e.code = pr::ErrorCode::ShuttingDown;
                    e.fatal = true;
                    sendMsg(s, e);
                    uint8_t payload[2] = {uint8_t(pr::CloseCode::ShuttingDown >> 8), uint8_t(pr::CloseCode::ShuttingDown & 0xFF)};
                    sendFrame(s, 0x8, payload, 2);
                    continue;
                }
                pr::Welcome w;
                w.proto = pr::kProtocolVersion;
                w.serverTime = epochMs() + kSkewMs;
                w.userId = 7;
                w.username = "alice";
                w.serverName = "Fake";
                w.heartbeatMs = heartbeatMs.load();
                w.clientPingMs = clientPingMs.load();
                w.maxMsgPerSec = 20;
                w.activeGame = activeGame.load();
                w.gestureRate = gestureRate.load();
                w.gestureBurst = gestureBurst.load();
                sendMsg(s, w);
                if (w.activeGame) sendSnapshot(s, gseq);
                pr::S_Ping sp;
                sp.nonce = 99;
                sp.serverTime = epochMs() + kSkewMs;
                sendMsg(s, sp);
            } else if (t == pr::MsgType::C_Ping) {
                pr::C_Ping m;
                if (!pr::decode(p, n, m)) return;
                ++pings;
                if (!answerPings.load()) continue;
                pr::S_Pong r2;
                r2.nonce = m.nonce;
                r2.serverTime = epochMs() + kSkewMs;
                sendMsg(s, r2);
            } else if (t == pr::MsgType::QueueJoin) {
                pr::QueueJoin m;
                if (!pr::decode(p, n, m)) return;
                pr::QueueStatus q;
                q.category = m.category;
                q.rated = m.rated;
                q.state = pr::QueueState::Matched;
                q.queued = 2;
                sendMsg(s, q);
                {
                    std::lock_guard<std::mutex> lk(gameMu_);
                    gameMoves_.clear();
                    gameCategory_ = m.category;
                    gameRated_ = m.rated;
                }
                activeGame.store(77);
                sendSnapshot(s, gseq);
            } else if (t == pr::MsgType::Move) {
                pr::Move m;
                if (!pr::decode(p, n, m)) return;
                ++moves;
                if (m.posHash != 923150620u || m.ply != 0) {
                    pr::MoveRejected rj;
                    rj.game = m.game;
                    rj.ply = m.ply;
                    rj.move = m.move;
                    rj.code = pr::ErrorCode::Desync;
                    sendMsg(s, rj);
                    continue;
                }
                pr::MoveMade mm;
                mm.game = m.game;
                mm.gseq = ++gseq;
                mm.ply = 0;
                mm.move = m.move;
                mm.flags = pr::MoveFlag::DoublePush;
                mm.whiteMs = mm.blackMs = 180000;
                mm.serverTime = epochMs() + kSkewMs;
                mm.firstMoveMs = 30000;
                sendMsg(s, mm);
                mm.gseq = ++gseq;                       // the opponent answers e7e5
                mm.ply = 1;
                mm.move = net::packMove(52, 36, 0);
                mm.firstMoveMs = 0;
                sendMsg(s, mm);
                {
                    std::lock_guard<std::mutex> lk(gameMu_);
                    gameMoves_.push_back({m.move, 1500, 180000});
                    gameMoves_.push_back({mm.move, 900, 180000});
                }
                pr::GameEvent ge;
                ge.game = m.game;
                ge.gseq = ++gseq;
                ge.kind = pr::GameEventKind::DrawOffered;
                ge.color = pr::Color::Black;
                sendMsg(s, ge);
            } else if (t == pr::MsgType::C_Gesture) {
                pr::C_Gesture m;
                if (!pr::decode(p, n, m)) return;
                std::lock_guard<std::mutex> lk(gestureMu_);
                gestures_.push_back({m, std::chrono::steady_clock::now()});
            } else if (t == pr::MsgType::Resign) {
                pr::Resign m;
                if (!pr::decode(p, n, m)) return;
                pr::GameEnd e;
                e.game = m.game;
                e.gseq = ++gseq;
                e.status = pr::GameStatus::BlackWins;
                e.reason = pr::EndReason::Resignation;
                e.whiteMs = 170000;
                e.blackMs = 175000;
                e.serverTime = epochMs() + kSkewMs;
                sendMsg(s, e);
                activeGame.store(0);
            }
        }
    }

    void sendSnapshot(Sock s, uint32_t gseq) {
        pr::GameSnapshot g;
        g.game = 77;
        g.gseq = gseq;
        {
            std::lock_guard<std::mutex> lk(gameMu_);
            g.category = gameCategory_;
            g.rated = gameRated_;
            g.moves = gameMoves_;
        }
        g.baseMs = 180000;
        g.incMs = 2000;
        g.white = {7, "alice", 1520, true};
        g.black = {8, "bob", 1600, false};
        g.you = pr::Color::White;
        g.running = g.moves.size() >= 2 ? pr::Color::White : pr::Color::None;
        g.whiteMs = g.blackMs = 180000;
        g.serverTime = epochMs() + kSkewMs;
        g.drawOffer = pr::Color::None;
        g.rematch = pr::Color::None;
        g.whiteConnected = g.blackConnected = true;
        g.firstMoveMs = g.moves.empty() ? 30000 : 0;
        g.autoPress = autoPress.load();
        sendMsg(s, g);
    }

    static size_t frameSize(const std::string& in) {
        if (in.size() < 2) return 2;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(in.data());
        size_t len = b[1] & 0x7F, hdr = 2;
        if (len == 126) {
            if (in.size() < 4) return 4;
            len = size_t(b[2]) << 8 | b[3];
            hdr = 4;
        }
        return hdr + ((b[1] & 0x80) ? 4 : 0) + len;
    }
};

// Polls until an event of this kind arrives (other events are kept in 'seen').
bool waitEvent(net::OnlineClient& c, net::Event::Kind kind, net::Event& out, int timeoutMs, std::vector<net::Event>* seen = nullptr,
               std::function<bool(const net::Event&)> pred = nullptr) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        net::Event ev;
        while (c.poll(ev)) {
            bool match = ev.kind == kind && (!pred || pred(ev));
            if (seen) seen->push_back(ev);
            if (match) {
                out = ev;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool waitState(net::OnlineClient& c, net::ConnState s, int timeoutMs, std::vector<net::Event>* seen = nullptr) {
    net::Event ev;
    return waitEvent(c, net::Event::Kind::ConnectionChanged, ev, timeoutMs, seen, [s](const net::Event& e) { return e.state == s; });
}

}  // namespace

TEST(net_online_client_loopback) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    FakeServer srv;
    CHECK(srv.start());
    std::string credPath = tempCredentialPath("client");
    RemovedAtEnd removeCredentials{credPath};
    using K = net::Event::Kind;
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        net::ServerEndpoint ep;
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port;
        ep.wsPort = 0;               // left empty: the WebSocket shares the API port
        ep.insecureDev = true;
        c.setServer(ep);
        CHECK_EQ(c.server().origin(), "127.0.0.1:" + std::to_string(srv.port));
        CHECK(!c.hasSavedSession());

        net::Event ev;
        c.fetchServerInfo();
        CHECK(waitEvent(c, K::ServerInfoResult, ev, 10000));
        CHECK(ev.ok);
        CHECK(ev.info.compatible);
        CHECK_EQ(ev.info.name, std::string("Fake"));
        CHECK_EQ(ev.info.serverId, std::string("srv-1"));
        CHECK_EQ(int(ev.info.wsPort), int(srv.port));
        CHECK(ev.info.registrationOpen);
        CHECK_EQ(ev.info.powRegisterBits, 10);
        CHECK_EQ(ev.info.categories.size(), size_t(1));

        // Connecting without a session says so and does not retry.
        c.connect();
        CHECK(waitState(c, net::ConnState::Unauthorized, 5000));

        c.login("alice", "wrong");
        CHECK(waitEvent(c, K::LoginResult, ev, 10000));
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_credentials"));

        c.login("alice", "pw");      // 428 -> proof of work -> 200
        CHECK(waitEvent(c, K::LoginResult, ev, 20000));
        CHECK(ev.ok);
        CHECK_EQ(ev.account.username, std::string("alice"));
        CHECK_EQ(ev.account.userId, 7u);
        CHECK_EQ(srv.powAccepted.load(), 1);
        CHECK(c.hasSavedSession());
        CHECK_EQ(c.savedUsername(), std::string("alice"));

        c.fetchAccount();
        CHECK(waitEvent(c, K::AccountResult, ev, 10000));
        CHECK(ev.ok);
        CHECK(ev.account.mfaEnabled);
        CHECK_EQ(ev.account.ratings.size(), size_t(1));
        if (!ev.account.ratings.empty()) CHECK_EQ(ev.account.ratings[0].rating, 1520);

        // Realtime: Hello with the token, Welcome, ping, clock offset.
        std::vector<net::Event> seen;
        c.connect();
        CHECK(waitEvent(c, K::Welcome, ev, 10000, &seen));
        CHECK_EQ(ev.account.username, std::string("alice"));
        CHECK_EQ(ev.serverName, std::string("Fake"));
        CHECK(c.state() == net::ConnState::Online);
        CHECK(srv.helloTokenOk.load());
        auto t0 = std::chrono::steady_clock::now();
        while (c.pingMs() < 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(c.pingMs() >= 0);
        CHECK(c.pingMs() < 500);
        double skew = c.serverNowMs() - epochMs();
        CHECK(std::fabs(skew - FakeServer::kSkewMs) < 250);

        // Queue -> game -> move (confirmed as mine) -> the opponent's move -> draw offer.
        c.joinQueue("3+2", true);
        CHECK(waitEvent(c, K::QueueStatus, ev, 5000));
        CHECK_EQ(ev.queueCategory, std::string("3+2"));
        CHECK(waitEvent(c, K::GameSnapshot, ev, 5000));
        CHECK_EQ(ev.game.id, uint64_t(77));
        CHECK_EQ(ev.game.black.name, std::string("bob"));
        CHECK_EQ(ev.game.you, 0);
        chess::Position pos;
        c.sendMove(77, 0, net::packMove(12, 28, 0), pos.fen(), 1500, false);
        CHECK(waitEvent(c, K::MoveMade, ev, 5000));
        CHECK(ev.mine);
        CHECK_EQ(ev.ply, 0);
        CHECK_EQ(int(ev.flags), int(pr::MoveFlag::DoublePush));
        CHECK(waitEvent(c, K::MoveMade, ev, 5000));
        CHECK(!ev.mine);
        CHECK_EQ(ev.ply, 1);
        CHECK_EQ(ev.game.running, 0);          // White's clock runs from White's second move
        CHECK(waitEvent(c, K::GameEvent, ev, 5000));
        CHECK_EQ(ev.gameEventKind, int(pr::GameEventKind::DrawOffered));
        CHECK_EQ(ev.game.moves.size(), size_t(2));
        CHECK_EQ(ev.game.drawOfferBy, 1);
        // A move with a wrong position digest is refused.
        c.sendMove(77, 2, net::packMove(6, 21, 0), "8/8/8/8/8/8/8/K6k w - - 0 1", 100, false);
        CHECK(waitEvent(c, K::MoveRejected, ev, 5000));
        CHECK_EQ(ev.code, int(pr::ErrorCode::Desync));

        // Network failure: reconnects by itself, the game stays until a new snapshot. The automatic
        // reconnection reuses the /info answer of the connection that reached Welcome.
        int hellosBefore = srv.hellos.load(), infosBefore = srv.infos.load();
        srv.dropWebSockets();
        CHECK(waitState(c, net::ConnState::Reconnecting, 5000));
        CHECK(waitEvent(c, K::GameSnapshot, ev, 10000));   // Welcome.activeGame, then the snapshot
        CHECK_EQ(ev.game.id, uint64_t(77));
        CHECK(c.state() == net::ConnState::Online);
        CHECK_EQ(srv.hellos.load(), hellosBefore + 1);
        CHECK_EQ(srv.infos.load(), infosBefore);
        CHECK_EQ(ev.game.moves.size(), size_t(2));
        CHECK_EQ(ev.game.running, 0);
        CHECK_EQ(ev.game.drawOfferBy, 2);

        c.resign(77);
        CHECK(waitEvent(c, K::GameEnd, ev, 5000));
        CHECK_EQ(ev.game.status, int(pr::GameStatus::BlackWins));
        CHECK_EQ(ev.game.reason, int(pr::EndReason::Resignation));

        // Banned: the Notice's end of ban comes with the state; no retry.
        srv.kick(pr::CloseCode::Banned, true);
        net::Event st;
        CHECK(waitEvent(c, K::ConnectionChanged, st, 5000, nullptr, [](const net::Event& e) { return e.state == net::ConnState::Banned; }));
        CHECK_EQ(st.noticeArg, 4102444800000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        CHECK(c.state() == net::ConnState::Banned);

        // Token refused (4003): Unauthorized, the saved session is dropped. (A connect() asked by the
        // player reads /info again.)
        infosBefore = srv.infos.load();
        c.connect();
        CHECK(waitState(c, net::ConnState::Online, 10000));
        CHECK_EQ(srv.infos.load(), infosBefore + 1);
        srv.kick(pr::CloseCode::Unauthorized);
        CHECK(waitState(c, net::ConnState::Unauthorized, 5000));
        CHECK(!c.hasSavedSession());
        CHECK_EQ(c.savedUsername(), std::string("alice"));

        // Another origin (same machine, other name) never sees this origin's credentials.
        c.login("alice", "pw");
        CHECK(waitEvent(c, K::LoginResult, ev, 20000) && ev.ok);
        net::ServerEndpoint other = ep;
        other.host = "localhost";
        c.setServer(other);
        CHECK(!c.hasSavedSession());
        c.setServer(ep);
        CHECK(c.hasSavedSession());

        // Logout: revoked on the server, erased here.
        c.logout();
        CHECK(waitEvent(c, K::LogoutResult, ev, 10000));
        CHECK(ev.ok);
        CHECK_EQ(srv.logouts.load(), 1);
        CHECK(!c.hasSavedSession());

        // Commands that need the connection answer "offline" instead of vanishing.
        c.joinQueue("3+2", false);
        CHECK(waitEvent(c, K::ServerError, ev, 5000));
        CHECK_EQ(ev.error, std::string("offline"));
    }
}

TEST(net_online_client_unreachable) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    // A port nobody listens on (any more): a network error, quickly.
    uint16_t port = 0;
    {
        FakeServer probe;
        CHECK(probe.start());
        port = probe.port;
    }
    std::string credPath = tempCredentialPath("unreachable");
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        net::ServerEndpoint ep;
        ep.host = "127.0.0.1";
        ep.apiPort = port;
        ep.insecureDev = true;
        c.setServer(ep);
        c.fetchServerInfo();
        net::Event ev;
        CHECK(waitEvent(c, net::Event::Kind::ServerInfoResult, ev, 20000));
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("network"));
        // An invalid endpoint never reaches the network.
        ep.host = "example.org";
        c.setServer(ep);
        c.fetchServerInfo();
        CHECK(waitEvent(c, net::Event::Kind::ServerInfoResult, ev, 5000));
        CHECK_EQ(ev.error, std::string("invalid_server"));
    }
    net::sys::removeFile(credPath);
}

// ---- pacing: reconnection delays and the client Ping interval ----

TEST(net_reconnect_delay_policy) {
    using net::RetryCause;
    // No game in progress, no Retry-After unless given.
    auto idle = [](int attempt, RetryCause c, double u, uint32_t retryAfterMs = 0) {
        return net::reconnectDelayMs(attempt, c, u, false, retryAfterMs);
    };
    auto inGame = [](int attempt, RetryCause c, double u) { return net::reconnectDelayMs(attempt, c, u, true, 0); };
    const double top = std::nextafter(1.0, 0.0);
    // Failure: full jitter, uniform in [0.5 s, min(30 s, 2 s x 2^attempt)].
    CHECK_EQ(idle(0, RetryCause::Failure, 0.0), 500u);
    CHECK_EQ(idle(0, RetryCause::Failure, 0.5), 1250u);
    CHECK(idle(0, RetryCause::Failure, top) >= 1999u);
    CHECK_EQ(idle(0, RetryCause::Failure, 1.0), 2000u);
    CHECK_EQ(idle(1, RetryCause::Failure, 1.0), 4000u);
    CHECK_EQ(idle(2, RetryCause::Failure, 1.0), 8000u);
    CHECK_EQ(idle(3, RetryCause::Failure, 1.0), 16000u);
    CHECK_EQ(idle(4, RetryCause::Failure, 1.0), 30000u);
    CHECK_EQ(idle(1000000, RetryCause::Failure, 1.0), 30000u);
    CHECK_EQ(idle(1000000, RetryCause::Failure, 0.0), 500u);
    CHECK_EQ(idle(-3, RetryCause::Failure, 1.0), 2000u);
    CHECK_EQ(idle(0, RetryCause::Failure, std::nan("")), 500u);
    CHECK_EQ(idle(0, RetryCause::Failure, -2.0), 500u);
    CHECK_EQ(idle(0, RetryCause::Failure, 7.0), 2000u);
    // A full server: 60 s to 120 s, whatever the attempt.
    for (int attempt : {0, 1, 5, 40}) {
        CHECK_EQ(idle(attempt, RetryCause::ServerFull, 0.0), 60000u);
        CHECK_EQ(idle(attempt, RetryCause::ServerFull, 0.25), 75000u);
        CHECK_EQ(idle(attempt, RetryCause::ServerFull, 1.0), 120000u);
    }
    // A shutdown: the first attempt is spread over 5 s to 35 s, later ones are full jitter.
    CHECK_EQ(idle(0, RetryCause::Shutdown, 0.0), 5000u);
    CHECK_EQ(idle(0, RetryCause::Shutdown, 0.5), 20000u);
    CHECK_EQ(idle(0, RetryCause::Shutdown, 1.0), 35000u);
    CHECK_EQ(idle(1, RetryCause::Shutdown, 0.0), 500u);
    CHECK_EQ(idle(1, RetryCause::Shutdown, 1.0), 4000u);
    CHECK_EQ(idle(9, RetryCause::Shutdown, 1.0), 30000u);
    // A game in progress (15 s of reconnection grace by default, 90 s for a game restored after a
    // restart): 8 s at most between attempts, whatever the cause, unless the server gave a
    // Retry-After (below); a shutdown's first attempt is spread over 1 s to 8 s.
    CHECK_EQ(inGame(0, RetryCause::Failure, 1.0), 2000u);
    CHECK_EQ(inGame(2, RetryCause::Failure, 1.0), 8000u);
    CHECK_EQ(inGame(3, RetryCause::Failure, 1.0), 8000u);
    CHECK_EQ(inGame(50, RetryCause::Failure, 1.0), 8000u);
    CHECK_EQ(inGame(50, RetryCause::Failure, 0.0), 500u);
    CHECK_EQ(inGame(0, RetryCause::ServerFull, 1.0), 2000u);
    CHECK_EQ(inGame(7, RetryCause::ServerFull, 1.0), 8000u);
    CHECK_EQ(inGame(0, RetryCause::Shutdown, 0.0), 1000u);
    CHECK_EQ(inGame(0, RetryCause::Shutdown, 1.0), 8000u);
    CHECK_EQ(inGame(1, RetryCause::Shutdown, 1.0), 4000u);
    // Retry-After: at least that long, spread over up to half more, 10 minutes at most.
    CHECK_EQ(idle(0, RetryCause::Failure, 0.0, 30000), 30000u);
    CHECK_EQ(idle(0, RetryCause::Failure, 1.0, 30000), 45000u);
    CHECK_EQ(idle(0, RetryCause::ServerFull, 0.0, 1000), 60000u);   // already longer
    CHECK_EQ(idle(0, RetryCause::Failure, 1.0, 3600000), 600000u);
    CHECK_EQ(idle(0, RetryCause::Failure, 0.0, 0xFFFFFFFFu), 600000u);
    CHECK_EQ(net::reconnectDelayMs(0, RetryCause::Failure, 0.0, true, 20000), 20000u);   // even in game
    // Every u in [0, 1) stays inside the bounds; the spread really is uniform (no clustering).
    for (int attempt = 0; attempt < 12; ++attempt) {
        uint32_t cap = uint32_t(std::min(30000.0, 2000.0 * std::pow(2.0, attempt)));
        uint32_t capInGame = std::min(cap, 8000u);
        int low = 0;
        for (int i = 0; i < 1000; ++i) {
            double u = i / 1000.0;
            uint32_t f = idle(attempt, RetryCause::Failure, u);
            CHECK(f >= 500u && f <= cap);
            if (f < 500u + (cap - 500u) / 2) ++low;
            uint32_t full = idle(attempt, RetryCause::ServerFull, u);
            CHECK(full >= 60000u && full <= 120000u);
            for (RetryCause c : {RetryCause::Failure, RetryCause::ServerFull, RetryCause::Shutdown}) {
                uint32_t g = inGame(attempt, c, u);
                CHECK(g >= 500u && g <= 8000u);
                if (c != RetryCause::Shutdown || attempt > 0) CHECK(g <= capInGame);
            }
        }
        CHECK(low >= 490 && low <= 510);
    }
}

TEST(net_client_ping_interval) {
    CHECK_EQ(net::clientPingIntervalMs(0), 10000u);            // not announced: the default
    CHECK_EQ(net::clientPingIntervalMs(1), 1000u);
    CHECK_EQ(net::clientPingIntervalMs(999), 1000u);
    CHECK_EQ(net::clientPingIntervalMs(1000), 1000u);
    CHECK_EQ(net::clientPingIntervalMs(2000), 2000u);
    CHECK_EQ(net::clientPingIntervalMs(25000), 25000u);
    CHECK_EQ(net::clientPingIntervalMs(60000), 60000u);
    CHECK_EQ(net::clientPingIntervalMs(60001), 60000u);
    CHECK_EQ(net::clientPingIntervalMs(0xFFFFFFFFu), 60000u);
}

namespace {

// One OnlineClient signed in on its own fake server. The pacing scenarios below wait for seconds
// each, so they run side by side on their own threads; each collects its failures, which the
// test then checks on its own thread.
struct PacingRig {
    FakeServer srv;
    std::string credPath;
    std::unique_ptr<net::OnlineClient> c;
    std::vector<std::string> fails;

    void expect(bool ok, const std::string& what) {
        if (!ok) fails.push_back(what);
    }
    bool until(const std::function<bool()>& pred, int timeoutMs) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
    bool stateIs(net::ConnState s, int timeoutMs) {
        return until([&] { return c->state() == s; }, timeoutMs);
    }
    // True when pred held at every check for that long.
    bool always(const std::function<bool()>& pred, int forMs) {
        return !until([&] { return !pred(); }, forMs);
    }
    static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    static double msSince(std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    }

    // Server, login, connect, Welcome.
    bool start(const char* tag) {
        if (!srv.start()) return false;
        credPath = tempCredentialPath(tag);
        c = std::make_unique<net::OnlineClient>();
        c->setCredentialsFile(credPath);
        net::ServerEndpoint ep;
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port;
        ep.insecureDev = true;
        c->setServer(ep);
        c->login("alice", "pw");
        net::Event ev;
        if (!waitEvent(*c, net::Event::Kind::LoginResult, ev, 20000) || !ev.ok) return false;
        c->connect();
        return waitEvent(*c, net::Event::Kind::Welcome, ev, 10000);
    }

    ~PacingRig() {
        c.reset();
        if (!credPath.empty()) net::sys::removeFile(credPath);
    }
};

// The client Ping after Welcome: one at once and three about a second apart, then one per
// Welcome.clientPingMs (60 s here: no fifth one during the test).
void pingPacingScenario(PacingRig& r) {
    auto t0 = std::chrono::steady_clock::now();
    r.expect(r.until([&] { return r.srv.pings.load() >= 1; }, 1000), "a Ping at once after Welcome");
    r.expect(r.until([&] { return r.srv.pings.load() >= 4; }, 6000), "three more quick pings");
    double t4 = r.msSince(t0);
    r.expect(t4 >= 3000 && t4 < 5500, "the quick pings about a second apart (4th after " + std::to_string(int(t4)) + " ms)");
    r.sleepMs(4000);
    r.expect(r.srv.pings.load() == 4, "then Welcome.clientPingMs: " + std::to_string(r.srv.pings.load()) + " pings");
    r.expect(r.c->pingMs() >= 0 && r.c->pingMs() < 500, "the ping indicator is known");
    r.expect(std::fabs(r.c->serverNowMs() - epochMs() - FakeServer::kSkewMs) < 250, "the server clock offset is known");
}

// Welcome.clientPingMs = 3.5 s: the fifth ping comes 3.5 s after the fourth (neither a fixed
// interval of the client's own, nor its 10 s default).
void pingIntervalScenario(PacingRig& r) {
    r.expect(r.until([&] { return r.srv.pings.load() >= 4; }, 6000), "four quick pings");
    auto t4 = std::chrono::steady_clock::now();
    r.expect(r.until([&] { return r.srv.pings.load() >= 5; }, 6000), "a fifth ping");
    double gap = r.msSince(t4);
    r.expect(gap >= 3000 && gap <= 4200, "the fifth ping after Welcome.clientPingMs (" + std::to_string(int(gap)) + " ms)");
}

// Welcome.heartbeatMs = 1 s and a server that sends no heartbeat (the client's own pings are a
// minute apart): nothing arrives after the quick pings. At 1.5 heartbeats (7.5 s at least) the
// client sends one Ping to ask; its answer keeps the connection, which otherwise ends at two
// heartbeats (10 s at least).
void probeScenario(PacingRig& r) {
    r.expect(r.until([&] { return r.srv.pings.load() >= 4; }, 6000), "four quick pings");
    auto t4 = std::chrono::steady_clock::now();
    int hellos = r.srv.hellos.load();
    r.expect(r.until([&] { return r.srv.pings.load() >= 5; }, 9000), "a probe after 7.5 s of silence");
    double at = r.msSince(t4);
    r.expect(at >= 7000 && at <= 8600, "the probe after 7.5 s of silence (" + std::to_string(int(at)) + " ms)");
    r.expect(r.always([&] { return r.c->state() == net::ConnState::Online; }, int(11500 - r.msSince(t4))),
             "answered: still online after 11.5 s of server silence");
    r.expect(r.srv.hellos.load() == hellos, "answered: no reconnection");
}

// The same with a probe that gets no answer: the connection ends at two heartbeats, not before.
void probeUnansweredScenario(PacingRig& r) {
    r.expect(r.until([&] { return r.srv.pings.load() >= 4; }, 6000), "four quick pings");
    auto t4 = std::chrono::steady_clock::now();
    r.sleepMs(200);                          // the fourth Pong is on its way; later pings get none
    r.srv.answerPings.store(false);
    r.expect(r.until([&] { return r.srv.pings.load() >= 5; }, 9000), "unanswered: a probe");
    r.expect(r.always([&] { return r.c->state() == net::ConnState::Online; }, int(9400 - r.msSince(t4))),
             "unanswered: online until two heartbeats");
    r.expect(r.stateIs(net::ConnState::Reconnecting, int(11500 - r.msSince(t4))), "unanswered: dropped at two heartbeats");
}

// A full server (close 4006, then HTTP 503 at the upgrade): no attempt for a minute; a connect()
// asked by the player goes at once and reads /info again.
void serverFullScenario(PacingRig& r) {
    int ups = r.srv.upgrades.load(), infos = r.srv.infos.load();
    r.srv.kick(4006);
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "4006: reconnecting");
    r.sleepMs(2200);
    r.expect(r.srv.upgrades.load() == ups, "4006: no attempt within 2 s");
    r.expect(r.c->state() == net::ConnState::Reconnecting, "4006: still reconnecting");
    auto t0 = std::chrono::steady_clock::now();
    r.c->connect();
    r.expect(r.stateIs(net::ConnState::Online, 5000), "connect() after 4006: online");
    r.expect(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3), "connect() is not delayed by the backoff");
    r.expect(r.srv.infos.load() == infos + 1, "connect() reads /info");

    r.c->disconnect();
    r.expect(r.stateIs(net::ConnState::Offline, 3000), "disconnect: offline");
    r.srv.upgradeStatus.store(503);
    ups = r.srv.upgrades.load();
    r.c->connect();
    r.expect(r.until([&] { return r.srv.upgrades.load() == ups + 1; }, 3000), "503: the upgrade was tried");
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "503: reconnecting");
    r.sleepMs(2200);
    r.expect(r.srv.upgrades.load() == ups + 1, "503: no attempt within 2 s");
    r.srv.upgradeStatus.store(0);
    r.c->connect();
    r.expect(r.stateIs(net::ConnState::Online, 5000), "connect() after 503: online");
}

// A shutdown (close 4008): the first attempt waits 5 s at least. The first 503 at the upgrade
// after a shutdown is the restart, not a full server: the next attempt follows quickly. A
// connect() asked by the player starts afresh: a 503 then means a full server.
void shutdownScenario(PacingRig& r) {
    int ups = r.srv.upgrades.load();
    r.srv.kick(pr::CloseCode::ShuttingDown);
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "4008: reconnecting");
    r.sleepMs(2200);
    r.expect(r.srv.upgrades.load() == ups, "4008: no attempt within 2 s");
    r.srv.upgradeStatus.store(503);
    r.c->connect();
    r.expect(r.until([&] { return r.srv.upgrades.load() == ups + 1; }, 3000), "connect() after 4008: tried at once");
    r.sleepMs(2200);
    r.expect(r.srv.upgrades.load() == ups + 1, "connect() after 4008, then 503: a full server (no attempt within 2 s)");
    r.srv.upgradeStatus.store(0);
    // 502 (attempt 1 follows), Error{ShuttingDown} + 4008 at Hello (a shutdown after a failed
    // attempt: the next one follows as a failure's), then 503 (the restart: tried again soon).
    ups = r.srv.upgrades.load();
    int hellos = r.srv.hellos.load();
    r.srv.scriptUpgrades({502, FakeServer::kShutdownAtHello, 503});
    r.c->connect();
    r.expect(r.stateIs(net::ConnState::Online, 16000), "online after the restart");
    r.expect(r.srv.upgrades.load() == ups + 4, "the restart: four upgrades (" + std::to_string(r.srv.upgrades.load() - ups) + ")");
    r.expect(r.srv.hellos.load() == hellos + 2, "the restart: two Hellos");
}

// A server that comes back full after a restart: only the first 503 after the shutdown is the
// restart; the next one is a full server (no attempt for a minute, where a failure's fourth
// attempt would come within 16 s).
void restartFullScenario(PacingRig& r) {
    r.c->disconnect();
    r.expect(r.stateIs(net::ConnState::Offline, 3000), "disconnect: offline");
    int ups = r.srv.upgrades.load();
    r.srv.scriptUpgrades({502, FakeServer::kShutdownAtHello, 503, 503});
    r.c->connect();
    r.expect(r.until([&] { return r.srv.upgrades.load() == ups + 4; }, 16000), "the restart, then 503 twice");
    r.expect(r.always([&] { return r.srv.upgrades.load() == ups + 4; }, 16500), "back full: no attempt within 16.5 s");
    r.expect(r.c->state() == net::ConnState::Reconnecting, "back full: still reconnecting");
}

// Notice{ServerShutdown}, then the connection drops without a close code: a shutdown too.
void shutdownNoticeScenario(PacingRig& r) {
    int ups = r.srv.upgrades.load();
    r.srv.noticeShutdown();
    r.sleepMs(200);
    r.srv.dropWebSockets();
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "notice + drop: reconnecting");
    r.sleepMs(2200);
    r.expect(r.srv.upgrades.load() == ups, "notice + drop: no attempt within 2 s");
}

// With a game in progress the reconnection grace is short: even a full server is tried again
// within seconds (8 s at most between attempts), not after a minute. After a shutdown too (1 s to
// 8 s), with the /info answer of the connection that reached Welcome: a restart's reconnection
// wave costs one TLS handshake per player, as a crash's (another server is caught at the 101
// answer, below).
void inGameScenario(PacingRig& r) {
    r.c->joinQueue("3+2", true);
    net::Event ev;
    r.expect(waitEvent(*r.c, net::Event::Kind::GameSnapshot, ev, 5000) && ev.game.id == 77, "in game: snapshot");
    int hellos = r.srv.hellos.load();
    r.srv.kick(4006);
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "in game, 4006: reconnecting");
    r.expect(r.until([&] { return r.srv.hellos.load() > hellos; }, 4000), "in game, 4006: tried again within 2 s");
    r.expect(r.stateIs(net::ConnState::Online, 3000), "in game: online again");
    hellos = r.srv.hellos.load();
    int infos = r.srv.infos.load();
    r.srv.kick(pr::CloseCode::ShuttingDown);
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "in game, 4008: reconnecting");
    r.expect(r.until([&] { return r.srv.hellos.load() > hellos; }, 9500), "in game, 4008: tried again within 8 s");
    r.expect(r.srv.infos.load() == infos,
             "in game, 4008: the /info answer is reused after a shutdown (" + std::to_string(r.srv.infos.load() - infos) + " reads)");
    r.expect(r.stateIs(net::ConnState::Online, 3000), "in game: online after the restart");
    r.expect(r.srv.infos.load() == infos, "in game: online after the restart without an /info read");
}

// A restart that brings another server at the same origin (a reinstall: another server id). The
// client reuses the /info answer of the previous server after the shutdown, so the new server id
// is seen in the 101 answer: the saved session is dropped instead of being sent in Hello.
void restartServerChangedScenario(PacingRig& r) {
    r.c->joinQueue("3+2", true);
    net::Event ev;
    r.expect(waitEvent(*r.c, net::Event::Kind::GameSnapshot, ev, 5000) && ev.game.id == 77, "in game: snapshot");
    int hellos = r.srv.hellos.load(), infos = r.srv.infos.load(), ups = r.srv.upgrades.load();
    r.srv.serverNo.store(2);
    r.srv.kick(pr::CloseCode::ShuttingDown);
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "4008: reconnecting");
    r.expect(waitEvent(*r.c, net::Event::Kind::ConnectionChanged, ev, 11000, nullptr,
                       [](const net::Event& e) { return e.state == net::ConnState::Unauthorized; }) &&
                 ev.error == "server_changed",
             "4008, another server id: unauthorized (server_changed)");
    r.expect(r.srv.upgrades.load() == ups + 1 && r.srv.infos.load() == infos,
             "4008, another server id: seen at the upgrade, no /info read (" + std::to_string(r.srv.infos.load() - infos) +
                 " reads, " + std::to_string(r.srv.upgrades.load() - ups) + " upgrades)");
    r.expect(r.srv.hellos.load() == hellos, "4008, another server id: no Hello, the token was not sent");
    r.expect(!r.c->hasSavedSession(), "4008, another server id: the saved session is dropped");
    r.sleepMs(1500);
    r.expect(r.srv.upgrades.load() == ups + 1 && r.c->state() == net::ConnState::Unauthorized,
             "4008, another server id: no further attempt");
}

// A reverse proxy answering 502 at the upgrade (its backend restarts): retried like a network
// failure, with the same /info answer.
void badGatewayScenario(PacingRig& r) {
    int ups = r.srv.upgrades.load(), infos = r.srv.infos.load();
    r.srv.upgradeStatus.store(502);
    r.srv.dropWebSockets();
    r.expect(r.until([&] { return r.srv.upgrades.load() >= ups + 2; }, 8000), "502: tried twice");
    r.expect(r.srv.infos.load() == infos, "502: the /info answer is kept (" + std::to_string(r.srv.infos.load() - infos) + " reads)");
    r.srv.upgradeStatus.store(0);
    r.expect(r.stateIs(net::ConnState::Online, 10000), "502: online again");
    r.expect(r.srv.infos.load() == infos, "502: online again without an /info read");
}

// 404 at the upgrade: the server may have changed since /info was read, which is read again.
void notFoundScenario(PacingRig& r) {
    int infos = r.srv.infos.load();
    r.srv.upgradeStatus.store(404);
    r.srv.dropWebSockets();
    r.expect(r.until([&] { return r.srv.infos.load() == infos + 1; }, 8000), "404: /info read again");
    r.srv.upgradeStatus.store(0);
    r.expect(r.stateIs(net::ConnState::Online, 10000), "404: online again");
}

// Another server at the same origin (a reinstall: another server id) while the client reuses the
// /info answer of the previous one: the 101 answer names it, and the saved session is dropped
// instead of being sent in Hello.
void serverChangedScenario(PacingRig& r) {
    int hellos = r.srv.hellos.load(), infos = r.srv.infos.load(), ups = r.srv.upgrades.load();
    r.srv.serverNo.store(2);
    r.srv.dropWebSockets();
    net::Event ev;
    r.expect(waitEvent(*r.c, net::Event::Kind::ConnectionChanged, ev, 5000, nullptr,
                       [](const net::Event& e) { return e.state == net::ConnState::Unauthorized; }) &&
                 ev.error == "server_changed",
             "another server id: unauthorized (server_changed)");
    r.expect(r.srv.upgrades.load() == ups + 1 && r.srv.infos.load() == infos, "another server id: seen at the upgrade");
    r.expect(r.srv.hellos.load() == hellos, "another server id: no Hello, the token was not sent");
    r.expect(!r.c->hasSavedSession(), "another server id: the saved session is dropped");
}

// Error{CheatDetected} + close 4302: no automatic reconnection.
void cheatScenario(PacingRig& r) {
    int ups = r.srv.upgrades.load();
    r.srv.kick(pr::CloseCode::CheatDetected);
    r.expect(r.stateIs(net::ConnState::Offline, 3000), "4302: offline");
    r.sleepMs(2200);
    r.expect(r.srv.upgrades.load() == ups && r.c->state() == net::ConnState::Offline, "4302: no reconnection");
}

// Joins the fake server's queue: game 77, the client plays White.
bool enterGame(PacingRig& r, net::Event& snapshot) {
    r.c->joinQueue("3+2", true);
    return waitEvent(*r.c, net::Event::Kind::GameSnapshot, snapshot, 5000) && snapshot.game.id == 77;
}

// Welcome.gestureRate 10, gestureBurst 6, GameSnapshot.autoPress false. A call every 2 ms for
// 1.5 s: only the latest Gesture waits and they go at the pace of a bucket one smaller than the
// server's, the very latest last; the numbering stays shared with the other messages. Nothing
// goes for another game. The opponent's S_Gesture become one OpponentGesture (the latest; 'game'
// not filled in), for the current game only.
void gestureScenario(PacingRig& r) {
    using K = net::Event::Kind;
    net::Event ev;
    if (!enterGame(r, ev)) return r.expect(false, "in game: snapshot");
    r.expect(!ev.game.autoPress, "GameSnapshot.autoPress reaches OnlineGame");

    const int kRate = 10, kCapacity = net::gestureSendCapacity(6);
    auto t0 = std::chrono::steady_clock::now();
    net::Gesture g;
    g.touch = 12;
    while (r.msSince(t0) < 1500) {
        ++g.ply;
        g.yaw = 0.001f * float(g.ply);
        r.c->sendGesture(77, g);
        r.sleepMs(2);
    }
    const int last = g.ply;
    r.expect(r.until([&] {
                 auto v = r.srv.gestures();
                 return !v.empty() && v.back().m.ply == last;
             }, 1000),
             "the latest Gesture goes");
    r.sleepMs(300);
    auto in = r.srv.gestures();
    const int n = int(in.size());
    double span = in.empty() ? 0.0 : std::chrono::duration<double, std::milli>(in.back().at - t0).count();
    r.expect(n >= 12, "gestures go all along (" + std::to_string(n) + ")");
    r.expect(n <= kCapacity + int(kRate * span / 1000.0) + 1,
             "at most the bucket's worth (" + std::to_string(n) + " in " + std::to_string(int(span)) + " ms)");
    bool paced = true, newer = true, current = true;
    for (size_t i = 0; i < in.size(); ++i) {
        current = current && in[i].m.game == 77 && in[i].m.touch == 12;
        if (i > 0) newer = newer && in[i].m.ply > in[i - 1].m.ply;
        for (size_t j = i; j < in.size(); ++j) {
            double w = std::chrono::duration<double, std::milli>(in[j].at - in[i].at).count();
            paced = paced && double(j - i + 1) <= kCapacity + kRate * w / 1000.0 + 3.0;   // + network jitter
        }
    }
    r.expect(paced, "paced in every window");
    r.expect(newer, "never an older state after a newer one");
    r.expect(current, "all for game 77, whole");
    r.expect(!in.empty() && in.back().m.yaw == net::gestureYawToWire(0.001f * float(last)), "the latest head angle");

    // A move after them: the numbering is shared, the server sees no gap.
    chess::Position pos;
    r.c->sendMove(77, 0, net::packMove(12, 28, 0), pos.fen(), 1500, false);
    r.expect(waitEvent(*r.c, K::MoveMade, ev, 5000, nullptr, [](const net::Event& e) { return e.mine; }), "the move after the gestures");
    r.expect(waitEvent(*r.c, K::GameEvent, ev, 5000), "the opponent's answer");
    r.expect(r.srv.seqOk.load(), "one numbering for every message");

    // Another game: dropped (and the next one of the current game still goes).
    size_t before = r.srv.gestures().size();
    g.ply = 2;
    r.c->sendGesture(78, g);
    r.sleepMs(400);
    r.expect(r.srv.gestures().size() == before, "no Gesture for another game");
    g.aim = 28;
    r.c->sendGesture(77, g);
    r.expect(r.until([&] { return r.srv.gestures().size() == before + 1; }, 1000), "the current game's goes");

    // The opponent's: five for this game and one for another, none polled in between.
    while (r.c->poll(ev)) {
    }
    pr::S_Gesture s;
    for (int i = 1; i <= 5; ++i) {
        net::Gesture o;
        o.ply = 2;
        o.touch = 52;
        o.aim = i == 5 ? 36 : 44;
        o.flags = i == 5 ? pr::GestureFlag::Side : 0;
        o.yaw = -0.1f * float(i);
        o.pitch = -0.2f;
        o.lean = 0.1f * float(i);
        s.game = 77;
        net::gestureToWire(o, s);
        r.srv.sendGesture(s);
    }
    s.game = 99;
    s.ply = 9;
    r.srv.sendGesture(s);
    r.sleepMs(800);
    int count = 0;
    net::Event got;
    while (r.c->poll(ev)) {
        if (ev.kind != K::OpponentGesture) continue;
        ++count;
        got = ev;
    }
    r.expect(count == 1, "one OpponentGesture, the latest (" + std::to_string(count) + ")");
    r.expect(got.gameId == 77 && got.game.id == 0, "for game 77, without a copy of the game");
    r.expect(got.gesture.ply == 2 && got.gesture.touch == 52 && got.gesture.aim == 36 && got.gesture.flags == pr::GestureFlag::Side,
             "the latest state");
    r.expect(std::fabs(got.gesture.yaw + 0.5f) < 1e-3f && std::fabs(got.gesture.pitch + 0.2f) < 1e-3f &&
                 std::fabs(got.gesture.lean - 0.5f) < 1e-3f,
             "the latest head");
}

// Welcome.gestureRate 0 (no relay on this server): no Gesture ever goes.
void gestureOffScenario(PacingRig& r) {
    net::Event ev;
    if (!enterGame(r, ev)) return r.expect(false, "in game: snapshot");
    r.expect(ev.game.autoPress, "GameSnapshot.autoPress true by default");
    net::Gesture g;
    for (int i = 0; i < 60; ++i) {
        g.ply = i;
        r.c->sendGesture(77, g);
        r.sleepMs(5);
    }
    r.sleepMs(500);
    r.expect(r.srv.gestures().empty(), "no Gesture at rate 0");
    r.expect(r.c->state() == net::ConnState::Online, "still online");
}

// A Gesture made while Reconnecting or Offline is never sent after the next Welcome.
void gestureDownScenario(PacingRig& r) {
    using K = net::Event::Kind;
    net::Event ev;
    if (!enterGame(r, ev)) return r.expect(false, "in game: snapshot");
    net::Gesture g;
    g.touch = 12;
    r.c->sendGesture(77, g);
    r.expect(r.until([&] { return r.srv.gestures().size() == 1; }, 1000), "online: it goes");

    r.srv.upgradeStatus.store(502);                  // no way back until the test says so
    r.srv.dropWebSockets();
    r.expect(r.stateIs(net::ConnState::Reconnecting, 3000), "reconnecting");
    g.touch = 13;
    for (int i = 0; i < 20; ++i) {
        r.c->sendGesture(77, g);
        r.sleepMs(10);
    }
    r.srv.upgradeStatus.store(0);
    r.expect(waitEvent(*r.c, K::GameSnapshot, ev, 15000), "back in the game");
    r.sleepMs(500);
    r.expect(r.srv.gestures().size() == 1, "nothing of the reconnection sent");

    r.c->disconnect();
    r.expect(r.stateIs(net::ConnState::Offline, 3000), "offline");
    g.touch = 14;
    r.c->sendGesture(77, g);
    r.c->connect();
    r.expect(waitEvent(*r.c, K::GameSnapshot, ev, 10000), "back in the game after connect()");
    r.sleepMs(500);
    r.expect(r.srv.gestures().size() == 1, "nothing of the offline time sent");

    g.touch = 15;
    r.c->sendGesture(77, g);
    r.expect(r.until([&] {
                 auto v = r.srv.gestures();
                 return v.size() == 2 && v.back().m.touch == 15;
             }, 1000),
             "online again: the next one goes");
}

// Starts each rig and runs its scenario on a thread of its own, then reports every failure.
void runRigs(PacingRig* rigs, const char* const* tags, void (*const* scenarios)(PacingRig&), int n) {
    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i) {
        threads.emplace_back([=] {
            if (!rigs[i].start(tags[i])) {
                rigs[i].fails.push_back("setup (server, login, Welcome)");
                return;
            }
            scenarios[i](rigs[i]);
        });
    }
    for (auto& t : threads) t.join();
    for (int i = 0; i < n; ++i) {
        for (const std::string& f : rigs[i].fails) std::fprintf(stderr, "  %s: %s\n", tags[i], f.c_str());
        CHECK(rigs[i].fails.empty());
    }
}

}  // namespace

TEST(net_online_client_pacing) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    constexpr int kRigs = 14;
    PacingRig rigs[kRigs];
    rigs[0].srv.clientPingMs.store(60000);
    rigs[5].srv.clientPingMs.store(3500);
    for (int i : {6, 7}) {
        rigs[i].srv.heartbeatMs.store(1000);
        rigs[i].srv.clientPingMs.store(60000);
    }
    const char* tags[kRigs] = {"pace-ping",     "pace-full",  "pace-shutdown", "pace-notice", "pace-ingame",
                               "pace-interval", "pace-probe", "pace-probe2",   "pace-502",    "pace-404",
                               "pace-srvid",    "pace-cheat", "pace-restart-full", "pace-srvid-restart"};
    void (*scenarios[kRigs])(PacingRig&) = {
        pingPacingScenario,   serverFullScenario, shutdownScenario,        shutdownNoticeScenario, inGameScenario,
        pingIntervalScenario, probeScenario,      probeUnansweredScenario, badGatewayScenario,     notFoundScenario,
        serverChangedScenario, cheatScenario,     restartFullScenario,     restartServerChangedScenario};
    runRigs(rigs, tags, scenarios, kRigs);
}

// Live gestures through OnlineClient: paced and coalesced at Welcome's rate, for the current game
// only, none without a relay (rate 0) or while the connection is down; the opponent's.
TEST(net_online_client_gestures) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    constexpr int kRigs = 3;
    PacingRig rigs[kRigs];
    rigs[0].srv.gestureRate.store(10);
    rigs[0].srv.gestureBurst.store(6);
    rigs[0].srv.autoPress.store(false);
    rigs[2].srv.gestureRate.store(10);
    rigs[2].srv.gestureBurst.store(20);
    const char* tags[kRigs] = {"gesture", "gesture-off", "gesture-down"};
    void (*scenarios[kRigs])(PacingRig&) = {gestureScenario, gestureOffScenario, gestureDownScenario};
    runRigs(rigs, tags, scenarios, kRigs);
}

// Frames the client ignores (a client message type, a server message that does not decode): the
// first ones of a connection are logged, the others counted in one line when it ends; it stays up.
TEST(net_online_client_bad_frames_logged_once) {
    if (!net::transportAvailable()) return;
    PacingRig r;
    CHECK(r.start("bad-frames"));
    if (!r.c) return;
    std::string logPath = tempCredentialPath("bad-frames-log");
    logx::init(logPath.c_str());
    const int ups = r.srv.upgrades.load();
    r.srv.sendBadFrames(5000);
    CHECK(r.always([&] { return r.c->state() == net::ConnState::Online; }, 500));
    r.srv.kick(1000);   // after the bad frames: the client has read them all when it sees the close
    CHECK(r.stateIs(net::ConnState::Reconnecting, 5000));
    logx::shutdown();
    CHECK_EQ(r.srv.upgrades.load(), ups);
    std::string text;
    CHECK(net::sys::readFile(logPath, text, 1 << 20));
    net::sys::removeFile(logPath);
    int perFrame = 0, summaries = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.find("ignoring a frame") != std::string::npos || line.find("malformed Welcome") != std::string::npos) ++perFrame;
        if (line.find("9995 more frames from the server ignored or malformed") != std::string::npos) ++summaries;
    }
    CHECK_EQ(perFrame, 5);
    CHECK_EQ(summaries, 1);
}

// The session a connection sent is the one erased when the server revokes it (Notice) or refuses
// it (close 4003): one saved since, by a sign-in on net-http, stays.
TEST(net_online_client_refusal_keeps_a_newer_session) {
    if (!net::transportAvailable()) return;
    for (bool refused : {false, true}) {
        PacingRig r;
        bool started = r.start(refused ? "refused-newer" : "revoked-newer");
        CHECK(started);
        if (!started) continue;
        r.srv.loginToken2.store(true);
        r.c->login("alice", "pw");
        net::Event ev;
        CHECK(waitEvent(*r.c, net::Event::Kind::LoginResult, ev, 20000) && ev.ok);
        if (refused) {
            r.srv.kick(pr::CloseCode::Unauthorized);
            CHECK(r.stateIs(net::ConnState::Unauthorized, 5000));
        } else {
            r.srv.noticeRevoked();
            CHECK(waitEvent(*r.c, net::Event::Kind::Notice, ev, 5000));
        }
        CHECK(r.c->hasSavedSession());
    }
}

// =============================================================================================
// Account API (HTTPS) against a scripted server (tests/http_fake.h): each call's request (method,
// path and query, body, bearer token) and what the client makes of the answers, errors included
// (a 401 erases the token, size caps, malformed answers); the realtime connection stops when the
// account is deleted; the saved session of the official server's former port moves to 443.
// =============================================================================================

namespace {

const std::string kRigToken = "sct_" + std::string(43, 'K');

fakehttp::Reply jsonReply(int status, const std::string& body) {
    fakehttp::Reply r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
    r.body = body;
    return r;
}

bool hasBearer(const fakehttp::Request& r) { return r.get("authorization") == "Bearer " + kRigToken; }

Value bodyOf(const fakehttp::Request& r) {
    Value v;
    net::json::parse(r.body, v);
    return v;
}

// One OnlineClient on its own scripted server (plain HTTP to 127.0.0.1), signed in as "alice"
// unless told otherwise: the token is in the credential file before the client reads it.
// concurrent: the server answers several connections at once (fakehttp::Server).
struct AccountRig {
    fakehttp::Server srv;
    std::string credPath;
    net::ServerEndpoint ep;
    std::unique_ptr<net::OnlineClient> c;

    AccountRig(const char* tag, fakehttp::Server::Handler h, bool signedIn = true, bool concurrent = false)
        : srv(std::move(h), concurrent) {
        credPath = tempCredentialPath(tag);
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port();
        ep.insecureDev = true;
        if (signedIn) {
            net::CredentialStore s(credPath);
            net::Credential cr;
            cr.origin = ep.origin();
            cr.username = "alice";
            cr.token = kRigToken;
            cr.serverId = "srv-acct";
            s.put(cr);
        }
        c = std::make_unique<net::OnlineClient>();
        c->setCredentialsFile(credPath);
        c->setServer(ep);
    }
    ~AccountRig() {
        c.reset();
        net::sys::removeFile(credPath);
    }
    net::Event wait(net::Event::Kind k) {
        net::Event ev;
        bool got = waitEvent(*c, k, ev, 15000);
        CHECK(got);
        return ev;
    }
    size_t count() const { return srv.requests().size(); }
    fakehttp::Request last() const {
        std::vector<fakehttp::Request> r = srv.requests();
        return r.empty() ? fakehttp::Request() : r.back();
    }
};

const char kGamesPage[] = R"({"games":[
  {"id":812,"category":"3+2","rated":true,"timeControl":"180+2","baseMs":180000,"incMs":2000,
   "white":{"name":"alice","rating":1520,"ratingAfter":1528,"ratingDiff":8},
   "black":{"name":"deleted#12","rating":1490,"ratingAfter":1482,"ratingDiff":-8},
   "color":"white","status":1,"reason":1,"result":"1-0","termination":"Checkmate","plies":41,
   "startedAt":1790000000000,"endedAt":1790000400000,"outcome":"win"},
  {"id":790,"category":"custom","rated":false,"timeControl":"600+5",
   "white":{"name":"bob","rating":null,"ratingAfter":null,"ratingDiff":null},
   "black":{"name":"alice","rating":1500,"ratingAfter":null,"ratingDiff":null},
   "color":"black","status":4,"reason":12,"result":"*","termination":"Aborted","plies":1,
   "startedAt":1789000000000,"endedAt":1789000030000,"outcome":"aborted"}],
 "next":790,"total":45})";

const char kGameDetails[] = R"({"id":812,"category":"3+2","rated":true,"timeControl":"180+2",
  "white":{"name":"alice","rating":1520,"ratingAfter":1528,"ratingDiff":8},
  "black":{"name":"bob","rating":1490,"ratingAfter":1482,"ratingDiff":-8},
  "status":1,"reason":1,"result":"1-0","termination":"Checkmate","plies":3,
  "startedAt":1790000000000,"endedAt":1790000400000,"baseMs":180000,"incMs":2000,"statusName":"WhiteWins",
  "rematchOf":700,"moves":[{"uci":"e2e4","spentMs":1500,"clockMs":180500},{"uci":"e7e8q","spentMs":null,"clockMs":null},
  {"uci":"a2a1n","spentMs":250,"clockMs":3000}],"pgn":{"Event":"Fake rated 3+2"},"you":"black","reportable":true})";

}  // namespace

TEST(net_account_games_history) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    std::atomic<int> mode{0};
    AccountRig r("acct-games", [&](const fakehttp::Request& q) {
        if (q.path.compare(0, 22, "/api/v1/account/games?") != 0) return jsonReply(404, R"({"error":"not_found"})");
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        switch (mode.load()) {
        case 1: return jsonReply(400, R"({"error":"invalid_filter","message":"result must be win, loss or draw."})");
        case 2: return jsonReply(200, R"({"games":[{"id":"x"}],"next":null,"total":1})");
        case 3: return jsonReply(200, R"({"games":{},"next":null,"total":0})");
        case 4: return jsonReply(401, R"({"error":"invalid_token","message":"log in again"})");
        default: return jsonReply(200, kGamesPage);
        }
    });
    CHECK(r.srv.ok());
    CHECK(r.c->hasSavedSession());

    r.c->fetchMyGames(0, 20, net::GamesFilter());
    net::Event ev = r.wait(K::GamesResult);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("GET"));
    CHECK_EQ(q.path, std::string("/api/v1/account/games?limit=20"));
    CHECK(hasBearer(q));
    CHECK(q.body.empty());
    CHECK(ev.ok);
    const net::GamesPage& p = ev.gamesPage;
    CHECK_EQ(p.before, uint64_t(0));
    CHECK_EQ(p.next, uint64_t(790));
    CHECK_EQ(p.total, 45);
    CHECK_EQ(p.games.size(), size_t(2));
    if (p.games.size() == 2) {
        const net::GameSummary& a = p.games[0];
        CHECK_EQ(a.id, uint64_t(812));
        CHECK_EQ(a.category, std::string("3+2"));
        CHECK(a.rated);
        CHECK_EQ(a.baseMs, int64_t(180000));
        CHECK_EQ(a.incMs, int64_t(2000));
        CHECK_EQ(a.white.name, std::string("alice"));
        CHECK_EQ(a.white.rating, 1520);
        CHECK(a.white.ratingChanged);
        CHECK_EQ(a.white.ratingAfter, 1528);
        CHECK_EQ(a.white.ratingDiff, 8);
        CHECK_EQ(a.black.name, std::string("deleted#12"));
        CHECK_EQ(a.black.ratingDiff, -8);
        CHECK_EQ(a.you, 0);
        CHECK_EQ(a.status, 1);
        CHECK_EQ(a.reason, 1);
        CHECK_EQ(a.result, std::string("1-0"));
        CHECK_EQ(a.plies, 41);
        CHECK_EQ(a.startedAtMs, int64_t(1790000000000));
        CHECK_EQ(a.endedAtMs, int64_t(1790000400000));
        const net::GameSummary& b = p.games[1];
        CHECK_EQ(b.id, uint64_t(790));
        CHECK_EQ(b.category, std::string("custom"));
        CHECK(!b.rated);
        CHECK_EQ(b.baseMs, int64_t(600000));        // from timeControl when baseMs is absent
        CHECK_EQ(b.incMs, int64_t(5000));
        CHECK_EQ(b.white.rating, 0);                // null: unknown
        CHECK(!b.white.ratingChanged);
        CHECK(!b.black.ratingChanged);
        CHECK_EQ(b.you, 1);
        CHECK_EQ(b.result, std::string("*"));
        CHECK_EQ(b.reason, 12);
    }

    // The cursor and the filters, URL-encoded ("3+2" would read "3 2"); limit at most 50.
    net::GamesFilter f;
    f.category = "3+2";
    f.rated = 1;
    f.result = "win";
    r.c->fetchMyGames(790, 99, f);
    ev = r.wait(K::GamesResult);
    CHECK_EQ(r.last().path, std::string("/api/v1/account/games?before=790&limit=50&category=3%2B2&rated=true&result=win"));
    CHECK_EQ(ev.gamesPage.before, uint64_t(790));
    CHECK_EQ(ev.gamesPage.filter.category, std::string("3+2"));   // the request, named in its answer
    CHECK_EQ(ev.gamesPage.filter.rated, 1);
    CHECK_EQ(ev.gamesPage.filter.result, std::string("win"));
    f = net::GamesFilter();
    f.rated = 0;
    f.category = "custom";
    r.c->fetchMyGames(0, 0, f);
    r.wait(K::GamesResult);
    CHECK_EQ(r.last().path, std::string("/api/v1/account/games?limit=20&category=custom&rated=false"));

    // Errors: the server's code (the request still named); malformed answers.
    mode = 1;
    f = net::GamesFilter();
    f.result = "draw";
    r.c->fetchMyGames(812, 10, f);
    ev = r.wait(K::GamesResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_filter"));
    CHECK_EQ(ev.gamesPage.before, uint64_t(812));
    CHECK_EQ(ev.gamesPage.filter.result, std::string("draw"));
    for (int m : {2, 3}) {
        mode = m;
        r.c->fetchMyGames(0, 10, net::GamesFilter());
        ev = r.wait(K::GamesResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_response"));
        CHECK(ev.gamesPage.games.empty());
    }
    CHECK(r.c->hasSavedSession());

    // 401: the session is gone, its token erased (the user name stays); later calls need a login.
    // Whatever the server's code (invalid_token for a refused bearer), the game gets one:
    // "unauthorized", and the same without a saved token (nothing sent).
    mode = 4;
    r.c->fetchMyGames(0, 10, net::GamesFilter());
    ev = r.wait(K::GamesResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    CHECK(ev.sessionLost);
    CHECK(!r.c->hasSavedSession());
    CHECK_EQ(r.c->savedUsername(), std::string("alice"));
    size_t before = r.count();
    r.c->fetchMyGames(0, 10, net::GamesFilter());
    ev = r.wait(K::GamesResult);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    CHECK_EQ(r.count(), before);
}

// A rating change the server leaves out is computed as ratingAfter - rating, clamped to the int
// range: a buggy or hostile server's extreme ratingAfter must not overflow it.
TEST(net_account_games_rating_change_clamped) {
    if (!net::transportAvailable()) return;
    AccountRig r("acct-games-clamp", [](const fakehttp::Request& q) {
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        return jsonReply(200, R"({"games":[{"id":5,"category":"3+2","rated":true,"timeControl":"180+2",
            "white":{"name":"alice","rating":1500,"ratingAfter":-2147483648},
            "black":{"name":"bob","rating":1490,"ratingAfter":1482},
            "color":"white","status":1,"reason":1,"result":"1-0","plies":2}],"next":null,"total":1})");
    });
    r.c->fetchMyGames(0, 20, net::GamesFilter());
    net::Event ev = r.wait(net::Event::Kind::GamesResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.gamesPage.games.size(), size_t(1));
    if (ev.gamesPage.games.size() == 1) {
        CHECK_EQ(ev.gamesPage.games[0].white.ratingDiff, int(INT32_MIN));
        CHECK_EQ(ev.gamesPage.games[0].black.ratingDiff, -8);
    }
}

// The real server refuses a session it no longer accepts (expired, revoked, the account gone)
// with 401 invalid_token. The game, fed the client's events, must then show the player signed out:
// on an account call, on a public read asked again without the token (its answer is ok), and on a
// call made after the token was erased.
TEST(net_account_refused_session_signs_the_game_out) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    auto handler = [](const fakehttp::Request& q) {
        if (q.has("authorization")) return jsonReply(401, R"({"error":"invalid_token","message":"Log in again."})");
        if (q.path == "/api/v1/games/812") return jsonReply(200, std::string(kGameDetails).substr(0, std::string(kGameDetails).find(R"(,"you")")) + "}");
        return jsonReply(401, R"({"error":"unauthorized"})");
    };
    {
        AccountRig r("acct-refused", handler);
        net::AccountInfo account;
        account.username = "alice";
        bool signedIn = true;
        game::AccountData data;
        r.c->fetchMyGames(0, 10, net::GamesFilter());
        net::Event ev = r.wait(K::GamesResult);
        CHECK(!ev.ok);
        CHECK(!r.c->hasSavedSession());
        CHECK(data.apply(ev, account, signedIn));
        CHECK(!signedIn);
        // A call after that: no token any more, nothing sent, still signed out.
        signedIn = true;
        const size_t n = r.count();
        r.c->fetchSessions();
        ev = r.wait(K::SessionsResult);
        CHECK(!ev.ok);
        CHECK_EQ(r.count(), n);
        CHECK(data.apply(ev, account, signedIn));
        CHECK(!signedIn);
    }
    {
        // A public read: the refused token is erased and the game asked for again without it. The
        // answer is the public one (ok), and the game learns that its session is gone.
        AccountRig r("acct-refused-public", handler);
        net::AccountInfo account;
        bool signedIn = true;
        game::AccountData data;
        data.gameWanted = 812;
        r.c->fetchGame(812);
        net::Event ev = r.wait(K::GameDetailsResult);
        CHECK(ev.ok);
        CHECK_EQ(ev.gameDetails.you, 2);
        CHECK(!r.c->hasSavedSession());
        CHECK(data.apply(ev, account, signedIn));
        CHECK(data.gameLoaded);
        CHECK(!signedIn);
    }
    // The GIFs need the session (no anonymous retry): refused on either route, it signs out like
    // any account call.
    for (int route = 0; route < 2; ++route) {
        AccountRig r(route == 0 ? "acct-refused-gif" : "acct-refused-gif-pgn", handler);
        net::AccountInfo account;
        bool signedIn = true;
        game::AccountData data;
        if (route == 0) r.c->downloadGameGif(812, net::GifOptions());
        else r.c->renderPgnGif("[Event \"x\"]\n[Result \"*\"]\n\n1. e4 *\n", net::GifOptions());
        net::Event ev = r.wait(K::GifResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(ev.sessionLost);
        CHECK_EQ(r.count(), size_t(1));
        CHECK(!r.c->hasSavedSession());
        CHECK(data.apply(ev, account, signedIn));
        CHECK(!signedIn);
    }
}

TEST(net_account_game_details) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    static_assert(chess::Knight == 2 && chess::Bishop == 3 && chess::Rook == 4 && chess::Queen == 5, "promotion numbering");
    std::atomic<int> mode{0};
    AccountRig r("acct-game", [&](const fakehttp::Request& q) {
        if (q.method != "GET" || q.path.compare(0, 14, "/api/v1/games/") != 0) return jsonReply(404, R"({"error":"not_found"})");
        if (q.path != "/api/v1/games/812") return jsonReply(404, R"({"error":"not_found","message":"No such game."})");
        std::string body = kGameDetails;
        switch (mode.load()) {
        case 1: {                                   // the public answer: no you, no reportable
            size_t at = body.find(R"(,"you")");
            body = body.substr(0, at) + "}";
            break;
        }
        case 2: body.replace(body.find("a2a1n"), 5, "a2a9n"); break;
        case 3: body.replace(body.find("\"id\":812"), 8, "\"id\":813"); break;
        case 4:                                     // a session the server no longer knows
            if (q.has("authorization")) return jsonReply(401, R"({"error":"invalid_token"})");
            body = body.substr(0, body.find(R"(,"you")")) + "}";
            break;
        default: break;
        }
        return jsonReply(200, body);
    });
    CHECK(r.srv.ok());

    r.c->fetchGame(812);
    net::Event ev = r.wait(K::GameDetailsResult);
    CHECK_EQ(r.last().method, std::string("GET"));
    CHECK_EQ(r.last().path, std::string("/api/v1/games/812"));
    CHECK(hasBearer(r.last()));                     // the token goes along when one is saved
    CHECK(ev.ok);
    CHECK_EQ(ev.gameId, uint64_t(812));
    const net::GameDetails& d = ev.gameDetails;
    CHECK_EQ(d.id, uint64_t(812));
    CHECK_EQ(d.you, 1);
    CHECK(d.reportable);
    CHECK_EQ(d.rematchOf, uint64_t(700));
    CHECK_EQ(d.plies, 3);
    CHECK_EQ(d.baseMs, int64_t(180000));
    CHECK_EQ(d.white.ratingAfter, 1528);
    CHECK_EQ(d.moves.size(), size_t(3));
    if (d.moves.size() == 3) {
        CHECK_EQ(d.moves[0].uci, std::string("e2e4"));
        CHECK_EQ(d.moves[0].move, net::packMove(12, 28, 0));
        CHECK_EQ(d.moves[0].spentMs, int64_t(1500));
        CHECK_EQ(d.moves[0].clockMs, int64_t(180500));
        CHECK_EQ(d.moves[1].move, net::packMove(52, 60, chess::Queen));
        CHECK_EQ(net::movePromo(d.moves[1].move), int(chess::Queen));
        CHECK_EQ(d.moves[1].spentMs, int64_t(-1));  // null: unknown
        CHECK_EQ(d.moves[1].clockMs, int64_t(-1));
        CHECK_EQ(d.moves[2].move, net::packMove(8, 0, chess::Knight));
        CHECK_EQ(d.moves[2].clockMs, int64_t(3000));
    }

    mode = 1;
    r.c->fetchGame(812);
    ev = r.wait(K::GameDetailsResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.gameDetails.you, 2);
    CHECK(!ev.gameDetails.reportable);

    // A move that is not UCI text, or another game than the one asked for: refused whole.
    for (int m : {2, 3}) {
        mode = m;
        r.c->fetchGame(812);
        ev = r.wait(K::GameDetailsResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_response"));
        CHECK(ev.gameDetails.moves.empty());
    }
    mode = 0;
    r.c->fetchGame(9);
    ev = r.wait(K::GameDetailsResult);
    CHECK_EQ(ev.error, std::string("not_found"));
    size_t n = r.count();
    r.c->fetchGame(0);
    ev = r.wait(K::GameDetailsResult);
    CHECK_EQ(ev.error, std::string("invalid_game_id"));
    CHECK_EQ(r.count(), n);

    // A refused token is erased, and the public answer asked for without it.
    CHECK(!ev.sessionLost);
    mode = 4;
    n = r.count();
    r.c->fetchGame(812);
    ev = r.wait(K::GameDetailsResult);
    CHECK(ev.ok);
    CHECK(ev.sessionLost);                          // the game signs out
    CHECK_EQ(ev.gameDetails.you, 2);
    std::vector<fakehttp::Request> all = r.srv.requests();
    CHECK_EQ(all.size(), n + 2);
    if (all.size() == n + 2) {
        CHECK(hasBearer(all[n]));
        CHECK(!all[n + 1].has("authorization"));
    }
    CHECK(!r.c->hasSavedSession());
    // Signed out: asked without a token.
    mode = 1;
    r.c->fetchGame(812);
    ev = r.wait(K::GameDetailsResult);
    CHECK(ev.ok);
    CHECK(!ev.sessionLost);
    CHECK(!r.last().has("authorization"));
}

TEST(net_account_pgn) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    const std::string pgn =
        "[Event \"Fake rated 3+2\"]\n[Site \"127.0.0.1\"]\n[Date \"2026.09.21\"]\n[Round \"-\"]\n[White \"alice\"]\n"
        "[Black \"bob\"]\n[Result \"1-0\"]\n[ScacelithGameId \"812\"]\n\n"
        "1. e4 {[%clk 0:03:00.5] [%emt 0:00:01.5]} e5 {[%clk 0:03:01.0] [%emt 0:00:01.0]} 1-0\n";
    std::string exact = "[Event \"x\"]\n\n{";
    exact += std::string((size_t(4) << 20) - exact.size() - 4, 'a');
    exact += "} *\n";
    std::atomic<int> mode{0};
    AccountRig r("acct-pgn", [&](const fakehttp::Request& q) {
        if (q.path != "/api/v1/games/812/pgn") return jsonReply(404, R"({"error":"not_found","message":"No such game."})");
        fakehttp::Reply rep;
        rep.headers.emplace_back("Content-Type", "application/x-chess-pgn; charset=utf-8");
        rep.headers.emplace_back("Content-Disposition", "attachment; filename=\"scacelith-812.pgn\"");
        switch (mode.load()) {
        case 1: rep.body = exact; break;
        case 2: rep.body = exact + "\n"; break;   // one byte over 4 MiB
        case 3: rep.body = "<html><body>Proxy error</body></html>"; break;
        case 4: return jsonReply(500, R"({"error":"internal_error","message":"The game cannot be replayed."})");
        default: rep.body = "\xEF\xBB\xBF" + pgn; break;
        }
        return rep;
    });
    CHECK(r.srv.ok());
    CHECK_EQ(exact.size(), size_t(4) << 20);

    r.c->downloadPgn(812);
    net::Event ev = r.wait(K::PgnResult);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("GET"));
    CHECK_EQ(q.path, std::string("/api/v1/games/812/pgn"));
    CHECK_EQ(q.get("accept"), std::string("application/x-chess-pgn"));
    CHECK(hasBearer(q));
    CHECK(ev.ok);
    CHECK_EQ(ev.gameId, uint64_t(812));
    CHECK_EQ(ev.text, "\xEF\xBB\xBF" + pgn);            // as received, byte for byte

    mode = 1;                                           // 4 MiB exactly: accepted
    r.c->downloadPgn(812);
    ev = r.wait(K::PgnResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.text.size(), size_t(4) << 20);
    for (int m : {2, 3}) {                              // too large; not a PGN
        mode = m;
        r.c->downloadPgn(812);
        ev = r.wait(K::PgnResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_response"));
        CHECK(ev.text.empty());
    }
    mode = 4;
    r.c->downloadPgn(812);
    ev = r.wait(K::PgnResult);
    CHECK_EQ(ev.error, std::string("internal_error"));
    mode = 0;
    r.c->downloadPgn(5);
    ev = r.wait(K::PgnResult);
    CHECK_EQ(ev.error, std::string("not_found"));
    CHECK_EQ(ev.gameId, uint64_t(5));
    CHECK(r.c->hasSavedSession());
}

// Animated GIFs: GET /games/:id/gif with the options in the query, POST /gif with the PGN text in
// JSON; signed in only (the renders count per account); the picture byte for byte, 16 MiB at
// most and only when it starts as a GIF does; the quota (429) and the busy renderer (503) with
// their wait; a refused or missing session is "unauthorized" (sessionLost when refused), as for
// every account call.
TEST(net_account_gif) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    // A 2x2 GIF (one frame, two colours): zero bytes inside, to see the body kept as binary.
    const unsigned char kTiny[] = {'G', 'I', 'F', '8', '9', 'a', 2, 0, 2, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255,
                                   0x2C, 0, 0, 0, 0, 2, 0, 2, 0, 0, 2, 2, 0x44, 0x01, 0, 0x3B};
    const std::string gif(reinterpret_cast<const char*>(kTiny), sizeof(kTiny));
    std::string big = gif.substr(0, 6);
    big += std::string(net::OnlineClient::kGifMaxBytes - big.size(), '\0');
    std::atomic<int> mode{0};
    AccountRig r("acct-gif", [&](const fakehttp::Request& q) {
        // A game's public details (no session needed): game 812 is there, no other.
        if (q.method == "GET" && q.path.compare(0, 14, "/api/v1/games/") == 0 && q.path.find('/', 14) == std::string::npos) {
            if (q.path == "/api/v1/games/812") return jsonReply(200, kGameDetails);
            return jsonReply(404, R"({"error":"not_found","message":"No such game."})");
        }
        // A server without the GIF routes: its router's answer, before any session check.
        if (mode.load() == 10) return jsonReply(404, R"({"error":"not_found","message":"No such endpoint."})");
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        const bool get = q.method == "GET" && q.path.compare(0, 22, "/api/v1/games/812/gif?") == 0;
        const bool post = q.method == "POST" && q.path == "/api/v1/gif";
        if (!get && !post) return jsonReply(404, R"({"error":"not_found","message":"No such game."})");
        fakehttp::Reply rep;
        rep.headers.emplace_back("Content-Type", "image/gif");
        switch (mode.load()) {
        case 1: rep.body = big; break;                                    // 16 MiB exactly
        case 2: rep.body = big + "!"; break;                              // one byte over
        case 3: rep.body = "<html><body>Proxy error</body></html>"; break;
        case 4: rep.body = "GIF87a" + gif.substr(6); break;
        case 5: {
            fakehttp::Reply busy = jsonReply(429, R"({"error":"rate_limited","message":"30 GIFs an hour.","retryAfter":95})");
            busy.headers.emplace_back("Retry-After", "95");
            return busy;
        }
        case 6: {                                                         // a proxy's page: the header only
            fakehttp::Reply busy;
            busy.status = 503;
            busy.headers.emplace_back("Content-Type", "text/html");
            busy.headers.emplace_back("Retry-After", "8");
            busy.body = "<html>Busy</html>";
            return busy;
        }
        case 7: {
            fakehttp::Reply busy = jsonReply(503, R"({"error":"server_busy","retryAfter":12})");
            return busy;
        }
        case 8: return jsonReply(422, R"({"error":"game_too_long","message":"Over 600 plies."})");
        case 9: return jsonReply(401, R"({"error":"invalid_token"})");
        case 11: return jsonReply(500, R"({"error":"render_failed","message":"The GIF could not be made."})");
        case 12: return jsonReply(503, R"({"error":"busy","message":"Try again shortly.","retryAfter":1})");
        default: rep.body = gif; break;
        }
        return rep;
    });
    CHECK(r.srv.ok());

    // GET with the defaults: medium, white, 500 ms, coordinates.
    r.c->downloadGameGif(812, net::GifOptions());
    net::Event ev = r.wait(K::GifResult);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("GET"));
    CHECK_EQ(q.path, std::string("/api/v1/games/812/gif?size=medium&orientation=white&delay=500&coords=1"));
    CHECK_EQ(q.get("accept"), std::string("image/gif"));
    CHECK(hasBearer(q));
    CHECK(q.body.empty());
    CHECK(ev.ok);
    CHECK_EQ(ev.gameId, uint64_t(812));
    CHECK_EQ(ev.text, gif);                             // byte for byte, zero bytes included
    net::GifOptions o;
    o.size = "small";
    o.orientation = "black";
    o.delayMs = 1500;
    o.coords = false;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK(ev.ok);
    CHECK_EQ(r.last().path, std::string("/api/v1/games/812/gif?size=small&orientation=black&delay=1500&coords=0"));
    r.c->downloadGameGif(5, o);                         // not a game of the server
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("not_found"));
    CHECK_EQ(ev.gameId, uint64_t(5));
    CHECK(ev.text.empty());
    CHECK_EQ(r.last().path, std::string("/api/v1/games/5"));   // its details: not there either

    // POST: the PGN text and the options in JSON.
    const std::string pgn = "[Event \"Casual\"]\n[White \"alice\"]\n[Black \"\xC3\x89lodie\"]\n[Result \"*\"]\n\n1. e4 e5 *\n";
    r.c->renderPgnGif(pgn, o);
    ev = r.wait(K::GifResult);
    q = r.last();
    CHECK_EQ(q.method, std::string("POST"));
    CHECK_EQ(q.path, std::string("/api/v1/gif"));
    CHECK_EQ(q.get("accept"), std::string("image/gif"));
    CHECK(hasBearer(q));
    Value b = bodyOf(q);
    CHECK_EQ(b["pgn"].asString(), pgn);
    CHECK_EQ(b["size"].asString(), std::string("small"));
    CHECK_EQ(b["orientation"].asString(), std::string("black"));
    CHECK_EQ(b["delayMs"].asInt(), 1500);
    CHECK(b["coords"].isBool());
    CHECK(!b["coords"].asBool(true));
    CHECK(ev.ok);
    CHECK_EQ(ev.gameId, uint64_t(0));
    CHECK_EQ(ev.text, gif);
    r.c->renderPgnGif(pgn, net::GifOptions());
    r.wait(K::GifResult);
    b = bodyOf(r.last());
    CHECK_EQ(b["size"].asString(), std::string("medium"));
    CHECK_EQ(b["orientation"].asString(), std::string("white"));
    CHECK_EQ(b["delayMs"].asInt(), 500);
    CHECK(b["coords"].asBool(false));

    // What the client refuses without asking: game 0, a PGN text over 64 KiB.
    size_t before = r.count();
    r.c->downloadGameGif(0, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("invalid_game_id"));
    std::string longPgn = "[Event \"x\"]\n\n{" + std::string(net::OnlineClient::kGifMaxPgnBytes, 'a') + "} *\n";
    r.c->renderPgnGif(longPgn, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("pgn_too_large"));
    CHECK_EQ(r.count(), before);
    std::string exactPgn = "[Event \"x\"]\n\n{";
    exactPgn += std::string(net::OnlineClient::kGifMaxPgnBytes - exactPgn.size() - 4, 'a') + "} *\n";
    CHECK_EQ(exactPgn.size(), net::OnlineClient::kGifMaxPgnBytes);
    r.c->renderPgnGif(exactPgn, o);                     // 64 KiB exactly: sent
    ev = r.wait(K::GifResult);
    CHECK(ev.ok);
    CHECK_EQ(r.count(), before + 1);

    // The cap and the signature.
    mode = 1;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.text.size(), net::OnlineClient::kGifMaxBytes);
    mode = 4;                                           // GIF87a: a GIF too
    r.c->renderPgnGif(pgn, o);
    ev = r.wait(K::GifResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.text.compare(0, 6, "GIF87a"), 0);
    for (int m : {2, 3}) {                              // too large; not a GIF
        mode = m;
        for (int route = 0; route < 2; ++route) {
            if (route == 0) r.c->downloadGameGif(812, o);
            else r.c->renderPgnGif(pgn, o);
            ev = r.wait(K::GifResult);
            CHECK(!ev.ok);
            CHECK_EQ(ev.error, std::string("invalid_response"));
            CHECK(ev.text.empty());
        }
    }

    // The quota and the busy renderer, with how long to wait (the body's, else the header's).
    mode = 5;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("rate_limited"));
    CHECK_EQ(ev.retryAfterSec, 95);
    CHECK_EQ(ev.gameId, uint64_t(812));
    r.c->renderPgnGif(pgn, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("rate_limited"));
    CHECK_EQ(ev.retryAfterSec, 95);
    mode = 6;
    r.c->renderPgnGif(pgn, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("server_busy"));
    CHECK_EQ(ev.retryAfterSec, 8);
    mode = 7;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("server_busy"));
    CHECK_EQ(ev.retryAfterSec, 12);
    mode = 8;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("game_too_long"));
    CHECK(!ev.sessionLost);
    CHECK(r.c->hasSavedSession());
    // The other documented answers: a render that failed; the 503 busy of a locked database, a
    // busy server like the renderer's, with its wait.
    mode = 11;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("render_failed"));
    CHECK(!ev.sessionLost);
    mode = 12;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("server_busy"));
    CHECK_EQ(ev.retryAfterSec, 1);
    CHECK(!ev.sessionLost);
    // A server without the GIF routes (an older one) answers 404 not_found: a server without GIFs
    // for POST /gif, which has no other not_found, and for GET when the game itself is there.
    mode = 10;
    r.c->renderPgnGif(pgn, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("gif_disabled"));
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("gif_disabled"));
    CHECK_EQ(ev.gameId, uint64_t(812));
    CHECK_EQ(r.last().path, std::string("/api/v1/games/812"));
    CHECK(!r.last().has("authorization"));              // a public read
    r.c->downloadGameGif(5, o);                         // the game is not there: not_found still
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("not_found"));
    CHECK(r.c->hasSavedSession());

    // A refused token is forgotten, and the answer has the one code of a session gone (the server
    // said invalid_token) with sessionLost; signed out, nothing is sent and the code is the same.
    mode = 9;
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    CHECK(ev.sessionLost);
    CHECK_EQ(ev.gameId, uint64_t(812));
    CHECK(ev.text.empty());
    CHECK(!r.c->hasSavedSession());
    before = r.count();
    r.c->downloadGameGif(812, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    CHECK(!ev.sessionLost);
    CHECK_EQ(ev.gameId, uint64_t(812));
    r.c->renderPgnGif(pgn, o);
    ev = r.wait(K::GifResult);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    CHECK(!ev.sessionLost);
    CHECK_EQ(r.count(), before);
}

// A GIF the server takes long to make (its rendering queue, then a render at the lowest priority:
// up to 45 s with its default settings, after which it answers 503 timeout) has a thread and a time
// limit of its own: the history asked for meanwhile is answered at once, the GIF when it is ready.
// Every answer names the server its command went to, the one in use when it was given.
TEST(net_account_gif_beside_other_calls) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    using SteadyClock = std::chrono::steady_clock;
    CHECK(net::OnlineClient::kGifTimeoutMs >= 2 * 45000);   // twice the server's default bound
    const int renderMs = 2500;
    AccountRig r(
        "acct-gif-slow",
        [&](const fakehttp::Request& q) {
            if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
            if (q.path.compare(0, 22, "/api/v1/games/812/gif?") == 0) {
                fakehttp::Reply rep;
                rep.headers.emplace_back("Content-Type", "image/gif");
                rep.body = "GIF89a" + std::string(64, 'x');
                rep.silenceMs = renderMs;
                return rep;
            }
            if (q.path.compare(0, 21, "/api/v1/account/games") == 0) return jsonReply(200, kGamesPage);
            return jsonReply(404, R"({"error":"not_found","message":"No such endpoint."})");
        },
        true, true);
    CHECK(r.srv.ok());
    const std::string origin = r.ep.origin();
    const SteadyClock::time_point t0 = SteadyClock::now();
    r.c->downloadGameGif(812, net::GifOptions());
    r.c->fetchMyGames(0, 10, net::GamesFilter());
    // Another server chosen meanwhile: its own requests go there (nothing listens on port 1), the
    // GIF's answer still names the server it came from.
    net::ServerEndpoint other = r.ep;
    other.apiPort = 1;
    r.c->setServer(other);
    r.c->fetchSessions();
    net::Event gif, games, sessions;
    double gifAt = -1, gamesAt = -1, sessionsAt = -1;
    while (SteadyClock::now() - t0 < std::chrono::seconds(20) && (gifAt < 0 || gamesAt < 0 || sessionsAt < 0)) {
        net::Event ev;
        while (r.c->poll(ev)) {
            const double at = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
            if (ev.kind == K::GifResult) {
                gif = ev;
                gifAt = at;
            } else if (ev.kind == K::GamesResult) {
                games = ev;
                gamesAt = at;
            } else if (ev.kind == K::SessionsResult) {
                sessions = ev;
                sessionsAt = at;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(gamesAt >= 0 && gamesAt < renderMs - 500);        // never behind the GIF
    CHECK(games.ok);
    CHECK_EQ(games.gamesPage.games.size(), size_t(2));
    CHECK_EQ(games.origin, origin);
    CHECK(sessionsAt >= 0);
    CHECK(!sessions.ok);
    CHECK_EQ(sessions.origin, other.origin());
    CHECK(gifAt >= renderMs);
    CHECK(gif.ok);
    CHECK_EQ(gif.gameId, uint64_t(812));
    CHECK_EQ(gif.text.compare(0, 6, "GIF89a"), 0);
    CHECK_EQ(gif.origin, origin);
}

// Out of memory while a large answer arrives (a GIF of up to 16 MiB on net-gif, a PGN of up to
// 4 MiB on net-http): the call still answers, a failure (invalid_response, its game named), so
// that the GIF saver and the game page waiting for it end; the next calls are answered as usual.
TEST(net_account_large_answers_out_of_memory) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    if (!allocfail::available()) SKIP("AddressSanitizer build: no simulated out of memory");
    allocfail::Reset reset;
    const std::string gif = "GIF89a" + std::string(size_t(12) << 20, '\0');
    std::string pgn = "[Event \"x\"]\n\n{";
    pgn += std::string(size_t(3) << 20, 'a');
    pgn += "} *\n";
    AccountRig r("acct-oom", [&](const fakehttp::Request& q) {
        allocfail::spareThisThread();   // the server has the memory it needs
        fakehttp::Reply rep;
        if (q.path.compare(0, 22, "/api/v1/games/812/gif?") == 0) {
            rep.headers.emplace_back("Content-Type", "image/gif");
            rep.body = gif;
        } else if (q.path == "/api/v1/games/812/pgn") {
            rep.headers.emplace_back("Content-Type", "application/x-chess-pgn; charset=utf-8");
            rep.body = pgn;
        } else {
            return jsonReply(404, R"({"error":"not_found","message":"No such endpoint."})");
        }
        return rep;
    });
    CHECK(r.srv.ok());
    game::GifSaver saver;
    CHECK(saver.begin("history:812", 812, net::sys::exeDirectory() + "acct-oom-gif", "never.gif"));
    allocfail::failFrom(size_t(8) << 20);
    r.c->downloadGameGif(812, net::GifOptions());
    net::Event ev = r.wait(K::GifResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_response"));
    CHECK_EQ(ev.gameId, uint64_t(812));
    CHECK(ev.text.empty());
    CHECK_EQ(ev.origin, r.ep.origin());
    CHECK(saver.finish(std::move(ev)));
    CHECK(saver.stage() == game::GifSaver::Stage::Failed);
    CHECK(!saver.busy());
    CHECK_EQ(saver.error(), std::string("invalid_response"));

    allocfail::failFrom(size_t(2) << 20);
    r.c->downloadPgn(812);
    ev = r.wait(K::PgnResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_response"));
    CHECK_EQ(ev.gameId, uint64_t(812));
    CHECK(ev.text.empty());

    // Memory again: the same calls are answered (neither thread stopped).
    allocfail::failFrom(0);
    r.c->downloadGameGif(812, net::GifOptions());
    ev = r.wait(K::GifResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.text.size(), gif.size());
    r.c->downloadPgn(812);
    ev = r.wait(K::PgnResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.text.size(), pgn.size());
    CHECK(r.c->hasSavedSession());
}

TEST(net_account_sessions) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    AccountRig r("acct-sessions", [&](const fakehttp::Request& q) {
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        if (q.method == "GET" && q.path == "/api/v1/auth/sessions")
            return jsonReply(200, R"({"sessions":[
              {"id":31,"createdAt":1789000000000,"lastSeenAt":1790000000000,"expiresAt":1792000000000,"clientLabel":"Scacelith/0.1.0 win64","current":false},
              {"id":32,"createdAt":1789500000000,"lastSeenAt":1790000500000,"expiresAt":1792500000000,"clientLabel":null,"current":true}]})");
        if (q.method == "DELETE" && (q.path == "/api/v1/auth/sessions/31" || q.path == "/api/v1/auth/sessions/32"))
            return jsonReply(200, R"({"status":"revoked"})");
        return jsonReply(404, R"({"error":"not_found","message":"No such session."})");
    });
    CHECK(r.srv.ok());

    r.c->fetchSessions();
    net::Event ev = r.wait(K::SessionsResult);
    CHECK_EQ(r.last().method, std::string("GET"));
    CHECK_EQ(r.last().path, std::string("/api/v1/auth/sessions"));
    CHECK(ev.ok);
    CHECK_EQ(ev.sessions.size(), size_t(2));
    if (ev.sessions.size() == 2) {
        CHECK_EQ(ev.sessions[0].id, int64_t(31));
        CHECK_EQ(ev.sessions[0].createdAtMs, int64_t(1789000000000));
        CHECK_EQ(ev.sessions[0].lastSeenAtMs, int64_t(1790000000000));
        CHECK_EQ(ev.sessions[0].expiresAtMs, int64_t(1792000000000));
        CHECK_EQ(ev.sessions[0].clientLabel, std::string("Scacelith/0.1.0 win64"));
        CHECK(!ev.sessions[0].current);
        CHECK_EQ(ev.sessions[1].clientLabel, std::string(""));   // null: the client gave none
        CHECK(ev.sessions[1].current);
    }

    // Another device: signed out there, not here.
    r.c->revokeSession(31);
    ev = r.wait(K::SessionRevoked);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("DELETE"));
    CHECK_EQ(q.path, std::string("/api/v1/auth/sessions/31"));
    CHECK(hasBearer(q));
    CHECK(q.body.empty());
    CHECK(ev.ok);
    CHECK_EQ(ev.sessionId, int64_t(31));
    CHECK(r.c->hasSavedSession());
    r.c->revokeSession(77);
    ev = r.wait(K::SessionRevoked);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("not_found"));
    CHECK_EQ(ev.sessionId, int64_t(77));
    size_t n = r.count();
    r.c->revokeSession(0);
    ev = r.wait(K::SessionRevoked);
    CHECK_EQ(ev.error, std::string("not_found"));
    CHECK_EQ(r.count(), n);
    // The session of this game: signed out here too.
    r.c->revokeSession(32);
    ev = r.wait(K::SessionRevoked);
    CHECK(ev.ok);
    CHECK(!r.c->hasSavedSession());
    CHECK(r.c->state() == net::ConnState::Offline);
}

TEST(net_account_me_and_preferences) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    std::atomic<int> mode{0};
    AccountRig r("acct-prefs", [&](const fakehttp::Request& q) {
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        if (q.method == "PUT" && q.path == "/api/v1/account/preferences") {
            Value b = bodyOf(q);
            return jsonReply(200, R"({"preferences":{"acceptChallenges":")" + b["acceptChallenges"].asString() + "\"}}");
        }
        if (q.method == "GET" && q.path == "/api/v1/account/me") {
            if (mode.load() == 1)
                return jsonReply(200, R"({"user":{"id":7,"username":"alice","email":"a@example.org","emailVerified":true,
                  "mfaEnabled":false,"googleLinked":false},"ratings":[],"sanctions":[],"ban":null})");
            return jsonReply(200, R"({"user":{"id":7,"username":"alice","email":"a@example.org","emailVerified":true,
              "mfaEnabled":true,"googleLinked":true,"hasPassword":false,"acceptChallenges":"none","createdAt":1700000000000,
              "lastLoginAt":1790000000000,"pendingEmail":"new@example.org"},"ratings":[{"category":"3+2","rating":1520,
              "games":3,"wins":2,"draws":0,"losses":1,"peak":1530,"provisional":true}],"sanctions":[],"ban":null})");
        }
        return jsonReply(404, R"({"error":"not_found"})");
    });
    CHECK(r.srv.ok());

    r.c->fetchAccount();
    net::Event ev = r.wait(K::AccountResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.account.userId, 7u);
    CHECK(ev.account.googleLinked);
    CHECK(!ev.account.hasPassword);
    CHECK(!ev.account.acceptChallenges);
    CHECK_EQ(ev.account.pendingEmail, std::string("new@example.org"));
    CHECK_EQ(ev.account.createdAtMs, int64_t(1700000000000));
    CHECK_EQ(ev.account.lastLoginAtMs, int64_t(1790000000000));
    CHECK_EQ(ev.account.ratings.size(), size_t(1));
    mode = 1;                                         // a server without these fields: the defaults
    r.c->fetchAccount();
    ev = r.wait(K::AccountResult);
    CHECK(ev.ok);
    CHECK(ev.account.hasPassword);
    CHECK(ev.account.acceptChallenges);
    CHECK(ev.account.pendingEmail.empty());
    CHECK_EQ(ev.account.createdAtMs, int64_t(0));
    CHECK_EQ(ev.account.lastLoginAtMs, int64_t(0));

    r.c->setAcceptChallenges(false);
    ev = r.wait(K::PreferencesResult);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("PUT"));
    CHECK_EQ(q.path, std::string("/api/v1/account/preferences"));
    CHECK_EQ(q.get("content-type"), std::string("application/json"));
    CHECK_EQ(q.body, std::string(R"({"acceptChallenges":"none"})"));
    CHECK(hasBearer(q));
    CHECK(ev.ok);
    CHECK(!ev.account.acceptChallenges);
    r.c->setAcceptChallenges(true);
    ev = r.wait(K::PreferencesResult);
    CHECK_EQ(r.last().body, std::string(R"({"acceptChallenges":"all"})"));
    CHECK(ev.ok);
    CHECK(ev.account.acceptChallenges);
}

// The categories of /info: an id longer than the protocol carries (7 bytes) or with a control
// character is dropped; any other one is kept, the official "digits+digits" form or not.
TEST(net_info_category_ids) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    AccountRig r("info-categories", [&](const fakehttp::Request& q) {
        if (q.path != "/api/v1/info") return jsonReply(404, R"({"error":"not_found"})");
        return jsonReply(200, R"({"name":"Fake","categories":[{"id":"3+2","baseSec":180,"incSec":2},{"id":"1234567"},
          {"id":"blitz"},{"id":"é+1"},{"id":"12345678"},{"id":"éééé"},{"id":"5+\n3"},
          {"id":"5+\t3"},{"id":"5+3\u007f"},{"id":""}]})");
    }, false);
    CHECK(r.srv.ok());
    r.c->fetchServerInfo();
    net::Event ev = r.wait(net::Event::Kind::ServerInfoResult);
    std::vector<std::string> ids;
    for (const net::Category& c : ev.info.categories) ids.push_back(c.id);
    CHECK(ids == std::vector<std::string>({"3+2", "1234567", "blitz", "\xC3\xA9+1"}));
    CHECK_EQ(ev.info.categories.size() > 0 ? ev.info.categories[0].baseSec : 0, 180);
}

TEST(net_account_email_change) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    AccountRig r("acct-email", [&](const fakehttp::Request& q) {
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        if (q.method != "POST" || q.path != "/api/v1/account/email") return jsonReply(404, R"({"error":"not_found"})");
        Value b = bodyOf(q);
        if (b["password"].asString() != "pw") return jsonReply(403, R"({"error":"invalid_password","message":"Wrong password."})");
        std::string to = b["newEmail"].asString();
        if (to == "taken@example.org") return jsonReply(409, R"({"error":"email_taken"})");
        if (to == "a@example.org") return jsonReply(400, R"({"error":"same_email"})");
        if (b.has("code") || b.has("recoveryCode")) return jsonReply(200, R"({"status":"email_changed","email":")" + to + "\"}");
        return jsonReply(202, R"({"status":"verification_sent"})");
    });
    CHECK(r.srv.ok());

    r.c->changeEmail("new@example.org", "pw", "");
    net::Event ev = r.wait(K::EmailChangeResult);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.path, std::string("/api/v1/account/email"));
    CHECK(hasBearer(q));
    CHECK_EQ(q.body, std::string(R"({"newEmail":"new@example.org","password":"pw"})"));   // two-factor off: no field
    CHECK(ev.ok);
    CHECK_EQ(ev.status, std::string("verification_sent"));

    r.c->changeEmail("new@example.org", "pw", "123456");
    ev = r.wait(K::EmailChangeResult);
    CHECK_EQ(r.last().body, std::string(R"({"newEmail":"new@example.org","password":"pw","code":"123456"})"));
    CHECK(ev.ok);
    CHECK_EQ(ev.status, std::string("email_changed"));
    CHECK_EQ(ev.account.email, std::string("new@example.org"));
    r.c->changeEmail("new@example.org", "pw", "abcd-efgh-12");
    r.wait(K::EmailChangeResult);
    CHECK_EQ(r.last().body, std::string(R"({"newEmail":"new@example.org","password":"pw","recoveryCode":"abcd-efgh-12"})"));
    r.c->changeEmail("new@example.org", "pw", "12345");         // not 6 digits: a recovery code
    r.wait(K::EmailChangeResult);
    CHECK(bodyOf(r.last()).has("recoveryCode"));

    for (auto& c : std::vector<std::pair<std::string, std::string>>{{"taken@example.org", "email_taken"}, {"a@example.org", "same_email"}}) {
        r.c->changeEmail(c.first, "pw", "");
        ev = r.wait(K::EmailChangeResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, c.second);
    }
    r.c->changeEmail("new@example.org", "nope", "");
    ev = r.wait(K::EmailChangeResult);
    CHECK_EQ(ev.error, std::string("invalid_password"));
    CHECK(r.c->hasSavedSession());                    // a 403 is not a lost session
}

TEST(net_account_export) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    const std::string doc =
        R"({"format":"scacelith-account-export","version":1,"exportedAt":1790000000000,"server":{"name":"Fake","host":"127.0.0.1"},)"
        R"("account":{"id":7,"username":"alice","email":"a@example.org"},"ratings":[],"sessions":[{"id":32}],)"
        R"("games":{"total":2,"list":[{"id":812,"white":{"name":"alice"}},{"id":790}]},"notes":["No password hash."]})";
    // A hostile server's documents: tens of thousands of top-level members (each one kept, and a
    // name looked up among the kept ones: quadratic), or a top-level array of a million values.
    std::string manyKeys = R"({"format":"scacelith-account-export","version":1)";
    for (int i = 0; i < 60000; ++i) manyKeys += ",\"k" + std::to_string(i) + "\":0";
    manyKeys += "}";
    std::string bigArray = "[0";
    for (int i = 1; i < 1000000; ++i) bigArray += ",0";
    bigArray += "]";
    std::atomic<int> mode{0};
    AccountRig r("acct-export", [&](const fakehttp::Request& q) {
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        if (q.method != "POST" || q.path != "/api/v1/account/export") return jsonReply(404, R"({"error":"not_found"})");
        fakehttp::Reply rep = jsonReply(200, doc);
        rep.headers.emplace_back("Content-Disposition", "attachment; filename=\"scacelith-account-alice.json\"");
        switch (mode.load()) {
        case 1: rep.body = doc.substr(0, doc.size() - 1); break;                               // truncated JSON
        case 2: rep.body = R"({"format":"something-else","version":1})"; break;
        case 3:                                                                               // announces 64 MiB + 1
            rep.noLength = true;
            rep.headers.emplace_back("Content-Length", std::to_string((size_t(64) << 20) + 1));
            rep.body = "{}";
            break;
        case 4: {
            fakehttp::Reply busy = jsonReply(429, R"({"error":"rate_limited","message":"Later.","retryAfter":120})");
            busy.headers.emplace_back("Retry-After", "120");
            return busy;
        }
        case 5: rep.body = manyKeys; break;
        case 6: rep.body = bigArray; break;
        default: break;
        }
        return rep;
    });
    CHECK(r.srv.ok());

    r.c->exportAccount("pw", "123456");
    net::Event ev = r.wait(K::AccountExportResult);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("POST"));
    CHECK_EQ(q.path, std::string("/api/v1/account/export"));
    CHECK(hasBearer(q));
    CHECK_EQ(q.body, std::string(R"({"password":"pw","code":"123456"})"));
    CHECK(ev.ok);
    CHECK_EQ(ev.text, doc);                         // the document as the server wrote it
    r.c->exportAccount("pw", "");
    r.wait(K::AccountExportResult);
    CHECK_EQ(r.last().body, std::string(R"({"password":"pw"})"));

    for (int m : {1, 2, 3}) {
        mode = m;
        r.c->exportAccount("pw", "");
        ev = r.wait(K::AccountExportResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_response"));
        CHECK(ev.text.empty());
    }
    // Only a handful of top-level members are kept while the document is checked: more is not the
    // export, refused at once (neither memory nor time grows with what the server sends).
    for (int m : {5, 6}) {
        mode = m;
        const auto t0 = std::chrono::steady_clock::now();
        r.c->exportAccount("pw", "");
        ev = r.wait(K::AccountExportResult);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_response"));
        CHECK(ev.text.empty());
        CHECK(s < 3.0);
        if (s >= 3.0) std::fprintf(stderr, "  export mode %d answered after %.1f s\n", m, s);
    }
    mode = 4;
    r.c->exportAccount("pw", "");
    ev = r.wait(K::AccountExportResult);
    CHECK_EQ(ev.error, std::string("rate_limited"));
    CHECK_EQ(ev.retryAfterSec, 120);
}

TEST(net_account_delete) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    AccountRig r("acct-delete", [&](const fakehttp::Request& q) {
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"unauthorized"})");
        if (q.method != "POST" || q.path != "/api/v1/account/delete") return jsonReply(404, R"({"error":"not_found"})");
        Value b = bodyOf(q);
        if (b["password"].asString() != "pw") return jsonReply(403, R"({"error":"invalid_password"})");
        if (!b.has("code") && !b.has("recoveryCode")) return jsonReply(403, R"({"error":"mfa_code_required"})");
        return jsonReply(200, R"({"status":"deleted"})");
    });
    CHECK(r.srv.ok());
    r.c->deleteAccount("wrong", "123456");
    net::Event ev = r.wait(K::AccountDeleted);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_password"));
    r.c->deleteAccount("pw", "");
    ev = r.wait(K::AccountDeleted);
    CHECK_EQ(ev.error, std::string("mfa_code_required"));
    CHECK_EQ(r.last().body, std::string(R"({"password":"pw"})"));
    CHECK(r.c->hasSavedSession());

    r.c->deleteAccount("pw", "abcd-efgh-12");
    ev = r.wait(K::AccountDeleted);
    CHECK_EQ(r.last().path, std::string("/api/v1/account/delete"));
    CHECK_EQ(r.last().body, std::string(R"({"password":"pw","recoveryCode":"abcd-efgh-12"})"));
    CHECK(ev.ok);
    CHECK(!r.c->hasSavedSession());
    CHECK(r.c->savedUsername().empty());            // the deleted account's name is forgotten
    net::CredentialStore s(r.credPath);             // the origin's server id stays
    net::Credential cr;
    CHECK(s.get(r.ep.origin(), cr));
    CHECK(cr.token.empty());
    CHECK_EQ(cr.serverId, std::string("srv-acct"));
}

// The realtime connection of a deleted account stops for good.
TEST(net_account_delete_stops_realtime) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    FakeServer srv;
    CHECK(srv.start());
    std::string credPath = tempCredentialPath("acct-delete-rt");
    using K = net::Event::Kind;
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        net::ServerEndpoint ep;
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port;
        ep.insecureDev = true;
        c.setServer(ep);
        net::Event ev;
        c.login("alice", "pw");
        CHECK(waitEvent(c, K::LoginResult, ev, 20000) && ev.ok);
        c.connect();
        CHECK(waitEvent(c, K::Welcome, ev, 10000));
        int hellos = srv.hellos.load();
        c.deleteAccount("pw", "");
        std::vector<net::Event> seen;
        CHECK(waitEvent(c, K::AccountDeleted, ev, 10000, &seen));
        CHECK(ev.ok);
        CHECK_EQ(srv.deletes.load(), 1);
        CHECK(!c.hasSavedSession());
        // Offline (its event may come before or after AccountDeleted), and it stays so.
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (c.state() != net::ConnState::Offline && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(c.state() == net::ConnState::Offline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        while (c.poll(ev)) seen.push_back(ev);
        CHECK(std::none_of(seen.begin(), seen.end(), [](const net::Event& e) {
            return e.kind == net::Event::Kind::ConnectionChanged &&
                   (e.state == net::ConnState::Reconnecting || e.state == net::ConnState::Connecting);
        }));
        CHECK(c.state() == net::ConnState::Offline);
        CHECK_EQ(srv.hellos.load(), hellos);        // no reconnection
    }
    net::sys::removeFile(credPath);
}

// The realtime connection closes before the deletion is asked for, so that the server's closing
// of the deleted account's connections brings no revoked-session notice, refusal or Unauthorized
// state. A deletion that fails opens it again, if it was open; a stopped state stays.
TEST(net_account_delete_closes_realtime_first) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    FakeServer srv;
    CHECK(srv.start());
    std::string credPath = tempCredentialPath("acct-delete-first");
    using K = net::Event::Kind;
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        net::ServerEndpoint ep;
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port;
        ep.insecureDev = true;
        c.setServer(ep);
        net::Event ev;
        c.login("alice", "pw");
        CHECK(waitEvent(c, K::LoginResult, ev, 20000) && ev.ok);
        // Not connected: a deletion refused opens nothing.
        c.deleteAccount("wrong", "");
        CHECK(waitEvent(c, K::AccountDeleted, ev, 10000));
        CHECK_EQ(ev.error, std::string("invalid_password"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(c.state() == net::ConnState::Offline);
        CHECK_EQ(srv.hellos.load(), 0);
        // Stopped (Incompatible here): the state stays, nothing opens.
        c.connect();
        CHECK(waitEvent(c, K::Welcome, ev, 10000));
        srv.kick(pr::CloseCode::UnsupportedProtocol);
        CHECK(waitState(c, net::ConnState::Incompatible, 10000));
        c.deleteAccount("wrong", "");
        std::vector<net::Event> seen;
        CHECK(waitEvent(c, K::AccountDeleted, ev, 10000, &seen));
        CHECK_EQ(ev.error, std::string("invalid_password"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        while (c.poll(ev)) seen.push_back(ev);
        CHECK(std::none_of(seen.begin(), seen.end(), [](const net::Event& e) {
            return e.kind == net::Event::Kind::ConnectionChanged;
        }));
        CHECK(c.state() == net::ConnState::Incompatible);
        CHECK_EQ(srv.hellos.load(), 1);
        // Connected: closed for the request, open again once it is refused.
        c.connect();
        CHECK(waitEvent(c, K::Welcome, ev, 10000));
        c.deleteAccount("wrong", "");
        seen.clear();
        CHECK(waitEvent(c, K::AccountDeleted, ev, 10000, &seen));
        CHECK_EQ(ev.error, std::string("invalid_password"));
        // net-rt's events may come before or after AccountDeleted, the new Welcome too.
        const bool welcomed = std::any_of(seen.begin(), seen.end(), [](const net::Event& e) {
            return e.kind == net::Event::Kind::Welcome;
        });
        if (!welcomed) CHECK(waitEvent(c, K::Welcome, ev, 10000, &seen));
        // Offline in between.
        CHECK(std::any_of(seen.begin(), seen.end(), [](const net::Event& e) {
            return e.kind == net::Event::Kind::ConnectionChanged && e.state == net::ConnState::Offline;
        }));
        CHECK_EQ(srv.hellos.load(), 3);
        CHECK(c.hasSavedSession());
        // Deleted: the connection was closed before, and stays so.
        seen.clear();
        c.deleteAccount("pw", "");
        CHECK(waitEvent(c, K::AccountDeleted, ev, 10000, &seen));
        CHECK(ev.ok);
        CHECK_EQ(srv.deletes.load(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        while (c.poll(ev)) seen.push_back(ev);
        CHECK(std::none_of(seen.begin(), seen.end(), [](const net::Event& e) {
            return e.kind == net::Event::Kind::Notice || e.kind == net::Event::Kind::ServerError ||
                   (e.kind == net::Event::Kind::ConnectionChanged && e.state != net::ConnState::Offline);
        }));
        CHECK(c.state() == net::ConnState::Offline);
        CHECK_EQ(srv.hellos.load(), 3);
    }
    net::sys::removeFile(credPath);
}

// Signing out everywhere succeeds only when the server says it did. A refused token (401) revoked
// nothing: a failure, the token erased all the same (it is dead). Any other failure keeps the
// token, so that the player can try again. Signing out here erases it whatever the answer.
TEST(net_logout_all_verdict) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    using K = net::Event::Kind;
    std::atomic<int> mode{0};
    auto handler = [&](const fakehttp::Request& q) {
        if (q.method != "POST" || (q.path != "/api/v1/auth/logout-all" && q.path != "/api/v1/auth/logout"))
            return jsonReply(404, R"({"error":"not_found"})");
        if (!hasBearer(q)) return jsonReply(401, R"({"error":"invalid_token"})");
        fakehttp::Reply cut = jsonReply(200, R"({"status":"logged_out"})");
        cut.cutAfter = 5;                             // the connection lost in the middle of the answer
        switch (mode.load()) {
        case 1: return jsonReply(429, R"({"error":"rate_limited","retryAfter":30})");
        case 2: return jsonReply(503, R"({"error":"maintenance"})");
        case 3: return cut;
        case 4: return jsonReply(401, R"({"error":"invalid_token"})");
        default: return jsonReply(200, R"({"status":"logged_out"})");
        }
    };
    {
        AccountRig r("logout-all-fails", handler);
        CHECK(r.srv.ok());
        for (int m : {1, 2, 3}) {
            mode = m;
            r.c->logout(true);
            net::Event ev = r.wait(K::LogoutResult);
            CHECK_EQ(r.last().path, std::string("/api/v1/auth/logout-all"));
            CHECK(!ev.ok);
            CHECK(!ev.error.empty());
            CHECK(r.c->hasSavedSession());
        }
        mode = 4;
        r.c->logout(true);
        net::Event ev = r.wait(K::LogoutResult);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(!r.c->hasSavedSession());
    }
    {
        AccountRig r("logout-all-ok", handler);
        CHECK(r.srv.ok());
        mode = 0;
        r.c->logout(true);
        net::Event ev = r.wait(K::LogoutResult);
        CHECK(ev.ok);
        CHECK(!r.c->hasSavedSession());
    }
    {
        AccountRig r("logout-fails", handler);
        CHECK(r.srv.ok());
        mode = 2;
        r.c->logout(false);
        net::Event ev = r.wait(K::LogoutResult);
        CHECK_EQ(r.last().path, std::string("/api/v1/auth/logout"));
        CHECK_EQ(ev.error, std::string("maintenance"));
        CHECK(!r.c->hasSavedSession());
    }
}

namespace {
bool runningUnderWine() {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
#else
    return false;
#endif
}
}  // namespace

// Signing out while a connection attempt waits for a slow server (here /info, answered after 10 s)
// cancels that attempt, as disconnect() does: the state is Offline at once, not when the server
// answers or the request times out.
TEST(net_logout_cancels_a_connection_attempt) {
    if (!net::transportAvailable()) return;
    std::atomic<int> infos{0};
    fakehttp::Server srv(
        [&](const fakehttp::Request& q) {
            fakehttp::Reply rep;
            rep.headers.emplace_back("Content-Type", "application/json");
            if (q.path == "/api/v1/info") {
                ++infos;
                rep.silenceMs = 10000;
                rep.body = "{}";
            } else {
                rep.body = "{\"status\":\"logged_out\"}";
            }
            return rep;
        },
        true);
    CHECK(srv.ok());
    std::string credPath = tempCredentialPath("logout-attempt");
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = srv.port();
    ep.insecureDev = true;
    {
        net::CredentialStore s(credPath);
        net::Credential cr;
        cr.origin = ep.origin();
        cr.username = "alice";
        cr.token = "sct_" + std::string(43, 'L');
        CHECK(s.put(cr));
    }
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        c.setServer(ep);
        c.connect();
        CHECK(waitState(c, net::ConnState::Connecting, 5000));
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (infos.load() == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(infos.load(), 1);
        auto t0 = std::chrono::steady_clock::now();
        c.logout();
        std::vector<net::Event> seen;
        // Wine's WinHTTP does not end a blocking call when another thread closes its handle: the
        // cancelled call ends when the server answers or the call times out, not at once.
        CHECK(waitState(c, net::ConnState::Offline, runningUnderWine() ? 15000 : 3000, &seen));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "  Offline %.0f ms after logout()\n", ms);
        net::Event ev;
        CHECK(std::any_of(seen.begin(), seen.end(), [](const net::Event& e) { return e.kind == net::Event::Kind::LogoutResult; }) ||
              waitEvent(c, net::Event::Kind::LogoutResult, ev, 10000));
        CHECK(!c.hasSavedSession());
    }
    net::sys::removeFile(credPath);
}

// A disconnect() that comes as net-rt starts a connection attempt (after it took its queue, before
// the attempt resets its cancel token) stops that attempt too: Offline at once, not when the server
// answers /info (here after 3 s). The window is a few instructions wide, so it is tried at many
// delays after connect().
TEST(net_disconnect_as_an_attempt_starts) {
    if (!net::transportAvailable()) return;
    if (runningUnderWine()) {
        std::fprintf(stderr, "  (Wine: a cancelled call ends with the server's answer: skipped)\n");
        return;
    }
    fakehttp::Server srv(
        [&](const fakehttp::Request&) {
            fakehttp::Reply rep;
            rep.headers.emplace_back("Content-Type", "application/json");
            rep.silenceMs = 3000;
            rep.body = "{}";
            return rep;
        },
        true);
    CHECK(srv.ok());
    std::string credPath = tempCredentialPath("stop-attempt");
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = srv.port();
    ep.insecureDev = true;
    {
        net::CredentialStore s(credPath);
        net::Credential cr;
        cr.origin = ep.origin();
        cr.username = "alice";
        cr.token = "sct_" + std::string(43, 'S');
        CHECK(s.put(cr));
    }
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        c.setServer(ep);
        int tries = 0, late = 0;
        for (; tries < 600 && late == 0; ++tries) {
            c.connect();
            const auto t0 = std::chrono::steady_clock::now();
            const auto delay = std::chrono::nanoseconds((tries * 7919) % 100000);   // 0 to 100 us
            while (std::chrono::steady_clock::now() - t0 < delay) {
            }
            c.disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            while (c.state() != net::ConnState::Offline && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (c.state() != net::ConnState::Offline) ++late;
            net::Event ev;
            while (c.poll(ev)) {
            }
        }
        std::fprintf(stderr, "  %d tries, %d attempts still running 1 s after disconnect()\n", tries, late);
        CHECK_EQ(late, 0);
    }
    net::sys::removeFile(credPath);
}

// A proof of work for a server that was left stops at once: net-http is free for the next one.
TEST(net_pow_abandoned_on_server_switch) {
    if (!net::transportAvailable()) return;
    // No 30-bit nonce below 300 000 000 for this challenge (tens of seconds of hashing).
    std::atomic<int> logins{0}, infos{0};
    fakehttp::Server hard([&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/auth/login") ++logins;
        return jsonReply(428, "{\"error\":\"pow_required\",\"pow\":{\"challenge\":\"abandoned\",\"bits\":30}}");
    });
    fakehttp::Server next([&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/info") ++infos;
        return jsonReply(200, "{}");
    });
    CHECK(hard.ok());
    CHECK(next.ok());
    std::string credPath = tempCredentialPath("pow-switch");
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = hard.port();
    ep.insecureDev = true;
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        c.setServer(ep);
        c.login("alice", "pw");
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (logins.load() == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(logins.load(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the puzzle is being solved
        auto t0 = std::chrono::steady_clock::now();
        ep.apiPort = next.port();
        c.setServer(ep);
        c.fetchServerInfo();
        net::Event ev;
        CHECK(waitEvent(c, net::Event::Kind::LoginResult, ev, 3000));
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("cancelled"));
        CHECK(waitEvent(c, net::Event::Kind::ServerInfoResult, ev, 3000));
        CHECK_EQ(infos.load(), 1);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "  the next server answered %.0f ms after setServer()\n", ms);
        CHECK_EQ(logins.load(), 1);
    }
    net::sys::removeFile(credPath);
}

// The pin field emptied (Options applied) while a sign-in runs: that sign-in saves the pin of the
// endpoint it was given, so the pin is forgotten after it, not before.
TEST(net_forget_saved_pin_after_a_sign_in_under_way) {
    if (!net::transportAvailable()) return;
    std::atomic<int> logins{0};
    fakehttp::Server srv([&](const fakehttp::Request& q) {
        if (q.path != "/api/v1/auth/login") return jsonReply(404, "{\"error\":\"not_found\"}");
        ++logins;
        fakehttp::Reply rep = jsonReply(200, "{\"token\":\"" + kRigToken + "\",\"username\":\"alice\"}");
        rep.silenceMs = 300;
        return rep;
    });
    CHECK(srv.ok());
    std::string credPath = tempCredentialPath("forget-pin-late");
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = srv.port();
    ep.insecureDev = true;
    ep.pinnedSha256 = std::string(64, 'c');   // saved at sign-in (plain HTTP here: never checked)
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        c.setServer(ep);
        c.login("alice", "pw");
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (logins.load() == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(logins.load(), 1);
        ep.pinnedSha256.clear();
        c.setServer(ep);
        c.forgetSavedPin();
        net::Event ev;
        CHECK(waitEvent(c, net::Event::Kind::LoginResult, ev, 10000) && ev.ok);
        until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!net::CredentialStore(credPath).pin(ep.origin()).empty() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(net::CredentialStore(credPath).pin(ep.origin()).empty());
        CHECK(c.hasSavedSession());
    }
    net::sys::removeFile(credPath);
}

// The pin field emptied, then the game quits while a request still runs ahead of the clear on
// net-http: the pin is forgotten all the same (else the next start would use it again, and
// Options could no longer clear it). The session stays.
TEST(net_forget_saved_pin_at_exit) {
    if (!net::transportAvailable()) return;
    std::atomic<int> infos{0};
    fakehttp::Server srv([&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/info") ++infos;
        fakehttp::Reply rep = jsonReply(200, "{}");
        rep.silenceMs = 1500;
        return rep;
    });
    CHECK(srv.ok());
    std::string credPath = tempCredentialPath("forget-pin-exit");
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = srv.port();
    ep.insecureDev = true;
    {
        net::CredentialStore s(credPath);
        net::Credential cr;
        cr.origin = ep.origin();
        cr.username = "alice";
        cr.token = kRigToken;
        cr.pinnedSha256 = std::string(64, 'c');
        CHECK(s.put(cr));
    }
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        c.setServer(ep);
        c.fetchServerInfo();
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (infos.load() == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(infos.load(), 1);
        c.forgetSavedPin();
    }
    net::CredentialStore after(credPath);
    CHECK(after.pin(ep.origin()).empty());
    CHECK(after.hasToken(ep.origin()));
    net::sys::removeFile(credPath);
}

// ---- Google sign-in (loopback redirect, dedicated-server/docs/API.md) --------------------------------
// The scripted server plays start / finish / link / complete / login/mfa; the browser opener seam
// plays the browser and Google: it reads the redirect URI and the state of the Google page the
// client was given, and sends the redirect to the game's own listener on 127.0.0.1.

namespace {

const std::string kSsoAttempt = "sso_" + std::string(43, 'A');
const std::string kSsoState = "St4te_" + std::string(37, 's');
const std::string kSsoLinkTicket = "sso_" + std::string(43, 'L');
const std::string kSsoUserJson =
    R"({"id":7,"username":"alice","email":"a@example.org","emailVerified":true,"mfaEnabled":false,"googleLinked":true})";

std::string pctEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += char(ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}

// What a server answers to start: Google's page for the redirect URI on 'port' with 'tag'.
// 'tweak' edits the parameters before they are joined (a lookalike page, another redirect...).
std::string googleAuthUrl(uint16_t port, const std::string& tag, const std::string& state,
                          const std::function<void(std::vector<std::pair<std::string, std::string>>&)>& tweak = nullptr) {
    std::vector<std::pair<std::string, std::string>> q = {
        {"client_id", "1234-abc.apps.googleusercontent.com"},
        {"redirect_uri", "http://127.0.0.1:" + std::to_string(port) + "/oauth2/google/" + tag},
        {"response_type", "code"},
        {"scope", "openid email profile"},
        {"state", state},
        {"nonce", std::string(43, 'n')},
        {"code_challenge", std::string(43, 'G')},
        {"code_challenge_method", "S256"},
        {"prompt", "select_account"},
    };
    if (tweak) tweak(q);
    std::string url = "https://accounts.google.com/o/oauth2/v2/auth?";
    for (size_t i = 0; i < q.size(); ++i) url += (i ? "&" : "") + q[i].first + "=" + pctEncode(q[i].second);
    return url;
}

std::string authParam(const std::string& url, const std::string& name) {
    std::vector<std::pair<std::string, std::string>> q;
    size_t at = url.find('?');
    if (at == std::string::npos || !net::loopback::queryParams(url.substr(at + 1), q)) return std::string();
    for (auto& p : q)
        if (p.first == name) return p.second;
    return std::string();
}

uint16_t redirectPortOf(const std::string& redirectUri) {
    const std::string pre = "http://127.0.0.1:";
    if (redirectUri.compare(0, pre.size(), pre) != 0) return 0;
    return uint16_t(std::atoi(redirectUri.c_str() + pre.size()));
}

// One GET on 127.0.0.1:port, the whole answer ("" when the connection is refused).
std::string loopbackGet(uint16_t port, const std::string& target) {
    net::sock::Endpoint ep;
    std::string err;
    if (!net::sock::Endpoint::parse("127.0.0.1", port, ep)) return std::string();
    net::sock::Handle h = net::sock::connectWithTimeout(ep, 2000, err);
    if (h == net::sock::kInvalid) return std::string();
    std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nAccept: text/html\r\n\r\n";
    size_t off = 0;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string ans;
    while (std::chrono::steady_clock::now() < end) {
        if (off < req.size()) {
            int r = net::sock::sendSome(h, reinterpret_cast<const uint8_t*>(req.data()) + off, req.size() - off);
            if (r < 0) break;
            off += size_t(r);
        }
        net::sock::PollSet ps;
        ps.add(h, true, false);
        ps.wait(20);
        uint8_t buf[4096];
        bool closed = false;
        int r = net::sock::recvSome(h, buf, sizeof buf, closed);
        if (r > 0) ans.append(reinterpret_cast<char*>(buf), size_t(r));
        else if (r < 0) break;
    }
    net::sock::closeSocket(h);
    return ans;
}

// The browser after Google: the redirect of a Google page to the listener it names, with 'query'
// (its state added unless 'state' says otherwise).
std::string googleRedirect(const std::string& authUrl, const std::string& query, const char* state = nullptr) {
    const std::string uri = authParam(authUrl, "redirect_uri");
    const uint16_t port = redirectPortOf(uri);
    const std::string path = uri.substr(uri.find('/', 8));
    return loopbackGet(port, path + "?state=" + (state ? std::string(state) : authParam(authUrl, "state")) + "&" + query);
}

bool portRefused(uint16_t port) {
    net::sock::Endpoint ep;
    std::string err;
    if (!net::sock::Endpoint::parse("127.0.0.1", port, ep)) return false;
    net::sock::Handle h = net::sock::connectWithTimeout(ep, 1000, err);
    if (h == net::sock::kInvalid) return true;
    net::sock::closeSocket(h);
    return false;
}

// A client on a scripted server. The handlers answer each route ('start' by default: Google's page
// for the posted port, the tag of this server, kSsoState); 'browser' runs as the opener, on net-http.
struct SsoRig {
    std::mutex mu;
    std::function<fakehttp::Reply(const Value&)> start, finish, link, complete, mfa;
    std::string challenge;            // of the last start
    uint16_t port = 0;                // redirectPort of the last start
    std::vector<std::string> opened;  // the URLs handed to the opener
    std::function<bool(const std::string&)> browser;
    fakehttp::Server srv;
    std::string credPath;
    net::ServerEndpoint ep;
    std::unique_ptr<net::OnlineClient> c;

    explicit SsoRig(const char* tag)
        : srv([this](const fakehttp::Request& q) { return handle(q); }), credPath(tempCredentialPath(tag)) {
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port();
        ep.insecureDev = true;
        start = [this](const Value& b) {
            return jsonReply(200, "{\"attemptId\":\"" + kSsoAttempt + "\",\"authUrl\":\"" +
                                      googleAuthUrl(uint16_t(b["redirectPort"].asInt(0)), net::ssoOriginTag(ep.origin()), kSsoState) +
                                      "\",\"state\":\"" + kSsoState + "\",\"expiresIn\":600}");
        };
        c = std::make_unique<net::OnlineClient>();
        c->setCredentialsFile(credPath);
        c->setServer(ep);
        c->setBrowserOpener([this](const std::string& url) {
            std::function<bool(const std::string&)> b;
            {
                std::lock_guard<std::mutex> lk(mu);
                opened.push_back(url);
                b = browser;
            }
            return b ? b(url) : true;
        });
    }
    ~SsoRig() {
        c.reset();
        net::sys::removeFile(credPath);
    }

    fakehttp::Reply handle(const fakehttp::Request& q) {
        Value b = bodyOf(q);
        std::function<fakehttp::Reply(const Value&)> h;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (q.path == "/api/v1/auth/sso/google/start") {
                challenge = b["codeChallenge"].asString();
                port = uint16_t(b["redirectPort"].asInt(0));
                h = start;
            } else if (q.path == "/api/v1/auth/sso/google/finish") {
                h = finish;
            } else if (q.path == "/api/v1/auth/sso/google/link") {
                h = link;
            } else if (q.path == "/api/v1/auth/sso/complete") {
                h = complete;
            } else if (q.path == "/api/v1/auth/login/mfa") {
                h = mfa;
            }
        }
        return h ? h(b) : jsonReply(404, R"({"error":"not_found"})");
    }
    std::vector<fakehttp::Request> requests(const std::string& path) {
        std::vector<fakehttp::Request> out;
        for (auto& r : srv.requests())
            if (r.path == path) out.push_back(r);
        return out;
    }
    size_t openedCount() {
        std::lock_guard<std::mutex> lk(mu);
        return opened.size();
    }
    std::string lastOpened() {
        std::lock_guard<std::mutex> lk(mu);
        return opened.empty() ? std::string() : opened.back();
    }
};

fakehttp::Reply signedInReply() {
    return jsonReply(200, "{\"token\":\"" + kRigToken + "\",\"expiresAt\":1900000000000,\"user\":" + kSsoUserJson + "}");
}

std::vector<std::string> keysOf(const Value& v) {
    std::vector<std::string> k;
    for (const auto& m : v.members()) k.push_back(m.first);
    std::sort(k.begin(), k.end());
    return k;
}

}  // namespace

// Start: the body is exactly { codeChallenge, redirectPort } with the port of the listener bound
// before it; a start answer that is not Google's page for this listener, this server and this state
// opens nothing and closes the listener.
TEST(net_sso_start_body) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-start");
    REQUIRE(rig.srv.ok());
    struct Case {
        const char* what;
        std::function<std::string(uint16_t port, const std::string& tag)> answer;
        const char* error;
    };
    auto reply = [](const std::string& url, const std::string& state, const std::string& attempt = kSsoAttempt) {
        return "{\"attemptId\":\"" + attempt + "\",\"authUrl\":\"" + url + "\",\"state\":\"" + state + "\",\"expiresIn\":600}";
    };
    using Params = std::vector<std::pair<std::string, std::string>>;
    auto edit = [](const char* name, const char* value) {
        return [name, value](Params& q) {
            for (auto& p : q)
                if (p.first == name) p.second = value;
        };
    };
    auto drop = [](const char* name) {
        return [name](Params& q) { q.erase(std::remove_if(q.begin(), q.end(), [name](const auto& p) { return p.first == name; }), q.end()); };
    };
    const std::vector<Case> cases = {
        {"plain http", [&](uint16_t port, const std::string& tag) {
             std::string u = googleAuthUrl(port, tag, kSsoState);
             return reply("http://" + u.substr(8), kSsoState);
         }, "bad_response"},
        {"lookalike host", [&](uint16_t port, const std::string& tag) {
             std::string u = googleAuthUrl(port, tag, kSsoState);
             return reply("https://accounts.google.com.evil.example/o/oauth2/v2/auth?" + u.substr(u.find('?') + 1), kSsoState);
         }, "bad_response"},
        {"another redirect port", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(uint16_t(port == 65535 ? 1024 : port + 1), tag, kSsoState), kSsoState);
         }, "bad_response"},
        {"another redirect path", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, [&](Params& q) {
                              q[1].second = "http://127.0.0.1:" + std::to_string(port) + "/sso/google";
                          }),
                          kSsoState);
         }, "bad_response"},
        {"localhost redirect", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, [&](Params& q) {
                              q[1].second = "http://localhost:" + std::to_string(port) + "/oauth2/google/" + tag;
                          }),
                          kSsoState);
         }, "bad_response"},
        {"state mismatch", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState), "Other_" + std::string(37, 's'));
         }, "bad_response"},
        {"state missing", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, drop("state")), kSsoState);
         }, "bad_response"},
        {"two states", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, [](Params& q) { q.push_back({"state", kSsoState}); }), kSsoState);
         }, "bad_response"},
        {"no code_challenge_method", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, drop("code_challenge_method")), kSsoState);
         }, "bad_response"},
        {"plain challenge", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, edit("code_challenge_method", "plain")), kSsoState);
         }, "bad_response"},
        {"implicit flow", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState, edit("response_type", "token")), kSsoState);
         }, "bad_response"},
        {"short attempt id", [&](uint16_t port, const std::string& tag) {
             return reply(googleAuthUrl(port, tag, kSsoState), kSsoState, "sso_x");
         }, "bad_response"},
        {"another server's tag", [&](uint16_t port, const std::string&) {
             return reply(googleAuthUrl(port, net::ssoOriginTag("caissa.scacelith.com:443"), kSsoState), kSsoState);
         }, "sso_origin"},
    };
    for (const Case& k : cases) {
        std::fprintf(stderr, "  -- %s\n", k.what);
        {
            std::lock_guard<std::mutex> lk(rig.mu);
            rig.start = [&rig, &k](const Value& b) {
                return jsonReply(200, k.answer(uint16_t(b["redirectPort"].asInt(0)), net::ssoOriginTag(rig.ep.origin())));
            };
        }
        const size_t before = rig.srv.requests().size();
        rig.c->startGoogleSso(net::SsoBrowserPage());
        net::Event ev;
        CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string(k.error));
        CHECK_EQ(rig.openedCount(), size_t(0));
        REQUIRE(rig.srv.requests().size() == before + 1);
        const fakehttp::Request q = rig.srv.requests().back();
        CHECK_EQ(q.path, std::string("/api/v1/auth/sso/google/start"));
        Value b = bodyOf(q);
        CHECK(keysOf(b) == (std::vector<std::string>{"codeChallenge", "redirectPort"}));
        CHECK_EQ(b["codeChallenge"].asString().size(), size_t(43));
        const int64_t port = b["redirectPort"].asInt(0);
        CHECK(port >= 1024 && port <= 65535);
        CHECK(portRefused(uint16_t(port)));   // the listener is closed
    }
}

// A hostile community server relays the game's start to another server (the official one): that
// server's Google page names its own tag, so the game opens nothing.
TEST(net_sso_relay_refused) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig official("sso-official");
    REQUIRE(official.srv.ok());
    std::atomic<int> relayed{0};
    fakehttp::Server hostile([&](const fakehttp::Request& q) {
        if (q.path != "/api/v1/auth/sso/google/start") return jsonReply(404, R"({"error":"not_found"})");
        ++relayed;
        // The official server's answer, as it gives it to anyone who calls start with this body.
        return official.start(bodyOf(q));
    });
    REQUIRE(hostile.ok());
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = hostile.port();
    ep.insecureDev = true;
    std::string credPath = tempCredentialPath("sso-relay");
    RemovedAtEnd cleanup{credPath};
    std::atomic<int> opens{0};
    net::OnlineClient c;
    c.setCredentialsFile(credPath);
    c.setServer(ep);
    c.setBrowserOpener([&](const std::string&) {
        ++opens;
        return true;
    });
    c.startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK_EQ(relayed.load(), 1);
    CHECK_EQ(ev.error, std::string("sso_origin"));
    CHECK_EQ(opens.load(), 0);
}

// The code: SsoCodeReceived, then finish carries { attemptId, codeVerifier, state, code, iss,
// clientLabel } (S256 of the verifier is the start's challenge) to the server that answered start;
// a session signs in and is saved. The browser got the 'done' page, and the port is closed after it.
TEST(net_sso_finish_login) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-finish");
    REQUIRE(rig.srv.ok());
    std::string page;
    rig.finish = [](const Value&) { return signedInReply(); };
    rig.browser = [&](const std::string& url) {
        page = googleRedirect(url, "iss=https%3A%2F%2Faccounts.google.com&code=4%2F0AbCd-Ef&scope=email+profile+openid&authuser=0");
        return true;
    };
    net::SsoBrowserPage texts;
    texts.doneHeading = "Back to Scacelith (test)";
    rig.c->startGoogleSso(texts);
    net::Event ev;
    std::vector<net::Event> seen;
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000, &seen));
    CHECK(ev.ok);
    CHECK_EQ(ev.account.username, std::string("alice"));
    CHECK(rig.c->hasSavedSession());
    CHECK(page.find("<h1>Back to Scacelith (test)</h1>") != std::string::npos);
    bool opened = false, code = false;
    for (const net::Event& e : seen) {
        if (e.kind == net::Event::Kind::SsoBrowserOpened) opened = e.ok;
        if (e.kind == net::Event::Kind::SsoCodeReceived) code = opened;
    }
    CHECK(code);
    auto fin = rig.requests("/api/v1/auth/sso/google/finish");
    REQUIRE(fin.size() == 1);
    Value b = bodyOf(fin[0]);
    CHECK(keysOf(b) == (std::vector<std::string>{"attemptId", "clientLabel", "code", "codeVerifier", "iss", "state"}));
    CHECK_EQ(b["attemptId"].asString(), kSsoAttempt);
    CHECK_EQ(b["state"].asString(), kSsoState);
    CHECK_EQ(b["code"].asString(), std::string("4/0AbCd-Ef"));
    CHECK_EQ(b["iss"].asString(), std::string("https://accounts.google.com"));
    CHECK_EQ(net::crypto::pkceChallenge(b["codeVerifier"].asString()), rig.challenge);
    CHECK(b["clientLabel"].asString().compare(0, 10, "Scacelith/") == 0);
    CHECK(portRefused(rig.port));
}

// An existing link with two-factor: mfaRequired, then loginMfa() signs in. No iss in the redirect:
// none in finish.
TEST(net_sso_finish_mfa) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-mfa");
    REQUIRE(rig.srv.ok());
    const std::string mfaToken = "mfa_" + std::string(43, 'M');
    rig.finish = [&](const Value&) { return jsonReply(200, "{\"mfaRequired\":true,\"mfaToken\":\"" + mfaToken + "\",\"expiresIn\":300}"); };
    rig.mfa = [&](const Value& b) {
        if (b["mfaToken"].asString() != mfaToken || b["code"].asString() != "123456") return jsonReply(401, R"({"error":"invalid_code"})");
        return signedInReply();
    };
    rig.browser = [](const std::string& url) {
        googleRedirect(url, "code=4%2F0Mfa");
        return true;
    };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK(!ev.ok && ev.mfaRequired);
    auto fin = rig.requests("/api/v1/auth/sso/google/finish");
    REQUIRE(fin.size() == 1);
    CHECK(!bodyOf(fin[0]).has("iss"));
    rig.c->loginMfa("123456");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK(ev.ok);
    CHECK(rig.c->hasSavedSession());
}

// A new Google account: SsoNeedsUsername with the server's suggestion, then completeSso().
TEST(net_sso_needs_username) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-name");
    REQUIRE(rig.srv.ok());
    const std::string ticket = "sso_" + std::string(43, 'T');
    rig.finish = [&](const Value&) {
        return jsonReply(200, "{\"needsUsername\":true,\"ssoTicket\":\"" + ticket + "\",\"suggestedUsername\":\"alice_g\"}");
    };
    rig.complete = [&](const Value& b) {
        if (b["ssoTicket"].asString() != ticket || b["username"].asString() != "alice") return jsonReply(400, R"({"error":"invalid_request"})");
        return signedInReply();
    };
    rig.browser = [](const std::string& url) {
        googleRedirect(url, "code=4%2F0New");
        return true;
    };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoNeedsUsername, ev, 10000));
    CHECK_EQ(ev.account.username, std::string("alice_g"));
    rig.c->completeSso("alice");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK(ev.ok);
}

// The address of an account with a password: SsoNeedsPassword with its name, then linkSso(). A
// wrong password, or too many, keeps the step (the same ticket goes again), a proof of work is
// solved and the request repeated once, mfaRequired continues with loginMfa(); a 410 ends the step,
// and so does another server chosen at that step (nothing is sent to either server).
TEST(net_sso_link) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-link");
    REQUIRE(rig.srv.ok());
    const std::string mfaToken = "mfa_" + std::string(43, 'N');
    rig.finish = [&](const Value&) {
        return jsonReply(200, "{\"needsPassword\":true,\"linkTicket\":\"" + kSsoLinkTicket + "\",\"username\":\"alice\",\"expiresIn\":600}");
    };
    std::atomic<int> links{0};
    rig.link = [&](const Value& b) {
        const int n = ++links;
        if (b["linkTicket"].asString() != kSsoLinkTicket) return jsonReply(410, R"({"error":"sso_expired"})");
        if (n == 1) return jsonReply(401, R"({"error":"invalid_credentials","message":"Wrong password."})");
        if (n == 2) return jsonReply(429, R"({"error":"too_many_attempts","retryAfter":1})");
        if (n == 3) return jsonReply(428, R"({"error":"pow_required","pow":{"challenge":"sso-link-test","bits":4}})");
        if (!b["pow"].isObject() || b["password"].asString() != "right password") return jsonReply(400, R"({"error":"invalid_request"})");
        return jsonReply(200, "{\"mfaRequired\":true,\"mfaToken\":\"" + mfaToken + "\",\"expiresIn\":300}");
    };
    rig.mfa = [&](const Value& b) { return b["mfaToken"].asString() == mfaToken ? signedInReply() : jsonReply(410, R"({"error":"sso_expired"})"); };
    rig.browser = [](const std::string& url) {
        googleRedirect(url, "code=4%2F0Link");
        return true;
    };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoNeedsPassword, ev, 10000));
    CHECK(ev.ok);
    CHECK_EQ(ev.account.username, std::string("alice"));
    rig.c->linkSso("wrong password");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("invalid_credentials"));
    rig.c->linkSso("right password");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("too_many_attempts"));
    rig.c->linkSso("right password");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 15000));
    CHECK(ev.mfaRequired);
    CHECK_EQ(links.load(), 4);
    auto sent = rig.requests("/api/v1/auth/sso/google/link");
    REQUIRE(sent.size() == 4);
    for (auto& r : sent) {
        Value b = bodyOf(r);
        CHECK_EQ(b["linkTicket"].asString(), kSsoLinkTicket);
        CHECK(b["clientLabel"].isString());
    }
    CHECK(keysOf(bodyOf(sent[0])) == (std::vector<std::string>{"clientLabel", "linkTicket", "password"}));
    CHECK(bodyOf(sent[3])["pow"]["nonce"].isString());
    rig.c->loginMfa("654321");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK(ev.ok);

    // Another sign-in; the server says the step expired: it ends, and a second try sends nothing.
    rig.link = [](const Value&) { return jsonReply(410, R"({"error":"sso_expired"})"); };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoNeedsPassword, ev, 10000));
    rig.c->linkSso("right password");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("sso_expired"));
    const size_t sentBefore = rig.requests("/api/v1/auth/sso/google/link").size();
    rig.c->linkSso("right password");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("sso_expired"));
    CHECK_EQ(rig.requests("/api/v1/auth/sso/google/link").size(), sentBefore);

    // Another sign-in; the player switches servers at the password step ("localhost" reaches the same
    // fake server under another origin): the step ends there.
    rig.c->startGoogleSso(net::SsoBrowserPage());
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoNeedsPassword, ev, 10000));
    net::ServerEndpoint other = rig.ep;
    other.host = "localhost";
    rig.c->setServer(other);
    rig.c->linkSso("right password");
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("sso_expired"));
    CHECK_EQ(rig.requests("/api/v1/auth/sso/google/link").size(), sentBefore);
}

// Cancelled at Google (error=access_denied): sso_cancelled, the browser's 'cancelled' page, and no
// finish. Another error: sso_failed.
TEST(net_sso_provider_cancel) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-denied");
    REQUIRE(rig.srv.ok());
    std::string page;
    rig.browser = [&](const std::string& url) {
        page = googleRedirect(url, "error=access_denied");
        return true;
    };
    net::SsoBrowserPage texts;
    texts.cancelledHeading = "Cancelled (test)";
    rig.c->startGoogleSso(texts);
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("sso_cancelled"));
    CHECK(page.find("<h1>Cancelled (test)</h1>") != std::string::npos);
    rig.browser = [](const std::string& url) {
        googleRedirect(url, "error=server_error");
        return true;
    };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("sso_failed"));
    CHECK(rig.requests("/api/v1/auth/sso/google/finish").empty());
}

// The player cancels while Google's page is open: LoginResult "cancelled", the port closed (the
// redirect that comes later is refused); a link sent by someone else (another state) never ends
// the wait.
TEST(net_sso_cancel) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-cancel");
    REQUIRE(rig.srv.ok());
    rig.c->startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(ev.ok);
    const std::string url = rig.lastOpened();
    const std::string foreign = googleRedirect(url, "code=4%2F0Stranger", ("Other_" + std::string(37, 'x')).c_str());
    CHECK(foreign.compare(0, 12, "HTTP/1.1 400") == 0);
    CHECK(!waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 300));
    rig.c->cancelSso();
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("cancelled"));
    CHECK(portRefused(rig.port));
    CHECK(googleRedirect(url, "code=4%2F0Late").empty());
    CHECK(rig.requests("/api/v1/auth/sso/google/finish").empty());
    // Nothing under way: no answer.
    rig.c->cancelSso();
    CHECK(!waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 300));
}

// Cancelled while the server answers the start: no browser, the listener closed, the start
// answered "cancelled" and nothing else. The same when another server is chosen meanwhile (that
// Google page is for the previous one).
TEST(net_sso_cancel_during_start) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-cancel-start");
    REQUIRE(rig.srv.ok());
    std::mutex m;
    std::condition_variable cv;
    bool asked = false, release = false;
    rig.start = [&](const Value& b) {
        {
            std::unique_lock<std::mutex> lk(m);
            asked = true;
            cv.notify_all();
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return release; });
        }
        return jsonReply(200, "{\"attemptId\":\"" + kSsoAttempt + "\",\"authUrl\":\"" +
                                  googleAuthUrl(uint16_t(b["redirectPort"].asInt(0)), net::ssoOriginTag(rig.ep.origin()), kSsoState) +
                                  "\",\"state\":\"" + kSsoState + "\",\"expiresIn\":600}");
    };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return asked; }));
    }
    rig.c->cancelSso();
    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("cancelled"));
    CHECK_EQ(rig.openedCount(), size_t(0));
    CHECK(portRefused(rig.port));
    CHECK(!waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 300));   // the cancel finds nothing under way
    // The next sign-in is not affected.
    rig.c->startGoogleSso(net::SsoBrowserPage());
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(ev.ok);
    CHECK_EQ(rig.openedCount(), size_t(1));
    rig.c->cancelSso();
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));

    {
        std::lock_guard<std::mutex> lk(m);
        asked = release = false;
    }
    rig.c->startGoogleSso(net::SsoBrowserPage());
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return asked; }));
    }
    net::ServerEndpoint other = rig.ep;
    other.host = "localhost";
    rig.c->setServer(other);
    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("cancelled"));
    CHECK_EQ(rig.openedCount(), size_t(1));
    CHECK(portRefused(rig.port));
}

// Another server chosen while Google's page is open: the listener stops, and a code that reached it
// just before is never sent (to either server).
TEST(net_sso_origin_change) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-origin");
    REQUIRE(rig.srv.ok());
    rig.finish = [](const Value&) { return signedInReply(); };
    std::mutex m;
    std::condition_variable cv;
    bool switched = false;
    // The code arrives while net-http still runs the start; the game switches servers before it runs.
    rig.browser = [&](const std::string& url) {
        googleRedirect(url, "code=4%2F0Meanwhile");
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return switched; });
        return true;
    };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rig.openedCount() == 0 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    net::ServerEndpoint other = rig.ep;
    other.host = "localhost";
    rig.c->setServer(other);
    {
        std::lock_guard<std::mutex> lk(m);
        switched = true;
    }
    cv.notify_all();
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(!waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 500));
    CHECK(rig.requests("/api/v1/auth/sso/google/finish").empty());
    CHECK(portRefused(rig.port));

    // The same with the page open and nothing received yet: the port closes with the switch.
    rig.c->setServer(rig.ep);
    rig.browser = nullptr;
    rig.c->startGoogleSso(net::SsoBrowserPage());
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(ev.ok);
    const uint16_t port = rig.port;
    CHECK(!portRefused(port));
    rig.c->setServer(other);
    until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!portRefused(port) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(portRefused(port));
    CHECK(rig.requests("/api/v1/auth/sso/google/finish").empty());
}

// Nothing comes back from Google before the attempt's time is up: sso_expired, the port closed.
// (The shortest wait is 30 s; the test hook shortens it.)
TEST(net_sso_expiry) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-expiry");
    REQUIRE(rig.srv.ok());
    rig.start = [&rig](const Value& b) {
        return jsonReply(200, "{\"attemptId\":\"" + kSsoAttempt + "\",\"authUrl\":\"" +
                                  googleAuthUrl(uint16_t(b["redirectPort"].asInt(0)), net::ssoOriginTag(rig.ep.origin()), kSsoState) +
                                  "\",\"state\":\"" + kSsoState + "\",\"expiresIn\":0}");
    };
    rig.c->setSsoMinWaitMs(300);
    rig.c->startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK(ev.ok);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(waitEvent(*rig.c, net::Event::Kind::LoginResult, ev, 10000));
    CHECK_EQ(ev.error, std::string("sso_expired"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    CHECK(portRefused(rig.port));
}

// The browser could not be opened: "browser", the listener closed.
TEST(net_sso_browser_fails) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    SsoRig rig("sso-nobrowser");
    REQUIRE(rig.srv.ok());
    rig.browser = [](const std::string&) { return false; };
    rig.c->startGoogleSso(net::SsoBrowserPage());
    net::Event ev;
    CHECK(waitEvent(*rig.c, net::Event::Kind::SsoBrowserOpened, ev, 10000));
    CHECK_EQ(ev.error, std::string("browser"));
    CHECK(portRefused(rig.port));
}

// The official server moved from port 44664 to 443: a saved session moves with it, once; other
// origins never move.
TEST(net_credentials_origin_move) {
    const std::string oldO = "play.example.org:44664", newO = "play.example.org:443", other = "chess.example.net:44664";
    const std::string token = "sct_" + std::string(43, 'M'), token2 = "sct_" + std::string(43, 'N');
    std::string path = tempCredentialPath("move");
    {
        net::CredentialStore s(path);
        net::Credential c;
        c.origin = oldO;
        c.username = "alice";
        c.token = token;
        c.serverId = "srv-official";
        c.pinnedSha256 = std::string(64, 'c');
        CHECK(s.put(c));
        c.origin = other;
        c.username = "bob";
        c.token = token2;
        CHECK(s.put(c));
    }
    {
        net::CredentialStore s(path);
        s.addOriginMove(oldO, newO);
        net::Credential out;
        CHECK(s.get(newO, out));
        CHECK_EQ(out.username, std::string("alice"));
        CHECK_EQ(out.token, token);                 // re-protected for its new origin
        CHECK_EQ(out.serverId, std::string("srv-official"));
        CHECK_EQ(out.pinnedSha256, std::string(64, 'c'));
        CHECK(!s.get(oldO, out));
        CHECK(s.get(other, out));
        CHECK_EQ(out.token, token2);
    }
    {
        net::CredentialStore s(path);               // saved: no move rule needed any more
        net::Credential out;
        CHECK(s.get(newO, out));
        CHECK_EQ(out.token, token);
        CHECK_EQ(s.origins().size(), size_t(2));
        // A record for the new origin already there: nothing moves (the old one stays as it is).
        net::Credential c;
        c.origin = oldO;
        c.username = "carol";
        c.token = token2;
        CHECK(s.put(c));
    }
    {
        net::CredentialStore s(path);
        net::Credential out;
        CHECK(s.get(oldO, out));                    // loaded before the rule: applied at once
        s.addOriginMove(oldO, newO);
        CHECK(s.get(newO, out));
        CHECK_EQ(out.username, std::string("alice"));
        CHECK(s.get(oldO, out));
        CHECK_EQ(out.username, std::string("carol"));
    }
    net::sys::removeFile(path);

    // Through the client: this build's official server, when it is not on 44664.
    net::ServerEndpoint off = net::officialServer();
    if (!off.valid() || off.apiPort == 44664) return;
    net::ServerEndpoint legacy = off;
    legacy.apiPort = 44664;
    path = tempCredentialPath("move-official");
    {
        net::CredentialStore s(path);
        net::Credential c;
        c.origin = legacy.origin();
        c.username = "alice";
        c.token = token;
        CHECK(s.put(c));
        c.origin = other;                           // a community server on the former port
        c.username = "bob";
        CHECK(s.put(c));
    }
    {
        net::OnlineClient c;
        c.setCredentialsFile(path);
        c.setServer(off);
        CHECK(c.hasSavedSession());
        CHECK_EQ(c.savedUsername(), std::string("alice"));
    }
    {
        net::CredentialStore s(path);
        net::Credential out;
        CHECK(!s.get(legacy.origin(), out));
        CHECK(s.get(off.origin(), out));
        CHECK_EQ(out.token, token);
        CHECK(s.get(other, out));                   // never moved
        CHECK_EQ(out.username, std::string("bob"));
    }
    net::sys::removeFile(path);
}

// TLS certificate rules against a real TLS server, opt-in because the test cannot start one on
// every platform by itself. Needs a self-signed certificate for localhost and 127.0.0.1:
//   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 -subj /CN=localhost
//       -addext subjectAltName=DNS:localhost,IP:127.0.0.1 -keyout k.pem -out c.pem
// and an HTTPS server on 127.0.0.1:PORT with it that answers GET with a complete response
// (Content-Length) and logs the request lines. Then:
//   SCACELITH_NET_TLS_TEST=PORT:<hex SHA-256 of the DER certificate> ./scacelith_tests net_tls
// The server log must hold no request with "pin=<wrong prefix>" (printed by the test): nothing is
// sent to a server whose certificate differs from the pin.
TEST(net_tls_pinning_manual) {
    const char* env = std::getenv("SCACELITH_NET_TLS_TEST");
    if (!env) SKIP("SCACELITH_NET_TLS_TEST not set");
    REQUIRE(net::transportAvailable());  // asked for, so it must not pass without running
    std::string spec = env;
    size_t colon = spec.find(':');
    CHECK(colon != std::string::npos);
    if (colon == std::string::npos) return;
    uint16_t port = uint16_t(std::atoi(spec.substr(0, colon).c_str()));
    std::string pin = spec.substr(colon + 1);
    std::string wrong = pin;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    const bool wine = runningUnderWine();
    // Wine's WinHTTP reports certificate failures as ERROR_WINHTTP_SECURE_CHANNEL_ERROR without
    // the SECURE_FAILURE callback (Windows: ERROR_WINHTTP_SECURE_FAILURE + flags).
    auto certError = [&](const std::string& e) { return e == "certificate" || (wine && e == "tls"); };

    auto get = [&](const std::string& host, const std::string& pinHex, net::HttpResponse& resp) {
        net::HttpRequest r;
        r.host = host;
        r.port = port;
        r.pinnedSha256 = pinHex;
        r.path = "/api/v1/info?pin=" + (pinHex.empty() ? std::string("none") : pinHex.substr(0, 8));
        r.headers.emplace_back("Authorization", "Bearer tls-secret-token");
        r.timeoutMs = 3000;
        resp = net::HttpResponse();
        net::httpRequest(r, resp);
        std::fprintf(stderr, "  %s pin=%s -> status %d error '%s' (%s)\n", host.c_str(),
                     pinHex.empty() ? "none" : pinHex.substr(0, 8).c_str(), resp.status, resp.error.c_str(), resp.detail.c_str());
    };
    net::HttpResponse resp;
    for (const char* host : {"127.0.0.1", "localhost"}) {
        get(host, "", resp);                          // self-signed: refused by the trust store
        CHECK_EQ(resp.status, 0);
        CHECK(certError(resp.error));
        get(host, wrong, resp);                       // wrong pin: refused before sending anything
        CHECK_EQ(resp.status, 0);
        CHECK(certError(resp.error));
        get(host, pin, resp);                         // pinned: accepted
        if (wine && std::string(host) == "127.0.0.1") {
            // Wine's crypt32 does not match iPAddress subjectAltName entries (Windows does).
            std::fprintf(stderr, "  (Wine: host name check of an IP literal not supported, not counted)\n");
            continue;
        }
        CHECK_EQ(resp.status, 200);
        CHECK(resp.error.empty());
        CHECK_EQ(resp.body, std::string("{\"ok\":true}"));
    }
    std::fprintf(stderr, "  the server log must not contain pin=%s\n", wrong.substr(0, 8).c_str());

    net::WsParams w;
    w.host = "localhost";
    w.port = port;
    w.subprotocol = pr::kWsSubprotocol;
    w.timeoutMs = 3000;
    std::string err;
    int status = 0;
    w.path = "/ws?pin=" + wrong.substr(0, 8);
    w.pinnedSha256 = wrong;
    CHECK(net::wsConnect(w, err, status) == nullptr);
    CHECK(certError(err));
    w.path = "/ws?pin=" + pin.substr(0, 8);
    w.pinnedSha256 = pin;                             // TLS accepted; the test server does not upgrade
    CHECK(net::wsConnect(w, err, status) == nullptr);
    std::fprintf(stderr, "  wss pinned -> error '%s' status %d\n", err.c_str(), status);
    CHECK_EQ(err, std::string("http_200"));
}

// =============================================================================================
// Live check against a real dedicated server (opt-in). dedicated-server/tools/live-cpp-check.js
// starts a server (self-signed certificate, HTTPS API and WSS on one port, proof of work for
// registration), a Node bot queued in 3+2, then runs:
//   SCACELITH_NET_LIVE=host:port:<pin hex>:<username>:<password> ./scacelith_tests net_live_server_game
// This client registers, logs in, connects, queues rated 3+2, plays legal moves for 12 plies
// (posHash from its own chess::Position FEN) and resigns; the result and the rating update must
// come back from the server. The account API has its own live check
// (tests/net_live_account_tests.cpp).
// =============================================================================================
TEST(net_live_server_game) {
    const char* env = std::getenv("SCACELITH_NET_LIVE");
    if (!env) SKIP("SCACELITH_NET_LIVE not set");
    REQUIRE(net::transportAvailable());  // asked for, so it must not pass without running
    std::vector<std::string> f;
    {
        std::string s = env, cur;
        for (char ch : s) { if (ch == ':') { f.push_back(cur); cur.clear(); } else cur += ch; }
        f.push_back(cur);
    }
    CHECK_EQ(int(f.size()), 5);
    if (f.size() != 5) return;
    char credPath[256];
    std::snprintf(credPath, sizeof credPath, "scacelith-live-%d.credentials", int(std::time(nullptr) % 100000));
    net::OnlineClient c;
    c.setCredentialsFile(credPath);
    net::ServerEndpoint ep;
    ep.host = f[0];
    ep.apiPort = uint16_t(std::atoi(f[1].c_str()));
    ep.wsPort = 0;                                  // same port as the API
    ep.pinnedSha256 = f[2];
    c.setServer(ep);
    const std::string user = f[3], pass = f[4];
    std::vector<net::Event> seen;
    net::Event ev;

    c.registerAccount(user, user + "@example.org", pass);
    CHECK(waitEvent(c, net::Event::Kind::RegisterResult, ev, 30000, &seen));
    std::fprintf(stderr, "  register: ok=%d error='%s'\n", int(ev.ok), ev.error.c_str());
    CHECK(ev.ok);
    c.login(user, pass);
    CHECK(waitEvent(c, net::Event::Kind::LoginResult, ev, 15000, &seen));
    std::fprintf(stderr, "  login: ok=%d error='%s' user=%s\n", int(ev.ok), ev.error.c_str(), ev.account.username.c_str());
    CHECK(ev.ok);
    if (!ev.ok) { std::remove(credPath); return; }
    c.connect();
    CHECK(waitEvent(c, net::Event::Kind::Welcome, ev, 15000, &seen));
    std::fprintf(stderr, "  welcome from '%s' as %s\n", ev.serverName.c_str(), ev.account.username.c_str());
    c.joinQueue("3+2", true);
    CHECK(waitEvent(c, net::Event::Kind::GameSnapshot, ev, 20000, &seen));
    const uint64_t gameId = ev.game.id;
    const int you = ev.game.you;
    std::fprintf(stderr, "  game %llu, playing %s against %s\n", (unsigned long long)gameId, you == 0 ? "White" : "Black",
                 (you == 0 ? ev.game.black.name : ev.game.white.name).c_str());
    CHECK(gameId != 0);

    // Plays until 12 plies were made, then resigns on its turn.
    chess::Game mirror;
    size_t applied = 0;
    auto sync = [&](const net::OnlineGame& g) {
        for (; applied < g.moves.size(); ++applied) {
            uint16_t m = g.moves[applied].move;
            chess::Move mv = mirror.position().findLegal(chess::Square(net::moveFrom(m)), chess::Square(net::moveTo(m)),
                                                          chess::PieceType(net::movePromo(m)));
            CHECK(mirror.play(mv));
        }
    };
    sync(ev.game);
    int sent = -1, confirmed = 0;
    bool ended = false, resigned = false;
    net::Event end;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!ended && std::chrono::steady_clock::now() < deadline) {
        net::Event e;
        while (c.poll(e)) {
            if (e.kind == net::Event::Kind::MoveMade || e.kind == net::Event::Kind::GameSnapshot) {
                sync(e.game);
                if (e.kind == net::Event::Kind::MoveMade && e.mine) ++confirmed;
            } else if (e.kind == net::Event::Kind::MoveRejected) {
                std::fprintf(stderr, "  move rejected: ply %d code %d\n", e.ply, e.code);
                CHECK(false);
            } else if (e.kind == net::Event::Kind::GameEnd) {
                ended = true;
                end = e;
            } else if (e.kind == net::Event::Kind::ServerError) {
                std::fprintf(stderr, "  server error %d '%s'\n", e.code, e.error.c_str());
            }
        }
        int ply = int(mirror.moves().size());
        if (!ended && ply % 2 == you && sent < ply) {
            if (ply >= 12 && !resigned) {
                c.resign(gameId);
                resigned = true;
                sent = ply;
            } else if (ply < 12) {
                std::vector<chess::Move> legal = mirror.position().legalMoves();
                CHECK(!legal.empty());
                if (legal.empty()) break;
                const chess::Move& mv = legal[size_t(ply * 7) % legal.size()];
                c.sendMove(gameId, ply, net::packMove(mv.from, mv.to, mv.promotion), mirror.position().fen(), 300, false);
                sent = ply;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "  plies %zu, own moves confirmed %d, ended %d (status %d reason %d)\n", mirror.moves().size(), confirmed,
                 int(ended), end.game.status, end.game.reason);
    CHECK(ended);
    CHECK(confirmed >= 6);
    CHECK_EQ(end.game.reason, int(net::proto::EndReason::Resignation));
    CHECK_EQ(end.game.status, you == 0 ? int(net::proto::GameStatus::BlackWins) : int(net::proto::GameStatus::WhiteWins));
    CHECK(waitEvent(c, net::Event::Kind::RatingUpdate, ev, 10000, &seen));
    const net::Event::Rating& mine = you == 0 ? ev.ratingWhite : ev.ratingBlack;
    std::fprintf(stderr, "  rating %d -> %d (games %d)\n", mine.before, mine.after, mine.games);
    // A new account's loss before its first draw or win counts for neither player's rating
    // (dedicated-server/docs/DESIGN.md, ratings, "Zero score"): the update comes, the game is
    // counted, the rating stays.
    CHECK_EQ(mine.games, 1);
    CHECK_EQ(mine.after, mine.before);
    c.logout();
    waitEvent(c, net::Event::Kind::LogoutResult, ev, 5000, &seen);
    c.disconnect();
    std::remove(credPath);
}
