#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/Clock.h"
#include "core/Limits.h"
#include "core/Segment.h"
#include "core/Settings.h"

#include <limits>
#include <vector>

using namespace optipiston::core;

TEST_CASE("world visuals keep their own time while player movement is detached") {
    WorldVisualClock clock;
    CHECK_FALSE(clock.advance(100, true, 0.25f, 0.0));
    CHECK(clock.sample(140).tick == 100);
    REQUIRE(clock.sample(140).partial);
    CHECK(*clock.sample(140).partial == doctest::Approx(0.25));
    CHECK_FALSE(clock.advance(140, true, 0.0f, 0.5));
    CHECK_FALSE(clock.advance(160, true, 0.0f, 3.5));
    CHECK(clock.sample(160).tick == 104);
    auto const partial = clock.advance(160, false, 0.0f, 0.0);
    REQUIRE(partial);
    CHECK(*partial == doctest::Approx(0.25));
    CHECK(clock.sample(161).tick == 105);
    CHECK_FALSE(clock.sample(161).partial);
}

TEST_CASE("duration and speed clamp to their ranges") {
    CHECK(clampDuration(1.5f) == 2.0f);
    CHECK(clampDuration(2.5f) == 2.5f);
    CHECK(clampDuration(9.0f) == 4.0f);
    CHECK(clampDuration(std::numeric_limits<float>::quiet_NaN()) == 4.0f);
    CHECK(clampWorldSpeed(0.0f) == MinWorldSpeed);
    CHECK(clampWorldSpeed(0.01f) == doctest::Approx(0.01f));
    CHECK(clampWorldSpeed(100.0f) == doctest::Approx(100.0f));
    CHECK(clampWorldSpeed(1000.0f) == MaxWorldSpeed);
    CHECK(clampWorldSpeed(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
    CHECK(clampPistonRange(-5) == MinPistonRange);
    CHECK(clampPistonRange(0) == 0);
    CHECK(clampPistonRange(64) == 64);
    CHECK(clampPistonRange(5000) == MaxPistonRange);
}

TEST_CASE("piston range check matches 3D block-center distance and treats 0 as unlimited") {
    CHECK(withinPistonRange(0.5, 64.5, 0.5, 1000, 64, 1000, 0));
    CHECK(withinPistonRange(0.5, 64.5, 0.5, 64, 64, 0, 64));
    CHECK_FALSE(withinPistonRange(0.5, 64.5, 0.5, 65, 64, 0, 64));
    CHECK(withinPistonRange(0.5, 64.5, 0.5, 3, 68, 0, 5));
    CHECK_FALSE(withinPistonRange(0.5, 64.5, 0.5, 4, 68, 0, 5));
    // Large coordinates must not lose precision or overflow squared distance.
    CHECK(withinPistonRange(10000000.5, 64.5, -10000000.5, 10000128, 64, -10000001, 128));
    CHECK_FALSE(withinPistonRange(10000000.5, 64.5, -10000000.5, 10000129, 64, -10000001, 128));
}

TEST_CASE("4gt matches the original fixed timing") {
    auto const segment = makeSegment(100, 0.0f, 1.0f, 4.0f);
    CHECK(segment.length == doctest::Approx(4.0));
    CHECK(segmentValue(segment, 100.0) == doctest::Approx(0.0f));
    CHECK(segmentValue(segment, 102.0) == doctest::Approx(0.5f));
    CHECK(segmentValue(segment, 104.0) == doctest::Approx(1.0f));
    // Original rule: expired when tick > start + 4 or tick < start.
    CHECK(segmentRunning(segment, 100));
    CHECK(segmentRunning(segment, 104));
    CHECK_FALSE(segmentRunning(segment, 105));
    CHECK_FALSE(segmentRunning(segment, 99));
}

TEST_CASE("shorter durations finish sooner but keep the 4gt lifetime") {
    auto const segment = makeSegment(10, 1.0f, 0.0f, 2.0f);
    CHECK(segmentValue(segment, 11.0) == doctest::Approx(0.5f));
    CHECK(segmentValue(segment, 12.0) == doctest::Approx(0.0f));
    CHECK(segmentValue(segment, 13.5) == doctest::Approx(0.0f));
    CHECK(segmentRunning(segment, 14));
    CHECK_FALSE(segmentRunning(segment, 15));
}

TEST_CASE("fractional durations set the visual length") {
    auto const segment = makeSegment(0, 0.0f, 1.0f, 2.5f);
    CHECK(segment.length == doctest::Approx(2.5));
    CHECK(segmentValue(segment, 1.25) == doctest::Approx(0.5f));
    CHECK(segmentValue(segment, 3.0) == doctest::Approx(1.0f));
    CHECK(segmentRunning(segment, 4));
    CHECK_FALSE(segmentRunning(segment, 5));
}

TEST_CASE("a corner path accelerates its signed tail and plays the new push at normal speed") {
    auto const first = carryPushPath(
        {
            makeSegment(8, 0.0f, 1.0f, 4.0f),
            {1, 0, 0}
    },
        {},
        false
    );
    auto const path = carryPushPath(
        {
            makeSegment(10, 0.0f, 1.0f, 4.0f),
            {0, 0, 1}
    },
        first,
        true
    );
    REQUIRE(path.size() == 2);
    CHECK(path[0].segment.length == doctest::Approx(1.0));
    CHECK(path[1].delay == doctest::Approx(1.0));
    CHECK(path[1].segment.length == doctest::Approx(4.0));
    CHECK(segmentEndTick(path[1].segment) == 15);
    auto offset = pushPathOffset(path, 10.0);
    CHECK(offset[0] == doctest::Approx(-0.5f));
    CHECK(offset[2] == doctest::Approx(-1.0f));
    offset = pushPathOffset(path, 10.5);
    CHECK(offset[0] == doctest::Approx(-0.25f));
    CHECK(offset[2] == doctest::Approx(-1.0f));
    offset = pushPathOffset(path, 11.0);
    CHECK(offset[0] == doctest::Approx(0.0f));
    CHECK(offset[2] == doctest::Approx(-1.0f));
    offset = pushPathOffset(path, 12.0);
    CHECK(offset[0] == doctest::Approx(0.0f));
    CHECK(offset[2] == doctest::Approx(-0.75f));
    offset = pushPathOffset(path, 14.0);
    CHECK(offset[0] == doctest::Approx(0.0f));
    CHECK(offset[2] == doctest::Approx(-0.25f));
    CHECK((pushPathOffset(path, 15.0) == std::array<float, 3>{}));
}

TEST_CASE("a corner head shares the front block's retimed progress in every direction") {
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign : {-1, 1}) {
            PushPathLeg previous{makeSegment(8, 0.0f, 1.0f, 4.0f)};
            previous.facing[(axis + 1) % 3] = 1;
            PushPathLeg next{makeSegment(10, 0.0f, 1.0f, 4.0f)};
            next.facing[axis] = sign;
            auto const path   = carryPushPath(next, {previous}, true);
            CHECK(pushPathProgress(path, 11.0) == doctest::Approx(0.0f));
            CHECK(pushPathProgress(path, 12.0) == doctest::Approx(0.25f));
            CHECK(pushPathProgress(path, 15.0) == doctest::Approx(1.0f));
            for (int sample = 0; sample <= 100; ++sample) {
                auto const time   = 10.0 + sample * 0.05;
                auto const offset = pushPathOffset(path, time);
                CHECK(pushPathProgress(path, time) == doctest::Approx(1.0f + sign * offset[axis]));
            }

            PushPathLeg pull{makeSegment(10, 1.0f, 0.0f, 4.0f)};
            pull.facing[axis]   = sign;
            auto const pullPath = carryPushPath(pull, {previous}, true);
            CHECK(pushPathProgress(pullPath, 10.5) == doctest::Approx(1.0f));
            CHECK(pushPathProgress(pullPath, 11.0) == doctest::Approx(1.0f));
            CHECK(pushPathProgress(pullPath, 13.0) == doctest::Approx(0.5f));
            CHECK(pushPathProgress(pullPath, 15.0) == doctest::Approx(0.0f));
        }
    }
}

