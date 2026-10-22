// Temporary diagnostics: counts the drawers of every pushed cell per frame and logs violations of the
// single-drawer rule to mods/OptiPiston/probe.log.
#ifndef OPTIPISTON_PROBE
#define OPTIPISTON_PROBE 1
#endif

#if OPTIPISTON_PROBE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "platform/Features.h"
#include "platform/bedrock/Observe.h"

#include "core/Clock.h"
#include "core/Limits.h"
#include "core/MeshView.h"
#include "core/PushTimeline.h"
#include "core/Segment.h"
#include "core/Settings.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/block/BlockOccluder.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderData.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/PistonBlockActorRenderer.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/client/renderer/chunks/RenderChunkGeometry.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/minecraft_renderer/framebuilder/dragon/RenderMetadata.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/RenderMaterial.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
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
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/MovingBlockActor.h"
#include "mc/world/level/block/actor/PistonBlockActor.h"
#include "mc/world/level/block/actor/PistonState.h"
#include "mc/world/phys/AABB.h"

#if OPTIPISTON_MC == 2610
#include "mc/client/renderer/chunks/RenderChunkBuilder.h"
#include "mc/world/level/block/PistonBlock.h"
#include "mc/world/level/block/VanillaStates.h"
#endif
#if OPTIPISTON_MC != 2620
#include "mc/world/level/BlockSourceListener.h"
#endif
#if OPTIPISTON_MC >= 2632
#include "mc/world/level/block/actor/VanillaBlockActor.h"
#include "mc/world/level/block/actor/component/IVanillaRenderBlockActorComponent.h"
#if OPTIPISTON_PROBE
#include "mc/world/level/block/BrightnessPair.h"
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if OPTIPISTON_PROBE
#define OPTIPISTON_PROBE_DO(...) __VA_ARGS__
#else
#define OPTIPISTON_PROBE_DO(...)
#endif

