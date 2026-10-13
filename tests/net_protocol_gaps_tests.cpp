// The parts of the protocol codec, the JSON reader and writer and the crypto helpers that
// tests/net_tests.cpp leaves out: every enum value's name and membership (and the "?" of a value a
// later version adds), the close codes of the fatal errors, the checks of a rating update; JSON
// strings with every escape both ways, malformed escapes, UTF-8 and numbers; base64 with the
// standard alphabet's '+' and '/', and the streamed SHA-256.
#include "test.h"
#include "net/crypto.h"
#include "net/json.h"
#include "net/protocol_gen.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pr = net::proto;

namespace {

// Every member of E with its name, then values that are not members.
template <class E> void checkEnum(const std::vector<std::pair<E, const char*>>& members, std::initializer_list<int> outside) {
    for (const auto& m : members) {
        if (!pr::isValid(m.first) || std::strcmp(pr::enumName(m.first), m.second) != 0)
            std::fprintf(stderr, "  enum value %d: '%s', expected '%s'\n", int(m.first), pr::enumName(m.first), m.second);
        CHECK(pr::isValid(m.first));
        CHECK_EQ(std::string(pr::enumName(m.first)), std::string(m.second));
    }
    for (int v : outside) {
        CHECK(!pr::isValid(E(v)));
        CHECK_EQ(std::string(pr::enumName(E(v))), std::string("?"));
    }
}

}  // namespace

TEST(net_protocol_gaps_enum_names) {
    using C = pr::Color;
    checkEnum<C>({{C::White, "White"}, {C::Black, "Black"}, {C::None, "None"}}, {3, 200});
    using CP = pr::ColorPref;
    checkEnum<CP>({{CP::Random, "Random"}, {CP::White, "White"}, {CP::Black, "Black"}}, {3, 255});
    using GS = pr::GameStatus;
    checkEnum<GS>({{GS::Ongoing, "Ongoing"}, {GS::WhiteWins, "WhiteWins"}, {GS::BlackWins, "BlackWins"}, {GS::Draw, "Draw"},
                   {GS::Aborted, "Aborted"}},
                  {5, 99});
    using R = pr::EndReason;
    checkEnum<R>({{R::None, "None"},
                  {R::Checkmate, "Checkmate"},
                  {R::Resignation, "Resignation"},
                  {R::Timeout, "Timeout"},
                  {R::IllegalMoves, "IllegalMoves"},
                  {R::Stalemate, "Stalemate"},
                  {R::InsufficientMaterial, "InsufficientMaterial"},
                  {R::TimeoutVsInsufficient, "TimeoutVsInsufficient"},
                  {R::FivefoldRepetition, "FivefoldRepetition"},
                  {R::SeventyFiveMoves, "SeventyFiveMoves"},
                  {R::ThreefoldClaim, "ThreefoldClaim"},
                  {R::FiftyMoveClaim, "FiftyMoveClaim"},
                  {R::Agreement, "Agreement"},
                  {R::IllegalMovesVsInsufficient, "IllegalMovesVsInsufficient"},
                  {R::ResignationVsInsufficient, "ResignationVsInsufficient"},
                  {R::Abandonment, "Abandonment"},
                  {R::AbandonmentVsInsufficient, "AbandonmentVsInsufficient"},
                  {R::Aborted, "Aborted"},
                  {R::NoShow, "NoShow"},
                  {R::Forfeit, "Forfeit"},
                  {R::ServerAborted, "ServerAborted"},
                  {R::BothDisconnected, "BothDisconnected"}},
                 {15, 19, 27, 255});
    using K = pr::GameEventKind;
    checkEnum<K>({{K::DrawOffered, "DrawOffered"}, {K::DrawDeclined, "DrawDeclined"}, {K::PlayerDisconnected, "PlayerDisconnected"},
                  {K::PlayerReconnected, "PlayerReconnected"}, {K::RematchOffered, "RematchOffered"}, {K::RematchDeclined, "RematchDeclined"}},
                 {0, 7});
    using Q = pr::QueueState;
    checkEnum<Q>({{Q::Left, "Left"}, {Q::Searching, "Searching"}, {Q::Matched, "Matched"}}, {3});
    using CS = pr::ChallengeState;
    checkEnum<CS>({{CS::Pending, "Pending"}, {CS::Accepted, "Accepted"}, {CS::Declined, "Declined"}, {CS::Cancelled, "Cancelled"},
                   {CS::Expired, "Expired"}, {CS::Unavailable, "Unavailable"}},
                  {6});
    using N = pr::NoticeCode;
    checkEnum<N>({{N::ServerShutdown, "ServerShutdown"}, {N::Banned, "Banned"}, {N::SessionRevoked, "SessionRevoked"},
                  {N::MatchmakingCooldown, "MatchmakingCooldown"}, {N::ReplacedByNewConnection, "ReplacedByNewConnection"},
                  {N::RatingRestored, "RatingRestored"}},
                 {0, 6, 8});
    using E = pr::ErrorCode;
    checkEnum<E>({{E::Malformed, "Malformed"},
                  {E::UnsupportedProtocol, "UnsupportedProtocol"},
                  {E::Unauthorized, "Unauthorized"},
                  {E::Banned, "Banned"},
                  {E::RateLimited, "RateLimited"},
                  {E::ServerFull, "ServerFull"},
                  {E::Replaced, "Replaced"},
                  {E::ShuttingDown, "ShuttingDown"},
                  {E::Internal, "Internal"},
                  {E::HelloRequired, "HelloRequired"},
                  {E::EmailUnverified, "EmailUnverified"},
                  {E::NotInGame, "NotInGame"},
                  {E::NotYourTurn, "NotYourTurn"},
                  {E::IllegalMove, "IllegalMove"},
                  {E::StalePly, "StalePly"},
                  {E::Desync, "Desync"},
                  {E::GameOver, "GameOver"},
                  {E::AlreadyInGame, "AlreadyInGame"},
                  {E::InvalidCategory, "InvalidCategory"},
                  {E::DrawOfferLimit, "DrawOfferLimit"},
                  {E::NothingToClaim, "NothingToClaim"},
                  {E::AbortNotAllowed, "AbortNotAllowed"},
                  {E::NoPendingOffer, "NoPendingOffer"},
                  {E::FlagFell, "FlagFell"},
                  {E::QueueNotAllowed, "QueueNotAllowed"},
                  {E::ChallengeNotFound, "ChallengeNotFound"},
                  {E::UserUnavailable, "UserUnavailable"},
                  {E::ChallengeLimit, "ChallengeLimit"},
                  {E::CannotChallengeSelf, "CannotChallengeSelf"},
                  {E::CodeInvalid, "CodeInvalid"},
                  {E::RatedRequiresOfficialTc, "RatedRequiresOfficialTc"},
                  {E::MatchmakingCooldown, "MatchmakingCooldown"},
                  {E::InvalidTimeControl, "InvalidTimeControl"},
                  {E::RematchUnavailable, "RematchUnavailable"},
                  {E::RatedRepeatLimit, "RatedRepeatLimit"},
                  {E::ProtocolViolation, "ProtocolViolation"},
                  {E::Flood, "Flood"},
                  {E::CheatDetected, "CheatDetected"}},
                 {0, 12, 99, 113, 211, 243});
    using S = pr::Stance;
    checkEnum<S>({{S::Seated, "Seated"}, {S::Standing, "Standing"}, {S::SideLeft, "SideLeft"}, {S::SideRight, "SideRight"}}, {4, 9});
}

