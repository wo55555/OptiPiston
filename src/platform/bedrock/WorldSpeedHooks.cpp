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
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#if OPTIPISTON_MC >= 2632
#include "mc/client/entity/systems/ClientInputUpdateSystem.h"
#include "mc/deps/vanilla_components/ActorDataFlagComponent.h"
#include "mc/deps/vanilla_components/MovementAbilitiesComponent.h"
#include "mc/entity/components/ActorOwnerComponent.h"
#include "mc/entity/components/MobEffectsComponent.h"
#include "mc/entity/components/MoveInputComponent.h"
#include "mc/entity/components/PassengerComponent.h"
#include "mc/entity/components/RawMoveInputComponent.h"
#include "mc/entity/components/SneakingComponent.h"
#include "mc/entity/components/WasInWaterFlagComponent.h"
#endif
#include "mc/client/particlesystem/particle/ParticleEmitterActual.h"
#include "mc/client/particlesystem/particle/ParticleSystemEngine.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/EntityRegistry.h"
#include "mc/deps/ecs/systems/IEntitySystemsCollection.h"
#include "mc/deps/ecs/systems/ITickingSystem.h"
#include "mc/deps/ecs/systems/SystemInfo.h"
#include "mc/deps/ecs/systems/TickingSystemId.h"
#if OPTIPISTON_MC >= 2660
#include "mc/deps/vanilla_components/entity/components/ActorTickedComponent.h"
#else
#include "mc/entity/components/ActorTickedComponent.h"
#endif
#include "mc/entity/components/ActorMovementTickNeededComponent.h"
#include "mc/entity/components/IPlayerTickPolicy.h"
#include "mc/entity/components/ServerPlayerMovementComponent.h"
#include "mc/entity/systems/EntitySystems.h"
#include "mc/util/Timer.h"
#include "mc/world/GameCallbacks.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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

using BeforeSimTickFn = void (*)(::GameCallbacks*);
void*           gBeforeSimTickTarget{};
BeforeSimTickFn gBeforeSimTickOrigin{};

bool independentMovement() {
    auto const& world = core::worldSpeedSettings();
    return gInstalled.load(std::memory_order_acquire) && gLocalServerTicking.load(std::memory_order_acquire)
        && gGuestPlayers.load(std::memory_order_relaxed) == 0 && world.normalMovement.load(std::memory_order_relaxed)
        && world.applied.load(std::memory_order_relaxed) != 1.0f;
}

void beforeSimTickDetour(::GameCallbacks* callbacks) {
    if (independentMovement()) {
        auto client = ll::service::getClientInstance();
        if (client && client->getLocalPlayer() && static_cast<::GameCallbacks*>(&*client) == callbacks) return;
    }
    gBeforeSimTickOrigin(callbacks);
}

bool hookMovementInput() {
    auto client = ll::service::getClientInstance();
    if (!client) return false;
    auto* callbacks = static_cast<::GameCallbacks*>(&*client);
    // GameCallbacks::onBeforeSimTick (slot 3).
    gBeforeSimTickTarget = (*reinterpret_cast<void***>(callbacks))[3];
    if (ll::memory::hook(
            gBeforeSimTickTarget,
            reinterpret_cast<ll::memory::FuncPtr>(&beforeSimTickDetour),
            reinterpret_cast<ll::memory::FuncPtr*>(&gBeforeSimTickOrigin),
            ll::memory::HookPriority::Normal
        )
        != 0) {
        gBeforeSimTickTarget = nullptr;
        return false;
    }
    return true;
}

void unhookMovementInput() {
    if (!gBeforeSimTickTarget) return;
    ll::memory::unhook(gBeforeSimTickTarget, reinterpret_cast<ll::memory::FuncPtr>(&beforeSimTickDetour));
    gBeforeSimTickTarget = nullptr;
}

struct ClientMovementSystems {
    ::EntitySystems*  owner{};
    ::ITickingSystem* localRewind{};
    ::ITickingSystem* clientRewind{};
    ::ITickingSystem* publishHistory{};
    ::ITickingSystem* sendInput{};
    uint              addMovementTickFlagId{~0u};
    uint              designateClientId{~0u};
    uint              sendInputId{~0u};
};

ClientMovementSystems gClientMovementSystems;
thread_local bool     gSuppressNativeLocalMovement{false};
thread_local bool     gRestoreTickedAfterSendInput{false};

struct SuppressGuard {
    bool prev;
    explicit SuppressGuard(bool enable) : prev(gSuppressNativeLocalMovement) {
        if (enable) gSuppressNativeLocalMovement = true;
    }
    ~SuppressGuard() {
        gSuppressNativeLocalMovement = prev;
        gRestoreTickedAfterSendInput = false;
    }
};

void stripLocalPlayerSimMovement() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto& ctx = player->getEntityContext();
    ctx.removeComponent<::ActorMovementTickNeededComponent>();
    ctx.removeComponent<::ActorTickedComponent>();
}

