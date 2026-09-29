#include "platform/Features.h"

#include "core/Clock.h"
#include "core/Settings.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/particlesystem/particle/ParticleEmitterActual.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/util/Timer.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace optipiston::platform {

namespace {

std::atomic_bool gInstalled{false};

// ---- Gating: written on the server thread, read on the client thread ----

// Only a local world runs the integrated server's level tick.
std::atomic_bool         gLocalServerTicking{false};
std::atomic<int>         gGuestPlayers{0};
std::atomic<char const*> gBlocker{"optipiston.worldSpeed.noWorld"};

struct PlayerCounts {
    int  players{};
    int  guests{};
    int  remote{};
    int  active{};
    bool operator==(PlayerCounts const&) const = default;
};

// Server thread only.
PlayerCounts gLoggedCounts{-1, -1, -1, -1};

// ---- Timer: scale the sim timers on top of whatever scale the game itself set ----

struct ScaledTimer {
    float base{};    // scale the game set, e.g. 0 while paused
    float written{}; // scale we last stored
};

std::mutex                                gTimerMutex;
std::unordered_map<::Timer*, ScaledTimer> gScaledTimers;
std::unordered_set<::Timer const*>        gLoggedTimers;

bool isSimTimer(::Timer const* timer) {
    for (bool const clientSide : {false, true}) {
        auto minecraft = ll::service::getMinecraft(clientSide);
        if (minecraft && &static_cast<::Timer&>(minecraft->mSimTimer) == timer) return true;
    }
    return false;
}

// Logs which timers get scaled; one line per timer.
void logTimerOnce(::Timer const* timer, bool sim) {
    {
        std::lock_guard const guard(gTimerMutex);
        if (!gLoggedTimers.insert(timer).second) return;
    }
    ll::mod::NativeMod::current()->getLogger().info(
        "World speed timer {} sim={}",
        static_cast<void const*>(timer),
        sim
    );
}

// Caller holds gTimerMutex; every tracked timer must still be alive.
void restoreTimersLocked() {
    for (auto& [timer, scaled] : gScaledTimers)
        if (timer->mTimeScale == scaled.written) timer->mTimeScale = scaled.base;
    gScaledTimers.clear();
    gLoggedTimers.clear();
}

// ---- FMOD, resolved from the game's fmod.dll; no FMOD headers ----

using FmodResult            = int;
using SystemUpdateFn        = FmodResult (*)(void* system);
using SystemPlaySoundFn     = FmodResult (*)(void* system, void* sound, void* group, bool paused, void** channel);
using GetMasterGroupFn      = FmodResult (*)(void* system, void** group);
using ChannelGetFrequencyFn = FmodResult (*)(void* channel, float* frequency);
using ChannelSetFrequencyFn = FmodResult (*)(void* channel, float frequency);
using GroupGetNumFn         = FmodResult (*)(void* group, int* count);
using GroupGetChildFn       = FmodResult (*)(void* group, int index, void** child);
constexpr FmodResult FmodOk = 0;

struct Fmod {
    void*                 update{};
    void*                 playSound{};
    GetMasterGroupFn      getMasterGroup{};
    ChannelGetFrequencyFn getFrequency{};
    ChannelSetFrequencyFn setFrequency{};
    GroupGetNumFn         getNumChannels{};
    GroupGetChildFn       getChannel{};
    GroupGetNumFn         getNumGroups{};
    GroupGetChildFn       getGroup{};
    SystemUpdateFn        originalUpdate{};
    SystemPlaySoundFn     originalPlaySound{};
    bool                  hooked{};
};

Fmod gFmod;
// Speed already baked into playing channels; FMOD calls these on its update thread.
std::atomic<float> gAudioApplied{1.0f};

float targetAudioSpeed() {
    auto const& world = core::worldSpeedSettings();
    return world.audio.load(std::memory_order_relaxed) ? world.applied.load(std::memory_order_relaxed) : 1.0f;
}

void scaleGroup(void* group, float ratio, int depth) {
    if (!group || depth > 32) return;
    int channels = 0;
    if (gFmod.getNumChannels(group, &channels) == FmodOk) {
        for (int i = 0; i < channels; ++i) {
            void* channel = nullptr;
            if (gFmod.getChannel(group, i, &channel) != FmodOk || !channel) continue;
            float frequency = 0.0f;
            if (gFmod.getFrequency(channel, &frequency) == FmodOk && frequency > 0.0f)
                gFmod.setFrequency(channel, frequency * ratio);
        }
    }
    int groups = 0;
    if (gFmod.getNumGroups(group, &groups) != FmodOk) return;
    for (int i = 0; i < groups; ++i) {
        void* child = nullptr;
        if (gFmod.getGroup(group, i, &child) == FmodOk) scaleGroup(child, ratio, depth + 1);
    }
}

FmodResult fmodUpdateDetour(void* system) {
    auto const target  = targetAudioSpeed();
    auto const applied = gAudioApplied.load(std::memory_order_relaxed);
    if (target != applied) {
        void* master = nullptr;
        if (gFmod.getMasterGroup(system, &master) == FmodOk) scaleGroup(master, target / applied, 0);
        gAudioApplied.store(target, std::memory_order_relaxed);
    }
    return gFmod.originalUpdate(system);
}

FmodResult fmodPlaySoundDetour(void* system, void* sound, void* group, bool paused, void** channel) {
    auto const result  = gFmod.originalPlaySound(system, sound, group, paused, channel);
    auto const applied = gAudioApplied.load(std::memory_order_relaxed);
    // New sounds join at the speed already baked into the others, so the update pass never scales them twice.
    if (result == FmodOk && channel && *channel && applied != 1.0f) {
        float frequency = 0.0f;
        if (gFmod.getFrequency(*channel, &frequency) == FmodOk && frequency > 0.0f)
            gFmod.setFrequency(*channel, frequency * applied);
    }
    return result;
}

template <class T>
bool resolve(HMODULE module, char const* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return out != nullptr;
}

bool hookFmod() {
    auto* module = GetModuleHandleW(L"fmod.dll");
    if (!module) return false;
    bool const resolved =
        resolve(module, "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", gFmod.update)
        && resolve(
            module,
            "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z",
            gFmod.playSound
        )
        && resolve(module, "FMOD_System_GetMasterChannelGroup", gFmod.getMasterGroup)
        && resolve(module, "FMOD_Channel_GetFrequency", gFmod.getFrequency)
        && resolve(module, "FMOD_Channel_SetFrequency", gFmod.setFrequency)
        && resolve(module, "FMOD_ChannelGroup_GetNumChannels", gFmod.getNumChannels)
        && resolve(module, "FMOD_ChannelGroup_GetChannel", gFmod.getChannel)
        && resolve(module, "FMOD_ChannelGroup_GetNumGroups", gFmod.getNumGroups)
        && resolve(module, "FMOD_ChannelGroup_GetGroup", gFmod.getGroup);
    if (!resolved) return false;
    using ll::memory::FuncPtr;
    if (ll::memory::hook(
            gFmod.update,
            reinterpret_cast<FuncPtr>(&fmodUpdateDetour),
            reinterpret_cast<FuncPtr*>(&gFmod.originalUpdate),
            ll::memory::HookPriority::Normal
        )
        != 0)
        return false;
    if (ll::memory::hook(
            gFmod.playSound,
            reinterpret_cast<FuncPtr>(&fmodPlaySoundDetour),
            reinterpret_cast<FuncPtr*>(&gFmod.originalPlaySound),
            ll::memory::HookPriority::Normal
        )
        != 0) {
        ll::memory::unhook(gFmod.update, reinterpret_cast<FuncPtr>(&fmodUpdateDetour));
        return false;
    }
    gFmod.hooked = true;
    return true;
}

void unhookFmod() {
    if (!gFmod.hooked) return;
    using ll::memory::FuncPtr;
    ll::memory::unhook(gFmod.playSound, reinterpret_cast<FuncPtr>(&fmodPlaySoundDetour));
    ll::memory::unhook(gFmod.update, reinterpret_cast<FuncPtr>(&fmodUpdateDetour));
    gFmod.hooked = false;
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonSimTimerHook,
    ll::memory::HookPriority::Normal,
    ::Timer,
    &::Timer::advanceTime,
    void,
    float preferredFrameStep
) {
    auto const speed  = core::worldSpeedSettings().applied.load(std::memory_order_relaxed);
    bool       scaled = false;
    {
        std::lock_guard const guard(gTimerMutex);
        auto const            it = gScaledTimers.find(this);
        // The game changed the scale since our write (pause, native slow-down); that becomes the new base.
        if (it != gScaledTimers.end() && mTimeScale != it->second.written) it->second.base = mTimeScale;
        if (speed != 1.0f) {
            if (it != gScaledTimers.end()) {
                it->second.written = it->second.base * speed;
                mTimeScale         = it->second.written;
            } else if (isSimTimer(this)) {
                ScaledTimer const entry{mTimeScale, mTimeScale * speed};
                gScaledTimers.emplace(this, entry);
                mTimeScale = entry.written;
            }
        } else if (it != gScaledTimers.end()) {
            mTimeScale = it->second.base;
            gScaledTimers.erase(it);
        }
        scaled = gScaledTimers.contains(this);
    }
    if (speed != 1.0f) logTimerOnce(this, scaled);
    origin(preferredFrameStep);
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonParticleHook,
    ll::memory::HookPriority::Normal,
    ::ParticleSystem::ParticleEmitterActual,
    &::ParticleSystem::ParticleEmitterActual::$tick,
    void,
    ::std::chrono::nanoseconds const& dtIn,
    float const                       a
) {
    auto const& world = core::worldSpeedSettings();
    auto const  speed = world.applied.load(std::memory_order_relaxed);
    if (speed == 1.0f || !world.particles.load(std::memory_order_relaxed)) {
        origin(dtIn, a);
        return;
    }
    auto const scaledDt = std::chrono::duration_cast<std::chrono::nanoseconds>(dtIn * static_cast<double>(speed));
    origin(scaledDt, a);
}

char const* computeBlocker() {
    if (!gInstalled.load(std::memory_order_acquire)) return "optipiston.worldSpeed.unavailable";
    if (core::externalClock().sample()) return "optipiston.worldSpeed.externalClock";
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return "optipiston.worldSpeed.noWorld";
    if (!gLocalServerTicking.load(std::memory_order_acquire)) return "optipiston.worldSpeed.notLocal";
    if (gGuestPlayers.load(std::memory_order_relaxed) > 0) return "optipiston.worldSpeed.multiplayer";
    return nullptr;
}

// Engine counters include the host and non-human players, so count joined guests directly.
PlayerCounts countPlayers(::Level& level) {
    PlayerCounts counts{0, 0, level.getNumRemotePlayers(), level.getActivePlayerCount()};
    level.forEachPlayer([&counts](::Player const& player) {
        ++counts.players;
        if (!player.isHostingPlayer() && !player.isSimulatedPlayer()) ++counts.guests;
        return true;
    });
    return counts;
}

std::vector<ll::event::ListenerPtr> gListeners;

void listen() {
    auto& bus = ll::event::EventBus::getInstance();
    gListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto& event) {
        auto const counts = countPlayers(event.level());
        gGuestPlayers.store(counts.guests, std::memory_order_relaxed);
        gLocalServerTicking.store(true, std::memory_order_release);
        if (counts == gLoggedCounts) return;
        gLoggedCounts = counts;
        ll::mod::NativeMod::current()->getLogger().info(
            "World speed players={} guests={} remote={} active={}",
            counts.players,
            counts.guests,
            counts.remote,
            counts.active
        );
    }));
    // The server's timer is still alive here and gone right after.
    gListeners.push_back(bus.emplaceListener<ll::event::ServerStoppingEvent>([](auto&&) {
        gLocalServerTicking.store(false, std::memory_order_release);
        gLoggedCounts = {-1, -1, -1, -1};
        core::worldSpeedSettings().applied.store(1.0f);
        std::lock_guard const guard(gTimerMutex);
        restoreTimersLocked();
    }));
    gListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&&) {
        gLocalServerTicking.store(false, std::memory_order_release);
        gGuestPlayers.store(0, std::memory_order_relaxed);
        refreshWorldSpeed();
    }));
}

