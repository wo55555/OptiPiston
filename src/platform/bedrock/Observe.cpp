// Stage S0 of the overlay rewrite, observation probes version 2 (r59), for the design in optipiston-design.md.
//   P1  promote vs. block-actor draw order and thread (counters + per-period token signature)
//   P2  does a build read the live chunk or a copy (counters + offset histogram)
//   P3  geometry pointer / build version reuse
//   P4  client MovingBlockActor ctor/load/update/tick (render thread only, per tracked cell)
//   P5  moving_block -> air gap (per cell, in frames and ticks, split by what ends the gap)
//   P6  getDrawPos thread and alpha
//   P7  vanilla terrain-interlock visibility state of every drawn moving block actor
//   P8  per-cell visibility oracle: absent / double / ghost frames, from the real draw calls and the real mesh content
//   P9  dirty -> build begin -> commit -> promote latencies (ms and ticks), tracked changes vs. other changes
//   P10 per-frame cost census, hitch lines with a breakdown of what ran in the period
//   P11 cause of every block change (packet, moving block tick, piston tick, other)
//   P14 segments: /optipiston mark <label> closes the segment with summary lines and starts a new one
// Everything is aggregated per segment; individual lines are limited per category and segment.
#include "platform/bedrock/Observe.h"

#if OPTIPISTON_OBSERVE

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/Overlay.h"
#include "core/Settings.h"
#include "platform/Features.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/client/renderer/chunks/RenderChunkGeometry.h"
#include "mc/client/renderer/chunks/RenderChunkInstanced.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/world/actor/ActorTerrainInterlockData.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/MovingBlockActor.h"
#include "mc/world/level/block/actor/PistonBlockActor.h"
#include "mc/world/level/block/actor/VanillaBlockActor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace optipiston::platform::observe {

namespace {

struct PosHash {
    [[nodiscard]] size_t operator()(::BlockPos const& pos) const noexcept {
        return (static_cast<size_t>(static_cast<uint32_t>(pos.x)) * 73856093u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.y)) * 19349663u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.z)) * 83492791u);
    }
};

uint64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

double qpcMs(uint64_t ticks) {
    static double const perMs = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(frequency.QuadPart) / 1000.0;
    }();
    return static_cast<double>(ticks) / perMs;
}

double signedMs(uint64_t a, uint64_t b) { return a >= b ? qpcMs(a - b) : -qpcMs(b - a); }

using ull = unsigned long long;

std::atomic<bool>     gOn{false};
std::atomic<uint64_t> gFrame{0};
std::atomic<DWORD>    gRenderTid{0};
std::atomic<bool>     gInFrame{false};
std::atomic<bool>     gRbeSeen{false};
std::atomic<uint64_t> gSeq{0};      // bumped for every layer-0 block change
std::atomic<uint64_t> gTrackSeq{0}; // bumped for every tracked cell birth

bool        onRenderThread() { return GetCurrentThreadId() == gRenderTid.load(std::memory_order_relaxed); }
int         modeIndex() { return core::pistonSettings().enabled.load(std::memory_order_relaxed) ? 1 : 0; }
char const* modeName(int mode) { return mode ? "on" : "off"; }

// World speed in effect: 1x / slow / fast.
int speedClass() {
    float const applied = core::worldSpeedSettings().applied.load(std::memory_order_relaxed);
    return applied < 1.0f ? 1 : applied > 1.0f ? 2 : 0;
}

int64_t levelTick() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    return player ? static_cast<int64_t>(player->getLevel().getCurrentTick().tickID) : -1;
}

enum class Cls : uint8_t { None, Air, Moving, Real };

bool isMoving(::Block const& block) { return block.getTypeName() == "minecraft:moving_block"; }

Cls classify(::Block const& block) {
    if (block.isAir()) return Cls::Air;
    return isMoving(block) ? Cls::Moving : Cls::Real;
}

char clsLetter(Cls cls) {
    switch (cls) {
    case Cls::Air:
        return 'A';
    case Cls::Moving:
        return 'M';
    case Cls::Real:
        return 'R';
    default:
        return '-';
    }
}

int kindOf(Cls cls) { return cls == Cls::Moving ? 0 : cls == Cls::Air ? 1 : 2; }

enum Cause : uint8_t {
    CauseNone,
    CauseSubChunk,
    CauseUpdateBlock,
    CauseSynced,
    CauseMbaTick,
    CausePistonTick,
    CauseCount
};
constexpr char const* kCauseName[CauseCount]   = {"none", "subchunk", "updateBlock", "synced", "mbaTick", "pistonTick"};
constexpr char        kCauseLetter[CauseCount] = {'-', 'S', 'U', 'Y', 'T', 'K'};

thread_local Cause tCause = CauseNone;

struct CauseScope {
    Cause saved;
    explicit CauseScope(Cause cause) : saved(tCause) { tCause = cause; }
    ~CauseScope() { tCause = saved; }
};

// ---- bucketing -------------------------------------------------------------------------------------------------
constexpr double kFrameEdges[7]  = {8.0, 12.0, 16.7, 25.0, 33.4, 50.0, 100.0};
constexpr double kLatEdges[7]    = {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0};
constexpr double kOffEdges[7]    = {-100.0, -20.0, -5.0, 0.0, 2.0, 5.0, 20.0};
constexpr double kMarginEdges[7] = {0.05, 0.2, 1.0, 3.0, 10.0, 30.0, 100.0};

int msBucket(double value, double const (&edges)[7]) {
    int index = 0;
    while (index < 7 && value >= edges[index]) ++index;
    return index;
}

int tickBucket(int64_t ticks) {
    if (ticks <= 0) return 0;
    if (ticks <= 2) return 1;
    if (ticks <= 5) return 2;
    if (ticks <= 10) return 3;
    if (ticks <= 20) return 4;
    if (ticks <= 40) return 5;
    return 6;
}

int runBucket(uint32_t run) {
    if (run <= 1) return 0;
    if (run == 2) return 1;
    if (run <= 5) return 2;
    if (run <= 20) return 3;
    return 4;
}

int burstBucket(uint32_t count) {
    if (count == 0) return 0;
    if (count == 1) return 1;
    if (count <= 3) return 2;
    if (count <= 7) return 3;
    if (count <= 15) return 4;
    return 5;
}

// ---- data model ------------------------------------------------------------------------------------------------
struct Change {
    uint64_t       seq{};
    ::Block const* now{};
    ::Block const* before{};
    uint64_t       enter{}; // listener entry; the chunk write happened before this
    uint64_t       done{};  // listener return
};

struct ItemRec {
    uint64_t       key{};
    ::Block const* block{};
    bool           produced{};
    bool           suppressed{};
    int            x{}, y{}, z{};
};

struct BuildRes {
    ::BlockPos                            pos{};
    unsigned char                         version{};
    uint64_t                              trackSeq{};   // gTrackSeq when the build started
    uint64_t                              seqAtBegin{}; // gSeq when the build started
    uint64_t                              ledgerKey{};  // evidence ledger key; 0 until committed
    uint64_t                              dirtyQpc{}, beginQpc{}, endQpc{}, commitQpc{};
    int64_t                               dirtyTick{-1};
    bool                                  trackedDirty{false};
    std::vector<ItemRec>                  items; // appended on the build thread
    std::unordered_map<uint64_t, ItemRec> idx;   // built at commit
};

struct Sub {
    uint64_t dirtyQpc{};
    int64_t  dirtyTick{-1};
    bool     tracked{false};
};

struct Cell {
    ::BlockPos     pos{};
    uint64_t       key{};
    uint64_t       bornSeq{};
    uint64_t       chgSeq{};
    uint64_t       birthFrame{};
    int64_t        birthTick{-1};
    int            birthMode{0};
    bool           dest{false}, source{false}, confirmed{false};
    bool           sawMoving{false}, airFromMoving{false};
    bool           meshHas{false}, noGeom{false};
    Cls            cls{Cls::None};
    ::Block const* block{};
    std::string    realName;
    uint64_t       lastActivity{0};
    uint64_t       lastDrawFrame{0}, lastVisDrawFrame{0};
    uint32_t       drawFrames{0}, drawCalls{0}, ticks{0};
    int            visState{-1};
    bool           dd{false};
    bool           preserved{false};
    bool           relPending{false}, landPending{false}, relOpen{false};
    ::Block const* relBlock{};
    uint64_t       relSeq{}, relQpc{}, landSeq{}, landQpc{};
    int64_t        relTick{-1}, landTick{-1};
    uint32_t       epAir{0}, epNoVis{0};
    uint32_t       absent[3]{}, dbl[3]{}, ghost{0};
    uint32_t       runAbsent{0}, runDbl{0}, runGhost{0}, maxAbsent{0}, maxDbl{0}, maxGhost{0};
    std::string    log;
    bool           logCut{false};
};

// Per-period (render frame start to next frame start) scratch, includes the events between two renderLevel calls.
struct Fr {
    uint32_t    promoteN{0}, promoteChanged{0};
    double      promoteMs{0};
    uint32_t    listenerN{0};
    double      listenerMs{0};
    uint32_t    loadN{0};
    double      loadMs{0};
    uint32_t    commitN{0};
    double      commitMs{0};
    uint32_t    drawN{0};
    uint32_t    bcN{0};
    std::string sig;
};

struct Seg {
    std::string label{"start"};
    uint32_t    index{0};
    uint64_t    f0{0}, q0{0};
    int64_t     lt0{-1};
    uint64_t    frames[2]{};
    uint64_t    dt[2][2][8]{};
    double      dtSum[2][2]{}, dtMax[2][2]{};
    uint64_t    rl[2][2][8]{};
    double      rlMax[2][2]{};
    uint64_t    burst[2][6]{};
    uint64_t    lat[2][2][8]{};
    uint64_t    latTicks[2][2][7]{};
    uint64_t    latUnknown[2]{};
    uint64_t    d2b[2][8]{}, b2c[2][8]{}, c2p[2][8]{};
    uint64_t    evalFrames[2]{}, absent[2][3]{}, dbl[2][3]{}, ghost[2]{}, absentVisMb[2]{};
    uint64_t    absentRun[2][5]{}, dblRun[2][5]{}, ghostRun[2][5]{};
    uint64_t    cellsClosed[2]{}, cellsBad[2]{}, cellsEndAbsent[2]{}, cellsEndGhost[2]{}, cellsNoGeom[2]{};
    uint64_t    gapEp[2][2]{}, gapFr[2][2]{}, gapNoVis[2][2]{};
    uint64_t    cause[CauseCount][4][4]{};
    uint64_t    vis[2][3][2]{};
    uint64_t    drawUntracked[2]{};
    uint64_t    p2off[3][8]{};
    uint64_t    p2age[8]{};
    uint64_t    slowSub[2]{};
    // P12 premise (c): a build that read the sequence number after a change must see that change.
    uint64_t preOk[2]{}, preStale[2]{}, preOther[2]{};
    uint64_t preStaleAge[2][8]{};
    uint64_t preMargin[2][8]{}; // begin - listener entry, for changes the build must see
    uint64_t gateR[2][3]{};     // ignores later changes: resolved / still showed the old block / cell landed first
    uint64_t gateRTicks[2][7]{}, gateRMs[2][8]{};
    uint64_t seenReal[2]{}, missReal[2]{};
    // P13 what the evidence rule would have cost: change -> first promoted build that began after it.
    uint64_t gate[2][2]{}; // [mode][clean / contradicted by a build that still showed another block]
    uint64_t gateTicks[2][7]{}, gateMs[2][8]{};
    uint64_t land[2][5]{}; // [mode][present / culled / suppressed / other block / missing]
    uint64_t landTicks[2][7]{}, landMs[2][8]{};
    // S2 shadow overlay: the rewrite's core on the real event stream (acts on nothing).
    uint64_t shStart[2]{}, shDeferred[2]{}, shNever[2]{}, shNoPiston[2]{}, shAsks[2]{}, shReset{0};
    uint64_t shEnd[2][5]{};     // [mode][none / mesh / culled / superseded / abandoned]
    uint64_t shStarved[2][3]{}; // abandoned while Gated / Animating / Holding
    uint64_t shGate[2][8]{}, shGateTicks[2][7]{}, shAnim[2][8]{}, shHold[2][8]{}, shTotal[2][8]{}, shNat[2][8]{};
    uint64_t shMesh[2][4]{};   // verdict Mesh: world agrees / other block / world moved on / cell unknown
    uint64_t shCulled[2][2]{}; // verdict Culled: a later qualifying build confirmed absent / produced the cell
    uint64_t shAbsent[2][3][2]{}, shGhost[2][2]{}; // oracle frames the shadow covers [0] / does not cover [1]
    // S2 diagnostics: which events explain the shadow's open questions.
    uint64_t spdFrames[3]{}, speedChanges{0}, spdTicks[3]{};     // frames and level ticks at 1x / slow / fast
    double   spdMs[3]{};                                         // wall time of those ticks, pauses included
    uint64_t shSpdEnd[3][5]{}, shSpdGate[3][8]{};                // the end and gate stats again, split by world speed
    uint64_t shSupNoCtx[2]{}, shSup[2][3][3]{};                  // [mode][chain / takeover / neither][same / state / other]
    uint64_t shChainLen[2][3][6]{}, shChainMs[2][8]{};           // chain tail: mesh / culled / abandoned by chain depth
    uint64_t shLoads[2]{}, shNeverDup[2]{};                      // derived loads; replaced pendings with the same dst
    uint64_t shNeverWhy[2][4]{}, shNeverSrc[2][4]{}, shNeverVac[2][4]{}, shNeverSeen[2][4]{};
    uint64_t shNeverDir[2][2]{}, shNeverDst[2][3]{}, shNeverLiveDst[2]{};
    uint64_t shUncovAbs[2][3][8]{}, shUncovGhost[2][8]{}, shHistOverflow{0};
    uint64_t shCulledNb[2][4]{}, shWatchEnd[2][5]{};
    std::unordered_map<std::string, uint64_t> neverName;
    std::unordered_set<uint64_t>              uncovSeen, supSeen; // cells / signatures already shown as samples
    std::unordered_map<std::string, uint64_t> sig;
    int                                       used[16]{};
};

enum Cat {
    CatCellBad,
    CatCellOk,
    CatHitch,
    CatSlow,
    CatMode,
    CatMisc,
    CatPremise,
    CatShadow,
    CatShadowOdd,
    CatShSup,
    CatShNever,
    CatShUncov,
    CatShNb,
    CatSpeed
};

struct Counters {
    std::atomic<uint64_t> promoteTotal{0}, promoteChanged{0}, promoteBeforeRbe{0}, promoteAfterRbe{0},
        promoteRenderOutside{0}, promoteOtherThread{0}, frames{0}, rbeCalls[2]{};
    std::atomic<uint64_t> p2Live{0}, p2Snapshot{0}, p2Ambiguous{0}, p2Third{0};
    std::atomic<uint64_t> buildBegin{0}, buildCommit{0}, reuseInFlight{0}, commitNoBegin{0}, verRepeat{0},
        inFlightMax{0}, commitOffRender{0};
    std::atomic<uint64_t> bcTotal{0}, bcMoving{0}, mbToBlock{0}, mbToAir{0};
    std::atomic<uint64_t> mbaCtorRender{0}, mbaCtorOther{0}, mbaLoad{0}, mbaUpdate{0}, mbaTickRender{0},
        mbaTickOther{0};
    std::atomic<uint64_t> dpRender{0}, dpOther{0}, dpProxy{0}, dpAlpha0{0}, dpAlpha1{0}, dpAlphaMid{0};
    std::atomic<uint64_t> joinPtr{0}, joinPos{0}, joinMiss{0};
    std::atomic<uint64_t> cellsCreated{0}, cellsConfirmed{0}, cellsExpired{0}, cellsOverflow{0};
};
Counters gC;

