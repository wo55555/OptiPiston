#pragma once

#include <cstdint>

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

} // namespace optipiston::core
