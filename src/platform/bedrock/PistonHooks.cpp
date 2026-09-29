#include "platform/Features.h"

#include "core/Clock.h"
#include "core/Segment.h"
#include "core/Settings.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/block/BlockOccluder.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderData.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/PistonBlockActorRenderer.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/client/renderer/chunks/RenderChunkGeometry.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/BlockActorDataPacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksChangedInfo.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#if OPTIPISTON_MC < 2651
#include "mc/platform/threading/Mutex.h"
#endif
#include "mc/world/actor/ActorTerrainInterlockData.h"
#include "mc/world/level/BlockPalette.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/VanillaBlockTypeIds.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/MovingBlockActor.h"
#include "mc/world/level/block/actor/PistonBlockActor.h"
#include "mc/world/level/block/actor/PistonState.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/LevelChunkBlockActorStorage.h"
#include "mc/world/phys/AABB.h"

#if OPTIPISTON_MC == 2610
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/chunks/RenderChunkBuilder.h"
#include "mc/deps/minecraft_renderer/framebuilder/dragon/RenderMetadata.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/world/level/block/PistonBlock.h"
#include "mc/world/level/block/VanillaStates.h"
#endif
#if OPTIPISTON_MC != 2620
#include "mc/world/level/BlockSourceListener.h"
#endif
#if OPTIPISTON_MC >= 2632
#include "mc/world/level/block/actor/VanillaBlockActor.h"
#include "mc/world/level/block/actor/component/IVanillaRenderBlockActorComponent.h"
#endif

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace optipiston::platform {

namespace {

using VisibilityState = ::ActorTerrainInterlockData::VisibilityState;
using core::Segment;

std::atomic_bool gInstalled{false};

bool smoothPistonRenderEnabled() { return core::pistonSettings().enabled.load(std::memory_order_relaxed); }

struct BlockPosHash {
    [[nodiscard]] size_t operator()(::BlockPos const& pos) const noexcept {
        return (static_cast<size_t>(static_cast<uint32_t>(pos.x)) * 73856093u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.y)) * 19349663u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.z)) * 83492791u);
    }
};

// ---- Version adapters: the only places that know which Minecraft API this build targets ----

::ActorTerrainInterlockData& interlockOf(::BlockActor& actor) {
#if OPTIPISTON_MC >= 2632
    // Only called for pistons and moving blocks, both vanilla block actors.
    return static_cast<::VanillaBlockActor&>(actor).mTerrainInterlockData.get();
#else
    return actor.mTerrainInterlockData.get();
#endif
}

std::string wrappedName(::MovingBlockActor const& moving) {
#if OPTIPISTON_MC == 2620
    return moving.getWrappedBlock().getTypeName();
#else
    ::Block const* block = moving.mWrappedBlock;
    return block ? std::string{block->getTypeName()} : std::string{"minecraft:air"};
#endif
}

bool wrappedAir(::MovingBlockActor const& moving) {
#if OPTIPISTON_MC == 2620
    return moving.getWrappedBlock().isAir();
#else
    ::Block const* block = moving.mWrappedBlock;
    return !block || block->isAir();
#endif
}

::PistonBlockActor* owningPiston(::MovingBlockActor& moving, ::BlockSource& region) {
#if OPTIPISTON_MC == 2620
    return moving.getOwningPiston(region);
#else
    auto* actor = region.getBlockEntity(moving.mPistonBlockPos.get());
    return actor && actor->mType == ::BlockActorType::PistonArm ? static_cast<::PistonBlockActor*>(actor) : nullptr;
#endif
}

::BlockPos facingOf(::PistonBlockActor const& piston, ::BlockSource& region) {
#if OPTIPISTON_MC == 2610
    auto const facing = region.getBlock(piston.mPosition.get()).getState<int>(::VanillaStates::FacingDirection());
    if (!facing || *facing < 0 || *facing > 5) return {0, 0, 0};
    return ::PistonBlock::ARM_DIRECTION_OFFSETS()[*facing];
#else
    return piston.getFacingDir(region);
#endif
}

void fireAreaChanged(::BlockSource& region, ::BlockPos const& min, ::BlockPos const& max) {
#if OPTIPISTON_MC == 2620
    region.fireAreaChanged(min, max);
#else
    // Copied: a listener may unregister itself while being notified.
    auto const listeners = region.mListeners.get();
    for (auto* listener : listeners) listener->onAreaChanged(region, min, max);
#endif
}

#if OPTIPISTON_MC >= 2632
using QueuedItem = ::IVanillaRenderBlockActorComponent;
QueuedItem* queuedItemOf(::BlockActor* actor) { return actor->_getRenderComponent(); }
auto&       opaqueQueueOf(::LevelRendererCamera& camera) { return camera.mRenderComponentRenderQueue.get(); }
auto&       alphaQueueOf(::LevelRendererCamera& camera) { return camera.mRenderComponentRenderAlphaQueue.get(); }
auto&       shadowQueueOf(::LevelRendererCamera& camera) { return camera.mRenderComponentShadowQueue.get(); }
#else
using QueuedItem = ::BlockActor;
QueuedItem* queuedItemOf(::BlockActor* actor) { return actor; }
auto&       opaqueQueueOf(::LevelRendererCamera& camera) { return camera.mBlockActorRenderQueue.get(); }
auto&       alphaQueueOf(::LevelRendererCamera& camera) { return camera.mBlockActorRenderAlphaQueue.get(); }
auto&       shadowQueueOf(::LevelRendererCamera& camera) { return camera.mBlockActorShadowQueue.get(); }
#endif

// Tick at which a MovingBlock was seen detached from its cell; the interlock struct has no field to reuse for this.
std::unordered_map<::MovingBlockActor const*, uint64_t> gTailAnchors;

// Replay rebuilds PistonBlockActors without retiring the old ones, leaving several on one cell that the engine draws a
// head for each. Animating ones advance mProgress in $tick; leftovers never do, so stepping distinguishes the two.
std::mutex                              gSteppedPistonMutex;
std::unordered_set<::BlockActor const*> gSteppedPistons;

// Ownership is scoped to one round: a destroyed piston's address cannot be detected, and a claim outliving its owner
// locked the cell for good, leaving frames with no head at all. Re-electing each round bounds a stale address to the
// round it was seen in. The round advances on PistonBlockActor::$tick, which runs once per game tick.
std::atomic<uint64_t> gHeadOwnerRound{0};

struct HeadOwner {
    uint64_t            round{};
    ::BlockActor const* actor{};
    int                 rank{};
};

std::mutex                                              gHeadOwnerMutex;
std::unordered_map<::BlockPos, HeadOwner, BlockPosHash> gHeadOwners;

// ---- Render-only animation on the visual clock; native progress, state and world blocks stay untouched ----

// Non-zero while a piston or moving-block renderer is on this thread's stack; logic callers keep native values.
thread_local int tRenderDepth = 0;

struct RenderScope {
    RenderScope() { ++tRenderDepth; }
    ~RenderScope() { --tRenderDepth; }
    RenderScope(RenderScope const&)            = delete;
    RenderScope& operator=(RenderScope const&) = delete;
};

// MovingBlock being drawn on this thread; it outlives the piston it carries, which data packets rebuild mid-push.
thread_local ::BlockActor const* tCarrier = nullptr;

struct CarrierScope {
    explicit CarrierScope(::BlockActor const* carrier) : saved(tCarrier) { tCarrier = carrier; }
    ~CarrierScope() { tCarrier = saved; }
    CarrierScope(CarrierScope const&)                  = delete;
    CarrierScope&       operator=(CarrierScope const&) = delete;
    ::BlockActor const* saved;
};

using VisualSegment = Segment;

struct ActionVisual {
    VisualSegment segment;
    ::BlockPos    facing;
};

struct PistonVisual {
    uint64_t      action{};
    VisualSegment current;
    // A truncated predecessor keeps drawing the arm until the new action's start tick.
    std::optional<VisualSegment> previous;
    // Tail of the push that carried this piston's body here, still playing when its own action starts.
    std::optional<ActionVisual> bodyCarry;
};