TEST_CASE("path progress uses the newest push rather than older motion on the same axis") {
    PushPath path{
        {makeSegment(10, 0.0f, 1.0f, 4.0f), {1, 0, 0}},
        {makeSegment(12, 0.0f, 1.0f, 4.0f), {-1, 0, 0}, 1.0}
    };
    CHECK(pushPathProgress(path, 12.5) == doctest::Approx(0.0f));
    CHECK(pushPathProgress(path, 14.0) == doctest::Approx(0.25f));
    CHECK(pushPathProgress(path, 17.0) == doctest::Approx(1.0f));
}

TEST_CASE("a third push preserves the retimed path and walks the oldest tail first") {
    auto const first = carryPushPath(
        {
            makeSegment(0, 0.0f, 1.0f, 4.0f),
            {1, 0, 0}
    },
        {},
        false
    );
    auto const second = carryPushPath(
        {
            makeSegment(1, 0.0f, 1.0f, 4.0f),
            {0, 0, 1}
    },
        first,
        false
    );
    auto const third = carryPushPath(
        {
            makeSegment(2, 0.0f, 1.0f, 4.0f),
            {-1, 0, 0}
    },
        second,
        true
    );
    REQUIRE(third.size() == 3);
    auto       offset = pushPathOffset(third, 2.0);
    auto const before = pushPathOffset(second, 2.0);
    CHECK(offset[0] == doctest::Approx(before[0] + 1.0f));
    CHECK(offset[2] == doctest::Approx(before[2]));
    CHECK(offset[0] == doctest::Approx(0.5f));
    CHECK(offset[2] == doctest::Approx(-0.75f));
    offset = pushPathOffset(third, 2.2);
    CHECK(offset[0] == doctest::Approx(0.75f));
    CHECK(offset[2] == doctest::Approx(-0.75f));
    offset = pushPathOffset(third, 2.4);
    CHECK(offset[0] == doctest::Approx(1.0f));
    CHECK(offset[2] == doctest::Approx(-0.75f));
    offset = pushPathOffset(third, 2.7);
    CHECK(offset[0] == doctest::Approx(1.0f));
    CHECK(offset[2] == doctest::Approx(-0.375f));
    offset = pushPathOffset(third, 3.0);
    CHECK(offset[0] == doctest::Approx(1.0f));
    CHECK(offset[2] == doctest::Approx(0.0f));
    offset = pushPathOffset(third, 5.0);
    CHECK(offset[0] == doctest::Approx(0.5f));
    CHECK(offset[2] == doctest::Approx(0.0f));
    CHECK((pushPathOffset(third, 7.0) == std::array<float, 3>{}));
}

