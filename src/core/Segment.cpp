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

PushPath carryPushPath(PushPathLeg next, PushPath const& previous, bool corner) {
    PushPath path;
    path.reserve(previous.size() + 1);
    auto const start      = static_cast<double>(next.segment.startTick);
    auto const length     = next.segment.length;
    auto       tailTotal  = 0.0;
    auto       tailNormal = 0.0;
    for (auto leg : previous) {
        auto const offset = segmentValue(leg.segment, start - leg.delay) - leg.segment.to;
        if (offset == 0.0f) continue;
        auto const rem   = static_cast<double>(std::abs(offset));
        auto const span  = static_cast<double>(std::abs(leg.segment.to - leg.segment.from));
        auto const remDt = span > 0.0 ? leg.segment.length * (rem / span) : 0.0;
        tailTotal       += rem;
        tailNormal      += remDt;
        if (corner) {
            leg.segment.startTick = next.segment.startTick;
            leg.segment.from      = leg.segment.to + offset;
        }
        path.push_back(leg);
    }
    if (corner) {
        next.delay = 0.0;
        if (tailTotal > 0.0 && length > 0.0) {
            auto const tailBudget = std::min(tailNormal * 0.5, length * 0.25);
            double     delay      = 0.0;
            for (auto& leg : path) {
                auto const rem     = static_cast<double>(std::abs(leg.segment.to - leg.segment.from));
                leg.delay          = delay;
                leg.segment.length = tailBudget * (rem / tailTotal);
                delay             += leg.segment.length;
            }
            next.delay            = delay;
            next.segment.lifetime = std::max(next.segment.lifetime, delay + next.segment.length);
        }
    }
    path.push_back(next);
    return path;
}

std::array<float, 3> pushPathOffset(PushPath const& path, double time) noexcept {
    std::array<float, 3> offset{};
    for (auto const& leg : path) {
        auto const left = segmentValue(leg.segment, time - leg.delay) - leg.segment.to;
        for (int axis = 0; axis < 3; ++axis) offset[axis] += static_cast<float>(leg.facing[axis]) * left;
    }
    return offset;
}

float pushPathProgress(PushPath const& path, double time) noexcept {
    if (path.empty()) return 1.0f;
    auto const& leg = path.back();
    return segmentValue(leg.segment, time - leg.delay);
}

} // namespace optipiston::core