// ---- logging ---------------------------------------------------------------------------------------------------
std::mutex  gLogMutex;
std::FILE*  gFile = nullptr;
std::string gLastSummary;

template <class... Args>
void rawLog(char const* format, Args... args) {
    std::lock_guard const guard(gLogMutex);
    if (!gFile) return;
    std::fprintf(
        gFile,
        "f=%llu tid=%lu ",
        static_cast<ull>(gFrame.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(GetCurrentThreadId())
    );
    std::fprintf(gFile, format, args...);
    std::fflush(gFile);
}

void app(std::string& out, char const* format, ...) {
    char    buffer[512];
    va_list args;
    va_start(args, format);
    int const n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) out.append(buffer, static_cast<size_t>(std::min<int>(n, sizeof(buffer) - 1)));
}

template <size_t N>
void appHist(std::string& out, char const* name, uint64_t const (&hist)[N]) {
    app(out, " %s=", name);
    for (size_t i = 0; i < N; ++i) app(out, i ? "/%llu" : "%llu", static_cast<ull>(hist[i]));
}

// ---- state guarded by gMain ------------------------------------------------------------------------------------
std::mutex gMain;

Seg      gSeg;
Fr       gFr;
uint32_t gSegIndex    = 0;
int64_t  gCurTick     = -1;
int64_t  gHotTick     = -1;
int      gLastMode    = -1;
uint64_t gPeriodBegin = 0, gRenderEnd = 0;
int      gPeriodMode = 0;

std::unordered_map<uint64_t, Cell>                                 gCells;
std::vector<uint64_t>                                              gPending;
std::unordered_map<uint64_t, Change>                               gChanges;
std::unordered_map<::BlockPos, Sub, PosHash>                       gSubs;
std::unordered_map<void const*, std::shared_ptr<BuildRes>>         gBuilding;
std::unordered_map<void const*, std::shared_ptr<BuildRes>>         gCommittedGeom;
std::unordered_map<::BlockPos, std::shared_ptr<BuildRes>, PosHash> gCommittedPos;
std::unordered_map<::BlockPos, unsigned char, PosHash>             gLastVersion;
std::unordered_map<uint64_t, uint64_t>                             gBucketLast;

thread_local std::shared_ptr<BuildRes> tBuild;

// ---- S2 shadow state (gMain) -----------------------------------------------------------------------------------
struct ShadowRec {
    core::ObjectId id{};
    core::CellPos  src{}, dst{};
    uint64_t       srcKey{}, dstKey{};
    ::Block const* blockPtr{};
    int            mode{0};
    core::Phase    last{core::Phase::Gated};
    uint64_t       startQpc{}, animQpc{}, holdQpc{}, landQpc{};
    uint64_t       startFrame{};
    int64_t        startTick{-1}, animTick{-1};
    bool           visualEnded{false};
    uint32_t       depth{0};   // pushes this object superseded on its chain
    uint64_t       rootQpc{0}; // start of the oldest object on the chain
    int            spd{0};     // world speed class at start: 1x / slow / fast
};

// A client MovingBlockActor seen at $load, waiting for the change that vacated its source cell.
struct ShadowPending {
    core::CellPos  src{}, dst{};
    ::Block const* blockPtr{};
    uint64_t       frame{};
    int64_t        tick{-1};
    int            mode{0};
    bool           noPiston{false};
    bool           expanding{false}; // push (true) or pull
    core::Seq      loadSeq{};
    Cls            srcAtLoad{Cls::None}; // what the last change left in the source cell
    uint8_t        vacAtLoad{0};         // vacancy at load: none / already used / other block / stale
    uint8_t        srcSeen{0};           // bit per Cls the source cell took since the load
    bool           srcRealOutOther{false};
};

// After a Culled verdict: later qualifying builds must also leave the cell empty.
struct CulledWatch {
    core::CellPos cell{};
    core::Seq     need{};
    int            left{2};
    int            mode{0};
    ::Block const* block{};
    core::Seq      judgedBegin{}; // sequence number the judging build started at
    core::Seq      nb[6]{};       // last change of each neighbour when judged; 0 = none recorded
    uint64_t       clears{0};     // gLast clears so far; a clear loses neighbour history
};

// The change that took a Real block out of a cell; one change belongs to one push.
struct Vac {
    core::Seq      seq{};
    ::Block const* block{};
    bool           used{false};
    int64_t        tick{-1};
};

struct LastChange {
    core::Seq      seq{};
    Cls            cls{Cls::None};
    ::Block const* block{};
};

core::Overlay                                    gOv;
core::EvidenceLedger                             gLedger;
core::ObjectId                                   gShNext = 0;
int64_t                                          gShTick = -1;
std::unordered_map<core::ObjectId, ShadowRec>    gSh;
std::unordered_map<uint64_t, core::ObjectId>     gShByDst;
std::unordered_multimap<uint64_t, ShadowPending> gShPending; // by source cell key
std::unordered_map<uint64_t, Vac>                gVac;
std::unordered_map<uint64_t, LastChange>         gLast;
std::vector<CulledWatch>                         gWatch;

// What shadow objects did with a cell, to explain why the oracle finds cells no live object covers.
enum : uint8_t { HAsDst = 1, HAsSrc = 2, HNeverDst = 4, HNeverSrc = 8, HNoPistonDst = 16 };
struct ShHist {
    uint8_t flags{0};
    uint8_t dstEnd{0}, srcEnd{0}; // EndReason of the latest object that ended with the cell as destination / source
};
std::unordered_map<uint64_t, ShHist>  gShHist;
std::unordered_map<uint64_t, uint32_t> gShDstLive, gShSrcLive; // live objects per destination / source cell

// The start being processed: Overlay::start finishes the objects it supersedes, and shSync sees them afterwards.
struct ShCtx {
    core::ObjectId id{};
    core::CellPos  src{}, dst{};
    uint64_t       srcKey{}, dstKey{};
    ::Block const* block{};
};
ShCtx    gShCtx;
uint64_t gLastClears = 0; // times gLast was emptied; a watch that spans one has lost neighbour history
float    gLastSpeed  = -1.0f;
int64_t  gPrevTick   = -1;
uint64_t gPrevQpc    = 0;
template <class... Args>
void logSeg(int category, int limit, char const* format, Args... args) {
    if (gSeg.used[category]++ >= limit) return;
    rawLog(format, args...);
}

uint64_t packKey(int x, int y, int z) {
    return ((static_cast<uint64_t>(static_cast<uint32_t>(x)) & 0x3FFFFFFull) << 38)
         | ((static_cast<uint64_t>(static_cast<uint32_t>(z)) & 0x3FFFFFFull) << 12)
         | (static_cast<uint64_t>(static_cast<uint32_t>(y + 2048)) & 0xFFFull);
}

::BlockPos subOrigin(::BlockPos const& pos) { return ::BlockPos{pos.x & ~15, pos.y & ~15, pos.z & ~15}; }

// ---- bloom filter of changed cells: build threads test it without a lock --------------------------------------
constexpr size_t      kBloomWords = 2048;
std::atomic<uint64_t> gBloom[2][kBloomWords]{};
std::atomic<int>      gBloomActive{0};

uint64_t mix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

void bloomSet(int which, uint64_t key) {
    auto const h  = mix(key);
    auto const b0 = h & 0x1FFFF, b1 = (h >> 20) & 0x1FFFF;
    gBloom[which][b0 >> 6].fetch_or(1ull << (b0 & 63), std::memory_order_relaxed);
    gBloom[which][b1 >> 6].fetch_or(1ull << (b1 & 63), std::memory_order_relaxed);
}

void bloomAdd(uint64_t key) {
    bloomSet(0, key);
    bloomSet(1, key);
}

bool bloomTest(uint64_t key) {
    int const  which = gBloomActive.load(std::memory_order_relaxed);
    auto const h     = mix(key);
    auto const b0 = h & 0x1FFFF, b1 = (h >> 20) & 0x1FFFF;
    return (gBloom[which][b0 >> 6].load(std::memory_order_relaxed) >> (b0 & 63) & 1ull)
        && (gBloom[which][b1 >> 6].load(std::memory_order_relaxed) >> (b1 & 63) & 1ull);
}

void bloomRebuild() { // gMain held, render thread
    int const next = 1 - gBloomActive.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kBloomWords; ++i) gBloom[next][i].store(0, std::memory_order_relaxed);
    for (auto const& entry : gChanges) bloomSet(next, entry.first);
    for (auto const& entry : gCells) bloomSet(next, entry.first);
    for (auto const& entry : gSh) {
        bloomSet(next, entry.second.srcKey);
        bloomSet(next, entry.second.dstKey);
        auto const gate = gOv.gateCell(entry.first);
        bloomSet(next, packKey(gate.x, gate.y, gate.z));
    }
    for (auto const& watch : gWatch) bloomSet(next, packKey(watch.cell.x, watch.cell.y, watch.cell.z));
    gBloomActive.store(next, std::memory_order_relaxed);
}

// ---- per-period signature --------------------------------------------------------------------------------------
void tok(char token) { // gMain held; render-thread events only
    auto& sig = gFr.sig;
    if (sig.size() >= 56) return;
    if (!sig.empty() && sig.back() == token) return;
    sig.push_back(token);
}

// ---- cells -----------------------------------------------------------------------------------------------------
void ev(Cell& cell, char const* what, uint64_t frame, int64_t tick) {
    if (cell.log.size() > 320) {
        if (!cell.logCut) {
            cell.log    += " ...";
            cell.logCut  = true;
        }
        return;
    }
    char           buffer[64];
    uint64_t const df = frame >= cell.birthFrame ? frame - cell.birthFrame : 0;
    if (tick >= 0 && cell.birthTick >= 0)
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%s%s@%llu/%lld",
            cell.log.empty() ? "" : " ",
            what,
            static_cast<ull>(df),
            static_cast<long long>(tick - cell.birthTick)
        );
    else std::snprintf(buffer, sizeof(buffer), "%s%s@%llu", cell.log.empty() ? "" : " ", what, static_cast<ull>(df));
    cell.log += buffer;
}

Cell& createCell(::BlockPos const& pos, uint64_t key, Cls initial, uint64_t frame, int64_t tick) {
    if (gCells.size() >= 20000) {
        gC.cellsOverflow.fetch_add(1, std::memory_order_relaxed);
        gCells.clear();
        gPending.clear();
    }
    auto [it, inserted] = gCells.try_emplace(key);
    Cell& cell          = it->second;
    if (inserted) {
        cell.pos          = pos;
        cell.key          = key;
        cell.bornSeq      = gTrackSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        cell.chgSeq       = gSeq.load(std::memory_order_relaxed);
        cell.birthFrame   = frame;
        cell.birthTick    = tick;
        cell.birthMode    = modeIndex();
        cell.cls          = initial;
        cell.meshHas      = initial == Cls::Real;
        cell.lastActivity = frame;
        gC.cellsCreated.fetch_add(1, std::memory_order_relaxed);
        bloomAdd(key);
    }
    return cell;
}

void markDirty(::BlockPos const& pos, bool tracked, int64_t tick) {
    auto const now   = qpcNow();
    int        dx[3] = {0, 0, 0}, dy[3] = {0, 0, 0}, dz[3] = {0, 0, 0};
    int        nx = 1, ny = 1, nz = 1;
    if ((pos.x & 15) == 0) dx[nx++] = -16;
    else if ((pos.x & 15) == 15) dx[nx++] = 16;
    if ((pos.y & 15) == 0) dy[ny++] = -16;
    else if ((pos.y & 15) == 15) dy[ny++] = 16;
    if ((pos.z & 15) == 0) dz[nz++] = -16;
    else if ((pos.z & 15) == 15) dz[nz++] = 16;
    auto const base = subOrigin(pos);
    for (int ix = 0; ix < nx; ++ix)
        for (int iy = 0; iy < ny; ++iy)
            for (int iz = 0; iz < nz; ++iz) {
                auto& sub = gSubs[::BlockPos{base.x + dx[ix], base.y + dy[iy], base.z + dz[iz]}];
                if (!sub.dirtyQpc) {
                    sub.dirtyQpc  = now;
                    sub.dirtyTick = tick;
                }
                sub.tracked = sub.tracked || tracked;
            }
}

void transition(Cell& cell, Cls next, ::Block const& block, Cause cause, uint64_t frame, int64_t tick) {
    cell.chgSeq       = gSeq.load(std::memory_order_relaxed);
    cell.lastActivity = frame;
    if (next == cell.cls && &block == cell.block) return;
    int const mode = modeIndex();
    if (cell.cls == Cls::Air && next != Cls::Air && cell.airFromMoving) {
        int const ends             = next == Cls::Real ? 0 : 1;
        gSeg.gapEp[mode][ends]    += 1;
        gSeg.gapFr[mode][ends]    += cell.epAir;
        gSeg.gapNoVis[mode][ends] += cell.epNoVis;
    }
    if (next == Cls::Air && cell.cls != Cls::Air) {
        cell.airFromMoving = cell.cls == Cls::Moving;
        cell.epAir         = 0;
        cell.epNoVis       = 0;
    }
    if (cell.cls == Cls::Real && next != Cls::Real) {
        cell.relPending  = true;
        cell.relOpen     = true;
        cell.relBlock    = cell.block;
        cell.landPending = false;
        cell.relSeq      = cell.chgSeq;
        cell.relQpc      = qpcNow();
        cell.relTick     = tick;
    } else if (next == Cls::Real && (cell.cls == Cls::Air || cell.cls == Cls::Moving)) {
        if (cell.relOpen) gSeg.gateR[mode][2]++;
        cell.relOpen     = false;
        cell.relPending  = false;
        cell.landPending = true;
        cell.landSeq     = cell.chgSeq;
        cell.landQpc     = qpcNow();
        cell.landTick    = tick;
    }
    if (next == Cls::Real) cell.realName = block.getTypeName();
    if (next == Cls::Moving) cell.sawMoving = true;
    cell.cls     = next;
    cell.block   = &block;
    char what[8] = {clsLetter(next), ':', kCauseLetter[cause], 0};
    ev(cell, what, frame, tick);
}

// ---- S2 shadow overlay -----------------------------------------------------------------------------------------
// The rewrite's core runs on the real event stream and acts on nothing; each of its answers is checked against what the
// world and the meshes did. Everything here runs with gMain held, except shDerive and clientRegion.
void const* gRegionId = nullptr;

constexpr char const* kEndName[5] = {"none", "mesh", "culled", "superseded", "abandoned"};

core::CellPos cellOf(::BlockPos const& pos) { return core::CellPos{pos.x, pos.y, pos.z}; }

uint64_t keyOf(core::CellPos const& cell) { return packKey(cell.x, cell.y, cell.z); }

core::CellPos neighbour(core::CellPos const& cell, int face) {
    static constexpr int kStep[6][3] = {
        {1,  0,  0 },
        {-1, 0,  0 },
        {0,  1,  0 },
        {0,  -1, 0 },
        {0,  0,  1 },
        {0,  0,  -1}
    };
    return core::CellPos{cell.x + kStep[face][0], cell.y + kStep[face][1], cell.z + kStep[face][2]};
}

std::string blockName(::Block const* block) { return block ? block->getTypeName() : std::string("-"); }

