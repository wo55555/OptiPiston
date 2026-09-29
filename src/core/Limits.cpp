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

} // namespace optipiston::core