namespace optipiston::platform {

namespace {

using VisibilityState = ::ActorTerrainInterlockData::VisibilityState;
using core::PushDrawer;
using core::PushState;
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

core::CellPos cellOf(::BlockPos const& pos) { return core::CellPos{pos.x, pos.y, pos.z}; }

// ---- Version adapters: the only places that know which Minecraft API this build targets ----

::ActorTerrainInterlockData& interlockOf(::BlockActor& actor) {
#if OPTIPISTON_MC >= 2632
    // Only called for pistons and moving blocks, both vanilla block actors.
    return static_cast<::VanillaBlockActor&>(actor).mTerrainInterlockData.get();
#else
    return actor.mTerrainInterlockData.get();
#endif
}

// A view into the block's registered name, so the per-frame render path does not allocate.
std::string_view wrappedName(::MovingBlockActor const& moving) {
#if OPTIPISTON_MC == 2620
    return moving.getWrappedBlock().getTypeName();
#else
    ::Block const* block = moving.mWrappedBlock;
    return block ? std::string_view{block->getTypeName()} : std::string_view{"minecraft:air"};
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

std::shared_ptr<::BlockActor>& wrappedActorOf(::MovingBlockActor& moving) { return moving.mWrappedBlockActor; }

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
#else
using QueuedItem = ::BlockActor;
QueuedItem* queuedItemOf(::BlockActor* actor) { return actor; }
#endif

// Replay rebuilds PistonBlockActors without retiring the old ones, leaving several on one cell that the engine draws a
// head for each. Animating ones advance mProgress in $tick; leftovers never do, so stepping distinguishes the two.
// Maps each stepped piston to the level tick it last ticked on, so pruning can drop destroyed ones.
std::unordered_map<::BlockActor const*, int64_t> gSteppedPistons;

// Ownership is scoped to one round: a destroyed piston's address cannot be detected, and a claim outliving its owner
// locked the cell for good, leaving frames with no head at all. Re-electing each round bounds a stale address to the
// round it was seen in. The round advances on PistonBlockActor::$tick, which runs once per game tick.
std::atomic<uint64_t> gHeadOwnerRound{0};

struct HeadOwner {
    uint64_t            round{};
    ::BlockActor const* actor{};
    // Rank inputs are kept instead of the rank, so only a contested cell has to look at the stepped set.
    ::BlockActor const* claimant{};
    bool                cargo{};
};

// Leaf lock over gSteppedPistons; only contested cells and the piston tick take it.
std::mutex gHeadMutex;

// A cell's heads are all drawn by one thread within one pass, so each drawing thread elects in its own table and
// the per-arm path takes no lock. Tables stay registered until the module goes away, never freed under a thread.
struct HeadTable {
    uint64_t                                                epoch{};
    uint64_t                                                round{};
    std::unordered_map<::BlockPos, HeadOwner, BlockPosHash> owners;
};

std::atomic<uint64_t>                   gHeadEpoch{0};
std::mutex                              gHeadTablesMutex;
std::vector<std::unique_ptr<HeadTable>> gHeadTables;
thread_local HeadTable*                 tHeadTable = nullptr;
#if OPTIPISTON_PROBE
std::atomic<std::size_t> gProbeHeadOwners{0};
#endif

// ---- Render-only animation on the visual clock; native progress, state and world blocks stay untouched ----

// Non-zero while a piston or moving-block renderer is on this thread's stack; logic callers keep native values.
thread_local int tRenderDepth = 0;
// The proxy being dispatched by drawProxies on this thread.
thread_local ::BlockActor const* tProxyDraw = nullptr;

#if OPTIPISTON_MC == 2651
// Set around one proxy dispatch: the cell the proxy stands in and where the last lit answer for that cell is kept.
struct ProxyLightFix {
    bool                   active{};
    ::BlockPos             cell{};
    std::atomic<uint16_t>* memory{};
};
thread_local ProxyLightFix tProxyLightFix;
#endif

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

// Levels ticks a finished cell keeps hiding the game's own instance, and levels ticks after a start in which the
// target cells are searched for the game's instances.
constexpr int64_t kTombstoneTicks = 10;
constexpr int64_t kScanTicks      = 3;

struct PistonVisual {
    uint64_t      action{};
    VisualSegment current;
    // A truncated predecessor keeps drawing the arm until the new action's start tick.
    std::optional<VisualSegment> previous;
    // Tail of the push that carried this piston's body here, still playing when its own action starts.
    std::optional<ActionVisual> bodyCarry;
    // The body was pushed away while the arm still played; the arm went along and this entry no longer answers here.
    bool vacated{};
    // Last tick on which target cells are searched, and a copy of the piston's attached blocks taken on its own
    // thread, so the render thread never reads the live vector.
    int64_t scanUntil{};
    // Replaced whole, never edited, so a published view shares it.
    std::shared_ptr<std::vector<::BlockPos> const> attached;
    OPTIPISTON_PROBE_DO(uint64_t startFrame{};) // real frame count when the action started
};

// Tail of the action that carried a block into the cell a newer action picks it up from.
struct CarryVisual {
    uint64_t     action{};
    ActionVisual visual;
};

// State of one cell that render and mesh threads write without any lock. Shared by every copy of the cell.
struct CellShared {
    // Frame on which the landed cell's own actor was last drawn; 0 if never. Only ever rises.
    std::atomic<uint64_t> actorFrame{0};
    // An early re-mesh for the cell is already queued.
    std::atomic_bool earlyRemesh{false};
    // The draw read the world empty under the landed cell this frame; taken once per frame by advanceFrame.
    std::atomic_bool blockGone{false};
    // Last non-dark light answer the renderer got for the cell while its proxy was dispatched; 0 if none yet.
    std::atomic<uint16_t> lightMemory{0};
    OPTIPISTON_PROBE_DO(std::atomic<uint64_t> strippedFrame{0};) // last frame the proxy drew without its actor copy
};

// One pushed target cell. The timeline decides who draws it; the proxy is our own MovingBlock copy.
struct PushCell {
    uint64_t                           action{};
    ::BlockPos                         cell;
    ::BlockPos                         pistonPos;
    ActionVisual                       visual; // copied, so a successor can still replay its tail after the action ends
    std::optional<CarryVisual>         carry;
    core::PushTimeline                 timeline;
    std::shared_ptr<std::string const> wrapped; // block name, shared with the published views
    std::shared_ptr<::MovingBlockActor> proxy;
    std::shared_ptr<CellShared>         shared{std::make_shared<CellShared>()};
    int64_t                             doneTick{-1}; // tick on which the timeline was first seen done
    int64_t                             goneTick{-1}; // tick on which the world first read empty under the cell
    OPTIPISTON_PROBE_DO(uint64_t probeGoneFrame{};)
    // What the census and the mesh view currently hold for this cell.
    uint8_t         traits{};
    core::MeshCell  meshKind{core::MeshCell::None};
    core::PushState lastState{core::PushState::Done};
    OPTIPISTON_PROBE_DO(uint64_t probeStartFrame{};) // real frame count when its action started
    OPTIPISTON_PROBE_DO(bool probeSawAir{};)         // the landing poll found the cell empty at least once
};

// Objects whose last owner leaves while gAnimMutex is held die later, outside it.
using Graveyard = std::vector<std::shared_ptr<void const>>;

using CellMap   = std::unordered_map<::BlockPos, PushCell, BlockPosHash>;
using PistonMap = std::unordered_map<::BlockPos, PistonVisual, BlockPosHash>;

// A cell that a newer action replaced while its push was still playing, before the sibling cell of that action
// which picks the block up from there was committed. The sibling takes the tail over, so the commit order of the
// cells of one action does not matter.
struct ReplacedTail {
    uint64_t    action{}; // the newer action
    CarryVisual carry;
};
using TailMap   = std::unordered_map<::BlockPos, ReplacedTail, BlockPosHash>;
using ActionMap = std::unordered_map<uint64_t, ActionVisual>;

// Read-only references to one consistent set of tables: either the live ones under gAnimMutex or a published copy.
struct StateRef {
    CellMap const&   cells;
    PistonMap const& pistons;
    ActionMap const& actions;
};

// Immutable copy of all animation state, rebuilt lazily by the first reader after a change and then shared.
struct RenderView {
    uint64_t                     revision{};
    CellMap                      cells;
    PistonMap                    pistons;
    ActionMap                    actions;
    std::vector<PushCell const*> proxied; // cells that own a live proxy, in table order

    [[nodiscard]] StateRef state() const { return {cells, pistons, actions}; }
};

#if OPTIPISTON_PROBE
// ---- Probe timing: QueryPerformanceCounter scopes, grouped by name and by thread kind ----

enum class PerfId : int {
    Frame,
    QueueEntities,
    RenderBlockEntities,
    DrawMoving,
    DrawArm,
    Dispatch,
    DrawPos,
    PistonTick,
    MeshHook,
    FaceOcclusion,
    MeshBuild,
    DataHandler,
    PistonAction,
    LandedBlock,
    ScanTargets,
    ViewBuild,       // rebuilding the published render view under gAnimMutex
    ProxyCreate,     // constructing one proxy MovingBlockActor
    AnimWait,        // every acquisition of gAnimMutex; total/max are the time spent waiting
    AnimWaitBlocked, // only acquisitions that had to wait
    MeshPublish,     // copying the mesh builder into a new published view
    ScanLookups,     // the game's getBlockEntity for one candidate cell; kept out of our work total
    SweepActions,    // inside Frame
    ReleaseEarly,    // inside Frame
    Count
};

constexpr char const* kPerfNames[] = {"frame",           "queueEntities", "renderBlockEntities",
                                      "drawMoving",      "drawArm",       "dispatch",
                                      "drawPos",         "pistonTick",    "meshHook",
                                      "faceOcclusion",   "meshBuild",     "dataHandler",
                                      "pistonAction",    "landedBlock",   "scanTargets",
                                      "viewBuild",       "proxyCreate",   "animWait",
                                      "animWaitBlocked", "meshPublish",   "scanLookups",
                                      "sweepActions",    "releaseEarly"};
static_assert(std::size(kPerfNames) == static_cast<std::size_t>(PerfId::Count));

struct PerfSlot {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> max{0};
};

PerfSlot gPerf[static_cast<int>(PerfId::Count)][2]; // [id][0 = render thread, 1 = other]
PerfSlot gFrameWork;                                // OptiPiston work per frame on the render thread

// The thread that runs the frame hook.
thread_local bool tRenderThread = false;
// Innermost perf scope open on this thread (a PerfId), or -1; names the caller of shared work such as view builds.
thread_local int tPerfCur = -1;
// Running (not paused) scopes on this thread, and the time the outermost ones added since the last frame end.
thread_local int      tPerfDepth = 0;
thread_local uint64_t tWorkTicks = 0;
// Render-thread time and call counts per scope in the current frame. Inclusive: nested scopes overlap.
thread_local uint64_t tFrameScopeTicks[static_cast<int>(PerfId::Count)]{};
thread_local uint32_t tFrameScopeCalls[static_cast<int>(PerfId::Count)]{};

uint64_t perfNow() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

uint64_t perfFrequency() {
    static uint64_t const frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    return frequency;
}

void perfAdd(PerfSlot& slot, uint64_t ticks) {
    slot.calls.fetch_add(1, std::memory_order_relaxed);
    slot.total.fetch_add(ticks, std::memory_order_relaxed);
    auto seen = slot.max.load(std::memory_order_relaxed);
    while (ticks > seen && !slot.max.compare_exchange_weak(seen, ticks, std::memory_order_relaxed)) {}
}

void perfRecord(PerfId id, uint64_t ticks) {
    perfAdd(gPerf[static_cast<int>(id)][tRenderThread ? 0 : 1], ticks);
    if (tRenderThread) {
        tFrameScopeTicks[static_cast<int>(id)] += ticks;
        ++tFrameScopeCalls[static_cast<int>(id)];
    }
}

// Times our own code only: pause() around calls into the game, so its time is not ours.
class PerfScope {
public:
    explicit PerfScope(PerfId id) : mId(id), mPrev(tPerfCur), mStart(perfNow()) {
        tPerfCur = static_cast<int>(id);
        ++tPerfDepth;
    }
    ~PerfScope() { stop(); }
    PerfScope(PerfScope const&)            = delete;
    PerfScope& operator=(PerfScope const&) = delete;

    void pause() {
        if (!mRunning) return;
        auto const elapsed  = perfNow() - mStart;
        mElapsed           += elapsed;
        mRunning            = false;
        if (--tPerfDepth == 0 && tRenderThread) tWorkTicks += elapsed;
    }
    void resume() {
        if (mRunning || mDone) return;
        mStart   = perfNow();
        mRunning = true;
        ++tPerfDepth;
    }
    void stop() {
        pause();
        if (mDone) return;
        mDone    = true;
        tPerfCur = mPrev;
        perfRecord(mId, mElapsed);
    }

private:
    PerfId   mId;
    int      mPrev;
    uint64_t mStart;
    uint64_t mElapsed{};
    bool     mRunning{true};
    bool     mDone{};
};

// Time in the game's own code on our behalf: recorded like a scope, but not added to the work total.
class GameSpan {
public:
    explicit GameSpan(PerfId id) : mId(id), mStart(perfNow()) {}
    ~GameSpan() { perfRecord(mId, perfNow() - mStart); }
    GameSpan(GameSpan const&)            = delete;
    GameSpan& operator=(GameSpan const&) = delete;

private:
    PerfId   mId;
    uint64_t mStart;
};

// Set at load when a file named perf-only sits in the mod folder: the bookkeeping probes that cost real time stay off,
// so the timings come close to a build without probes.
std::atomic_bool gProbePerfOnly{false};

// Counts behind the per-call costs, summed per window and reset when printed.
struct PerfCounts {
    std::atomic<uint64_t> armCalls{0};      // drawArm entered
    std::atomic<uint64_t> armArms{0};       // of those, a PistonArm with smooth rendering on
    std::atomic<uint64_t> armNested{0};     // arms drawn inside a carrying MovingBlock
    std::atomic<uint64_t> armTypeChecks{0}; // arms that asked the world for the block type name
    std::atomic<uint64_t> armSkipped{0};    // dropped by the head election
    std::atomic<uint64_t> armOffsets{0};    // asked for a body offset
    std::atomic<uint64_t> armShifted{0};    // drawn shifted
    std::atomic<uint64_t> armSamples{0};    // calls the stage timers measured
    std::atomic<uint64_t> armStageTicks[6]{};
    std::atomic<uint64_t> scanKnown{0};      // candidates dropped: the cell table already holds the action
    std::atomic<uint64_t> scanDuplicates{0}; // candidates dropped: already in this piston's list
    std::atomic<uint64_t> scanHits{0};       // lookups that found the piston's own MovingBlock
    std::atomic<uint64_t> scanCreated{0};
    std::atomic<uint64_t> scanFollowed{0};
    std::atomic<uint64_t> bumps{0}; // revision changes, all callers
    std::atomic<uint64_t> bumpTouch{0};
    std::atomic<uint64_t> bumpState{0};
    std::atomic<uint64_t> bumpKind{0};
    std::atomic<uint64_t> bumpForget{0};
    std::atomic<uint64_t> viewEntries{0}; // table entries copied by render view builds
};
PerfCounts gPerfCounts;

// Counters only, no timers inside the loops they describe, so a perf-only run stays close to a build without probes.
constexpr int      kPerfSlots   = static_cast<int>(PerfId::Count) + 1; // last slot: no scope open
constexpr uint32_t kLightSample = 1024;                              // one getLightColor call in this many is timed
struct PerfCounts2 {
    // Render view builds by the scope that asked for them; ticks exclude the wait for the lock.
    std::atomic<uint64_t> viewBy[kPerfSlots]{};
    std::atomic<uint64_t> viewByTicks[kPerfSlots]{};
    std::atomic<uint64_t> viewByEntries[kPerfSlots]{};
    std::atomic<uint64_t> meshPubs{};
    std::atomic<uint64_t> meshPubEntries{};
    std::atomic<uint64_t> sweepCalls{};    // sweepActions entered
    std::atomic<uint64_t> sweepIdle{};     // left at the idle-world gate
    std::atomic<uint64_t> sweepLocked{};   // took gAnimMutex
    std::atomic<uint64_t> sweepAdvanced{}; // the tick had moved, so the tables were walked
    std::atomic<uint64_t> sweepCells{};    // cells handed to rebuildCells by sweeps
    std::atomic<uint64_t> rebuildCalls{};
    std::atomic<uint64_t> rebuildCells{};
    std::atomic<uint64_t> rebuildSubchunks{}; // distinct 16-cube subchunks the +-1 boxes of one call touch, summed
    std::atomic<uint64_t> pruneRuns{};        // markPistonStepped walked a whole table
    std::atomic<uint64_t> pruneScanned{};
    std::atomic<uint64_t> pruneErased{};
    std::atomic<uint64_t> stepCalls{};        // markPistonStepped calls
    std::atomic<uint64_t> clockCalls{};       // visualClock calls on the render thread
    std::atomic<uint64_t> clockFrames{};
    std::atomic<uint64_t> lightSeen[2]{};     // sampled getLightColor calls [render, other]
    std::atomic<uint64_t> lightGameTicks[2]{}; // sampled time inside the game's function
    std::atomic<uint64_t> lightOwnTicks[2]{};  // sampled time in our hook around it
    std::atomic<uint64_t> scanWindow{};        // pistons inside their scan window with a live action
    std::atomic<uint64_t> scanNoAttached{};    // of those, no attached list yet
    std::atomic<uint64_t> scanAttSettled{};    // of those, a list whose derived cells are all already tracked
    // Lookups and their hits by where the candidate came from: 0 line, 1 attached, 2 attached+facing, 3 attached-facing.
    std::atomic<uint64_t> scanLookRole[4]{};
    std::atomic<uint64_t> scanHitRole[4]{};
    std::atomic<uint64_t> scanHitLineOnly{};   // hits by the line when the piston had an attached list (the list missed it)
};
PerfCounts2 gPerfCounts2;
thread_local uint32_t tClockCalls = 0;

// drawArm tallies, flushed once per frame so the hot path adds no atomics.
struct ArmTally {
    uint32_t calls{};
    uint32_t arms{};
    uint32_t nested{};
    uint32_t typeChecks{};
    uint32_t skipped{};
    uint32_t offsets{};
    uint32_t shifted{};
};
thread_local ArmTally tArmTally;

enum class ArmStage : int { Setup, Cargo, Elect, Note, Progress, Offset };
thread_local uint32_t tArmSeq = 0;

// Times every eighth drawArm call stage by stage, so the timer does not dominate a sub-microsecond call.
class ArmSplit {
public:
    ArmSplit() : mOn((++tArmSeq & 7u) == 0), mLast(mOn ? perfNow() : 0) {}
    ~ArmSplit() {
        if (!mOn) return;
        for (int i = 0; i < 6; ++i) gPerfCounts.armStageTicks[i].fetch_add(mTicks[i], std::memory_order_relaxed);
        gPerfCounts.armSamples.fetch_add(1, std::memory_order_relaxed);
    }
    ArmSplit(ArmSplit const&)            = delete;
    ArmSplit& operator=(ArmSplit const&) = delete;

    void stage(ArmStage which) {
        if (!mOn) return;
        auto const now                   = perfNow();
        mTicks[static_cast<int>(which)] += now - mLast;
        mLast                            = now;
    }
    // Drops the time since the last mark.
    void skip() {
        if (mOn) mLast = perfNow();
    }

private:
    bool     mOn;
    uint64_t mLast;
    uint64_t mTicks[6]{};
};

#define OPTIPISTON_PERF_SCOPE(var, id) OPTIPISTON_PROBE_DO(PerfScope var(PerfId::id);)

// Set around the probe's own cross-checks, so their lock waits stay out of the timings.
thread_local bool tUntimedLock = false;

// Same interface as std::mutex for lock_guard/unique_lock; measures how long acquiring it takes.
class AnimMutex {
public:
    void lock() {
        if (tUntimedLock) {
            mMutex.lock();
            return;
        }
        if (mMutex.try_lock()) {
            perfRecord(PerfId::AnimWait, 0);
            return;
        }
        auto const start = perfNow();
        mMutex.lock();
        auto const waited = perfNow() - start;
        perfRecord(PerfId::AnimWait, waited);
        perfRecord(PerfId::AnimWaitBlocked, waited);
    }
    void unlock() { mMutex.unlock(); }

private:
    std::mutex mMutex;
};

// Level tick and render-frame count the log lines carry. The latter counts every frame hook call.
std::atomic<int64_t>  gFrameLt{0};
std::atomic<uint64_t> gRealFrame{0};
#else
#define OPTIPISTON_PERF_SCOPE(var, id)
using AnimMutex = std::mutex;
#endif

// Writers hold gAnimMutex. Readers use the published views below and never take it.
AnimMutex          gAnimMutex;
uint64_t           gNextAction = 1;
core::ClockTracker gClockTracker; // guarded by gAnimMutex
// Keyed by position: replay rebuilds piston instances mid-action.
PistonMap        gPistonVisuals;
ActionMap        gActionVisuals;
CellMap          gCells;
TailMap          gReplacedTails;       // guarded by gAnimMutex
std::atomic_bool gStateWatch{false};   // any cell or piston visual
std::atomic_bool gFaceWatch{false};    // any cell still animating
std::atomic_bool gLandedWatch{false};  // any landed cell with its mesh hidden
std::atomic_bool gMeshWatch{false};    // any cell hiding or awaiting its mesh
std::atomic_bool gActorWatch{false};   // any cell waiting for its own actor
std::atomic_bool gPendingWatch{false}; // gPendingRemesh is not empty
// Counts frames that drew block actors; export also renders passes without them.
std::atomic<uint64_t> gFrame{1};
std::atomic_bool      gFrameDrewActors{false};
// Chunk geometry being tessellated on this thread.
thread_local ::RenderChunkGeometry const* tMeshGeometry = nullptr;
// Cells tessellated as real blocks after their re-mesh request, per build that has not been uploaded yet.
std::unordered_map<::RenderChunkGeometry const*, std::vector<::BlockPos>> gReleasedMesh;
// Cells a build met as awaiting but found the stand-in or air in its chunk copy: it cannot contain the block.
std::unordered_map<::RenderChunkGeometry const*, std::vector<::BlockPos>> gStaleMesh;
// Cells whose awaiting state was just published; builds in flight at that moment may not have seen it.
std::vector<::BlockPos> gAwaitPublished;
// Builds between start and commit, with the origin of their chunk; and the cells created while one was running.
// Leaf lock: taken under gAnimMutex, never the other way round.
std::mutex                                                                gFlightMutex;
std::unordered_map<::RenderChunkGeometry const*, ::BlockPos>              gBuildsInFlight;
std::unordered_map<::RenderChunkGeometry const*, std::vector<::BlockPos>> gStaleRisk;
// Landed cells found by the renderer; the next tick re-meshes them.
std::vector<::BlockPos> gPendingRemesh;
// Landed cells are re-meshed just before their motion ends, so the proxy does not have to cover the re-mesh. All in
// visual ticks. Written by the render and frame threads without gAnimMutex, hence atomic.
std::atomic<double>                   gLastVisualTime{0.0};
std::chrono::steady_clock::time_point gPrevFrameWall{}; // frame thread only
std::atomic<double>                   gFrameStep{0.15}; // visual ticks per frame
std::atomic<double>                   gFrameSeconds{1.0 / 60.0};
std::atomic<double>                   gTicksPerSecond{20.0};
// Request to live in wall-clock seconds, so a world speed or frame rate change leaves it valid.
std::atomic<double> gMeshLatency{0.03};
constexpr double    kMaxMeshLatencySeconds = 0.4;
// A slow build then shows the proxy a little late instead of the block early.
constexpr double kMaxEarlyLeadTicks = 0.3;
// Leaf lock: the render thread queues early re-meshes here, the frame thread drains them.
std::mutex              gEarlyMutex;
std::vector<::BlockPos> gEarlyRemesh;
std::vector<::BlockPos> gEarlyScratch; // frame thread only
std::atomic<int64_t>    gLastTick{0};

// ---- Published views: the only state mesh and render threads read, so they never take gAnimMutex ----

#if OPTIPISTON_PROBE
std::atomic<uint64_t> gProbeViewBuilds{0};
std::atomic<uint64_t> gProbeMeshPublishes{0};
std::atomic<uint64_t> gProbeViewChecks{0};
std::atomic<uint64_t> gProbeViewMismatch{0};
std::atomic<uint64_t> gProbeActorFrames{0}; // probeFrameEnd calls in the current frameWork window
#endif

// The builder, its dirty flag and the census are guarded by gAnimMutex; the published copy is replaced whole.
core::MeshView                                     gMeshBuilder;
bool                                               gMeshDirty = false;
core::TraitCensus                                  gCensus;
std::atomic<std::shared_ptr<core::MeshView const>> gMeshView;
std::atomic<uint64_t>                              gMeshVersion{0};
// Bumped under gAnimMutex by every change the render view has to show.
std::atomic<uint64_t>                          gRevision{1};
std::atomic<std::shared_ptr<RenderView const>> gRenderView;

core::MeshView const& emptyMeshView() {
    static core::MeshView const empty;
    return empty;
}

class MeshBuildScope;
// The chunk build running on this thread.
thread_local MeshBuildScope* tBuild = nullptr;

// Wraps one chunk build: the mesh view is cached for the build and re-read only after a publication, and cells
// released to the mesher are reported once at the end instead of under a lock per block.
class MeshBuildScope {
public:
    explicit MeshBuildScope(::RenderChunkGeometry const* geometry) : mGeometry(geometry), mSaved(tBuild) {
        tBuild = this;
    }
    ~MeshBuildScope() {
        tBuild = mSaved;
        if ((mReleased.empty() && mStale.empty()) || !gMeshWatch.load(std::memory_order_relaxed)) return;
        std::lock_guard const guard(gAnimMutex);
        if (!mReleased.empty()) {
            auto& list = gReleasedMesh[mGeometry];
            list.insert(list.end(), mReleased.begin(), mReleased.end());
        }
        if (!mStale.empty()) {
            auto& list = gStaleMesh[mGeometry];
            list.insert(list.end(), mStale.begin(), mStale.end());
        }
    }
    MeshBuildScope(MeshBuildScope const&)            = delete;
    MeshBuildScope& operator=(MeshBuildScope const&) = delete;

    core::MeshView const& view() {
        // Version first: a view newer than the version read only costs one more reload.
        auto const version = gMeshVersion.load(std::memory_order_acquire);
        if (version != mVersion) {
            mView    = gMeshView.load(std::memory_order_acquire);
            mVersion = version;
        }
        return mView ? *mView : emptyMeshView();
    }
    void released(::BlockPos const& pos) { mReleased.push_back(pos); }
    void stale(::BlockPos const& pos) { mStale.push_back(pos); }

private:
    ::RenderChunkGeometry const*          mGeometry;
    MeshBuildScope*                       mSaved;
    uint64_t                              mVersion{~0ull};
    std::shared_ptr<core::MeshView const> mView;
    std::vector<::BlockPos>               mReleased;
    std::vector<::BlockPos>               mStale;
};

// The current mesh view: the build's cached one on a mesh thread, otherwise a fresh load.
class MeshViewPin {
public:
    core::MeshView const& get() {
        if (tBuild) return tBuild->view();
        mHold = gMeshView.load(std::memory_order_acquire);
        return mHold ? *mHold : emptyMeshView();
    }

private:
    std::shared_ptr<core::MeshView const> mHold;
};

// The render view matching the current revision, rebuilt by the first reader after a change.
std::shared_ptr<RenderView const> acquireRenderView() {
    auto view = gRenderView.load(std::memory_order_acquire);
    if (view && view->revision == gRevision.load(std::memory_order_acquire)) return view;
    // Declared before the lock, so the view it replaces dies after the lock is released.
    std::shared_ptr<RenderView const> retired;
    std::lock_guard const             guard(gAnimMutex);
    auto const                        revision = gRevision.load(std::memory_order_acquire);
    if (auto current = gRenderView.load(std::memory_order_acquire); current && current->revision == revision)
        return current;
    OPTIPISTON_PROBE_DO(int const caller = tPerfCur < 0 ? kPerfSlots - 1 : tPerfCur; auto const buildStart = perfNow();)
    OPTIPISTON_PERF_SCOPE(perf, ViewBuild);
    OPTIPISTON_PROBE_DO(gProbeViewBuilds.fetch_add(1, std::memory_order_relaxed);)
    auto fresh      = std::make_shared<RenderView>();
    fresh->revision = revision;
    fresh->cells    = gCells;
    fresh->pistons  = gPistonVisuals;
    fresh->actions  = gActionVisuals;
    OPTIPISTON_PROBE_DO(gPerfCounts.viewEntries.fetch_add(
        fresh->cells.size() + fresh->pistons.size() + fresh->actions.size(),
        std::memory_order_relaxed
    );)
    fresh->proxied.reserve(fresh->cells.size());
    for (auto const& [pos, rec] : fresh->cells)
        if (rec.proxy && rec.timeline.state() != PushState::Done) fresh->proxied.push_back(&rec);
    std::shared_ptr<RenderView const> published = std::move(fresh);
    retired                                     = gRenderView.exchange(published, std::memory_order_acq_rel);
    OPTIPISTON_PROBE_DO(
        gPerfCounts2.viewBy[caller].fetch_add(1, std::memory_order_relaxed);
        gPerfCounts2.viewByTicks[caller].fetch_add(perfNow() - buildStart, std::memory_order_relaxed);
        gPerfCounts2.viewByEntries[caller].fetch_add(
            published->cells.size() + published->pistons.size() + published->actions.size(),
            std::memory_order_relaxed
        );
    )
    return published;
}

class RenderViewScope;
// The render pass running on this thread.
thread_local RenderViewScope* tRenderScope = nullptr;

// Wraps one block-entity render pass: everything drawn inside answers from one cached view, refreshed only when the
// revision moves. Views it replaced stay alive to the end, since callers may still hold references into them.
class RenderViewScope {
public:
    RenderViewScope() : mSaved(tRenderScope) { tRenderScope = this; }
    ~RenderViewScope() { tRenderScope = mSaved; }
    RenderViewScope(RenderViewScope const&)            = delete;
    RenderViewScope& operator=(RenderViewScope const&) = delete;

    RenderView const& get() {
        if (!mView || mView->revision != gRevision.load(std::memory_order_acquire)) refresh();
        return *mView;
    }
    // Like get(), but accepts a view that lacks only cells this thread pure-inserted since the last refresh (none
    // replaced or superseded). Lets a burst of first-time game MovingBlocks be handled without rebuilding the view
    // after each one.
    RenderView const& lagged() {
        auto const revision = gRevision.load(std::memory_order_acquire);
        if (!mView || (mView->revision != revision && !(mLagging && mLagRevision == revision))) refresh();
        return *mView;
    }
    // The clock read once for the whole pass: every arm of one pass answers from the same sample.
    template <class Sample>
    std::optional<core::ClockSample> const& clock(Sample&& sample) {
        if (!mClockKnown) {
            mClock      = sample();
            mClockKnown = true;
        }
        return mClock;
    }
    // Caller holds gAnimMutex and has just moved the revision from `before` to `after` by adding a cell.
    void created(uint64_t before, uint64_t after) {
        if (!mView || (mView->revision != before && !(mLagging && mLagRevision == before))) return;
        mLagging     = true;
        mLagRevision = after;
    }

private:
    void refresh() {
        auto fresh = acquireRenderView();
        if (mView) mRetired.push_back(std::move(mView));
        mView    = std::move(fresh);
        mLagging = false;
    }

    RenderViewScope*                               mSaved;
    std::shared_ptr<RenderView const>              mView;
    std::vector<std::shared_ptr<RenderView const>> mRetired;
    bool                                           mLagging{};
    uint64_t                                       mLagRevision{};
    std::optional<core::ClockSample>               mClock;
    bool                                           mClockKnown{};
};

// The current render view: the pass's cached one on the render thread, otherwise a fresh acquisition.
class ViewPin {
public:
    RenderView const& get() {
        if (tRenderScope) return tRenderScope->get();
        mHold = acquireRenderView();
        return *mHold;
    }
    RenderView const& lagged() {
        if (tRenderScope) return tRenderScope->lagged();
        return get();
    }

private:
    std::shared_ptr<RenderView const> mHold;
};

uint64_t actorNote(PushCell const& rec) { return rec.shared->actorFrame.load(std::memory_order_relaxed); }

// Who draws the cell on `frame`, counting a landed-actor sighting the timeline has not absorbed yet.
PushDrawer drawerOf(PushCell const& rec, uint64_t frame) { return rec.timeline.drawer(frame, actorNote(rec)); }

// ---- Writers: caller holds gAnimMutex ----

void flagStaleRiskLocked(::BlockPos const& cell, bool awaiting = false);

void bumpLocked() {
    gRevision.fetch_add(1, std::memory_order_release);
    OPTIPISTON_PROBE_DO(gPerfCounts.bumps.fetch_add(1, std::memory_order_relaxed);)
}

// Brings the census and the mesh view in step with the cell's timeline. `touch` says that other data the render view
// copies changed too.
void syncCellLocked(::BlockPos const& pos, PushCell& rec, bool touch) {
    auto const traits = core::pushTraits(rec.timeline);
    auto const state  = rec.timeline.state();
    auto const kind   = core::meshCellOf(rec.timeline);
    if (traits != rec.traits) {
        gCensus.replace(rec.traits, traits);
        rec.traits = traits;
    }
    OPTIPISTON_PROBE_DO(bool const asked = touch;)
    if (state != rec.lastState) {
        rec.lastState = state;
        OPTIPISTON_PROBE_DO(if (!touch) gPerfCounts.bumpState.fetch_add(1, std::memory_order_relaxed);)
        touch = true;
    }
    if (kind != rec.meshKind) {
        gMeshBuilder.set(cellOf(pos), kind);
        rec.meshKind = kind;
        gMeshDirty   = true;
        if (kind == core::MeshCell::Awaiting) gAwaitPublished.push_back(pos);
        OPTIPISTON_PROBE_DO(if (!touch) gPerfCounts.bumpKind.fetch_add(1, std::memory_order_relaxed);)
        touch = true;
    }
    OPTIPISTON_PROBE_DO(if (asked) gPerfCounts.bumpTouch.fetch_add(1, std::memory_order_relaxed);)
    if (touch) bumpLocked();
}

// The cell is about to leave gCells.
void forgetCellLocked(::BlockPos const& pos, PushCell& rec) {
    gCensus.replace(rec.traits, 0);
    rec.traits = 0;
    if (rec.meshKind != core::MeshCell::None) {
        gMeshBuilder.erase(cellOf(pos));
        rec.meshKind = core::MeshCell::None;
        gMeshDirty   = true;
    }
    OPTIPISTON_PROBE_DO(gPerfCounts.bumpForget.fetch_add(1, std::memory_order_relaxed);)
    bumpLocked();
}

// Publishes the mesh view if it changed and refreshes the watch flags, all without scanning the tables.
void flushLocked() {
    if (gMeshDirty) {
        gMeshDirty = false;
        OPTIPISTON_PERF_SCOPE(publish, MeshPublish);
        gMeshView.store(std::make_shared<core::MeshView>(gMeshBuilder), std::memory_order_release);
        gMeshVersion.fetch_add(1, std::memory_order_release);
        OPTIPISTON_PROBE_DO(
            gPerfCounts2.meshPubs.fetch_add(1, std::memory_order_relaxed);
            gPerfCounts2.meshPubEntries.fetch_add(gMeshBuilder.size(), std::memory_order_relaxed);
        )
        OPTIPISTON_PROBE_DO(gProbeMeshPublishes.fetch_add(1, std::memory_order_relaxed);)
    }
    // Builds that started before the publication cannot be relied on to have seen the awaiting state.
    if (!gAwaitPublished.empty() && !gMeshDirty) {
        for (auto const& awaitingPos : gAwaitPublished) flagStaleRiskLocked(awaitingPos, true);
        gAwaitPublished.clear();
    }
    bool const suppressed = gCensus.any(core::TraitSuppressed);
    bool const awaiting   = gCensus.any(core::TraitAwaiting);
    gStateWatch.store(!gCells.empty() || !gPistonVisuals.empty(), std::memory_order_relaxed);
    gFaceWatch.store(gCensus.any(core::TraitAnimating), std::memory_order_relaxed);
    gLandedWatch.store(suppressed, std::memory_order_relaxed);
    gMeshWatch.store(suppressed || awaiting, std::memory_order_relaxed);
    gActorWatch.store(gCensus.any(core::TraitWantsActor), std::memory_order_relaxed);
    if (!awaiting) {
        if (!gReleasedMesh.empty()) gReleasedMesh.clear();
        if (!gStaleMesh.empty()) gStaleMesh.clear();
    }
}

// flushLocked for callers that may have emptied the tables: the render view then goes too, after the lock.
void settleLocked(Graveyard& drop) {
    flushLocked();
    if (gCells.empty() && gPistonVisuals.empty()) {
        if (auto retired = gRenderView.exchange(nullptr, std::memory_order_acq_rel)) drop.push_back(std::move(retired));
    }
}

// Moves sightings of landed actors into the timelines, which end a handover from them.
void foldActorNotesLocked() {
    if (!gCensus.any(core::TraitWantsActor)) return;
    for (auto& [pos, rec] : gCells) {
        if (!(rec.traits & core::TraitWantsActor)) continue;
        if (auto const frame = actorNote(rec)) rec.timeline.landedActorSeen(frame);
    }
}

#if OPTIPISTON_PROBE
struct ProbeCounters {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> proxiesCreated{0};
    std::atomic<uint64_t> proxiesReleased{0};
    std::atomic<uint64_t> vanillaHidden{0};
    std::atomic<uint64_t> proxyDraws{0};
    std::atomic<uint64_t> zero{0};
    std::atomic<uint64_t> doubled{0};
    std::atomic<uint64_t> liveDouble{0};   // proxy away from the cell while the chunk's last committed build holds it
    std::atomic<uint64_t> buildLat[6]{};   // start-to-commit frames: 0-1, 2-3, 4-7, 8-15, 16-31, 32+
    std::atomic<uint64_t> buildLatSlow{0}; // logged slow builds
    std::atomic<uint64_t> snapshotMissing{0};
    std::atomic<uint64_t> handoverShown{0};
    std::atomic<uint64_t> handoverTimeout{0};
    std::atomic<uint64_t> dispatchNoRender{0}; // dispatched, but the renderer or getDrawPos never ran for the proxy
    std::atomic<uint64_t> noDispatch{0};       // drawer Proxy, no dispatch this frame
    std::atomic<uint64_t> meshGap{0};          // drawer Mesh/Handover without a verified committed build
    std::atomic<uint64_t> meshEmpty{0};        // a build tessellated the real block but it produced no geometry
    std::atomic<uint64_t> meshRegress{0};      // a later build of the geometry no longer contained a verified cell
    std::atomic<uint64_t> buildStarts{0};      // render-chunk builds begun
    std::atomic<uint64_t> buildCommits{0};     // builds that reached endRebuild
    std::atomic<uint64_t> rebuildAsks{0};      // our own rebuildCells calls with at least one cell
    std::atomic<uint64_t> askCells{0};         // cells in those calls
    std::atomic<uint64_t> staleFlag{0};        // cells created while a build of their chunk was running
    std::atomic<uint64_t> staleAsk{0};         // of those, cells asked to re-mesh once that build was live
    std::atomic<uint64_t> awaitFlag{0};        // cells published as awaiting while a build of their chunk was running
    std::atomic<uint64_t> landReask{0};   // awaiting cells asked again because the live build cannot have held them
    std::atomic<uint64_t> cleanCommit{0}; // awaiting cells that ended on a clean build that never visited them
    std::atomic<uint64_t> approachHidden{
        0
    }; // tessellations of a real block skipped because its proxy is still gliding in
    std::atomic<uint64_t> startGap{0};
    std::atomic<uint64_t> armGap{0};         // an animating piston's head was drawn last actor frame but not this one
    std::atomic<uint64_t> armDouble{0};      // a piston's head was drawn at two different positions in one frame
    std::atomic<uint64_t> armSkipped{0};     // head draws dropped by the owner election, all pistons
    std::atomic<uint64_t> rbeCalls[2]{};     // renderBlockEntities calls per pass
    std::atomic<uint64_t> rbeFrame[2]{};     // calls in the current frame
    std::atomic<uint64_t> rbeMax[2]{};       // most calls in one frame
    std::atomic<uint64_t> rbeMissing[2]{};   // frames with no call at all
    std::atomic<uint64_t> snapDetached{0};   // snapshotMoving skipped an instance that is not the cell's block entity
    std::atomic<uint64_t> olderDropped{0};   // commit dropped a proxy because the cell already has a newer action
    std::atomic<uint64_t> replaced{0};       // commit replaced a record of another action
    std::atomic<uint64_t> ghostDraws{0};     // the game's instance drawn while its cell shows no matching block
    std::atomic<uint64_t> ghostInstances{0}; // distinct game instances that ever drew like that
    std::atomic<uint64_t> stuckCells{0};     // record not Done long after its visual segment ended
    std::atomic<uint64_t> doneWithProxy{0};  // record Done that still owns a proxy
    std::atomic<uint64_t> actorGap{0};       // carried actor: no proxy, no landed actor and no game instance drawn
    std::atomic<uint64_t> actorDouble{0};    // carried actor: proxy and landed actor drawn in the same frame
    std::atomic<uint64_t> carriedDone{0};    // carried cells that reached Done
    std::atomic<uint64_t> carriedTimeout{0}; // of those, the handover ended by timeout
    std::atomic<uint64_t> leakMesh{0};      // a build produced the real block of a cell whose proxy is still gliding in
    std::atomic<uint64_t> outerPassed{0};   // tessellateInWorld reached for a managed cell, not suppressed
    std::atomic<uint64_t> outerHidden{0};   // tessellateInWorld reached for a managed cell, suppressed
    std::atomic<uint64_t> innerPassed{0};   // tessellateBlockInWorld reached for a managed cell, not suppressed
    std::atomic<uint64_t> innerHidden{0};   // tessellateBlockInWorld reached for a managed cell, suppressed
    std::atomic<uint64_t> actorStripped{0}; // proxy drawn without its actor copy because the landed actor was drawn
    // Pre-arrival flicker probe (r74).
    std::atomic<uint64_t> dupEpisodes{0};    // closed runs of consecutive frames in which a cell was drawn twice
    std::atomic<uint64_t> dupMs{0};          // their summed duration, wall milliseconds
    std::atomic<uint64_t> dupMaxFrames{0};   // longest such run
    std::atomic<uint64_t> dupVisFrames{0};   // frames of those runs in which the proxy was over 0.1 block from the cell
    std::atomic<uint64_t> animCellFrames{0}; // cell-frames in Animating drawn by their proxy (denominator)
    std::atomic<uint64_t> zeroAnim{0};       // zero (nobody draws the cell) while the cell is Animating
    std::atomic<uint64_t> unpositioned{0};   // Animating proxy drawn while visualDrawOffset gave no offset
    std::atomic<uint64_t> posChecks{0};      // an Animating proxy compared with its position on the previous frame
    std::atomic<uint64_t> posJump{0};        // it moved over twice as far as the visual time that passed allows
    std::atomic<uint64_t> posBack{0};        // it moved away from its cell while the visual time moved forward
    std::atomic<uint64_t> lightQueries{0};   // light queries seen inside proxy dispatches (positive control; 26.50 only)
    std::atomic<uint64_t> lightFrames{0};    // Animating proxy frames in which the light hook saw queries
    std::atomic<uint64_t> lightAnyDark{0};   // of those, frames in which some queried cell read 0/0
    std::atomic<uint64_t> lightSelfDark{0};  // of those, frames in which the proxy's own cell read 0/0
    std::atomic<uint64_t> lightFlip{0};      // dark pattern differed from the cell's previous Animating frame
    std::atomic<uint64_t> lightSelfFixed{0}; // own-cell 0/0 answers replaced by the cell's remembered lit answer
    std::atomic<uint64_t> lightSelfNoMem{0}; // own-cell 0/0 answers with nothing remembered, passed on as they were
    std::atomic<uint64_t> armGapMid{0};      // armGap of a running piston that still had over 0.05 block to go
    std::atomic<uint64_t> clkChecks{0};      // visual clock advance compared with the wall clock
    std::atomic<uint64_t> clkBack{0};        // the visual clock went backwards between two frames
    std::atomic<uint64_t> clkStall{0};       // it did not move although the wall clock did
    std::atomic<uint64_t> clkSpread{0};      // proxies of one frame drew with visual times over 0.02 tick apart
    std::atomic<uint64_t> clkErr[7]{};       // |advance - wall*rate| in ticks: <.02 <.05 <.1 <.25 <.5 <.9 >=.9
    std::atomic<uint64_t> armGapLen[5]{};    // frames a head stayed gone before it drew again: 1, 2, 3-5, 6-20, 21+
    std::atomic<uint64_t> scanPistons{0};    // pistons scanTargets looked at while inside their scan window
    std::atomic<uint64_t> scanLookups{0};   // getBlockEntity calls made by scanTargets
    std::atomic<uint64_t> cellsMax{0};      // largest cell table seen since the last sum
    std::atomic<uint64_t> pistonsMax{0};    // largest piston visual table seen since the last sum
    std::atomic<uint64_t> actionsMax{0};    // largest action visual table seen since the last sum
    std::atomic<uint64_t> meshCommitLines{0};   // meshCommit lines written
    std::atomic<uint64_t> proxyEndLines{0};     // proxyEnd lines written at normal speed
    std::atomic<uint64_t> proxyEndSlowLines{0}; // proxyEnd lines written in a slowed world
};

// What drew one cell during the current frame.
struct ProbeTally {
    int       dispatched{}; // drawProxies called dispatcher.render
    int       rendered{};   // the MovingBlock renderer path ran for the proxy
    int       drawPos{};    // getDrawPos was reached for the proxy
    int       pass{-1};     // 0 opaque, 1 alpha
    int       vanilla{};
    float     minOffset{1.0e9f};
    float     maxOffset{-1.0e9f};
    double    time{};   // visual time of the last draw that positioned the proxy
    bool      timed{};  // time is valid; vx/vy/vz hold that draw's offset
    float     vx{}, vy{}, vz{};
    int       lightQ{};        // light queries made while the proxy was dispatched
    int       lightDark{};     // of those, answered 0/0
    int       lightSelfDark{}; // of those, asked for the proxy's own cell and answered 0/0
    int       why{}; // ProbeWhy::reason of the last game instance drawn for the cell
    uint64_t  whyRec{};
    uint64_t  whyPiston{};
    PushState whyState{PushState::Done};
};

// Per managed cell, reset when a newer action takes the cell over.
struct ProbeCell {
    uint64_t                     action{};
    bool                         firstRender{};
    int                          hiddenBeforeRender{};
    bool                         haveDrawer{};
    PushDrawer                   drawer{PushDrawer::None};
    uint64_t                     switchFrame{};
    int64_t                      lastLost{-1000};
    int64_t                      lastMeshGap{-1000};
    int64_t                      lastNoDispatch{-1000};
    int64_t                      lastStuckLog{-1000};
    int64_t                      lastDoneProxyLog{-1000};
    int64_t                      lastActorGap{-1000};
    int64_t                      lastActorDouble{-1000};
    bool                         summarized{}; // carried cell: first observed Done
    uint64_t                     doneFrame{};  // frame of that observation
    int                          animFrames{};
    int                          landingFrames{};
    int                          handoverFrames{};
    int                          proxyFrames{};
    int                          actorFrames{};
    bool                         meshVerified{};
    uint64_t                     meshCommit{};
    ::RenderChunkGeometry const* geometry{};
    uint64_t                     createdFrame{}; // frame on which this cell record was created
    uint64_t                     builtFrame{}; // frame on which the build that verified the mesh tessellated the block
    int                          builtKind{};  // core::MeshCell the mesh view held for the cell at that moment
    uint64_t                     lastTessFrame{}; // last frame a build tessellated the real block of this cell
    bool                         lastTessProduced{};
    uint64_t vanishFrames{}; // consecutive frames the cell's block was in the world but drawn by nobody
    int      doubleLogged{}; // double-draw frames of this action so far
    // The open double-draw episode (consecutive frames with the same cell drawn twice); dupFrames == 0 when none.
    int                                   dupFrames{};
    uint64_t                              dupStart{};
    uint64_t                              dupLastFrame{};
    float                                 dupStartOff{}; // distance the proxy still had to go on the first frame
    float                                 dupEndOff{};   // ... and on the last
    int                                   dupVis{};      // frames of the episode with the proxy over 0.1 block away
    char const*                           dupWho{};
    char const*                           dupState{};
    std::string                           dupWorld; // block the world held in the cell when the episode began
    std::chrono::steady_clock::time_point dupStartWall{};
    std::chrono::steady_clock::time_point dupLastWall{};
    // The previous Animating frame that positioned the proxy: its offset from the cell, the visual time, the light.
    float    lastX{}, lastY{}, lastZ{};
    double   lastTime{};
    uint64_t lastPosFrame{};
    bool     havePos{};
    int      lastLightSig{-1}; // queries, dark answers and own-cell dark answers of the previous Animating frame
    uint64_t lastLightFrame{};
    int      stepLogged{};
    int      unposLogged{};
    uint64_t regressFrame{}; // last frame a build committed without this cell's block (the mesh stopped showing it)
    int      zeroLogged{};
    int      followStarts{};  // builds of the geometry started after the verifying commit
    int      followCommits{}; // builds of the geometry committed after it
    uint64_t originStarts{};  // builds of the cell's chunk started before the record was created
    uint64_t originCommits{}; // builds of the cell's chunk committed before it
};

// One real block a build tessellated for a managed cell.
struct ProbeBuilt {
    ::BlockPos pos;
    uint64_t   frame{};
    int        kind{};
};

// The last re-mesh request of a cell. src: 0 early (near the end of the segment), 2 segment expired.
struct ProbeRemesh {
    int     src{};
    double  remain{}; // visual ticks the proxy still had to move at the request
    int64_t tick{};
};

// Why vanillaHidden let the game's instance of a cell draw.
struct ProbeWhy {
    int       reason{}; // 0 unmanaged, 1 pistonPos mismatch, 2 newer action, 3 drawer Vanilla, 4 detached (hidden)
    uint64_t  recAction{};
    uint64_t  pistonAction{};
    PushState state{PushState::Done};
};

// What drawArm did for the arm at one position during the current frame.
struct ProbeArm {
    int    calls{};   // entered for a PistonArm with smooth rendering on
    int    skipped{}; // dropped by the head owner election
    int    drawn{};   // reached draw()
    int    nested{};  // of those, drawn inside a carrying MovingBlock
    int    cargo{};   // of the calls, the arm was cargo
    ::Vec3 first{};   // position of the first draw
    bool   spread{};  // a later draw sat elsewhere than the first
};

// Per animating piston across frames; frames are logFrame values.
struct ProbeArmCell {
    uint64_t action{};
    int64_t  gapStart{-1};        // frame of the armGap still open, -1 when none
    int64_t  lastDrawnFrame{-1000};
    int64_t  lastActiveFrame{-1000};
    int64_t  lastLogFrame{-1000}; // armGap
    int64_t  lastDoubleFrame{-1000};
};

// Per game MovingBlock instance that was drawn while its cell no longer showed a matching block; frames are real
// frames.
struct ProbeGhost {
    uint64_t firstFrame{};
    uint64_t lastSeenFrame{};
    uint64_t lastLogFrame{};
    uint64_t draws{};
};

ProbeCounters                                              gProbe;
std::mutex                                                 gProbeMutex; // leaf lock
std::string                                                gProbeBuffer;
std::FILE*                                                 gProbeFile = nullptr;
std::chrono::steady_clock::time_point                      gProbeLastSum{};
std::unordered_map<::BlockPos, ProbeTally, BlockPosHash>   gProbeTally;
// The first positioned proxy draw of the current frame: when it happened and the visual time it used.
bool                                                       gProbeDrawSeen{};
std::chrono::steady_clock::time_point                      gProbeDrawWall{};
double                                                     gProbeDrawTime{};
// The previous frame that drew a positioned proxy, for the clock-advance check.
bool                                                       gProbeClkValid{};
std::chrono::steady_clock::time_point                      gProbeClkWall{};
double                                                     gProbeClkTime{};
double                                                     gProbeClkRate{};
std::unordered_map<::BlockPos, ProbeCell, BlockPosHash>    gProbeCells;
std::unordered_map<::BlockPos, ProbeRemesh, BlockPosHash>  gProbeRemesh;
std::unordered_map<::BlockPos, ProbeArm, BlockPosHash>     gProbeArm;      // this frame, by arm position
std::unordered_map<::BlockPos, ProbeArmCell, BlockPosHash> gProbeArmCells; // by piston position
std::unordered_map<std::uintptr_t, ProbeGhost>             gProbeGhosts;   // by address of the game's instance
// Cells tessellated as the real block by a build that has not committed yet.
std::unordered_map<::RenderChunkGeometry const*, std::vector<ProbeBuilt>> gProbeBuilt;
struct ProbeGeom {
    bool          valid{};
    unsigned long flags{};
    int           iter{};
    unsigned long interlock{};
    unsigned      version{};
    unsigned      neighbor[6]{};
    uint64_t      firstChange{};
    uint64_t      lastChange{};
    float         ready{};
};

ProbeGeom probeGeomOf(::RenderChunkGeometry const* geometry) {
    ProbeGeom s;
    if (!geometry) return s;
    s.valid              = true;
    s.flags              = geometry->mFlags.get().to_ulong();
    s.iter               = geometry->mBuildIterationCount;
    s.interlock          = geometry->mInterlockBitField.get().to_ulong();
    s.version            = geometry->mBuildVersionNumber;
    auto const& versions = geometry->mInterlockNeighborVersionNumbers.get();
    for (std::size_t i = 0; i < 6; ++i) s.neighbor[i] = versions[i];
    s.firstChange = static_cast<uint64_t>(geometry->mFirstChangeTick.get().tickID);
    s.lastChange  = static_cast<uint64_t>(geometry->mLastChangeTick.get().tickID);
    s.ready       = geometry->mReadyTime;
    return s;
}

std::string probeGeomText(ProbeGeom const& s) {
    if (!s.valid) return "-";
    char buf[192];
    std::snprintf(
        buf,
        sizeof buf,
        "f=%lx it=%d il=%lx v=%u nv=%u.%u.%u.%u.%u.%u ft=%llu lt=%llu rt=%.2f",
        s.flags,
        s.iter,
        s.interlock,
        s.version,
        s.neighbor[0],
        s.neighbor[1],
        s.neighbor[2],
        s.neighbor[3],
        s.neighbor[4],
        s.neighbor[5],
        static_cast<unsigned long long>(s.firstChange),
        static_cast<unsigned long long>(s.lastChange),
        static_cast<double>(s.ready)
    );
    return buf;
}
// Call chain of the running hook as game function offsets, to tell what runs a commit.
std::string probeCallChain() {
    void*        frames[14]{};
    USHORT const count = CaptureStackBackTrace(1, 14, frames, nullptr);
    std::string  out;
    int          shown = 0;
    for (USHORT i = 0; i < count && shown < 8; ++i) {
        auto const   pc   = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64      base = 0;
        auto const*  fn   = RtlLookupFunctionEntry(pc, &base, nullptr);
        std::string  name = "?";
        wchar_t      path[MAX_PATH]{};
        if (base && GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH)) {
            std::wstring_view full(path);
            auto const        cut = full.find_last_of(L"\\/");
            if (cut != std::wstring_view::npos) full.remove_prefix(cut + 1);
            name.clear();
            for (wchar_t c : full) name.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        if (name == "OptiPiston.dll") continue;
        char buf[96];
        std::snprintf(
            buf,
            sizeof buf,
            "%s%s+%llx+%llx",
            out.empty() ? "" : " < ",
            name.c_str(),
            static_cast<unsigned long long>(fn ? fn->BeginAddress : pc - base),
            static_cast<unsigned long long>(fn ? pc - base - fn->BeginAddress : 0)
        );
        out += buf;
        ++shown;
    }
    return out.empty() ? "-" : out;
}
thread_local std::string tProbeCommitChain;
thread_local void const* tProbeCommitBuilder = nullptr;
struct ProbeChainTally {
    uint64_t fast{};
    uint64_t slow{};
    uint64_t maxLatency{};
};
std::unordered_map<std::string, ProbeChainTally> gProbeChains;
std::unordered_set<std::string>                  gProbeAreaChains;
uint64_t                                         gProbeChainLines = 0;
// What the last committed build of a chunk holds, by chunk origin.
struct ProbeOrigin {
    uint64_t                                     starts{};
    uint64_t                                     commits{};
    uint64_t                                     lastStart{};
    uint64_t                                     lastCommit{};
    uint64_t                                     startFrame{};    // frame of the latest start, for build latency
    uint64_t                                     depthAtStart{};  // builds in flight when it started
    uint64_t                                     rebuildEnd{};    // frame the worker's rebuild() returned
    uint64_t                                     rebuildWallUs{}; // wall time inside rebuild()
    uint64_t                                     rebuildCpuUs{};  // thread CPU time inside rebuild()
    uint64_t                                     asks{};          // our re-mesh asks touching it since the start
    uint64_t                                     lastAsk{};       // frame of the latest such ask
    ProbeGeom                                    atStart;
    ProbeGeom                                    atReturn;
    void const*                                  builder{};    // RenderChunkBuilder that ran the latest build
    unsigned long                                rebuildTid{}; // thread that ran its rebuild()
    std::unordered_set<::BlockPos, BlockPosHash> live; // tracked cells the last committed build tessellated
};
std::unordered_map<::BlockPos, ProbeOrigin, BlockPosHash> gProbeOrigins;

::BlockPos probeOriginOf(::BlockPos const& pos) {
    return ::BlockPos{(pos.x >> 4) << 4, (pos.y >> 4) << 4, (pos.z >> 4) << 4};
}
std::atomic_bool gProbeAnyCell{false};
// A landed cell whose world block read air, so the render path dropped it. Closed when a newer record draws the cell.
struct ProbeClear {
    uint64_t action{};
    uint64_t frame{};
    int64_t  tick{};
    bool     landed{};
    uint64_t createdFrame{}; // 0 until a newer record took the cell over
    uint64_t newAction{};
};
std::unordered_map<::BlockPos, ProbeClear, BlockPosHash> gProbeCleared;
// Frames from the clear to the next proxy dispatch: 0-1, 2-3, 4-6, 7+, and never within the watch window.
std::atomic<uint64_t> gProbeClearGap[5]{};
std::atomic<uint64_t> gProbeClearStarted{0};
std::atomic<uint64_t> gProbeClearClosed{0};
// Empty reads of a landed cell that the world filled again (0) or kept empty past a tick (1).
std::atomic<uint64_t> gProbeGone[2]{};
// Pass of the proxy being dispatched (0 opaque, 1 alpha).
thread_local int      tProbePass = 0;
thread_local ProbeWhy tProbeWhy;
// Light answers the game's renderer got while one of our proxies was being dispatched on this thread.
struct ProbeProxyLight {
    bool       active{};
    ::BlockPos cell;
    int        queries{};
    int        dark{};
    int        selfDark{};
};
thread_local ProbeProxyLight tProbeProxyLight;

// Every line starts with the frame counter and the level tick.
template <class... Args>
void probeLineLocked(char const* format, Args... args) {
    char      text[1024];
    int const head = std::snprintf(
        text,
        sizeof(text),
        "f=%llu lt=%lld ",
        static_cast<unsigned long long>(gFrame.load(std::memory_order_relaxed)),
        static_cast<long long>(gFrameLt.load(std::memory_order_relaxed))
    );
    if (head <= 0 || gProbeBuffer.size() > (4u << 20)) return;
    int const body = std::snprintf(text + head, sizeof(text) - static_cast<std::size_t>(head), format, args...);
    if (body <= 0) return;
    auto const length = std::min<std::size_t>(static_cast<std::size_t>(head + body), sizeof(text) - 1);
    gProbeBuffer.append(text, length);
}

// True while a line class still has budget; its first refusal writes one "cap reached" line. Caller holds gProbeMutex.
bool probeBudget(std::atomic<uint64_t>& used, uint64_t cap, char const* what) {
    auto const n = used.fetch_add(1, std::memory_order_relaxed);
    if (n < cap) return true;
    if (n == cap) probeLineLocked("cap reached at %s\n", what);
    return false;
}

// Closes the open double-draw episode of a cell and writes one line for it. Caller holds gProbeMutex.
void probeCloseDupLocked(::BlockPos const& pos, ProbeCell& cell) {
    if (cell.dupFrames == 0) return;
    auto const ms = std::chrono::duration<double, std::milli>(cell.dupLastWall - cell.dupStartWall).count();
    gProbe.dupEpisodes.fetch_add(1, std::memory_order_relaxed);
    gProbe.dupMs.fetch_add(static_cast<uint64_t>(ms), std::memory_order_relaxed);
    gProbe.dupVisFrames.fetch_add(static_cast<uint64_t>(cell.dupVis), std::memory_order_relaxed);
    if (static_cast<uint64_t>(cell.dupFrames) > gProbe.dupMaxFrames.load(std::memory_order_relaxed))
        gProbe.dupMaxFrames.store(static_cast<uint64_t>(cell.dupFrames), std::memory_order_relaxed);
    static std::atomic<uint64_t> used{0};
    if (probeBudget(used, 300, "dupEnd")) {
        probeLineLocked(
            "dupEnd cell=%d,%d,%d who=%s state=%s frames=%d vis=%d spanMs=%.1f startOff=%.3f endOff=%.3f world=%s "
            "created=%llu built=%llu kind=%d commit=%llu regress=%llu start=%llu\n",
            pos.x,
            pos.y,
            pos.z,
            cell.dupWho ? cell.dupWho : "?",
            cell.dupState ? cell.dupState : "?",
            cell.dupFrames,
            cell.dupVis,
            ms,
            static_cast<double>(cell.dupStartOff),
            static_cast<double>(cell.dupEndOff),
            cell.dupWorld.c_str(),
            static_cast<unsigned long long>(cell.createdFrame),
            static_cast<unsigned long long>(cell.builtFrame),
            cell.builtKind,
            static_cast<unsigned long long>(cell.meshCommit),
            static_cast<unsigned long long>(cell.regressFrame),
            static_cast<unsigned long long>(cell.dupStart)
        );
    }
    cell.dupFrames = 0;
    cell.dupVis    = 0;
    cell.dupWho    = nullptr;
    cell.dupState  = nullptr;
    cell.dupWorld.clear();
}

void probeFlushLocked() {
    if (gProbeFile && !gProbeBuffer.empty()) {
        std::fwrite(gProbeBuffer.data(), 1, gProbeBuffer.size(), gProbeFile);
        std::fflush(gProbeFile);
    }
    gProbeBuffer.clear();
}

void probeOpen() {
    std::lock_guard const guard(gProbeMutex);
    if (gProbeFile) std::fclose(gProbeFile);
    gProbeFile = nullptr;
    gProbeBuffer.clear();
    gProbeTally.clear();
    gProbeCells.clear();
    gProbeRemesh.clear();
    gProbeArm.clear();
    gProbeArmCells.clear();
    gProbeDrawSeen = false;
    gProbeClkValid = false;
    gProbeGhosts.clear();
    gProbeBuilt.clear();
    gProbeOrigins.clear();
    gProbeCleared.clear();
    gProbeAnyCell.store(false, std::memory_order_relaxed);
    bool perfOnly = false;
    if (auto* const mod = ll::mod::NativeMod::current().get()) {
        auto const path = mod->getModDir() / "probe.log";
        if (fopen_s(&gProbeFile, path.string().c_str(), "wb") != 0) gProbeFile = nullptr;
        std::FILE* flag = nullptr;
        if (fopen_s(&flag, (mod->getModDir() / "perf-only").string().c_str(), "rb") == 0 && flag) {
            std::fclose(flag);
            perfOnly = true;
        }
    }
    gProbePerfOnly.store(perfOnly, std::memory_order_relaxed);
    // What one timer read costs, so per-call numbers can be corrected by it.
    auto const readStart = perfNow();
    for (int i = 0; i < 2000; ++i) (void)perfNow();
    auto const qpcNs = static_cast<double>(perfNow() - readStart) * 1.0e9
                     / (static_cast<double>(std::max<uint64_t>(perfFrequency(), 1)) * 2000.0);
    probeLineLocked("mode perfOnly=%d qpcNs=%.1f\n", perfOnly ? 1 : 0, qpcNs);
    gProbeLastSum = std::chrono::steady_clock::now();
}

void probeClose() {
    std::lock_guard const guard(gProbeMutex);
    probeFlushLocked();
    if (gProbeFile) std::fclose(gProbeFile);
    gProbeFile = nullptr;
}

// Caller holds gProbeMutex. drawn=false means nothing drew the cell within the watch window.
void probeClearCloseLocked(::BlockPos const& cell, ProbeClear const& clear, bool drawn) {
    auto const gap  = static_cast<long long>(gFrame.load(std::memory_order_relaxed) - clear.frame);
    int const  slot = !drawn ? 4 : gap <= 1 ? 0 : gap <= 3 ? 1 : gap <= 6 ? 2 : 3;
    gProbeClearGap[slot].fetch_add(1, std::memory_order_relaxed);
    if (gProbeClearClosed.fetch_add(1, std::memory_order_relaxed) >= 800) return;
    probeLineLocked(
        "clearGap cell=%d,%d,%d oldAct=%llu newAct=%llu landed=%d createdAfter=%lld drawnAfter=%lld ticks=%lld "
        "end=%s\n",
        cell.x,
        cell.y,
        cell.z,
        static_cast<unsigned long long>(clear.action),
        static_cast<unsigned long long>(clear.newAction),
        clear.landed ? 1 : 0,
        clear.newAction != 0 ? static_cast<long long>(clear.createdFrame - clear.frame) : -1LL,
        drawn ? gap : -1LL,
        static_cast<long long>(gFrameLt.load(std::memory_order_relaxed) - clear.tick),
        drawn ? "drawn" : "none"
    );
}

// drawProxies handed the proxy to the dispatcher. Says nothing about whether anything was drawn.
void probeProxyDispatched(::BlockPos const& cell, int pass) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    auto&                 tally = gProbeTally[cell];
    ++tally.dispatched;
    tally.pass = pass;
    if (auto const it = gProbeCleared.find(cell); it != gProbeCleared.end()) {
        probeClearCloseLocked(cell, it->second, true);
        gProbeCleared.erase(it);
    }
}

// The MovingBlock renderer path was entered for the proxy.
void probeProxyRendered(::BlockPos const& cell) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    ++gProbeTally[cell].rendered;
}

// getDrawPos was reached for the proxy.
void probeProxyDrawPos(::BlockPos const& cell, float offset, ::Vec3 const* vec) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    auto&                 tally = gProbeTally[cell];
    ++tally.drawPos;
    tally.minOffset = std::min(tally.minOffset, offset);
    tally.maxOffset = std::max(tally.maxOffset, offset);
    if (vec) {
        tally.vx = vec->x;
        tally.vy = vec->y;
        tally.vz = vec->z;
        // visualDrawOffset stored the time it just used on this thread.
        auto const time = gLastVisualTime.load(std::memory_order_relaxed);
        // Every positioned draw of a frame should use the same visual time.
        if (gProbeDrawSeen && std::fabs(time - gProbeDrawTime) > 0.02)
            gProbe.clkSpread.fetch_add(1, std::memory_order_relaxed);
        if (!gProbeDrawSeen) {
            gProbeDrawSeen = true;
            gProbeDrawWall = std::chrono::steady_clock::now();
            gProbeDrawTime = time;
        }
        tally.time  = time;
        tally.timed = true;
    }
}

