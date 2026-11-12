#pragma once

#include <cstdint>
#include <vector>

namespace optipiston::core {

// One interpolated value: moves over `length` ticks, then holds `to` until `lifetime` ends.
struct Segment {
    int64_t startTick{};
    float   from{};
    float   to{};
    double  length{1.0};
    double  lifetime{1.0};
};

// Native interpolation at tick t draws time t - 1 + alpha.
[[nodiscard]] double visualTime(int64_t tick, float alpha) noexcept;

[[nodiscard]] float segmentValue(Segment const& segment, double time) noexcept;

// Last tick on which the segment is still alive.
[[nodiscard]] int64_t segmentEndTick(Segment const& segment) noexcept;

[[nodiscard]] bool segmentRunning(Segment const& segment, int64_t tick) noexcept;

// Past its end, or ahead of the clock after a backward jump.
[[nodiscard]] bool segmentExpired(Segment const& segment, int64_t tick) noexcept;

// Segment for a piston action started at `tick` with the current settings.
[[nodiscard]] Segment makeSegment(int64_t tick, float from, float to, float durationTicks) noexcept;

// tailLeft: share of the current owed push still to cover; value: the new segment's value.
struct CornerPose {
    float tailLeft{};
    float value{};
};

// One leg of a block's path: the push it is playing now, then the earlier pushes it still owes, oldest last.
struct CornerLeg {
    Segment segment;
    int     facing[3]{};
};

// Distance still owed on each leg at `time`, in the same order.
[[nodiscard]] std::vector<float> cornerLegsLeft(std::vector<CornerLeg> const& legs, double time) noexcept;

// Walks the owed legs in order before the new push, landing on time.
[[nodiscard]] CornerPose cornerPose(Segment const& next, std::vector<float> const& legsLeft, double time) noexcept;

} // namespace optipiston::core
