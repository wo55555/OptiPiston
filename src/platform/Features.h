#pragma once

// Implemented by each src/platform/mcXXXX adapter.

namespace optipiston::platform {

[[nodiscard]] bool hookPistonRender(bool enable);

// Timer, particle and audio hooks for singleplayer world speed.
[[nodiscard]] bool hookWorldSpeed(bool enable);

// Re-evaluates whether the current world may be sped up; call once per client tick.
void refreshWorldSpeed();

// I18n key for why world speed is not applied, or nullptr if it is.
[[nodiscard]] char const* worldSpeedBlocker();

} // namespace optipiston::platform