// Tail of the action that carried a block into the cell a newer action picks it up from.
struct CarryVisual {
    uint64_t     action{};
    ActionVisual visual;
};

// One MovingBlock target cell owned by an action.
struct CellClaim {
    uint64_t    action{};
    std::string wrapped;
    bool        landed{};    // the real block arrived and its chunk mesh is suppressed
    bool        released{};  // visual ended, the real block is being re-meshed
    bool        rebuilt{};   // a build containing the real block has committed
    uint64_t    liveFrame{}; // frame of that commit; the mesh is on screen from the next frame
};

std::mutex         gAnimMutex;
uint64_t           gNextAction = 1;
core::ClockTracker gClockTracker; // guarded by gAnimMutex
// Keyed by position: replay rebuilds piston instances mid-action.
std::unordered_map<::BlockPos, PistonVisual, BlockPosHash> gPistonVisuals;
std::unordered_map<uint64_t, ActionVisual>                 gActionVisuals;
struct MovingEntry {
    uint64_t     action{};
    ::BlockPos   cell;
    ActionVisual visual; // copied, so a successor can still replay its tail after the action ends
    // A newer action already draws this block, so this copy is never drawn again.
    bool handedOff{};
    // Its cell is re-meshed with the real block, so drawing it again would double it.
    bool                       released{};
    std::optional<CarryVisual> carry;
    // The chunk may destroy a detached instance before its cell is re-meshed.
    std::shared_ptr<::BlockActor> keep;
};
// Released outside gAnimMutex because the dtor hook locks it.
std::vector<std::shared_ptr<::BlockActor>> gDropKeep;

std::unordered_map<::MovingBlockActor const*, MovingEntry> gMovingAction;
std::unordered_map<::BlockPos, CellClaim, BlockPosHash>    gCellClaims;
std::atomic_bool                                           gMeshWatch{false};
std::atomic_bool                                           gFaceWatch{false}; // any unreleased claim
std::atomic<uint64_t>                                      gFrameLt{0};
// Counts frames that drew block actors; export also renders passes without them.
std::atomic<uint64_t> gFrame{1};
std::atomic_bool      gFrameDrewActors{false};
// Chunk geometry being tessellated on this thread.
thread_local ::RenderChunkGeometry const* tMeshGeometry = nullptr;
// Released cells tessellated as real blocks, per build that has not been uploaded yet.
std::unordered_map<::RenderChunkGeometry const*, std::vector<::BlockPos>> gReleasedMesh;
// Landed cells found by the renderer; the next tick re-meshes them.
std::vector<::BlockPos> gPendingRemesh;

// An external clock (e.g. a replay) wins; in a live world the client level tick runs 1:1 with redstone.
std::optional<core::ClockSample> visualClock() {
    if (auto external = core::externalClock().sample()) return external;
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return std::nullopt;
    return core::ClockSample{static_cast<int64_t>(player->getLevel().getCurrentTick().tickID), 0, false};
}

std::optional<int64_t> visualClockTick() {
    auto const sample = visualClock();
    if (!sample) return std::nullopt;
    return sample->tick;
}

bool animationActive() { return smoothPistonRenderEnabled() && visualClockTick().has_value(); }

// Native alpha keeps cycling while a replay is paused, so the clock owner's fraction wins.
double visualTime(float alpha) {
    auto const sample = visualClock();
    if (!sample) return core::visualTime(0, alpha);
    return core::visualTime(sample->tick, sample->partial.value_or(alpha));
}

using core::segmentValue;

// An interrupted predecessor keeps playing its remaining tail on top of the new action.
float armValue(PistonVisual const& visual, double time) {
    auto value = segmentValue(visual.current, time);
    if (visual.previous) value += segmentValue(*visual.previous, time) - visual.previous->to;
    return std::clamp(value, 0.0f, 1.0f);
}

// Relative to the target cell, same convention as native getDrawPos.
::Vec3 actionOffset(ActionVisual const& visual, double time) {
    auto const rel = segmentValue(visual.segment, time) - visual.segment.to;
    return ::Vec3{
        static_cast<float>(visual.facing.x) * rel,
        static_cast<float>(visual.facing.y) * rel,
        static_cast<float>(visual.facing.z) * rel
    };
}

void updateMeshWatchLocked() {
    gMeshWatch.store(
        std::any_of(
            gCellClaims.begin(),
            gCellClaims.end(),
            [](auto const& entry) { return entry.second.landed && !entry.second.rebuilt; }
        ),
        std::memory_order_relaxed
    );
    gFaceWatch.store(
        std::any_of(gCellClaims.begin(), gCellClaims.end(), [](auto const& entry) { return !entry.second.released; }),
        std::memory_order_relaxed
    );
}

// Returns cells whose mesh is still missing the landed block.
std::vector<::BlockPos> clearAnimationStateLocked() {
    std::vector<::BlockPos> cells;
    for (auto const& [pos, claim] : gCellClaims)
        if (claim.landed && !claim.rebuilt) cells.push_back(pos);
    gPistonVisuals.clear();
    gActionVisuals.clear();
    for (auto& [moving, entry] : gMovingAction)
        if (entry.keep) gDropKeep.push_back(std::move(entry.keep));
    gMovingAction.clear();
    gPendingRemesh.clear();
    gCellClaims.clear();
    gReleasedMesh.clear();
    gClockTracker.reset();
    updateMeshWatchLocked();
    return cells;
}

void clearAnimationState() {
    std::lock_guard const guard(gAnimMutex);
    clearAnimationStateLocked();
}

// Caller holds gAnimMutex. Returns landed cells whose real block must be re-meshed now.
std::vector<::BlockPos> endActionLocked(uint64_t action) {
    gActionVisuals.erase(action);
    std::vector<::BlockPos> cells;
    for (auto it = gCellClaims.begin(); it != gCellClaims.end();) {
        auto& claim = it->second;
        if (claim.action != action || claim.released) {
            ++it;
        } else if (!claim.landed) {
            it = gCellClaims.erase(it);
        } else {
            claim.released = true;
            cells.push_back(it->first);
            ++it;
        }
    }
    return cells;
}

// Set while our own re-mesh requests pass through the level listeners.
thread_local bool tImmediateRebuild = false;

void rebuildCells(::BlockSource& region, std::vector<::BlockPos> const& cells) {
    tImmediateRebuild = true;
    // Neighbours kept faces against the hidden block and may sit in another subchunk.
    for (auto const& pos : cells) fireAreaChanged(region, pos - ::BlockPos{1, 1, 1}, pos + ::BlockPos{1, 1, 1});
    tImmediateRebuild = false;
}

// Caller holds gAnimMutex. Visual ticks mean nothing after a clock switch, a new epoch or a backward jump.
bool clockInvalidatedLocked() {
    auto const sample = visualClock();
    return sample && gClockTracker.invalidated(*sample);
}

bool hasAnimationStateLocked() {
    return !gPistonVisuals.empty() || !gActionVisuals.empty() || !gCellClaims.empty() || !gMovingAction.empty();
}

// Ends visuals past their duration, or ahead of the clock after a backward seek.
void sweepActions(::BlockSource& region) {
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const guard(gAnimMutex);
        bool const            active = animationActive();
        if ((!active || clockInvalidatedLocked()) && hasAnimationStateLocked()) cells = clearAnimationStateLocked();
    }
    if (!cells.empty()) {
        rebuildCells(region, cells);
        return;
    }
    {
        std::lock_guard const guard(gAnimMutex);
        cells.swap(gPendingRemesh);
    }
    rebuildCells(region, cells);
    cells.clear();
    {
        std::lock_guard const guard(gAnimMutex);
        if (!animationActive()) return;
        auto const tick = visualClockTick().value_or(0);
        if (!gClockTracker.advance(tick)) return;
        auto const expired = [tick](VisualSegment const& segment) { return core::segmentExpired(segment, tick); };
        std::erase_if(gPistonVisuals, [&](auto const& entry) { return expired(entry.second.current); });
        std::vector<uint64_t> ended;
        for (auto const& [action, visual] : gActionVisuals)
            if (expired(visual.segment)) ended.push_back(action);
        for (auto const action : ended) {
            auto released = endActionLocked(action);
            cells.insert(cells.end(), released.begin(), released.end());
        }
        updateMeshWatchLocked();
    }
    rebuildCells(region, cells);
}

