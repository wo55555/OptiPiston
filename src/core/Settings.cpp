#include "core/Settings.h"

#include <algorithm>

namespace optipiston::core {

PistonSettings& pistonSettings() noexcept {
    static PistonSettings settings;
    return settings;
}

WorldSpeedSettings& worldSpeedSettings() noexcept {
    static WorldSpeedSettings settings;
    return settings;
}

float nextPreset(std::vector<float> const& presets, float current) noexcept {
    if (presets.empty()) return current;
    float best = *std::max_element(presets.begin(), presets.end());
    for (auto const value : presets)
        if (value > current && value < best) best = value;
    return best > current ? best : current;
}

float previousPreset(std::vector<float> const& presets, float current) noexcept {
    if (presets.empty()) return current;
    float best = *std::min_element(presets.begin(), presets.end());
    for (auto const value : presets)
        if (value < current && value > best) best = value;
    return best < current ? best : current;
}

} // namespace optipiston::core
