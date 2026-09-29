#pragma once

#include <string>
#include <vector>

namespace optipiston::configuration {

struct PistonConfig {
    bool  enabled       = true;
    float durationTicks = 4.0f;
};

struct WorldSpeedConfig {
    std::vector<float> presets   = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
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
