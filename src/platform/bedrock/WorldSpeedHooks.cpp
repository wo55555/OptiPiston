#include "platform/Features.h"

#include "core/Clock.h"
#include "core/Settings.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Memory.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/particlesystem/particle/ParticleEmitterActual.h"
#include "mc/client/particlesystem/particle/ParticleSystemEngine.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/IPlayerTickPolicy.h"
#include "mc/entity/components/ServerPlayerMovementComponent.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/util/Timer.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/FallingBlockActor.h"
#include "mc/world/actor/item/ITickDelegate.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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

std::mutex             gVisualClockMutex;
core::WorldVisualClock gVisualClock;
::Level const*         gVisualLevel{};

bool isSimTimer(::Timer const* timer, bool clientSide) {
    auto minecraft = ll::service::getMinecraft(clientSide);
    return minecraft && &static_cast<::Timer&>(minecraft->mSimTimer) == timer;
}

std::atomic_bool gDrainMovementBacklog{false};

// Keep the host player's server movement credits topped up so low/paused world speed does not stall or rewind movement.
void creditLocalMovement(::Timer const* timer, float speed) {
    auto server = ll::service::getMinecraft(false);
    if (!server || &static_cast<::Timer&>(server->mRealTimer) != timer) return;
    if (speed < 1.0f) {
        gDrainMovementBacklog.store(true, std::memory_order_relaxed);
    } else if (!gDrainMovementBacklog.load(std::memory_order_relaxed)) {
        return;
    }
    auto* level = server->getLevel();
    if (!level) return;
    level->forEachPlayer([speed](::Player& player) {
        if (!player.mIsHostingPlayer) return true;
        if (auto movement = player.getEntityContext().tryGetComponent<::ServerPlayerMovementComponent>()) {
            uint64&    credits = movement->mPlayerTickCredits;
            auto const queued  = static_cast<uint64>(movement->mQueuedUpdates.get().size());
            if (speed >= 1.0f && queued <= 2) {
                gDrainMovementBacklog.store(false, std::memory_order_relaxed);
            } else {
                credits = std::max(credits, std::min<uint64>(queued, 1000));
            }
        }
        return false;
    });
}