// Worker threads build chunk meshes; the MovingBlock renderer tessellates on the render thread and must pass.
bool meshSuppressed(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gMeshWatch.load(std::memory_order_relaxed)) return false;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(pos);
    if (it == gCellClaims.end() || !it->second.landed) return false;
    auto& claim = it->second;
    if (!claim.released) return true;
    // Counts only once this build is uploaded; a superseded build never reaches endRebuild.
    if (!claim.rebuilt && tMeshGeometry) gReleasedMesh[tMeshGeometry].push_back(pos);
    return false;
}

// A newer build of the same geometry replaces the pending one.
void meshBuildStarted(::RenderChunkGeometry const* geometry) {
    std::lock_guard const guard(gAnimMutex);
    gReleasedMesh.erase(geometry);
}

// Runs on the frame thread once the rebuilt geometry is live.
void meshBuildCommitted(::RenderChunkGeometry const* geometry) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gReleasedMesh.find(geometry);
    if (it == gReleasedMesh.end()) return;
    auto const frame = gFrame.load(std::memory_order_relaxed);
    for (auto const& pos : it->second) {
        auto const claim = gCellClaims.find(pos);
        if (claim == gCellClaims.end() || !claim->second.released || claim->second.rebuilt) continue;
        claim->second.rebuilt   = true;
        claim->second.liveFrame = frame;
    }
    gReleasedMesh.erase(it);
    updateMeshWatchLocked();
}

std::string blockNameOf(uint runtimeId) {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return {};
    return player->getLevel().getBlockPalette().getBlock(runtimeId).getTypeName();
}

// startTick is the recorded tick of a transition the client applied late; otherwise the action starts now.
void startAction(
    ::BlockSource&         region,
    ::BlockPos const&      pistonPos,
    ::BlockPos const&      facing,
    bool                   extending,
    std::optional<int64_t> startTick = std::nullopt
) {
    std::vector<::BlockPos> cells;
    {
        std::unique_lock lock(gAnimMutex);
        // A packet can arrive before the first sweep on the new clock.
        if (clockInvalidatedLocked()) cells = clearAnimationStateLocked();
        auto const   now      = visualClockTick().value_or(0);
        auto const   tick     = startTick.value_or(now);
        float const  to       = extending ? 1.0f : 0.0f;
        auto const&  settings = core::pistonSettings();
        PistonVisual visual{
            gNextAction++,
            core::makeSegment(tick, 1.0f - to, to, settings.durationTicks.load()),
            std::nullopt,
            std::nullopt
        };
        if (core::segmentExpired(visual.current, now)) {
            lock.unlock();
            rebuildCells(region, cells);
            return;
        }
        if (auto it = gPistonVisuals.find(pistonPos); it != gPistonVisuals.end()) {
            auto const& old     = it->second;
            bool const  running = core::segmentRunning(old.current, tick);
            // A piston cannot start the same direction twice, so this is a resent packet, not a new action.
            if (running && old.current.to == to) return;
            // Chain policy: the old action's remaining tail plays on top of the new one.
            if (running) visual.previous = old.current;
            cells = endActionLocked(old.action);
        }
        // This piston's body may still be arriving from a push; its arm must follow that motion until it ends.
        for (auto const& [moving, entry] : gMovingAction) {
            if (entry.cell != pistonPos || entry.handedOff) continue;
            // A finished tail contributes zero offset, so only the newest carrier matters.
            if (!visual.bodyCarry || entry.visual.segment.startTick > visual.bodyCarry->segment.startTick)
                visual.bodyCarry = entry.visual;
        }
        gActionVisuals[visual.action] = {visual.current, facing};
        gPistonVisuals[pistonPos]     = visual;
        updateMeshWatchLocked();
    }
    rebuildCells(region, cells);
}

std::optional<float> visualArmProgress(::BlockPos const& pistonPos, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gPistonVisuals.find(pistonPos);
    if (it == gPistonVisuals.end()) return std::nullopt;
    return armValue(it->second, visualTime(alpha));
}

// Relative to the MovingBlock's own (target) cell, same convention as native getDrawPos.
std::optional<::Vec3> visualDrawOffset(::MovingBlockActor const& moving, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    auto const            owned = gMovingAction.find(&moving);
    if (owned == gMovingAction.end()) return std::nullopt;
    auto const& entry = owned->second;
    if (!gActionVisuals.contains(entry.action)) {
        // Visual ended but still held for the re-mesh: stay exactly on the cell.
        auto const claim = gCellClaims.find(entry.cell);
        if (claim != gCellClaims.end() && claim->second.action == entry.action) return ::Vec3{0.0f, 0.0f, 0.0f};
        return std::nullopt;
    }
    auto const time   = visualTime(alpha);
    auto       offset = actionOffset(entry.visual, time);
    if (entry.carry) {
        auto const tail  = actionOffset(entry.carry->visual, time);
        offset.x        += tail.x;
        offset.y        += tail.y;
        offset.z        += tail.z;
    }
    return offset;
}

// Extra offset for the arm of a piston whose body is still finishing the push that carried it here.
std::optional<::Vec3> visualBodyOffset(::BlockPos const& pistonPos, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gPistonVisuals.find(pistonPos);
    if (it == gPistonVisuals.end() || !it->second.bodyCarry) return std::nullopt;
    return actionOffset(*it->second.bodyCarry, visualTime(alpha));
}

// Offset of the MovingBlock still drawing a landed piston's body; its head must stay attached to it.
std::optional<::Vec3> landedBodyOffset(::BlockPos const& pistonPos, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    MovingEntry const*    carrier = nullptr;
    for (auto const& [moving, entry] : gMovingAction) {
        if (entry.cell != pistonPos || entry.handedOff || entry.released || !gActionVisuals.contains(entry.action))
            continue;
        if (!carrier || entry.visual.segment.startTick > carrier->visual.segment.startTick) carrier = &entry;
    }
    if (!carrier) return std::nullopt;
    auto const time   = visualTime(alpha);
    auto       offset = actionOffset(carrier->visual, time);
    if (carrier->carry) {
        auto const tail  = actionOffset(carrier->carry->visual, time);
        offset.x        += tail.x;
        offset.y        += tail.y;
        offset.z        += tail.z;
    }
    return offset;
}

// Caller holds gAnimMutex; the dtor hook takes it under the chunk lock, so the chunk lock is only tried.
std::shared_ptr<::BlockActor> findOwner(::BlockSource& region, ::MovingBlockActor const& moving) {
    auto* const chunk = region.getChunkAt(moving.mPosition.get());
    if (chunk == nullptr) return {};
    std::unique_lock<std::mutex> const lock(chunk->mBlockEntityAccessLock.get(), std::try_to_lock);
    if (!lock.owns_lock()) return {};
    auto const* const actor = static_cast<::BlockActor const*>(&moving);
    for (auto const& [pos, owned] : chunk->mBlockEntities.get().mMap.get())
        if (owned.get() == actor) return owned;
    for (auto const& owned : chunk->mPreservedBlockEntities.get())
        if (owned.get() == actor) return owned;
    return {};
}

