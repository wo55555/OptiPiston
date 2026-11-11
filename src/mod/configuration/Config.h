#pragma once

#include "core/Settings.h"

#include <string>
#include <vector>

namespace optipiston::configuration {

struct PistonConfig {
    bool            enabled       = true;
    float           durationTicks = 4.0f;
    core::TurnStyle turn          = core::TurnStyle::Diagonal;
};

struct WorldSpeedConfig {
    std::vector<float> presets   = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 10.0f, 20.0f, 50.0f, 100.0f};
    bool               audio     = true;
    bool               particles = true;
};

struct CommandConfig {
    bool        enabled = true;
    std::string command = "optipiston";
};

struct Config {
    int              version    = 1;
    std::string      locateName = "zh_CN";
    PistonConfig     piston;
    WorldSpeedConfig worldSpeed;
    CommandConfig    command;
};

} // namespace optipiston::configuration
