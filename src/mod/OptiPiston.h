#pragma once

#include "mod/configuration/Config.h"

#include "ll/api/mod/NativeMod.h"

#include <functional>
#include <memory>

namespace optipiston {

class OptiPiston {
public:
    static OptiPiston& getInstance();

    OptiPiston();
    ~OptiPiston();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    // Snapshot; edits go through updateConfig.
    [[nodiscard]] configuration::Config getConfig();

    // Edits, sanitizes, publishes to the hooks and saves the config under one lock.
    void updateConfig(std::function<void(configuration::Config&)> const& edit);

    // Copies config values into the atomics the hooks read.
    void applyConfig();

    bool load();
    bool enable();
    bool disable();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    ll::mod::NativeMod&   mSelf;
};

} // namespace optipiston