void registerMoving(::MovingBlockActor const& moving, ::BlockSource& region) {
    std::lock_guard const guard(gAnimMutex);
    if (gMovingAction.contains(&moving)) return;
    auto const it = gPistonVisuals.find(moving.mPistonBlockPos.get());
    if (it == gPistonVisuals.end()) return;
    auto const visual = gActionVisuals.find(it->second.action);
    if (visual == gActionVisuals.end()) return;
    auto const  action = it->second.action;
    auto const  cell   = moving.mPosition.get();
    MovingEntry entry{action, cell, visual->second};
    // The block now leaves its source cell; an older MovingBlock there still playing its tail hands it over.
    auto const&      facing = visual->second.facing;
    int const        dir    = visual->second.segment.to > visual->second.segment.from ? 1 : -1;
    ::BlockPos const source{cell.x - facing.x * dir, cell.y - facing.y * dir, cell.z - facing.z * dir};
    if (source != moving.mPistonBlockPos.get()) {
        for (auto& [other, old] : gMovingAction) {
            if (old.cell != source || old.action == action || old.handedOff) continue;
            old.handedOff = true;
            if (!entry.carry || old.visual.segment.startTick > entry.carry->visual.segment.startTick)
                entry.carry = CarryVisual{old.action, old.visual};
        }
        // The old claim would keep its stale copy held in a cell the block already left.
        if (auto const old = gCellClaims.find(source); old != gCellClaims.end() && old->second.action != action) {
            gCellClaims.erase(old);
            updateMeshWatchLocked();
        }
    }
    entry.keep             = findOwner(region, moving);
    gMovingAction[&moving] = entry;
    auto const claim       = gCellClaims.find(cell);
    // An older claim still hiding its landed block keeps the cell until it is re-meshed.
    if (claim != gCellClaims.end() && claim->second.landed && !claim->second.released) return;
    if (claim != gCellClaims.end() && claim->second.action == action) return;
    gCellClaims[cell] = {action, wrappedName(moving)};
    updateMeshWatchLocked();
}

// Held while its action plays, then until the real block has been re-meshed and shown for a couple of frames.
bool movingHeld(::MovingBlockActor const& moving) {
    std::lock_guard const guard(gAnimMutex);
    auto const            owned = gMovingAction.find(&moving);
    if (owned == gMovingAction.end()) return false;
    if (gActionVisuals.contains(owned->second.action)) return true;
    auto const claim = gCellClaims.find(owned->second.cell);
    if (claim == gCellClaims.end() || claim->second.action != owned->second.action) return false;
    // The real block is on screen only from the frame after its mesh was uploaded.
    if (!claim->second.rebuilt || gFrame.load(std::memory_order_relaxed) <= claim->second.liveFrame) return true;
    // Every MovingBlock sharing this claim is covered by the same rebuilt mesh.
    for (auto& [other, entry] : gMovingAction)
        if (entry.action == claim->second.action && entry.cell == claim->first) entry.released = true;
    gCellClaims.erase(claim);
    updateMeshWatchLocked();
    return false;
}

bool movingRetired(::MovingBlockActor const& moving) {
    std::lock_guard const guard(gAnimMutex);
    auto const            owned = gMovingAction.find(&moving);
    return owned != gMovingAction.end() && (owned->second.handedOff || owned->second.released);
}

bool hasClaims() {
    std::lock_guard const guard(gAnimMutex);
    return !gCellClaims.empty();
}

// Runs before the packet applies, so the mesh rebuild it triggers already skips the landed block.
void holdLandedCell(::BlockPos const& cell, std::string const& block) {
    if (block.empty() || block == "minecraft:air" || block == "minecraft:moving_block") return;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(cell);
    if (it == gCellClaims.end() || it->second.released || !gActionVisuals.contains(it->second.action)) return;
    it->second.landed = true;
    updateMeshWatchLocked();
}

// A replay can place the real block before its MovingBlock is first drawn, with no landing packet.
void holdPlacedCell(::BlockPos const& cell, std::string const& block) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(cell);
    if (it == gCellClaims.end() || it->second.landed || it->second.released || it->second.wrapped != block) return;
    if (!gActionVisuals.contains(it->second.action)) return;
    it->second.landed = true;
    gPendingRemesh.push_back(cell);
    updateMeshWatchLocked();
}

void forgetMoving(::MovingBlockActor const* moving) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gMovingAction.find(moving);
    if (it == gMovingAction.end()) return;
    if (it->second.keep) gDropKeep.push_back(std::move(it->second.keep));
    gMovingAction.erase(it);
}

// Chunk meshes skip faces against the target cell, but it stays empty on screen while the MovingBlock is still
// gliding in. Returns 0 if shown, 1 landed, 2 still a moving_block.
int neighborHidden(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gFaceWatch.load(std::memory_order_relaxed)) return 0;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(pos);
    if (it == gCellClaims.end() || it->second.released) return 0;
    return it->second.landed ? 1 : 2;
}

// Full cubes cull against simple neighbours through the chunk's bitset, never asking the occluder.
bool touchesLanded(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gFaceWatch.load(std::memory_order_relaxed)) return false;
    static ::BlockPos const dirs[6]{
        {0,  -1, 0 },
        {0,  1,  0 },
        {0,  0,  -1},
        {0,  0,  1 },
        {-1, 0,  0 },
        {1,  0,  0 }
    };
    for (auto const& dir : dirs)
        if (neighborHidden(pos + dir) == 1) return true;
    return false;
}

// Caller holds gAnimMutex. Same rule as movingHeld, without its unhold side effect.
bool heldLocked(MovingEntry const& entry) {
    if (entry.handedOff || entry.released) return false;
    if (gActionVisuals.contains(entry.action)) return true;
    auto const claim = gCellClaims.find(entry.cell);
    return claim != gCellClaims.end() && claim->second.action == entry.action;
}

// A MovingBlock created in a cell pushed again right after landing can miss the camera's collection for several
// ticks.
void queueFreshMoving(::LevelRendererCamera& camera) {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto&                   region = player->getDimensionBlockSource();
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const   guard(gAnimMutex);
        static ::BlockPos const dirs[7]{
            {0,  0,  0 },
            {0,  -1, 0 },
            {0,  1,  0 },
            {0,  0,  -1},
            {0,  0,  1 },
            {-1, 0,  0 },
            {1,  0,  0 }
        };
        std::unordered_set<::BlockPos, BlockPosHash> seen;
        for (auto const& [pos, claim] : gCellClaims)
            for (auto const& dir : dirs)
                if (seen.insert(pos + dir).second) cells.push_back(pos + dir);
    }
    auto& queue = opaqueQueueOf(camera);
    auto& alpha = alphaQueueOf(camera);
    for (auto const& pos : cells) {
        auto* actor = region.getBlockEntity(pos);
        if (!actor || actor->mType != ::BlockActorType::MovingBlock) continue;
        auto* const item = queuedItemOf(actor);
        if (!item) continue;
        auto const same = [item](auto const& queued) { return queued.get() == item; };
        if (std::any_of(queue.begin(), queue.end(), same) || std::any_of(alpha.begin(), alpha.end(), same)) continue;
        queue.emplace_back(item);
    }
}

// Runs right after the camera re-collects its queues, the only point where a kept instance can be let go
// safely.
void requeueHeldMoving(::LevelRendererCamera& camera) {
    std::vector<std::shared_ptr<::BlockActor>> drop;
    {
        std::lock_guard const guard(gAnimMutex);
        auto&                 queue  = opaqueQueueOf(camera);
        auto&                 alpha  = alphaQueueOf(camera);
        auto&                 shadow = shadowQueueOf(camera);
        auto const            queued = [](auto const& list, QueuedItem const* item) {
            return std::any_of(list.begin(), list.end(), [item](auto const& other) { return other.get() == item; });
        };
        for (auto& [moving, entry] : gMovingAction) {
            auto* actor = static_cast<::BlockActor*>(const_cast<::MovingBlockActor*>(moving));
            if (!heldLocked(entry)) {
                if (entry.keep) drop.push_back(std::move(entry.keep));
                continue;
            }
            auto* const item = queuedItemOf(actor);
            if (!item || queued(queue, item) || queued(alpha, item)) continue;
            queue.emplace_back(item);
        }
        drop.insert(drop.end(), std::make_move_iterator(gDropKeep.begin()), std::make_move_iterator(gDropKeep.end()));
        gDropKeep.clear();
        // Only this reference keeps it alive, so only our requeue put it here.
        for (auto const& ref : drop) {
            if (ref.use_count() != 1) continue;
            auto const same = [item = queuedItemOf(ref.get())](auto const& other) { return other.get() == item; };
            std::erase_if(queue, same);
            std::erase_if(alpha, same);
            std::erase_if(shadow, same);
        }
    }
    // The dtor hook locks gAnimMutex.
    drop.clear();
    queueFreshMoving(camera);
}