// The renderer is done with the proxy of `cell`: folds the light answers it got meanwhile into the frame's tally.
void probeProxyLightEnd(::BlockPos const& cell) {
    auto const seen         = tProbeProxyLight;
    tProbeProxyLight.active = false;
    if (seen.queries == 0 || gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    auto&                 tally = gProbeTally[cell];
    tally.lightQ         += seen.queries;
    tally.lightDark      += seen.dark;
    tally.lightSelfDark  += seen.selfDark;
    gProbe.lightQueries.fetch_add(static_cast<uint64_t>(seen.queries), std::memory_order_relaxed);
}

// The game's instance of a managed cell was skipped.
void probeVanillaHidden(::BlockPos const& cell) {
    gProbe.vanillaHidden.fetch_add(1, std::memory_order_relaxed);
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    if (auto const it = gProbeCells.find(cell); it != gProbeCells.end() && !it->second.firstRender)
        ++it->second.hiddenBeforeRender;
}

// The game's instance of a cell was drawn; tProbeWhy says why vanillaHidden let it.
void probeVanillaDraw(::BlockPos const& cell) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    auto&                 tally = gProbeTally[cell];
    ++tally.vanilla;
    tally.why       = tProbeWhy.reason;
    tally.whyRec    = tProbeWhy.recAction;
    tally.whyPiston = tProbeWhy.pistonAction;
    tally.whyState  = tProbeWhy.state;
}

// The game's instance is about to be drawn. Suspicious when it is detached, or its cell shows neither a
// moving_block nor the block it wraps (air after the real block was broken). Takes gProbeMutex twice, never
// while reading the render view.
void probeGhost(
    ::MovingBlockActor const&          moving,
    ::Block const&                     cellBlock,
    bool                               detached,
    bool                               arrived,
    bool                               hasOwner,
    ::ActorTerrainInterlockData const& interlock
) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    auto const  wrapped  = wrappedName(moving);
    auto const& cellName = cellBlock.getTypeName();
    bool const  matches  = cellName == "minecraft:moving_block" || cellName == wrapped;
    if (!detached && matches) return;

    gProbe.ghostDraws.fetch_add(1, std::memory_order_relaxed);
    auto const pos       = moving.mPosition.get();
    auto const realFrame = gRealFrame.load(std::memory_order_relaxed);
    uint64_t   draws     = 0;
    uint64_t   age       = 0;
    {
        std::lock_guard const guard(gProbeMutex);
        auto const [it, inserted] = gProbeGhosts.try_emplace(reinterpret_cast<std::uintptr_t>(&moving));
        auto& ghost               = it->second;
        if (inserted) {
            ghost.firstFrame = realFrame;
            gProbe.ghostInstances.fetch_add(1, std::memory_order_relaxed);
        }
        ++ghost.draws;
        ghost.lastSeenFrame = realFrame;
        draws               = ghost.draws;
        age                 = realFrame - ghost.firstFrame;
        if (draws == 1 || draws == 30 || draws == 300 || draws == 3000) ghost.lastLogFrame = realFrame;
        else draws = 0;
        if (inserted && gProbeGhosts.size() > 256)
            std::erase_if(gProbeGhosts, [realFrame](auto const& entry) {
                return realFrame > entry.second.lastSeenFrame && realFrame - entry.second.lastSeenFrame > 600;
            });
    }
    if (draws == 0) return;

    bool managed = false;
    {
        ViewPin pin;
        managed = pin.get().cells.contains(pos);
    }
    std::string const     wrappedText(wrapped);
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "ghost cell=%d,%d,%d wrapped=%s cellBlock=%s detached=%d preserved=%d life=%d arrived=%d owner=%d "
        "interlock=%d managed=%d draws=%llu ageFrames=%llu\n",
        pos.x,
        pos.y,
        pos.z,
        wrappedText.c_str(),
        cellName.c_str(),
        detached ? 1 : 0,
        moving.mPreserved ? 1 : 0,
        static_cast<int>(moving.mPreservedLifespan),
        arrived ? 1 : 0,
        hasOwner ? 1 : 0,
        static_cast<int>(interlock.mRenderVisibilityState),
        managed ? 1 : 0,
        static_cast<unsigned long long>(draws),
        static_cast<unsigned long long>(age)
    );
}

// drawArm was entered for a PistonArm with smooth rendering on.
void probeArmCall(::BlockPos const& at, bool isCargo) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    auto&                 arm = gProbeArm[at];
    ++arm.calls;
    if (isCargo) ++arm.cargo;
}

// The head owner election dropped this instance.
void probeArmSkipped(::BlockPos const& at) {
    gProbe.armSkipped.fetch_add(1, std::memory_order_relaxed);
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    ++gProbeArm[at].skipped;
}

// draw() is about to run with the arm at its final position.
void probeArmDrawn(::BlockPos const& at, ::Vec3 const& position, bool nested) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    auto&                 arm = gProbeArm[at];
    if (arm.drawn++ == 0) {
        arm.first = position;
    } else if (std::abs(position.x - arm.first.x) > 1.0e-3f || std::abs(position.y - arm.first.y) > 1.0e-3f
               || std::abs(position.z - arm.first.z) > 1.0e-3f) {
        arm.spread = true;
    }
    if (nested) ++arm.nested;
}

// Caller holds gAnimMutex.
void probeCellCreated(::BlockPos const& cell, uint64_t action) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    ProbeCell             fresh;
    fresh.action       = action;
    fresh.createdFrame = gFrame.load(std::memory_order_relaxed);
    if (auto const org = gProbeOrigins.find(probeOriginOf(cell)); org != gProbeOrigins.end()) {
        fresh.originStarts  = org->second.starts;
        fresh.originCommits = org->second.commits;
    }
    gProbeCells[cell] = fresh;
    gProbeAnyCell.store(true, std::memory_order_relaxed);
    if (auto const it = gProbeCleared.find(cell); it != gProbeCleared.end() && it->second.newAction == 0) {
        it->second.createdFrame = fresh.createdFrame;
        it->second.newAction    = action;
    }
}

// Caller holds gAnimMutex. The record is about to be cleared because its world block read air.
void probeCellCleared(::BlockPos const& cell, PushCell const& rec) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gProbeMutex);
    if (gProbeCleared.size() > 4096) gProbeCleared.clear();
    gProbeCleared[cell] = ProbeClear{
        rec.action,
        gFrame.load(std::memory_order_relaxed),
        static_cast<int64_t>(gFrameLt.load(std::memory_order_relaxed)),
        rec.timeline.landed()
    };
    gProbeClearStarted.fetch_add(1, std::memory_order_relaxed);
}

// Caller holds gAnimMutex. A landed cell read empty; the world then filled it again or kept it empty past a tick.
void probeGoneEnd(::BlockPos const& cell, uint64_t action, int64_t ticks, uint64_t frames, bool cleared) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    if (gProbeGone[cleared ? 1 : 0].fetch_add(1, std::memory_order_relaxed) >= 400) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "goneEnd cell=%d,%d,%d act=%llu result=%s ticks=%lld frames=%llu\n",
        cell.x,
        cell.y,
        cell.z,
        static_cast<unsigned long long>(action),
        cleared ? "cleared" : "returned",
        static_cast<long long>(ticks),
        static_cast<unsigned long long>(frames)
    );
}

char const* probeStateName(PushState state);
float        armValue(PistonVisual const& visual, double time);

// Called under gAnimMutex (then gProbeMutex). Only the first 200 replacements are logged.
void probeCellReplaced(::BlockPos const& cell, uint64_t oldAction, PushState oldState, uint64_t newAction) {
    if (gProbe.replaced.fetch_add(1, std::memory_order_relaxed) >= 200) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "replace cell=%d,%d,%d old=%llu new=%llu oldState=%s\n",
        cell.x,
        cell.y,
        cell.z,
        static_cast<unsigned long long>(oldAction),
        static_cast<unsigned long long>(newAction),
        probeStateName(oldState)
    );
}

// Called under gAnimMutex (then gProbeMutex). A cell took over the tail of a cell its own action had replaced.
void probeTailCarried(::BlockPos const& cell, ::BlockPos const& source, uint64_t oldAction, uint64_t newAction) {
    static std::atomic<uint64_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 3000) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "tailCarry cell=%d,%d,%d source=%d,%d,%d old=%llu new=%llu\n",
        cell.x,
        cell.y,
        cell.z,
        source.x,
        source.y,
        source.z,
        static_cast<unsigned long long>(oldAction),
        static_cast<unsigned long long>(newAction)
    );
}

// Called under gAnimMutex (then gProbeMutex). remain > 0: the mesh goes live while the proxy still has that many
// visual ticks to move, so the block shows in its final cell early. Only the first 300 are logged.
void probeMeshCommit(::BlockPos const& cell, PushCell const& rec, double step, double latencyFrames) {
    auto const                   speed   = core::worldSpeedSettings().applied.load(std::memory_order_relaxed);
    auto const&                  segment = rec.visual.segment;
    auto const                   end     = static_cast<double>(segment.startTick) + segment.length;
    auto const                   remain  = end - gLastVisualTime.load(std::memory_order_relaxed) - step;
    static std::atomic<uint64_t> earlyLines{0};
    static std::atomic<uint64_t> restSeen{0};
    if (remain > 0.05) {
        if (earlyLines.fetch_add(1, std::memory_order_relaxed) >= 1500) return;
    } else if (restSeen.fetch_add(1, std::memory_order_relaxed) % 8 != 0
               || gProbe.meshCommitLines.fetch_add(1, std::memory_order_relaxed) >= 400) {
        return;
    }
    std::lock_guard const guard(gProbeMutex);
    ProbeRemesh           request{-1, 0.0, 0};
    if (auto const it = gProbeRemesh.find(cell); it != gProbeRemesh.end()) request = it->second;
    probeLineLocked(
        "meshCommit cell=%d,%d,%d remain=%.3f step=%.4f latencyFrames=%.2f estimate=%.2f speed=%.2f src=%d req=%.3f "
        "reqTick=%lld\n",
        cell.x,
        cell.y,
        cell.z,
        remain,
        step,
        latencyFrames,
        gMeshLatency.load(std::memory_order_relaxed) / std::max(gFrameSeconds.load(std::memory_order_relaxed), 1.0e-6),
        static_cast<double>(speed),
        request.src,
        request.remain,
        static_cast<long long>(request.tick)
    );
}

// Called under gAnimMutex (then gProbeMutex) once a timeline accepted a re-mesh request.
void probeRemeshRequest(::BlockPos const& cell, PushCell const& rec, int src) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    auto const& segment = rec.visual.segment;
    auto const  remain =
        static_cast<double>(segment.startTick) + segment.length - gLastVisualTime.load(std::memory_order_relaxed);
    std::lock_guard const guard(gProbeMutex);
    gProbeRemesh[cell] = ProbeRemesh{src, remain, gLastTick.load(std::memory_order_relaxed)};
}

void probeRenderBlockEntities(bool renderAlphaLayer) {
    int const pass = renderAlphaLayer ? 1 : 0;
    gProbe.rbeCalls[pass].fetch_add(1, std::memory_order_relaxed);
    gProbe.rbeFrame[pass].fetch_add(1, std::memory_order_relaxed);
}

// A build tessellated the real block of a managed cell (not the moving_block stand-in, not suppressed).
void probeMeshTessellated(::BlockPos const& pos, ::Block const& block, bool produced) {
    if (tRenderDepth > 0 || !tMeshGeometry || !gProbeAnyCell.load(std::memory_order_relaxed)) return;
    core::MeshCell kind = core::MeshCell::None;
    {
        MeshViewPin pin;
        kind = pin.get().at(cellOf(pos));
    }
    std::lock_guard const guard(gProbeMutex);
    auto const            tracked = gProbeCells.find(pos);
    if (tracked == gProbeCells.end()) return;
    if (block.isAir() || block.getTypeName() == "minecraft:moving_block") return;
    tracked->second.lastTessFrame    = gFrame.load(std::memory_order_relaxed);
    tracked->second.lastTessProduced = produced;
    if (produced)
        gProbeBuilt[tMeshGeometry].push_back(
            ProbeBuilt{pos, gFrame.load(std::memory_order_relaxed), static_cast<int>(kind)}
        );
    else gProbe.meshEmpty.fetch_add(1, std::memory_order_relaxed);
    // The real block came out of a build while its proxy was still on the way in: visible next to the proxy.
    if (produced && kind == core::MeshCell::Approaching
        && gProbe.leakMesh.fetch_add(1, std::memory_order_relaxed) < 300)
        probeLineLocked("leakMesh cell=%d,%d,%d block=%s\n", pos.x, pos.y, pos.z, block.getTypeName().c_str());
}

// A build asked for an awaiting cell but was holding the stand-in, so it was not credited.
void probeStaleRelease(::BlockPos const& pos, ::Block const& block) {
    static std::atomic<uint64_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 500) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked("staleRelease cell=%d,%d,%d block=%s\n", pos.x, pos.y, pos.z, block.getTypeName().c_str());
}

// A committed build holds the block of a cell whose newer action is already animating there.
void probeStaleCommit(::BlockPos const& pos, uint64_t action) {
    static std::atomic<uint64_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 300) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "staleCommit cell=%d,%d,%d act=%llu\n",
        pos.x,
        pos.y,
        pos.z,
        static_cast<unsigned long long>(action)
    );
}

std::atomic<uint64_t> gLandPolls{0};
std::atomic<uint64_t> gOverrunEnds{0};

// Caller holds gAnimMutex. The cell held a real block when its visual ran out, but no packet or placed check saw it.
void probeLandPoll(::BlockPos const& pos, std::string const& block, std::string const& wrapped) {
    if (gLandPolls.fetch_add(1, std::memory_order_relaxed) >= 300) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "landPoll cell=%d,%d,%d block=%s wrapped=%s\n",
        pos.x,
        pos.y,
        pos.z,
        block.c_str(),
        wrapped.c_str()
    );
}

// Caller holds gAnimMutex. The overrun ran out and the block never landed in this cell.
void probeOverrunEnd(::BlockPos const& pos, PushCell const& rec) {
    if (gOverrunEnds.fetch_add(1, std::memory_order_relaxed) >= 300) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "overrunEnd cell=%d,%d,%d act=%llu wrapped=%s\n",
        pos.x,
        pos.y,
        pos.z,
        static_cast<unsigned long long>(rec.action),
        rec.wrapped ? rec.wrapped->c_str() : "?"
    );
}

// Light at the cells an arm could be lit from, by class: bit0 shifted, bit1 cargo, bit2 nested, bit3 piston moving.
struct ArmLightStats {
    std::atomic<uint64_t> sampled{0};
    std::atomic<uint64_t> spread{0};   // the three cells differ by 0.3 or more
    std::atomic<uint64_t> selfDark{0}; // the piston's own cell is below 0.1
    std::atomic<uint64_t> loggedSpread{0};
    std::atomic<uint64_t> loggedBase{0};
};
ArmLightStats gArmLight[16];

void probeArmLightSample(
    ::BlockActor&                entity,
    ::BlockSource&               source,
    ::Vec3 const&                position,
    std::optional<::Vec3> const& offset,
    bool                         shifted,
    bool                         cargo,
    bool                         nested
) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    auto&                        piston = static_cast<::PistonBlockActor&>(entity);
    auto const                   state  = static_cast<int>(piston.mState);
    bool const                   moving = state == 1 || state == 3;
    static std::atomic<uint64_t> calls{0};
    if (!shifted && !cargo && !moving && (calls.fetch_add(1, std::memory_order_relaxed) & 31u) != 0) return;
    auto const self  = entity.mPosition.get();
    auto const head  = self + facingOf(piston, source);
    ::BlockPos shift = self;
    if (offset)
        shift = ::BlockPos{
            self.x + static_cast<int>(std::lround(offset->x)),
            self.y + static_cast<int>(std::lround(offset->y)),
            self.z + static_cast<int>(std::lround(offset->z))
        };
    float const bSelf  = source.getBrightness(self);
    float const bHead  = source.getBrightness(head);
    float const bShift = source.getBrightness(shift);
    float const lo     = std::min({bSelf, bHead, bShift});
    float const hi     = std::max({bSelf, bHead, bShift});
    int const   cls    = (shifted ? 1 : 0) | (cargo ? 2 : 0) | (nested ? 4 : 0) | (moving ? 8 : 0);
    auto&       stats  = gArmLight[cls];
    stats.sampled.fetch_add(1, std::memory_order_relaxed);
    if (moving) {
        auto const&                  headBlock = source.getBlock(head);
        auto const&                  headName  = headBlock.getTypeName();
        bool const                   carried   = !headBlock.isAir() && headName != "minecraft:piston_arm_collision"
                               && headName != "minecraft:moving_block";
        static std::atomic<uint64_t> carriedLogged{0};
        static std::atomic<uint64_t> emptyLogged{0};
        auto const                   limit = carried ? 150u : 20u;
        if ((carried ? carriedLogged : emptyLogged).fetch_add(1, std::memory_order_relaxed) < limit) {
            std::lock_guard const guard(gProbeMutex);
            probeLineLocked(
                "armHead%s cls=%d st=%d prog=%.2f last=%.2f dur=%.1f piston=%d,%d,%d head=%s self=%.2f hd=%.2f "
                "off=%.2f,%.2f,%.2f\n",
                carried ? "Block" : "Free",
                (shifted ? 1 : 0) | (cargo ? 2 : 0) | (nested ? 4 : 0) | 8,
                state,
                piston.mProgress,
                piston.mLastProgress,
                core::pistonSettings().durationTicks.load(),
                self.x,
                self.y,
                self.z,
                headName.c_str(),
                bSelf,
                bHead,
                offset ? offset->x : 0.0f,
                offset ? offset->y : 0.0f,
                offset ? offset->z : 0.0f
            );
        }
    }
    bool const wide = hi - lo >= 0.3f;
    if (wide) stats.spread.fetch_add(1, std::memory_order_relaxed);
    if (bSelf < 0.1f) stats.selfDark.fetch_add(1, std::memory_order_relaxed);
    bool const logWide = wide && stats.loggedSpread.fetch_add(1, std::memory_order_relaxed) < 60;
    bool const logBase = !wide && stats.loggedBase.fetch_add(1, std::memory_order_relaxed) < 15;
    if (!logWide && !logBase) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "armLight%s cls=%d piston=%d,%d,%d st=%d prog=%.2f last=%.2f self=%.2f head=%.2f shift=%.2f "
        "blk=%s/%s/%s pos=%.2f,%.2f,%.2f off=%.2f,%.2f,%.2f\n",
        wide ? "Wide" : "Base",
        cls,
        self.x,
        self.y,
        self.z,
        state,
        piston.mProgress,
        piston.mLastProgress,
        bSelf,
        bHead,
        bShift,
        source.getBlock(self).getTypeName().c_str(),
        source.getBlock(head).getTypeName().c_str(),
        source.getBlock(shift).getTypeName().c_str(),
        position.x,
        position.y,
        position.z,
        offset ? offset->x : 0.0f,
        offset ? offset->y : 0.0f,
        offset ? offset->z : 0.0f
    );
}

// What the game itself asks the light for while an arm is drawn: every BlockSource light query made between the
// start and the end of an arm's draw(), by arm class, query kind and cell relative to the piston.
struct ArmLightCtx {
    bool       active{};
    ::BlockPos piston{};
    int        cls{};
    int        state{};
};
thread_local ArmLightCtx tArmLightCtx;

constexpr int kLightSpan  = 5; // relative cells -2..2 on each axis
constexpr int kLightCells = kLightSpan * kLightSpan * kLightSpan;
struct LightBucket {
    std::atomic<uint64_t> n{0};
    std::atomic<uint64_t> dark{0};
};
LightBucket           gLightBuckets[16][3][kLightCells + 1]; // [cls][kind][rel or far]
std::atomic<uint64_t> gLightDarkLogged{0};
std::atomic<uint64_t> gLightMoveNear{0};
std::atomic<uint64_t> gLightMoveFar{0};
std::atomic<uint64_t> gLightFixWindows{0};
std::atomic<uint64_t> gLightFixHeadQueries{0};
std::atomic<uint64_t> gLightFixRewrites{0};
std::atomic<uint64_t> gLightFixRawDark{0};

// kind 0 getBrightness (value 0..1), 1 getBrightnessPair, 2 getLightColor (value = brighter channel / 15).
void probeLightQuery(
    ::BlockSource const& source,
    int                  kind,
    ::BlockPos const&    pos,
    bool                 dark,
    float                value,
    int                  sky      = -1,
    int                  block    = -1,
    int                  minLight = -1
) {
    auto const& ctx     = tArmLightCtx;
    int const   dx      = pos.x - ctx.piston.x + 2;
    int const   dy      = pos.y - ctx.piston.y + 2;
    int const   dz      = pos.z - ctx.piston.z + 2;
    bool const  outside = dx < 0 || dy < 0 || dz < 0 || dx >= kLightSpan || dy >= kLightSpan || dz >= kLightSpan;
    auto&       b = gLightBuckets[ctx.cls][kind][outside ? kLightCells : (dx * kLightSpan + dy) * kLightSpan + dz];
    b.n.fetch_add(1, std::memory_order_relaxed);
    if ((ctx.cls & 8) != 0 && (outside ? gLightMoveFar : gLightMoveNear).fetch_add(1, std::memory_order_relaxed) < (outside ? 60u : 200u)) {
        std::lock_guard const guard(gProbeMutex);
        probeLineLocked(
            "armLightMove kind=%d cls=%d st=%d piston=%d,%d,%d at=%d,%d,%d rel=%d,%d,%d val=%.2f blk=%s sky=%d bl=%d min=%d\n",
            kind,
            ctx.cls,
            ctx.state,
            ctx.piston.x,
            ctx.piston.y,
            ctx.piston.z,
            pos.x,
            pos.y,
            pos.z,
            pos.x - ctx.piston.x,
            pos.y - ctx.piston.y,
            pos.z - ctx.piston.z,
            value,
            const_cast<::BlockSource&>(source).getBlock(pos).getTypeName().c_str(),
            sky,
            block,
            minLight
        );
    }
    if (!dark) return;
    b.dark.fetch_add(1, std::memory_order_relaxed);
    if (gLightDarkLogged.fetch_add(1, std::memory_order_relaxed) >= 150) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "armLightDark kind=%d cls=%d st=%d piston=%d,%d,%d rel=%d,%d,%d val=%.2f blk=%s\n",
        kind,
        ctx.cls,
        ctx.state,
        ctx.piston.x,
        ctx.piston.y,
        ctx.piston.z,
        pos.x - ctx.piston.x,
        pos.y - ctx.piston.y,
        pos.z - ctx.piston.z,
        value,
        const_cast<::BlockSource&>(source).getBlock(pos).getTypeName().c_str()
    );
}

// Samples the three candidate cells (first/spread logging), then opens the window in which the hooks count the
// game's own light queries. probeArmLightEnd() closes it after draw().
void probeArmLight(
    ::BlockActor&                entity,
    ::BlockSource&               source,
    ::Vec3 const&                position,
    std::optional<::Vec3> const& offset,
    bool                         shifted,
    bool                         cargo,
    bool                         nested
) {
    tArmLightCtx.active = false;
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    probeArmLightSample(entity, source, position, offset, shifted, cargo, nested);
    auto&      piston = static_cast<::PistonBlockActor&>(entity);
    auto const state  = static_cast<int>(piston.mState);
    bool const moving = state == 1 || state == 3;
    tArmLightCtx      = {
        true,
        entity.mPosition.get(),
        (shifted ? 1 : 0) | (cargo ? 2 : 0) | (nested ? 4 : 0) | (moving ? 8 : 0),
        state
    };
}

void probeArmLightEnd() { tArmLightCtx.active = false; }

// [0..7] cells the poll never found empty, [8..15] cells it found empty at least once.
std::atomic<uint64_t> gLandDelayHist[16]{};

// Caller holds gAnimMutex. A pushed piston took its still-playing arm along.
void probeArmMoved(::BlockPos const& from, ::BlockPos const& to, uint64_t action) {
    static std::atomic<uint64_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) >= 300) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked(
        "armMoved from=%d,%d,%d to=%d,%d,%d act=%llu\n",
        from.x,
        from.y,
        from.z,
        to.x,
        to.y,
        to.z,
        static_cast<unsigned long long>(action)
    );
}

// The game's instance of a managed cell names another piston than the one the cell was built from; it draws beside
// our proxy. Two rows per action, 400 in all.
void probePosMismatch(::BlockPos const& cell, ::MovingBlockActor const& moving, StateRef const& state) {
    static std::atomic<uint64_t>             lines{0};
    static std::unordered_map<uint64_t, int> perAction; // guarded by gProbeMutex
    auto const                               found = state.cells.find(cell);
    if (found == state.cells.end()) return;
    auto const& rec = found->second;
    if (lines.load(std::memory_order_relaxed) >= 400) return;
    std::lock_guard const guard(gProbeMutex);
    if (perAction[rec.action]++ >= 2) return;
    lines.fetch_add(1, std::memory_order_relaxed);
    auto const& other    = moving.mPistonBlockPos.get();
    auto const  actionOf = [&](::BlockPos const& at) -> unsigned long long {
        auto const it = state.pistons.find(at);
        return it == state.pistons.end() ? 0 : it->second.action;
    };
    auto const        recWrapped = rec.wrapped ? std::string_view(*rec.wrapped) : std::string_view();
    std::string const gameWrapped(wrappedName(moving));
    probeLineLocked(
        "posMismatch cell=%d,%d,%d game=%d,%d,%d rec=%d,%d,%d recAct=%llu gameAct=%llu recPistonAct=%llu "
        "gameIsCell=%d recIsCell=%d state=%s sameBlock=%d expanding=%d carry=%d unknown=%d gw=%s rw=%s\n",
        cell.x,
        cell.y,
        cell.z,
        other.x,
        other.y,
        other.z,
        rec.pistonPos.x,
        rec.pistonPos.y,
        rec.pistonPos.z,
        static_cast<unsigned long long>(rec.action),
        actionOf(other),
        actionOf(rec.pistonPos),
        state.cells.count(other) ? 1 : 0,
        state.cells.count(rec.pistonPos) ? 1 : 0,
        probeStateName(rec.timeline.state()),
        recWrapped == gameWrapped ? 1 : 0,
        static_cast<int>(moving.mPistonBlockExpanding),
        rec.carry.has_value() ? 1 : 0,
        state.pistons.find(other) == state.pistons.end() ? 1 : 0,
        gameWrapped.c_str(),
        std::string(recWrapped).c_str()
    );
}

// Caller holds gAnimMutex. Ticks between the end of the visual and the block landing; counted when the cell is done.
void probeLandDelay(int64_t delay, bool sawAir) {
    int const bucket = delay < 0    ? 0
                     : delay == 0   ? 1
                     : delay <= 3   ? 2
                     : delay <= 10  ? 3
                     : delay <= 20  ? 4
                     : delay <= 50  ? 5
                     : delay <= 100 ? 6
                                    : 7;
    gLandDelayHist[(sawAir ? 8 : 0) + bucket].fetch_add(1, std::memory_order_relaxed);
}

// Caller holds gProbeMutex. Cumulative, so the last line of each class is the total.
void probeRound33Locked() {
    probeLineLocked(
        "landing polled=%llu overrunEnd=%llu\n",
        static_cast<unsigned long long>(gLandPolls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gOverrunEnds.load(std::memory_order_relaxed))
    );
    probeLineLocked(
        "armLightFix windows=%llu headQ=%llu rewrites=%llu rawDark=%llu\n",
        static_cast<unsigned long long>(gLightFixWindows.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLightFixHeadQueries.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLightFixRewrites.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLightFixRawDark.load(std::memory_order_relaxed))
    );
    for (int cls = 0; cls < 16; ++cls) {
        auto const sampled = gArmLight[cls].sampled.load(std::memory_order_relaxed);
        if (sampled == 0) continue;
        probeLineLocked(
            "armLightSum cls=%d sampled=%llu spread=%llu selfDark=%llu\n",
            cls,
            static_cast<unsigned long long>(sampled),
            static_cast<unsigned long long>(gArmLight[cls].spread.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gArmLight[cls].selfDark.load(std::memory_order_relaxed))
        );
    }
    for (int air = 0; air < 2; ++air) {
        auto const h = [&](int bucket) {
            return static_cast<unsigned long long>(gLandDelayHist[air * 8 + bucket].load(std::memory_order_relaxed));
        };
        probeLineLocked(
            "%s lt0=%llu d0=%llu d1_3=%llu d4_10=%llu d11_20=%llu d21_50=%llu d51_100=%llu gt100=%llu\n",
            air ? "landDelayAir" : "landDelay",
            h(0),
            h(1),
            h(2),
            h(3),
            h(4),
            h(5),
            h(6),
            h(7)
        );
    }
    for (int cls = 0; cls < 16; ++cls)
        for (int kind = 0; kind < 3; ++kind)
            for (int cell = 0; cell <= kLightCells; ++cell) {
                auto const n = gLightBuckets[cls][kind][cell].n.load(std::memory_order_relaxed);
                if (n == 0) continue;
                auto const dark = gLightBuckets[cls][kind][cell].dark.load(std::memory_order_relaxed);
                if (cell == kLightCells) {
                    probeLineLocked(
                        "armLightQ cls=%d kind=%d rel=far n=%llu dark=%llu\n",
                        cls,
                        kind,
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(dark)
                    );
                    continue;
                }
                probeLineLocked(
                    "armLightQ cls=%d kind=%d rel=%d,%d,%d n=%llu dark=%llu\n",
                    cls,
                    kind,
                    cell / (kLightSpan * kLightSpan) - 2,
                    cell / kLightSpan % kLightSpan - 2,
                    cell % kLightSpan - 2,
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(dark)
                );
            }
}

// A translucent proxy dispatched in the opaque pass.
void probeAlphaEarly(::BlockPos const& pos) {
    static std::atomic<uint64_t> lines{0};
    auto const                   n = lines.fetch_add(1, std::memory_order_relaxed);
    if (n >= 300) return;
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked("alphaEarly n=%llu cell=%d,%d,%d\n", static_cast<unsigned long long>(n), pos.x, pos.y, pos.z);
}

#if OPTIPISTON_MC != 2610
// One dump of the materials the moving-block renderer draws with: depth write and blend decide how glass sorts.
void probeMovingMaterials(::MovingBlockActorRenderer& renderer) {
    static std::atomic_bool done{false};
    if (done.exchange(true)) return;
    std::lock_guard const guard(gProbeMutex);
    int                   index = 0;
    for (auto& slot : renderer.mBlockMaterials) {
        auto const* const info = slot.get().mRenderMaterialInfoPtr.get();
        auto const* const mat  = info ? info->mPtr.get() : nullptr;
        if (!mat) {
            probeLineLocked("material index=%d null\n", index++);
            continue;
        }
        auto const& depth = mat->depthStencilStateDescription.get();
        auto const& blend = mat->blendStateDescription.get();
        probeLineLocked(
            "material index=%d name=%s depthTest=%d depthWrite=%d depthFunc=%d blend=%d src=%d dst=%d colorMask=%d "
            "bias=%.2f\n",
            index++,
            info->mHashedName.get().c_str(),
            depth.depthTestEnabled ? 1 : 0,
            static_cast<int>(depth.depthWriteMask),
            static_cast<int>(depth.depthFunc),
            blend.enableBlend ? 1 : 0,
            static_cast<int>(blend.blendSource),
            static_cast<int>(blend.blendDestination),
            static_cast<int>(blend.colorWriteMask),
            static_cast<double>(mat->mDepthBias)
        );
    }
}
#endif

// Which tessellation entry points a managed cell goes through, and whether they were suppressed.
void probeMeshCall(bool outer, ::BlockPos const& pos, bool hidden) {
    if (tRenderDepth > 0 || gProbePerfOnly.load(std::memory_order_relaxed)) return;
    MeshViewPin pin;
    if (!pin.get().managed(cellOf(pos))) return;
    auto& counter =
        outer ? (hidden ? gProbe.outerHidden : gProbe.outerPassed) : (hidden ? gProbe.innerHidden : gProbe.innerPassed);
    counter.fetch_add(1, std::memory_order_relaxed);
}

void probeMeshBuildStarted(::RenderChunkGeometry const* geometry) {
    gProbe.buildStarts.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard const guard(gProbeMutex);
    for (auto& [pos, cell] : gProbeCells)
        if (cell.geometry == geometry && cell.meshVerified) ++cell.followStarts;
    gProbeBuilt.erase(geometry);
    if (geometry) {
        auto& org = gProbeOrigins[geometry->mPosition.get()];
        ++org.starts;
        org.lastStart  = gFrame.load(std::memory_order_relaxed);
        org.startFrame = org.lastStart;
        org.rebuildEnd = 0;
        org.asks       = 0;
        org.lastAsk    = 0;
        org.atStart    = probeGeomOf(geometry);
        org.depthAtStart =
            gProbe.buildStarts.load(std::memory_order_relaxed) - gProbe.buildCommits.load(std::memory_order_relaxed);
    }
}

// Wall and CPU time of one rebuild() on the worker; a gap between them means the build was blocked.
struct ProbeRebuildClock {
    LARGE_INTEGER    wall0{};
    ULONGLONG        cpu0{};
    static ULONGLONG cpuNow() {
        FILETIME create{}, exit{}, kernel{}, user{};
        GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user);
        auto const join = [](FILETIME const& t) {
            return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        return join(kernel) + join(user);
    }
    ProbeRebuildClock() {
        QueryPerformanceCounter(&wall0);
        cpu0 = cpuNow();
    }
    uint64_t wallUs() const {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        return static_cast<uint64_t>((now.QuadPart - wall0.QuadPart) * 1000000 / freq.QuadPart);
    }
    uint64_t cpuUs() const { return (cpuNow() - cpu0) / 10; }
};

