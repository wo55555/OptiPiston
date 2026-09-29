#include "mod/command/Command.h"

#include "core/Limits.h"
#include "core/Settings.h"
#include "mod/OptiPiston.h"
#include "platform/Features.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace optipiston::command {

// Command params need external linkage for boost::pfr reflection.
struct DurationParams {
    float ticks{};
};

struct SpeedParams {
    float value{};
};

namespace {

using namespace ll::i18n_literals;

std::string onOff(bool value) { return value ? "optipiston.command.on"_tr() : "optipiston.command.off"_tr(); }

std::string translate(char const* key) {
    return std::string{ll::i18n::getInstance().get(key, ll::i18n::getDefaultLocaleCode())};
}

void setWorldSpeed(CommandOutput& output, float value) {
    auto& world = core::worldSpeedSettings();
    world.requested.store(core::clampWorldSpeed(value));
    platform::refreshWorldSpeed();
    output.success("optipiston.command.speed.set"_tr(world.requested.load()));
    if (auto const* blocker = platform::worldSpeedBlocker())
        output.error("optipiston.command.speed.blocked"_tr(translate(blocker)));
}

void setPistonEnabled(CommandOutput& output, bool enabled) {
    OptiPiston::getInstance().updateConfig([enabled](auto& config) { config.piston.enabled = enabled; });
    output.success("optipiston.command.piston.enabled"_tr(onOff(enabled)));
}

} // namespace

void registerCommands(std::string const& name) {
    auto& command = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        name,
        "optipiston.command.description"_tr()
    );

    command.overload().text("piston").text("on").execute([](CommandOrigin const&, CommandOutput& output) {
        setPistonEnabled(output, true);
    });
    command.overload().text("piston").text("off").execute([](CommandOrigin const&, CommandOutput& output) {
        setPistonEnabled(output, false);
    });
    command.overload<DurationParams>()
        .text("piston")
        .text("duration")
        .required("ticks")
        .execute([](CommandOrigin const&, CommandOutput& output, DurationParams const& params) {
            if (!(params.ticks >= core::MinDurationTicks && params.ticks <= core::MaxDurationTicks)) {
                output.error(
                    "optipiston.command.piston.durationRange"_tr(core::MinDurationTicks, core::MaxDurationTicks)
                );
                return;
            }
            OptiPiston::getInstance().updateConfig([&](auto& config) { config.piston.durationTicks = params.ticks; });
            output.success("optipiston.command.piston.duration"_tr(params.ticks));
        });

    command.overload<SpeedParams>().text("speed").required("value").execute(
        [](CommandOrigin const&, CommandOutput& output, SpeedParams const& params) {
            if (params.value < core::MinWorldSpeed || params.value > core::MaxWorldSpeed) {
                output.error("optipiston.command.speed.range"_tr(core::MinWorldSpeed, core::MaxWorldSpeed));
                return;
            }
            setWorldSpeed(output, params.value);
        }
    );
    command.overload().text("speed").text("reset").execute([](CommandOrigin const&, CommandOutput& output) {
        setWorldSpeed(output, 1.0f);
    });
    command.overload().text("speed").text("next").execute([](CommandOrigin const&, CommandOutput& output) {
        auto const presets = OptiPiston::getInstance().getConfig().worldSpeed.presets;
        setWorldSpeed(output, core::nextPreset(presets, core::worldSpeedSettings().requested.load()));
    });
    command.overload().text("speed").text("prev").execute([](CommandOrigin const&, CommandOutput& output) {
        auto const presets = OptiPiston::getInstance().getConfig().worldSpeed.presets;
        setWorldSpeed(output, core::previousPreset(presets, core::worldSpeedSettings().requested.load()));
    });

    command.overload().text("status").execute([](CommandOrigin const&, CommandOutput& output) {
        auto const& piston  = core::pistonSettings();
        auto const& world   = core::worldSpeedSettings();
        auto const* blocker = platform::worldSpeedBlocker();
        output.success(
            "optipiston.command.status.piston"_tr(onOff(piston.enabled.load()), piston.durationTicks.load())
        );
        output.success(
            "optipiston.command.status.world"_tr(
                world.requested.load(),
                world.applied.load(),
                blocker ? translate(blocker) : "optipiston.command.status.active"_tr()
            )
        );
    });
}

} // namespace optipiston::command