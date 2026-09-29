#include "core/Segment.h"

#include "core/Limits.h"

#include <algorithm>
#include <cmath>

namespace optipiston::core {

double visualTime(int64_t tick, float alpha) noexcept { return static_cast<double>(tick) - 1.0 + alpha; }

float segmentValue(Segment const& segment, double time) noexcept {
    if (segment.length <= 0.0) return segment.to;
    auto const f = std::clamp((time - static_cast<double>(segment.startTick)) / segment.length, 0.0, 1.0);
    return segment.from + (segment.to - segment.from) * static_cast<float>(f);
}

int64_t segmentEndTick(Segment const& segment) noexcept {
    auto const span = std::max({segment.length, segment.lifetime, 0.0});
    return segment.startTick + static_cast<int64_t>(std::ceil(span));
}

bool segmentRunning(Segment const& segment, int64_t tick) noexcept {
    return tick >= segment.startTick && tick <= segmentEndTick(segment);
}

bool segmentExpired(Segment const& segment, int64_t tick) noexcept { return !segmentRunning(segment, tick); }

Segment makeSegment(int64_t tick, float from, float to, float durationTicks) noexcept {
    auto const length = static_cast<double>(clampDuration(durationTicks));
    // A shorter animation still holds the block for the full 4gt lifecycle the landing handoff was verified with.
    return {tick, from, to, length, static_cast<double>(MaxDurationTicks)};
}

} // namespace optipiston::core