// A worker's rebuild() returned; the commit (endRebuild) follows later on the frame thread.
void probeMeshBuildReturned(::RenderChunkGeometry const* geometry, ProbeRebuildClock const& clock, void const* builder) {
    if (!geometry) return;
    auto const            frame = gFrame.load(std::memory_order_relaxed);
    std::lock_guard const guard(gProbeMutex);
    auto&                 org = gProbeOrigins[geometry->mPosition.get()];
    org.rebuildEnd            = frame;
    org.rebuildWallUs         = clock.wallUs();
    org.rebuildCpuUs          = clock.cpuUs();
    org.atReturn              = probeGeomOf(geometry);
    org.builder               = builder;
    org.rebuildTid            = GetCurrentThreadId();
}
// Our re-mesh asks, per chunk origin the area touches.
void probeAreaAsk(::BlockPos const& min, ::BlockPos const& max) {
    auto const            frame = gFrame.load(std::memory_order_relaxed);
    std::lock_guard const guard(gProbeMutex);
    ::BlockPos            seen[8];
    int                   count = 0;
    for (int dx = 0; dx < 2; ++dx)
        for (int dy = 0; dy < 2; ++dy)
            for (int dz = 0; dz < 2; ++dz) {
                auto const origin =
                    probeOriginOf(::BlockPos{dx ? max.x : min.x, dy ? max.y : min.y, dz ? max.z : min.z});
                bool known = false;
                for (int i = 0; i < count; ++i) known = known || seen[i] == origin;
                if (known) continue;
                seen[count++] = origin;
                auto& org     = gProbeOrigins[origin];
                ++org.asks;
                org.lastAsk = frame;
            }
}
// The call chain of area-change events, as a reference for what a commit may run under.
void probeAreaChain(bool ours) {
    static std::atomic<uint32_t> seen[2]{};
    if (seen[ours ? 1 : 0].fetch_add(1, std::memory_order_relaxed) >= (ours ? 60u : 400u)) return;
    auto const            chain = probeCallChain();
    std::lock_guard const guard(gProbeMutex);
    if (gProbeAreaChains.insert(std::string(ours ? "ours " : "game ") + chain).second)
        probeLineLocked("areaChain ours=%d by=%s\n", ours ? 1 : 0, chain.c_str());
}
// Credits the cells the committed build contained, and revokes the ones an earlier build of the geometry had.
void probeMeshCommitted(::RenderChunkGeometry const* geometry) {
    gProbe.buildCommits.fetch_add(1, std::memory_order_relaxed);
    auto const              frame = gFrame.load(std::memory_order_relaxed);
    std::lock_guard const   guard(gProbeMutex);
    std::vector<ProbeBuilt> built;
    if (auto const it = gProbeBuilt.find(geometry); it != gProbeBuilt.end()) {
        built = std::move(it->second);
        gProbeBuilt.erase(it);
    }
    {
        auto&             org        = gProbeOrigins[geometry->mPosition.get()];
        uint64_t const    latency    = frame >= org.startFrame ? frame - org.startFrame : 0;
        std::size_t const heldBefore = org.live.size();
        int const         bucket     = latency < 2  ? 0
                                     : latency < 4  ? 1
                                     : latency < 8  ? 2
                                     : latency < 16 ? 3
                                     : latency < 32 ? 4
                                                    : 5;
        gProbe.buildLat[bucket].fetch_add(1, std::memory_order_relaxed);
        {
            auto&      tally = gProbeChains[tProbeCommitChain];
            bool const first = tally.fast + tally.slow == 0;
            ++(latency >= 8 ? tally.slow : tally.fast);
            tally.maxLatency = std::max<uint64_t>(tally.maxLatency, latency);
            if (first && gProbeChainLines < 80) {
                ++gProbeChainLines;
                probeLineLocked(
                    "commitChain firstSeenLatency=%llu by=%s\n",
                    static_cast<unsigned long long>(latency),
                    tProbeCommitChain.c_str()
                );
            }
        }
        static uint32_t fastLogged = 0;
        if (latency < 8 && org.asks > 0 && fastLogged < 60) {
            ++fastLogged;
            auto const origin = geometry->mPosition.get();
            probeLineLocked(
                "fastBuild origin=%d,%d,%d latency=%llu asks=%llu lastAskAgo=%lld C[%s]\n",
                origin.x,
                origin.y,
                origin.z,
                static_cast<unsigned long long>(latency),
                static_cast<unsigned long long>(org.asks),
                org.lastAsk ? static_cast<long long>(frame) - static_cast<long long>(org.lastAsk) : -1LL,
                probeGeomText(probeGeomOf(geometry)).c_str()
            );
        }
        if (latency >= 8 && gProbe.buildLatSlow.fetch_add(1, std::memory_order_relaxed) < 300) {
            auto const origin = geometry->mPosition.get();
            probeLineLocked(
                "slowBuild origin=%d,%d,%d started=%llu committed=%llu latency=%llu depthAtStart=%llu depthNow=%lld "
                "heldBefore=%zu heldAfter=%zu rebuildEnd=%llu inRebuild=%llu toCommit=%llu wallMs=%.1f cpuMs=%.1f "
                "curTick=%lld asks=%llu lastAskAgo=%lld bldR=%p bldC=%p tidR=%lu tidC=%lu by=%s S[%s] R[%s] C[%s]\n",
                origin.x,
                origin.y,
                origin.z,
                static_cast<unsigned long long>(org.startFrame),
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(latency),
                static_cast<unsigned long long>(org.depthAtStart),
                static_cast<long long>(gProbe.buildStarts.load(std::memory_order_relaxed))
                    - static_cast<long long>(gProbe.buildCommits.load(std::memory_order_relaxed)),
                heldBefore,
                built.size(),
                static_cast<unsigned long long>(org.rebuildEnd),
                static_cast<unsigned long long>(
                    org.rebuildEnd >= org.startFrame && org.rebuildEnd ? org.rebuildEnd - org.startFrame : 0
                ),
                static_cast<unsigned long long>(org.rebuildEnd && frame >= org.rebuildEnd ? frame - org.rebuildEnd : 0),
                static_cast<double>(org.rebuildWallUs) / 1000.0,
                static_cast<double>(org.rebuildCpuUs) / 1000.0,
                static_cast<long long>(gLastTick.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(org.asks),
                org.lastAsk ? static_cast<long long>(frame) - static_cast<long long>(org.lastAsk) : -1LL,
                org.builder,
                tProbeCommitBuilder,
                org.rebuildTid,
                GetCurrentThreadId(),
                tProbeCommitChain.c_str(),
                probeGeomText(org.atStart).c_str(),
                probeGeomText(org.atReturn).c_str(),
                probeGeomText(probeGeomOf(geometry)).c_str()
            );
            static uint64_t lastDump = 0;
            if (frame != lastDump && gProbeChainLines < 160) {
                lastDump = frame;
                for (auto const& [chain, tally] : gProbeChains) {
                    ++gProbeChainLines;
                    probeLineLocked(
                        "chainTally fast=%llu slow=%llu maxLatency=%llu by=%s\n",
                        static_cast<unsigned long long>(tally.fast),
                        static_cast<unsigned long long>(tally.slow),
                        static_cast<unsigned long long>(tally.maxLatency),
                        chain.c_str()
                    );
                }
            }
        }
        ++org.commits;
        org.lastCommit = frame;
        org.live.clear();
        for (auto const& entry : built) org.live.insert(entry.pos);
    }
    for (auto& [pos, cell] : gProbeCells) {
        if (cell.geometry != geometry || !cell.meshVerified) continue;
        ++cell.followCommits;
        if (std::any_of(built.begin(), built.end(), [&](ProbeBuilt const& b) { return b.pos == pos; })) continue;
        cell.meshVerified = false;
        cell.regressFrame = frame;
        gProbe.meshRegress.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto const& entry : built) {
        auto const it = gProbeCells.find(entry.pos);
        if (it == gProbeCells.end()) continue;
        // Only the first commit counts; a later rebuild of the same geometry does not take the mesh back off screen.
        if (!it->second.meshVerified) {
            it->second.meshVerified  = true;
            it->second.followStarts  = 0;
            it->second.followCommits = 0;
            it->second.meshCommit    = frame;
            it->second.builtFrame    = entry.frame;
            it->second.builtKind     = entry.kind;
        }
        it->second.geometry = geometry;
    }
}

char const* probeWhyName(int reason) {
    switch (reason) {
    case 1:
        return "pistonPosMismatch";
    case 2:
        return "newerAction";
    case 3:
        return "drawerVanilla";
    case 4:
        return "detached";
    default:
        break;
    }
    return "unmanaged";
}

char const* probeStateName(PushState state) {
    switch (state) {
    case PushState::Animating:
        return "animating";
    case PushState::Landing:
        return "landing";
    case PushState::Handover:
        return "handover";
    case PushState::Done:
        break;
    }
    return "done";
}

char const* probeDrawerName(PushDrawer drawer) {
    switch (drawer) {
    case PushDrawer::Proxy:
        return "proxy";
    case PushDrawer::Mesh:
        return "mesh";
    case PushDrawer::None:
        return "none";
    case PushDrawer::Vanilla:
        break;
    }
    return "vanilla";
}

char const* probeHandoverEndName(core::HandoverEnd end) {
    switch (end) {
    case core::HandoverEnd::ActorSeen:
        return "actorSeen";
    case core::HandoverEnd::Timeout:
        return "timeout";
    case core::HandoverEnd::None:
        break;
    }
    return "none";
}

unsigned long long probeCount(std::atomic<uint64_t> const& counter) {
    return static_cast<unsigned long long>(counter.load(std::memory_order_relaxed));
}

// Prints and resets the timing window. Caller holds gProbeMutex.
void probePerfLocked(double seconds) {
    auto const frequency = std::max<uint64_t>(perfFrequency(), 1);
    auto const micros    = [frequency](uint64_t ticks) {
        return static_cast<unsigned long long>(ticks * 1000000ull / frequency);
    };
    for (int id = 0; id < static_cast<int>(PerfId::Count); ++id) {
        for (int kind = 0; kind < 2; ++kind) {
            auto&      slot  = gPerf[id][kind];
            auto const calls = slot.calls.exchange(0, std::memory_order_relaxed);
            auto const total = slot.total.exchange(0, std::memory_order_relaxed);
            auto const max   = slot.max.exchange(0, std::memory_order_relaxed);
            if (calls == 0) continue;
            probeLineLocked(
                "perf %s %s calls=%llu totalUs=%llu maxUs=%llu\n",
                kPerfNames[id],
                kind == 0 ? "render" : "other",
                static_cast<unsigned long long>(calls),
                micros(total),
                micros(max)
            );
        }
    }
    auto const   frames = gFrameWork.calls.exchange(0, std::memory_order_relaxed);
    auto const   total  = gFrameWork.total.exchange(0, std::memory_order_relaxed);
    auto const   max    = gFrameWork.max.exchange(0, std::memory_order_relaxed);
    auto const   actors = gProbeActorFrames.exchange(0, std::memory_order_relaxed);
    double const avgUs =
        frames ? static_cast<double>(total) * 1000000.0 / (static_cast<double>(frequency) * static_cast<double>(frames))
               : 0.0;
    probeLineLocked(
        "frameWork avgUs=%.1f maxUs=%llu frames=%llu actorFrames=%llu fps=%.1f\n",
        avgUs,
        micros(max),
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(actors),
        seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0
    );
}

// Once per rendered frame on the render thread, before the frame counter advances.
void probeFrameEnd() {
    gProbeActorFrames.fetch_add(1, std::memory_order_relaxed);
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;

    // Whether the world holds a block in each settled cell; asked before any of our locks are taken.
    struct WorldAt {
        ::Block const* block;
        int            open; // air neighbours of a cell that was already vanishing, else -1
    };
    static thread_local std::unordered_map<::BlockPos, WorldAt, BlockPosHash> airAt;
    static std::unordered_map<::BlockPos, bool, BlockPosHash>                 vanishWatch;
    airAt.clear();
    if (vanishWatch.size() > 4096) vanishWatch.clear();
    {
        auto  client = ll::service::getClientInstance();
        auto* player = client ? client->getLocalPlayer() : nullptr;
        if (player) {
            auto&       region = player->getDimensionBlockSource();
            ViewPin     pin;
            auto const& cells = pin.get().cells;
            for (auto const& [pos, rec] : cells) {
                if (rec.timeline.state() == PushState::Animating) {
                    airAt.emplace(pos, WorldAt{&region.getBlock(pos), -1});
                    continue;
                }
                auto const& here = region.getBlock(pos);
                int         open = -1;
                if (!here.isAir() && vanishWatch.contains(pos)) {
                    open = 0;
                    for (auto const& d : {
                             ::BlockPos{1,  0,  0 },
                             ::BlockPos{-1, 0,  0 },
                             ::BlockPos{0,  1,  0 },
                             ::BlockPos{0,  -1, 0 },
                             ::BlockPos{0,  0,  1 },
                             ::BlockPos{0,  0,  -1}
                    })
                        if (region.getBlock(::BlockPos{pos.x + d.x, pos.y + d.y, pos.z + d.z}).isAir()) ++open;
                }
                airAt.emplace(pos, WorldAt{&here, open});
            }
        }
    }

    std::lock_guard const anim(gAnimMutex);
    std::lock_guard const guard(gProbeMutex);
    auto const            frame     = gFrame.load(std::memory_order_relaxed);
    auto const            realFrame = gRealFrame.load(std::memory_order_relaxed);
    auto const            logFrame  = static_cast<int64_t>(frame);
    gProbe.frames.fetch_add(1, std::memory_order_relaxed);
    {
        auto const raise = [](std::atomic<uint64_t>& slot, std::size_t value) {
            if (value > slot.load(std::memory_order_relaxed)) slot.store(value, std::memory_order_relaxed);
        };
        raise(gProbe.cellsMax, gCells.size());
        raise(gProbe.pistonsMax, gPistonVisuals.size());
        raise(gProbe.actionsMax, gActionVisuals.size());
    }
    for (int pass = 0; pass < 2; ++pass) {
        auto const calls = gProbe.rbeFrame[pass].exchange(0, std::memory_order_relaxed);
        if (calls == 0) gProbe.rbeMissing[pass].fetch_add(1, std::memory_order_relaxed);
        if (calls > gProbe.rbeMax[pass].load(std::memory_order_relaxed))
            gProbe.rbeMax[pass].store(calls, std::memory_order_relaxed);
    }
    std::erase_if(gProbeCells, [](auto& entry) {
        if (gCells.contains(entry.first)) return false;
        probeCloseDupLocked(entry.first, entry.second);
        return true;
    });
    gProbeAnyCell.store(!gProbeCells.empty(), std::memory_order_relaxed);
    for (auto it = gProbeCleared.begin(); it != gProbeCleared.end();) {
        if (frame - it->second.frame < 120) {
            ++it;
            continue;
        }
        probeClearCloseLocked(it->first, it->second, false);
        it = gProbeCleared.erase(it);
    }

    // The incrementally kept census, mesh view and render view must equal what the tables say.
    {
        core::TraitCensus recount;
        core::MeshView    rebuilt;
        for (auto const& [pos, rec] : gCells) {
            recount.replace(0, core::pushTraits(rec.timeline));
            rebuilt.set(cellOf(pos), core::meshCellOf(rec.timeline));
        }
        bool same = recount == gCensus && rebuilt == gMeshBuilder;
        if (!gMeshDirty) {
            auto const published = gMeshView.load(std::memory_order_acquire);
            same                 = same && (published ? *published : emptyMeshView()) == gMeshBuilder;
        }
        if (auto const view = gRenderView.load(std::memory_order_acquire);
            view && view->revision == gRevision.load(std::memory_order_acquire)) {
            same = same && view->cells.size() == gCells.size() && view->pistons.size() == gPistonVisuals.size()
                && view->actions.size() == gActionVisuals.size();
            for (auto const& [pos, rec] : gCells) {
                auto const it = view->cells.find(pos);
                same          = same && it != view->cells.end() && it->second.action == rec.action
                    && it->second.timeline.state() == rec.timeline.state() && it->second.proxy == rec.proxy;
            }
        }
        gProbeViewChecks.fetch_add(1, std::memory_order_relaxed);
        if (!same) gProbeViewMismatch.fetch_add(1, std::memory_order_relaxed);
    }

    for (auto const& [pos, rec] : gCells) {
        auto& cell = gProbeCells[pos];
        if (cell.action != rec.action) {
            probeCloseDupLocked(pos, cell);
            cell        = ProbeCell{};
            cell.action = rec.action;
        }
        // An episode that was not continued on the previous frame is over.
        if (cell.dupFrames > 0 && cell.dupLastFrame + 1 < frame) probeCloseDupLocked(pos, cell);
        auto const state  = rec.timeline.state();
        auto const drawer = drawerOf(rec, frame);
        if (!cell.haveDrawer || cell.drawer != drawer) {
            auto const slowWorld = core::worldSpeedSettings().applied.load(std::memory_order_relaxed) != 1.0f;
            if (cell.haveDrawer && cell.drawer == PushDrawer::Proxy
                && (slowWorld ? gProbe.proxyEndSlowLines : gProbe.proxyEndLines).fetch_add(1, std::memory_order_relaxed)
                       < (slowWorld ? 800u : 100u)) {
                auto const& segment = rec.visual.segment;
                auto const  at      = gLastVisualTime.load(std::memory_order_relaxed);
                probeLineLocked(
                    "proxyEnd cell=%d,%d,%d to=%s state=%s landed=%d left=%.3f t=%.3f start=%lld len=%.2f "
                    "speed=%.2f\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    probeDrawerName(drawer),
                    probeStateName(state),
                    rec.timeline.landed() ? 1 : 0,
                    static_cast<double>(std::fabs(core::segmentValue(segment, at) - segment.to)),
                    at,
                    static_cast<long long>(segment.startTick),
                    segment.length,
                    static_cast<double>(core::worldSpeedSettings().applied.load(std::memory_order_relaxed))
                );
            }
            cell.haveDrawer  = true;
            cell.drawer      = drawer;
            cell.switchFrame = frame;
        }
        // Both stay 0 in a healthy run: a record outliving its segment, and a finished record still holding a proxy.
        if (state != PushState::Done
            && gLastTick.load(std::memory_order_relaxed) - core::segmentEndTick(rec.visual.segment) > 60) {
            gProbe.stuckCells.fetch_add(1, std::memory_order_relaxed);
            if (logFrame - cell.lastStuckLog >= 200) {
                cell.lastStuckLog = logFrame;
                probeLineLocked(
                    "stuck cell=%d,%d,%d state=%s act=%llu proxy=%d carries=%d landed=%d\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    probeStateName(state),
                    static_cast<unsigned long long>(rec.action),
                    rec.proxy != nullptr ? 1 : 0,
                    rec.carry.has_value() ? 1 : 0,
                    rec.timeline.landed() ? 1 : 0
                );
            }
        }
        if (state == PushState::Done && rec.proxy != nullptr) {
            gProbe.doneWithProxy.fetch_add(1, std::memory_order_relaxed);
            if (logFrame - cell.lastDoneProxyLog >= 200) {
                cell.lastDoneProxyLog = logFrame;
                probeLineLocked(
                    "doneProxy cell=%d,%d,%d act=%llu\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    static_cast<unsigned long long>(rec.action)
                );
            }
        }
        // The world holds a block in a settled cell, yet neither a proxy, the game's instance nor a committed mesh
        // draws it.
        if (state != PushState::Animating
            && (drawer == PushDrawer::Proxy || drawer == PushDrawer::Mesh || drawer == PushDrawer::Vanilla)) {
            auto const seenIt = gProbeTally.find(pos);
            bool const seenDrawn =
                seenIt != gProbeTally.end()
                && ((seenIt->second.rendered > 0 && seenIt->second.drawPos > 0) || seenIt->second.vanilla > 0);
            auto const  airIt      = airAt.find(pos);
            bool const  blockThere = airIt != airAt.end() && !airIt->second.block->isAir();
            char const* blockName  = airIt != airAt.end() ? airIt->second.block->getTypeName().c_str() : "?";
            bool const  meshShows  = cell.meshVerified && cell.meshCommit < frame;
            // One cap per drawer and landed flag, so a flood in one class cannot hide the others.
            int const klass = (drawer == PushDrawer::Proxy  ? 0
                               : drawer == PushDrawer::Mesh ? 1
                                                            : 2)
                                * 2
                            + (rec.timeline.landed() ? 1 : 0);
            static std::atomic<uint64_t> vanishLines[6];
            static std::atomic<uint64_t> vanishEnds[6];
            if (blockThere && !seenDrawn && !meshShows) {
                ++cell.vanishFrames;
                if (cell.vanishFrames == 1) vanishWatch[pos] = true;
                if ((cell.vanishFrames == 1 || cell.vanishFrames == 10 || cell.vanishFrames % 100 == 0)
                    && vanishLines[klass].fetch_add(1, std::memory_order_relaxed) < 300)
                    probeLineLocked(
                        "vanish cell=%d,%d,%d drawer=%s state=%s act=%llu frames=%llu tessAge=%lld produced=%d "
                        "landed=%d block=%s open=%d\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        probeDrawerName(drawer),
                        probeStateName(state),
                        static_cast<unsigned long long>(rec.action),
                        static_cast<unsigned long long>(cell.vanishFrames),
                        cell.lastTessFrame ? static_cast<long long>(frame - cell.lastTessFrame) : -1LL,
                        cell.lastTessProduced ? 1 : 0,
                        rec.timeline.landed() ? 1 : 0,
                        blockName,
                        airIt->second.open
                    );
            } else {
                if (cell.vanishFrames > 0 && vanishEnds[klass].fetch_add(1, std::memory_order_relaxed) < 300)
                    probeLineLocked(
                        "vanishEnd cell=%d,%d,%d drawer=%s state=%s frames=%llu landed=%d now=%s tally=%d mesh=%d\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        probeDrawerName(drawer),
                        probeStateName(state),
                        static_cast<unsigned long long>(cell.vanishFrames),
                        rec.timeline.landed() ? 1 : 0,
                        blockThere ? blockName : "air",
                        seenDrawn ? 1 : 0,
                        meshShows ? 1 : 0
                    );
                if (cell.vanishFrames > 0) vanishWatch.erase(pos);
                cell.vanishFrames = 0;
            }
        } else if (cell.vanishFrames > 0) {
            vanishWatch.erase(pos);
            cell.vanishFrames = 0;
        }
        // A carried block actor is entity-rendered, so a committed mesh says nothing about it being visible. This runs
        // before the drawer filter below so Done cells get here too; the tally is looked up on its own for that reason.
        if (rec.timeline.carriesActor()) {
            ProbeTally        noTally;
            auto const        found        = gProbeTally.find(pos);
            auto const&       seen         = found != gProbeTally.end() ? found->second : noTally;
            bool const        carriedDrawn = seen.rendered > 0 && seen.drawPos > 0;
            auto const        actorFrame   = rec.shared->actorFrame.load(std::memory_order_relaxed);
            bool const        actorNoted   = actorFrame == frame;
            auto const        end          = rec.timeline.handoverEnd();
            char const* const endName      = probeHandoverEndName(end);
            if (state == PushState::Done) {
                if (!cell.summarized) {
                    cell.summarized = true;
                    cell.doneFrame  = frame;
                    gProbe.carriedDone.fetch_add(1, std::memory_order_relaxed);
                    if (end == core::HandoverEnd::Timeout)
                        gProbe.carriedTimeout.fetch_add(1, std::memory_order_relaxed);
                    probeLineLocked(
                        "carried cell=%d,%d,%d act=%llu anim=%d landing=%d handover=%d proxy=%d actor=%d end=%s "
                        "block=%s\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        static_cast<unsigned long long>(rec.action),
                        cell.animFrames,
                        cell.landingFrames,
                        cell.handoverFrames,
                        cell.proxyFrames,
                        cell.actorFrames,
                        endName,
                        rec.wrapped ? rec.wrapped->c_str() : "?"
                    );
                }
            } else {
                if (state == PushState::Animating) ++cell.animFrames;
                else if (state == PushState::Landing) ++cell.landingFrames;
                else ++cell.handoverFrames;
                if (carriedDrawn) ++cell.proxyFrames;
                if (actorNoted) ++cell.actorFrames;
            }
            // Done frames are not judged: the landed actor is then only drawn by the game and never noted.
            bool const landingOrHandover = state == PushState::Landing || state == PushState::Handover;
            if (landingOrHandover && !carriedDrawn && !actorNoted && seen.vanilla == 0) {
                gProbe.actorGap.fetch_add(1, std::memory_order_relaxed);
                if (logFrame - cell.lastActorGap >= 20) {
                    cell.lastActorGap = logFrame;
                    probeLineLocked(
                        "actorGap cell=%d,%d,%d state=%s drawer=%s proxyDrawn=%d actorNoted=%d actorFrame=%llu "
                        "frame=%llu end=%s\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        probeStateName(state),
                        probeDrawerName(drawer),
                        carriedDrawn ? 1 : 0,
                        actorNoted ? 1 : 0,
                        static_cast<unsigned long long>(actorFrame),
                        static_cast<unsigned long long>(frame),
                        endName
                    );
                }
            }
            if (landingOrHandover && carriedDrawn && actorNoted
                && rec.shared->strippedFrame.load(std::memory_order_relaxed) != frame) {
                gProbe.actorDouble.fetch_add(1, std::memory_order_relaxed);
                if (logFrame - cell.lastActorDouble >= 20) {
                    cell.lastActorDouble = logFrame;
                    probeLineLocked(
                        "actorDouble cell=%d,%d,%d state=%s drawer=%s\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        probeStateName(state),
                        probeDrawerName(drawer)
                    );
                }
            }
        }
        if (drawer != PushDrawer::Proxy && drawer != PushDrawer::Mesh) continue;
        ProbeTally tally;
        if (auto const it = gProbeTally.find(pos); it != gProbeTally.end()) tally = it->second;

        // Drawn means the renderer path ran and asked for the draw position, not just that we dispatched it.
        bool const        proxyDrawn = tally.rendered > 0 && tally.drawPos > 0;
        bool const        meshOn     = cell.meshVerified && cell.meshCommit < frame;
        char const* const stateName  = probeStateName(state);

        // Pre-arrival checks on a proxy that is still on its way: is it placed, does it move smoothly, is it lit.
        if (state == PushState::Animating && proxyDrawn) {
            gProbe.animCellFrames.fetch_add(1, std::memory_order_relaxed);
            if (!tally.timed) {
                gProbe.unpositioned.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<uint64_t> used{0};
                if (cell.unposLogged < 1 && probeBudget(used, 100, "unpositioned")) {
                    ++cell.unposLogged;
                    probeLineLocked(
                        "unpositioned cell=%d,%d,%d act=%llu drawer=%s dispatched=%d rendered=%d drawPos=%d\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        static_cast<unsigned long long>(rec.action),
                        drawer == PushDrawer::Mesh ? "mesh" : "proxy",
                        tally.dispatched,
                        tally.rendered,
                        tally.drawPos
                    );
                }
            } else {
                if (cell.havePos && cell.lastPosFrame + 1 == frame) {
                    gProbe.posChecks.fetch_add(1, std::memory_order_relaxed);
                    double const dt = tally.time - cell.lastTime;
                    auto const   dx = static_cast<double>(tally.vx - cell.lastX);
                    auto const   dy = static_cast<double>(tally.vy - cell.lastY);
                    auto const   dz = static_cast<double>(tally.vz - cell.lastZ);
                    double const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    auto const   rate = [](core::Segment const& s) {
                        return s.length > 0.0 ? std::fabs(static_cast<double>(s.to - s.from)) / s.length : 0.0;
                    };
                    double speed = rate(rec.visual.segment);
                    if (rec.carry) speed += rate(rec.carry->visual.segment);
                    double const expected = speed * std::max(dt, 0.0);
                    auto const   magNow   = std::sqrt(
                        static_cast<double>(tally.vx) * tally.vx + static_cast<double>(tally.vy) * tally.vy
                        + static_cast<double>(tally.vz) * tally.vz
                    );
                    auto const magLast = std::sqrt(
                        static_cast<double>(cell.lastX) * cell.lastX + static_cast<double>(cell.lastY) * cell.lastY
                        + static_cast<double>(cell.lastZ) * cell.lastZ
                    );
                    char const* kind = nullptr;
                    if (dist > 2.0 * expected + 0.03) {
                        kind = "jump";
                        gProbe.posJump.fetch_add(1, std::memory_order_relaxed);
                    } else if (!rec.carry && dt > 1.0e-4 && magNow > magLast + 0.02) {
                        kind = "back";
                        gProbe.posBack.fetch_add(1, std::memory_order_relaxed);
                    }
                    static std::atomic<uint64_t> used{0};
                    if (kind && cell.stepLogged < 3 && probeBudget(used, 200, "posStep")) {
                        ++cell.stepLogged;
                        probeLineLocked(
                            "posStep cell=%d,%d,%d kind=%s act=%llu dist=%.3f expected=%.3f dt=%.4f magLast=%.3f "
                            "magNow=%.3f time=%.3f carry=%d mesh=%d\n",
                            pos.x,
                            pos.y,
                            pos.z,
                            kind,
                            static_cast<unsigned long long>(rec.action),
                            dist,
                            expected,
                            dt,
                            magLast,
                            magNow,
                            tally.time,
                            rec.carry ? 1 : 0,
                            meshOn ? 1 : 0
                        );
                    }
                }
                cell.lastX       = tally.vx;
                cell.lastY       = tally.vy;
                cell.lastZ       = tally.vz;
                cell.lastTime    = tally.time;
                cell.lastPosFrame = frame;
                cell.havePos     = true;
            }
            if (tally.lightQ > 0) {
                int const sig = 1 + ((tally.lightDark > 0 ? 1 : 0) | (tally.lightSelfDark > 0 ? 2 : 0));
                gProbe.lightFrames.fetch_add(1, std::memory_order_relaxed);
                if (tally.lightDark > 0) gProbe.lightAnyDark.fetch_add(1, std::memory_order_relaxed);
                if (tally.lightSelfDark > 0) gProbe.lightSelfDark.fetch_add(1, std::memory_order_relaxed);
                if (cell.lastLightSig >= 0 && cell.lastLightFrame + 1 == frame && sig != cell.lastLightSig) {
                    gProbe.lightFlip.fetch_add(1, std::memory_order_relaxed);
                    static std::atomic<uint64_t> used{0};
                    if (probeBudget(used, 150, "lightFlip"))
                        probeLineLocked(
                            "lightFlip cell=%d,%d,%d act=%llu prev=%d now=%d q=%d dark=%d selfDark=%d off=%.3f\n",
                            pos.x,
                            pos.y,
                            pos.z,
                            static_cast<unsigned long long>(rec.action),
                            cell.lastLightSig,
                            sig,
                            tally.lightQ,
                            tally.lightDark,
                            tally.lightSelfDark,
                            static_cast<double>(tally.maxOffset)
                        );
                }
                cell.lastLightSig   = sig;
                cell.lastLightFrame = frame;
            }
        }

        if (proxyDrawn) {
            gProbe.proxyDraws.fetch_add(1, std::memory_order_relaxed);
            if (!cell.firstRender) {
                cell.firstRender = true;
                auto const gap   = realFrame > rec.probeStartFrame ? realFrame - rec.probeStartFrame : 0;
                if (gap > 0) {
                    gProbe.startGap.fetch_add(1, std::memory_order_relaxed);
                    probeLineLocked(
                        "startGap cell=%d,%d,%d frames=%llu vanillaHiddenBefore=%d\n",
                        pos.x,
                        pos.y,
                        pos.z,
                        static_cast<unsigned long long>(gap),
                        cell.hiddenBeforeRender > 0 ? 1 : 0
                    );
                }
            }
        }
        if (tally.dispatched > 0 && !proxyDrawn) {
            gProbe.dispatchNoRender.fetch_add(1, std::memory_order_relaxed);
            if (logFrame - cell.lastLost >= 20) {
                cell.lastLost = logFrame;
                probeLineLocked(
                    "lost cell=%d,%d,%d state=%s pass=%s dispatched=1 rendered=%d drawPos=%d\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    stateName,
                    tally.pass == 1 ? "alpha" : "opaque",
                    tally.rendered > 0 ? 1 : 0,
                    tally.drawPos > 0 ? 1 : 0
                );
            }
        }
        if (drawer == PushDrawer::Proxy && tally.dispatched == 0) {
            gProbe.noDispatch.fetch_add(1, std::memory_order_relaxed);
            if (logFrame - cell.lastNoDispatch >= 20) {
                cell.lastNoDispatch = logFrame;
                probeLineLocked("noDispatch cell=%d,%d,%d state=%s\n", pos.x, pos.y, pos.z, stateName);
            }
        }
        if ((drawer == PushDrawer::Mesh || state == PushState::Handover) && !meshOn) {
            gProbe.meshGap.fetch_add(1, std::memory_order_relaxed);
            if (logFrame - cell.lastMeshGap >= 20) {
                cell.lastMeshGap = logFrame;
                probeLineLocked(
                    "meshGap cell=%d,%d,%d state=%s framesSinceSwitch=%llu realCommitted=%d\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    stateName,
                    static_cast<unsigned long long>(frame - cell.switchFrame),
                    cell.meshVerified ? 1 : 0
                );
            }
        }

        if (!proxyDrawn && tally.vanilla == 0 && !meshOn) {
            gProbe.zero.fetch_add(1, std::memory_order_relaxed);
            if (state == PushState::Animating) gProbe.zeroAnim.fetch_add(1, std::memory_order_relaxed);
            if (cell.zeroLogged++ < 3)
                probeLineLocked(
                    "zero cell=%d,%d,%d state=%s act=%llu drawer=%s dispatched=%d rendered=%d drawPos=%d\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    stateName,
                    static_cast<unsigned long long>(rec.action),
                    drawer == PushDrawer::Mesh ? "mesh" : "proxy",
                    tally.dispatched,
                    tally.rendered,
                    tally.drawPos
                );
            continue;
        }
        if (proxyDrawn && tally.maxOffset > 2.05f) {
            static std::atomic<uint64_t> farLines{0};
            if (farLines.fetch_add(1, std::memory_order_relaxed) < 300)
                probeLineLocked(
                    "farOffset cell=%d,%d,%d state=%s act=%llu off=%.2f carry=%d\n",
                    pos.x,
                    pos.y,
                    pos.z,
                    stateName,
                    static_cast<unsigned long long>(rec.action),
                    static_cast<double>(tally.maxOffset),
                    rec.carry.has_value() ? 1 : 0
                );
        }
        bool const proxyAway = proxyDrawn && tally.maxOffset > 1.0e-3f;
        bool       liveHeld  = false;
        uint64_t   orgStarts = 0, orgCommits = 0, orgLastStart = 0, orgLastCommit = 0;
        if (auto const org = gProbeOrigins.find(probeOriginOf(pos)); org != gProbeOrigins.end()) {
            liveHeld      = org->second.lastCommit < frame && org->second.live.contains(pos);
            orgStarts     = org->second.starts - cell.originStarts;
            orgCommits    = org->second.commits - cell.originCommits;
            orgLastStart  = org->second.lastStart;
            orgLastCommit = org->second.lastCommit;
        }
        if (proxyAway && liveHeld) gProbe.liveDouble.fetch_add(1, std::memory_order_relaxed);
        char const* who = nullptr;
        if (proxyAway && meshOn) who = "proxy+mesh";
        else if (proxyDrawn && tally.vanilla >= 1) who = "proxy+vanilla";
        else if (tally.rendered >= 2 && tally.maxOffset - tally.minOffset > 1.0e-3f) who = "proxy+proxy";
        else if (meshOn && tally.vanilla >= 1) who = "mesh+vanilla";
        if (!who) continue;
        gProbe.doubled.fetch_add(1, std::memory_order_relaxed);
        {
            auto const now = std::chrono::steady_clock::now();
            if (cell.dupFrames == 0) {
                cell.dupStart     = frame;
                cell.dupStartOff  = tally.maxOffset;
                cell.dupStartWall = now;
                cell.dupWho       = who;
                cell.dupState     = stateName;
                auto const world  = airAt.find(pos);
                cell.dupWorld     = world != airAt.end() ? world->second.block->getTypeName() : std::string("?");
            }
            ++cell.dupFrames;
            cell.dupLastFrame = frame;
            cell.dupLastWall  = now;
            cell.dupEndOff    = tally.maxOffset;
            if (tally.maxOffset > 0.1f) ++cell.dupVis;
        }
        auto const episodeFrames = ++cell.doubleLogged;
        if (episodeFrames > 3 && episodeFrames != 8 && episodeFrames != 16 && episodeFrames != 32
            && episodeFrames != 64)
            continue;
        bool const vanilla = tally.vanilla >= 1;
        auto const worldIt = airAt.find(pos);
        probeLineLocked(
            "double cell=%d,%d,%d who=%s why=%s recAction=%llu pistonAction=%llu state=%s whyRec=%llu created=%llu "
            "built=%llu kind=%d commit=%llu now=%llu n=%d world=%s follow=%d/%d org=%llu/%llu last=%llu/%llu live=%d "
            "tess=%llu\n",
            pos.x,
            pos.y,
            pos.z,
            who,
            vanilla ? probeWhyName(tally.why) : "-",
            static_cast<unsigned long long>(rec.action),
            static_cast<unsigned long long>(vanilla ? tally.whyPiston : 0),
            probeStateName(vanilla ? tally.whyState : state),
            static_cast<unsigned long long>(vanilla ? tally.whyRec : 0),
            static_cast<unsigned long long>(cell.createdFrame),
            static_cast<unsigned long long>(cell.builtFrame),
            cell.builtKind,
            static_cast<unsigned long long>(cell.meshCommit),
            static_cast<unsigned long long>(frame),
            episodeFrames,
            worldIt != airAt.end() ? worldIt->second.block->getTypeName().c_str() : "?",
            cell.followStarts,
            cell.followCommits,
            static_cast<unsigned long long>(orgStarts),
            static_cast<unsigned long long>(orgCommits),
            static_cast<unsigned long long>(orgLastStart),
            static_cast<unsigned long long>(orgLastCommit),
            liveHeld ? 1 : 0,
            static_cast<unsigned long long>(cell.lastTessFrame)
        );
    }

    // Head of each animating piston, and for a while after it stops: did it vanish, or draw at two places?
    {
        // Level tick at the start of this frame; -1 without a clock, when every piston counts as running.
        auto const tick    = gFrameLt.load(std::memory_order_relaxed);
        auto const running = [tick](PistonVisual const& visual) {
            return tick < 0 || core::segmentRunning(visual.current, tick)
                || (visual.previous && core::segmentRunning(*visual.previous, tick));
        };
        for (auto const& [pos, visual] : gPistonVisuals) {
            if (!running(visual)) continue;
            auto& cell           = gProbeArmCells[pos];
            cell.action          = visual.action;
            cell.lastActiveFrame = logFrame;
        }
        ProbeArm const kNoArm{};
        for (auto it = gProbeArmCells.begin(); it != gProbeArmCells.end();) {
            auto const& pos  = it->first;
            auto&       cell = it->second;
            auto const  seen = gProbeArm.find(pos);
            auto const& arm  = seen != gProbeArm.end() ? seen->second : kNoArm;

            bool const active = cell.lastActiveFrame == logFrame;
            bool const tail   = !active && logFrame - cell.lastActiveFrame <= 20;
            // Length of the gap just closed: frames the head was missing before it drew again.
            if (arm.drawn > 0 && cell.gapStart >= 0) {
                auto const len = logFrame - cell.gapStart;
                gProbe.armGapLen[len <= 1 ? 0 : len == 2 ? 1 : len <= 5 ? 2 : len <= 20 ? 3 : 4].fetch_add(
                    1,
                    std::memory_order_relaxed
                );
                cell.gapStart = -1;
            }
            if (active || tail) {
                if (arm.drawn == 0 && cell.lastDrawnFrame == logFrame - 1) {
                    gProbe.armGap.fetch_add(1, std::memory_order_relaxed);
                    cell.gapStart = logFrame;
                    if (active) {
                        // Gone while the head still had a way to go: visible as a vanishing head, not a settling one.
                        if (auto const visual = gPistonVisuals.find(pos); visual != gPistonVisuals.end()) {
                            auto const left = std::fabs(
                                armValue(visual->second, gLastVisualTime.load(std::memory_order_relaxed))
                                - visual->second.current.to
                            );
                            if (left > 0.05f) gProbe.armGapMid.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (logFrame - cell.lastLogFrame >= 20) {
                        cell.lastLogFrame = logFrame;
                        probeLineLocked(
                            "armGap piston=%d,%d,%d act=%llu calls=%d skipped=%d nested=%d cargo=%d state=%s\n",
                            pos.x,
                            pos.y,
                            pos.z,
                            static_cast<unsigned long long>(cell.action),
                            arm.calls,
                            arm.skipped,
                            arm.nested,
                            arm.cargo,
                            active ? "running" : "tail"
                        );
                    }
                }
                if (arm.drawn >= 2 && arm.spread) {
                    gProbe.armDouble.fetch_add(1, std::memory_order_relaxed);
                    if (logFrame - cell.lastDoubleFrame >= 20) {
                        cell.lastDoubleFrame = logFrame;
                        probeLineLocked(
                            "armDouble piston=%d,%d,%d act=%llu drawn=%d nested=%d cargo=%d\n",
                            pos.x,
                            pos.y,
                            pos.z,
                            static_cast<unsigned long long>(cell.action),
                            arm.drawn,
                            arm.nested,
                            arm.cargo
                        );
                    }
                }
            }
            if (arm.drawn > 0) cell.lastDrawnFrame = logFrame;
            if (!gPistonVisuals.contains(pos) && logFrame - std::max(cell.lastDrawnFrame, cell.lastActiveFrame) > 100)
                it = gProbeArmCells.erase(it);
            else ++it;
        }
        gProbeArm.clear();
    }
    // The visual clock against the wall clock: does the proxy time advance by what really passed?
    if (gProbeDrawSeen) {
        if (gProbeClkValid) {
            auto const wall     = std::chrono::duration<double>(gProbeDrawWall - gProbeClkWall).count();
            auto const advance  = gProbeDrawTime - gProbeClkTime;
            auto const expected = wall * gProbeClkRate;
            if (wall > 0.0 && wall < 0.25) {
                gProbe.clkChecks.fetch_add(1, std::memory_order_relaxed);
                auto const err = std::fabs(advance - expected);
                int const  bin = err < 0.02 ? 0 : err < 0.05 ? 1 : err < 0.1 ? 2 : err < 0.25 ? 3 : err < 0.5 ? 4 : err < 0.9 ? 5 : 6;
                gProbe.clkErr[bin].fetch_add(1, std::memory_order_relaxed);
                bool const back  = advance < -0.001;
                bool const stall = !back && advance < 0.001 && expected > 0.05;
                if (back) gProbe.clkBack.fetch_add(1, std::memory_order_relaxed);
                if (stall) gProbe.clkStall.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<uint64_t> used{0};
                if ((back || stall || err >= 0.5) && probeBudget(used, 200, "clkStep"))
                    probeLineLocked(
                        "clkStep kind=%s advance=%.3f expected=%.3f wallMs=%.2f rate=%.2f time=%.3f lt=%lld\n",
                        back ? "back" : stall ? "stall" : "err",
                        advance,
                        expected,
                        wall * 1000.0,
                        gProbeClkRate,
                        gProbeDrawTime,
                        static_cast<long long>(gFrameLt.load(std::memory_order_relaxed))
                    );
            }
        }
        gProbeClkValid = true;
        gProbeClkWall  = gProbeDrawWall;
        gProbeClkTime  = gProbeDrawTime;
        gProbeClkRate  = gTicksPerSecond.load(std::memory_order_relaxed);
    }
    gProbeDrawSeen = false;
    gProbeTally.clear();
    probeFlushLocked();
}

// Render-thread samples of the current window: our work in a frame, and the real time since the previous frame.
thread_local std::vector<uint32_t>                 tWorkSamples;
thread_local std::vector<uint32_t>                 tIntervalSamples;
thread_local std::chrono::steady_clock::time_point tPrevFrameEnd{};
thread_local uint32_t                              tHitchLines  = 0;
constexpr uint64_t                                 kHitchWorkUs = 1000;

// Frame thread. When no render-chunk build starts although our re-mesh requests keep coming, drop a marker file for an
// external stack dump of the stalled pipeline.
void probeFreezeWatch(std::chrono::steady_clock::time_point now) {
    static uint64_t                              lastStarts = 0;
    static uint64_t                              askAtStart = 0;
    static std::chrono::steady_clock::time_point startedAt  = now;
    static bool                                  marked     = false;
    auto const                                   starts     = gProbe.buildStarts.load(std::memory_order_relaxed);
    auto const                                   asked      = gProbe.askCells.load(std::memory_order_relaxed);
    if (starts != lastStarts) {
        lastStarts = starts;
        askAtStart = asked;
        startedAt  = now;
        marked     = false;
        return;
    }
    auto const idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt).count();
    if (marked || asked - askAtStart < 200 || idleMs < 1500) return;
    marked               = true;
    std::size_t inFlight = 0;
    {
        std::lock_guard const guard(gFlightMutex);
        inFlight = gBuildsInFlight.size();
    }
    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "freeze idleMs=%lld starts=%llu commits=%llu inFlight=%zu askCells=%llu askedSince=%llu\n",
        static_cast<long long>(idleMs),
        static_cast<unsigned long long>(starts),
        static_cast<unsigned long long>(gProbe.buildCommits.load(std::memory_order_relaxed)),
        inFlight,
        static_cast<unsigned long long>(asked),
        static_cast<unsigned long long>(asked - askAtStart)
    );
    {
        std::lock_guard const guard(gProbeMutex);
        probeLineLocked("%s", line);
    }
    if (auto* const mod = ll::mod::NativeMod::current().get()) {
        std::FILE* marker = nullptr;
        if (fopen_s(&marker, (mod->getModDir() / "freeze.flag").string().c_str(), "wb") == 0 && marker) {
            std::fputs(line, marker);
            std::fclose(marker);
        }
    }
}
// Called once per real frame with that frame's work. Frames over kHitchWorkUs get a line naming the scopes behind them.
void probeFrameSample(uint64_t workTicks, std::chrono::steady_clock::time_point now) {
    probeFreezeWatch(now);
    auto const frequency  = std::max<uint64_t>(perfFrequency(), 1);
    auto const workUs     = workTicks * 1000000ull / frequency;
    uint64_t   intervalUs = 0;
    if (tPrevFrameEnd != std::chrono::steady_clock::time_point{})
        intervalUs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - tPrevFrameEnd).count());
    tPrevFrameEnd = now;
    if (intervalUs > 0 && intervalUs < 500000 && tIntervalSamples.size() < 8192) {
        tWorkSamples.push_back(static_cast<uint32_t>(workUs));
        tIntervalSamples.push_back(static_cast<uint32_t>(intervalUs));
    }
    if (workUs >= kHitchWorkUs && tHitchLines < 40) {
        ++tHitchLines;
        constexpr int count = static_cast<int>(PerfId::Count);
        bool          used[count]{};
        char          top[320];
        std::size_t   length = 0;
        top[0]               = '\0';
        for (int rank = 0; rank < 6; ++rank) {
            int best = -1;
            for (int id = 0; id < count; ++id)
                if (!used[id] && (best < 0 || tFrameScopeTicks[id] > tFrameScopeTicks[best])) best = id;
            if (best < 0) break;
            used[best]    = true;
            auto const us = tFrameScopeTicks[best] * 1000000ull / frequency;
            if (us < 100) break;
            auto const written = std::snprintf(
                top + length,
                sizeof(top) - length,
                "%s%s:%llu/%u",
                length ? "," : "",
                kPerfNames[best],
                static_cast<unsigned long long>(us),
                tFrameScopeCalls[best]
            );
            if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(top) - length) break;
            length += static_cast<std::size_t>(written);
        }
        std::lock_guard const guard(gProbeMutex);
        probeLineLocked(
            "hitch workUs=%llu intervalUs=%llu top=%s\n",
            static_cast<unsigned long long>(workUs),
            static_cast<unsigned long long>(intervalUs),
            top
        );
    }
    std::fill(std::begin(tFrameScopeTicks), std::end(tFrameScopeTicks), 0ull);
    std::fill(std::begin(tFrameScopeCalls), std::end(tFrameScopeCalls), 0u);
}