void markPistonStepped(::BlockActor const* piston) {
    std::lock_guard const guard(gSteppedPistonMutex);
    gSteppedPistons.insert(piston);
}

bool hasPistonStepped(::BlockActor const* piston) {
    std::lock_guard const guard(gSteppedPistonMutex);
    return gSteppedPistons.contains(piston);
}

// Piston NBT carries no interlock data, so a client-side piston rebuilt from a block-actor packet would stay
// InitialNotVisible until the engine's timeout and drop the arm for about three ticks.
#if OPTIPISTON_MC >= 2632
// PistonBlockActor's own constructor is not exported here; the base that owns the interlock data is.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPistonArmVisibilityHook,
    ll::memory::HookPriority::Normal,
    VanillaBlockActor,
    &VanillaBlockActor::$ctor,
    void*,
    ::BlockActorType       type,
    ::BlockPos const&      pos,
    ::BlockActorRendererId rendererId
) {
    auto* result = origin(type, pos, rendererId);
    if (type == ::BlockActorType::PistonArm && smoothPistonRenderEnabled())
        mTerrainInterlockData->mRenderVisibilityState = VisibilityState::Visible;
    return result;
}
#else
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPistonArmVisibilityHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$ctor,
    void*,
    ::BlockPos const& pos,
    bool              isSticky
) {
    auto* result = origin(pos, isSticky);
    if (smoothPistonRenderEnabled()) mTerrainInterlockData->mRenderVisibilityState = VisibilityState::Visible;
    return result;
}
#endif

// The round counter has to advance somewhere that runs once per game tick and independently of the render
// framerate.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPistonStepHook,
    ll::memory::HookPriority::Lowest,
    PistonBlockActor,
    &PistonBlockActor::$tick,
    void,
    ::BlockSource& region
) {
    bool const          client      = region.getLevel().isClientSide();
    float const         beforeP     = mProgress;
    float const         beforeLP    = mLastProgress;
    ::PistonState const beforeState = mState;
    auto const          nowTick     = region.getLevel().getCurrentTick().tickID;

    origin(region);

    // Also runs when disabled, so meshes hidden by an unfinished visual get rebuilt.
    if (client) sweepActions(region);
    if (!smoothPistonRenderEnabled()) return;

    // Unconditional, because a hidden piston is never handed to the renderer: a fix that lives in the render
    // hook cannot run for the very frames the arm is missing. Tick is the only path that still executes while
    // hidden.
    auto& interlock = mTerrainInterlockData.get();
    if (interlock.mRenderVisibilityState != VisibilityState::Visible || interlock.mHasBeenDelayedDeleted) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        interlock.mHasBeenDelayedDeleted = false;
    }

    if (!client) return;
    if (beforeP != mProgress || beforeLP != mLastProgress || beforeState != mState) markPistonStepped(this);
    gHeadOwnerRound.store(static_cast<uint64_t>(nowTick), std::memory_order_relaxed);
}

#if OPTIPISTON_MC != 2620
// getProgress is inlined into the renderer here, so the visual value is lent to the fields it reads.
class ProgressOverride {
public:
    ProgressOverride(::BlockActor& entity, float alpha) {
        if (entity.mType != ::BlockActorType::PistonArm || !animationActive()) return;
        auto const visual = visualArmProgress(entity.mPosition.get(), alpha);
        if (!visual) return;
        mPiston                = static_cast<::PistonBlockActor*>(&entity);
        mProgress              = mPiston->mProgress;
        mLastProgress          = mPiston->mLastProgress;
        mPiston->mProgress     = *visual;
        mPiston->mLastProgress = *visual;
    }
    ~ProgressOverride() {
        if (!mPiston) return;
        mPiston->mProgress     = mProgress;
        mPiston->mLastProgress = mLastProgress;
    }
    ProgressOverride(ProgressOverride const&)            = delete;
    ProgressOverride& operator=(ProgressOverride const&) = delete;

private:
    ::PistonBlockActor* mPiston{};
    float               mProgress{};
    float               mLastProgress{};
};
#endif

// Shared by the renderer hooks and, where those are not exported, the dispatcher hook. position is the draw's
// own copy; draw() renders with whatever it holds.
template <class Draw>
void drawArm(
    ::BaseActorRenderContext& renderContext,
    ::BlockSource&            renderSource,
    ::BlockActor&             entity,
    ::Vec3&                   position,
    Draw&&                    draw
) {
    auto& interlock = interlockOf(entity);

    bool const enabled = smoothPistonRenderEnabled();
    bool const hidden  = interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible;

    // Same-cell pistons cannot be found by walking the chunk collections - each instance only ever sees itself
    // there - so one head per cell is elected here instead, fresh every round.
    bool const isArm       = entity.mType == ::BlockActorType::PistonArm;
    bool const selfStepped = isArm && hasPistonStepped(&entity);
    // Drawn by a MovingBlock carrying it; a replay can leave that cell as air instead of moving_block.
    bool const nested = tRenderDepth > 0;
    // A piston in a moving_block cell is cargo being pushed by another piston rather than the one driving the
    // animation, so it must lose to an actively extending head instead of competing with it.
    bool const isCargo =
        isArm && (nested || renderSource.getBlock(entity.mPosition).getTypeName() == "minecraft:moving_block");
    // Driving heads outrank cargo, and within either role a piston that advanced mProgress outranks an
    // untouched leftover. Equal rank falls back to first-come so exactly one head survives.
    int const selfRank       = (isCargo ? 0 : 2) + (selfStepped ? 1 : 0);
    bool      skipZombieHead = false;
    // A nested head keeps its carrier's claim when the piston inside is rebuilt.
    auto const* const self = nested && tCarrier ? tCarrier : &entity;
    if (enabled && isArm) {
        auto const            round = gHeadOwnerRound.load(std::memory_order_relaxed);
        std::lock_guard const guard(gHeadOwnerMutex);
        auto&                 owner = gHeadOwners[entity.mPosition.get()];
        if (owner.round != round || owner.actor == nullptr || owner.actor == self || selfRank > owner.rank) {
            owner = {round, self, selfRank};
        } else {
            skipZombieHead = true;
        }
    }

    if (enabled && hidden) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        // A rebuilt piston also inherits the delete flag, which would re-hide it on the next frame.
        interlock.mHasBeenDelayedDeleted = false;
    }

    if (skipZombieHead) return;

    if (enabled) {
        gFrameLt.store(renderSource.getLevel().getCurrentTick().tickID);
        gFrameDrewActors.store(true, std::memory_order_relaxed);
    }
    RenderScope const scope;
#if OPTIPISTON_MC != 2620
    ProgressOverride const progress(entity, renderContext.mFrameAlpha);
#endif
    std::optional<::Vec3> bodyOffset;
    if (isArm && animationActive() && !nested) {
        bodyOffset = visualBodyOffset(entity.mPosition.get(), renderContext.mFrameAlpha);
        // Cargo is drawn through its MovingBlock, whose draw position already carries the offset.
        if (!bodyOffset && !isCargo) bodyOffset = landedBodyOffset(entity.mPosition.get(), renderContext.mFrameAlpha);
    }
    if (!bodyOffset || (bodyOffset->x == 0.0f && bodyOffset->y == 0.0f && bodyOffset->z == 0.0f)) {
        draw();
        return;
    }
    // Shift the arm with its still-moving body.
    auto const saved  = position;
    position.x       += bodyOffset->x;
    position.y       += bodyOffset->y;
    position.z       += bodyOffset->z;
    draw();
    position = saved;
}