TEST_CASE("a handoff drops only finished legs and keeps delayed legs") {
    auto const first = carryPushPath(
        {
            makeSegment(0, 0.0f, 1.0f, 4.0f),
            {1, 0, 0}
    },
        {},
        false
    );
    auto const second = carryPushPath(
        {
            makeSegment(1, 0.0f, 1.0f, 4.0f),
            {0, 0, 1}
    },
        first,
        true
    );
    auto const third = carryPushPath(
        {
            makeSegment(4, 0.0f, 1.0f, 4.0f),
            {-1, 0, 0}
    },
        second,
        true
    );
    REQUIRE(third.size() == 2);
    CHECK(third[0].facing[2] == 1);
    auto offset = pushPathOffset(third, 4.0);
    CHECK(offset[0] == doctest::Approx(1.0f));
    CHECK(offset[2] == doctest::Approx(-0.5f));
    offset = pushPathOffset(third, 5.0);
    CHECK(offset[0] == doctest::Approx(1.0f));
    CHECK(offset[2] == doctest::Approx(0.0f));
    offset = pushPathOffset(third, 7.0);
    CHECK(offset[0] == doctest::Approx(0.5f));
    CHECK(offset[2] == doctest::Approx(0.0f));
    CHECK((pushPathOffset(third, 9.0) == std::array<float, 3>{}));
}

TEST_CASE("a path without a tail is the plain segment including retraction") {
    auto const next = makeSegment(0, 1.0f, 0.0f, 2.0f);
    auto const path = carryPushPath(
        {
            next,
            {0, 1, 0}
    },
        {},
        true
    );
    for (double t : {0.0, 0.5, 1.0, 1.5, 2.0, 3.0}) {
        auto const offset = pushPathOffset(path, t);
        CHECK(offset[0] == doctest::Approx(0.0f));
        CHECK(offset[1] == doctest::Approx(segmentValue(next, t)));
        CHECK(offset[2] == doctest::Approx(0.0f));
    }
}

TEST_CASE("diagonal redirects continue the previous path without retiming it") {
    auto const first = carryPushPath(
        {
            makeSegment(0, 0.0f, 1.0f, 4.0f),
            {1, 0, 0}
    },
        {},
        false
    );
    auto const path = carryPushPath(
        {
            makeSegment(1, 0.0f, 1.0f, 4.0f),
            {0, 0, 1}
    },
        first,
        false
    );
    auto const offset = pushPathOffset(path, 2.0);
    CHECK(offset[0] == doctest::Approx(-0.5f));
    CHECK(offset[2] == doctest::Approx(-0.75f));
}