// The block source of the local player's dimension; null while no world is open.
::BlockSource* clientRegion() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    return player ? &player->getDimensionBlockSource() : nullptr;
}

// Level ticks are the only clock the event matching uses: server updates for one push arrive within a tick or two.
bool shFresh(int64_t tick) { return tick < 0 || gCurTick < 0 || std::llabs(gCurTick - tick) <= 1; }

bool shCovers(uint64_t dstKey) {
    auto const found = gShByDst.find(dstKey);
    return found != gShByDst.end() && gOv.active(found->second);
}

// Does the world still hold the block the shadow expects in this cell? Unknown counts as yes.
bool shWorldSame(uint64_t key, ::Block const* block) {
    auto const last = gLast.find(key);
    return last == gLast.end() || (last->second.cls == Cls::Real && last->second.block == block);
}

ShHist& shHist(uint64_t key) {
    if (gShHist.size() >= 65536 && !gShHist.contains(key)) {
        gShHist.clear();
        ++gSeg.shHistOverflow;
    }
    return gShHist[key];
}

// The object has ended: it no longer covers its cells, and its end is what the oracle will later ask about.
void shEnded(ShadowRec const& rec, core::EndReason reason) {
    for (auto* live : {&gShDstLive, &gShSrcLive}) {
        auto const found = live->find(live == &gShDstLive ? rec.dstKey : rec.srcKey);
        if (found != live->end() && --found->second == 0) live->erase(found);
    }
    shHist(rec.dstKey).dstEnd = static_cast<uint8_t>(reason);
    shHist(rec.srcKey).srcEnd = static_cast<uint8_t>(reason);
}

// A pending push never found the change that vacated its source. why: sweep / replaced / capacity / reset.
void shNeverFate(ShadowPending const& p, int why) {
    auto&      s = gSeg;
    int const  m = p.mode;
    ++s.shNever[m];
    ++s.shNeverWhy[m][why];
    ++s.shNeverSrc[m][static_cast<int>(p.srcAtLoad)];
    ++s.shNeverVac[m][p.vacAtLoad];
    ++s.shNeverDir[m][p.expanding ? 1 : 0];
    int const realBit = 1 << static_cast<int>(Cls::Real);
    int const seen    = !p.srcSeen ? 0 : p.srcRealOutOther ? 2 : (p.srcSeen & realBit) == 0 ? 1 : 3;
    ++s.shNeverSeen[m][seen];
    auto const dstKey = keyOf(p.dst);
    auto const srcKey = keyOf(p.src);
    auto const dst    = gLast.find(dstKey);
    int const  fate   = dst == gLast.end() ? 2
                      : (dst->second.cls == Cls::Real && dst->second.block == p.blockPtr && dst->second.seq > p.loadSeq)
                          ? 0
                          : 1;
    ++s.shNeverDst[m][fate];
    if (shCovers(srcKey)) ++s.shNeverLiveDst[m];
    shHist(dstKey).flags |= HNeverDst;
    shHist(srcKey).flags |= HNeverSrc;
    auto name = blockName(p.blockPtr);
    if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
    if (s.neverName.size() >= 24 && !s.neverName.contains(name)) name = "other";
    ++s.neverName[name];
    logSeg(
        CatShNever,
        40,
        "shadow never src=(%d,%d,%d) dst=(%d,%d,%d) %s why=%d srcAtLoad=%c vac=%d seen=%d dir=%s dst=%d\n",
        p.src.x,
        p.src.y,
        p.src.z,
        p.dst.x,
        p.dst.y,
        p.dst.z,
        name.c_str(),
        why,
        clsLetter(p.srcAtLoad),
        static_cast<int>(p.vacAtLoad),
        seen,
        p.expanding ? "push" : "pull",
        fate
    );
}

// Which of the two objects the new start took the cell from, and whether it was the same block.
void shSupersede(ShadowRec const& old) {
    auto&     s = gSeg;
    int const m = old.mode;
    if (!gShCtx.id) {
        ++s.shSupNoCtx[m];
        return;
    }
    int const rel = old.dstKey == gShCtx.srcKey ? 0 : old.dstKey == gShCtx.dstKey ? 1 : 2;
    int       blk = 0;
    std::string was, now;
    if (old.blockPtr != gShCtx.block) {
        was = blockName(old.blockPtr);
        now = blockName(gShCtx.block);
        blk = was == now ? 1 : 2;
    }
    ++s.shSup[m][rel][blk];
    if (auto const next = gSh.find(gShCtx.id); next != gSh.end()) {
        next->second.depth   = std::max(next->second.depth, old.depth + 1);
        next->second.rootQpc = std::min(next->second.rootQpc, old.rootQpc);
    }
    if ((rel != 0 || blk == 2) && s.supSeen.size() < 256) {
        auto const sig = std::hash<std::string>{}(was + '>' + now) * 8 + static_cast<size_t>(rel * 3 + blk);
        if (s.supSeen.insert(sig).second)
            logSeg(
                CatShSup,
                40,
                "shadow superseded rel=%s blocks=%s old=%s new=%s old.dst=(%d,%d,%d) new.src=(%d,%d,%d) "
                "new.dst=(%d,%d,%d)\n",
                rel == 0 ? "chain" : rel == 1 ? "takeover" : "NEITHER",
                blk == 0 ? "same" : blk == 1 ? "state" : "other",
                was.c_str(),
                now.c_str(),
                old.dst.x,
                old.dst.y,
                old.dst.z,
                gShCtx.src.x,
                gShCtx.src.y,
                gShCtx.src.z,
                gShCtx.dst.x,
                gShCtx.dst.y,
                gShCtx.dst.z
            );
    }
}

// Why no live object covers a cell the oracle found empty (absent) or double-drawn by the mesh (ghost).
int shUncovAbsent(uint64_t key) {
    if (auto const live = gShDstLive.find(key); live != gShDstLive.end()) return 0;
    auto const hist = gShHist.find(key);
    if (hist == gShHist.end()) return 7;
    if (hist->second.dstEnd) return std::min<int>(hist->second.dstEnd, 4);
    if (hist->second.flags & HNeverDst) return 5;
    if (hist->second.flags & HNoPistonDst) return 6;
    return 7;
}

int shUncovGhost(uint64_t key) {
    if (auto const live = gShSrcLive.find(key); live != gShSrcLive.end()) return 0;
    auto const hist = gShHist.find(key);
    if (hist == gShHist.end()) return 7;
    if (hist->second.srcEnd) return std::min<int>(hist->second.srcEnd, 4);
    if (hist->second.dstEnd) return 5;
    if (hist->second.flags & HNeverSrc) return 6;
    return 7;
}

void shVerdictMesh(ShadowRec const& rec, BuildRes const* res) {
    auto& s = gSeg;
    if (!res) {
        ++s.shMesh[rec.mode][3];
        return;
    }
    auto const found = res->idx.find(rec.dstKey);
    if (found == res->idx.end()) {
        ++s.shMesh[rec.mode][3];
        return;
    }
    if (found->second.block == rec.blockPtr) {
        ++s.shMesh[rec.mode][0];
        return;
    }
    bool const same = shWorldSame(rec.dstKey, rec.blockPtr);
    ++s.shMesh[rec.mode][same ? 1 : 2];
    if (same)
        logSeg(
            CatShadowOdd,
            60,
            "shadow MESH-CONTRADICTED id=%llu dst=(%d,%d,%d) mode=%s want=%s mesh=%s\n",
            static_cast<ull>(rec.id),
            rec.dst.x,
            rec.dst.y,
            rec.dst.z,
            modeName(rec.mode),
            blockName(rec.blockPtr).c_str(),
            blockName(found->second.block).c_str()
        );
}

// Records what the core did since the last call; `res` is the build that was just promoted, if any.
void shSync(BuildRes const* res) {
    auto& s = gSeg;
    for (auto it = gSh.begin(); it != gSh.end();) {
        auto const id    = it->first;
        auto&      rec   = it->second;
        auto const phase = gOv.phase(id);
        if (phase == rec.last) {
            ++it;
            continue;
        }
        int const  mode   = rec.mode;
        auto const reason = gOv.endReason(id);
        bool const given  = phase == core::Phase::Done
                         && (reason == core::EndReason::Superseded || reason == core::EndReason::Abandoned);
        auto const now = qpcNow();
        if (!given) {
            if (rec.last == core::Phase::Gated) {
                s.shGate[mode][msBucket(qpcMs(now - rec.startQpc), kLatEdges)]++;
                s.shSpdGate[rec.spd][msBucket(qpcMs(now - rec.startQpc), kLatEdges)]++;
                s.shGateTicks[mode][tickBucket(gCurTick >= 0 && rec.startTick >= 0 ? gCurTick - rec.startTick : -1)]++;
                rec.animQpc = now;
            }
            if (rec.last != core::Phase::Holding && (phase == core::Phase::Holding || phase == core::Phase::Done)) {
                s.shAnim[mode][msBucket(qpcMs(now - rec.animQpc), kLatEdges)]++;
                rec.holdQpc = now;
            }
        }
        if (phase != core::Phase::Done) {
            rec.last = phase;
            ++it;
            continue;
        }
        ++s.shEnd[mode][static_cast<int>(reason)];
        ++s.shSpdEnd[rec.spd][static_cast<int>(reason)];
        shEnded(rec, reason);
        if (reason == core::EndReason::Superseded) shSupersede(rec);
        else {
            // The end of a chain decides what the player finally sees.
            int const tail = reason == core::EndReason::Mesh ? 0 : reason == core::EndReason::Culled ? 1 : 2;
            ++s.shChainLen[mode][tail][burstBucket(rec.depth)];
            if (rec.depth) s.shChainMs[mode][msBucket(qpcMs(now - rec.rootQpc), kLatEdges)]++;
        }
        if (given) {
            ++s.shStarved[mode][static_cast<int>(rec.last)];
        } else {
            if (rec.holdQpc) s.shHold[mode][msBucket(qpcMs(now - rec.holdQpc), kLatEdges)]++;
            s.shTotal[mode][msBucket(qpcMs(now - rec.startQpc), kLatEdges)]++;
            if (rec.landQpc > rec.startQpc) s.shNat[mode][msBucket(qpcMs(rec.landQpc - rec.startQpc), kLatEdges)]++;
            if (reason == core::EndReason::Mesh) shVerdictMesh(rec, res);
            else if (reason == core::EndReason::Culled && gWatch.size() >= 4096) ++s.shWatchEnd[mode][3];
            else if (reason == core::EndReason::Culled) {
                CulledWatch watch;
                watch.cell        = rec.dst;
                watch.need        = gOv.needSeq(id);
                watch.mode        = mode;
                watch.block       = rec.blockPtr;
                watch.judgedBegin = res ? res->seqAtBegin : 0;
                watch.clears      = gLastClears;
                for (int n = 0; n < 6; ++n) {
                    auto const nbKey = keyOf(neighbour(rec.dst, n));
                    auto const last  = gLast.find(nbKey);
                    watch.nb[n]      = last == gLast.end() ? 0 : last->second.seq;
                }
                gWatch.push_back(watch);
                bloomAdd(rec.dstKey);
            }
        }
        logSeg(
            CatShadow,
            60,
            "shadow end id=%llu dst=(%d,%d,%d) %s reason=%s gateMs=%.1f total=%.1f natural=%.1f\n",
            static_cast<ull>(id),
            rec.dst.x,
            rec.dst.y,
            rec.dst.z,
            blockName(rec.blockPtr).c_str(),
            kEndName[static_cast<int>(reason)],
            rec.animQpc > rec.startQpc ? qpcMs(rec.animQpc - rec.startQpc) : -1.0,
            qpcMs(now - rec.startQpc),
            rec.landQpc > rec.startQpc ? qpcMs(rec.landQpc - rec.startQpc) : -1.0
        );
        if (auto const byDst = gShByDst.find(rec.dstKey); byDst != gShByDst.end() && byDst->second == id)
            gShByDst.erase(byDst);
        it = gSh.erase(it);
    }
    gShCtx = ShCtx{};
    gSeg.shAsks[modeIndex()] += gOv.takeRebuilds().size();
}

// Did a neighbour change between the Culled verdict and the build that now produced the cell? A change the later build
// began after explains it (face culling depends on neighbours); one it could not have seen does not.
// 0 no neighbour changed / 1 changed before that build began / 2 changed only after it began / 3 history was lost
int shNeighbourVerdict(CulledWatch const& watch, core::Seq beginSeq) {
    bool before = false, after = false;
    for (int n = 0; n < 6; ++n) {
        auto const last = gLast.find(keyOf(neighbour(watch.cell, n)));
        auto const cur  = last == gLast.end() ? core::Seq{0} : last->second.seq;
        if (cur == watch.nb[n]) continue;
        (cur <= beginSeq ? before : after) = true;
    }
    if (before) return 1;
    if (after) return 2;
    return watch.clears != gLastClears ? 3 : 0;
}

// After a Culled verdict, later builds that qualify must also leave the cell empty.
void shWatch(core::BuildKey build, core::Seq beginSeq) {
    if (gWatch.empty()) return;
    auto const sub = gLedger.subChunk(build);
    for (auto it = gWatch.begin(); it != gWatch.end();) {
        if (core::subChunkOf(it->cell) != sub) {
            ++it;
            continue;
        }
        auto const seen = gLedger.judge(build, it->cell, it->need);
        if (seen == core::Seen::Stale || seen == core::Seen::Suppressed) {
            ++it;
            continue;
        }
        if (!shWorldSame(keyOf(it->cell), it->block)) {
            ++gSeg.shWatchEnd[it->mode][2];
            it = gWatch.erase(it);
            continue;
        }
        ++gSeg.shCulled[it->mode][seen == core::Seen::Present ? 1 : 0];
        if (seen == core::Seen::Present) {
            int const nb = shNeighbourVerdict(*it, beginSeq);
            ++gSeg.shCulledNb[it->mode][nb];
            logSeg(
                CatShadowOdd,
                60,
                "shadow CULLED-THEN-PRODUCED (%d,%d,%d) mode=%s block=%s neighbours=%s\n",
                it->cell.x,
                it->cell.y,
                it->cell.z,
                modeName(it->mode),
                blockName(it->block).c_str(),
                nb == 0   ? "NONE-CHANGED"
                : nb == 1 ? "changed-before-build"
                : nb == 2 ? "changed-after-build"
                          : "history-lost"
            );
            ++gSeg.shWatchEnd[it->mode][1];
            it = gWatch.erase(it);
        } else if (--it->left <= 0) {
            ++gSeg.shWatchEnd[it->mode][0];
            it = gWatch.erase(it);
        } else ++it;
    }
}

void shResetAll() {
    auto const seq = gSeq.load(std::memory_order_relaxed);
    if (!gSh.empty()) ++gSeg.shReset;
    gShCtx = ShCtx{};
    for (auto const& entry : gSh) gOv.abandon(entry.first, seq);
    shSync(nullptr);
    for (auto const& entry : gShPending) shNeverFate(entry.second, 3);
    gOv.clear();
    gSh.clear();
    gShByDst.clear();
    gShPending.clear();
    gVac.clear();
    gLast.clear();
    ++gLastClears;
    gWatch.clear();
    gLedger.clear();
    gShHist.clear();
    gShDstLive.clear();
    gShSrcLive.clear();
    gSeg.uncovSeen.clear();
}