template <class Draw>
void drawMoving(::BaseActorRenderContext&, ::BlockSource& source, ::BlockActor& entity, Draw&& draw) {
    bool const isMoving  = entity.mType == ::BlockActorType::MovingBlock;
    auto*      movingPtr = isMoving ? static_cast<::MovingBlockActor*>(&entity) : nullptr;

    RenderScope const  scope;
    CarrierScope const carrier(&entity);

    if (!smoothPistonRenderEnabled() || !isMoving) {
        draw();
        return;
    }

    auto&      interlock = interlockOf(entity);
    auto const nowTick   = source.getLevel().getCurrentTick().tickID;
    gFrameLt.store(nowTick);
    gFrameDrewActors.store(true, std::memory_order_relaxed);

    // A MovingBlock rebuilt mid-animation starts hidden just like the piston does. Only lift that initial
    // state: the retirement logic below owns DelayedDestructionNotVisible and must not be overridden.
    if (interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
    }

    if (animationActive()) {
        registerMoving(*movingPtr, source);
        holdPlacedCell(entity.mPosition.get(), source.getBlock(entity.mPosition).getTypeName());
        // A successor or the rebuilt mesh already shows this block; a second copy would be a ghost.
        if (movingRetired(*movingPtr)) return;
        // The visual owns this block until the real block is re-meshed; native retirement would cut it short.
        if (movingHeld(*movingPtr)) {
            interlock.mRenderVisibilityState = VisibilityState::Visible;
            interlock.mHasBeenDelayedDeleted = false;
            draw();
            return;
        }
    }

    auto const& cellBlock = source.getBlock(entity.mPosition);

    // An air-wrapped MovingBlock has no replacement block of its own coming, so hiding it while its cell is
    // still empty would open a hole. Once a real block occupies the cell there is nothing left for it to cover.
    bool const coversNothing = wrappedAir(*movingPtr) && cellBlock.isAir();

    // getDrawPos collapses as soon as mProgress reaches 1, but the renderer keeps lerping mLastProgress towards
    // it for another tick. Both fields at 1 is the only state where no interpolation is left.
    auto* const owner   = owningPiston(*movingPtr, source);
    bool const  arrived = owner != nullptr && owner->mProgress >= 1.0f && owner->mLastProgress >= 1.0f;

    // Detached means the block entity no longer belongs to this cell: the block has been handed to the next
    // MovingBlock one cell along, so this cell is meant to be empty and keeping it drawn only redraws the block
    // at the position it already left.
    bool const detached = source.getBlockEntity(entity.mPosition) != &entity;

    // A piston body never hands its block to a neighbour, so for it detachment only means the cell
    // re-registered a fresh instance. Retiring it left the cell with neither entity nor replacement block for
    // several frames, which is the piston vanishing mid-extension.
    bool const isPistonBody = wrappedName(*movingPtr).find("piston") != std::string::npos;

    if (coversNothing || isPistonBody) {
        // Leave it to vanilla: nothing to hold on screen, nothing to hide.
    } else if (movingPtr->mPreserved && arrived && detached) {
        // Retire on the first frame all three hold. Also requiring a real block in the cell was too strict: the
        // replacement can arrive several frames late and the block stayed visible in its old cell until then.
        interlock.mRenderVisibilityState = VisibilityState::DelayedDestructionNotVisible;
        interlock.mHasBeenDelayedDeleted = true;
    } else if (detached) {
        gTailAnchors[movingPtr] = nowTick;
    }

    draw();
}

#if OPTIPISTON_MC == 2610
// Neither renderer exports its render here, so both are intercepted where the dispatcher hands them the actor.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonBlockActorDispatchHook,
    ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,
    static_cast<void (::BlockActorRenderDispatcher::*)(
        ::BaseActorRenderContext&,
        ::BlockSource&,
        ::BlockActor&,
        ::Block const&,
        ::Vec3 const&,
        ::BlockPos const&,
        bool,
        ::mce::MaterialPtr const&,
        ::mce::ClientTexture const*,
        int,
        std::optional<::dragon::RenderMetadata>
    )>(&::BlockActorRenderDispatcher::render),
    void,
    ::BaseActorRenderContext&               entityRenderContext,
    ::BlockSource&                          renderSource,
    ::BlockActor&                           e,
    ::Block const&                          block,
    ::Vec3 const&                           renderPos,
    ::BlockPos const&                       worldPos,
    bool                                    renderAlphaLayer,
    ::mce::MaterialPtr const&               forcedMat,
    ::mce::ClientTexture const*             forceTex,
    int                                     breakingAmount,
    std::optional<::dragon::RenderMetadata> renderMetadata
) {
    ::Vec3     position = renderPos;
    auto const draw     = [&] {
        origin(
            entityRenderContext,
            renderSource,
            e,
            block,
            position,
            worldPos,
            renderAlphaLayer,
            forcedMat,
            forceTex,
            breakingAmount,
            std::move(renderMetadata)
        );
    };
    // The renderer hooks this replaces only ever covered the opaque pass.
    if (renderAlphaLayer) draw();
    else if (e.mType == ::BlockActorType::PistonArm) drawArm(entityRenderContext, renderSource, e, position, draw);
    else if (e.mType == ::BlockActorType::MovingBlock) drawMoving(entityRenderContext, renderSource, e, draw);
    else draw();
}
#else
::BlockActor& renderedActor(::BlockActorRenderData& data) {
#if OPTIPISTON_MC >= 2632
    return data.entity.getBlockActor();
#else
    return data.entity;
#endif
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonPistonArmHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActorRenderer,
    &PistonBlockActorRenderer::$render,
    void,
    ::BaseActorRenderContext& renderContext,
    ::BlockActorRenderData&   blockEntityRenderData
) {
    // The dispatcher owns this position for the current draw only.
    auto& position = const_cast<::Vec3&>(blockEntityRenderData.renderPosition);
    drawArm(renderContext, blockEntityRenderData.renderSource, renderedActor(blockEntityRenderData), position, [&] {
        origin(renderContext, blockEntityRenderData);
    });
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonMovingBlockGhostHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActorRenderer,
    &MovingBlockActorRenderer::$render,
    void,
    ::BaseActorRenderContext& renderContext,
    ::BlockActorRenderData&   blockEntityRenderData
) {
    drawMoving(renderContext, blockEntityRenderData.renderSource, renderedActor(blockEntityRenderData), [&] {
        origin(renderContext, blockEntityRenderData);
    });
}
#endif

// A recycled address must not inherit the previous MovingBlock's tail state.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonMovingBlockConstructionHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$ctor,
    void*,
    ::BlockPos const& pos
) {
    auto* result = origin(pos);
    gTailAnchors.erase(static_cast<::MovingBlockActor const*>(this));
    forgetMoving(this);
    return result;
}

// A destroyed MovingBlock must leave the tables before its address can be re-queued or reused.
#if OPTIPISTON_MC >= 2632
// BlockActor's destructor is not exported on every version here; every MovingBlock passes through this one.
using DtorOwner = ::VanillaBlockActor;
#else
using DtorOwner = ::BlockActor;
#endif
LL_TYPE_INSTANCE_HOOK(
    OptiPistonBlockActorDtorHook,
    ll::memory::HookPriority::Normal,
    DtorOwner,
    &DtorOwner::$dtor,
    void
) {
    if (mType == ::BlockActorType::MovingBlock)
        forgetMoving(static_cast<::MovingBlockActor const*>(static_cast<::BlockActor const*>(this)));
    origin();
}

#if OPTIPISTON_MC == 2620 || OPTIPISTON_MC == 2632
LL_TYPE_INSTANCE_HOOK(
    OptiPistonStartRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::startRebuild,
    void,
    ::RenderChunkBuilder& builder,
    ::Vec3 const&         origin_
) {
    meshBuildStarted(this);
    tMeshGeometry = this;
    origin(builder, origin_);
}
#endif