void unlisten() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto const& listener : gListeners) bus.removeListener(listener);
    gListeners.clear();
}

} // namespace

void refreshWorldSpeed() {
    auto&       world   = core::worldSpeedSettings();
    auto const* blocker = computeBlocker();
    gBlocker.store(blocker, std::memory_order_relaxed);
    world.applied.store(blocker ? 1.0f : world.requested.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

char const* worldSpeedBlocker() { return gBlocker.load(std::memory_order_relaxed); }

bool hookWorldSpeed(bool enable) {
    auto& logger = ll::mod::NativeMod::current()->getLogger();
    if (enable) {
        if (gInstalled.load(std::memory_order_acquire)) return true;
        if (OptiPistonSimTimerHook::hook() != 0) return false;
        if (OptiPistonParticleHook::hook() != 0) {
            OptiPistonSimTimerHook::unhook();
            return false;
        }
        if (!hookFmod()) logger.warn("FMOD exports unavailable; world speed will not change audio");
        listen();
        gInstalled.store(true, std::memory_order_release);
        return true;
    }
    if (!gInstalled.load(std::memory_order_acquire)) return true;
    unlisten();
    core::worldSpeedSettings().applied.store(1.0f);
    unhookFmod();
    OptiPistonParticleHook::unhook();
    OptiPistonSimTimerHook::unhook();
    {
        // Tracked timers belong to a running server; a stopped one was already restored and cleared.
        std::lock_guard const guard(gTimerMutex);
        restoreTimersLocked();
    }
    gInstalled.store(false, std::memory_order_release);
    return true;
}

} // namespace optipiston::platform