bool stripLocalPlayerTicked() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return false;
    auto& ctx = player->getEntityContext();
    if (!ctx.hasComponent<::ActorTickedComponent>()) return false;
    ctx.removeComponent<::ActorTickedComponent>();
    return true;
}

void restoreLocalPlayerTicked() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto& ctx = player->getEntityContext();
    ctx.getRegistry().get_or_emplace<::ActorTickedComponent>(ctx.mEntity);
}

ClientMovementSystems const& ensureClientMovementSystems(::EntitySystems& systems) {
    if (gClientMovementSystems.owner == &systems) return gClientMovementSystems;
    gClientMovementSystems       = {};
    gClientMovementSystems.owner = &systems;
    if (auto* collection = systems.mSystems.get()) {
        uint const limit =
#if OPTIPISTON_MC >= 2632
            static_cast<uint>(collection->getLargestSystemIndex());
#else
            2048u;
#endif
        for (uint i = 0; i < limit; ++i) {
            ::TickingSystemId const id{i};
            auto const*             info = collection->getSystemInfoForTickingSystemId(id);
            if (!info) continue;
            auto* sys = collection->getTickingSystemForTickingSystemId(id);
            if (info->mName == "LocalPlayerRewindSystem") gClientMovementSystems.localRewind = sys;
            else if (info->mName == "ClientPlayerRewindSystem") gClientMovementSystems.clientRewind = sys;
            else if (info->mName == "PublishHistory") gClientMovementSystems.publishHistory = sys;
            else if (info->mName == "SendPlayerInputPacketSystem") {
                gClientMovementSystems.sendInput   = sys;
                gClientMovementSystems.sendInputId = i;
            } else if (info->mName == "AddMovementTickFlag") gClientMovementSystems.addMovementTickFlagId = i;
            else if (info->mName == "ActorMovementTickFilterSystem::designateClientEntities")
                gClientMovementSystems.designateClientId = i;
        }
    }
    auto prevPre             = std::move(systems.mPreSystemInvoke.get());
    auto prevPost            = std::move(systems.mPostSystemInvoke.get());
    systems.mPreSystemInvoke = [prevPre = std::move(prevPre)](::TickingSystemId id) {
        if (prevPre) prevPre(id);
        if (!gSuppressNativeLocalMovement) return;
        if (id.value == gClientMovementSystems.designateClientId) stripLocalPlayerSimMovement();
        else if (id.value == gClientMovementSystems.sendInputId)
            gRestoreTickedAfterSendInput = stripLocalPlayerTicked();
    };
    systems.mPostSystemInvoke = [prevPost = std::move(prevPost)](::TickingSystemId id) {
        if (gSuppressNativeLocalMovement) {
            if (id.value == gClientMovementSystems.addMovementTickFlagId) stripLocalPlayerSimMovement();
            else if (id.value == gClientMovementSystems.sendInputId && gRestoreTickedAfterSendInput) {
                restoreLocalPlayerTicked();
                gRestoreTickedAfterSendInput = false;
            }
        }
        if (prevPost) prevPost(id);
    };
    return gClientMovementSystems;
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonLevelEntitySystemsHook,
    ll::memory::HookPriority::Normal,
    ::Level,
    &::Level::$tickEntitySystems,
    void
) {
    auto       client   = ll::service::getClientInstance();
    auto*      player   = client ? client->getLocalPlayer() : nullptr;
    bool const suppress = independentMovement() && player && &player->getLevel() == this;
    if (suppress) (void)ensureClientMovementSystems(this->getEntitySystems());
    SuppressGuard const guard(suppress);
    origin();
}

#if OPTIPISTON_MC >= 2632
template <class T>
::Optional<T const> optionalMovementComponent(::EntityContext& ctx) {
    return {
        reinterpret_cast<typename ::Optional<T const>::storage_type*>(&ctx.getRegistry().storage<T>()),
        ctx.mEntity
    };
}
#endif

