#include "mod/OptiPiston.h"

#include "core/Clock.h"
#include "core/Limits.h"
#include "core/Settings.h"
#include "mod/command/Command.h"
#include "platform/Features.h"

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/io/LogLevel.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <set>

namespace optipiston {

struct OptiPiston::Impl {
    configuration::Config            mConfig;
    std::mutex                       mConfigMutex;
    std::set<ll::event::ListenerPtr> mListeners;
    bool                             mPistonInstalled{};
    bool                             mWorldSpeedInstalled{};
};

OptiPiston::OptiPiston() : impl(std::make_unique<Impl>()), mSelf(*ll::mod::NativeMod::current()) {}
OptiPiston::~OptiPiston() = default;

OptiPiston& OptiPiston::getInstance() {
    static OptiPiston instance;
    return instance;
}

configuration::Config OptiPiston::getConfig() {
    std::lock_guard const guard(impl->mConfigMutex);
    return impl->mConfig;
}

namespace {

void sanitize(configuration::Config& config) {
    config.piston.durationTicks = core::clampDuration(config.piston.durationTicks);
    auto& presets               = config.worldSpeed.presets;
    for (auto& value : presets) value = core::clampWorldSpeed(value);
    std::sort(presets.begin(), presets.end());
    presets.erase(std::unique(presets.begin(), presets.end()), presets.end());
    // 1x must stay reachable through next/prev.
    if (!std::binary_search(presets.begin(), presets.end(), 1.0f))
        presets.insert(std::lower_bound(presets.begin(), presets.end(), 1.0f), 1.0f);
}

} // namespace

void OptiPiston::applyConfig() {
    auto const& config = impl->mConfig;
    auto&       piston = core::pistonSettings();
    piston.enabled.store(config.piston.enabled);
    piston.durationTicks.store(config.piston.durationTicks);
    auto& world = core::worldSpeedSettings();
    world.audio.store(config.worldSpeed.audio);
    world.particles.store(config.worldSpeed.particles);
}

void OptiPiston::updateConfig(std::function<void(configuration::Config&)> const& edit) {
    std::lock_guard const guard(impl->mConfigMutex);
    edit(impl->mConfig);
    sanitize(impl->mConfig);
    applyConfig();
    try {
        if (!ll::config::saveConfig(impl->mConfig, getSelf().getConfigDir() / "config.json"))
            getSelf().getLogger().error("Unable to save OptiPiston configuration");
    } catch (std::exception const& error) {
        getSelf().getLogger().error("Unable to save OptiPiston configuration: {}", error.what());
    }
}

bool OptiPiston::load() {
#ifdef DEBUG
    getSelf().getLogger().setLevel(ll::io::LogLevel::Debug);
#endif
    auto& logger = getSelf().getLogger();
    {
        std::lock_guard const guard(impl->mConfigMutex);
        try {
            auto config = impl->mConfig;
            if (!ll::config::loadConfig(config, getSelf().getConfigDir() / "config.json"))
                logger.warn("OptiPiston configuration required migration; the original file was preserved");
            impl->mConfig = std::move(config);
        } catch (std::exception const& error) {
            logger.error("Unable to load OptiPiston configuration; using defaults: {}", error.what());
        }
        sanitize(impl->mConfig);
        applyConfig();
    }

    if (auto result = ll::i18n::getInstance().load(getSelf().getLangDir()); !result) {
        logger.error("Failed to load I18n");
        result.error().log(logger);
    }
    return true;
}

bool OptiPiston::enable() {
    auto& logger           = getSelf().getLogger();
    impl->mPistonInstalled = platform::hookPistonRender(true);
    if (!impl->mPistonInstalled) logger.warn("Unable to install piston render hooks; piston animations stay native");
    impl->mWorldSpeedInstalled = platform::hookWorldSpeed(true);
    if (!impl->mWorldSpeedInstalled) logger.warn("Unable to install world speed hooks; world speed is unavailable");

    auto& bus = ll::event::EventBus::getInstance();
    impl->mListeners.emplace(bus.emplaceListener<ll::event::ClientCommandRegisterEvent>([this](auto&&) {
        auto const config = getConfig();
        if (config.command.enabled) command::registerCommands(config.command.command);
    }));
    // World speed is per session: every world starts at 1x.
    impl->mListeners.emplace(bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&&) {
        core::worldSpeedSettings().requested.store(1.0f);
    }));
    impl->mListeners.emplace(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&&) {
        core::worldSpeedSettings().requested.store(1.0f);
        core::externalClock().clear();
    }));
    impl->mListeners.emplace(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&&) {
        platform::refreshWorldSpeed();
    }));
    return true;
}

bool OptiPiston::disable() {
    auto& logger = getSelf().getLogger();
    auto& bus    = ll::event::EventBus::getInstance();
    for (auto const& listener : impl->mListeners) bus.removeListener(listener);
    impl->mListeners.clear();

    core::worldSpeedSettings().requested.store(1.0f);
    platform::refreshWorldSpeed();
    if (impl->mWorldSpeedInstalled && !platform::hookWorldSpeed(false)) {
        logger.error("Unable to remove world speed hooks");
        return false;
    }
    impl->mWorldSpeedInstalled = false;
    if (impl->mPistonInstalled && !platform::hookPistonRender(false)) {
        logger.error("Unable to remove piston render hooks");
        return false;
    }
    impl->mPistonInstalled = false;
    return true;
}

} // namespace optipiston

LL_REGISTER_MOD(optipiston::OptiPiston, optipiston::OptiPiston::getInstance());