#if OPTIPISTON_MC == 2610
// The geometry's own rebuild entry points are not exported here; the builder pass that fills it is.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkBuilder,
    &RenderChunkBuilder::build,
    void,
    ::RenderChunkGeometry&                                     geometry,
    bool                                                       transparentLeaves,
    ::BakedBlockLightType                                      lightingType,
    bool                                                       forExport,
    ::mce::framebuilder::FrameLightingModelCapabilities const& caps
) {
    meshBuildStarted(&geometry);
    auto const saved = tMeshGeometry;
    tMeshGeometry    = &geometry;
    origin(geometry, transparentLeaves, lightingType, forExport, caps);
    tMeshGeometry = saved;
}
#else
LL_TYPE_INSTANCE_HOOK(
    OptiPistonRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::rebuild,
    void,
    ::RenderChunkBuilder&                                      builder,
    bool                                                       lightingType,
    ::BakedBlockLightType                                      forExport,
    bool                                                       lightingModelCapabilities,
    ::mce::framebuilder::FrameLightingModelCapabilities const& caps
) {
#if OPTIPISTON_MC >= 2640
    // startRebuild is not exported here; this is the first point of a new build.
    meshBuildStarted(this);
#endif
    auto const saved = tMeshGeometry;
    tMeshGeometry    = this;
    origin(builder, lightingType, forExport, lightingModelCapabilities, caps);
    tMeshGeometry = saved;
}
#endif

#if OPTIPISTON_MC >= 2651
LL_TYPE_INSTANCE_HOOK(
    OptiPistonEndRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::endRebuild,
    void,
    ::RenderChunkBuilder&           builder,
    ::mce::BufferResourceService&   bufferResourceService,
    bool                            isBuilding,
    bool                            alreadyHadGeometry,
    ::dragon::RenderMetadata const& renderMetadata,
    bool                            useSplitStream
) {
    origin(builder, bufferResourceService, isBuilding, alreadyHadGeometry, renderMetadata, useSplitStream);
    tMeshGeometry = nullptr;
    meshBuildCommitted(this);
}
#else
LL_TYPE_INSTANCE_HOOK(
    OptiPistonEndRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::endRebuild,
    void,
    ::RenderChunkBuilder&           builder,
    ::mce::BufferResourceService&   bufferResourceService,
    bool                            isBuilding,
    ::dragon::RenderMetadata const& renderMetadata,
    bool                            useSplitStream
) {
    origin(builder, bufferResourceService, isBuilding, renderMetadata, useSplitStream);
    tMeshGeometry = nullptr;
    meshBuildCommitted(this);
}
#endif

// Passes without block actors (export) do not count, so "next frame" always means a frame that drew them.
#if OPTIPISTON_MC == 2620
LL_TYPE_INSTANCE_HOOK(
    OptiPistonFrameHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::endFrame,
    void,
    ::mce::TextureResourceService& textureResourceService
) {
    origin(textureResourceService);
    if (gFrameDrewActors.exchange(false, std::memory_order_relaxed)) gFrame.fetch_add(1, std::memory_order_relaxed);
}
#else
// endFrame is not exported here; renderLevel is the exported per-frame call.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonFrameHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::renderLevel,
    void,
    ::ScreenContext&           screenContext,
    ::FrameRenderObject const& renderObj
) {
    origin(screenContext, renderObj);
    if (gFrameDrewActors.exchange(false, std::memory_order_relaxed)) gFrame.fetch_add(1, std::memory_order_relaxed);
}
#endif

// A subchunk re-collect drops a detached MovingBlock from the queue; the main camera overrides the base
// collection.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPlayerQueueEntitiesHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$queueRenderEntities,
    void,
    ::LevelRenderPreRenderUpdateParameters const& parameters
) {
    origin(parameters);
    requeueHeldMoving(*this);
}

// The camera dispatches by the cell's block; once the real block lands there, a held MovingBlock gets no
// renderer. Later versions no longer have this lookup.
#if OPTIPISTON_MC <= 2620
LL_TYPE_INSTANCE_HOOK(
    OptiPistonBlockForEntityHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$_getBlockForBlockEnity,
    ::Block const*,
    ::BlockActor const& blockActor
) {
    auto const* block = origin(blockActor);
    if (blockActor.mType != ::BlockActorType::MovingBlock || !animationActive()) return block;
    if (block && block->getTypeName() == "minecraft:moving_block") return block;
    auto const* moving = static_cast<::MovingBlockActor const*>(&blockActor);
    {
        std::lock_guard const guard(gAnimMutex);
        auto const            it = gMovingAction.find(moving);
        if (it == gMovingAction.end() || !heldLocked(it->second)) return block;
    }
    // Looked up per call: the replay re-registers blocks, which frees any cached Block.
    return &::BlockTypeRegistry::get().getDefaultBlockState(::VanillaBlockTypeIds::MovingBlock());
}
#endif

// Nonzero while a BlockActorDataPacket is being handled; the client may re-apply held data later outside it.
thread_local int  tBlockActorDataDepth   = 0;
thread_local bool tBlockActorDataApplied = false;

// Piston data the client did not apply on arrival, with the clock it arrived on.
struct HeldPistonData {
    ::BlockPos                     pos;
    core::ClockSample              clock;
    std::unique_ptr<::CompoundTag> data;
};
std::vector<HeldPistonData> gHeldPistonData; // guarded by gAnimMutex

bool sameClockRun(core::ClockSample const& a, core::ClockSample const& b) {
    return a.external == b.external && a.epoch == b.epoch;
}

bool isPistonData(::CompoundTag const& data) {
    return data.contains("id", ::Tag::Type::String) && static_cast<std::string const&>(data.at("id")) == "PistonArm";
}

// Arrival tick of the held copy of this data, consumed on match.
std::optional<int64_t> takeHeldTick(::BlockPos const& pos, ::CompoundTag const& data) {
    auto const clock = visualClock();
    if (!clock) return std::nullopt;
    std::lock_guard const guard(gAnimMutex);
    for (auto it = gHeldPistonData.begin(); it != gHeldPistonData.end(); ++it) {
        if (it->pos != pos || !sameClockRun(it->clock, *clock) || !it->data->equals(data)) continue;
        auto const tick = it->clock.tick;
        gHeldPistonData.erase(it);
        return tick;
    }
    return std::nullopt;
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonBlockActorDataHandlerHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&              source,
    std::shared_ptr<::BlockActorDataPacket> packet
) {
    std::optional<HeldPistonData> held;
    if (packet && isPistonData(*packet->mData)) {
        if (auto const clock = visualClock()) held = HeldPistonData{packet->mPos, *clock, packet->mData->clone()};
    }
    tBlockActorDataApplied = false;
    ++tBlockActorDataDepth;
    origin(source, std::move(packet));
    --tBlockActorDataDepth;
    if (!held || tBlockActorDataApplied) return;
    std::lock_guard const guard(gAnimMutex);
    // Data held on an earlier clock run can no longer be placed on the timeline.
    std::erase_if(gHeldPistonData, [&](HeldPistonData const& entry) {
        return !sameClockRun(entry.clock, held->clock);
    });
    gHeldPistonData.push_back(std::move(*held));
}

// Starts the visual when a packet flips a piston into motion; the native state change itself is untouched.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPistonActionHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$_onUpdatePacket,
    void,
    ::CompoundTag const& data,
    ::BlockSource&       region
) {
    ::PistonState const before = mState;
    origin(data, region);
    std::optional<int64_t> heldTick;
    if (tBlockActorDataDepth > 0) tBlockActorDataApplied = true;
    else heldTick = takeHeldTick(mPosition.get(), data);
    if (!animationActive() || mState == before) return;
    if (mState != ::PistonState::Expanding && mState != ::PistonState::Retracting) return;
    // Late data with no recorded arrival cannot be placed on the timeline; native state already reflects it.
    if (tBlockActorDataDepth == 0 && !heldTick) return;
    startAction(region, mPosition.get(), facingOf(*this, region), mState == ::PistonState::Expanding, heldTick);
}