// Pending pushes whose source cell never vacated, e.g. the piston arm itself.
void shSweep() {
    for (auto it = gShPending.begin(); it != gShPending.end();) {
        if (gCurTick >= 0 && it->second.tick >= 0 && gCurTick - it->second.tick > 2) {
            shNeverFate(it->second, 0);
            it = gShPending.erase(it);
        } else ++it;
    }
}

void shBegin(ShadowPending const& pending, uint64_t srcKey, core::Seq release, bool deferred) {
    if (gSh.size() >= 20000) shResetAll();
    auto&      s      = gSeg;
    int const  mode   = modeIndex();
    auto const dstKey = keyOf(pending.dst);
    // The destination already holds the block, set after the source was vacated: the push has landed.
    core::Seq landSeq = 0;
    if (auto const last = gLast.find(dstKey); last != gLast.end() && last->second.cls == Cls::Real
                                              && last->second.block == pending.blockPtr && last->second.seq >= release)
        landSeq = last->second.seq;
    auto const id = ++gShNext;
    ShadowRec  rec;
    rec.id         = id;
    rec.src        = pending.src;
    rec.dst        = pending.dst;
    rec.srcKey     = srcKey;
    rec.dstKey     = dstKey;
    rec.blockPtr   = pending.blockPtr;
    rec.mode       = mode;
    rec.startQpc   = qpcNow();
    rec.startFrame = gFrame.load(std::memory_order_relaxed);
    rec.startTick  = gCurTick;
    rec.rootQpc    = rec.startQpc;
    rec.spd        = speedClass();
    if (landSeq) rec.landQpc = rec.startQpc;
    auto const now = gSeq.load(std::memory_order_acquire);
    gShCtx         = ShCtx{id, pending.src, pending.dst, srcKey, dstKey, pending.blockPtr};
    gOv.start(id, pending.src, pending.dst, release, landSeq, now);
    if (landSeq) {
        rec.visualEnded = true;
        gOv.visualEnded(id, now);
    }
    gSh[id]          = rec;
    gShByDst[dstKey] = id;
    ++gShDstLive[dstKey];
    ++gShSrcLive[srcKey];
    shHist(dstKey).flags |= HAsDst;
    shHist(srcKey).flags |= HAsSrc;
    bloomAdd(srcKey);
    bloomAdd(dstKey);
    ++s.shStart[mode];
    if (deferred) ++s.shDeferred[mode];
    logSeg(
        CatShadow,
        40,
        "shadow start id=%llu %s src=(%d,%d,%d) dst=(%d,%d,%d) release=%llu landed=%d loadFirst=%d\n",
        static_cast<ull>(id),
        blockName(pending.blockPtr).c_str(),
        pending.src.x,
        pending.src.y,
        pending.src.z,
        pending.dst.x,
        pending.dst.y,
        pending.dst.z,
        static_cast<ull>(release),
        landSeq ? 1 : 0,
        deferred ? 1 : 0
    );
}

// Source and destination of the push that made this client MovingBlockActor. Render thread, without gMain.
bool shDerive(::MovingBlockActor const& actor, ::BlockPos const& dst, ShadowPending& out) {
    auto* const region = clientRegion();
    if (!region) return false;
    auto* const entity = region->getBlockEntity(actor.mPistonBlockPos.get());
    if (!entity || entity->mType != ::BlockActorType::PistonArm) return false;
    auto const& facing  = static_cast<::PistonBlockActor*>(entity)->getFacingDir(*region);
    ::Block const* wrapped = actor.mWrappedBlock;
    if (!wrapped || (!facing.x && !facing.y && !facing.z)) return false;
    // A pushed block travels along the facing, a pulled one against it.
    int const sign = actor.mPistonBlockExpanding ? 1 : -1;
    out.src        = core::CellPos{dst.x - facing.x * sign, dst.y - facing.y * sign, dst.z - facing.z * sign};
    out.dst        = cellOf(dst);
    out.blockPtr   = wrapped;
    out.frame      = gFrame.load(std::memory_order_relaxed);
    out.mode       = modeIndex();
    out.expanding  = actor.mPistonBlockExpanding;
    return true;
}

// A client MovingBlockActor was loaded. The change that vacated its source cell may have come before or may come after.
void shOnLoad(ShadowPending pending, bool derived, uint64_t actorKey) {
    auto&     s    = gSeg;
    int const mode = modeIndex();
    if (!derived) {
        ++s.shNoPiston[mode];
        shHist(actorKey).flags |= HNoPistonDst;
        return;
    }
    ++s.shLoads[mode];
    pending.tick      = gCurTick;
    pending.loadSeq   = gSeq.load(std::memory_order_relaxed);
    auto const srcKey = keyOf(pending.src);
    if (auto const vac = gVac.find(srcKey);
        vac != gVac.end() && !vac->second.used && vac->second.block == pending.blockPtr && shFresh(vac->second.tick)) {
        vac->second.used = true;
        shBegin(pending, srcKey, vac->second.seq, false);
        shSync(nullptr);
        return;
    }
    if (auto const last = gLast.find(srcKey); last != gLast.end()) pending.srcAtLoad = last->second.cls;
    if (auto const vac = gVac.find(srcKey); vac != gVac.end())
        pending.vacAtLoad = vac->second.used ? 1 : vac->second.block != pending.blockPtr ? 2 : 3;
    // A second actor for the same block and source replaces the first.
    auto range = gShPending.equal_range(srcKey);
    for (auto it = range.first; it != range.second;) {
        if (it->second.blockPtr == pending.blockPtr) {
            if (it->second.dst == pending.dst) ++s.shNeverDup[mode];
            shNeverFate(it->second, 1);
            it = gShPending.erase(it);
        } else ++it;
    }
    if (gShPending.size() >= 8192) {
        for (auto const& entry : gShPending) shNeverFate(entry.second, 2);
        gShPending.clear();
    }
    gShPending.emplace(srcKey, pending);
}

void shOnChange(
    uint64_t          key,
    ::BlockPos const& pos,
    Cls               next,
    Cls               prev,
    ::Block const&    block,
    ::Block const&    oldBlock,
    core::Seq         seq
) {
    if (gLast.size() >= 65536) {
        gLast.clear();
        ++gLastClears;
    }
    gLast[key] = LastChange{seq, next, &block};
    gOv.worldChanged(
        cellOf(pos),
        next == Cls::Real     ? core::WorldCell::Real
        : next == Cls::Moving ? core::WorldCell::Moving
                              : core::WorldCell::Air,
        seq
    );
    bool changed = false;
    // The block landing in its destination is the end of the vanilla visual.
    if (next == Cls::Real) {
        if (auto const byDst = gShByDst.find(key); byDst != gShByDst.end()) {
            auto const rec = gSh.find(byDst->second);
            if (rec != gSh.end() && rec->second.blockPtr == &block) {
                if (!rec->second.landQpc) rec->second.landQpc = qpcNow();
                if (!rec->second.visualEnded) {
                    rec->second.visualEnded = true;
                    gOv.visualEnded(byDst->second, seq);
                    changed = true;
                }
            }
        }
    }
    {
        // What the source cell of a waiting push did since the load, for the ones that never get matched.
        auto waiting = gShPending.equal_range(key);
        for (auto it = waiting.first; it != waiting.second; ++it) {
            it->second.srcSeen |= static_cast<uint8_t>(1u << static_cast<unsigned>(next));
            if (prev == Cls::Real && next != Cls::Real && it->second.blockPtr != &oldBlock)
                it->second.srcRealOutOther = true;
        }
    }
    if (prev == Cls::Real && next != Cls::Real) {
        if (gVac.size() >= 16384) gVac.clear();
        gVac[key] = Vac{seq, &oldBlock, false, gCurTick};
        auto range = gShPending.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.blockPtr != &oldBlock) continue;
            ShadowPending const pending = it->second;
            gShPending.erase(it);
            gVac[key].used = true;
            shBegin(pending, key, seq, true);
            changed = true;
            break;
        }
    }
    if (changed) shSync(nullptr);
}

// The destination actor stopped animating.
void shOnPreserved(uint64_t key, ::Block const* block) {
    auto const byDst = gShByDst.find(key);
    if (byDst == gShByDst.end()) return;
    auto const rec = gSh.find(byDst->second);
    if (rec == gSh.end() || rec->second.visualEnded || rec->second.blockPtr != block) return;
    rec->second.visualEnded = true;
    gOv.visualEnded(byDst->second, gSeq.load(std::memory_order_acquire));
    shSync(nullptr);
}

void onBlockChange(
    ::BlockPos const& pos,
    ::Block const&    block,
    ::Block const&    oldBlock,
    uint64_t          enter,
    uint64_t          done,
    int               updateFlags
) {
    (void)updateFlags;
    bool const            rt    = onRenderThread();
    Cls const             next  = classify(block);
    Cls const             prev  = classify(oldBlock);
    Cause const           cause = tCause;
    auto const            tick  = levelTick();
    auto const            key   = packKey(pos.x, pos.y, pos.z);
    std::lock_guard const guard(gMain);
    auto const            frame = gFrame.load(std::memory_order_relaxed);
    bloomAdd(key); // a build that reads the new sequence number must already tessellate this cell into its record
    auto const            seq   = gSeq.fetch_add(1, std::memory_order_release) + 1;
    gCurTick                    = tick;
    gC.bcTotal.fetch_add(1, std::memory_order_relaxed);
    gFr.bcN++;
    gFr.listenerN++;
    gFr.listenerMs += qpcMs(done - enter);
    if (rt) tok('b');
    if (gChanges.size() > 8192) gChanges.clear();
    gChanges[key] = Change{seq, &block, &oldBlock, enter, done};
    bloomAdd(key);
    gSeg.cause[cause][static_cast<int>(prev)][static_cast<int>(next)]++;

    bool const movingInvolved = next == Cls::Moving || prev == Cls::Moving;
    if (movingInvolved) gC.bcMoving.fetch_add(1, std::memory_order_relaxed);
    if (prev == Cls::Moving && next != Cls::Moving) {
        (next == Cls::Air ? gC.mbToAir : gC.mbToBlock).fetch_add(1, std::memory_order_relaxed);
    }

    auto it = gCells.find(key);
    if (movingInvolved) {
        if (it == gCells.end()) {
            auto& cell = createCell(pos, key, prev, frame, tick);
            cell.block = &oldBlock;
            if (prev == Cls::Real) cell.realName = oldBlock.getTypeName();
            cell.dest      = true;
            cell.confirmed = true;
            gC.cellsConfirmed.fetch_add(1, std::memory_order_relaxed);
            it = gCells.find(key);
        } else if (!it->second.confirmed) {
            it->second.confirmed = true;
            it->second.dest      = true;
            gC.cellsConfirmed.fetch_add(1, std::memory_order_relaxed);
        } else it->second.dest = true;
        if (tick >= 0) {
            gHotTick = tick;
            for (auto pendingKey : gPending) {
                auto const found = gCells.find(pendingKey);
                if (found == gCells.end() || found->second.confirmed) continue;
                if (found->second.birthTick >= 0 && tick - found->second.birthTick <= 1) {
                    found->second.confirmed = true;
                    found->second.source    = true;
                    gC.cellsConfirmed.fetch_add(1, std::memory_order_relaxed);
                    markDirty(found->second.pos, true, tick);
                }
            }
            gPending.clear();
        }
    } else if (it == gCells.end() && prev == Cls::Real && next == Cls::Air) {
        // Possibly the vacated source of a push: track it provisionally, confirmed by a moving block within a tick.
        auto& cell    = createCell(pos, key, prev, frame, tick);
        cell.block    = &oldBlock;
        cell.realName = oldBlock.getTypeName();
        cell.source   = true;
        if (tick >= 0 && gHotTick >= 0 && std::llabs(tick - gHotTick) <= 1) {
            cell.confirmed = true;
            gC.cellsConfirmed.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (gPending.size() > 20000) gPending.clear();
            gPending.push_back(key);
        }
        it = gCells.find(key);
    }

    bool tracked = movingInvolved;
    if (it != gCells.end()) {
        transition(it->second, next, block, cause, frame, tick);
        tracked = tracked || it->second.confirmed;
    }
    shOnChange(key, pos, next, prev, block, oldBlock, seq);
    markDirty(pos, tracked, tick);
}

// ---- oracle ----------------------------------------------------------------------------------------------------
uint64_t bucketKey(int x, int y, int z) { return packKey(x >> 3, y >> 3, z >> 3); }

bool inView(Cell const& cell, uint64_t frame) {
    int const            bx = cell.pos.x >> 3, by = cell.pos.y >> 3, bz = cell.pos.z >> 3;
    static constexpr int offsets[7][3] = {
        {0,  0,  0 },
        {1,  0,  0 },
        {-1, 0,  0 },
        {0,  1,  0 },
        {0,  -1, 0 },
        {0,  0,  1 },
        {0,  0,  -1}
    };
    for (auto const& o : offsets) {
        auto const it = gBucketLast.find(packKey(bx + o[0], by + o[1], bz + o[2]));
        if (it != gBucketLast.end() && frame - it->second <= 20) return true;
    }
    return false;
}

void runStep(uint32_t& run, uint32_t& maxRun, bool flag, uint64_t (&hist)[5]) {
    if (flag) {
        ++run;
        maxRun = std::max(maxRun, run);
    } else if (run) {
        hist[runBucket(run)]++;
        run = 0;
    }
}

// One line per uncovered cell and kind, so the log shows where they are without growing with the frame count.
void shUncovSample(Cell const& cell, char const* kind, int why) {
    if (!gSeg.uncovSeen.insert(cell.key * 2 + (kind[0] == 'g' ? 1 : 0)).second) return;
    logSeg(
        CatShUncov,
        40,
        "shadow uncovered %s why=%d (%d,%d,%d) cls=%c %s\n",
        kind,
        why,
        cell.pos.x,
        cell.pos.y,
        cell.pos.z,
        clsLetter(cell.cls),
        cell.realName.c_str()
    );
}

void evalCell(Cell& cell, uint64_t frame, int mode) {
    if (!cell.confirmed) return;
    bool const drawn      = cell.lastDrawFrame == frame;
    bool const mesh       = cell.meshHas;
    int        absentKind = -1;
    bool       dbl = false, ghost = false, relevant = false;
    switch (cell.cls) {
    case Cls::Moving:
        relevant = true;
        if (!drawn) {
            if (mesh) ghost = true;
            else absentKind = 0;
        } else if (mesh) dbl = true;
        break;
    case Cls::Air:
        relevant = cell.airFromMoving;
        if (cell.airFromMoving && !drawn && !mesh) absentKind = 1;
        if (drawn && mesh) dbl = true;
        else if (!drawn && mesh) ghost = true;
        if (dbl || ghost) relevant = true;
        break;
    case Cls::Real:
        relevant = cell.sawMoving || mesh;
        if (!cell.noGeom && cell.sawMoving && !drawn && !mesh) absentKind = 2;
        if (drawn && mesh) dbl = true;
        break;
    default:
        break;
    }
    bool const interesting =
        relevant && (absentKind >= 0 || dbl || ghost || (cell.cls == Cls::Air && cell.airFromMoving));
    bool const counted = interesting && inView(cell, frame);
    auto&      s       = gSeg;
    if (counted) {
        s.evalFrames[mode]++;
        if (cell.cls == Cls::Air && cell.airFromMoving) {
            cell.epAir++;
            if (absentKind == 1) cell.epNoVis++;
        }
        if (cell.cls == Cls::Moving && drawn && cell.lastVisDrawFrame != frame) s.absentVisMb[mode]++;
        if (absentKind >= 0) {
            s.absent[mode][absentKind]++;
            bool const covered = shCovers(cell.key);
            s.shAbsent[mode][absentKind][covered ? 0 : 1]++;
            if (!covered) {
                int const why = shUncovAbsent(cell.key);
                s.shUncovAbs[mode][absentKind][why]++;
                shUncovSample(cell, "absent", why);
            }
            cell.absent[absentKind]++;
        }
        if (dbl) {
            s.dbl[mode][kindOf(cell.cls)]++;
            cell.dbl[kindOf(cell.cls)]++;
        }
        if (ghost) {
            s.ghost[mode]++;
            bool const covered = gOv.meshSuppressed(core::CellPos{cell.pos.x, cell.pos.y, cell.pos.z});
            s.shGhost[mode][covered ? 0 : 1]++;
            if (!covered) {
                int const why = shUncovGhost(cell.key);
                s.shUncovGhost[mode][why]++;
                shUncovSample(cell, "ghost", why);
            }
            cell.ghost++;
        }
    }
    runStep(cell.runAbsent, cell.maxAbsent, counted && absentKind >= 0, s.absentRun[mode]);
    runStep(cell.runDbl, cell.maxDbl, counted && dbl, s.dblRun[mode]);
    runStep(cell.runGhost, cell.maxGhost, counted && ghost, s.ghostRun[mode]);
}

