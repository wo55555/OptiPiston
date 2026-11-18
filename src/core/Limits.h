#pragma once

#include <cstdint>

namespace optipiston::core {

// Render-only piston animation length in game ticks; fractions allowed.
inline constexpr float MinDurationTicks = 2.0f;
inline constexpr float MaxDurationTicks = 4.0f;

// Singleplayer simulation rate.
inline constexpr float MinWorldSpeed = 0.01f;
inline constexpr float MaxWorldSpeed = 100.0f;

// Max distance from the player in blocks for piston animation optimization; 0 means unlimited.
inline constexpr int MinPistonRange     = 0;
inline constexpr int MaxPistonRange     = 1024;
inline constexpr int DefaultPistonRange = 128;

[[nodiscard]] float clampDuration(float ticks) noexcept;
[[nodiscard]] float clampWorldSpeed(float speed) noexcept;
[[nodiscard]] int   clampPistonRange(int blocks) noexcept;
[[nodiscard]] bool  withinPistonRange(
     double playerX,
     double playerY,
     double playerZ,
     int    blockX,
     int    blockY,
     int    blockZ,
     int    rangeBlocks
 ) noexcept;

} // namespace optipiston::core
