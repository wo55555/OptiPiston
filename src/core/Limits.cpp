#include "core/Limits.h"

#include <algorithm>
#include <cmath>

namespace optipiston::core {

float clampDuration(float ticks) noexcept {
    if (!std::isfinite(ticks)) return MaxDurationTicks;
    return std::clamp(ticks, MinDurationTicks, MaxDurationTicks);
}

float clampWorldSpeed(float speed) noexcept {
    if (!std::isfinite(speed)) return 1.0f;
    return std::clamp(speed, MinWorldSpeed, MaxWorldSpeed);
}

int clampPistonRange(int blocks) noexcept { return std::clamp(blocks, MinPistonRange, MaxPistonRange); }

bool withinPistonRange(
    double playerX,
    double playerY,
    double playerZ,
    int    blockX,
    int    blockY,
    int    blockZ,
    int    rangeBlocks
) noexcept {
    if (rangeBlocks <= 0) return true;
    double const dx    = (static_cast<double>(blockX) + 0.5) - playerX;
    double const dy    = (static_cast<double>(blockY) + 0.5) - playerY;
    double const dz    = (static_cast<double>(blockZ) + 0.5) - playerZ;
    double const limit = static_cast<double>(rangeBlocks);
    return dx * dx + dy * dy + dz * dz <= limit * limit;
}

} // namespace optipiston::core