void tickMovementInput(::Timer const* timer) {
    if (!independentMovement()) return;
    auto minecraft = ll::service::getMinecraft(true);
    if (!minecraft || &static_cast<::Timer&>(minecraft->mRealTimer) != timer) return;
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto*       callbacks = static_cast<::GameCallbacks*>(&*client);
    auto&       systems   = player->getLevel().getEntitySystems();
    auto const& sys       = ensureClientMovementSystems(systems);
    for (int i = 0; i < timer->mTicks; ++i) {
        auto& ctx = player->getEntityContext();
#if OPTIPISTON_MC >= 2632
        auto abilities = ctx.tryGetComponent<::MovementAbilitiesComponent>();
        auto moveInput = ctx.tryGetComponent<::MoveInputComponent>();
        auto flags     = ctx.tryGetComponent<::ActorDataFlagComponent>();
        auto effects   = ctx.tryGetComponent<::MobEffectsComponent>();
        auto owner     = ctx.tryGetComponent<::ActorOwnerComponent>();
        auto raw       = ctx.tryGetComponent<::RawMoveInputComponent>();
        if (!abilities || !moveInput || !flags || !effects || !owner || !raw) continue;
        ::ClientInputUpdateSystem::inputHandlerUpdatePlayerState(
            *abilities,
            *effects,
            *flags,
            *owner,
            *moveInput,
            optionalMovementComponent<::PassengerComponent>(ctx),
            optionalMovementComponent<::WasInWaterFlagComponent>(ctx)
        );
        ::ClientInputUpdateSystem::extractRawHIDInput(
            *abilities,
            *moveInput,
            *flags,
            *raw,
            optionalMovementComponent<::SneakingComponent>(ctx),
            optionalMovementComponent<::WasInWaterFlagComponent>(ctx)
        );
#endif
        gBeforeSimTickOrigin(callbacks);
        auto&      registry  = ctx.getRegistry();
        bool const hadTicked = ctx.hasComponent<::ActorTickedComponent>();
        registry.get_or_emplace<::ActorMovementTickNeededComponent>(ctx.mEntity);
        registry.get_or_emplace<::ActorTickedComponent>(ctx.mEntity);
        if (sys.localRewind) sys.localRewind->tick(ctx.mRegistry);
        if (sys.clientRewind) sys.clientRewind->tick(ctx.mRegistry);
        if (sys.publishHistory) sys.publishHistory->tick(ctx.mRegistry);
        systems.tickMovementCorrectionReplay(ctx.mRegistry);
        if (sys.sendInput) sys.sendInput->tick(ctx.mRegistry);
        if (!hadTicked) ctx.removeComponent<::ActorTickedComponent>();
    }
}

bool isSimTimer(::Timer const* timer, bool clientSide) {
    auto minecraft = ll::service::getMinecraft(clientSide);
    return minecraft && &static_cast<::Timer&>(minecraft->mSimTimer) == timer;
}

// Keep the host player's server movement credits topped up so low world speed does not stall local movement.
void creditLocalMovement(::Timer const* timer) {
    auto server = ll::service::getMinecraft(false);
    if (!server || &static_cast<::Timer&>(server->mRealTimer) != timer) return;
    auto* level = server->getLevel();
    if (!level) return;
    level->forEachPlayer([](::Player& player) {
        if (!player.mIsHostingPlayer) return true;
        if (auto movement = player.getEntityContext().tryGetComponent<::ServerPlayerMovementComponent>()) {
            uint64&    credits = movement->mPlayerTickCredits;
            auto const queued  = static_cast<uint64>(movement->mQueuedUpdates.get().size());
            credits            = std::max(credits, std::min<uint64>(queued, 20));
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
    bool const  detached      = client && speed == 0.0f;
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
        if (speed != 1.0f && (client || it != gScaledTimers.end() || isSimTimer(this, false))) {
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
    tickMovementInput(this);
    if (!client && independentMovement()) creditLocalMovement(this);
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
        gGuestPlayers.store(0, std::memory_order_relaxed);
        gClientMovementSystems = {};
        refreshWorldSpeed();
        std::lock_guard const guard(gVisualClockMutex);
        gVisualLevel = nullptr;
        gVisualClock = {};
    }));
}

void unlisten() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto const& listener : gListeners) bus.removeListener(listener);
    gListeners.clear();
    gClientMovementSystems = {};
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
        if (!hookMovementInput()) return false;
        if (OptiPistonLevelEntitySystemsHook::hook() != 0) {
            unhookMovementInput();
            return false;
        }
        if (OptiPistonSimTimerHook::hook() != 0) {
            OptiPistonLevelEntitySystemsHook::unhook();
            unhookMovementInput();
            return false;
        }
        if (OptiPistonParticleHook::hook() != 0) {
            OptiPistonSimTimerHook::unhook();
            OptiPistonLevelEntitySystemsHook::unhook();
            unhookMovementInput();
            return false;
        }
        if (OptiPistonParticleEmitHook::hook() != 0)
            logger.warn("Particle emitter hook unavailable; ambient particles may pile up at low speed");
        if (!hookFmod()) logger.warn("FMOD exports unavailable; world speed will not change audio");
        listen();
        gInstalled.store(true, std::memory_order_release);
        return true;
    }
    if (!gInstalled.load(std::memory_order_acquire)) return true;
    unlisten();
    core::worldSpeedSettings().applied.store(1.0f);
    unhookFmod();
    OptiPistonParticleEmitHook::unhook();
    OptiPistonParticleHook::unhook();
    OptiPistonSimTimerHook::unhook();
    OptiPistonLevelEntitySystemsHook::unhook();
    unhookMovementInput();
    {
        // Tracked timers belong to a running server; a stopped one was already restored and cleared.
        std::lock_guard const guard(gTimerMutex);
        restoreTimersLocked();
    }
    gInstalled.store(false, std::memory_order_release);
    return true;
}

} // namespace optipiston::platform