void probeFlushArmTally() {
    auto const add = [](std::atomic<uint64_t>& slot, uint32_t& value) {
        slot.fetch_add(value, std::memory_order_relaxed);
        value = 0;
    };
    add(gPerfCounts.armCalls, tArmTally.calls);
    add(gPerfCounts.armArms, tArmTally.arms);
    add(gPerfCounts.armNested, tArmTally.nested);
    add(gPerfCounts.armTypeChecks, tArmTally.typeChecks);
    add(gPerfCounts.armSkipped, tArmTally.skipped);
    add(gPerfCounts.armOffsets, tArmTally.offsets);
    add(gPerfCounts.armShifted, tArmTally.shifted);
    gPerfCounts2.clockCalls.fetch_add(tClockCalls, std::memory_order_relaxed);
    gPerfCounts2.clockFrames.fetch_add(1, std::memory_order_relaxed);
    tClockCalls = 0;
}

uint32_t probePercentile(std::vector<uint32_t> const& sorted, double q) {
    if (sorted.empty()) return 0;
    auto const at = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
    return sorted[std::min(sorted.size() - 1, at)];
}

// Prints and resets the frame samples of the window. Caller holds gProbeMutex.
void probeFrameStatsLocked() {
    if (tIntervalSamples.empty()) return;
    auto        work        = tWorkSamples;
    auto        interval    = tIntervalSamples;
    uint64_t    workSum     = 0;
    uint64_t    intervalSum = 0;
    std::size_t over1       = 0;
    std::size_t over2       = 0;
    std::size_t over5       = 0;
    for (auto const value : work) {
        workSum += value;
        over1   += value >= 1000;
        over2   += value >= 2000;
        over5   += value >= 5000;
    }
    for (auto const value : interval) intervalSum += value;
    std::sort(work.begin(), work.end());
    std::sort(interval.begin(), interval.end());
    auto const medianInterval = probePercentile(interval, 0.5);
    // Frames that took over twice the median, and of those how many our own work made up a quarter of.
    std::size_t slow     = 0;
    std::size_t slowOurs = 0;
    for (std::size_t i = 0; i < tIntervalSamples.size(); ++i) {
        if (tIntervalSamples[i] <= medianInterval * 2) continue;
        ++slow;
        if (static_cast<uint64_t>(tWorkSamples[i]) * 4 >= tIntervalSamples[i]) ++slowOurs;
    }
    probeLineLocked(
        "frameStats frames=%zu workP50=%u workP95=%u workP99=%u workMax=%u over1ms=%zu over2ms=%zu over5ms=%zu "
        "intervalP50=%u intervalP95=%u intervalP99=%u intervalMax=%u slow=%zu slowOurs=%zu sharePct=%.2f "
        "hitchLines=%u\n",
        work.size(),
        probePercentile(work, 0.5),
        probePercentile(work, 0.95),
        probePercentile(work, 0.99),
        work.back(),
        over1,
        over2,
        over5,
        medianInterval,
        probePercentile(interval, 0.95),
        probePercentile(interval, 0.99),
        interval.back(),
        slow,
        slowOurs,
        intervalSum ? 100.0 * static_cast<double>(workSum) / static_cast<double>(intervalSum) : 0.0,
        tHitchLines
    );
    tWorkSamples.clear();
    tIntervalSamples.clear();
    tHitchLines = 0;
}

// Prints and resets the per-call counters of the window. Caller holds gProbeMutex.
void probeExtraLocked() {
    auto const take = [](std::atomic<uint64_t>& slot) {
        return static_cast<unsigned long long>(slot.exchange(0, std::memory_order_relaxed));
    };
    auto const samples = gPerfCounts.armSamples.exchange(0, std::memory_order_relaxed);
    double     stageNs[6]{};
    auto const frequency = static_cast<double>(std::max<uint64_t>(perfFrequency(), 1));
    for (int i = 0; i < 6; ++i) {
        auto const ticks = gPerfCounts.armStageTicks[i].exchange(0, std::memory_order_relaxed);
        stageNs[i] = samples ? static_cast<double>(ticks) * 1.0e9 / (frequency * static_cast<double>(samples)) : 0.0;
    }
    probeLineLocked(
        "perfx armCalls=%llu arms=%llu nested=%llu typeChecks=%llu skipped=%llu offsets=%llu shifted=%llu "
        "armSampled=%llu setupNs=%.0f cargoNs=%.0f electNs=%.0f noteNs=%.0f progressNs=%.0f offsetNs=%.0f "
        "scanKnown=%llu scanDup=%llu scanHits=%llu scanCreated=%llu scanFollowed=%llu bumps=%llu bumpTouch=%llu "
        "bumpState=%llu bumpKind=%llu bumpForget=%llu viewEntries=%llu\n",
        take(gPerfCounts.armCalls),
        take(gPerfCounts.armArms),
        take(gPerfCounts.armNested),
        take(gPerfCounts.armTypeChecks),
        take(gPerfCounts.armSkipped),
        take(gPerfCounts.armOffsets),
        take(gPerfCounts.armShifted),
        static_cast<unsigned long long>(samples),
        stageNs[0],
        stageNs[1],
        stageNs[2],
        stageNs[3],
        stageNs[4],
        stageNs[5],
        take(gPerfCounts.scanKnown),
        take(gPerfCounts.scanDuplicates),
        take(gPerfCounts.scanHits),
        take(gPerfCounts.scanCreated),
        take(gPerfCounts.scanFollowed),
        take(gPerfCounts.bumps),
        take(gPerfCounts.bumpTouch),
        take(gPerfCounts.bumpState),
        take(gPerfCounts.bumpKind),
        take(gPerfCounts.bumpForget),
        take(gPerfCounts.viewEntries)
    );
    for (int slot = 0; slot < kPerfSlots; ++slot) {
        auto const builds = gPerfCounts2.viewBy[slot].exchange(0, std::memory_order_relaxed);
        auto const ticks  = gPerfCounts2.viewByTicks[slot].exchange(0, std::memory_order_relaxed);
        auto const items  = gPerfCounts2.viewByEntries[slot].exchange(0, std::memory_order_relaxed);
        if (builds == 0) continue;
        probeLineLocked(
            "viewBy caller=%s builds=%llu us=%llu entries=%llu\n",
            slot == kPerfSlots - 1 ? "none" : kPerfNames[slot],
            static_cast<unsigned long long>(builds),
            static_cast<unsigned long long>(ticks * 1000000ull / static_cast<uint64_t>(frequency)),
            static_cast<unsigned long long>(items)
        );
    }
    probeLineLocked(
        "perf2 meshPubs=%llu meshPubEntries=%llu sweepCalls=%llu sweepIdle=%llu sweepLocked=%llu sweepAdvanced=%llu "
        "sweepCells=%llu rebuildCalls=%llu rebuildCells=%llu rebuildSubchunks=%llu stepCalls=%llu pruneRuns=%llu "
        "pruneScanned=%llu pruneErased=%llu clockCalls=%llu clockFrames=%llu scanWindow=%llu scanNoAttached=%llu "
        "scanAttSettled=%llu look=%llu/%llu/%llu/%llu hit=%llu/%llu/%llu/%llu hitLineOnly=%llu\n",
        take(gPerfCounts2.meshPubs),
        take(gPerfCounts2.meshPubEntries),
        take(gPerfCounts2.sweepCalls),
        take(gPerfCounts2.sweepIdle),
        take(gPerfCounts2.sweepLocked),
        take(gPerfCounts2.sweepAdvanced),
        take(gPerfCounts2.sweepCells),
        take(gPerfCounts2.rebuildCalls),
        take(gPerfCounts2.rebuildCells),
        take(gPerfCounts2.rebuildSubchunks),
        take(gPerfCounts2.stepCalls),
        take(gPerfCounts2.pruneRuns),
        take(gPerfCounts2.pruneScanned),
        take(gPerfCounts2.pruneErased),
        take(gPerfCounts2.clockCalls),
        take(gPerfCounts2.clockFrames),
        take(gPerfCounts2.scanWindow),
        take(gPerfCounts2.scanNoAttached),
        take(gPerfCounts2.scanAttSettled),
        take(gPerfCounts2.scanLookRole[0]),
        take(gPerfCounts2.scanLookRole[1]),
        take(gPerfCounts2.scanLookRole[2]),
        take(gPerfCounts2.scanLookRole[3]),
        take(gPerfCounts2.scanHitRole[0]),
        take(gPerfCounts2.scanHitRole[1]),
        take(gPerfCounts2.scanHitRole[2]),
        take(gPerfCounts2.scanHitRole[3]),
        take(gPerfCounts2.scanHitLineOnly)
    );
    // Sampled one call in kLightSample; ns per call, game function and our wrapper separately.
    for (int kind = 0; kind < 2; ++kind) {
        auto const seen = gPerfCounts2.lightSeen[kind].exchange(0, std::memory_order_relaxed);
        auto const game = gPerfCounts2.lightGameTicks[kind].exchange(0, std::memory_order_relaxed);
        auto const own  = gPerfCounts2.lightOwnTicks[kind].exchange(0, std::memory_order_relaxed);
        if (seen == 0) continue;
        auto const toNs = 1.0e9 / (frequency * static_cast<double>(seen));
        probeLineLocked(
            "light %s sampled=%llu gameNs=%.1f ownNs=%.1f\n",
            kind == 0 ? "render" : "other",
            static_cast<unsigned long long>(seen),
            static_cast<double>(game) * toNs,
            static_cast<double>(own) * toNs
        );
    }
}

// Every real frame on the render thread, after onFrameEnd returned so its Frame scope is in the work total.
void probeRealFrameEnd() {
    auto const now = std::chrono::steady_clock::now();
    if (tRenderThread) {
        perfAdd(gFrameWork, tWorkTicks);
        probeFrameSample(tWorkTicks, now);
        probeFlushArmTally();
    }
    tWorkTicks = 0;
    // Unlocked pre-check keeps the common frame cheap; re-checked under the lock. Only atomics are read below.
    if (now - gProbeLastSum < std::chrono::seconds(5)) return;
    std::lock_guard const guard(gProbeMutex);
    if (now - gProbeLastSum >= std::chrono::seconds(5)) {
        auto const seconds  = std::chrono::duration<double>(now - gProbeLastSum).count();
        gProbeLastSum       = now;
        auto const created  = gProbe.proxiesCreated.load();
        auto const released = gProbe.proxiesReleased.load();
        probeLineLocked(
            "sum frames=%llu proxiesCreated=%llu proxiesReleased=%llu proxiesAlive=%lld vanillaHidden=%llu "
            "proxyDraws=%llu zero=%llu double=%llu dispatchNoRender=%llu noDispatch=%llu meshGap=%llu meshEmpty=%llu "
            "meshRegress=%llu startGap=%llu snapshotMissing=%llu handoverShown=%llu handoverTimeout=%llu "
            "rbeOpaque=%llu rbeAlpha=%llu rbeMaxOpaque=%llu rbeMaxAlpha=%llu rbeMissingOpaque=%llu "
            "rbeMissingAlpha=%llu viewChecks=%llu viewMismatch=%llu viewBuilds=%llu meshPublishes=%llu "
            "armGap=%llu armDouble=%llu armSkipped=%llu snapDetached=%llu olderDropped=%llu replaced=%llu "
            "ghostDraws=%llu ghostInstances=%llu stuckCells=%llu doneWithProxy=%llu actorGap=%llu "
            "actorDouble=%llu carriedDone=%llu carriedTimeout=%llu leakMesh=%llu outerPassed=%llu outerHidden=%llu "
            "innerPassed=%llu innerHidden=%llu actorStripped=%llu\n",
            probeCount(gProbe.frames),
            static_cast<unsigned long long>(created),
            static_cast<unsigned long long>(released),
            static_cast<long long>(created) - static_cast<long long>(released),
            probeCount(gProbe.vanillaHidden),
            probeCount(gProbe.proxyDraws),
            probeCount(gProbe.zero),
            probeCount(gProbe.doubled),
            probeCount(gProbe.dispatchNoRender),
            probeCount(gProbe.noDispatch),
            probeCount(gProbe.meshGap),
            probeCount(gProbe.meshEmpty),
            probeCount(gProbe.meshRegress),
            probeCount(gProbe.startGap),
            probeCount(gProbe.snapshotMissing),
            probeCount(gProbe.handoverShown),
            probeCount(gProbe.handoverTimeout),
            probeCount(gProbe.rbeCalls[0]),
            probeCount(gProbe.rbeCalls[1]),
            probeCount(gProbe.rbeMax[0]),
            probeCount(gProbe.rbeMax[1]),
            probeCount(gProbe.rbeMissing[0]),
            probeCount(gProbe.rbeMissing[1]),
            probeCount(gProbeViewChecks),
            probeCount(gProbeViewMismatch),
            probeCount(gProbeViewBuilds),
            probeCount(gProbeMeshPublishes),
            probeCount(gProbe.armGap),
            probeCount(gProbe.armDouble),
            probeCount(gProbe.armSkipped),
            probeCount(gProbe.snapDetached),
            probeCount(gProbe.olderDropped),
            probeCount(gProbe.replaced),
            probeCount(gProbe.ghostDraws),
            probeCount(gProbe.ghostInstances),
            probeCount(gProbe.stuckCells),
            probeCount(gProbe.doneWithProxy),
            probeCount(gProbe.actorGap),
            probeCount(gProbe.actorDouble),
            probeCount(gProbe.carriedDone),
            probeCount(gProbe.carriedTimeout),
            probeCount(gProbe.leakMesh),
            probeCount(gProbe.outerPassed),
            probeCount(gProbe.outerHidden),
            probeCount(gProbe.innerPassed),
            probeCount(gProbe.innerHidden),
            probeCount(gProbe.actorStripped)
        );
        // Pre-arrival flicker counters (cumulative, like the sum line).
        probeLineLocked(
            "flicker animCellFrames=%llu zeroAnim=%llu unpositioned=%llu dupEpisodes=%llu dupMs=%llu "
            "dupMaxFrames=%llu dupVisFrames=%llu posChecks=%llu posJump=%llu posBack=%llu armGapMid=%llu "
            "armGapLen=%llu/%llu/%llu/%llu/%llu clkChecks=%llu clkBack=%llu clkStall=%llu clkSpread=%llu "
            "clkErr=%llu/%llu/%llu/%llu/%llu/%llu/%llu lightQueries=%llu lightFrames=%llu lightAnyDark=%llu "
            "lightSelfDark=%llu lightFlip=%llu lightSelfFixed=%llu lightSelfNoMem=%llu\n",
            probeCount(gProbe.animCellFrames),
            probeCount(gProbe.zeroAnim),
            probeCount(gProbe.unpositioned),
            probeCount(gProbe.dupEpisodes),
            probeCount(gProbe.dupMs),
            probeCount(gProbe.dupMaxFrames),
            probeCount(gProbe.dupVisFrames),
            probeCount(gProbe.posChecks),
            probeCount(gProbe.posJump),
            probeCount(gProbe.posBack),
            probeCount(gProbe.armGapMid),
            probeCount(gProbe.armGapLen[0]),
            probeCount(gProbe.armGapLen[1]),
            probeCount(gProbe.armGapLen[2]),
            probeCount(gProbe.armGapLen[3]),
            probeCount(gProbe.armGapLen[4]),
            probeCount(gProbe.clkChecks),
            probeCount(gProbe.clkBack),
            probeCount(gProbe.clkStall),
            probeCount(gProbe.clkSpread),
            probeCount(gProbe.clkErr[0]),
            probeCount(gProbe.clkErr[1]),
            probeCount(gProbe.clkErr[2]),
            probeCount(gProbe.clkErr[3]),
            probeCount(gProbe.clkErr[4]),
            probeCount(gProbe.clkErr[5]),
            probeCount(gProbe.clkErr[6]),
            probeCount(gProbe.lightQueries),
            probeCount(gProbe.lightFrames),
            probeCount(gProbe.lightAnyDark),
            probeCount(gProbe.lightSelfDark),
            probeCount(gProbe.lightFlip),
            probeCount(gProbe.lightSelfFixed),
            probeCount(gProbe.lightSelfNoMem)
        );
        probeLineLocked(
            "builds starts=%llu commits=%llu asks=%llu askCells=%llu approachHidden=%llu staleFlag=%llu "
            "staleAsk=%llu awaitFlag=%llu landReask=%llu cleanCommit=%llu\n",
            probeCount(gProbe.buildStarts),
            probeCount(gProbe.buildCommits),
            probeCount(gProbe.rebuildAsks),
            probeCount(gProbe.askCells),
            probeCount(gProbe.approachHidden),
            probeCount(gProbe.staleFlag),
            probeCount(gProbe.staleAsk),
            probeCount(gProbe.awaitFlag),
            probeCount(gProbe.landReask),
            probeCount(gProbe.cleanCommit)
        );
        probeLineLocked(
            "liveStats liveDouble=%llu doubled=%llu buildLat0_1=%llu lat2_3=%llu lat4_7=%llu lat8_15=%llu "
            "lat16_31=%llu "
            "lat32p=%llu\n",
            probeCount(gProbe.liveDouble),
            probeCount(gProbe.doubled),
            probeCount(gProbe.buildLat[0]),
            probeCount(gProbe.buildLat[1]),
            probeCount(gProbe.buildLat[2]),
            probeCount(gProbe.buildLat[3]),
            probeCount(gProbe.buildLat[4]),
            probeCount(gProbe.buildLat[5])
        );
        probeLineLocked(
            "clearStats started=%llu gap0_1=%llu gap2_3=%llu gap4_6=%llu gap7p=%llu never=%llu goneReturned=%llu "
            "goneCleared=%llu\n",
            probeCount(gProbeClearStarted),
            probeCount(gProbeClearGap[0]),
            probeCount(gProbeClearGap[1]),
            probeCount(gProbeClearGap[2]),
            probeCount(gProbeClearGap[3]),
            probeCount(gProbeClearGap[4]),
            probeCount(gProbeGone[0]),
            probeCount(gProbeGone[1])
        );
        std::size_t const headOwners = gProbeHeadOwners.load(std::memory_order_relaxed);
        std::size_t       stepped    = 0;
        {
            std::lock_guard const heads(gHeadMutex);
            stepped = gSteppedPistons.size();
        }
        probeLineLocked(
            "scale seconds=%.2f cells=%llu pistons=%llu actions=%llu headOwners=%zu stepped=%zu scanPistons=%llu "
            "scanLookups=%llu\n",
            seconds,
            static_cast<unsigned long long>(gProbe.cellsMax.exchange(0, std::memory_order_relaxed)),
            static_cast<unsigned long long>(gProbe.pistonsMax.exchange(0, std::memory_order_relaxed)),
            static_cast<unsigned long long>(gProbe.actionsMax.exchange(0, std::memory_order_relaxed)),
            headOwners,
            stepped,
            static_cast<unsigned long long>(gProbe.scanPistons.exchange(0, std::memory_order_relaxed)),
            static_cast<unsigned long long>(gProbe.scanLookups.exchange(0, std::memory_order_relaxed))
        );
        probePerfLocked(seconds);
        probeExtraLocked();
        probeFrameStatsLocked();
        probeRound33Locked();
    }
    probeFlushLocked();
}
#endif

// An external clock (e.g. a replay) wins; in a live world the client level tick runs 1:1 with redstone.
std::optional<core::ClockSample> sampleVisualClock() {
    OPTIPISTON_PROBE_DO(++tClockCalls;)
    if (auto external = core::externalClock().sample()) return external;
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return std::nullopt;
    return core::ClockSample{static_cast<int64_t>(player->getLevel().getCurrentTick().tickID), 0, false};
}

