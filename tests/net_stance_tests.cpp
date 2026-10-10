// The Stance message of protocol minor 2 through the generated codec (C_Stance, S_Stance, the
// open Stance enum), and StanceSender (src/net/stance.h), the rules by which OnlineClient and
// DirectMatch send their player's stance: on a change, paced; while not Seated, again every
// keepalive; again on a new link; another game starts from Seated. The relays themselves are
// tested with the fake server (net_tests.cpp) and between two DirectMatch (direct_tests.cpp).
#include "test.h"
#include "net/gesture.h"
#include "net/protocol_gen.h"
#include "net/stance.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace pr = net::proto;

TEST(net_stance_codec) {
    // C_Stance: type, seq, game, stance: 14 bytes, decoded strictly (a client message).
    pr::C_Stance c;
    c.seq = 77;
    c.game = (uint64_t(1) << 52) + 5;
    c.stance = pr::Stance::SideRight;
    CHECK(pr::valid(c));
    std::vector<uint8_t> buf;
    pr::encode(c, buf);
    CHECK_EQ(buf.size(), size_t(14));
    CHECK_EQ(int(buf[0]), 0x29);
    pr::MsgType t{};
    CHECK(pr::peekType(buf.data(), buf.size(), t) && t == pr::MsgType::C_Stance);
    uint32_t seq = 0;
    CHECK(pr::peekSeq(buf.data(), buf.size(), seq) && seq == 77);
    pr::C_Stance cd;
    CHECK(pr::decode(buf.data(), buf.size(), cd));
    CHECK(cd.seq == 77 && cd.game == c.game && cd.stance == pr::Stance::SideRight);
    for (pr::Stance s : {pr::Stance::Seated, pr::Stance::Standing, pr::Stance::SideLeft, pr::Stance::SideRight}) {
        c.stance = s;
        buf.clear();
        pr::encode(c, buf);
        CHECK(pr::decode(buf.data(), buf.size(), cd) && cd.stance == s);
        CHECK_EQ(int(buf.back()), int(s));
    }
    // A value this minor does not define, a trailing byte, a short frame: refused (strict).
    buf.back() = 4;
    CHECK(!pr::decode(buf.data(), buf.size(), cd));
    buf.back() = 1;
    buf.push_back(0);
    CHECK(!pr::decode(buf.data(), buf.size(), cd));
    buf.pop_back();
    CHECK(!pr::decode(buf.data(), buf.size() - 1, cd));
    // The game id is an id53.
    c.game = uint64_t(1) << 53;
    CHECK(!pr::valid(c));
    c.game = 1;
    c.stance = pr::Stance(9);
    CHECK(!pr::valid(c));   // a sender never sends a value it does not know

    // S_Stance: the relay without the seq, 10 bytes, decoded leniently (a server message): a
    // value of a later minor is kept, and so are trailing bytes of a later minor.
    pr::S_Stance s;
    s.game = 12345;
    s.stance = pr::Stance::Standing;
    CHECK(pr::valid(s));
    buf.clear();
    pr::encode(s, buf);
    CHECK_EQ(buf.size(), size_t(10));
    CHECK_EQ(int(buf[0]), 0xA7);
    pr::S_Stance sd;
    CHECK(pr::decode(buf.data(), buf.size(), sd) && sd.game == 12345 && sd.stance == pr::Stance::Standing);
    buf.back() = 6;
    CHECK(pr::decode(buf.data(), buf.size(), sd) && int(sd.stance) == 6 && !pr::isValid(sd.stance));
    buf.push_back(0x42);
    CHECK(pr::decode(buf.data(), buf.size(), sd) && int(sd.stance) == 6);
    CHECK(!pr::decode(buf.data(), 9, sd));
    // The relay is the client message without its seq, byte for byte.
    pr::C_Stance from;
    from.seq = 3;
    from.game = 12345;
    from.stance = pr::Stance::SideLeft;
    std::vector<uint8_t> cbuf, sbuf;
    pr::encode(from, cbuf);
    s.stance = pr::Stance::SideLeft;
    pr::encode(s, sbuf);
    CHECK(sbuf.size() + 4 == cbuf.size());
    CHECK(std::equal(sbuf.begin() + 1, sbuf.end(), cbuf.begin() + 5));
}