// The close code that follows a fatal error, and back; a code of another range stands for none.
TEST(net_protocol_gaps_close_codes) {
    CHECK_EQ(pr::closeCodeFor(pr::ErrorCode::Unauthorized), pr::CloseCode::Unauthorized);
    CHECK_EQ(pr::closeCodeFor(pr::ErrorCode::EmailUnverified), pr::CloseCode::EmailUnverified);
    CHECK_EQ(pr::closeCodeFor(pr::ErrorCode::CheatDetected), pr::CloseCode::CheatDetected);
    CHECK_EQ(pr::closeCodeFor(pr::ErrorCode::NotYourTurn), uint16_t(0));   // never fatal
    pr::ErrorCode code{};
    CHECK(pr::errorCodeForClose(pr::CloseCode::SlowConsumer, code));
    CHECK_EQ(int(code), 243);
    CHECK(!pr::isValid(code));   // a code this codec does not know
    CHECK(!pr::errorCodeForClose(1000, code));
    CHECK(!pr::errorCodeForClose(4100, code));
    CHECK(!pr::errorCodeForClose(4316, code));
}

// A rating update is valid with a category of 1 to 7 bytes and an id within 2^53; its two rating
// changes need nothing more. It goes through the codec unchanged.
TEST(net_protocol_gaps_rating_update) {
    pr::RatingUpdate m;
    m.game = 42;
    m.category = "3+2";
    m.white = {1500, 1512, 3, true};
    m.black = {1700, 1688, 140, false};
    CHECK(pr::valid(m));
    CHECK(pr::valid(m.white));
    std::vector<uint8_t> b;
    pr::encode(m, b);
    pr::RatingUpdate back;
    REQUIRE(pr::decode(b.data(), b.size(), back));
    CHECK_EQ(back.category, std::string("3+2"));
    CHECK_EQ(back.white.after, uint16_t(1512));
    CHECK_EQ(back.black.games, 140u);
    CHECK(back.white.provisional && !back.black.provisional);
    m.category = "";
    CHECK(!pr::valid(m));
    m.category = "toolong8";
    CHECK(!pr::valid(m));
    pr::QueueLeave leave;
    CHECK(pr::valid(leave));
    b.clear();
    pr::encode(leave, b);
    pr::QueueLeave leaveBack;
    CHECK(pr::decode(b.data(), b.size(), leaveBack));
    CHECK_EQ(std::string(pr::messageName(pr::MsgType::QueueLeave)), std::string("QueueLeave"));
}