// Inside a render pass the clock is sampled once and shared by every arm of the pass.
std::optional<core::ClockSample> visualClock() {
    if (tRenderScope) return tRenderScope->clock(sampleVisualClock);
    return sampleVisualClock();
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

// Where the proxy of this cell is drawn, relative to the cell. Caller holds gAnimMutex.
::Vec3 cellOffset(PushCell const& rec, double time) {
    auto const state = rec.timeline.state();
    if (state != PushState::Animating && state != PushState::Landing) return ::Vec3{0.0f, 0.0f, 0.0f};
    auto offset = actionOffset(rec.visual, time);
    if (rec.carry) {
        auto const tail  = actionOffset(rec.carry->visual, time);
        offset.x        += tail.x;
        offset.y        += tail.y;
        offset.z        += tail.z;
    }
    return offset;
}

// Caller holds gAnimMutex. The proxy is destroyed by whoever drops `drop`, after the lock is gone.
void takeProxy(PushCell& rec, Graveyard& drop) {
    if (!rec.proxy) return;
    drop.push_back(std::move(rec.proxy));
    rec.proxy = nullptr;
    bumpLocked();
    OPTIPISTON_PROBE_DO(gProbe.proxiesReleased.fetch_add(1, std::memory_order_relaxed);)
}

// Returns cells whose mesh is still missing the landed block.
std::vector<::BlockPos> clearAnimationStateLocked(Graveyard& drop) {
    std::vector<::BlockPos> cells;
    for (auto& [pos, rec] : gCells) {
        if (rec.timeline.meshSuppressed() || rec.timeline.awaitingMesh()) cells.push_back(pos);
        takeProxy(rec, drop);
    }
    gCells.clear();
    gReplacedTails.clear();
    gPistonVisuals.clear();
    gActionVisuals.clear();
    gPendingRemesh.clear();
    gPendingWatch.store(false, std::memory_order_relaxed);
    {
        std::lock_guard const early(gEarlyMutex);
        gEarlyRemesh.clear();
    }
    gReleasedMesh.clear();
    gStaleMesh.clear();
    gAwaitPublished.clear();
    gClockTracker.reset();
    gMeshBuilder.clear();
    gMeshDirty = true;
    gCensus.clear();
    bumpLocked();
    settleLocked(drop);
    return cells;
}

void clearAnimationState() {
    // Declared first, so the proxies die after the lock is released.
    Graveyard             drop;
    std::lock_guard const guard(gAnimMutex);
    clearAnimationStateLocked(drop);
}

// Caller holds gAnimMutex. The action's cells keep their own copy of the segment and play it to the end.
void endActionLocked(uint64_t action) {
    gActionVisuals.erase(action);
    bumpLocked();
}

// Set while our own re-mesh requests pass through the level listeners.
thread_local bool tImmediateRebuild = false;

#if OPTIPISTON_PROBE
// Cells per call, and how many distinct subchunks their +-1 boxes touch: the ceiling for merging asks per subchunk.
void probeRebuildShape(std::vector<::BlockPos> const& cells) {
    if (cells.empty()) return;
    thread_local std::vector<std::uint64_t> chunks;
    chunks.clear();
    for (auto const& pos : cells)
        for (int corner = 0; corner < 8; ++corner) {
            auto const x = (pos.x + ((corner & 1) ? 1 : -1)) >> 4;
            auto const y = (pos.y + ((corner & 2) ? 1 : -1)) >> 4;
            auto const z = (pos.z + ((corner & 4) ? 1 : -1)) >> 4;
            chunks.push_back(
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0x1FFFFF) << 42)
                | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1FFFFF) << 21)
                | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1FFFFF))
            );
        }
    std::sort(chunks.begin(), chunks.end());
    auto const distinct = static_cast<std::uint64_t>(std::unique(chunks.begin(), chunks.end()) - chunks.begin());
    gPerfCounts2.rebuildCalls.fetch_add(1, std::memory_order_relaxed);
    gPerfCounts2.rebuildCells.fetch_add(cells.size(), std::memory_order_relaxed);
    gPerfCounts2.rebuildSubchunks.fetch_add(distinct, std::memory_order_relaxed);
}
#endif

void rebuildCells(::BlockSource& region, std::vector<::BlockPos> const& cells) {
    OPTIPISTON_PROBE_DO(if (!cells.empty()) {
        gProbe.rebuildAsks.fetch_add(1, std::memory_order_relaxed);
        gProbe.askCells.fetch_add(cells.size(), std::memory_order_relaxed);
    })
    OPTIPISTON_PROBE_DO(probeRebuildShape(cells);)
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

bool hasAnimationStateLocked() { return !gPistonVisuals.empty() || !gActionVisuals.empty() || !gCells.empty(); }

// Caller holds gAnimMutex. Finished cells give up their proxy at once and keep hiding the game's own instance for a
// while, so a lingering MovingBlock cannot draw a second copy.
void reapLocked(int64_t tick, Graveyard& drop) {
    // Only finished cells have anything to reap.
    if (!gCensus.any(core::TraitDone)) return;
    for (auto it = gCells.begin(); it != gCells.end();) {
        auto& rec = it->second;
        if (rec.timeline.state() != PushState::Done) {
            ++it;
            continue;
        }
        takeProxy(rec, drop);
        if (rec.doneTick < 0) {
            rec.doneTick = tick;
            OPTIPISTON_PROBE_DO(if (!rec.timeline.superseded()) {
                if (rec.timeline.landed()) probeLandDelay(rec.timeline.landDelay(), rec.probeSawAir);
                else probeOverrunEnd(it->first, rec);
            })
            OPTIPISTON_PROBE_DO(
                if (rec.timeline.handoverEnd() == core::HandoverEnd::ActorSeen)
                    gProbe.handoverShown.fetch_add(1, std::memory_order_relaxed);
                else if (rec.timeline.handoverEnd() == core::HandoverEnd::Timeout)
                    gProbe.handoverTimeout.fetch_add(1, std::memory_order_relaxed);
            )
        }
        if (!gActionVisuals.contains(rec.action) && tick - rec.doneTick >= kTombstoneTicks) {
            forgetCellLocked(it->first, rec);
            it = gCells.erase(it);
        } else {
            ++it;
        }
    }
}

// Caller holds gAnimMutex. Moves every visual and timeline to `tick`; cells that need a re-mesh are added to `cells`.
void advanceTickLocked(std::vector<::BlockPos>& cells, Graveyard& drop, ::BlockSource& region) {
    auto const tick = visualClockTick().value_or(0);
    if (!gClockTracker.advance(tick)) return;
    OPTIPISTON_PROBE_DO(gPerfCounts2.sweepAdvanced.fetch_add(1, std::memory_order_relaxed);)
    gLastTick.store(tick, std::memory_order_relaxed);
    foldActorNotesLocked();
    auto const expired = [tick](VisualSegment const& segment) { return core::segmentExpired(segment, tick); };
    if (std::erase_if(gPistonVisuals, [&](auto const& entry) { return expired(entry.second.current); }) != 0)
        bumpLocked();
    std::vector<uint64_t> ended;
    for (auto const& [action, visual] : gActionVisuals)
        if (expired(visual.segment)) ended.push_back(action);
    for (auto const action : ended) endActionLocked(action);
    auto const visualTimeNow = gLastVisualTime.load(std::memory_order_relaxed);
    bool       handover      = false;
    for (auto& [pos, rec] : gCells) {
        // The landing packet and the placed-cell check can both miss; the world is the final word.
        if (rec.timeline.awaitsLanding(tick)) {
            auto const& block = region.getBlock(pos);
            // The cell is empty for a moment before the real block arrives, so only a block ends the wait.
            if (block.isAir()) {
                OPTIPISTON_PROBE_DO(rec.probeSawAir = true;)
            } else if (block.getTypeName() != "minecraft:moving_block") {
                rec.timeline.blockLanded();
                OPTIPISTON_PROBE_DO(
                    probeLandPoll(pos, block.getTypeName(), rec.wrapped ? *rec.wrapped : std::string{});
                )
            }
        }
        if (rec.timeline.tick(tick, visualTimeNow)) {
            cells.push_back(pos);
            OPTIPISTON_PROBE_DO(probeRemeshRequest(pos, rec, 2);)
        }
        // Only a handover's deadline reads the timeline's tick; every other change shows up as a state change.
        syncCellLocked(pos, rec, false);
        handover = handover || rec.timeline.state() == PushState::Handover;
    }
    if (handover) bumpLocked();
    reapLocked(tick, drop);
    settleLocked(drop);
}

// Ends visuals past their duration, or ahead of the clock after a backward seek.
void sweepActions(::BlockSource& region) {
    OPTIPISTON_PROBE_DO(gPerfCounts2.sweepCalls.fetch_add(1, std::memory_order_relaxed);)
    // Nothing to end, re-mesh or free in an idle world.
    if (!gStateWatch.load(std::memory_order_relaxed) && !gPendingWatch.load(std::memory_order_relaxed)) {
        OPTIPISTON_PROBE_DO(gPerfCounts2.sweepIdle.fetch_add(1, std::memory_order_relaxed);)
        return;
    }
    // Declared first, so the proxies die after the lock is released.
    Graveyard               drop;
    std::vector<::BlockPos> cleared;
    std::vector<::BlockPos> pending;
    std::vector<::BlockPos> ticked;
    {
        std::lock_guard const guard(gAnimMutex);
        OPTIPISTON_PROBE_DO(gPerfCounts2.sweepLocked.fetch_add(1, std::memory_order_relaxed);)
        bool const            active = animationActive();
        if ((!active || clockInvalidatedLocked()) && hasAnimationStateLocked())
            cleared = clearAnimationStateLocked(drop);
        if (cleared.empty()) {
            pending.swap(gPendingRemesh);
            gPendingWatch.store(false, std::memory_order_relaxed);
            if (animationActive()) advanceTickLocked(ticked, drop, region);
        }
    }
    OPTIPISTON_PROBE_DO(
        gPerfCounts2.sweepCells.fetch_add(cleared.size() + pending.size() + ticked.size(), std::memory_order_relaxed);
    )
    if (!cleared.empty()) {
        rebuildCells(region, cleared);
        return;
    }
    rebuildCells(region, pending);
    rebuildCells(region, ticked);
}

// Worker threads build chunk meshes; the MovingBlock renderer tessellates on the render thread and must pass.
// A cell a mesh build tessellated as the real block after its re-mesh request. Counts only once the build is
// uploaded; a superseded build never reaches endRebuild.
void releasedToMesher(::BlockPos const& pos) {
    if (tBuild) {
        tBuild->released(pos);
        return;
    }
    if (!tMeshGeometry) return;
    std::lock_guard const guard(gAnimMutex);
    gReleasedMesh[tMeshGeometry].push_back(pos);
}

// A cell met as awaiting whose chunk copy still holds the stand-in or air: this build lacks the block.
void staleToMesher(::BlockPos const& pos) {
    if (tBuild) {
        tBuild->stale(pos);
        return;
    }
    if (!tMeshGeometry) return;
    std::lock_guard const guard(gAnimMutex);
    gStaleMesh[tMeshGeometry].push_back(pos);
}

// The mesh view says what to do for the cell; `kind` comes from one view, so the answer is self-consistent.
bool meshSuppressedIn(core::MeshView const& view, ::BlockPos const& pos, bool& released) {
    auto const kind = view.at(cellOf(pos));
    released        = kind == core::MeshCell::Awaiting;
    return kind == core::MeshCell::Landed;
}

#if OPTIPISTON_PROBE
void probeCheckMesh(::BlockPos const& pos);
#endif

// Worker threads build chunk meshes; the MovingBlock renderer tessellates on the render thread and must pass.
bool meshSuppressed(::BlockPos const& pos, ::Block const& block) {
    if (tRenderDepth > 0 || !gMeshWatch.load(std::memory_order_relaxed)) return false;
    MeshViewPin pin;
    bool        released = false;
    OPTIPISTON_PROBE_DO(probeCheckMesh(pos);)
    if (meshSuppressedIn(pin.get(), pos, released)) return true;
    if (!released) {
        // The world gets the real block before the visual ends; the proxy draws it until the landing re-mesh.
        if (pin.get().at(cellOf(pos)) != core::MeshCell::Approaching || block.isAir()
            || block.getTypeName() == "minecraft:moving_block")
            return false;
        OPTIPISTON_PROBE_DO(gProbe.approachHidden.fetch_add(1, std::memory_order_relaxed);)
        return true;
    }
    // A build working from a chunk copy older than the landing still holds the stand-in and lacks the block.
    if (block.isAir() || block.getTypeName() == "minecraft:moving_block") {
        OPTIPISTON_PROBE_DO(probeStaleRelease(pos, block);)
        staleToMesher(pos);
        return false;
    }
    releasedToMesher(pos);
    return false;
}

// A render chunk is one subchunk wide.
bool chunkHolds(::BlockPos const& origin, ::BlockPos const& cell) {
    constexpr int kEdge = 16;
    return cell.x >= origin.x && cell.x < origin.x + kEdge && cell.y >= origin.y && cell.y < origin.y + kEdge
        && cell.z >= origin.z && cell.z < origin.z + kEdge;
}

// Caller holds gAnimMutex. A build that started before the cell existed may have baked its old block into the mesh.
void flagStaleRiskLocked(::BlockPos const& cell, [[maybe_unused]] bool awaiting) {
    std::lock_guard const guard(gFlightMutex);
    for (auto const& [geometry, origin] : gBuildsInFlight) {
        if (!chunkHolds(origin, cell)) continue;
        gStaleRisk[geometry].push_back(cell);
        OPTIPISTON_PROBE_DO((awaiting ? gProbe.awaitFlag : gProbe.staleFlag).fetch_add(1, std::memory_order_relaxed);)
    }
}

// A newer build of the same geometry replaces the pending one.
void meshBuildStarted(::RenderChunkGeometry const* geometry) {
    OPTIPISTON_PERF_SCOPE(perf, MeshBuild);
    OPTIPISTON_PROBE_DO(probeMeshBuildStarted(geometry);)
    if (geometry) {
        // The new build sees every cell that exists now.
        std::lock_guard const guard(gFlightMutex);
        if (gBuildsInFlight.size() >= 4096) {
            gBuildsInFlight.clear();
            gStaleRisk.clear();
        }
        gBuildsInFlight[geometry] = geometry->mPosition.get();
        gStaleRisk.erase(geometry);
    }
    // Entries exist only while a cell awaits its mesh.
    if (!gMeshWatch.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gAnimMutex);
    gReleasedMesh.erase(geometry);
    gStaleMesh.erase(geometry);
}

// Runs on the frame thread once the rebuilt geometry is live.
void meshBuildCommitted(::RenderChunkGeometry const* geometry) {
    OPTIPISTON_PERF_SCOPE(perf, MeshBuild);
    OPTIPISTON_PROBE_DO(probeMeshCommitted(geometry);)
    std::vector<::BlockPos> risky;
    ::BlockPos              origin{};
    bool                    tracked = false;
    {
        std::lock_guard const guard(gFlightMutex);
        if (auto const flight = gBuildsInFlight.find(geometry); flight != gBuildsInFlight.end()) {
            origin  = flight->second;
            tracked = true;
            gBuildsInFlight.erase(flight);
        }
        if (auto const found = gStaleRisk.find(geometry); found != gStaleRisk.end()) {
            risky = std::move(found->second);
            gStaleRisk.erase(found);
        }
    }
    if (!risky.empty()) {
        // Cells created while this build ran may still be in it; the mesh is live now, so ask for a fresh one.
        std::lock_guard const guard(gAnimMutex);
        for (auto const& pos : risky) {
            auto const found = gCells.find(pos);
            if (found == gCells.end() || found->second.timeline.state() != PushState::Animating
                || found->second.timeline.landed())
                continue;
            gPendingRemesh.push_back(pos);
            OPTIPISTON_PROBE_DO(gProbe.staleAsk.fetch_add(1, std::memory_order_relaxed);)
        }
        gPendingWatch.store(!gPendingRemesh.empty(), std::memory_order_relaxed);
    }
    if (!gMeshWatch.load(std::memory_order_relaxed)) return;
    std::lock_guard const   guard(gAnimMutex);
    std::vector<::BlockPos> released;
    std::vector<::BlockPos> staleSeen;
    if (auto const found = gReleasedMesh.find(geometry); found != gReleasedMesh.end()) {
        released = std::move(found->second);
        gReleasedMesh.erase(found);
    }
    if (auto const found = gStaleMesh.find(geometry); found != gStaleMesh.end()) {
        staleSeen = std::move(found->second);
        gStaleMesh.erase(found);
    }
    auto const frame = gFrame.load(std::memory_order_relaxed);
    for (auto const& pos : released) {
        auto const found = gCells.find(pos);
        if (found == gCells.end() || !found->second.timeline.awaitingMesh()) {
            // The build released the block of an older action; a newer one now animates here and the mesh is stale.
            if (found != gCells.end() && found->second.timeline.state() == PushState::Animating
                && !found->second.timeline.landed()) {
                gPendingRemesh.push_back(pos);
                gPendingWatch.store(true, std::memory_order_relaxed);
                OPTIPISTON_PROBE_DO(probeStaleCommit(pos, found->second.action);)
            }
            continue;
        }
        auto& timeline = found->second.timeline;
        timeline.meshCommitted(frame);
        // Shown from the next frame on, hence the extra step.
        auto const step = gFrameStep.load(std::memory_order_relaxed);
        auto const rate = gTicksPerSecond.load(std::memory_order_relaxed);
        auto const seconds =
            rate > 0.0 ? (gLastVisualTime.load(std::memory_order_relaxed) - timeline.remeshTime() + step) / rate : 0.0;
        OPTIPISTON_PROBE_DO(probeMeshCommit(
                                found->first,
                                found->second,
                                step,
                                seconds / std::max(gFrameSeconds.load(std::memory_order_relaxed), 1.0e-6)
        );)
        if (timeline.remeshTime() > 0.0 && seconds > 0.0 && seconds < kMaxMeshLatencySeconds) {
            auto const estimate = gMeshLatency.load(std::memory_order_relaxed);
            gMeshLatency.store(estimate + (seconds - estimate) * 0.25, std::memory_order_relaxed);
        }
        syncCellLocked(found->first, found->second, true);
    }
    // Cells of this chunk still awaiting a mesh were not tessellated as real blocks by this build. The wait ends
    // here instead of on a timer: a build that started after the awaiting state was published and never visited
    // the cell has nothing to show (the mesher skips it), so it is live; a build that may have started before,
    // or that met a stale chunk copy, cannot tell, so the platform asks for a fresh one.
    if (tracked && gCensus.any(core::TraitAwaiting)) {
        bool again = false;
        for (auto& [cell, rec] : gCells) {
            if (!rec.timeline.awaitingMesh() || !chunkHolds(origin, cell)) continue;
            bool const unsure = std::find(risky.begin(), risky.end(), cell) != risky.end()
                             || std::find(staleSeen.begin(), staleSeen.end(), cell) != staleSeen.end();
            if (unsure) {
                gPendingRemesh.push_back(cell);
                again = true;
                OPTIPISTON_PROBE_DO(gProbe.landReask.fetch_add(1, std::memory_order_relaxed);)
                continue;
            }
            rec.timeline.meshCommitted(frame);
            OPTIPISTON_PROBE_DO(gProbe.cleanCommit.fetch_add(1, std::memory_order_relaxed);)
            syncCellLocked(cell, rec, true);
        }
        if (again) gPendingWatch.store(true, std::memory_order_relaxed);
    }
    flushLocked();
}

std::string blockNameOf(uint runtimeId) {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return {};
    return player->getLevel().getBlockPalette().getBlock(runtimeId).getTypeName();
}

// Keeps the copy of the piston's attached blocks current. Runs on the piston's own tick.
void refreshAttached(::BlockPos const& pistonPos, std::vector<::BlockPos> const& attached) {
    if (attached.empty() || !gStateWatch.load(std::memory_order_relaxed)) return;
    {
        // Unchanged is the common case and needs no lock.
        ViewPin     pin;
        auto const& pistons = pin.get().pistons;
        auto const  found   = pistons.find(pistonPos);
        if (found == pistons.end() || (found->second.attached && *found->second.attached == attached)) return;
    }
    std::shared_ptr<std::vector<::BlockPos> const> fresh = std::make_shared<std::vector<::BlockPos>>(attached);
    // Declared first, so the replaced vector dies after the lock is released.
    Graveyard             drop;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gPistonVisuals.find(pistonPos);
    if (it == gPistonVisuals.end() || (it->second.attached && *it->second.attached == attached)) return;
    drop.push_back(std::move(it->second.attached));
    it->second.attached = std::move(fresh);
    bumpLocked();
}

// startTick is the recorded tick of a transition the client applied late; otherwise the action starts now.
void startAction(
    ::BlockSource&          region,
    ::BlockPos const&       pistonPos,
    ::BlockPos const&       facing,
    bool                    extending,
    std::vector<::BlockPos> attached,
    std::optional<int64_t>  startTick = std::nullopt
) {
    // Declared first, so the proxies die after the lock is released.
    Graveyard               drop;
    std::vector<::BlockPos> cells;
    {
        std::unique_lock lock(gAnimMutex);
        // A packet can arrive before the first sweep on the new clock.
        if (clockInvalidatedLocked()) cells = clearAnimationStateLocked(drop);
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
        // The sweep that keeps the tick current is skipped while nothing is animating.
        gLastTick.store(now, std::memory_order_relaxed);
        visual.scanUntil = std::max(now, tick) + kScanTicks;
        visual.attached  = std::make_shared<std::vector<::BlockPos>>(std::move(attached));
        OPTIPISTON_PROBE_DO(visual.startFrame = gRealFrame.load(std::memory_order_relaxed);)
        if (core::segmentExpired(visual.current, now)) {
            lock.unlock();
            rebuildCells(region, cells);
            return;
        }
        if (auto it = gPistonVisuals.find(pistonPos); it != gPistonVisuals.end() && !it->second.vacated) {
            auto const& old     = it->second;
            bool const  running = core::segmentRunning(old.current, tick);
            // A piston cannot start the same direction twice, so this is a resent packet, not a new action.
            if (running && old.current.to == to) return;
            // Chain policy: the old action's remaining tail plays on top of the new one.
            if (running) visual.previous = old.current;
            endActionLocked(old.action);
        }
        // This piston's body may still be arriving from a push; its arm must follow that motion until it ends.
        if (auto const body = gCells.find(pistonPos); body != gCells.end() && !body->second.timeline.superseded())
            visual.bodyCarry = body->second.visual;
        gActionVisuals[visual.action] = {visual.current, facing};
        gPistonVisuals[pistonPos]     = visual;
        bumpLocked();
        settleLocked(drop);
    }
    rebuildCells(region, cells);
}

// ---- Answers over one set of tables: the wrappers below feed them the published render view ----

// The visual time of a call, read from the clock on first use and then fixed, so the same call can be answered twice.
struct LazyTime {
    float                         alpha;
    mutable std::optional<double> value;
    [[nodiscard]] double          get() const {
        if (!value) value = visualTime(alpha);
        return *value;
    }
};

std::optional<float> armProgressIn(StateRef const& state, ::BlockPos const& pistonPos, LazyTime const& time) {
    auto const it = state.pistons.find(pistonPos);
    if (it == state.pistons.end() || it->second.vacated) return std::nullopt;
    return armValue(it->second, time.get());
}

std::optional<::Vec3> bodyOffsetIn(StateRef const& state, ::BlockPos const& pistonPos, LazyTime const& time) {
    auto const it = state.pistons.find(pistonPos);
    if (it == state.pistons.end() || it->second.vacated || !it->second.bodyCarry) return std::nullopt;
    return actionOffset(*it->second.bodyCarry, time.get());
}

std::optional<::Vec3>
landedOffsetIn(StateRef const& state, ::BlockPos const& pistonPos, LazyTime const& time, uint64_t frame) {
    auto const found = state.cells.find(pistonPos);
    if (found == state.cells.end()) return std::nullopt;
    auto const& rec = found->second;
    if (rec.timeline.superseded() || drawerOf(rec, frame) != PushDrawer::Proxy) return std::nullopt;
    return cellOffset(rec, time.get());
}

std::optional<::Vec3> drawOffsetIn(StateRef const& state, ::MovingBlockActor const& moving, LazyTime const& time) {
    auto const found = state.cells.find(moving.mPosition.get());
    if (found == state.cells.end() || found->second.proxy.get() != &moving) return std::nullopt;
    return cellOffset(found->second, time.get());
}

// The game's instance of a managed cell is never drawn while our timeline owns the cell.
bool vanillaHiddenIn(
    StateRef const&   state,
    ::BlockPos const& cell,
    ::BlockPos const& movingPiston,
    uint64_t          frame,
    bool              attached
) {
    auto const found = state.cells.find(cell);
    if (found == state.cells.end()) return false;
    auto const& rec = found->second;
    // The action of the piston that owns the cell, 0 if it has none running.
    OPTIPISTON_PROBE_DO(auto const pistonAction = [&](::BlockPos const& at) -> uint64_t {
        auto const it = state.pistons.find(at);
        return it == state.pistons.end() ? 0 : it->second.action;
    };)
    // A detached instance is an older push of this cell; the block has moved on and the timeline owns the picture.
    if (!attached) {
        OPTIPISTON_PROBE_DO(tProbeWhy = {4, rec.action, pistonAction(movingPiston), rec.timeline.state()};)
        return true;
    }
    if (rec.pistonPos != movingPiston) {
        // An instance naming no tracked piston has no data yet; it is not another push of the cell.
        if (state.pistons.find(movingPiston) != state.pistons.end()) {
            OPTIPISTON_PROBE_DO(tProbeWhy = {1, rec.action, pistonAction(movingPiston), rec.timeline.state()};)
            return false;
        }
        OPTIPISTON_PROBE_DO(tProbeWhy = {5, rec.action, 0, rec.timeline.state()};)
    }
    // The same cell can be entered again by a newer action of the piston; that instance is a new one.
    if (auto const piston = state.pistons.find(rec.pistonPos);
        piston != state.pistons.end() && piston->second.action != rec.action) {
        OPTIPISTON_PROBE_DO(tProbeWhy = {2, rec.action, piston->second.action, rec.timeline.state()};)
        return false;
    }
    bool const hidden = drawerOf(rec, frame) != PushDrawer::Vanilla;
    OPTIPISTON_PROBE_DO(if (!hidden) tProbeWhy = {3, rec.action, pistonAction(rec.pistonPos), rec.timeline.state()};)
    return hidden;
}

#if OPTIPISTON_PROBE
// ---- Probe cross-checks: sampled calls are answered again from the live tables and compared ----

std::array<float, 4> probePack(bool value) { return {value ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}; }
std::array<float, 4> probePack(std::optional<float> const& value) {
    return {value ? 1.0f : 0.0f, value.value_or(0.0f), 0.0f, 0.0f};
}
std::array<float, 4> probePack(std::optional<::Vec3> const& value) {
    return {value ? 1.0f : 0.0f, value ? value->x : 0.0f, value ? value->y : 0.0f, value ? value->z : 0.0f};
}

constexpr uint32_t kCheckPeriod = 64;

// Every `period`-th call per call site and thread: `answer(StateRef)` on a fresh render view and on the live tables
// under gAnimMutex must agree. The lock wait is kept out of the timings.
template <class Answer>
void probeCrossCheck(uint32_t period, Answer&& answer) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    thread_local uint32_t calls = 0;
    if (++calls % period != 0) return;
    auto const saved = tProbeWhy;
    auto const view  = acquireRenderView();
    tUntimedLock     = true;
    {
        std::lock_guard const guard(gAnimMutex);
        // A change since the view was built makes the comparison meaningless.
        if (view->revision == gRevision.load(std::memory_order_acquire)) {
            StateRef const live{gCells, gPistonVisuals, gActionVisuals};
            bool const     same = answer(live) == answer(view->state());
            gProbeViewChecks.fetch_add(1, std::memory_order_relaxed);
            if (!same) gProbeViewMismatch.fetch_add(1, std::memory_order_relaxed);
        }
    }
    tUntimedLock = false;
    tProbeWhy    = saved;
}

// The published mesh view against the three questions the mesher asks, answered from the live tables.
void probeCheckMesh(::BlockPos const& pos) {
    if (gProbePerfOnly.load(std::memory_order_relaxed)) return;
    thread_local uint32_t calls = 0;
    if (++calls % (kCheckPeriod * 4) != 0) return;
    tUntimedLock = true;
    {
        std::lock_guard const guard(gAnimMutex);
        if (!gMeshDirty) {
            auto const  published = gMeshView.load(std::memory_order_acquire);
            auto const& view      = published ? *published : emptyMeshView();
            auto const  liveCell  = [](::BlockPos const& at) -> PushCell const* {
                auto const it = gCells.find(at);
                return it == gCells.end() ? nullptr : &it->second;
            };
            auto const* const self       = liveCell(pos);
            bool const        suppressed = self && self->timeline.meshSuppressed();
            bool const        awaiting   = self && self->timeline.awaitingMesh();
            int const         face =
                self && self->timeline.state() == PushState::Animating ? (self->timeline.landed() ? 1 : 2) : 0;
            static ::BlockPos const dirs[6]{
                {0,  -1, 0 },
                {0,  1,  0 },
                {0,  0,  -1},
                {0,  0,  1 },
                {-1, 0,  0 },
                {1,  0,  0 }
            };
            bool touches = false;
            for (auto const& dir : dirs) {
                auto const* const neighbour = liveCell(pos + dir);
                touches                     = touches || (neighbour && neighbour->timeline.meshSuppressed());
            }
            auto const key  = cellOf(pos);
            bool const same = view.suppressed(key) == suppressed && view.awaiting(key) == awaiting
                           && view.faceHidden(key) == face && view.touchesSuppressed(key) == touches;
            gProbeViewChecks.fetch_add(1, std::memory_order_relaxed);
            if (!same) gProbeViewMismatch.fetch_add(1, std::memory_order_relaxed);
        }
    }
    tUntimedLock = false;
}
#endif

std::optional<float> visualArmProgress(::BlockPos const& pistonPos, float alpha) {
    if (!gStateWatch.load(std::memory_order_relaxed)) return std::nullopt;
    ViewPin        pin;
    LazyTime const time{alpha};
    auto const     result = armProgressIn(pin.get().state(), pistonPos, time);
    OPTIPISTON_PROBE_DO(probeCrossCheck(kCheckPeriod, [&](StateRef const& state) {
                            return probePack(armProgressIn(state, pistonPos, time));
                        });)
    return result;
}

// A landed cell asks for its re-mesh once, however many draws see it near the end.
void requestEarlyRemesh(PushCell const& rec) {
    if (rec.shared->earlyRemesh.exchange(true, std::memory_order_relaxed)) return;
    std::lock_guard const guard(gEarlyMutex);
    gEarlyRemesh.push_back(rec.cell);
}

// Relative to the MovingBlock's own (target) cell, same convention as native getDrawPos. Only our own proxies are
// positioned here; the game's instances are either hidden or left alone.
std::optional<::Vec3> visualDrawOffset(::MovingBlockActor const& moving, float alpha) {
    if (!gStateWatch.load(std::memory_order_relaxed)) return std::nullopt;
    ViewPin pin;
    auto&   state = pin.get();
    auto    found = state.cells.find(moving.mPosition.get());
    if (found == state.cells.end() || found->second.proxy.get() != &moving) return std::nullopt;
    auto const&    rec = found->second;
    LazyTime const time{alpha};
    auto const     now = time.get();
    gLastVisualTime.store(now, std::memory_order_relaxed);
    // A carried actor stays on the proxy until the landed one shows up, so timelines never ask for an early mesh then.
    // The expected build time in visual ticks, plus the frame that shows it.
    auto const lead = gMeshLatency.load(std::memory_order_relaxed) * gTicksPerSecond.load(std::memory_order_relaxed)
                    + gFrameStep.load(std::memory_order_relaxed);
    if (rec.timeline.shouldRequestRemesh(now, std::min(lead, kMaxEarlyLeadTicks))) requestEarlyRemesh(rec);
    OPTIPISTON_PROBE_DO(probeCrossCheck(kCheckPeriod, [&](StateRef const& at) {
                            return probePack(drawOffsetIn(at, moving, time));
                        });)
    return cellOffset(rec, now);
}

// Extra offset for the arm of a piston whose body is still finishing the push that carried it here.
std::optional<::Vec3> visualBodyOffset(::BlockPos const& pistonPos, float alpha) {
    if (!gStateWatch.load(std::memory_order_relaxed)) return std::nullopt;
    ViewPin        pin;
    LazyTime const time{alpha};
    auto const     result = bodyOffsetIn(pin.get().state(), pistonPos, time);
    OPTIPISTON_PROBE_DO(probeCrossCheck(kCheckPeriod, [&](StateRef const& state) {
                            return probePack(bodyOffsetIn(state, pistonPos, time));
                        });)
    return result;
}

// Offset of the proxy still drawing a landed piston's body; its head must stay attached to it.
std::optional<::Vec3> landedBodyOffset(::BlockPos const& pistonPos, float alpha) {
    if (!gStateWatch.load(std::memory_order_relaxed)) return std::nullopt;
    ViewPin        pin;
    LazyTime const time{alpha};
    auto const     frame  = gFrame.load(std::memory_order_relaxed);
    auto const     result = landedOffsetIn(pin.get().state(), pistonPos, time, frame);
    OPTIPISTON_PROBE_DO(probeCrossCheck(kCheckPeriod, [&](StateRef const& state) {
                            return probePack(landedOffsetIn(state, pistonPos, time, frame));
                        });)
    return result;
}

// What a game MovingBlock of a live action needs: nothing, a first proxy, or its existing proxy pointed at the game's
// rebuilt actor.
struct SnapshotPlan {
    enum class Kind : uint8_t { None, Create, Follow };
    Kind                                kind{Kind::None};
    uint64_t                            action{};
    ActionVisual                        visual;
    std::shared_ptr<::MovingBlockActor> existing;
    OPTIPISTON_PROBE_DO(uint64_t startFrame{};)
};

SnapshotPlan planSnapshot(StateRef const& state, ::MovingBlockActor const& moving) {
    SnapshotPlan plan;
    auto const   piston = state.pistons.find(moving.mPistonBlockPos.get());
    if (piston == state.pistons.end()) return plan;
    auto const live = state.actions.find(piston->second.action);
    if (live == state.actions.end()) return plan;
    plan.action = piston->second.action;
    if (auto const known = state.cells.find(moving.mPosition.get());
        known != state.cells.end() && known->second.action == plan.action) {
        // The game rebuilds the carried actor mid-push; the proxy follows the live instance.
        auto const& proxy = known->second.proxy;
        if (!proxy || proxy->mWrappedBlockActor == moving.mWrappedBlockActor) return plan;
        plan.existing = proxy;
        plan.kind     = SnapshotPlan::Kind::Follow;
        return plan;
    }
    plan.kind   = SnapshotPlan::Kind::Create;
    plan.visual = live->second;
    OPTIPISTON_PROBE_DO(plan.startFrame = piston->second.startFrame;)
    return plan;
}