std::optional<float> updateVisualClock(bool detached, float alpha, double delta = 0.0) {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return std::nullopt;
    auto const&           level = player->getLevel();
    std::lock_guard const guard(gVisualClockMutex);
    if (gVisualLevel != &level) {
        gVisualClock = {};
        gVisualLevel = &level;
    }
    return gVisualClock.advance(static_cast<int64_t>(level.getCurrentTick().tickID), detached, alpha, delta);
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
    auto const  speed = world.applied.load(std::memory_order_relaxed);
    return world.audio.load(std::memory_order_relaxed) && speed > 0.0f ? speed : 1.0f;
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

#if OPTIPISTON_MC >= 2660
#define OPTIPISTON_TIMER_PARAMS float preferredFrameStep, bool forceOnlySingleTick
#define OPTIPISTON_TIMER_ARGS   preferredFrameStep, forceOnlySingleTick
#else
#define OPTIPISTON_TIMER_PARAMS float preferredFrameStep
#define OPTIPISTON_TIMER_ARGS   preferredFrameStep
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
LL_TYPE_INSTANCE_HOOK(
    OptiPistonSimTimerHook,
    ll::memory::HookPriority::Normal,
    ::Timer,
    &::Timer::advanceTime,
    void,
    OPTIPISTON_TIMER_PARAMS
) {
    auto const& world  = core::worldSpeedSettings();
    bool const  client = isSimTimer(this, true);
    if (client && world.paused.load(std::memory_order_relaxed)) refreshWorldSpeed();
    auto const  speed         = world.applied.load(std::memory_order_relaxed);
    bool const  allowMovement = client && world.normalMovement.load(std::memory_order_relaxed);
    bool const  detached      = client && (speed == 0.0f || (speed != 1.0f && allowMovement));
    float const beforeAlpha   = mAlpha;
    if (client && !detached) {
        if (auto const held = updateVisualClock(false, beforeAlpha)) {
            mPassedTime = *held;
            mAlpha      = *held;
        }
    }
    bool       scaled = false;
    {
        std::lock_guard const guard(gTimerMutex);
        auto const            it = gScaledTimers.find(this);
        // The game changed the scale since our write (pause, native slow-down); that becomes the new base.
        if (it != gScaledTimers.end() && mTimeScale != it->second.written) it->second.base = mTimeScale;
        if (speed != 1.0f && !allowMovement && (client || it != gScaledTimers.end() || isSimTimer(this, false))) {
            if (it != gScaledTimers.end()) {
                it->second.written = it->second.base * speed;
                mTimeScale         = it->second.written;
            } else {
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
    origin(OPTIPISTON_TIMER_ARGS);
    if (!client && world.normalMovement.load(std::memory_order_relaxed)
        && gGuestPlayers.load(std::memory_order_relaxed) == 0)
        creditLocalMovement(this, speed);
    if (detached) {
        auto const delta = static_cast<double>(static_cast<float>(mLastTimestep)) * static_cast<float>(mTimeScale)
                         * static_cast<float>(mTicksPerSecond) * speed;
        (void)updateVisualClock(true, beforeAlpha, std::max(delta, 0.0));
    }
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

// Ambient emitters such as campfire smoke are fed one particle at a time by game code that keeps its normal
// rate. Particle lifetimes stretch with the scaled step, so the same fraction of those emissions is dropped
// to keep the density unchanged.
bool dropAmbientEmission() {
    auto const& world = core::worldSpeedSettings();
    auto const  speed = world.applied.load(std::memory_order_relaxed);
    if (speed >= 1.0f || !world.particles.load(std::memory_order_relaxed)) return false;
    thread_local std::minstd_rand rng{std::random_device{}()};
    return speed <= 0.0f || std::uniform_real_distribution<float>{0.0f, 1.0f}(rng) >= speed;
}

#if OPTIPISTON_MC >= 2620
using ParticleEmitterHandle = Bedrock::slot_map_handle<std::unique_ptr<ParticleSystem::ParticleEmitter>, 64, 32>;

LL_TYPE_INSTANCE_HOOK(
    OptiPistonParticleEmitHook,
    ll::memory::HookPriority::Normal,
    ::ParticleSystemEngine,
    &::ParticleSystemEngine::emitParticleManually,
    void,
    ParticleEmitterHandle handle,
    ::Vec3 const&         pos,
    ::Vec3 const&         vel,
    float                 scale
) {
    if (!dropAmbientEmission()) origin(handle, pos, vel, scale);
}
#else
LL_TYPE_INSTANCE_HOOK(
    OptiPistonParticleEmitHook,
    ll::memory::HookPriority::Normal,
    ::ParticleSystem::ParticleEmitterActual,
    &::ParticleSystem::ParticleEmitterActual::$emitParticleManually,
    void,
    ::Vec3 const& pos,
    ::Vec3 const& vel,
    float const   scale
) {
    if (!dropAmbientEmission()) origin(pos, vel, scale);
}
#endif

// ---- FallingBlockActor: keep client-side falling block simulation and interpolation on WorldVisualClock ----

struct FallingBlockState {
    ::Vec3  prevPos{};
    ::Vec3  curPos{};
    int64_t lastClientTick{};
};

struct FallingBlockSync {
    ::Level const*                               level{};
    std::optional<int64_t>                       lastClientTick;
    int64_t                                      lastVisualTick{};
    int                                          stepsForTick{};
    std::unordered_map<int64, FallingBlockState> actors;

    void reset() {
        level = nullptr;
        lastClientTick.reset();
        lastVisualTick = 0;
        stepsForTick   = 0;
        actors.clear();
    }
};

std::mutex           gFallingBlockMutex;
FallingBlockSync     gFallingBlocks;
std::atomic_bool     gFallingSteppedValid{false};
std::atomic<int64_t> gFallingSteppedVisualTick{0};

void resetFallingBlockSync() {
    std::lock_guard const guard(gFallingBlockMutex);
    gFallingBlocks.reset();
    gFallingSteppedValid.store(false, std::memory_order_relaxed);
}

float distSquared(::Vec3 const& a, ::Vec3 const& b) noexcept {
    float const dx = a.x - b.x;
    float const dy = a.y - b.y;
    float const dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

#if OPTIPISTON_MC == 2660
using ll::memory_literals::operator""_sig;
// FallingBlockActor::doNormalTick
#define OPTIPISTON_FALLING_BLOCK_DO_NORMAL_TICK                                                                        \
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 85 00 01 00 00 0F 29 "  \
    "BD F0 00 00 00 0F 29 B5 E0 00 00 00 48 C7 85 D8 00 00 00 FE FF FF FF 49 89 D4 48 89 CE 48 83 B9 F8 03 00 00 00 "  \
    "0F 84 ?? ?? ?? ?? 48 8B 8E 00 02 00 00 48 8B 01 48 8B 80 E0 09 00 00 48 8D 5D B0 48 89 DA FF 15 ?? ?? ?? ??"_sig
#else
#define OPTIPISTON_FALLING_BLOCK_DO_NORMAL_TICK &::FallingBlockActor::doNormalTick
#endif

LL_TYPE_INSTANCE_HOOK(
    OptiPistonFallingBlockTickHook,
    ll::memory::HookPriority::Normal,
    ::FallingBlockActor,
    OPTIPISTON_FALLING_BLOCK_DO_NORMAL_TICK,
    void,
    ::ITickDelegate& tickDelegate
) {
    if (!this->mLevel) {
        origin(tickDelegate);
        return;
    }
    auto const& level = this->getLevel();
    if (!level.isClientSide()) {
        origin(tickDelegate);
        return;
    }

    auto const sample = worldVisualClock(level);
    if (!sample.partial) {
        {
            std::lock_guard const guard(gFallingBlockMutex);
            if (gFallingBlocks.level == &level && (gFallingBlocks.lastClientTick || !gFallingBlocks.actors.empty())) {
                gFallingBlocks.reset();
                gFallingSteppedValid.store(false, std::memory_order_relaxed);
            }
        }
        origin(tickDelegate);
        return;
    }

    auto const  clientTick = static_cast<int64_t>(level.getCurrentTick().tickID);
    auto const  actorId    = this->getOrCreateUniqueID().rawID;
    float const speed      = core::worldSpeedSettings().applied.load(std::memory_order_relaxed);
    auto        state      = this->getEntityContext().tryGetComponent<::StateVectorComponent>();

    int steps = 0;
    {
        std::lock_guard const guard(gFallingBlockMutex);
        if (gFallingBlocks.level != &level) {
            gFallingBlocks.reset();
            gFallingBlocks.level = &level;
            gFallingSteppedValid.store(false, std::memory_order_relaxed);
        }

        if (gFallingBlocks.lastClientTick != clientTick) {
            std::erase_if(gFallingBlocks.actors, [clientTick](auto const& entry) {
                return entry.second.lastClientTick < clientTick - 40;
            });

            if (!gFallingBlocks.lastClientTick || clientTick <= *gFallingBlocks.lastClientTick
                || clientTick - *gFallingBlocks.lastClientTick > 2) {
                gFallingBlocks.lastVisualTick = sample.tick;
                gFallingBlocks.stepsForTick   = 0;
            } else if (speed <= 0.0f) {
                gFallingBlocks.lastVisualTick = sample.tick;
                gFallingBlocks.stepsForTick   = 0;
            } else {
                auto const delta              = sample.tick - gFallingBlocks.lastVisualTick;
                auto const clamped            = static_cast<int>(std::clamp<int64_t>(delta, 0, 8));
                gFallingBlocks.stepsForTick   = clamped;
                gFallingBlocks.lastVisualTick += clamped;
            }
            gFallingBlocks.lastClientTick = clientTick;
            gFallingSteppedVisualTick.store(gFallingBlocks.lastVisualTick, std::memory_order_relaxed);
            gFallingSteppedValid.store(true, std::memory_order_release);
        }

        steps = gFallingBlocks.stepsForTick;
        if (steps <= 0) {
            if (state) {
                auto const cur = state->mPos.get();
                auto const it  = gFallingBlocks.actors.find(actorId);
                if (it != gFallingBlocks.actors.end()) {
                    auto& cached          = it->second;
                    cached.lastClientTick = clientTick;
                    float const d2        = distSquared(cur, cached.curPos);
                    if (d2 <= 1e-10f) {
                        // Unchanged on a skipped client tick: restore mPosPrev so render interpolation stays smooth.
                        state->mPosPrev.get() = cached.prevPos;
                    } else if (d2 < 4.0f) {
                        // Small external reconciliation (< 2 blocks): keep the previous interpolation anchor.
                        cached.curPos         = cur;
                        state->mPosPrev.get() = cached.prevPos;
                    } else {
                        // Large jump/teleport: snap interpolation anchor to avoid streaking.
                        cached.prevPos        = cur;
                        cached.curPos         = cur;
                        state->mPosPrev.get() = cur;
                    }
                } else {
                    gFallingBlocks.actors.emplace(
                        actorId,
                        FallingBlockState{state->mPosPrev.get(), cur, clientTick}
                    );
                }
            }
            return;
        }
    }

    for (int i = 0; i < steps; ++i) {
        if (this->mRemoved) break;
        origin(tickDelegate);
    }

    if (auto postState = this->getEntityContext().tryGetComponent<::StateVectorComponent>()) {
        std::lock_guard const guard(gFallingBlockMutex);
        if (this->mRemoved) {
            gFallingBlocks.actors.erase(actorId);
        } else {
            gFallingBlocks.actors[actorId] = FallingBlockState{
                postState->mPosPrev.get(),
                postState->mPos.get(),
                clientTick,
            };
        }
    }
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonFallingBlockInterpHook,
    ll::memory::HookPriority::Normal,
    ::Actor,
    &::Actor::getInterpolatedPosition,
    ::Vec3,
    float a
) {
    if (this->getEntityTypeId() == ::ActorType::FallingBlock && this->mLevel && this->getLevel().isClientSide()) {
        if (auto const sample = worldVisualClock(this->getLevel()); sample.partial) {
            float alpha = *sample.partial;
            // If WorldVisualClock already crossed to the next visual tick between 20 Hz client ticks, clamp at 1
            // until doNormalTick steps mPosPrev/mPos so the falling block never snaps backwards.
            if (gFallingSteppedValid.load(std::memory_order_acquire)
                && sample.tick > gFallingSteppedVisualTick.load(std::memory_order_relaxed)) {
                alpha = 1.0f;
            }
            return origin(alpha);
        }
    }
    return origin(a);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

bool gFallingTickHooked{false};
bool gFallingInterpHooked{false};

void hookFallingBlocks(ll::io::Logger& logger) {
    if (OptiPistonFallingBlockTickHook::hook() != 0) {
        logger.warn("FallingBlock tick hook unavailable; falling blocks will tick at client rate");
        return;
    }
    gFallingTickHooked = true;
    if (OptiPistonFallingBlockInterpHook::hook() != 0) {
        OptiPistonFallingBlockTickHook::unhook();
        gFallingTickHooked = false;
        logger.warn("FallingBlock interp hook unavailable; falling blocks will use native frame alpha");
        return;
    }
    gFallingInterpHooked = true;
}

void unhookFallingBlocks() {
    if (gFallingInterpHooked) {
        OptiPistonFallingBlockInterpHook::unhook();
        gFallingInterpHooked = false;
    }
    if (gFallingTickHooked) {
        OptiPistonFallingBlockTickHook::unhook();
        gFallingTickHooked = false;
    }
    resetFallingBlockSync();
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
        if (!player.mIsHostingPlayer && !player.isSimulatedPlayer()) ++counts.guests;
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
    gListeners.push_back(bus.emplaceListener<ll::event::PlayerJoinEvent>(
        [](auto& event) {
            auto const& player = event.self();
            if (!event.isCancelled() && !player.mIsHostingPlayer && !player.isSimulatedPlayer()) {
                gGuestPlayers.fetch_add(1, std::memory_order_relaxed);
                core::worldSpeedSettings().applied.store(1.0f, std::memory_order_relaxed);
            }
        },
        ll::event::EventPriority::Lowest
    ));
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
        gDrainMovementBacklog.store(false, std::memory_order_relaxed);
        gGuestPlayers.store(0, std::memory_order_relaxed);
        refreshWorldSpeed();
        {
            std::lock_guard const guard(gVisualClockMutex);
            gVisualLevel = nullptr;
            gVisualClock = {};
        }
        resetFallingBlockSync();
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
    auto const target =
        world.paused.load(std::memory_order_relaxed) ? 0.0f : world.requested.load(std::memory_order_relaxed);
    world.applied.store(blocker ? 1.0f : target, std::memory_order_relaxed);
}

char const* worldSpeedBlocker() { return gBlocker.load(std::memory_order_relaxed); }

core::ClockSample worldVisualClock(::Level const& level) {
    auto const            tick = static_cast<int64_t>(level.getCurrentTick().tickID);
    std::lock_guard const guard(gVisualClockMutex);
    return gVisualLevel == &level ? gVisualClock.sample(tick) : core::ClockSample{tick, 0, false};
}

bool hookWorldSpeed(bool enable) {
    auto& logger = ll::mod::NativeMod::current()->getLogger();
    if (enable) {
        if (gInstalled.load(std::memory_order_acquire)) return true;
        if (OptiPistonSimTimerHook::hook() != 0) return false;
        if (OptiPistonParticleHook::hook() != 0) {
            OptiPistonSimTimerHook::unhook();
            return false;
        }
        if (OptiPistonParticleEmitHook::hook() != 0)
            logger.warn("Particle emitter hook unavailable; ambient particles may pile up at low speed");
        hookFallingBlocks(logger);
        if (!hookFmod()) logger.warn("FMOD exports unavailable; world speed will not change audio");
        listen();
        gInstalled.store(true, std::memory_order_release);
        return true;
    }
    if (!gInstalled.load(std::memory_order_acquire)) return true;
    unlisten();
    core::worldSpeedSettings().applied.store(1.0f);
    unhookFmod();
    unhookFallingBlocks();
    OptiPistonParticleEmitHook::unhook();
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