TEST(net_stance_sender_on_change_and_refresh) {
    constexpr int kKeepalive = 1000;
    net::StanceSender s;
    CHECK(!s.due(0.0, kKeepalive));                 // no game yet
    CHECK(s.nextAtMs(kKeepalive) == HUGE_VAL);
    s.set(77, 0);
    CHECK(!s.due(0.0, kKeepalive));                 // Seated: what the receiver shows already
    CHECK(s.nextAtMs(kKeepalive) == HUGE_VAL);

    // Standing goes at once, then every keepalive while it lasts.
    s.set(77, 1);
    CHECK(s.due(1000.0, kKeepalive));
    CHECK_EQ(int(s.stance()), 1);
    CHECK_EQ(s.game(), uint64_t(77));
    s.sent(1000.0);
    CHECK(!s.due(1500.0, kKeepalive));
    CHECK(!s.due(1999.0, kKeepalive));
    CHECK(s.nextAtMs(kKeepalive) == 2000.0);
    CHECK(s.due(2000.0, kKeepalive));
    s.sent(2000.0);
    // The keepalive is the link's, clamped like the gestures' (gestureIdleMs 0 reads as 1 s).
    CHECK(s.nextAtMs(4000) == 6000.0);
    CHECK(s.nextAtMs(0) == 3000.0);
    CHECK(s.nextAtMs(60000) == 12000.0);

    // A change goes at once when the last message is a quarter of a second old...
    s.set(77, 2);
    CHECK(s.due(2250.0, kKeepalive));
    CHECK(!s.due(2249.0, kKeepalive));
    s.sent(2250.0);
    // ...and no sooner: keys tapped fast send the latest stance 250 ms after the last message.
    s.set(77, 3);
    s.set(77, 1);
    s.set(77, 3);
    CHECK(!s.due(2300.0, kKeepalive));
    CHECK(s.nextAtMs(kKeepalive) == 2500.0);
    CHECK(s.due(2500.0, kKeepalive));
    CHECK_EQ(int(s.stance()), 3);
    s.sent(2500.0);
    // Back where it was before a message went: nothing more to say until the keepalive.
    s.set(77, 1);
    s.set(77, 3);
    CHECK(!s.due(3000.0, kKeepalive));
    CHECK(s.due(3500.0, kKeepalive));
    s.sent(3500.0);

    // Back to Seated: once, never refreshed.
    s.set(77, 0);
    CHECK(s.due(3750.0, kKeepalive));
    s.sent(3750.0);
    CHECK(!s.due(10000.0, kKeepalive));
    CHECK(s.nextAtMs(kKeepalive) == HUGE_VAL);

    // A value this codec cannot send counts as Seated.
    s.set(77, 4);
    CHECK_EQ(int(s.stance()), 0);
    CHECK(!s.due(20000.0, kKeepalive));
    s.set(77, -1);
    CHECK_EQ(int(s.stance()), 0);
    s.set(77, 300);
    CHECK_EQ(int(s.stance()), 0);
}

TEST(net_stance_sender_new_link_and_new_game) {
    constexpr int kKeepalive = 1000;
    net::StanceSender s;
    s.set(5, 2);
    s.sent(100.0);
    // A new link (Welcome; the guest's Hello for a host): its receiver shows the player seated,
    // so the stance goes again at once, whatever the interval.
    s.linkUp();
    CHECK(s.due(150.0, kKeepalive));
    s.sent(150.0);
    CHECK(!s.due(400.0, kKeepalive));
    // Seated on a new link: nothing to say.
    s.set(5, 0);
    s.sent(500.0);
    s.linkUp();
    CHECK(!s.due(600.0, kKeepalive));
    CHECK(s.nextAtMs(kKeepalive) == HUGE_VAL);

    // Another game starts from Seated on both sides: a standing stance goes at once; one sent
    // in the previous game never counts for the new one.
    s.set(5, 1);
    s.sent(1000.0);
    s.set(6, 1);
    CHECK_EQ(s.game(), uint64_t(6));
    CHECK(s.due(1001.0, kKeepalive));
    s.sent(1001.0);
    s.set(7, 0);
    CHECK(!s.due(5000.0, kKeepalive));
    // Game 0 (no game shown): never.
    s.set(0, 1);
    CHECK(!s.due(9000.0, kKeepalive));
    CHECK(s.nextAtMs(kKeepalive) == HUGE_VAL);
}
