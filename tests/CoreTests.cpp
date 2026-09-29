#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/Clock.h"
#include "core/Limits.h"
#include "core/Segment.h"
#include "core/Settings.h"

#include <limits>
#include <vector>

using namespace optipiston::core;

TEST_CASE("duration and speed clamp to their ranges") {
    CHECK(clampDuration(1.5f) == 2.0f);
    CHECK(clampDuration(2.5f) == 2.5f);
    CHECK(clampDuration(9.0f) == 4.0f);
    CHECK(clampDuration(std::numeric_limits<float>::quiet_NaN()) == 4.0f);
    CHECK(clampWorldSpeed(0.0f) == MinWorldSpeed);
    CHECK(clampWorldSpeed(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
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
