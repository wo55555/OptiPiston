#include "optipiston/api.h"

#include "core/Clock.h"
#include "core/Limits.h"
#include "core/Settings.h"
#include "mod/OptiPiston.h"

#include <optional>

namespace {

using namespace optipiston;

void setExternalClock(int64_t tick, uint64_t epoch) { core::externalClock().set(tick, epoch); }

void clearExternalClock() { core::externalClock().clear(); }

int getPistonEnabled() { return core::pistonSettings().enabled.load() ? 1 : 0; }

void setPistonEnabled(int enabled) {
    OptiPiston::getInstance().updateConfig([enabled](auto& config) { config.piston.enabled = enabled != 0; });
}

void setExternalPartial(float partial) {
    core::externalClock().setPartial(partial >= 0.0f ? std::optional<float>{partial} : std::nullopt);
}

float getPistonDuration() { return core::pistonSettings().durationTicks.load(); }

void setPistonDuration(float ticks) {
    OptiPiston::getInstance().updateConfig([ticks](auto& config) { config.piston.durationTicks = ticks; });
}

constexpr OptiPistonApiV1 gApi{
    sizeof(OptiPistonApiV1),
    OPTIPISTON_API_VERSION,
    &setExternalClock,
    &setExternalPartial,
    &clearExternalClock,
    &getPistonEnabled,
    &setPistonEnabled,
    &getPistonDuration,
    &setPistonDuration,
    core::MinDurationTicks,
    core::MaxDurationTicks,
};

} // namespace

extern "C" __declspec(dllexport) OptiPistonApiV1 const* optipiston_get_api(uint32_t minVersion) {
    return minVersion <= OPTIPISTON_API_VERSION ? &gApi : nullptr;
}