#if OPTIPISTON_MC == 2620
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPistonProgressHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::getProgress,
    float,
    float a
) {
    if (tRenderDepth > 0 && animationActive()) {
        if (auto const visual = visualArmProgress(mPosition.get(), a)) return *visual;
    }
    return origin(a);
}
#endif

LL_TYPE_INSTANCE_HOOK(
    OptiPistonMovingDrawPosHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::getDrawPos,
    ::Vec3,
    ::IConstBlockSource const& region,
    float                      a
) {
    if (tRenderDepth > 0 && animationActive()) {
        if (auto const offset = visualDrawOffset(*this, a)) return *offset;
    }
    return origin(region, a);
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonLandedBlockHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&          source,
    ::UpdateSubChunkBlocksPacket const& packet
) {
    if (animationActive() && hasClaims()) {
        for (auto const& info : *packet.mBlocksChanged->mStandards)
            holdLandedCell(info.mPos, blockNameOf(info.mRuntimeId));
    }
    origin(source, packet);
}

// Other versions only export tessellateBlockInWorld, hooked below.
#if OPTIPISTON_MC == 2620
LL_TYPE_INSTANCE_HOOK(
    OptiPistonMeshInWorldHook,
    ll::memory::HookPriority::Normal,
    BlockTessellator,
    static_cast<bool (::BlockTessellator::*)(::Tessellator&, ::Block const&, ::BlockPos const&, bool)>(
        &::BlockTessellator::tessellateInWorld
    ),
    bool,
    ::Tessellator&    tessellator,
    ::Block const&    block,
    ::BlockPos const& pos,
    bool              useCalcWithCache
) {
    if (meshSuppressed(pos)) return false;
    return origin(tessellator, block, pos, useCalcWithCache);
}
#endif

LL_TYPE_INSTANCE_HOOK(
    OptiPistonMeshBlockInWorldHook,
    ll::memory::HookPriority::Normal,
    BlockTessellator,
    &BlockTessellator::tessellateBlockInWorld,
    bool,
    ::Tessellator&                 tessellator,
    ::Block const&                 block,
    ::BlockPos const&              pos,
    std::bitset<6>                 faces,
    ::AirAndSimpleBlockBits const* airAndSimpleBlocks
) {
    if (meshSuppressed(pos)) return false;
    if (airAndSimpleBlocks && touchesLanded(pos)) return origin(tessellator, block, pos, faces, nullptr);
    return origin(tessellator, block, pos, faces, airAndSimpleBlocks);
}

LL_TYPE_INSTANCE_HOOK(
    OptiPistonFaceOcclusionHook,
    ll::memory::HookPriority::Normal,
    BlockOccluder,
    &BlockOccluder::_shouldRenderFace,
    bool,
    ::BlockPos const& neighborPos,
    uchar             face,
    ::AABB const&     shape,
    ::BlockPos const& pos
) {
    if (origin(neighborPos, face, shape, pos)) return true;
    return neighborHidden(neighborPos) != 0;
}

// A plain area change can sit in the render queue for many frames at high framerates; the hidden block needs it
// now.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonImmediateRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkCoordinator,
    &RenderChunkCoordinator::$onAreaChanged,
    void,
    ::BlockSource&    source,
    ::BlockPos const& min,
    ::BlockPos const& max
) {
    origin(source, min, max);
    if (!tImmediateRebuild) return;
    _setDirty(min, max, true, false, false);
}

template <class Hook>
bool installHook(bool& installed) {
    if (!installed) installed = Hook::hook() == 0;
    return installed;
}

template <class Hook>
void removeHook(bool& installed) {
    if (!installed) return;
    Hook::unhook();
    installed = false;
}

// Hooks in install order; unhooked in reverse. The action hook is last, so no visual starts before everything
// that finishes it is in place.
#if OPTIPISTON_MC == 2610
#define OPTIPISTON_RENDER_HOOKS(X) X(OptiPistonBlockActorDispatchHook)
#else
#define OPTIPISTON_RENDER_HOOKS(X) X(OptiPistonPistonArmHook) X(OptiPistonMovingBlockGhostHook)
#endif
#if OPTIPISTON_MC == 2620
#define OPTIPISTON_MESH_IN_WORLD_HOOK(X) X(OptiPistonMeshInWorldHook)
#define OPTIPISTON_PROGRESS_HOOK(X)      X(OptiPistonPistonProgressHook)
#else
#define OPTIPISTON_MESH_IN_WORLD_HOOK(X)
#define OPTIPISTON_PROGRESS_HOOK(X)
#endif
#if OPTIPISTON_MC == 2620 || OPTIPISTON_MC == 2632
#define OPTIPISTON_START_REBUILD_HOOK(X) X(OptiPistonStartRebuildHook)
#else
#define OPTIPISTON_START_REBUILD_HOOK(X)
#endif
#if OPTIPISTON_MC <= 2620
#define OPTIPISTON_BLOCK_FOR_HOOK(X) X(OptiPistonBlockForEntityHook)
#else
#define OPTIPISTON_BLOCK_FOR_HOOK(X)
#endif

#define OPTIPISTON_ALL_HOOKS(X)                                                                                        \
    X(OptiPistonPistonArmVisibilityHook)                                                                               \
    OPTIPISTON_RENDER_HOOKS(X)                                                                                         \
    X(OptiPistonPistonStepHook)                                                                                        \
    X(OptiPistonMovingBlockConstructionHook)                                                                           \
    OPTIPISTON_MESH_IN_WORLD_HOOK(X)                                                                                   \
    X(OptiPistonMeshBlockInWorldHook)                                                                                  \
    X(OptiPistonFaceOcclusionHook)                                                                                     \
    X(OptiPistonImmediateRebuildHook)                                                                                  \
    X(OptiPistonBlockActorDtorHook)                                                                                    \
    OPTIPISTON_START_REBUILD_HOOK(X)                                                                                   \
    X(OptiPistonRebuildHook)                                                                                           \
    X(OptiPistonEndRebuildHook)                                                                                        \
    X(OptiPistonFrameHook)                                                                                             \
    X(OptiPistonPlayerQueueEntitiesHook)                                                                               \
    OPTIPISTON_BLOCK_FOR_HOOK(X)                                                                                       \
    X(OptiPistonLandedBlockHook)                                                                                       \
    OPTIPISTON_PROGRESS_HOOK(X)                                                                                        \
    X(OptiPistonMovingDrawPosHook)                                                                                     \
    X(OptiPistonBlockActorDataHandlerHook)                                                                             \
    X(OptiPistonPistonActionHook)

#define OPTIPISTON_HOOK_ENTRY(H) {&installHook<H>, &removeHook<H>},

struct HookEntry {
    bool (*install)(bool&);
    void (*remove)(bool&);
};
constexpr HookEntry kHooks[] = {OPTIPISTON_ALL_HOOKS(OPTIPISTON_HOOK_ENTRY)};

} // namespace

bool hookPistonRender(bool enable) {
    static bool installed[std::size(kHooks)]{};

    if (enable) {
        if (gInstalled.load(std::memory_order_acquire)) return true;
        for (std::size_t i = 0; i < std::size(kHooks); ++i) {
            if (!kHooks[i].install(installed[i])) return false;
        }
        gInstalled.store(true, std::memory_order_release);
        return true;
    }

    if (!gInstalled.load(std::memory_order_acquire)) return true;

    for (std::size_t i = std::size(kHooks); i-- > 0;) kHooks[i].remove(installed[i]);

    gTailAnchors.clear();
    {
        std::lock_guard const guard(gSteppedPistonMutex);
        gSteppedPistons.clear();
    }
    {
        std::lock_guard const guard(gHeadOwnerMutex);
        gHeadOwners.clear();
    }
    clearAnimationState();

    gInstalled.store(false, std::memory_order_release);
    return true;
}

} // namespace optipiston::platform
