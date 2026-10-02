// The values of the custom time control and engine limit steppers (src/ui/ui_stepper_values.h): a
// value from a hand-edited .ini is snapped to the one its stepper shows when the New Game,
// challenge and direct match pages open, so that the game uses what the page shows.
#include "test.h"
#include "ui/ui_stepper_values.h"
#include <climits>

TEST(ui_stepper_values_snap_hand_edited_values) {
    using namespace ui::detail;
    const std::vector<int>& bv = baseTimeValues();
    CHECK_EQ(bv.front(), 15);
    CHECK_EQ(bv.back(), 10800);
    // The values the pages write themselves stay as they are.
    for (const std::vector<int>* t : {&bv, &moveTimeValues(), &nodeValues()})
        for (int v : *t) CHECK_EQ(nearestValue(*t, v), v);
    // newgame.custom_base_seconds = 100: the stepper shows 1:45 (between 1:30 and 1:45), and so
    // does the game now, not 1:40; 1000 s plays 17:00.
    CHECK_EQ(nearestValue(bv, 100), 105);
    CHECK_EQ(nearestValue(bv, 1000), 1020);
    // direct.base_seconds = 0 (or any number below the table): 0:15, not a one-second game.
    CHECK_EQ(nearestValue(bv, 0), 15);
    CHECK_EQ(nearestValue(bv, INT_MIN), 15);
    CHECK_EQ(nearestValue(bv, INT_MAX), 10800);
    // engine.move_time_ms = 1234 shows "1 s": Stockfish thinks 1000 ms, not 1234.
    CHECK_EQ(nearestValue(moveTimeValues(), 1234), 1000);
    CHECK_EQ(nearestValue(moveTimeValues(), -5), 0);
    CHECK_EQ(nearestValue(nodeValues(), 1234), 1000);
    CHECK_EQ(nearestValue(nodeValues(), INT_MAX), 50000000);
    // Always the value the stepper shows: the one at nearestIndex().
    for (int v : {7, 100, 1000, 1234, 4000, 99999}) CHECK_EQ(nearestValue(bv, v), bv[size_t(nearestIndex(bv, v))]);
}