TEST_CASE("a carried retracting push keeps its direction when the path is retimed") {
    auto const first = carryPushPath(
        {
            makeSegment(0, 1.0f, 0.0f, 4.0f),
            {1, 0, 0}
    },
        {},
        false
    );
    auto const path = carryPushPath(
        {
            makeSegment(1, 0.0f, 1.0f, 4.0f),
            {0, 1, 0}
    },
        first,
        true
    );
    auto const offset = pushPathOffset(path, 1.0);
    CHECK(offset[0] == doctest::Approx(0.75f));
    CHECK(offset[1] == doctest::Approx(-1.0f));
}

TEST_CASE("repeated redirects keep the handoff continuous and discard finished history") {
    PushPath path;
    for (int tick = 0; tick < 1000; ++tick) {
        auto const  before = pushPathOffset(path, tick);
        PushPathLeg next{
            makeSegment(tick, 0.0f, 1.0f, 4.0f),
            {0, 0, 0}
        };
        next.facing[tick % 3] = (tick / 3) % 2 == 0 ? 1 : -1;
        path                  = carryPushPath(next, path, true);
        auto const after      = pushPathOffset(path, tick);
        for (int axis = 0; axis < 3; ++axis) CHECK(after[axis] == doctest::Approx(before[axis] - next.facing[axis]));
        CHECK(path.size() <= 16);
        auto const landed = pushPathOffset(path, tick + 5.000001);
        for (auto const axis : landed) CHECK(axis == doctest::Approx(0.0f));
    }
}

TEST_CASE("visual time lags the tick by one") {
    CHECK(visualTime(10, 0.0f) == doctest::Approx(9.0));
    CHECK(visualTime(10, 0.5f) == doctest::Approx(9.5));
}

TEST_CASE("clock tracker invalidates on switch, epoch and backward jump") {
    ClockTracker tracker;
    CHECK_FALSE(tracker.invalidated({5, 0, false}));
    CHECK_FALSE(tracker.invalidated({6, 0, false}));
    CHECK(tracker.invalidated({6, 1, true}));
    CHECK_FALSE(tracker.invalidated({7, 1, true}));
    CHECK(tracker.invalidated({7, 2, true}));
    CHECK(tracker.invalidated({3, 2, true}));
    CHECK(tracker.invalidated({3, 2, false}));
}

TEST_CASE("clock tracker advances once per tick") {
    ClockTracker tracker;
    CHECK(tracker.advance(1));
    CHECK_FALSE(tracker.advance(1));
    CHECK(tracker.advance(2));
    tracker.reset();
    CHECK(tracker.advance(2));
}

TEST_CASE("external clock overrides until cleared") {
    ExternalClock clock;
    CHECK_FALSE(clock.sample().has_value());
    clock.set(42, 7);
    auto const sample = clock.sample();
    REQUIRE(sample.has_value());
    CHECK(sample->tick == 42);
    CHECK(sample->epoch == 7);
    CHECK(sample->external);
    clock.clear();
    CHECK_FALSE(clock.sample().has_value());
}

TEST_CASE("external partial is kept, rejected out of range and dropped on clear") {
    ExternalClock clock;
    clock.set(10, 1);
    CHECK_FALSE(clock.sample()->partial.has_value());
    clock.setPartial(0.25f);
    REQUIRE(clock.sample()->partial.has_value());
    CHECK(*clock.sample()->partial == doctest::Approx(0.25f));
    CHECK(visualTime(clock.sample()->tick, *clock.sample()->partial) == doctest::Approx(9.25));
    clock.setPartial(1.5f);
    CHECK_FALSE(clock.sample()->partial.has_value());
    clock.setPartial(std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(clock.sample()->partial.has_value());
    clock.setPartial(0.5f);
    clock.clear();
    clock.set(11, 2);
    CHECK_FALSE(clock.sample()->partial.has_value());
}

TEST_CASE("presets step to neighbours and stop at the ends") {
    std::vector<float> const presets{0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    CHECK(nextPreset(presets, 1.0f) == 2.0f);
    CHECK(nextPreset(presets, 4.0f) == 4.0f);
    CHECK(nextPreset(presets, 1.5f) == 2.0f);
    CHECK(previousPreset(presets, 1.0f) == 0.5f);
    CHECK(previousPreset(presets, 0.1f) == 0.1f);
    CHECK(previousPreset(presets, 3.0f) == 2.0f);
}