void closeCell(Cell& cell, uint64_t frame) {
    int const mode = cell.birthMode;
    auto&     s    = gSeg;
    if (cell.runAbsent) s.absentRun[mode][runBucket(cell.runAbsent)]++;
    if (cell.runDbl) s.dblRun[mode][runBucket(cell.runDbl)]++;
    if (cell.runGhost) s.ghostRun[mode][runBucket(cell.runGhost)]++;
    if (cell.lastDrawFrame) ev(cell, "E", cell.lastDrawFrame, -1);
    bool const endAbsent = cell.cls == Cls::Real && cell.sawMoving && !cell.noGeom && !cell.meshHas;
    bool const endGhost  = cell.cls == Cls::Air && cell.meshHas;
    bool const anomaly = cell.absent[0] || cell.absent[1] || cell.absent[2] || cell.dbl[0] || cell.dbl[1] || cell.dbl[2]
                      || cell.ghost || endAbsent || endGhost;
    s.cellsClosed[mode]++;
    if (anomaly) s.cellsBad[mode]++;
    if (endAbsent) s.cellsEndAbsent[mode]++;
    if (endGhost) s.cellsEndGhost[mode]++;
    if (cell.noGeom) s.cellsNoGeom[mode]++;
    static uint32_t okCounter = 0;
    if (!anomaly && (++okCounter % 8) != 0) return;
    char      role[4] = {cell.dest ? 'D' : '-', cell.source ? 'S' : '-', 0};
    int const catg    = anomaly ? CatCellBad : CatCellOk;
    logSeg(
        catg,
        anomaly ? 150 : 30,
        "cell (%d,%d,%d) %s blk=%s role=%s mode=%s>%s end=%c mesh=%d noGeom=%d | abs M/A/R=%u/%u/%u dbl=%u/%u/%u "
        "ghost=%u "
        "maxRun a/d/g=%u/%u/%u | air=%u/%u | draw=%u/%u ticks=%u vis=%d dd=%d life=%llu | %s\n",
        cell.pos.x,
        cell.pos.y,
        cell.pos.z,
        anomaly ? "BAD" : "ok",
        cell.realName.empty() ? "-" : cell.realName.c_str(),
        role,
        modeName(cell.birthMode),
        modeName(modeIndex()),
        clsLetter(cell.cls),
        cell.meshHas ? 1 : 0,
        cell.noGeom ? 1 : 0,
        cell.absent[0],
        cell.absent[1],
        cell.absent[2],
        cell.dbl[0],
        cell.dbl[1],
        cell.dbl[2],
        cell.ghost,
        cell.maxAbsent,
        cell.maxDbl,
        cell.maxGhost,
        cell.epAir,
        cell.epNoVis,
        cell.drawFrames,
        cell.drawCalls,
        cell.ticks,
        cell.visState,
        cell.dd ? 1 : 0,
        static_cast<ull>(frame - cell.birthFrame),
        cell.log.c_str()
    );
}

