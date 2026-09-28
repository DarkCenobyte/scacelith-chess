// Online client tests: protocol codec against the JavaScript codec's vectors, position digest,
// JSON, crypto (hash / base64 / PKCE / proof of work), credential store isolation, endpoint
// validation, and OnlineClient end to end against a fake server on the loopback interface
// (plain HTTP + WebSocket, the insecureDev mode): login with a proof of work, account, Hello /
// Welcome, ping and clock offset, queue, moves, reconnection, 4003 and logout.
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
#include <unistd.h>
#endif

#include "test.h"
#include "chess/chess.h"
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace pr = net::proto;
using net::json::Value;

namespace {

// ---- files ----

std::string readRepoFile(const std::string& rel, std::string* foundAt = nullptr) {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("SCACELITH_SOURCE_DIR")) roots.push_back(std::string(env) + "/");
    roots.push_back("");
    roots.push_back("../");
    roots.push_back("../../");
    std::string exe = net::sys::exeDirectory();
    roots.push_back(exe + "../");
    roots.push_back(exe + "../../");
    for (auto& r : roots) {
        std::string text;
        if (net::sys::readFile(r + rel, text, 64 << 20)) {
            if (foundAt) *foundAt = r + rel;
            return text;
        }
    }
    return std::string();
}

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
    CHECK_EQ(pr::kProtocolVersion, 1);
    CHECK_EQ(std::string(pr::kWsSubprotocol), std::string("scacelith.v1"));
    CHECK_EQ(int(pr::MsgType::Move), 0x20);
    CHECK_EQ(int(pr::MsgType::C_Ping), 0x02);
    CHECK_EQ(int(pr::MsgType::S_Ping), 0x82);
    CHECK_EQ(std::string(pr::messageName(pr::MsgType::S_Pong)), std::string("S_Pong"));
    CHECK(pr::messageName(pr::MsgType(0x7F)) == nullptr);
    CHECK(pr::isValid(pr::EndReason::BothDisconnected));
    CHECK(!pr::isValid(pr::EndReason(14)));
    CHECK_EQ(std::string(pr::enumName(pr::ErrorCode::IllegalMove)), std::string("IllegalMove"));
    CHECK_EQ(pr::CloseCode::Unauthorized, 4003);
}

