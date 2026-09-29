#pragma once

#include <cstdint>

namespace optipiston::core {

// Render-only piston animation length in game ticks; fractions allowed.
inline constexpr float MinDurationTicks = 2.0f;
inline constexpr float MaxDurationTicks = 4.0f;

// Singleplayer simulation rate.
inline constexpr float MinWorldSpeed = 0.1f;
inline constexpr float MaxWorldSpeed = 10.0f;

[[nodiscard]] float clampDuration(float ticks) noexcept;
[[nodiscard]] float clampWorldSpeed(float speed) noexcept;

} // namespace optipiston::core