// A proxy built outside the lock, waiting to become a cell.
struct NewProxy {
    ::BlockPos                          cell;
    ::BlockPos                          pistonPos;
    uint64_t                            action{};
    ActionVisual                        visual;
    bool                                carriesActor{};
    bool                                placed{};     // the real block was already in the cell when it was built
    bool                                committed{};  // became a cell
    bool                                pureInsert{}; // committed without replacing or superseding another record
    std::shared_ptr<std::string const>  wrapped;
    std::shared_ptr<::MovingBlockActor> proxy;
    OPTIPISTON_PROBE_DO(uint64_t startFrame{};)
};

// Only the game is called here, never gAnimMutex. There is no proxy pool: the engine derives state from the
// position at construction that cannot be reset reliably, so every cell gets a fresh one.
NewProxy buildProxy(::MovingBlockActor& moving, SnapshotPlan const& plan) {
    OPTIPISTON_PERF_SCOPE(perf, ProxyCreate);
    NewProxy made;
    made.cell      = moving.mPosition.get();
    made.pistonPos = moving.mPistonBlockPos.get();
    made.action    = plan.action;
    made.visual    = plan.visual;
    OPTIPISTON_PROBE_DO(made.startFrame = plan.startFrame;)
    auto proxy                       = std::shared_ptr<::MovingBlockActor>(new ::MovingBlockActor(made.cell));
    proxy->mWrappedBlock             = moving.mWrappedBlock;
    proxy->mWrappedExtraBlock        = moving.mWrappedExtraBlock;
    wrappedActorOf(*proxy)           = wrappedActorOf(moving);
    proxy->mPistonBlockPos.get()     = made.pistonPos;
    proxy->mPistonBlockExpanding     = moving.mPistonBlockExpanding;
    auto& interlock                  = interlockOf(*proxy);
    interlock.mRenderVisibilityState = VisibilityState::Visible;
    interlock.mHasBeenDelayedDeleted = false;
    made.carriesActor                = wrappedActorOf(moving) != nullptr;
    made.wrapped                     = std::make_shared<std::string>(wrappedName(moving));
    made.proxy                       = std::move(proxy);
    return made;
}

// Caller holds gAnimMutex. A piston pushed while its arm still plays takes that motion along: the actors that draw
// it at the new cell look it up by position, and a landing packet in the same direction must not restart it.
void moveArmLocked(::BlockPos const& from, ::BlockPos const& to, std::string const& wrapped) {
    if (wrapped.find("piston") == std::string::npos || wrapped.find("arm_collision") != std::string::npos) return;
    auto const src = gPistonVisuals.find(from);
    if (src == gPistonVisuals.end() || src->second.vacated) return;
    auto const tick = visualClockTick().value_or(0);
    if (!core::segmentRunning(src->second.current, tick)
        && !(src->second.previous && core::segmentRunning(*src->second.previous, tick)))
        return;
    // A piston still leaving the target cell keeps its own entry; a vacated one is replaced.
    if (auto const dst = gPistonVisuals.find(to); dst != gPistonVisuals.end() && !dst->second.vacated) return;
    PistonVisual moved = src->second;
    moved.attached.reset();
    moved.scanUntil = 0;
    moved.bodyCarry.reset();
    src->second.vacated = true;
    OPTIPISTON_PROBE_DO(probeArmMoved(from, to, moved.action);)
    gPistonVisuals.insert_or_assign(to, std::move(moved));
    bumpLocked();
}

// Caller holds gAnimMutex. A proxy whose action ended meanwhile, or whose cell already has one for the action or a
// newer one, is dropped instead.
void commitProxyLocked(NewProxy& made, Graveyard& drop) {
    auto const existing = gCells.find(made.cell);
    if (!gActionVisuals.contains(made.action) || (existing != gCells.end() && existing->second.action == made.action)) {
        drop.push_back(std::move(made.proxy));
        return;
    }
    if (existing != gCells.end() && existing->second.action > made.action) {
        OPTIPISTON_PROBE_DO(gProbe.olderDropped.fetch_add(1, std::memory_order_relaxed);)
        drop.push_back(std::move(made.proxy));
        return;
    }
    PushCell rec;
    rec.action    = made.action;
    rec.cell      = made.cell;
    rec.pistonPos = made.pistonPos;
    OPTIPISTON_PROBE_DO(rec.probeStartFrame = made.startFrame;)
    rec.visual  = made.visual;
    rec.wrapped = std::move(made.wrapped);
    bool pure   = !made.placed;
    // The block now leaves its source cell; an older cell still playing its tail hands it over.
    auto const       dir = made.visual.segment.to > made.visual.segment.from ? 1 : -1;
    ::BlockPos const source{
        made.cell.x - made.visual.facing.x * dir,
        made.cell.y - made.visual.facing.y * dir,
        made.cell.z - made.visual.facing.z * dir
    };
    if (source != made.pistonPos) {
        auto const old = gCells.find(source);
        if (old != gCells.end() && old->second.action != made.action && !old->second.timeline.superseded()) {
            rec.carry = CarryVisual{old->second.action, old->second.visual};
            pure      = false;
            old->second.timeline.supersededBy(made.action);
            takeProxy(old->second, drop);
            syncCellLocked(source, old->second, true);
        } else if (auto const tail = gReplacedTails.find(source);
                   tail != gReplacedTails.end() && tail->second.action == made.action) {
            // A sibling of this action already replaced the cell the block comes from.
            rec.carry = tail->second.carry;
            pure      = false;
            OPTIPISTON_PROBE_DO(probeTailCarried(made.cell, source, tail->second.carry.action, made.action);)
            gReplacedTails.erase(tail);
        }
    }
    if (source != made.pistonPos && rec.wrapped) moveArmLocked(source, made.cell, *rec.wrapped);
    if (auto const replaced = gCells.find(made.cell); replaced != gCells.end()) {
        pure = false;
        OPTIPISTON_PROBE_DO(
            probeCellReplaced(made.cell, replaced->second.action, replaced->second.timeline.state(), made.action);
        )
        if (replaced->second.timeline.state() != PushState::Done && !replaced->second.timeline.superseded()) {
            // Its block may still be picked up from here by a sibling cell that is committed later.
            std::erase_if(gReplacedTails, [](auto const& entry) {
                return !gActionVisuals.contains(entry.second.action);
            });
            gReplacedTails[made.cell] = ReplacedTail{
                made.action,
                CarryVisual{replaced->second.action, replaced->second.visual}
            };
        }
        if (replaced->second.timeline.state() != PushState::Done) replaced->second.timeline.supersededBy(made.action);
        takeProxy(replaced->second, drop);
        forgetCellLocked(made.cell, replaced->second);
        gCells.erase(replaced);
    }
    rec.timeline.start(made.action, made.visual.segment, made.carriesActor);
    rec.proxy = std::move(made.proxy);
    // A replay can place the real block before its MovingBlock is first drawn, with no landing packet.
    if (made.placed) rec.timeline.blockLanded();
    // The game's own area change can wait in the render queue for many frames; the old block must leave the mesh now.
    gPendingRemesh.push_back(made.cell);
    gPendingWatch.store(true, std::memory_order_relaxed);
    flagStaleRiskLocked(made.cell);
    auto const inserted = gCells.emplace(made.cell, std::move(rec));
    syncCellLocked(made.cell, inserted.first->second, true);
    made.committed  = true;
    made.pureInsert = pure;
    OPTIPISTON_PROBE_DO(gProbe.proxiesCreated.fetch_add(1, std::memory_order_relaxed);
                        probeCellCreated(made.cell, made.action);)
}

// One lock and one publication for a whole batch of new proxies. Returns how many became cells.
std::size_t commitProxies(NewProxy* made, std::size_t count) {
    // Declared first, so dropped proxies die after the lock is released.
    Graveyard             drop;
    std::size_t           created = 0;
    bool                  allPure = true;
    std::lock_guard const guard(gAnimMutex);
    auto const            before = gRevision.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        commitProxyLocked(made[i], drop);
        created += made[i].committed ? 1 : 0;
        if (made[i].committed && !made[i].pureInsert) allPure = false;
    }
    if (created == 0) return 0;
    // A commit that replaced or superseded a record leaves that record stale in a lagging view.
    if (allPure && tRenderScope) tRenderScope->created(before, gRevision.load(std::memory_order_relaxed));
    settleLocked(drop);
    return created;
}

void followGameActor(::MovingBlockActor& moving, SnapshotPlan const& plan) {
    wrappedActorOf(*plan.existing) = wrappedActorOf(moving);
}

// Creates the proxy for a game MovingBlock of a live action, or refreshes the one it already has. Returns true if a
// proxy was created now. Runs on the render thread; the engine is never called with gAnimMutex held.
bool snapshotMoving(::MovingBlockActor& moving, ::BlockSource& region, bool checkPlaced) {
    if (!gStateWatch.load(std::memory_order_relaxed)) return false;
    // A detached instance is an older push of the cell; only the registered one may create or follow.
    bool const attached = region.getBlockEntity(moving.mPosition.get()) == &moving;
    if (!attached) {
        OPTIPISTON_PROBE_DO(gProbe.snapDetached.fetch_add(1, std::memory_order_relaxed);)
        return false;
    }
    SnapshotPlan plan;
    {
        ViewPin pin;
        plan = planSnapshot(pin.lagged().state(), moving);
    }
    if (plan.kind == SnapshotPlan::Kind::None) return false;
    if (plan.kind == SnapshotPlan::Kind::Follow) {
        followGameActor(moving, plan);
        return false;
    }
    auto made = buildProxy(moving, plan);
    if (checkPlaced) made.placed = region.getBlock(made.cell).getTypeName() == *made.wrapped;
    return commitProxies(&made, 1) != 0;
}

// The game's instance of a managed cell is never drawn while our timeline owns the cell.
bool vanillaHidden(::MovingBlockActor const& moving, bool attached) {
    OPTIPISTON_PROBE_DO(tProbeWhy = {};)
    if (!gStateWatch.load(std::memory_order_relaxed)) return false;
    ViewPin    pin;
    auto const frame = gFrame.load(std::memory_order_relaxed);
    auto const hidden =
        vanillaHiddenIn(pin.lagged().state(), moving.mPosition.get(), moving.mPistonBlockPos.get(), frame, attached);
    OPTIPISTON_PROBE_DO(
        if (tProbeWhy.reason == 1 || tProbeWhy.reason == 5)
            probePosMismatch(moving.mPosition.get(), moving, pin.lagged().state());
    )
    OPTIPISTON_PROBE_DO(
        probeCrossCheck(kCheckPeriod, [&](StateRef const& state) {
            return probePack(
                vanillaHiddenIn(state, moving.mPosition.get(), moving.mPistonBlockPos.get(), frame, attached)
            );
        });
    )
    return hidden;
}

// Looks for the game's MovingBlocks of recently started actions where their blocks are heading, since the camera
// does not always hand them to a renderer.
void scanTargets(::BlockSource& region) {
    if (!gStateWatch.load(std::memory_order_relaxed)) return;
    OPTIPISTON_PERF_SCOPE(perf, ScanTargets);
    // Reused across frames, so a steady scan allocates nothing.
    thread_local std::vector<::BlockPos> candidates;
    thread_local std::vector<NewProxy>   made;
    OPTIPISTON_PROBE_DO(thread_local std::vector<std::uint8_t> roles;)
    ViewPin                              pin;
    auto const&                          state = pin.get();
    auto const                           now   = visualClockTick().value_or(gLastTick.load(std::memory_order_relaxed));
    OPTIPISTON_PROBE_DO(
        uint64_t nKnown = 0; uint64_t nDupes = 0; uint64_t nHits = 0; uint64_t nCreated = 0; uint64_t nFollowed = 0;
    )
    for (auto const& [pistonPos, piston] : state.pistons) {
        if (now > piston.scanUntil) continue;
        OPTIPISTON_PROBE_DO(gProbe.scanPistons.fetch_add(1, std::memory_order_relaxed);)
        auto const live = state.actions.find(piston.action);
        if (live == state.actions.end()) continue;
        auto const& facing = live->second.facing;
        candidates.clear();
        OPTIPISTON_PROBE_DO(
            roles.clear(); std::uint8_t role = 0; uint64_t attPushed = 0;
            gPerfCounts2.scanWindow.fetch_add(1, std::memory_order_relaxed);
            if (!piston.attached) gPerfCounts2.scanNoAttached.fetch_add(1, std::memory_order_relaxed);
        )
        auto const add = [&](::BlockPos const& cell) {
            if (auto const known = state.cells.find(cell);
                known != state.cells.end() && known->second.action == piston.action) {
                OPTIPISTON_PROBE_DO(++nKnown;)
                return;
            }
            if (std::find(candidates.begin(), candidates.end(), cell) == candidates.end()) {
                candidates.push_back(cell);
                OPTIPISTON_PROBE_DO(roles.push_back(role); attPushed += role != 0;)
            } else {
                OPTIPISTON_PROBE_DO(++nDupes;)
            }
        };
        for (int k = 1; k <= 13; ++k)
            add(::BlockPos{pistonPos.x + facing.x * k, pistonPos.y + facing.y * k, pistonPos.z + facing.z * k});
        if (piston.attached) {
            for (auto const& block : *piston.attached) {
                OPTIPISTON_PROBE_DO(role = 1;)
                add(block);
                OPTIPISTON_PROBE_DO(role = 2;)
                add(block + facing);
                OPTIPISTON_PROBE_DO(role = 3;)
                add(block - facing);
            }
        }
        OPTIPISTON_PROBE_DO(
            if (piston.attached && attPushed == 0) gPerfCounts2.scanAttSettled.fetch_add(1, std::memory_order_relaxed);
        )
        OPTIPISTON_PROBE_DO(perf.pause();)
        for (auto const& cell : candidates) {
            OPTIPISTON_PROBE_DO(gProbe.scanLookups.fetch_add(1, std::memory_order_relaxed);)
            OPTIPISTON_PROBE_DO(
                auto const lookRole = roles[static_cast<std::size_t>(&cell - candidates.data())];
                gPerfCounts2.scanLookRole[lookRole].fetch_add(1, std::memory_order_relaxed);
            )
            auto* const actor = [&] {
                OPTIPISTON_PROBE_DO(GameSpan lookup(PerfId::ScanLookups);)
                return region.getBlockEntity(cell);
            }();
            if (!actor || actor->mType != ::BlockActorType::MovingBlock) continue;
            auto& moving = *static_cast<::MovingBlockActor*>(actor);
            if (moving.mPistonBlockPos.get() != pistonPos) continue;
            OPTIPISTON_PROBE_DO(
                ++nHits; gPerfCounts2.scanHitRole[lookRole].fetch_add(1, std::memory_order_relaxed);
                if (lookRole == 0 && piston.attached) gPerfCounts2.scanHitLineOnly.fetch_add(1, std::memory_order_relaxed);
            )
            auto const plan = planSnapshot(state.state(), moving);
            if (plan.kind == SnapshotPlan::Kind::Create) {
                OPTIPISTON_PROBE_DO(++nCreated; perf.resume();)
                made.push_back(buildProxy(moving, plan));
                OPTIPISTON_PROBE_DO(perf.pause();)
            } else if (plan.kind == SnapshotPlan::Kind::Follow) {
                OPTIPISTON_PROBE_DO(++nFollowed;)
                followGameActor(moving, plan);
            }
        }
        OPTIPISTON_PROBE_DO(perf.resume();)
    }
    OPTIPISTON_PROBE_DO(gPerfCounts.scanKnown.fetch_add(nKnown, std::memory_order_relaxed);
                        gPerfCounts.scanDuplicates.fetch_add(nDupes, std::memory_order_relaxed);
                        gPerfCounts.scanHits.fetch_add(nHits, std::memory_order_relaxed);
                        gPerfCounts.scanCreated.fetch_add(nCreated, std::memory_order_relaxed);
                        gPerfCounts.scanFollowed.fetch_add(nFollowed, std::memory_order_relaxed);)
    if (made.empty()) return;
    commitProxies(made.data(), made.size());
    made.clear();
}

// Caller holds gAnimMutex and calls flushLocked afterwards.
bool holdLandedLocked(::BlockPos const& cell) {
    auto const it = gCells.find(cell);
    if (it == gCells.end() || it->second.timeline.state() != PushState::Animating) return false;
    it->second.timeline.blockLanded();
    syncCellLocked(cell, it->second, true);
    return true;
}

// Runs before the packet applies, so the mesh rebuild it triggers already skips the landed block. Takes the lock
// once, and only if a block really lands.
template <class Infos>
void holdLandedBlocks(Infos const& infos) {
    thread_local std::vector<::BlockPos> landed;
    landed.clear();
    {
        ViewPin     pin;
        auto const& cells = pin.get().cells;
        for (auto const& info : infos) {
            auto const it = cells.find(info.mPos);
            if (it == cells.end() || it->second.timeline.state() != PushState::Animating) continue;
            auto const block = blockNameOf(info.mRuntimeId);
            if (block.empty() || block == "minecraft:air" || block == "minecraft:moving_block") continue;
            landed.push_back(info.mPos);
        }
    }
    if (landed.empty()) return;
    std::lock_guard const guard(gAnimMutex);
    bool                  any = false;
    for (auto const& cell : landed) any = holdLandedLocked(cell) || any;
    if (any) flushLocked();
}

// A replay can place the real block before its MovingBlock is first drawn, with no landing packet.
void holdPlacedCell(::BlockSource& region, ::BlockPos const& cell) {
    // Only an animating cell can land, and gFaceWatch is set exactly while one exists.
    if (!gFaceWatch.load(std::memory_order_relaxed)) return;
    auto const& block = region.getBlock(cell).getTypeName();
    {
        ViewPin     pin;
        auto const& cells = pin.lagged().cells;
        auto const  it    = cells.find(cell);
        if (it == cells.end() || it->second.timeline.state() != PushState::Animating || it->second.timeline.landed()
            || !it->second.wrapped || *it->second.wrapped != block)
            return;
    }
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCells.find(cell);
    if (it == gCells.end() || it->second.timeline.state() != PushState::Animating || it->second.timeline.landed()
        || !it->second.wrapped || *it->second.wrapped != block)
        return;
    it->second.timeline.blockLanded();
    gPendingRemesh.push_back(cell);
    gPendingWatch.store(true, std::memory_order_relaxed);
    syncCellLocked(cell, it->second, true);
    flushLocked();
}

// Chunk meshes skip faces against the target cell, but it stays empty on screen while the proxy is still gliding
// in. Returns 0 if shown, 1 landed, 2 still a moving_block.
int neighborHidden(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gFaceWatch.load(std::memory_order_relaxed)) return 0;
    MeshViewPin pin;
    OPTIPISTON_PROBE_DO(probeCheckMesh(pos);)
    return pin.get().faceHidden(cellOf(pos));
}

// Full cubes cull against simple neighbours through the chunk's bitset, never asking the occluder.
bool touchesLanded(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gLandedWatch.load(std::memory_order_relaxed)) return false;
    MeshViewPin pin;
    OPTIPISTON_PROBE_DO(probeCheckMesh(pos);)
    return pin.get().touchesSuppressed(cellOf(pos));
}

// The landed cell's own actor was drawn this frame; a handover may end. Only a note is left, which the timeline
// absorbs on the next tick or frame.
void noteActorShown(::BlockPos const& cell) {
    if (!gActorWatch.load(std::memory_order_relaxed)) return;
    ViewPin     pin;
    auto const& cells = pin.get().cells;
    auto const  it    = cells.find(cell);
    if (it == cells.end() || !it->second.timeline.wantsActorSeen()) return;
    auto&      note  = it->second.shared->actorFrame;
    auto const frame = gFrame.load(std::memory_order_relaxed);
    auto       seen  = note.load(std::memory_order_relaxed);
    while (seen < frame && !note.compare_exchange_weak(seen, frame, std::memory_order_relaxed)) {}
}

// Heads are noted by drawArm instead, after the per-cell election.
void noteDispatched(::BlockActor const& actor) {
    if (tRenderDepth > 0 || actor.mType == ::BlockActorType::PistonArm || actor.mType == ::BlockActorType::MovingBlock)
        return;
    noteActorShown(actor.mPosition.get());
}

// A landed block's own actor (a chest, a spawner's mob, a lectern's book...) would sit at its final cell while the
// proxy still carries a copy of it in. Pistons are excluded: drawArm keeps their head on the moving body.
bool landedActorCovered(::BlockActor const& entity) {
    if (tRenderDepth > 0 || !gLandedWatch.load(std::memory_order_relaxed)) return false;
    if (entity.mType == ::BlockActorType::PistonArm || entity.mType == ::BlockActorType::MovingBlock) return false;
    if (!animationActive()) return false;
    ViewPin     pin;
    auto const& cells = pin.get().cells;
    auto const  it    = cells.find(entity.mPosition.get());
    return it != cells.end() && it->second.timeline.coversLandedActor();
}

// Runs once per drawn frame, after the level is rendered.
void releaseEarlyCells() {
    auto const visual = gLastVisualTime.load(std::memory_order_relaxed);
    auto const speed  = static_cast<double>(core::worldSpeedSettings().applied.load(std::memory_order_relaxed));
    // The visual time only moves while a proxy is drawn, so its per-frame delta spans several frames after a pause.
    auto const wall = std::chrono::steady_clock::now();
    if (gPrevFrameWall != std::chrono::steady_clock::time_point{}) {
        auto const seconds = std::chrono::duration<double>(wall - gPrevFrameWall).count();
        if (seconds > 0.0 && seconds < 0.25) {
            auto const estimate = gFrameSeconds.load(std::memory_order_relaxed);
            gFrameSeconds.store(estimate + (seconds - estimate) * 0.1, std::memory_order_relaxed);
        }
    }
    gPrevFrameWall = wall;
    // Visual time runs at 20 ticks per second times the world speed.
    auto const rate = 20.0 * std::max(speed, 0.0);
    gTicksPerSecond.store(rate, std::memory_order_relaxed);
    gFrameStep.store(gFrameSeconds.load(std::memory_order_relaxed) * rate, std::memory_order_relaxed);
    {
        std::lock_guard const early(gEarlyMutex);
        if (gEarlyRemesh.empty()) return;
        gEarlyScratch.swap(gEarlyRemesh);
    }
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const guard(gAnimMutex);
        for (auto const& cell : gEarlyScratch) {
            auto const it = gCells.find(cell);
            if (it == gCells.end()) continue;
            it->second.shared->earlyRemesh.store(false, std::memory_order_relaxed);
            if (!it->second.timeline.remeshRequested(visual)) continue;
            OPTIPISTON_PROBE_DO(probeRemeshRequest(cell, it->second, 0);)
            cells.push_back(cell);
            syncCellLocked(cell, it->second, true);
        }
        if (!cells.empty()) flushLocked();
    }
    gEarlyScratch.clear();
    if (cells.empty()) return;
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (player) {
        rebuildCells(player->getDimensionBlockSource(), cells);
        return;
    }
    std::lock_guard const guard(gAnimMutex);
    gPendingRemesh.insert(gPendingRemesh.end(), cells.begin(), cells.end());
    gPendingWatch.store(true, std::memory_order_relaxed);
}

