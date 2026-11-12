#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace optipiston::core {

// Path of a block pushed again before its earlier push finished.
enum class RedirectType : uint8_t {
    Diagonal, // both pushes play at once
    Corner    // the earlier push finishes first, then the block turns
};

// Values the render and tick hooks read; written by commands and the external API.
struct PistonSettings {
    std::atomic_bool          enabled{true};
    std::atomic<float>        durationTicks{4.0f};
    std::atomic<RedirectType> redirectType{RedirectType::Corner};
    // A pushed block is lit like the landed one, and blocks and gives light like the block it carries.
    std::atomic_bool movingBlockLighting{true};
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