TEST(net_protocol_vectors) {
    std::string path;
    std::string text = readRepoFile("tests/data/net-protocol-vectors.json", &path);
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
    std::string text = readRepoFile("dedicated-server/test/fixtures/protocol-vectors.json", &path);
    if (text.empty()) {
        std::fprintf(stderr, "  (dedicated-server/test/fixtures/protocol-vectors.json not present: skipped)\n");
        return;
    }
    Value doc;
    net::json::Limits lim;
    lim.maxBytes = 64 << 20;
    lim.maxElements = 10000000;
    CHECK(net::json::parse(text, doc, nullptr, lim));
    for (const char* k : {"schemaHash", "schema_hash", "SCHEMA_HASH"})
        if (doc[k].isNumber() && uint32_t(doc[k].asInt()) != pr::kSchemaHash) {
            std::fprintf(stderr, "  shared fixture has another schema hash: skipped\n");
            return;
        }
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
// Credential store
// =============================================================================================

namespace {
std::string tempCredentialPath(const char* tag) {
    std::string p = net::sys::exeDirectory() + "net-test-" + tag + ".credentials";
    net::sys::removeFile(p);
    return p;
}
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
    // The official server: wss://caissa.scacelith.com:44664/ws, same port for the API.
    net::ServerEndpoint off = net::officialServer();
#ifndef SCACELITH_OFFICIAL_SERVER
#define SCACELITH_OFFICIAL_SERVER ""
#endif
    if (std::string(SCACELITH_OFFICIAL_SERVER).empty()) {
        CHECK_EQ(off.host, std::string("caissa.scacelith.com"));
        CHECK_EQ(off.apiPort, uint16_t(44664));
        CHECK_EQ(off.wsPort, uint16_t(44664));
        CHECK(off.pinnedSha256.empty());
        CHECK(!off.insecureDev);
        CHECK(off.valid());
        CHECK_EQ(off.origin(), std::string("caissa.scacelith.com:44664"));
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
    const std::string powChallenge = "fake-challenge-0123456789abcdef";
    static constexpr double kSkewMs = 5000;         // the server clock runs 5 s ahead
    std::atomic<int> loginAttempts{0}, powAccepted{0}, hellos{0}, pings{0}, moves{0}, logouts{0};
    std::atomic<bool> helloTokenOk{false};
    std::atomic<uint64_t> activeGame{0};

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

private:
    Sock ls_ = kBadSock;
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::vector<Sock> open_, ws_;
    std::atomic<bool> stop_{false};
    std::mutex gameMu_;
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

    static void sendFrame(Sock s, int opcode, const uint8_t* data, size_t n) {
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
            char info[512];
            std::snprintf(info, sizeof(info),
                          "{\"name\":\"Fake\",\"serverId\":\"srv-1\",\"motd\":\"hi\",\"protocol\":{\"min\":1,\"max\":1,\"schema\":%u,"
                          "\"subprotocol\":\"scacelith.v1\"},\"wsPort\":%u,\"wsPath\":\"/ws\",\"registration\":\"open\","
                          "\"emailVerification\":true,\"sso\":{\"google\":false},\"mfa\":true,\"pow\":{\"register\":10},"
                          "\"categories\":[{\"id\":\"3+2\",\"baseSec\":180,\"incSec\":2}]}",
                          unsigned(pr::kSchemaHash), unsigned(port));
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
                respond(s, 200, "{\"token\":\"" + token + "\",\"expiresAt\":1,\"user\":{\"id\":7,\"username\":\"alice\","
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
        } else {
            respond(s, 404, "{\"error\":\"not_found\"}");
        }
    }

    void websocket(Sock s, std::map<std::string, std::string>& h, std::string in) {
        std::string key = h["sec-websocket-key"];
        std::string proto = h["sec-websocket-protocol"];
        if (proto.find(pr::kWsSubprotocol) == std::string::npos || !h.count("sec-websocket-version")) {
            respond(s, 400, "{}");
            return;
        }
        std::string k = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        net::crypto::Sha1 d = net::crypto::sha1(k.data(), k.size());
        std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                        net::crypto::base64(d.data(), d.size()) + "\r\nSec-WebSocket-Protocol: " + pr::kWsSubprotocol + "\r\n\r\n";
        sendAll(s, r.data(), r.size());
        {
            std::lock_guard<std::mutex> lk(mu_);
            ws_.push_back(s);
        }
        std::atomic<uint32_t>& gseq = gseq_;   // game sequence, kept across connections
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
            if (t == pr::MsgType::Hello) {
                pr::Hello m;
                if (!pr::decode(p, n, m)) return;
                ++hellos;
                helloTokenOk.store(m.seq == 1 && m.token == token && m.schema == pr::kSchemaHash && m.proto == 1 &&
                                   m.client.compare(0, 10, "Scacelith/") == 0);
                pr::Welcome w;
                w.proto = 1;
                w.serverTime = epochMs() + kSkewMs;
                w.userId = 7;
                w.username = "alice";
                w.serverName = "Fake";
                w.heartbeatMs = 15000;
                w.maxMsgPerSec = 20;
                w.activeGame = activeGame.load();
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
    if (!net::transportAvailable()) {
        std::fprintf(stderr, "  (no transport in this build: skipped)\n");
        return;
    }
    FakeServer srv;
    CHECK(srv.start());
    std::string credPath = tempCredentialPath("client");
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
        CHECK(c.currentGame() == nullptr);

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
        CHECK(c.currentGame() && c.currentGame()->id == 77);
        CHECK_EQ(c.currentGame()->black.name, std::string("bob"));
        CHECK_EQ(c.currentGame()->you, 0);
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
        CHECK_EQ(c.currentGame()->moves.size(), size_t(2));
        CHECK_EQ(c.currentGame()->drawOfferBy, 1);
        // A move with a wrong position digest is refused.
        c.sendMove(77, 2, net::packMove(6, 21, 0), "8/8/8/8/8/8/8/K6k w - - 0 1", 100, false);
        CHECK(waitEvent(c, K::MoveRejected, ev, 5000));
        CHECK_EQ(ev.code, int(pr::ErrorCode::Desync));

        // Network failure: reconnects by itself, the game stays until a new snapshot.
        int hellosBefore = srv.hellos.load();
        srv.dropWebSockets();
        CHECK(waitState(c, net::ConnState::Reconnecting, 5000));
        CHECK(c.currentGame() && c.currentGame()->id == 77);
        CHECK(waitEvent(c, K::GameSnapshot, ev, 10000));   // Welcome.activeGame, then the snapshot
        CHECK(c.state() == net::ConnState::Online);
        CHECK_EQ(srv.hellos.load(), hellosBefore + 1);
        CHECK_EQ(ev.game.moves.size(), size_t(2));
        CHECK_EQ(ev.game.running, 0);
        CHECK_EQ(c.currentGame()->drawOfferBy, 2);

        c.resign(77);
        CHECK(waitEvent(c, K::GameEnd, ev, 5000));
        CHECK_EQ(c.currentGame()->status, int(pr::GameStatus::BlackWins));
        CHECK_EQ(c.currentGame()->reason, int(pr::EndReason::Resignation));

        // Banned: the Notice's end of ban comes with the state; no retry.
        srv.kick(pr::CloseCode::Banned, true);
        net::Event st;
        CHECK(waitEvent(c, K::ConnectionChanged, st, 5000, nullptr, [](const net::Event& e) { return e.state == net::ConnState::Banned; }));
        CHECK_EQ(st.noticeArg, 4102444800000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        CHECK(c.state() == net::ConnState::Banned);

        // Token refused (4003): Unauthorized, the saved session is dropped.
        c.connect();
        CHECK(waitState(c, net::ConnState::Online, 10000));
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
        CHECK(c.currentGame() == nullptr);
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
    net::sys::removeFile(credPath);
}

TEST(net_online_client_unreachable) {
    if (!net::transportAvailable()) return;
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

// TLS certificate rules against a real TLS server, opt-in because the test cannot start one on
// every platform by itself. Needs a self-signed certificate for localhost and 127.0.0.1:
//   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 -subj /CN=localhost
//       -addext subjectAltName=DNS:localhost,IP:127.0.0.1 -keyout k.pem -out c.pem
// and an HTTPS server on 127.0.0.1:PORT with it that answers GET with a complete response
// (Content-Length) and logs the request lines. Then:
//   SCACELITH_NET_TLS_TEST=PORT:<hex SHA-256 of the DER certificate> ./scacelith_tests net_tls
// The server log must hold no request with "pin=<wrong prefix>" (printed by the test): nothing is
// sent to a server whose certificate differs from the pin.
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

TEST(net_tls_pinning_manual) {
    const char* env = std::getenv("SCACELITH_NET_TLS_TEST");
    if (!env || !net::transportAvailable()) {
        std::fprintf(stderr, "  (SCACELITH_NET_TLS_TEST not set: skipped)\n");
        return;
    }
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
//   SCACELITH_NET_LIVE=host:port:<pin hex>:<username>:<password> ./scacelith_tests net_live
// This client registers, logs in, connects, queues rated 3+2, plays legal moves for 12 plies
// (posHash from its own chess::Position FEN) and resigns; the result and the rating change must
// come back from the server.
// =============================================================================================
TEST(net_live_server_game) {
    const char* env = std::getenv("SCACELITH_NET_LIVE");
    if (!env || !net::transportAvailable()) {
        std::fprintf(stderr, "  (SCACELITH_NET_LIVE not set: skipped)\n");
        return;
    }
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
    CHECK(mine.after < mine.before);
    c.logout();
    waitEvent(c, net::Event::Kind::LogoutResult, ev, 5000, &seen);
    c.disconnect();
    std::remove(credPath);
}
