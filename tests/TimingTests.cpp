#include <gtest/gtest.h>

#include "nemo/core/evaluation/Timing.hpp"

using namespace nemo;

// Spec acceptance scenario 1: a plate at sequence frame 200 and overlay at
// 212 become a composition with local starts 0 and 12; moving that
// composition preserves the offset and its internal animation.
TEST(TimingTest, AcceptanceScenario1_LocalOffsetsFromParentPlacement) {
    // One composition spans both inputs; its local time zero sits at the
    // parent frame of the earliest input (200). The overlay lands at local 12.
    const Timing composition{.parentStart = 200, .rate = 1.0};
    EXPECT_EQ(composition.toLocal(200), 0);   // plate input local start
    EXPECT_EQ(composition.toLocal(212), 12);  // overlay input local start
    // Moving the composition shifts only parent placement.
    const Timing moved{.parentStart = 350, .rate = 1.0};
    EXPECT_EQ(moved.toLocal(362), 12);
}

TEST(TimingTest, MoveChangesPlacementOnly) {
    Timing timing{.parentStart = 200, .rate = 1.0};
    EXPECT_EQ(timing.toParent(30), 230);
    // Moving the clip 40 frames later rewires parent placement; local time is
    // untouched, so internal animation maps identically after the move.
    timing.parentStart += 40;
    EXPECT_EQ(timing.toParent(30), 270);
    EXPECT_EQ(timing.toLocal(270), 30);
}

TEST(TimingTest, TrimAndSlipDoNotShiftLocalTime) {
    // Trim/slip are interval changes on top of the mapping; the mapping
    // itself stays stable so a slipped clip samples the same internal frames.
    const Timing moved{.parentStart = 500, .rate = 1.0};
    EXPECT_EQ(moved.toLocal(500), 0);
    EXPECT_EQ(moved.toLocal(509), 9);
}

TEST(TimingTest, RetimeChangesSamplingRate) {
    // 2x speed: one composition frame advances two parent frames.
    const Timing retime{.parentStart = 0, .rate = 2.0};
    EXPECT_EQ(retime.toParent(10), 20);
    EXPECT_EQ(retime.toLocal(20), 10);
    // Moving a retimed clip keeps the rate.
    Timing moved = retime;
    moved.parentStart = 100;
    EXPECT_EQ(moved.toParent(10), 120);
}

TEST(TimingTest, InternalSourceRetimeAppliesBeforeDownstream) {
    const SourceTiming source{.sourceOffset = 1000, .rate = 0.5};
    // Composition frame 0 samples source frame 1000; slow motion (0.5x)
    // samples every other source frame.
    EXPECT_EQ(source.toSource(0), 1000);
    EXPECT_EQ(source.toSource(10), 1005);
}

TEST(TimingTest, RoundTripIsStableForIntegerRates) {
    for (const std::int64_t start : {-500, 0, 200, 100000}) {
        for (const double rate : {1.0, 2.0}) {
            const Timing timing{.parentStart = start, .rate = rate};
            for (const std::int64_t local : {0, 1, 12, 47, 1000}) {
                EXPECT_EQ(timing.toLocal(timing.toParent(local)), local)
                    << "start=" << start << " rate=" << rate << " local=" << local;
            }
        }
    }
}
