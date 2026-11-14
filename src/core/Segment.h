#pragma once

#include <array>
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

// One owed push, with a fractional delay when earlier pushes must finish first.
struct PushPathLeg {
    Segment segment;
    int     facing[3]{};
    double  delay{};
};
using PushPath = std::vector<PushPathLeg>;

// Keeps the actual remaining path at the handoff, oldest first; a corner retimes it before the new push.
[[nodiscard]] PushPath carryPushPath(PushPathLeg next, PushPath const& previous, bool corner);

[[nodiscard]] std::array<float, 3> pushPathOffset(PushPath const& path, double time) noexcept;

} // namespace optipiston::core