// JSON strings: every short escape written and read back, \u escapes in either case, and what a
// reader refuses (a bad \u escape, a stray UTF-8 byte, malformed numbers).
TEST(net_json_gaps_strings_and_numbers) {
    using net::json::Value;
    Value o = Value::object();
    o.set("s", std::string("tab\there\r\nback\bfeed\f\"q\"\\ \x01"));
    const std::string text = o.dump();
    CHECK(text.find("\\t") != std::string::npos);
    CHECK(text.find("\\r\\n") != std::string::npos);
    CHECK(text.find("\\b") != std::string::npos);
    CHECK(text.find("\\f") != std::string::npos);
    CHECK(text.find("\\u0001") != std::string::npos);
    Value back;
    REQUIRE(net::json::parse(text, back));
    CHECK_EQ(back["s"].asString(), o["s"].asString());

    REQUIRE(net::json::parse(R"(["éé", "☃", "a\/b"])", back));
    CHECK_EQ(back[0].asString(), std::string("\xC3\xA9\xC3\xA9"));
    CHECK_EQ(back[1].asString(), std::string("\xE2\x98\x83"));
    CHECK_EQ(back[2].asString(), std::string("a/b"));

    std::string err;
    CHECK(!net::json::parse(R"(["\u12G4"])", back, &err));
    CHECK_EQ(err.find("invalid \\u escape") != std::string::npos, true);
    CHECK(!net::json::parse("[\"a\xFF\"]", back, &err));   // no UTF-8 sequence starts with 0xFF
    CHECK(err.find("invalid UTF-8") != std::string::npos);
    CHECK(!net::json::parse("[\"\xC3\"]", back, &err));    // a sequence cut short
    CHECK(err.find("invalid UTF-8") != std::string::npos);
    for (const char* bad : {"-", "-x", "1.", "1.e3", "1e", "1e+", "+1", "[-]"}) {
        CHECK(!net::json::parse(bad, back, &err));
    }
    REQUIRE(net::json::parse("[-0.5e+2, 0, 12E-1]", back));
    CHECK_EQ(back[0].asNumber(), -50.0);
    CHECK_EQ(back[2].asNumber(), 1.2);
}

// base64 with the standard alphabet's '+' and '/' (and the URL alphabet refusing them), and the
// streamed SHA-256 against the one-shot digest.
TEST(net_crypto_gaps_base64_and_stream) {
    const uint8_t bytes[] = {0xFB, 0xEF, 0xFF, 0x3E};
    const std::string std64 = net::crypto::base64(bytes, sizeof bytes);
    CHECK_EQ(std64, std::string("++//Pg=="));
    std::vector<uint8_t> out;
    CHECK(net::crypto::base64Decode(std64, out));
    CHECK(out == std::vector<uint8_t>(bytes, bytes + sizeof bytes));
    CHECK(!net::crypto::base64urlDecode("++//Pg", out));   // not the URL alphabet
    CHECK(net::crypto::base64urlDecode(net::crypto::base64url(bytes, sizeof bytes), out));
    CHECK(out == std::vector<uint8_t>(bytes, bytes + sizeof bytes));

    net::crypto::Sha256Stream s;
    const std::string a = "The quick brown fox ", b = "jumps over the lazy dog";
    s.update(a.data(), a.size());
    s.update(b.data(), b.size());
    CHECK_EQ(s.bytes(), uint64_t(a.size() + b.size()));
    const net::crypto::Sha256 d = s.finish();
    CHECK_EQ(net::crypto::hex(d), net::crypto::hex(net::crypto::sha256(a + b)));
    CHECK_EQ(net::crypto::hex(d), std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
    s.update("abc", 3);
    s.reset();
    CHECK_EQ(s.bytes(), uint64_t(0));
    CHECK_EQ(net::crypto::hex(s.finish()), net::crypto::hex(net::crypto::sha256(std::string())));
}