// Moves every timeline to the next frame and lets finished cells go.
void advanceFrame() {
    if (!gStateWatch.load(std::memory_order_relaxed)) {
        gFrame.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Declared first, so the proxies die after the lock is released.
    Graveyard             drop;
    std::lock_guard const guard(gAnimMutex);
    auto const            frame = gFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    auto const            tick  = gLastTick.load(std::memory_order_relaxed);
    foldActorNotesLocked();
    for (auto& [pos, rec] : gCells) {
        if (rec.timeline.state() != PushState::Done) {
            // The cell is empty for a moment while its successor lands; only air that outlives a game tick is gone.
            if (!rec.shared->blockGone.exchange(false, std::memory_order_relaxed)) {
                OPTIPISTON_PROBE_DO(
                    if (rec.goneTick >= 0)
                        probeGoneEnd(pos, rec.action, tick - rec.goneTick, frame - rec.probeGoneFrame, false);
                )
                rec.goneTick = -1;
            } else if (rec.goneTick < 0) {
                rec.goneTick = tick;
                OPTIPISTON_PROBE_DO(rec.probeGoneFrame = frame;)
            } else if (tick > rec.goneTick) {
                OPTIPISTON_PROBE_DO(
                    probeGoneEnd(pos, rec.action, tick - rec.goneTick, frame - rec.probeGoneFrame, true);
                    probeCellCleared(pos, rec);
                )
                rec.goneTick = -1;
                rec.timeline.cleared();
            }
        }
        rec.timeline.frameAdvanced(frame);
        syncCellLocked(pos, rec, false);
    }
    reapLocked(gLastTick.load(std::memory_order_relaxed), drop);
    settleLocked(drop);
}

// The camera's forced material, reused for the proxies only while its address never changes (a static).
std::atomic<::mce::MaterialPtr const*> gCameraForcedMat{nullptr};
std::atomic_bool                       gCameraMatMoved{false};

void noteCameraDispatch(::BlockActor const& actor, ::mce::MaterialPtr const& forcedMat) {
    if (tProxyDraw != nullptr || actor.mType != ::BlockActorType::MovingBlock) return;
    auto const* const previous = gCameraForcedMat.exchange(&forcedMat, std::memory_order_relaxed);
    if (previous && previous != &forcedMat) gCameraMatMoved.store(true, std::memory_order_relaxed);
}

// Draws the proxies whose timeline says they are the drawer. Runs right after the camera drew its queues.
void drawProxies(::BaseActorRenderContext& context, bool renderAlphaLayer) {
    OPTIPISTON_PERF_SCOPE(perf, RenderBlockEntities);
    if (!gStateWatch.load(std::memory_order_relaxed)) return;
    // A reused buffer of cells from the view, which the pin keeps alive for the whole draw.
    thread_local std::vector<PushCell const*> drawList;
    ViewPin                                   pin;
    auto const&                               view  = pin.get();
    auto const                                frame = gFrame.load(std::memory_order_relaxed);
    drawList.clear();
    for (auto const* rec : view.proxied)
        if (drawerOf(*rec, frame) == PushDrawer::Proxy) drawList.push_back(rec);
    if (drawList.empty()) return;
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    gFrameDrewActors.store(true, std::memory_order_relaxed);
    auto& region = player->getDimensionBlockSource();
    // A null MaterialPtr, for when the camera's own one is not known to be a static.
    alignas(::mce::MaterialPtr) static std::byte const noMaterialStorage[sizeof(::mce::MaterialPtr)]{};
    auto const* const                                  cameraMat  = gCameraForcedMat.load(std::memory_order_relaxed);
    auto const&                                        forcedMat  = cameraMat && !gCameraMatMoved.load()
                                                                      ? *cameraMat
                                                                      : *reinterpret_cast<::mce::MaterialPtr const*>(noMaterialStorage);
    ::BlockActorRenderDispatcher&                      dispatcher = context.mBlockEntityRenderDispatcher;
    for (auto const* rec : drawList) {
        auto const& proxy = rec->proxy;
        auto* const item  = queuedItemOf(proxy.get());
        // The game's alpha pass runs after the translucent terrain, which then cannot cover a translucent proxy.
        if (!item || renderAlphaLayer) continue;
        bool const  alphaLayer = item->hasAlphaLayer();
        auto const& block      = region.getBlock(proxy->mPosition);
        if (block.isAir() && rec->timeline.state() != PushState::Animating) {
            rec->shared->blockGone.store(true, std::memory_order_relaxed);
            continue;
        }
        interlockOf(*proxy).mRenderVisibilityState = VisibilityState::Visible;
        // The camera drew the landed actor just before this, so the proxy's copy of it would sit on top of it.
        std::shared_ptr<::BlockActor> stripped;
        if (rec->timeline.wantsActorSeen() && rec->shared->actorFrame.load(std::memory_order_relaxed) == frame) {
            stripped.swap(wrappedActorOf(*proxy));
            OPTIPISTON_PROBE_DO(if (stripped) {
                rec->shared->strippedFrame.store(frame, std::memory_order_relaxed);
                gProbe.actorStripped.fetch_add(1, std::memory_order_relaxed);
            })
        }
        tProxyDraw = proxy.get();
#if OPTIPISTON_MC == 2651
        tProxyLightFix = {true, proxy->mPosition.get(), &rec->shared->lightMemory};
#endif
        OPTIPISTON_PROBE_DO(
            tProbePass = alphaLayer ? 1 : 0; probeProxyDispatched(proxy->mPosition.get(), tProbePass);
            if (alphaLayer) probeAlphaEarly(proxy->mPosition.get());
            tProbeProxyLight = ProbeProxyLight{true, proxy->mPosition.get(), 0, 0, 0};
            perf.pause();
        )
        dispatcher.render(context, region, *item, block, alphaLayer, forcedMat, nullptr, 0, {});
        OPTIPISTON_PROBE_DO(perf.resume(); probeProxyLightEnd(proxy->mPosition.get());)
#if OPTIPISTON_MC == 2651
        tProxyLightFix = {};
#endif
        tProxyDraw = nullptr;
        if (stripped) wrappedActorOf(*proxy).swap(stripped);
    }
}

// Entries older than this many ticks are dropped once the tables grow, so destroyed pistons cannot pile up.
constexpr int64_t kHeadStaleTicks = 24000;
constexpr size_t  kHeadPruneAt    = 4096;

void markPistonStepped(::BlockActor const* piston, int64_t tick) {
    std::lock_guard const guard(gHeadMutex);
    gSteppedPistons[piston] = tick;
    OPTIPISTON_PROBE_DO(gPerfCounts2.stepCalls.fetch_add(1, std::memory_order_relaxed);)
    if (gSteppedPistons.size() > kHeadPruneAt) {
        OPTIPISTON_PROBE_DO(
            auto const before = gSteppedPistons.size();
            gPerfCounts2.pruneRuns.fetch_add(1, std::memory_order_relaxed);
            gPerfCounts2.pruneScanned.fetch_add(before, std::memory_order_relaxed);
        )
        std::erase_if(gSteppedPistons, [&](auto const& e) { return e.second < tick - kHeadStaleTicks; });
        OPTIPISTON_PROBE_DO(
            gPerfCounts2.pruneErased.fetch_add(before - gSteppedPistons.size(), std::memory_order_relaxed);
        )
    }
}

// The calling thread's election table for this round. A claim is dropped on a module restart through the epoch.
HeadTable& headTableFor(uint64_t round) {
    if (!tHeadTable) {
        auto           table = std::make_unique<HeadTable>();
        std::lock_guard const guard(gHeadTablesMutex);
        tHeadTable = table.get();
        gHeadTables.push_back(std::move(table));
    }
    auto&      table = *tHeadTable;
    auto const epoch = gHeadEpoch.load(std::memory_order_relaxed);
    if (table.epoch != epoch) {
        table.owners.clear();
        table.epoch = epoch;
    }
    if (table.round != round) {
        table.round = round;
        if (table.owners.size() > kHeadPruneAt) {
            OPTIPISTON_PROBE_DO(
                auto const before = table.owners.size();
                gPerfCounts2.pruneRuns.fetch_add(1, std::memory_order_relaxed);
                gPerfCounts2.pruneScanned.fetch_add(before, std::memory_order_relaxed);
            )
            std::erase_if(table.owners, [&](auto const& e) {
                return static_cast<int64_t>(e.second.round) < static_cast<int64_t>(round) - kHeadStaleTicks;
            });
            OPTIPISTON_PROBE_DO(
                gPerfCounts2.pruneErased.fetch_add(before - table.owners.size(), std::memory_order_relaxed);
            )
        }
        OPTIPISTON_PROBE_DO(gProbeHeadOwners.store(table.owners.size(), std::memory_order_relaxed);)
    }
    return table;
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
    OPTIPISTON_PERF_SCOPE(perf, PistonTick);

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
    // The render thread must never read the live vector; this copy is taken on the piston's own tick.
    refreshAttached(mPosition.get(), mAttachedBlocks.get());
    if (beforeP != mProgress || beforeLP != mLastProgress || beforeState != mState)
        markPistonStepped(this, static_cast<int64_t>(nowTick));
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

#if OPTIPISTON_MC == 2651
// The game lights an arm from its head cell. After the logic has landed a carried opaque block there, that cell
// stores no light while our extended visual window still draws the arm, which then renders black. For the span of
// one arm draw the head cell's light is raised to the piston cell's. Outside the span nothing is touched.
struct ArmLightFix {
    bool       active{};
    ::BlockPos piston{};
    ::BlockPos head{};
};
thread_local ArmLightFix tArmLightFix;

class ArmLightFixScope {
public:
    ArmLightFixScope(::BlockActor& entity, ::BlockSource& source, bool eligible) : mSaved(tArmLightFix) {
        tArmLightFix = {};
        if (!eligible) return;
        auto&     piston = static_cast<::PistonBlockActor&>(entity);
        int const state  = static_cast<int>(piston.mState);
        // Retracted or Expanded: the cells have settled, the head cell is what the logic left there.
        if (state != 0 && state != 2) return;
        ::BlockPos const at     = entity.mPosition.get();
        ::BlockPos const facing = facingOf(piston, source);
        if (facing.x == 0 && facing.y == 0 && facing.z == 0) return;
        tArmLightFix = {true, at, {at.x + facing.x, at.y + facing.y, at.z + facing.z}};
        OPTIPISTON_PROBE_DO(gLightFixWindows.fetch_add(1, std::memory_order_relaxed);)
    }

    ~ArmLightFixScope() { tArmLightFix = mSaved; }

    ArmLightFixScope(ArmLightFixScope const&)            = delete;
    ArmLightFixScope& operator=(ArmLightFixScope const&) = delete;

private:
    ArmLightFix mSaved;
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
    OPTIPISTON_PERF_SCOPE(perf, DrawArm);
    OPTIPISTON_PROBE_DO(ArmSplit split; ++tArmTally.calls;)
    auto& interlock = interlockOf(entity);

    bool const enabled = smoothPistonRenderEnabled();
    bool const hidden  = interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible;

    // Same-cell pistons cannot be found by walking the chunk collections - each instance only ever sees itself
    // there - so one head per cell is elected here instead, fresh every round.
    bool const isArm = entity.mType == ::BlockActorType::PistonArm;
    // Drawn by a MovingBlock carrying it; a replay can leave that cell as air instead of moving_block.
    bool const nested = tRenderDepth > 0;
    OPTIPISTON_PROBE_DO(split.stage(ArmStage::Setup);)
    // A piston in a moving_block cell is cargo being pushed by another piston rather than the one driving the
    // animation, so it must lose to an actively extending head instead of competing with it.
    bool const isCargo =
        isArm && (nested || renderSource.getBlock(entity.mPosition).getTypeName() == "minecraft:moving_block");
    OPTIPISTON_PROBE_DO(split.stage(ArmStage::Cargo); if (enabled && isArm)++ tArmTally.arms;
                        if (isArm && nested)++ tArmTally.nested;
                        if (isArm && !nested)++ tArmTally.typeChecks;)
    bool skipZombieHead = false;
    // A nested head keeps its carrier's claim when the piston inside is rebuilt.
    auto const* const self = nested && tCarrier ? tCarrier : &entity;
    if (enabled && isArm) {
        auto const round = gHeadOwnerRound.load(std::memory_order_relaxed);
        auto&      owner = headTableFor(round).owners[entity.mPosition.get()];
        if (owner.round != round || owner.actor == nullptr || owner.actor == self) {
            owner = {round, self, &entity, isCargo};
        } else {
            // Driving heads outrank cargo, and a piston that advanced mProgress outranks an untouched leftover.
            std::lock_guard const guard(gHeadMutex);
            auto const rank = [](bool cargo, ::BlockActor const* piston) {
                return (cargo ? 0 : 2) + (gSteppedPistons.contains(piston) ? 1 : 0);
            };
            if (rank(isCargo, &entity) > rank(owner.cargo, owner.claimant)) owner = {round, self, &entity, isCargo};
            else skipZombieHead = true;
        }
    }

    OPTIPISTON_PROBE_DO(split.stage(ArmStage::Elect);)
    if (enabled && hidden) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        // A rebuilt piston also inherits the delete flag, which would re-hide it on the next frame.
        interlock.mHasBeenDelayedDeleted = false;
    }

    OPTIPISTON_PROBE_DO(
        if (enabled && isArm) {
            probeArmCall(entity.mPosition.get(), isCargo);
            if (skipZombieHead) probeArmSkipped(entity.mPosition.get());
        } if (skipZombieHead)++ tArmTally.skipped;
        split.skip();
    )
    if (skipZombieHead) return;
    if (isArm && !nested) noteActorShown(entity.mPosition.get());

    if (enabled) gFrameDrewActors.store(true, std::memory_order_relaxed);
    RenderScope const scope;
    OPTIPISTON_PROBE_DO(split.stage(ArmStage::Note);)
#if OPTIPISTON_MC != 2620
    ProgressOverride const progress(entity, renderContext.mFrameAlpha);
#endif
    OPTIPISTON_PROBE_DO(split.stage(ArmStage::Progress);)
    std::optional<::Vec3> bodyOffset;
    bool const            armAnimating = isArm && !nested && animationActive();
    if (armAnimating) {
        OPTIPISTON_PROBE_DO(++tArmTally.offsets;)
        bodyOffset = visualBodyOffset(entity.mPosition.get(), renderContext.mFrameAlpha);
        // Cargo is drawn through its proxy, whose draw position already carries the offset.
        if (!bodyOffset && !isCargo) bodyOffset = landedBodyOffset(entity.mPosition.get(), renderContext.mFrameAlpha);
    }
    OPTIPISTON_PROBE_DO(split.stage(ArmStage::Offset);)
#if OPTIPISTON_MC == 2651
    ArmLightFixScope const lightFix(entity, renderSource, armAnimating);
#endif
    OPTIPISTON_PROBE_DO(perf.pause();)
    if (!bodyOffset || (bodyOffset->x == 0.0f && bodyOffset->y == 0.0f && bodyOffset->z == 0.0f)) {
        OPTIPISTON_PROBE_DO(if (enabled && isArm) probeArmDrawn(entity.mPosition.get(), position, nested);)
        OPTIPISTON_PROBE_DO(
            if (enabled && isArm) probeArmLight(entity, renderSource, position, bodyOffset, false, isCargo, nested);
        )
        draw();
        OPTIPISTON_PROBE_DO(probeArmLightEnd();)
        return;
    }
    // Shift the arm with its still-moving body.
    OPTIPISTON_PROBE_DO(++tArmTally.shifted;)
    auto const saved  = position;
    position.x       += bodyOffset->x;
    position.y       += bodyOffset->y;
    position.z       += bodyOffset->z;
    OPTIPISTON_PROBE_DO(if (enabled && isArm) probeArmDrawn(entity.mPosition.get(), position, nested);)
    OPTIPISTON_PROBE_DO(
        if (enabled && isArm) probeArmLight(entity, renderSource, position, bodyOffset, true, isCargo, nested);
    )
    draw();
    OPTIPISTON_PROBE_DO(probeArmLightEnd();)
    position = saved;
}

template <class Draw>
void drawMoving(::BaseActorRenderContext&, ::BlockSource& source, ::BlockActor& entity, Draw&& draw) {
    OPTIPISTON_PERF_SCOPE(perf, DrawMoving);
    OPTIPISTON_PROBE_DO(tProbeWhy = {};)
    bool const isMoving  = entity.mType == ::BlockActorType::MovingBlock;
    auto*      movingPtr = isMoving ? static_cast<::MovingBlockActor*>(&entity) : nullptr;

    RenderScope const  scope;
    CarrierScope const carrier(&entity);

    if (!smoothPistonRenderEnabled() || !isMoving) {
        OPTIPISTON_PROBE_DO(perf.pause();)
        draw();
        return;
    }

    auto& interlock = interlockOf(entity);
    gFrameDrewActors.store(true, std::memory_order_relaxed);

    // A MovingBlock rebuilt mid-animation starts hidden just like the piston does. Only lift that initial
    // state: the retirement logic below owns DelayedDestructionNotVisible and must not be overridden.
    if (interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
    }

    // Our own copy of a pushed block: drawn whenever its timeline says so.
    if (&entity == tProxyDraw) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        interlock.mHasBeenDelayedDeleted = false;
        if (animationActive()) holdPlacedCell(source, entity.mPosition.get());
        OPTIPISTON_PROBE_DO(probeProxyRendered(entity.mPosition.get()); perf.pause();)
        draw();
        return;
    }

    if (animationActive()) {
        snapshotMoving(*movingPtr, source, true);
        holdPlacedCell(source, entity.mPosition.get());
        // The timeline of this cell owns the picture; the game's instance would only add a second copy.
        if (vanillaHidden(*movingPtr, source.getBlockEntity(entity.mPosition) == &entity)) {
            OPTIPISTON_PROBE_DO(probeVanillaHidden(entity.mPosition.get());)
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
    }

    OPTIPISTON_PROBE_DO(probeGhost(*movingPtr, cellBlock, detached, arrived, owner != nullptr, interlock);
                        probeVanillaDraw(entity.mPosition.get());
                        perf.pause();)
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
    {
        OPTIPISTON_PERF_SCOPE(perf, Dispatch);
        if (landedActorCovered(e)) return;
        noteDispatched(e);
        noteCameraDispatch(e, forcedMat);
    }
    // The arm renderer this replaces only ever covered the opaque pass; a moving block can be drawn in either.
    if (e.mType == ::BlockActorType::MovingBlock) drawMoving(entityRenderContext, renderSource, e, draw);
    else if (renderAlphaLayer) draw();
    else if (e.mType == ::BlockActorType::PistonArm) drawArm(entityRenderContext, renderSource, e, position, draw);
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

// moving_block_blend writes depth, so translucent terrain drawn after a gliding glass vanishes behind it.
void relaxMovingBlendDepth(::MovingBlockActorRenderer& renderer) {
    thread_local uint32_t calls = 0;
    if ((calls++ & 255u) != 0) return;
    int index = 0;
    for (auto& slot : renderer.mBlockMaterials) {
        auto const* const info = slot.get().mRenderMaterialInfoPtr.get();
        auto* const       mat  = info ? info->mPtr.get() : nullptr;
        ++index;
        if (!mat || !mat->blendStateDescription.get().enableBlend) continue;
        auto& mask = mat->depthStencilStateDescription.get().depthWriteMask;
        if (static_cast<int>(mask) == 0) continue;
        mask = static_cast<::mce::DepthWriteMask>(0);
        OPTIPISTON_PROBE_DO(
            std::lock_guard const guard(gProbeMutex);
            probeLineLocked("materialRelaxed index=%d name=%s\n", index - 1, info->mHashedName.get().c_str());
        )
    }
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
    relaxMovingBlendDepth(*this);
    OPTIPISTON_PROBE_DO(probeMovingMaterials(*this);)
    drawMoving(renderContext, blockEntityRenderData.renderSource, renderedActor(blockEntityRenderData), [&] {
        origin(renderContext, blockEntityRenderData);
    });
}
#endif

#if OPTIPISTON_MC >= 2632
using DispatchedActor = ::IVanillaRenderBlockActorComponent;
::BlockActor& dispatchedActor(DispatchedActor& actor) { return actor.getBlockActor(); }
#else
using DispatchedActor = ::BlockActor;
::BlockActor& dispatchedActor(DispatchedActor& actor) { return actor; }
#endif

// Every block actor type reaches its renderer through the dispatcher; which overload the camera calls is not known,
// so both skip a covered landed actor.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonLandedActorHook,
    ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,
    static_cast<void (::BlockActorRenderDispatcher::*)(
        ::BaseActorRenderContext&,
        ::BlockSource&,
        DispatchedActor&,
        ::Block const&,
        bool,
        ::mce::MaterialPtr const&,
        ::mce::ClientTexture const*,
        int,
        std::optional<::dragon::RenderMetadata>
    )>(&::BlockActorRenderDispatcher::render),
    void,
    ::BaseActorRenderContext&               entityRenderContext,
    ::BlockSource&                          renderSource,
    DispatchedActor&                        e,
    ::Block const&                          block,
    bool                                    renderAlphaLayer,
    ::mce::MaterialPtr const&               forcedMat,
    ::mce::ClientTexture const*             forceTex,
    int                                     breakingAmount,
    std::optional<::dragon::RenderMetadata> renderMetadata
) {
    {
        OPTIPISTON_PERF_SCOPE(perf, Dispatch);
        if (landedActorCovered(dispatchedActor(e))) return;
        noteDispatched(dispatchedActor(e));
        noteCameraDispatch(dispatchedActor(e), forcedMat);
    }
    origin(
        entityRenderContext,
        renderSource,
        e,
        block,
        renderAlphaLayer,
        forcedMat,
        forceTex,
        breakingAmount,
        std::move(renderMetadata)
    );
}

// 26.10 already routes this overload through OptiPistonBlockActorDispatchHook.
#if OPTIPISTON_MC != 2610
LL_TYPE_INSTANCE_HOOK(
    OptiPistonLandedActorAtHook,
    ll::memory::HookPriority::Normal,
    BlockActorRenderDispatcher,
    static_cast<void (::BlockActorRenderDispatcher::*)(
        ::BaseActorRenderContext&,
        ::BlockSource&,
        DispatchedActor&,
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
    DispatchedActor&                        e,
    ::Block const&                          block,
    ::Vec3 const&                           renderPos,
    ::BlockPos const&                       worldPos,
    bool                                    renderAlphaLayer,
    ::mce::MaterialPtr const&               forcedMat,
    ::mce::ClientTexture const*             forceTex,
    int                                     breakingAmount,
    std::optional<::dragon::RenderMetadata> renderMetadata
) {
    {
        OPTIPISTON_PERF_SCOPE(perf, Dispatch);
        if (landedActorCovered(dispatchedActor(e))) return;
        noteDispatched(dispatchedActor(e));
        noteCameraDispatch(dispatchedActor(e), forcedMat);
    }
    origin(
        entityRenderContext,
        renderSource,
        e,
        block,
        renderPos,
        worldPos,
        renderAlphaLayer,
        forcedMat,
        forceTex,
        breakingAmount,
        std::move(renderMetadata)
    );
}
#endif

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
    MeshBuildScope scope(&geometry);
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
    MeshBuildScope scope(this);
    OPTIPISTON_PROBE_DO(ProbeRebuildClock clock;)
    OPTIPISTON_OBSERVE_DO(
        auto const observedPos = mPosition.get();
        observe::buildBegin(this, observedPos.x, observedPos.y, observedPos.z, mBuildVersionNumber);
    )
    origin(builder, lightingType, forExport, lightingModelCapabilities, caps);
    OPTIPISTON_OBSERVE_DO(observe::buildEnd();)
    OPTIPISTON_PROBE_DO(probeMeshBuildReturned(this, clock, &builder);)
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
    OPTIPISTON_PROBE_DO(tProbeCommitChain = probeCallChain(); tProbeCommitBuilder = &builder;)
    OPTIPISTON_OBSERVE_DO(auto const observedCommitT0 = observe::commitBegin();)
    origin(builder, bufferResourceService, isBuilding, alreadyHadGeometry, renderMetadata, useSplitStream);
    OPTIPISTON_OBSERVE_DO(observe::buildCommit(this, observedCommitT0);)
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
    OPTIPISTON_PROBE_DO(tProbeCommitChain = probeCallChain(); tProbeCommitBuilder = &builder;)
    origin(builder, bufferResourceService, isBuilding, renderMetadata, useSplitStream);
    tMeshGeometry = nullptr;
    meshBuildCommitted(this);
}
#endif

// Passes without block actors (export) do not count, so "next frame" always means a frame that drew them.
void onFrameEnd() {
    OPTIPISTON_PERF_SCOPE(perf, Frame);
    // Backs up the piston tick: with no piston left ticking, nothing else would end the visuals or free the proxies.
    if (gStateWatch.load(std::memory_order_relaxed)) {
        auto  client = ll::service::getClientInstance();
        auto* player = client ? client->getLocalPlayer() : nullptr;
        if (player) {
            OPTIPISTON_PERF_SCOPE(sweep, SweepActions);
            sweepActions(player->getDimensionBlockSource());
        }
    }
    if (!gFrameDrewActors.exchange(false, std::memory_order_relaxed)) return;
    {
        OPTIPISTON_PERF_SCOPE(early, ReleaseEarly);
        releaseEarlyCells();
    }
    OPTIPISTON_PROBE_DO(perf.pause(); probeFrameEnd(); perf.resume();)
    advanceFrame();
}

#if OPTIPISTON_PROBE
// Start of a frame hook call, on the render thread.
void probeFrameBegin() {
    tRenderThread = true;
    gFrameLt.store(visualClockTick().value_or(-1), std::memory_order_relaxed);
}
#endif

#if OPTIPISTON_MC == 2620
LL_TYPE_INSTANCE_HOOK(
    OptiPistonFrameHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::endFrame,
    void,
    ::mce::TextureResourceService& textureResourceService
) {
    OPTIPISTON_PROBE_DO(probeFrameBegin();)
    origin(textureResourceService);
    onFrameEnd();
    OPTIPISTON_PROBE_DO(probeRealFrameEnd();)
    OPTIPISTON_PROBE_DO(gRealFrame.fetch_add(1, std::memory_order_relaxed);)
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
    OPTIPISTON_PROBE_DO(probeFrameBegin();)
    OPTIPISTON_OBSERVE_DO(observe::frameBegin();)
    origin(screenContext, renderObj);
    OPTIPISTON_OBSERVE_DO(observe::frameEnd();)
    onFrameEnd();
    OPTIPISTON_PROBE_DO(probeRealFrameEnd();)
    OPTIPISTON_PROBE_DO(gRealFrame.fetch_add(1, std::memory_order_relaxed);)
}
#endif

// Runs right after the main camera collected its entities: the game's MovingBlocks of a fresh action are found and
// copied before the first draw of the frame.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonPlayerQueueEntitiesHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$queueRenderEntities,
    void,
    ::LevelRenderPreRenderUpdateParameters const& parameters
) {
    origin(parameters);
    OPTIPISTON_PERF_SCOPE(perf, QueueEntities);
    if (!smoothPistonRenderEnabled() || !gStateWatch.load(std::memory_order_relaxed)) return;
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (player) scanTargets(player->getDimensionBlockSource());
}

// The proxies are drawn by us, once per pass, after everything the camera queued.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonRenderBlockEntitiesHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    ::BaseActorRenderContext& renderContext,
    bool                      renderAlphaLayer
) {
    OPTIPISTON_PROBE_DO(probeRenderBlockEntities(renderAlphaLayer);)
    OPTIPISTON_OBSERVE_DO(observe::renderBlockEntities(renderAlphaLayer);)
    RenderViewScope scope;
    origin(renderContext, renderAlphaLayer);
    if (smoothPistonRenderEnabled() && gStateWatch.load(std::memory_order_relaxed) && animationActive())
        drawProxies(renderContext, renderAlphaLayer);
}

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
    OPTIPISTON_PERF_SCOPE(perf, DataHandler);
    std::optional<HeldPistonData> held;
    if (packet && isPistonData(*packet->mData)) {
        if (auto const clock = visualClock()) held = HeldPistonData{packet->mPos, *clock, packet->mData->clone()};
    }
    tBlockActorDataApplied = false;
    ++tBlockActorDataDepth;
    OPTIPISTON_PROBE_DO(perf.pause();)
    origin(source, std::move(packet));
    OPTIPISTON_PROBE_DO(perf.resume();)
    --tBlockActorDataDepth;
    if (!held || tBlockActorDataApplied) return;
    std::lock_guard const guard(gAnimMutex);
    // Data from an earlier clock run, or older than the longest visual, could only start an already expired visual.
    // A live world never changes run, so the age bound is what keeps this list short.
    auto const maxAge = static_cast<int64_t>(std::ceil(core::MaxDurationTicks));
    std::erase_if(gHeldPistonData, [&](HeldPistonData const& entry) {
        return !sameClockRun(entry.clock, held->clock) || held->clock.tick - entry.clock.tick > maxAge;
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
    OPTIPISTON_PERF_SCOPE(perf, PistonAction);
    std::optional<int64_t> heldTick;
    if (tBlockActorDataDepth > 0) tBlockActorDataApplied = true;
    else heldTick = takeHeldTick(mPosition.get(), data);
    if (!animationActive() || mState == before) return;
    if (mState != ::PistonState::Expanding && mState != ::PistonState::Retracting) return;
    // Late data with no recorded arrival cannot be placed on the timeline; native state already reflects it.
    if (tBlockActorDataDepth == 0 && !heldTick) return;
    startAction(
        region,
        mPosition.get(),
        facingOf(*this, region),
        mState == ::PistonState::Expanding,
        mAttachedBlocks.get(),
        heldTick
    );
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
    OPTIPISTON_PERF_SCOPE(perf, DrawPos);
    OPTIPISTON_OBSERVE_DO(observe::drawPos(*this, a, tProxyDraw == this);)
    if (tRenderDepth > 0 && animationActive()) {
        if (auto const offset = visualDrawOffset(*this, a)) {
            OPTIPISTON_PROBE_DO(probeProxyDrawPos(
                                    mPosition.get(),
                                    std::max({std::abs(offset->x), std::abs(offset->y), std::abs(offset->z)}),
                                    &*offset
            );)
            return *offset;
        }
    }
    OPTIPISTON_PROBE_DO(if (tProxyDraw == this) probeProxyDrawPos(mPosition.get(), 0.0f, nullptr);)
    OPTIPISTON_PROBE_DO(perf.pause();)
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
    OPTIPISTON_PERF_SCOPE(perf, LandedBlock);
    if (animationActive() && gFaceWatch.load(std::memory_order_relaxed))
        holdLandedBlocks(*packet.mBlocksChanged->mStandards);
    OPTIPISTON_PROBE_DO(perf.pause();)
    origin(source, packet);
}

// Hiding only tessellateBlockInWorld left a landed hopper beside its proxy (26.50).
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
    OPTIPISTON_PERF_SCOPE(perf, MeshHook);
    OPTIPISTON_OBSERVE_DO(observe::tessellate(pos.x, pos.y, pos.z, &block);)
    if (meshSuppressed(pos, block)) {
        OPTIPISTON_PROBE_DO(probeMeshCall(true, pos, true);)
        OPTIPISTON_OBSERVE_DO(observe::tessellated(pos.x, pos.y, pos.z, &block, false, true);)
        return false;
    }
    OPTIPISTON_PROBE_DO(probeMeshCall(true, pos, false);)
    OPTIPISTON_PROBE_DO(perf.pause();)
    auto const result = origin(tessellator, block, pos, useCalcWithCache);
    OPTIPISTON_OBSERVE_DO(observe::tessellated(pos.x, pos.y, pos.z, &block, result, false);)
    OPTIPISTON_PROBE_DO(probeMeshTessellated(pos, block, result);)
    return result;
}

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
    OPTIPISTON_PERF_SCOPE(perf, MeshHook);
    OPTIPISTON_OBSERVE_DO(observe::tessellate(pos.x, pos.y, pos.z, &block);)
    if (meshSuppressed(pos, block)) {
        OPTIPISTON_PROBE_DO(probeMeshCall(false, pos, true);)
        OPTIPISTON_OBSERVE_DO(observe::tessellated(pos.x, pos.y, pos.z, &block, false, true);)
        return false;
    }
    OPTIPISTON_PROBE_DO(probeMeshCall(false, pos, false);)
    bool const noCache = airAndSimpleBlocks && touchesLanded(pos);
    OPTIPISTON_PROBE_DO(perf.pause();)
    auto const result = origin(tessellator, block, pos, faces, noCache ? nullptr : airAndSimpleBlocks);
    OPTIPISTON_OBSERVE_DO(observe::tessellated(pos.x, pos.y, pos.z, &block, result, false);)
    OPTIPISTON_PROBE_DO(probeMeshTessellated(pos, block, result);)
    return result;
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
    OPTIPISTON_PERF_SCOPE(perf, FaceOcclusion);
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
    OPTIPISTON_PROBE_DO(probeAreaChain(tImmediateRebuild);)
    if (!tImmediateRebuild) {
        origin(source, min, max);
        return;
    }
    // Our own asks only need the immediate dirty; the native one stamps the chunk's change ticks and its commit waits.
    OPTIPISTON_PROBE_DO(probeAreaAsk(min, max);)
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

#ifndef OPTIPISTON_LIGHT_HOOKS
#define OPTIPISTON_LIGHT_HOOKS 4
#endif
// Off by default: its per-call thread-local read would skew the light hook's own cost.
#ifndef OPTIPISTON_LIGHT_LOG
#define OPTIPISTON_LIGHT_LOG 0
#endif
#if OPTIPISTON_MC >= 2651
// A user-provided copy constructor makes the compiler return it through a hidden pointer, like the game's BrightnessPair.
struct LightRet {
    std::uint16_t v{}; // low byte sky, high byte block

    LightRet() {}

    LightRet(LightRet const& other) : v(other.v) {}

    LightRet& operator=(LightRet const& other) {
        v = other.v;
        return *this;
    }
};

using LightColorFn = LightRet (::BlockSource::*)(::BlockPos const&, std::uint8_t) const;
#endif

#if OPTIPISTON_MC == 2651
// ABI recovered from 1.26.51.01: pair returned through a hidden pointer, minBlockLight a plain byte. Outside an
// ArmLightFixScope window this costs one thread-local read.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonLightColorHook,
    ll::memory::HookPriority::Normal,
    ::BlockSource,
    reinterpret_cast<LightColorFn>(&::BlockSource::getLightColor),
    LightRet,
    ::BlockPos const& pos,
    std::uint8_t      minBlockLight
) {
    OPTIPISTON_PROBE_DO(
        thread_local uint32_t sampleSeq = 0;
        bool const           timed      = ((++sampleSeq) & (kLightSample - 1)) == 0;
        auto const           gameStart  = timed ? perfNow() : 0ull;
    )
    LightRet const raw = origin(pos, minBlockLight);
    OPTIPISTON_PROBE_DO(auto const gameEnd = timed ? perfNow() : 0ull;)
    OPTIPISTON_PROBE_DO(if (tProbeProxyLight.active) {
        ++tProbeProxyLight.queries;
        if (raw.v == 0) {
            ++tProbeProxyLight.dark;
            auto const& own = tProbeProxyLight.cell;
            if (pos.x == own.x && pos.y == own.y && pos.z == own.z) ++tProbeProxyLight.selfDark;
        }
    })
    LightRet    result = raw;
    auto const& fix    = tArmLightFix;
    if (fix.active && pos.x == fix.head.x && pos.y == fix.head.y && pos.z == fix.head.z) {
        // An opaque cell stores 0/0; any other result is the game's own and stays as it is.
        if (raw.v == 0) result = origin(fix.piston, minBlockLight);
        OPTIPISTON_PROBE_DO(
            gLightFixHeadQueries.fetch_add(1, std::memory_order_relaxed);
            if (raw.v == 0) gLightFixRawDark.fetch_add(1, std::memory_order_relaxed);
            if (result.v != raw.v) gLightFixRewrites.fetch_add(1, std::memory_order_relaxed);
        )
    }
    // A proxy stands in the cell the logic has already settled: once the real block is there the game stores 0/0
    // for it and the moving copy renders dark. Keep the cell's own last lit answer for the rest of the animation.
    auto const& proxyFix = tProxyLightFix;
    if (proxyFix.active && pos.x == proxyFix.cell.x && pos.y == proxyFix.cell.y && pos.z == proxyFix.cell.z) {
        if (raw.v != 0) {
            proxyFix.memory->store(raw.v, std::memory_order_relaxed);
        } else if (result.v == 0) {
            auto const kept = proxyFix.memory->load(std::memory_order_relaxed);
            if (kept != 0) result.v = kept;
            OPTIPISTON_PROBE_DO(
                (kept != 0 ? gProbe.lightSelfFixed : gProbe.lightSelfNoMem).fetch_add(1, std::memory_order_relaxed);
            )
        }
    }
    OPTIPISTON_PROBE_DO(
        if (OPTIPISTON_LIGHT_LOG && (OPTIPISTON_LIGHT_HOOKS & 4) && tArmLightCtx.active) {
            int const sky   = raw.v & 0xFF;
            int const block = raw.v >> 8;
            probeLightQuery(
                *this,
                2,
                pos,
                sky == 0 && block == 0,
                static_cast<float>(std::max(sky, block)) / 15.0f,
                sky,
                block,
                minBlockLight
            );
        }
    )
    OPTIPISTON_PROBE_DO(if (timed) {
        auto const kind = tRenderThread ? 0 : 1;
        gPerfCounts2.lightSeen[kind].fetch_add(1, std::memory_order_relaxed);
        gPerfCounts2.lightGameTicks[kind].fetch_add(gameEnd - gameStart, std::memory_order_relaxed);
        gPerfCounts2.lightOwnTicks[kind].fetch_add(perfNow() - gameEnd, std::memory_order_relaxed);
    })
    return result;
}

bool gLightFixInstalled{};
#define OPTIPISTON_INSTALL_LIGHT_FIX() (void)installHook<OptiPistonLightColorHook>(gLightFixInstalled)
#define OPTIPISTON_REMOVE_LIGHT_FIX()  removeHook<OptiPistonLightColorHook>(gLightFixInstalled)
#else
#define OPTIPISTON_INSTALL_LIGHT_FIX() ((void)0)
#define OPTIPISTON_REMOVE_LIGHT_FIX()  ((void)0)
#endif

#if OPTIPISTON_PROBE && OPTIPISTON_MC >= 2651 && OPTIPISTON_LIGHT_HOOKS
// Count the game's own light queries while an arm is drawn. Failing to install them is not fatal.
LL_TYPE_INSTANCE_HOOK(
    OptiPistonLightBrightnessHook,
    ll::memory::HookPriority::Normal,
    ::BlockSource,
    &::BlockSource::$getBrightness,
    float,
    ::BlockPos const& pos
) {
    float const value = origin(pos);
    if (OPTIPISTON_LIGHT_LOG && tArmLightCtx.active) probeLightQuery(*this, 0, pos, value < 0.05f, value);
    return value;
}

using LightPairFn = LightRet (::BlockSource::*)(::BlockPos const&) const;

LL_TYPE_INSTANCE_HOOK(
    OptiPistonLightPairHook,
    ll::memory::HookPriority::Normal,
    ::BlockSource,
    reinterpret_cast<LightPairFn>(&::BlockSource::getBrightnessPair),
    LightRet,
    ::BlockPos const& pos
) {
    LightRet const pair = origin(pos);
    if (OPTIPISTON_LIGHT_LOG && tArmLightCtx.active) {
        int const sky   = pair.v & 0xFF;
        int const block = pair.v >> 8;
        probeLightQuery(*this, 1, pos, sky == 0 && block == 0, static_cast<float>(std::max(sky, block)) / 15.0f, sky, block);
    }
    return pair;
}


bool gLightHooks[3]{};

void dumpLightTarget(char const* name, ll::memory::FuncPtr target, std::size_t count) {
    auto const base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto const addr = reinterpret_cast<std::uintptr_t>(target);
    auto const* src = reinterpret_cast<unsigned char const*>(addr);
    char        hex[3 * 128 + 1]{};
    for (std::size_t i = 0; i < count && i < 128; ++i) std::snprintf(hex + i * 3, 4, "%02x ", src[i]);
    std::lock_guard const guard(gProbeMutex);
    probeLineLocked("lightTarget %s rva=0x%llx bytes=%s\n", name, static_cast<unsigned long long>(addr - base), hex);
}

void installLightProbeHooks() {
    dumpLightTarget("getBrightness", ll::memory::resolveIdentifier(&::BlockSource::$getBrightness), 64);
    dumpLightTarget("getBrightnessPair", ll::memory::resolveIdentifier(reinterpret_cast<LightPairFn>(&::BlockSource::getBrightnessPair)), 64);
    dumpLightTarget("getLightColor", ll::memory::resolveIdentifier(reinterpret_cast<LightColorFn>(&::BlockSource::getLightColor)), 128);
    if (OPTIPISTON_LIGHT_HOOKS & 1) (void)installHook<OptiPistonLightBrightnessHook>(gLightHooks[0]);
    if (OPTIPISTON_LIGHT_HOOKS & 2) (void)installHook<OptiPistonLightPairHook>(gLightHooks[1]);

}

void removeLightProbeHooks() {

    removeHook<OptiPistonLightPairHook>(gLightHooks[1]);
    removeHook<OptiPistonLightBrightnessHook>(gLightHooks[0]);
}
#define OPTIPISTON_INSTALL_LIGHT_PROBE() installLightProbeHooks()
#define OPTIPISTON_REMOVE_LIGHT_PROBE()  removeLightProbeHooks()
#else
#define OPTIPISTON_INSTALL_LIGHT_PROBE() ((void)0)
#define OPTIPISTON_REMOVE_LIGHT_PROBE()  ((void)0)
#endif

// Hooks in install order; unhooked in reverse. The action hook is last, so no visual starts before everything
// that finishes it is in place.
#if OPTIPISTON_MC == 2610
#define OPTIPISTON_RENDER_HOOKS(X) X(OptiPistonBlockActorDispatchHook) X(OptiPistonLandedActorHook)
#else
#define OPTIPISTON_RENDER_HOOKS(X)                                                                                     \
    X(OptiPistonPistonArmHook)                                                                                         \
    X(OptiPistonMovingBlockGhostHook) X(OptiPistonLandedActorHook) X(OptiPistonLandedActorAtHook)
#endif

#if OPTIPISTON_MC == 2620
#define OPTIPISTON_PROGRESS_HOOK(X) X(OptiPistonPistonProgressHook)
#else
#define OPTIPISTON_PROGRESS_HOOK(X)
#endif
#if OPTIPISTON_MC == 2620 || OPTIPISTON_MC == 2632
#define OPTIPISTON_START_REBUILD_HOOK(X) X(OptiPistonStartRebuildHook)
#else
#define OPTIPISTON_START_REBUILD_HOOK(X)
#endif

#define OPTIPISTON_ALL_HOOKS(X)                                                                                        \
    X(OptiPistonPistonArmVisibilityHook)                                                                               \
    OPTIPISTON_RENDER_HOOKS(X)                                                                                         \
    X(OptiPistonPistonStepHook)                                                                                        \
    X(OptiPistonMeshInWorldHook)                                                                                       \
    X(OptiPistonMeshBlockInWorldHook)                                                                                  \
    X(OptiPistonFaceOcclusionHook)                                                                                     \
    X(OptiPistonImmediateRebuildHook)                                                                                  \
    OPTIPISTON_START_REBUILD_HOOK(X)                                                                                   \
    X(OptiPistonRebuildHook)                                                                                           \
    X(OptiPistonEndRebuildHook)                                                                                        \
    X(OptiPistonFrameHook)                                                                                             \
    X(OptiPistonPlayerQueueEntitiesHook)                                                                               \
    X(OptiPistonRenderBlockEntitiesHook)                                                                               \
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

bool observeMark(std::string const& label) {
#if OPTIPISTON_OBSERVE
    observe::mark(label);
    return true;
#else
    (void)label;
    return false;
#endif
}

bool hookPistonRender(bool enable) {
    static bool installed[std::size(kHooks)]{};

    if (enable) {
        if (gInstalled.load(std::memory_order_acquire)) return true;
        OPTIPISTON_PROBE_DO(probeOpen();)
        for (std::size_t i = 0; i < std::size(kHooks); ++i) {
            if (!kHooks[i].install(installed[i])) return false;
        }
        OPTIPISTON_INSTALL_LIGHT_FIX();
        OPTIPISTON_INSTALL_LIGHT_PROBE();
        OPTIPISTON_OBSERVE_DO(observe::install();)
        gInstalled.store(true, std::memory_order_release);
        return true;
    }

    if (!gInstalled.load(std::memory_order_acquire)) return true;

    OPTIPISTON_OBSERVE_DO(observe::remove();)
    OPTIPISTON_REMOVE_LIGHT_PROBE();
    OPTIPISTON_REMOVE_LIGHT_FIX();
    for (std::size_t i = std::size(kHooks); i-- > 0;) kHooks[i].remove(installed[i]);

    {
        std::lock_guard const guard(gHeadMutex);
        gSteppedPistons.clear();
    }
    gHeadEpoch.fetch_add(1, std::memory_order_relaxed);
    clearAnimationState();
    OPTIPISTON_PROBE_DO(probeClose();)

    gInstalled.store(false, std::memory_order_release);
    return true;
}

} // namespace optipiston::platform
