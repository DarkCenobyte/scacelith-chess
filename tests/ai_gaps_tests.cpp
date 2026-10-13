// The computer opponent's helpers that tests/ai_tests.cpp leaves out, none of which needs the
// embedded engine: the recapture detection's board after castlings (the rook moves too) and
// promotions (the new piece is no longer a pawn: no en passant), and the Engine's settings, draw
// decisions and readiness before it is started.
#include "test.h"
#include "ai/behavior.h"
#include "ai/engine.h"

#include <string>
#include <vector>

TEST(ai_gaps_recapture_after_castling) {
    using ai::detail::isRecapture;
    // The rook leaves h1 for f1 with the king: a piece taken on f1 is a capture, h1 is empty.
    CHECK(isRecapture({"e1g1", "a8f1"}, "g1f1"));
    CHECK(!isRecapture({"e1g1", "a8h1"}, "g1h1"));
    // Black's queenside castling: the rook leaves a8 for d8.
    CHECK(isRecapture({"e8c8", "h1d8"}, "c8d8"));
    CHECK(!isRecapture({"e8c8", "h1a8"}, "c8a8"));
    // A king step of one file is no castling: the rook stays.
    CHECK(isRecapture({"e1f1", "a8h1"}, "f1h1"));
}

TEST(ai_gaps_recapture_after_promotion) {
    using ai::detail::isRecapture;
    // A pawn's diagonal step to an empty square takes en passant (the king beside it here)...
    CHECK(isRecapture({"d7d6", "d2d7", "e8e7", "d7e6"}, "f7e6"));
    // ...a queen's does not: once promoted, the piece is a queen.
    CHECK(!isRecapture({"d7d6", "d2d7q", "e8e7", "d7e6"}, "f7e6"));
    // The same for Black (lower case pieces).
    CHECK(isRecapture({"d2d3", "d7d2", "e1e2", "d2e3"}, "f2e3"));
    CHECK(!isRecapture({"d2d3", "d7d2q", "e1e2", "d2e3"}, "f2e3"));
    // An under-promotion that takes: a capture all the same.
    CHECK(isRecapture({"a2b8n"}, "a8b8"));
}

TEST(ai_gaps_engine_without_starting) {
    ai::Engine e;
    CHECK(!e.ready());
    ai::EngineSettings s;
    s.skillLevel = 3;
    s.depth = 4;
    s.humanize = false;
    e.configure(s);
    CHECK_EQ(e.settings().skillLevel, 3);
    CHECK_EQ(e.settings().depth, 4);
    CHECK(!e.settings().humanize);
    CHECK(ai::detail::sameStrength(e.settings(), s));
    // The draw decisions are the behaviour's, whatever the engine.
    for (int eval : {-100000, -150, -40, 0, 50, 150, 100000})
        for (int plies : {10, 40, 70, 90}) {
            CHECK_EQ(e.acceptsDraw(eval, plies), ai::detail::acceptsDrawOffer(eval, plies));
            for (int since : {-1, 5, 25}) CHECK_EQ(e.offersDraw(eval, plies, since), ai::detail::offersDraw(eval, plies, since));
        }
    CHECK(e.offersDraw(-60, 50, -1));    // somewhat worse, after the opening
    CHECK(!e.offersDraw(-60, 50, 10));   // offered 10 plies ago
    CHECK(!e.offersDraw(0, 40, -1));     // equal, but not late enough
    CHECK(e.offersDraw(10, 60, 30));
    CHECK(!e.ready());
}