// ---- segment output --------------------------------------------------------------------------------------------
void segPrint(char const* tag) { // gMain held
    auto&        s    = gSeg;
    double const secs = s.q0 ? qpcMs(qpcNow() - s.q0) / 1000.0 : 0.0;
    std::string  t;
    app(t,
        "%s#%u label=%s frames(on/off)=%llu/%llu secs=%.1f f0=%llu lt0=%lld cells=%zu speedChanges=%llu",
        tag,
        s.index,
        s.label.c_str(),
        static_cast<ull>(s.frames[1]),
        static_cast<ull>(s.frames[0]),
        secs,
        static_cast<ull>(s.f0),
        static_cast<long long>(s.lt0),
        gCells.size(),
        static_cast<ull>(s.speedChanges));
    rawLog("%s\n", t.c_str());
    if (s.spdFrames[0] || s.spdFrames[1] || s.spdFrames[2]) {
        t.clear();
        app(t, "%s#%u speed frames 1x/slow/fast=%llu/%llu/%llu", tag, s.index, static_cast<ull>(s.spdFrames[0]),
            static_cast<ull>(s.spdFrames[1]), static_cast<ull>(s.spdFrames[2]));
        for (int k = 0; k < 3; ++k)
            app(t, " tps[%d]=%.2f", k, s.spdMs[k] > 0.0 ? static_cast<double>(s.spdTicks[k]) * 1000.0 / s.spdMs[k] : 0.0);
        rawLog("%s\n", t.c_str());
    }
    for (int m = 0; m < 2; ++m) {
        for (int a = 0; a < 2; ++a) {
            uint64_t n = 0;
            for (auto v : s.dt[m][a]) n += v;
            if (!n) continue;
            t.clear();
            app(t,
                "%s#%u dt mode=%s active=%d n=%llu avg=%.2f max=%.1f",
                tag,
                s.index,
                modeName(m),
                a,
                static_cast<ull>(n),
                s.dtSum[m][a] / static_cast<double>(n),
                s.dtMax[m][a]);
            appHist(t, "ms<8/<12/<16.7/<25/<33.4/<50/<100/>=100", s.dt[m][a]);
            appHist(t, "renderLevel", s.rl[m][a]);
            app(t, " rlMax=%.1f", s.rlMax[m][a]);
            rawLog("%s\n", t.c_str());
        }
        uint64_t n = 0;
        for (auto v : s.burst[m]) n += v;
        if (n) {
            t.clear();
            app(t, "%s#%u promotesPerPeriod mode=%s", tag, s.index, modeName(m));
            appHist(t, "0/1/2-3/4-7/8-15/16+", s.burst[m]);
            rawLog("%s\n", t.c_str());
        }
        for (int k = 0; k < 2; ++k) {
            uint64_t cnt = 0;
            for (auto v : s.lat[m][k]) cnt += v;
            if (!cnt) continue;
            t.clear();
            app(t,
                "%s#%u lat dirty->promote mode=%s tracked=%d n=%llu",
                tag,
                s.index,
                modeName(m),
                k,
                static_cast<ull>(cnt));
            appHist(t, "ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.lat[m][k]);
            appHist(t, "ticks0/1-2/3-5/6-10/11-20/21-40/41+", s.latTicks[m][k]);
            rawLog("%s\n", t.c_str());
        }
        {
            uint64_t cnt = 0;
            for (auto v : s.c2p[m]) cnt += v;
            if (cnt || s.latUnknown[m]) {
                t.clear();
                app(t,
                    "%s#%u stage(tracked) mode=%s unknownDirty=%llu",
                    tag,
                    s.index,
                    modeName(m),
                    static_cast<ull>(s.latUnknown[m]));
                appHist(t, "dirty->begin", s.d2b[m]);
                appHist(t, "begin->commit", s.b2c[m]);
                appHist(t, "commit->promote", s.c2p[m]);
                rawLog("%s\n", t.c_str());
            }
        }
        if (s.evalFrames[m] || s.cellsClosed[m]) {
            t.clear();
            app(t,
                "%s#%u oracle mode=%s inViewCellFrames=%llu absent M/A/R=%llu/%llu/%llu dbl M/A/R=%llu/%llu/%llu "
                "ghost=%llu "
                "absentMbVisState=%llu | cells closed=%llu bad=%llu endAbsent=%llu endGhost=%llu noGeom=%llu",
                tag,
                s.index,
                modeName(m),
                static_cast<ull>(s.evalFrames[m]),
                static_cast<ull>(s.absent[m][0]),
                static_cast<ull>(s.absent[m][1]),
                static_cast<ull>(s.absent[m][2]),
                static_cast<ull>(s.dbl[m][0]),
                static_cast<ull>(s.dbl[m][1]),
                static_cast<ull>(s.dbl[m][2]),
                static_cast<ull>(s.ghost[m]),
                static_cast<ull>(s.absentVisMb[m]),
                static_cast<ull>(s.cellsClosed[m]),
                static_cast<ull>(s.cellsBad[m]),
                static_cast<ull>(s.cellsEndAbsent[m]),
                static_cast<ull>(s.cellsEndGhost[m]),
                static_cast<ull>(s.cellsNoGeom[m]));
            appHist(t, "absentRun(1/2/3-5/6-20/21+)", s.absentRun[m]);
            appHist(t, "dblRun", s.dblRun[m]);
            appHist(t, "ghostRun", s.ghostRun[m]);
            rawLog("%s\n", t.c_str());
        }
        if (s.gapEp[m][0] || s.gapEp[m][1]) {
            rawLog(
                "%s#%u gap(mb->air episodes) mode=%s endsReal eps=%llu frames=%llu noVisFrames=%llu | endsMoving "
                "eps=%llu frames=%llu noVisFrames=%llu\n",
                tag,
                s.index,
                modeName(m),
                static_cast<ull>(s.gapEp[m][0]),
                static_cast<ull>(s.gapFr[m][0]),
                static_cast<ull>(s.gapNoVis[m][0]),
                static_cast<ull>(s.gapEp[m][1]),
                static_cast<ull>(s.gapFr[m][1]),
                static_cast<ull>(s.gapNoVis[m][1])
            );
        }
        {
            uint64_t draws = s.drawUntracked[m];
            for (int st = 0; st < 3; ++st) draws += s.vis[m][st][0] + s.vis[m][st][1];
            if (draws) {
                rawLog(
                    "%s#%u interlock@draw mode=%s state0(InitialNotVisible) dd0/dd1=%llu/%llu "
                    "state1(Visible)=%llu/%llu state2(DelayedDestruction)=%llu/%llu untrackedDraws=%llu\n",
                    tag,
                    s.index,
                    modeName(m),
                    static_cast<ull>(s.vis[m][0][0]),
                    static_cast<ull>(s.vis[m][0][1]),
                    static_cast<ull>(s.vis[m][1][0]),
                    static_cast<ull>(s.vis[m][1][1]),
                    static_cast<ull>(s.vis[m][2][0]),
                    static_cast<ull>(s.vis[m][2][1]),
                    static_cast<ull>(s.drawUntracked[m])
                );
            }
        }
    }
    for (int m = 0; m < 2; ++m) {
        if (s.preOk[m] || s.preStale[m] || s.preOther[m] || s.seenReal[m] || s.missReal[m]) {
            t.clear();
            app(t,
                "%s#%u premiseC mode=%s tessellate(change before begin): sawNew=%llu sawOld=%llu sawOther=%llu | "
                "applyBuild(real cells, no later change): seen=%llu missing=%llu",
                tag,
                s.index,
                modeName(m),
                static_cast<ull>(s.preOk[m]),
                static_cast<ull>(s.preStale[m]),
                static_cast<ull>(s.preOther[m]),
                static_cast<ull>(s.seenReal[m]),
                static_cast<ull>(s.missReal[m]));
            appHist(t, "oldAge ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.preStaleAge[m]);
            appHist(t, "sawNew begin-enter ms<0.05/<0.2/<1/<3/<10/<30/<100/>=100", s.preMargin[m]);
            rawLog("%s\n", t.c_str());
        }
        if (s.gateR[m][0] || s.gateR[m][1] || s.gateR[m][2]) {
            t.clear();
            app(t,
                "%s#%u gateR(ignores later changes) mode=%s resolved=%llu stillOld=%llu landedFirst=%llu",
                tag,
                s.index,
                modeName(m),
                static_cast<ull>(s.gateR[m][0]),
                static_cast<ull>(s.gateR[m][1]),
                static_cast<ull>(s.gateR[m][2]));
            appHist(t, "ticks0/1-2/3-5/6-10/11-20/21-40/41+", s.gateRTicks[m]);
            appHist(t, "ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.gateRMs[m]);
            rawLog("%s\n", t.c_str());
        }
        if (s.gate[m][0] || s.gate[m][1]) {
            t.clear();
            app(t,
                "%s#%u gate(src left -> first build begun after it) mode=%s clean=%llu contradicted=%llu",
                tag,
                s.index,
                modeName(m),
                static_cast<ull>(s.gate[m][0]),
                static_cast<ull>(s.gate[m][1]));
            appHist(t, "ticks0/1-2/3-5/6-10/11-20/21-40/41+", s.gateTicks[m]);
            appHist(t, "ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.gateMs[m]);
            rawLog("%s\n", t.c_str());
        }
        uint64_t landN = 0;
        for (auto v : s.land[m]) landN += v;
        if (landN) {
            t.clear();
            app(t,
                "%s#%u land(real block back -> first build begun after it) mode=%s present=%llu culled=%llu "
                "suppressed=%llu otherBlock=%llu missing=%llu",
                tag,
                s.index,
                modeName(m),
                static_cast<ull>(s.land[m][0]),
                static_cast<ull>(s.land[m][1]),
                static_cast<ull>(s.land[m][2]),
                static_cast<ull>(s.land[m][3]),
                static_cast<ull>(s.land[m][4]));
            appHist(t, "ticks0/1-2/3-5/6-10/11-20/21-40/41+", s.landTicks[m]);
            appHist(t, "ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.landMs[m]);
            rawLog("%s\n", t.c_str());
        }
    }
    for (int m = 0; m < 2; ++m) {
        uint64_t ended = 0;
        for (auto v : s.shEnd[m]) ended += v;
        if (!s.shStart[m] && !ended && !s.shNoPiston[m] && !s.shNever[m]) continue;
        t.clear();
        app(t,
            "%s#%u shadow mode=%s start=%llu loadFirst=%llu noPiston=%llu neverMatched=%llu asks=%llu",
            tag,
            s.index,
            modeName(m),
            static_cast<ull>(s.shStart[m]),
            static_cast<ull>(s.shDeferred[m]),
            static_cast<ull>(s.shNoPiston[m]),
            static_cast<ull>(s.shNever[m]),
            static_cast<ull>(s.shAsks[m]));
        appHist(t, "end none/mesh/culled/superseded/abandoned", s.shEnd[m]);
        appHist(t, "starved gated/animating/holding", s.shStarved[m]);
        appHist(t, "meshVerdict agree/OTHERBLOCK/movedOn/missing", s.shMesh[m]);
        appHist(t, "culledLater confirmedAbsent/PRODUCED", s.shCulled[m]);
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t,
            "%s#%u shadowOracle mode=%s absent M/A/R covered|uncovered=%llu|%llu / %llu|%llu / %llu|%llu ghost "
            "covered|uncovered=%llu|%llu",
            tag,
            s.index,
            modeName(m),
            static_cast<ull>(s.shAbsent[m][0][0]),
            static_cast<ull>(s.shAbsent[m][0][1]),
            static_cast<ull>(s.shAbsent[m][1][0]),
            static_cast<ull>(s.shAbsent[m][1][1]),
            static_cast<ull>(s.shAbsent[m][2][0]),
            static_cast<ull>(s.shAbsent[m][2][1]),
            static_cast<ull>(s.shGhost[m][0]),
            static_cast<ull>(s.shGhost[m][1]));
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t, "%s#%u shadowLat mode=%s", tag, s.index, modeName(m));
        appHist(t, "gate ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.shGate[m]);
        appHist(t, "gateTicks0/1-2/3-5/6-10/11-20/21-40/41+", s.shGateTicks[m]);
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t, "%s#%u shadowLat mode=%s", tag, s.index, modeName(m));
        appHist(t, "anim ms", s.shAnim[m]);
        appHist(t, "hold ms", s.shHold[m]);
        appHist(t, "total ms", s.shTotal[m]);
        appHist(t, "natural(start->landing) ms", s.shNat[m]);
        rawLog("%s\n", t.c_str());
        // Superseded: chain = the old block's destination is the new push's source, takeover = same destination.
        t.clear();
        app(t, "%s#%u shadowSup mode=%s noCtx=%llu", tag, s.index, modeName(m), static_cast<ull>(s.shSupNoCtx[m]));
        static constexpr char const* kRel[3] = {"chain", "takeover", "NEITHER"};
        for (int r = 0; r < 3; ++r)
            app(t, " %s same/state/other=%llu/%llu/%llu", kRel[r], static_cast<ull>(s.shSup[m][r][0]),
                static_cast<ull>(s.shSup[m][r][1]), static_cast<ull>(s.shSup[m][r][2]));
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t, "%s#%u shadowChain mode=%s", tag, s.index, modeName(m));
        appHist(t, "tail=mesh depth0/1/2-3/4-7/8-15/16+", s.shChainLen[m][0]);
        appHist(t, "tail=culled", s.shChainLen[m][1]);
        appHist(t, "tail=abandoned", s.shChainLen[m][2]);
        appHist(t, "chainMs<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.shChainMs[m]);
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t,
            "%s#%u shadowNever mode=%s loads=%llu dupLoad=%llu",
            tag,
            s.index,
            modeName(m),
            static_cast<ull>(s.shLoads[m]),
            static_cast<ull>(s.shNeverDup[m]));
        appHist(t, "why sweep/replaced/cap/reset", s.shNeverWhy[m]);
        appHist(t, "srcAtLoad none/air/moving/real", s.shNeverSrc[m]);
        appHist(t, "vac none/used/otherBlock/stale", s.shNeverVac[m]);
        appHist(t, "seen none/onlyAirMoving/realOutOtherBlock/other", s.shNeverSeen[m]);
        appHist(t, "dir pull/push", s.shNeverDir[m]);
        appHist(t, "dst landedSame/other/unknown", s.shNeverDst[m]);
        app(t, " srcIsLiveDst=%llu", static_cast<ull>(s.shNeverLiveDst[m]));
        if (!s.neverName.empty()) {
            std::vector<std::pair<std::string, uint64_t>> rows(s.neverName.begin(), s.neverName.end());
            std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            app(t, " top=");
            for (size_t i = 0; i < rows.size() && i < 8; ++i)
                app(t, i ? ",%s:%llu" : "%s:%llu", rows[i].first.c_str(), static_cast<ull>(rows[i].second));
        }
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t, "%s#%u shadowUncov mode=%s", tag, s.index, modeName(m));
        appHist(t, "absentM olderActive/endMesh/endCulled/endSup/endAband/never/noPiston/none", s.shUncovAbs[m][0]);
        appHist(t, "absentA", s.shUncovAbs[m][1]);
        appHist(t, "absentR", s.shUncovAbs[m][2]);
        appHist(t, "ghost srcPastGate/srcEndMesh/srcEndCulled/srcEndSup/srcEndAband/dstEndedOnly/neverSrc/none",
                s.shUncovGhost[m]);
        rawLog("%s\n", t.c_str());
        t.clear();
        app(t, "%s#%u shadowCulledNb mode=%s", tag, s.index, modeName(m));
        appHist(t, "produced neighbours none/before/after/historyLost", s.shCulledNb[m]);
        appHist(t, "watchEnd left/produced/worldMoved/refused", s.shWatchEnd[m]);
        rawLog("%s\n", t.c_str());
    }
    {
        static constexpr char const* kSpd[3] = {"1x", "slow", "fast"};
        for (int k = 0; k < 3; ++k) {
            uint64_t n = 0;
            for (auto v : s.shSpdEnd[k]) n += v;
            if (!n) continue;
            t.clear();
            app(t, "%s#%u shadowBySpeed %s", tag, s.index, kSpd[k]);
            appHist(t, "end none/mesh/culled/superseded/abandoned", s.shSpdEnd[k]);
            appHist(t, "gate ms<20/<50/<100/<200/<500/<1000/<2000/>=2000", s.shSpdGate[k]);
            rawLog("%s\n", t.c_str());
        }
    }
    if (!gSh.empty() || !gShPending.empty() || s.shReset) {
        size_t phases[3]{};
        for (auto const& entry : gSh) {
            auto const phase = static_cast<size_t>(gOv.phase(entry.first));
            if (phase < 3) ++phases[phase];
        }
        rawLog(
            "%s#%u shadowLive objects=%zu gated/animating/holding=%zu/%zu/%zu pending=%zu vac=%zu watch=%zu "
            "ledger=%zu reset=%llu histOverflow=%llu hist=%zu\n",
            tag,
            s.index,
            gSh.size(),
            phases[0],
            phases[1],
            phases[2],
            gShPending.size(),
            gVac.size(),
            gWatch.size(),
            gLedger.size(),
            static_cast<ull>(s.shReset),
            static_cast<ull>(s.shHistOverflow),
            gShHist.size());
    }
    t.clear();
    app(t, "%s#%u cause(old>new)", tag, s.index);
    char const letters[4] = {'-', 'A', 'M', 'R'};
    for (int c = 0; c < CauseCount; ++c)
        for (int o = 0; o < 4; ++o)
            for (int n = 0; n < 4; ++n)
                if (s.cause[c][o][n])
                    app(t, " %s:%c>%c=%llu", kCauseName[c], letters[o], letters[n], static_cast<ull>(s.cause[c][o][n]));
    rawLog("%s\n", t.c_str());
    static constexpr char const* kinds[3] = {"live", "snapshot", "third"};
    for (int k = 0; k < 3; ++k) {
        uint64_t n = 0;
        for (auto v : s.p2off[k]) n += v;
        if (!n) continue;
        t.clear();
        app(t, "%s#%u p2 %s n=%llu", tag, s.index, kinds[k], static_cast<ull>(n));
        appHist(t, "enter-buildStart ms<-100/<-20/<-5/<0/<2/<5/<20/>=20", s.p2off[k]);
        if (k == 1) appHist(t, "age(now-listenerDone)", s.p2age);
        rawLog("%s\n", t.c_str());
    }
    if (!s.sig.empty()) {
        std::vector<std::pair<std::string, uint64_t>> rows(s.sig.begin(), s.sig.end());
        std::sort(rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        t.clear();
        app(t,
            "%s#%u sig(B=begin Q=queueEntities r/a=blockEntities o/a D=draw b=blockChange L/l/u=mba load/ctor/update "
            "P/p=promote changed/same C=commit E=end)",
            tag,
            s.index);
        for (size_t i = 0; i < rows.size() && i < 14; ++i)
            app(t, " [%llu]%s", static_cast<ull>(rows[i].second), rows[i].first.c_str());
        rawLog("%s\n", t.c_str());
    }
}

void summarize(bool force) {
    char text[1536];
    std::snprintf(
        text,
        sizeof(text),
        "sum P1 promote=%llu changed=%llu beforeRbe=%llu afterRbe=%llu renderOutside=%llu otherThread=%llu frames=%llu "
        "rbe=%llu/%llu | P2 live=%llu snap=%llu amb=%llu third=%llu | P3 begin=%llu commit=%llu reuseInFlight=%llu "
        "commitNoBegin=%llu verRepeat=%llu inFlightMax=%llu commitOffRender=%llu | bc total=%llu moving=%llu "
        "mbToBlock=%llu "
        "mbToAir=%llu | mba ctorRender=%llu ctorOther=%llu load=%llu update=%llu tickRender=%llu tickOther=%llu | P6 "
        "dpRender=%llu dpOther=%llu proxy=%llu a0=%llu a1=%llu aMid=%llu | join ptr=%llu pos=%llu miss=%llu | cells "
        "created=%llu confirmed=%llu expired=%llu overflow=%llu\n",
        (ull)gC.promoteTotal.load(),
        (ull)gC.promoteChanged.load(),
        (ull)gC.promoteBeforeRbe.load(),
        (ull)gC.promoteAfterRbe.load(),
        (ull)gC.promoteRenderOutside.load(),
        (ull)gC.promoteOtherThread.load(),
        (ull)gC.frames.load(),
        (ull)gC.rbeCalls[0].load(),
        (ull)gC.rbeCalls[1].load(),
        (ull)gC.p2Live.load(),
        (ull)gC.p2Snapshot.load(),
        (ull)gC.p2Ambiguous.load(),
        (ull)gC.p2Third.load(),
        (ull)gC.buildBegin.load(),
        (ull)gC.buildCommit.load(),
        (ull)gC.reuseInFlight.load(),
        (ull)gC.commitNoBegin.load(),
        (ull)gC.verRepeat.load(),
        (ull)gC.inFlightMax.load(),
        (ull)gC.commitOffRender.load(),
        (ull)gC.bcTotal.load(),
        (ull)gC.bcMoving.load(),
        (ull)gC.mbToBlock.load(),
        (ull)gC.mbToAir.load(),
        (ull)gC.mbaCtorRender.load(),
        (ull)gC.mbaCtorOther.load(),
        (ull)gC.mbaLoad.load(),
        (ull)gC.mbaUpdate.load(),
        (ull)gC.mbaTickRender.load(),
        (ull)gC.mbaTickOther.load(),
        (ull)gC.dpRender.load(),
        (ull)gC.dpOther.load(),
        (ull)gC.dpProxy.load(),
        (ull)gC.dpAlpha0.load(),
        (ull)gC.dpAlpha1.load(),
        (ull)gC.dpAlphaMid.load(),
        (ull)gC.joinPtr.load(),
        (ull)gC.joinPos.load(),
        (ull)gC.joinMiss.load(),
        (ull)gC.cellsCreated.load(),
        (ull)gC.cellsConfirmed.load(),
        (ull)gC.cellsExpired.load(),
        (ull)gC.cellsOverflow.load()
    );
    std::string body = text;
    auto const  from = body.find("frames=");
    auto const  to   = body.find("rbe=");
    if (from != std::string::npos && to != std::string::npos) body.erase(from, to - from);
    std::lock_guard const guard(gLogMutex);
    if (!gFile || (!force && body == gLastSummary)) return;
    gLastSummary = std::move(body);
    std::fprintf(gFile, "f=%llu %s", static_cast<ull>(gFrame.load(std::memory_order_relaxed)), text);
    std::fflush(gFile);
}

// ---- period bookkeeping ----------------------------------------------------------------------------------------
void finalizePeriod(uint64_t now) { // gMain held
    if (!gPeriodBegin) return;
    double const dt = qpcMs(now - gPeriodBegin);
    double const rl = gRenderEnd > gPeriodBegin ? qpcMs(gRenderEnd - gPeriodBegin) : 0.0;
    int const    m  = gPeriodMode;
    int const    a  = gFr.drawN > 0 ? 1 : 0;
    auto&        s  = gSeg;
    s.frames[m]++;
    s.dt[m][a][msBucket(dt, kFrameEdges)]++;
    s.dtSum[m][a] += dt;
    s.dtMax[m][a]  = std::max(s.dtMax[m][a], dt);
    s.rl[m][a][msBucket(rl, kFrameEdges)]++;
    s.rlMax[m][a] = std::max(s.rlMax[m][a], rl);
    s.burst[m][burstBucket(gFr.promoteChanged)]++;
    if (dt >= 50.0) {
        logSeg(
            CatHitch,
            120,
            "hitch mode=%s dt=%.1f renderLevel=%.1f outside=%.1f | promote=%u(changed %u)/%.2fms listener=%u/%.2fms "
            "load=%u/%.2fms "
            "commit=%u/%.2fms draws=%u bc=%u cells=%zu building=%zu sig=%s\n",
            modeName(m),
            dt,
            rl,
            dt - rl,
            gFr.promoteN,
            gFr.promoteChanged,
            gFr.promoteMs,
            gFr.listenerN,
            gFr.listenerMs,
            gFr.loadN,
            gFr.loadMs,
            gFr.commitN,
            gFr.commitMs,
            gFr.drawN,
            gFr.bcN,
            gCells.size(),
            gBuilding.size(),
            gFr.sig.c_str()
        );
    }
    if (!gFr.sig.empty()) {
        auto it = s.sig.find(gFr.sig);
        if (it != s.sig.end()) it->second++;
        else if (s.sig.size() < 400) s.sig.emplace(gFr.sig, 1);
    }
    gFr = Fr{};
}

// ---- promote ---------------------------------------------------------------------------------------------------
void applyBuild(BuildRes const& res, ::BlockPos const& sub, uint64_t frame) { // gMain held
    for (auto& entry : gCells) {
        Cell& cell = entry.second;
        if (!(subOrigin(cell.pos) == sub) || cell.bornSeq > res.trackSeq) continue;
        auto const found = res.idx.find(cell.key);
        bool       has   = false;
        bool       stale = cell.chgSeq > res.seqAtBegin;
        if (cell.relOpen && cell.cls != Cls::Real && res.seqAtBegin >= cell.relSeq) {
            int const  m = modeIndex();
            bool const stillOld =
                found != res.idx.end() && found->second.block == cell.relBlock && found->second.produced;
            if (stillOld) gSeg.gateR[m][1]++;
            else {
                gSeg.gateR[m][0]++;
                gSeg.gateRTicks[m][tickBucket(gCurTick >= 0 && cell.relTick >= 0 ? gCurTick - cell.relTick : -1)]++;
                gSeg.gateRMs[m][msBucket(qpcMs(qpcNow() > cell.relQpc ? qpcNow() - cell.relQpc : 0), kLatEdges)]++;
                cell.relOpen = false;
            }
        }
        if (!stale) {
            // Nothing changed this cell after the build read the sequence number: what the build saw is what the
            // evidence rule would have relied on.
            int const  m    = modeIndex();
            bool const seen = found != res.idx.end();
            if (cell.cls == Cls::Real) {
                if (seen) gSeg.seenReal[m]++;
                else {
                    gSeg.missReal[m]++;
                    logSeg(
                        CatPremise,
                        40,
                        "premiseC MISS (%d,%d,%d) mode=%s real=%s chgSeq=%llu beginSeq=%llu\n",
                        cell.pos.x,
                        cell.pos.y,
                        cell.pos.z,
                        modeName(m),
                        cell.realName.c_str(),
                        static_cast<ull>(cell.chgSeq),
                        static_cast<ull>(res.seqAtBegin)
                    );
                }
            }
            auto const elapsedTicks = [&](int64_t since) {
                return since >= 0 && gCurTick >= 0 ? gCurTick - since : -1;
            };
            auto const elapsedMs = [&](uint64_t since) { return qpcMs(qpcNow() > since ? qpcNow() - since : 0); };
            if (cell.relPending && cell.cls != Cls::Real && res.seqAtBegin >= cell.relSeq) {
                bool const contradicted = seen && found->second.block != cell.block;
                if (contradicted) {
                    gSeg.gate[m][1]++;
                    logSeg(
                        CatPremise,
                        40,
                        "premiseC GATE-CONTRADICTED (%d,%d,%d) mode=%s now=%c relSeq=%llu beginSeq=%llu\n",
                        cell.pos.x,
                        cell.pos.y,
                        cell.pos.z,
                        modeName(m),
                        clsLetter(cell.cls),
                        static_cast<ull>(cell.relSeq),
                        static_cast<ull>(res.seqAtBegin)
                    );
                } else {
                    gSeg.gate[m][0]++;
                    gSeg.gateTicks[m][tickBucket(elapsedTicks(cell.relTick))]++;
                    gSeg.gateMs[m][msBucket(elapsedMs(cell.relQpc), kLatEdges)]++;
                    cell.relPending = false;
                }
            }
            if (cell.landPending && cell.cls == Cls::Real && res.seqAtBegin >= cell.landSeq) {
                int outcome = 4;
                if (seen) {
                    auto const& item = found->second;
                    if (item.block != cell.block) outcome = 3;
                    else if (item.suppressed) outcome = 2;
                    else outcome = item.produced ? 0 : 1;
                }
                gSeg.land[m][outcome]++;
                if (outcome <= 1) {
                    gSeg.landTicks[m][tickBucket(elapsedTicks(cell.landTick))]++;
                    gSeg.landMs[m][msBucket(elapsedMs(cell.landQpc), kLatEdges)]++;
                    cell.landPending = false;
                } else if (outcome >= 3) {
                    logSeg(
                        CatPremise,
                        40,
                        "premiseC LAND-%s (%d,%d,%d) mode=%s real=%s landSeq=%llu beginSeq=%llu\n",
                        outcome == 3 ? "OTHERBLOCK" : "MISSING",
                        cell.pos.x,
                        cell.pos.y,
                        cell.pos.z,
                        modeName(m),
                        cell.realName.c_str(),
                        static_cast<ull>(cell.landSeq),
                        static_cast<ull>(res.seqAtBegin)
                    );
                }
            }
        }
        if (found != res.idx.end()) {
            auto const& item = found->second;
            has              = item.produced && !item.suppressed;
            if (item.block != cell.block) stale = true;
            if (!item.produced && !item.suppressed && item.block == cell.block && cell.cls == Cls::Real)
                cell.noGeom = true;
            else if (item.produced) cell.noGeom = false;
        }
        if (has != cell.meshHas) {
            ev(cell, has ? (stale ? "m*" : "m") : (stale ? "n*" : "n"), frame, gCurTick);
            cell.meshHas      = has;
            cell.lastActivity = frame;
        }
    }
}

void onPromote(void const* afterPtr, ::BlockPos const* afterPos, bool changed, double ms, char const* phase) {
    (void)phase;
    if (!onRenderThread()) return;
    std::lock_guard const guard(gMain);
    auto const            frame = gFrame.load(std::memory_order_relaxed);
    gFr.promoteN++;
    gFr.promoteMs += ms;
    tok(changed ? 'P' : 'p');
    if (!changed || !afterPtr || !afterPos) return;
    gFr.promoteChanged++;
    std::shared_ptr<BuildRes> res;
    if (auto it = gCommittedGeom.find(afterPtr); it != gCommittedGeom.end()) {
        res = it->second;
        gCommittedGeom.erase(it);
        gC.joinPtr.fetch_add(1, std::memory_order_relaxed);
    } else if (auto pit = gCommittedPos.find(*afterPos); pit != gCommittedPos.end()) {
        res = pit->second;
        gC.joinPos.fetch_add(1, std::memory_order_relaxed);
    } else gC.joinMiss.fetch_add(1, std::memory_order_relaxed);
    if (!res) return;
    int const  mode = modeIndex();
    auto const now  = qpcNow();
    if (res->dirtyQpc) {
        int const    tracked = res->trackedDirty ? 1 : 0;
        double const total   = qpcMs(now - res->dirtyQpc);
        gSeg.lat[mode][tracked][msBucket(total, kLatEdges)]++;
        if (res->dirtyTick >= 0 && gCurTick >= 0) gSeg.latTicks[mode][tracked][tickBucket(gCurTick - res->dirtyTick)]++;
        if (tracked) {
            gSeg.d2b[mode]
                    [msBucket(qpcMs(res->beginQpc > res->dirtyQpc ? res->beginQpc - res->dirtyQpc : 0), kLatEdges)]++;
            gSeg.b2c[mode]
                    [msBucket(qpcMs(res->commitQpc > res->beginQpc ? res->commitQpc - res->beginQpc : 0), kLatEdges)]++;
            gSeg.c2p[mode][msBucket(qpcMs(now > res->commitQpc ? now - res->commitQpc : 0), kLatEdges)]++;
            if (total >= 500.0) {
                gSeg.slowSub[mode]++;
                logSeg(
                    CatSlow,
                    80,
                    "slow sub (%d,%d,%d) mode=%s dirty->promote=%.0fms dirty->begin=%.0f begin->commit=%.0f "
                    "commit->promote=%.0f "
                    "ticks=%lld ver=%u frames=%llu\n",
                    afterPos->x,
                    afterPos->y,
                    afterPos->z,
                    modeName(mode),
                    total,
                    qpcMs(res->beginQpc > res->dirtyQpc ? res->beginQpc - res->dirtyQpc : 0),
                    qpcMs(res->commitQpc > res->beginQpc ? res->commitQpc - res->beginQpc : 0),
                    qpcMs(now > res->commitQpc ? now - res->commitQpc : 0),
                    static_cast<long long>(res->dirtyTick >= 0 && gCurTick >= 0 ? gCurTick - res->dirtyTick : -1),
                    static_cast<unsigned>(res->version),
                    static_cast<ull>(gSeg.frames[0] + gSeg.frames[1])
                );
            }
        }
    } else gSeg.latUnknown[mode]++;
    if (res->ledgerKey) {
        shWatch(res->ledgerKey, res->seqAtBegin); // earlier Culled verdicts first, so a verdict never confirms itself
        gOv.promoted(res->ledgerKey, gLedger, gSeq.load(std::memory_order_acquire));
        shSync(res.get());
        gLedger.drop(res->ledgerKey);
    }
    if (!res->idx.empty() || !gCells.empty()) applyBuild(*res, *afterPos, frame);
}

// ---- mba events ------------------------------------------------------------------------------------------------
void mbaEvent(::MovingBlockActor const& actor, int kind, uint64_t ticks) {
    bool const rt = onRenderThread();
    if (kind == 0) (rt ? gC.mbaCtorRender : gC.mbaCtorOther).fetch_add(1, std::memory_order_relaxed);
    else if (kind == 1) gC.mbaLoad.fetch_add(1, std::memory_order_relaxed);
    else gC.mbaUpdate.fetch_add(1, std::memory_order_relaxed);
    if (!rt) return;
    auto const            pos = actor.mPosition.get();
    auto const            key = packKey(pos.x, pos.y, pos.z);
    ShadowPending         pending{};
    bool const            derived = kind == 1 && shDerive(actor, pos, pending);
    std::lock_guard const guard(gMain);
    gFr.loadN++;
    gFr.loadMs                             += qpcMs(ticks);
    static constexpr char        kToken[3]  = {'l', 'L', 'u'};
    static constexpr char const* kWhat[3]   = {"l", "L", "u"};
    tok(kToken[kind]);
    if (kind == 1) shOnLoad(pending, derived, key);
    auto it = gCells.find(key);
    if (it == gCells.end()) return;
    auto const frame        = gFrame.load(std::memory_order_relaxed);
    it->second.lastActivity = frame;
    ev(it->second, kWhat[kind], frame, gCurTick);
}

void mbaTickEvent(::MovingBlockActor const& actor) {
    auto const            pos = actor.mPosition.get();
    auto const            key = packKey(pos.x, pos.y, pos.z);
    std::lock_guard const guard(gMain);
    if (actor.mPreserved) shOnPreserved(key, actor.mWrappedBlock);
    auto it = gCells.find(key);
    if (it == gCells.end()) return;
    auto&      cell   = it->second;
    auto const frame  = gFrame.load(std::memory_order_relaxed);
    cell.lastActivity = frame;
    if (cell.ticks++ == 0) ev(cell, "k", frame, gCurTick);
    bool const preserved = actor.mPreserved;
    if (preserved != cell.preserved) {
        char what[24];
        std::snprintf(what, sizeof(what), preserved ? "p%d" : "q%d", static_cast<int>(actor.mPreservedLifespan));
        ev(cell, what, frame, gCurTick);
        cell.preserved = preserved;
    }
}

// ---- hooks -----------------------------------------------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    ObservePromoteHook,
    ll::memory::HookPriority::Normal,
    RenderChunkInstanced,
    &RenderChunkInstanced::promoteSortedGeometry,
    void
) {
    auto const* before = mCurrentRenderChunkGeometry.get();
    auto const  t0     = qpcNow();
    origin();
    if (!gOn.load(std::memory_order_relaxed)) return;
    auto const  t1      = qpcNow();
    auto const* after   = mCurrentRenderChunkGeometry.get();
    bool const  changed = before != after;
    auto const  tid     = GetCurrentThreadId();
    gC.promoteTotal.fetch_add(1, std::memory_order_relaxed);
    if (changed) gC.promoteChanged.fetch_add(1, std::memory_order_relaxed);
    char const* phase = "other";
    if (tid != gRenderTid.load(std::memory_order_relaxed)) {
        gC.promoteOtherThread.fetch_add(1, std::memory_order_relaxed);
    } else if (!gInFrame.load(std::memory_order_relaxed)) {
        gC.promoteRenderOutside.fetch_add(1, std::memory_order_relaxed);
        phase = "renderOutsideFrame";
    } else if (!gRbeSeen.load(std::memory_order_relaxed)) {
        gC.promoteBeforeRbe.fetch_add(1, std::memory_order_relaxed);
        phase = "beforeRbe";
    } else {
        gC.promoteAfterRbe.fetch_add(1, std::memory_order_relaxed);
        phase = "afterRbe";
    }
    ::BlockPos pos{};
    if (changed && after) pos = after->mPosition.get();
    onPromote(static_cast<void const*>(after), changed && after ? &pos : nullptr, changed, qpcMs(t1 - t0), phase);
}

LL_TYPE_INSTANCE_HOOK(
    ObserveBlockChangedHook,
    ll::memory::HookPriority::Normal,
    RenderChunkCoordinator,
    &RenderChunkCoordinator::$onBlockChanged,
    void,
    ::BlockSource&                 source,
    ::BlockPos const&              pos,
    uint                           layer,
    ::Block const&                 block,
    ::Block const&                 oldBlock,
    int                            updateFlags,
    ::ActorBlockSyncMessage const* syncMsg,
    ::BlockChangedEventTarget      eventTarget,
    ::Actor*                       blockChangeSource
) {
    if (!gOn.load(std::memory_order_relaxed) || layer != 0) {
        origin(source, pos, layer, block, oldBlock, updateFlags, syncMsg, eventTarget, blockChangeSource);
        return;
    }
    auto const enter = qpcNow();
    origin(source, pos, layer, block, oldBlock, updateFlags, syncMsg, eventTarget, blockChangeSource);
    onBlockChange(pos, block, oldBlock, enter, qpcNow(), updateFlags);
}

LL_TYPE_INSTANCE_HOOK(
    ObserveMbaCtorHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$ctor,
    void*,
    ::BlockPos const& pos
) {
    auto const  t0     = qpcNow();
    auto* const result = origin(pos);
    if (gOn.load(std::memory_order_relaxed)) mbaEvent(*this, 0, qpcNow() - t0);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    ObserveMbaLoadHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$load,
    void,
    ::ILevel&            level,
    ::CompoundTag const& tag,
    ::DataLoadHelper&    helper
) {
    auto const t0 = qpcNow();
    origin(level, tag, helper);
    if (gOn.load(std::memory_order_relaxed)) mbaEvent(*this, 1, qpcNow() - t0);
}

LL_TYPE_INSTANCE_HOOK(
    ObserveMbaUpdateHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$_onUpdatePacket,
    void,
    ::CompoundTag const& data,
    ::BlockSource&       region
) {
    auto const t0 = qpcNow();
    origin(data, region);
    if (gOn.load(std::memory_order_relaxed)) mbaEvent(*this, 2, qpcNow() - t0);
}

LL_TYPE_INSTANCE_HOOK(
    ObserveMbaTickHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$tick,
    void,
    ::BlockSource& region
) {
    if (!gOn.load(std::memory_order_relaxed)) {
        origin(region);
        return;
    }
    if (!onRenderThread()) {
        gC.mbaTickOther.fetch_add(1, std::memory_order_relaxed);
        origin(region);
        return;
    }
    {
        CauseScope const scope(CauseMbaTick);
        origin(region);
    }
    gC.mbaTickRender.fetch_add(1, std::memory_order_relaxed);
    mbaTickEvent(*this);
}

LL_TYPE_INSTANCE_HOOK(
    ObservePistonTickHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$tick,
    void,
    ::BlockSource& region
) {
    CauseScope const scope(CausePistonTick);
    origin(region);
}

LL_TYPE_INSTANCE_HOOK(
    ObserveSubChunkHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&          source,
    ::UpdateSubChunkBlocksPacket const& packet
) {
    CauseScope const scope(CauseSubChunk);
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    ObserveUpdateBlockHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&             source,
    ::std::shared_ptr<::UpdateBlockPacket> packet
) {
    CauseScope const scope(CauseUpdateBlock);
    origin(source, std::move(packet));
}

LL_TYPE_INSTANCE_HOOK(
    ObserveUpdateBlockSyncedHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&                   source,
    ::std::shared_ptr<::UpdateBlockSyncedPacket> packet
) {
    CauseScope const scope(CauseSynced);
    origin(source, std::move(packet));
}

LL_TYPE_INSTANCE_HOOK(
    ObserveQueueEntitiesHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$queueRenderEntities,
    void,
    ::LevelRenderPreRenderUpdateParameters const& parameters
) {
    origin(parameters);
    if (!gOn.load(std::memory_order_relaxed) || !onRenderThread()) return;
    std::lock_guard const guard(gMain);
    tok('Q');
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

constexpr int kHooks = 11;
bool          gHooked[kHooks]{};

} // namespace

void install() {
    if (gOn.load()) return;
    if (auto* const mod = ll::mod::NativeMod::current().get()) {
        auto const path = mod->getModDir() / "observe.log";
        if (fopen_s(&gFile, path.string().c_str(), "wb") != 0) gFile = nullptr;
    }
    {
        std::lock_guard const guard(gMain);
        gSeg    = Seg{};
        gSeg.q0 = qpcNow();
    }
    gOn.store(true);
    bool const ok[kHooks] = {
        installHook<ObservePromoteHook>(gHooked[0]),
        installHook<ObserveBlockChangedHook>(gHooked[1]),
        installHook<ObserveMbaCtorHook>(gHooked[2]),
        installHook<ObserveMbaLoadHook>(gHooked[3]),
        installHook<ObserveMbaUpdateHook>(gHooked[4]),
        installHook<ObserveMbaTickHook>(gHooked[5]),
        installHook<ObservePistonTickHook>(gHooked[6]),
        installHook<ObserveSubChunkHook>(gHooked[7]),
        installHook<ObserveUpdateBlockHook>(gHooked[8]),
        installHook<ObserveUpdateBlockSyncedHook>(gHooked[9]),
        installHook<ObserveQueueEntitiesHook>(gHooked[10]),
    };
    std::string line = "install observe-v3 r63 shadow diag hooks:";
    for (int i = 0; i < kHooks; ++i) app(line, " %d", ok[i] ? 1 : 0);
    rawLog("%s\n", line.c_str());
    rawLog(
        "legend dt/renderLevel ms edges 8,12,16.7,25,33.4,50,100 | lat ms edges 20,50,100,200,500,1000,2000 | cell "
        "events: "
        "M/A/R:<cause S=subchunk U=updateBlock Y=synced T=mbaTick K=pistonTick -=other> block change, l=mba ctor "
        "L=load "
        "u=update k=first tick pN/qN=preserved on/off(lifespan) vN=interlock state d/e/E=draw run start/end/final "
        "m/n=mesh gained/lost (*=build older than the change) | @frames/ticks since cell birth\n"
    );
}

void remove() {
    if (!gOn.load()) return;
    {
        std::lock_guard const guard(gMain);
        finalizePeriod(qpcNow());
        segPrint("seg");
    }
    summarize(true);
    gOn.store(false);
    removeHook<ObserveQueueEntitiesHook>(gHooked[10]);
    removeHook<ObserveUpdateBlockSyncedHook>(gHooked[9]);
    removeHook<ObserveUpdateBlockHook>(gHooked[8]);
    removeHook<ObserveSubChunkHook>(gHooked[7]);
    removeHook<ObservePistonTickHook>(gHooked[6]);
    removeHook<ObserveMbaTickHook>(gHooked[5]);
    removeHook<ObserveMbaUpdateHook>(gHooked[4]);
    removeHook<ObserveMbaLoadHook>(gHooked[3]);
    removeHook<ObserveMbaCtorHook>(gHooked[2]);
    removeHook<ObserveBlockChangedHook>(gHooked[1]);
    removeHook<ObservePromoteHook>(gHooked[0]);
    std::lock_guard const guard(gLogMutex);
    if (gFile) std::fclose(gFile);
    gFile = nullptr;
}

void frameBegin() {
    if (!gOn.load(std::memory_order_relaxed)) return;
    auto const now  = qpcNow();
    auto const tick = levelTick();
    gRenderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    gRbeSeen.store(false, std::memory_order_relaxed);
    gInFrame.store(true, std::memory_order_relaxed);
    gC.frames.fetch_add(1, std::memory_order_relaxed);
    void const* const region = clientRegion();
    std::lock_guard const guard(gMain);
    if (region != gRegionId) {
        shResetAll();
        gRegionId = region;
    }
    finalizePeriod(now);
    auto const frame = gFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    gCurTick         = tick;
    gPeriodBegin     = now;
    gPeriodMode      = modeIndex();
    if (gPeriodMode != gLastMode) {
        logSeg(CatMode, 50, "mode piston=%s lt=%lld\n", modeName(gPeriodMode), static_cast<long long>(tick));
        gLastMode = gPeriodMode;
    }
    {
        auto const& speed   = core::worldSpeedSettings();
        float const applied = speed.applied.load(std::memory_order_relaxed);
        if (applied != gLastSpeed) {
            auto const* blocker = worldSpeedBlocker();
            logSeg(
                CatSpeed,
                50,
                "speed applied=%.3f requested=%.3f blocker=%s lt=%lld\n",
                static_cast<double>(applied),
                static_cast<double>(speed.requested.load(std::memory_order_relaxed)),
                blocker ? blocker : "-",
                static_cast<long long>(tick)
            );
            gLastSpeed = applied;
            ++gSeg.speedChanges;
        }
        int const cls = speedClass();
        ++gSeg.spdFrames[cls];
        // Level ticks per wall second, measured: an external clock slows the world without touching `applied`.
        if (gPrevTick >= 0 && tick >= gPrevTick && gPrevQpc && now > gPrevQpc) {
            gSeg.spdTicks[cls] += static_cast<uint64_t>(tick - gPrevTick);
            gSeg.spdMs[cls]    += qpcMs(now - gPrevQpc);
        }
        gPrevTick = tick;
        gPrevQpc  = now;
    }
    tok('B');
    if (frame % 1800 == 0) {
        segPrint("segprog");
        summarize(false);
    }
}

void frameEnd() {
    if (!gOn.load(std::memory_order_relaxed)) return;
    auto const now = qpcNow();
    gInFrame.store(false, std::memory_order_relaxed);
    std::lock_guard const guard(gMain);
    auto const            frame = gFrame.load(std::memory_order_relaxed);
    gRenderEnd                  = now;
    tok('E');
    int const mode = modeIndex();
    for (auto it = gCells.begin(); it != gCells.end();) {
        Cell& cell = it->second;
        evalCell(cell, frame, mode);
        bool erase = false;
        if (!cell.confirmed) {
            if (frame - cell.birthFrame >= 120) {
                gC.cellsExpired.fetch_add(1, std::memory_order_relaxed);
                erase = true;
            }
        } else if (frame - cell.lastActivity >= 90 && frame - cell.lastDrawFrame >= 90) {
            closeCell(cell, frame);
            erase = true;
        }
        it = erase ? gCells.erase(it) : std::next(it);
    }
    if (gBucketLast.size() > 4096) gBucketLast.clear();
    if (frame % 600 == 0) bloomRebuild();
    shSweep();
    if (frame % 120 == 0) gOv.reap();
}

void renderBlockEntities(bool alphaLayer) {
    if (!gOn.load(std::memory_order_relaxed)) return;
    gC.rbeCalls[alphaLayer ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    gRbeSeen.store(true, std::memory_order_relaxed);
    std::lock_guard const guard(gMain);
    tok(alphaLayer ? 'a' : 'r');
}

void drawPos(::MovingBlockActor const& actor, float alpha, bool proxy) {
    if (!gOn.load(std::memory_order_relaxed)) return;
    bool const rt = onRenderThread();
    (rt ? gC.dpRender : gC.dpOther).fetch_add(1, std::memory_order_relaxed);
    if (alpha <= 0.0f) gC.dpAlpha0.fetch_add(1, std::memory_order_relaxed);
    else if (alpha >= 1.0f) gC.dpAlpha1.fetch_add(1, std::memory_order_relaxed);
    else gC.dpAlphaMid.fetch_add(1, std::memory_order_relaxed);
    if (proxy) gC.dpProxy.fetch_add(1, std::memory_order_relaxed);
    if (!rt) return;
    auto const            pos   = actor.mPosition.get();
    auto const&           il    = actor.mTerrainInterlockData.get();
    int const             state = std::min(2, static_cast<int>(il.mRenderVisibilityState));
    bool const            dd    = il.mHasBeenDelayedDeleted;
    auto const            key   = packKey(pos.x, pos.y, pos.z);
    int const             mode  = modeIndex();
    std::lock_guard const guard(gMain);
    auto const            frame = gFrame.load(std::memory_order_relaxed);
    gFr.drawN++;
    tok('D');
    gBucketLast[bucketKey(pos.x, pos.y, pos.z)] = frame;
    gSeg.vis[mode][state][dd ? 1 : 0]++;
    auto it = gCells.find(key);
    if (it == gCells.end()) {
        gSeg.drawUntracked[mode]++;
        return;
    }
    auto& cell = it->second;
    cell.drawCalls++;
    cell.lastActivity = frame;
    if (cell.visState != state) {
        char what[8];
        std::snprintf(what, sizeof(what), "v%d", state);
        ev(cell, what, frame, gCurTick);
        cell.visState = state;
    }
    cell.dd = dd;
    if (cell.lastDrawFrame != frame) {
        cell.drawFrames++;
        if (cell.lastDrawFrame == 0) ev(cell, "d", frame, gCurTick);
        else if (cell.lastDrawFrame + 1 < frame) {
            ev(cell, "e", cell.lastDrawFrame, -1);
            ev(cell, "d", frame, gCurTick);
        }
    }
    cell.lastDrawFrame = frame;
    if (state == 1) cell.lastVisDrawFrame = frame;
}

void tessellate(int x, int y, int z, void const* block) {
    if (!tBuild) return;
    auto const key = packKey(x, y, z);
    if (!bloomTest(key)) return;
    auto const            t0         = tBuild->beginQpc;
    auto const            seqAtBegin = tBuild->seqAtBegin;
    auto const            now        = qpcNow();
    std::lock_guard const guard(gMain);
    auto const            it = gChanges.find(key);
    if (it == gChanges.end()) return;
    auto const& change = it->second;
    int const   mode   = modeIndex();
    // A change that finished before this build read the sequence number must be visible in either model (premise c
    // of the evidence rule). The sequence number is bumped after the chunk write, so the build reads the chunk after
    // it.
    if (change.seq <= seqAtBegin) {
        if (block == change.now) {
            gSeg.preOk[mode]++;
            gSeg.preMargin[mode][msBucket(signedMs(t0, change.enter), kMarginEdges)]++;
        } else if (block == change.before) {
            double const age = qpcMs(now > change.done ? now - change.done : 0);
            gSeg.preStale[mode]++;
            gSeg.preStaleAge[mode][msBucket(age, kLatEdges)]++;
            logSeg(
                CatPremise,
                40,
                "premiseC STALE (%d,%d,%d) mode=%s changeSeq=%llu beginSeq=%llu done->now=%.2fms enter->begin=%.2fms\n",
                x,
                y,
                z,
                modeName(mode),
                static_cast<ull>(change.seq),
                static_cast<ull>(seqAtBegin),
                age,
                signedMs(t0, change.enter)
            );
        } else gSeg.preOther[mode]++;
        return;
    }
    double const offset = signedMs(change.enter, t0);
    if (block == change.now) {
        // Build started more than 1 ms before the listener ran, yet it sees the new block: it reads the live chunk.
        if (offset > 1.0) {
            gC.p2Live.fetch_add(1, std::memory_order_relaxed);
            gSeg.p2off[0][msBucket(offset, kOffEdges)]++;
        } else gC.p2Ambiguous.fetch_add(1, std::memory_order_relaxed);
    } else if (block == change.before) {
        // Listener returned more than 1 ms ago and the build still sees the old block: it reads a copy.
        double const age = qpcMs(now > change.done ? now - change.done : 0);
        if (age > 1.0) {
            gC.p2Snapshot.fetch_add(1, std::memory_order_relaxed);
            gSeg.p2off[1][msBucket(offset, kOffEdges)]++;
            gSeg.p2age[msBucket(age, kLatEdges)]++;
        } else gC.p2Ambiguous.fetch_add(1, std::memory_order_relaxed);
    } else {
        gC.p2Third.fetch_add(1, std::memory_order_relaxed);
        gSeg.p2off[2][msBucket(offset, kOffEdges)]++;
    }
}

void tessellated(int x, int y, int z, void const* block, bool produced, bool suppressed) {
    if (!tBuild) return;
    auto const key = packKey(x, y, z);
    if (!bloomTest(key)) return;
    if (tBuild->items.size() < 8192)
        tBuild->items.push_back(ItemRec{key, static_cast<::Block const*>(block), produced, suppressed, x, y, z});
}

void buildBegin(void const* geometry, int x, int y, int z, unsigned char version) {
    if (!gOn.load(std::memory_order_relaxed)) return;
    auto res        = std::make_shared<BuildRes>();
    res->pos        = ::BlockPos{x, y, z};
    res->version    = version;
    res->beginQpc   = qpcNow();
    res->trackSeq   = gTrackSeq.load(std::memory_order_relaxed);
    res->seqAtBegin = gSeq.load(std::memory_order_acquire);
    tBuild          = res;
    gC.buildBegin.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard const guard(gMain);
    if (gBuilding.size() > 2048) gBuilding.clear();
    if (!gBuilding.emplace(geometry, res).second) {
        gC.reuseInFlight.fetch_add(1, std::memory_order_relaxed);
        gBuilding[geometry] = res;
    }
    auto const [vit, inserted] = gLastVersion.try_emplace(res->pos, version);
    if (!inserted) {
        if (vit->second == version) gC.verRepeat.fetch_add(1, std::memory_order_relaxed);
        vit->second = version;
    }
    if (gBuilding.size() > gC.inFlightMax.load(std::memory_order_relaxed))
        gC.inFlightMax.store(gBuilding.size(), std::memory_order_relaxed);
    auto const sit = gSubs.find(res->pos);
    if (sit != gSubs.end()) {
        res->dirtyQpc     = sit->second.dirtyQpc;
        res->dirtyTick    = sit->second.dirtyTick;
        res->trackedDirty = sit->second.tracked;
        gSubs.erase(sit);
    }
}

void buildEnd() {
    if (tBuild) {
        tBuild->endQpc = qpcNow();
        tBuild.reset();
    }
}

uint64_t commitBegin() { return gOn.load(std::memory_order_relaxed) ? qpcNow() : 0; }

void buildCommit(void const* geometry, uint64_t commitBeginQpc) {
    if (!gOn.load(std::memory_order_relaxed)) return;
    auto const now = qpcNow();
    gC.buildCommit.fetch_add(1, std::memory_order_relaxed);
    bool const rt = onRenderThread();
    if (!rt) gC.commitOffRender.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard const guard(gMain);
    if (rt) {
        gFr.commitN++;
        gFr.commitMs += commitBeginQpc ? qpcMs(now - commitBeginQpc) : 0.0;
        tok('C');
    }
    auto const it = gBuilding.find(geometry);
    if (it == gBuilding.end()) {
        gC.commitNoBegin.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto res = it->second;
    gBuilding.erase(it);
    res->commitQpc = now;
    res->ledgerKey = static_cast<core::BuildKey>(reinterpret_cast<uintptr_t>(geometry));
    if (gLedger.size() >= 4096) gLedger.clear();
    gLedger.buildBegan(
        res->ledgerKey,
        core::subChunkOf(core::CellPos{res->pos.x, res->pos.y, res->pos.z}),
        res->seqAtBegin
    );
    for (auto const& item : res->items)
        gLedger.tessellated(
            res->ledgerKey,
            core::CellPos{item.x, item.y, item.z},
            item.suppressed ? core::Tess::Suppressed : item.produced ? core::Tess::Produced : core::Tess::Skipped
        );
    for (auto const& item : res->items) res->idx[item.key] = item;
    res->items.clear();
    res->items.shrink_to_fit();
    if (gCommittedGeom.size() > 4096) gCommittedGeom.clear();
    if (gCommittedPos.size() > 8192) gCommittedPos.clear();
    gCommittedGeom[geometry] = res;
    gCommittedPos[res->pos]  = res;
}

void mark(std::string const& label) {
    if (!gOn.load(std::memory_order_relaxed)) return;
    std::lock_guard const guard(gMain);
    segPrint("seg");
    gSeg       = Seg{};
    gSeg.index = ++gSegIndex;
    gSeg.label = label;
    gSeg.f0    = gFrame.load(std::memory_order_relaxed);
    gSeg.q0    = qpcNow();
    gSeg.lt0   = gCurTick;
    rawLog(
        "mark #%u label=%s piston=%s lt=%lld cells=%zu\n",
        gSegIndex,
        label.c_str(),
        modeName(modeIndex()),
        static_cast<long long>(gCurTick),
        gCells.size()
    );
}

} // namespace optipiston::platform::observe

#endif
