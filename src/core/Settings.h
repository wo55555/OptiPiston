#pragma once

#include <atomic>
#include <vector>

namespace optipiston::core {

// Values the render and tick hooks read; written by commands and the external API.
struct PistonSettings {
    std::atomic_bool   enabled{true};
    std::atomic<float> durationTicks{4.0f};
};

[[nodiscard]] PistonSettings& pistonSettings() noexcept;

// Requested singleplayer world speed; applied only while the world qualifies.
struct WorldSpeedSettings {
    std::atomic<float> requested{1.0f};
    std::atomic_bool   audio{true};
    std::atomic_bool   particles{true};
    // Speed actually in effect this frame; 1 whenever the world does not qualify.
    std::atomic<float> applied{1.0f};
};

[[nodiscard]] WorldSpeedSettings& worldSpeedSettings() noexcept;

// Next preset strictly above `current`, or the largest one.
[[nodiscard]] float nextPreset(std::vector<float> const& presets, float current) noexcept;

// Next preset strictly below `current`, or the smallest one.
[[nodiscard]] float previousPreset(std::vector<float> const& presets, float current) noexcept;

} // namespace optipiston::core
