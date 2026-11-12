#include "core/Segment.h"

#include "core/Limits.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

std::vector<float> cornerLegsLeft(std::vector<CornerLeg> const& legs, double time) noexcept {
    std::vector<float> left;
    left.reserve(legs.size());
    for (auto const& leg : legs) {
        auto const remaining = std::abs(segmentValue(leg.segment, time) - leg.segment.to);
        if (remaining > 0.0f) left.push_back(remaining);
    }
    return left;
}

CornerPose cornerPose(Segment const& next, std::vector<float> const& legsLeft, double time) noexcept {
    auto const span = std::abs(next.to - next.from);
    auto       tail = 0.0f;
    for (auto const leg : legsLeft) tail += leg;
    if (next.length <= 0.0 || tail + span <= 0.0f) return {0.0f, next.to};
    auto const f      = std::clamp((time - static_cast<double>(next.startTick)) / next.length, 0.0, 1.0);
    auto       walked = static_cast<float>(f) * (tail + span);
    for (auto const leg : legsLeft) {
        if (walked < leg) return {1.0f - walked / leg, next.from};
        walked -= leg;
    }
    auto const on = span > 0.0f ? std::min(walked / span, 1.0f) : 1.0f;
    return {0.0f, next.from + (next.to - next.from) * on};
}

} // namespace optipiston::core
