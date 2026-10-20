#include <doctest/doctest.h>

#include "core/Evidence.h"
#include "core/Overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace optipiston::core;

namespace {

CellPos cellAt(int x, int y = 0, int z = 0) { return CellPos{x, y, z}; }

// ---------------------------------------------------------------------------------------------------------------
// Hand-driven harness: the test plays the platform's role and feeds one event at a time.
// ---------------------------------------------------------------------------------------------------------------
struct Harness {
    Overlay        overlay;
    EvidenceLedger ledger;
    BuildKey       nextKey{1};

    // A build of the sub-chunk holding `cell` that began at `beginSeq`, then went live.
    void promote(CellPos const& cell, Seq beginSeq, std::vector<std::pair<CellPos, Tess>> const& seen, Seq now) {
        auto const key = nextKey++;
        ledger.buildBegan(key, subChunkOf(cell), beginSeq);
        for (auto const& [at, what] : seen) ledger.tessellated(key, at, what);
        overlay.promoted(key, ledger, now);
        ledger.drop(key);
    }
};

} // namespace

TEST_CASE("sub-chunk index floors negative coordinates") {
    CHECK(subChunkOf(cellAt(0, 0, 0)) == cellAt(0, 0, 0));
    CHECK(subChunkOf(cellAt(15, 15, 15)) == cellAt(0, 0, 0));
    CHECK(subChunkOf(cellAt(16, 0, 0)) == cellAt(1, 0, 0));
    CHECK(subChunkOf(cellAt(-1, -16, -17)) == cellAt(-1, -1, -2));
}

TEST_CASE("a build proves nothing unless it began at or after the change") {
    EvidenceLedger ledger;
    CHECK(ledger.judge(1, cellAt(1), 5) == Seen::Stale);

    ledger.buildBegan(1, cellAt(0, 0, 0), 5);
    ledger.tessellated(1, cellAt(1), Tess::Produced);
    CHECK(ledger.judge(1, cellAt(1), 6) == Seen::Stale);
    CHECK(ledger.judge(1, cellAt(1), 5) == Seen::Present);
    CHECK(ledger.judge(1, cellAt(1), 0) == Seen::Present);
}

TEST_CASE("a cell the build never reported is absent from it") {
    EvidenceLedger ledger;
    ledger.buildBegan(7, cellAt(0, 0, 0), 3);
    CHECK(ledger.judge(7, cellAt(2), 3) == Seen::Absent);
    ledger.tessellated(7, cellAt(2), Tess::Skipped);
    CHECK(ledger.judge(7, cellAt(2), 3) == Seen::Absent);
}

TEST_CASE("repeated reports for one cell keep the strongest") {
    EvidenceLedger ledger;
    ledger.buildBegan(1, cellAt(0, 0, 0), 1);
    ledger.tessellated(1, cellAt(4), Tess::Produced);
    ledger.tessellated(1, cellAt(4), Tess::Skipped);
    CHECK(ledger.judge(1, cellAt(4), 1) == Seen::Present);
    ledger.tessellated(1, cellAt(4), Tess::Suppressed);
    CHECK(ledger.judge(1, cellAt(4), 1) == Seen::Suppressed);
    ledger.tessellated(1, cellAt(4), Tess::Produced);
    CHECK(ledger.judge(1, cellAt(4), 1) == Seen::Suppressed);
}

TEST_CASE("a reused build key starts a fresh record and dropped builds are forgotten") {
    EvidenceLedger ledger;
    ledger.buildBegan(1, cellAt(0, 0, 0), 1);
    ledger.tessellated(1, cellAt(4), Tess::Produced);
    ledger.buildBegan(1, cellAt(0, 0, 0), 9);
    CHECK(ledger.judge(1, cellAt(4), 9) == Seen::Absent);
    ledger.tessellated(99, cellAt(4), Tess::Produced); // unknown build: ignored
    CHECK_FALSE(ledger.known(99));
    ledger.drop(1);
    CHECK_FALSE(ledger.known(1));
    CHECK(ledger.size() == 0);
}

TEST_CASE("the proxy waits at the source until a later build drops the block from the mesh") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    CHECK(h.overlay.phase(1) == Phase::Gated);
    CHECK(h.overlay.pose(1) == Pose::Source);
    CHECK(h.overlay.gateCell(1) == cellAt(1));

    // Began before the block left: it may still contain the block.
    h.promote(cellAt(1), 4, {}, 6);
    CHECK(h.overlay.phase(1) == Phase::Gated);
    auto const asked = h.overlay.takeRebuilds();
    CHECK(std::find(asked.begin(), asked.end(), cellAt(1)) != asked.end());

    // Began after, and the cell produced nothing.
    h.promote(cellAt(1), 5, {}, 7);
    CHECK(h.overlay.phase(1) == Phase::Animating);
    CHECK(h.overlay.pose(1) == Pose::Moving);
}

TEST_CASE("a build that still contains the block at the source does not open the gate") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 8, {{cellAt(1), Tess::Produced}}, 9);
    CHECK(h.overlay.phase(1) == Phase::Gated);
}

TEST_CASE("a source another object keeps out of the mesh counts as clear") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 6, {{cellAt(1), Tess::Suppressed}}, 7);
    CHECK(h.overlay.phase(1) == Phase::Animating);
}

TEST_CASE("the destination stays out of the mesh until the visual ends") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    CHECK(h.overlay.meshSuppressed(cellAt(2)));
    // The source stays out of new builds while the old copy may still be on screen.
    CHECK(h.overlay.meshSuppressed(cellAt(1)));
    h.promote(cellAt(1), 5, {}, 6);
    CHECK(h.overlay.meshSuppressed(cellAt(2)));
    CHECK_FALSE(h.overlay.meshSuppressed(cellAt(1)));
    h.overlay.visualEnded(1, 7);
    CHECK(h.overlay.phase(1) == Phase::Holding);
    CHECK_FALSE(h.overlay.meshSuppressed(cellAt(2)));
}

TEST_CASE("the proxy lets go only for a build that began after the mod stopped suppressing the cell") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 5, {}, 6);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 8); // landed while the visual still ran
    h.overlay.visualEnded(1, 10);
    CHECK(h.overlay.phase(1) == Phase::Holding);
    CHECK(h.overlay.needSeq(1) == 10);

    // The block landed at 8 but suppression lasted until 10: a build from 9 saw the cell suppressed or empty.
    h.promote(cellAt(2), 9, {{cellAt(2), Tess::Produced}}, 11);
    CHECK(h.overlay.phase(1) == Phase::Holding);
    h.promote(cellAt(2), 10, {{cellAt(2), Tess::Produced}}, 12);
    CHECK(h.overlay.phase(1) == Phase::Done);
    CHECK(h.overlay.endReason(1) == EndReason::Mesh);
    CHECK(h.overlay.pose(1) == Pose::None);
}

TEST_CASE("an empty destination is not an end") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 5, {}, 6);
    h.overlay.visualEnded(1, 8);
    h.overlay.worldChanged(cellAt(2), WorldCell::Air, 9);
    // Plenty of builds see an empty cell; none of them end the hold.
    for (Seq begin = 9; begin < 20; ++begin) h.promote(cellAt(2), begin, {}, begin + 1);
    CHECK(h.overlay.phase(1) == Phase::Holding);
    CHECK(h.overlay.pose(1) == Pose::Dest);

    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 30);
    CHECK(h.overlay.needSeq(1) == 30);
    h.promote(cellAt(2), 29, {{cellAt(2), Tess::Produced}}, 31);
    CHECK(h.overlay.phase(1) == Phase::Holding);
    h.promote(cellAt(2), 30, {{cellAt(2), Tess::Produced}}, 32);
    CHECK(h.overlay.phase(1) == Phase::Done);
}

TEST_CASE("a landing that happens before the object starts is honoured") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 6, 7);
    h.promote(cellAt(1), 7, {}, 8);
    h.overlay.visualEnded(1, 9);
    CHECK(h.overlay.needSeq(1) == 9);
    h.promote(cellAt(2), 9, {{cellAt(2), Tess::Produced}}, 10);
    CHECK(h.overlay.phase(1) == Phase::Done);
}

TEST_CASE("a visual that ends behind a closed gate holds at the destination only after the gate opens") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.overlay.visualEnded(1, 20);
    CHECK(h.overlay.phase(1) == Phase::Gated);
    CHECK(h.overlay.pose(1) == Pose::Source);
    h.promote(cellAt(1), 21, {}, 22);
    CHECK(h.overlay.phase(1) == Phase::Holding);
    CHECK(h.overlay.pose(1) == Pose::Dest);
}

TEST_CASE("a build that produced nothing for a landed cell leaves nothing to cover") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 5, {}, 6);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 7);
    h.overlay.visualEnded(1, 8);
    h.promote(cellAt(2), 8, {}, 9);
    CHECK(h.overlay.phase(1) == Phase::Done);
    CHECK(h.overlay.endReason(1) == EndReason::Culled);
}

TEST_CASE("a stale or suppressed verdict asks the chunk to rebuild, once per round") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 5, {}, 6);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 7);
    h.overlay.visualEnded(1, 8);
    (void)h.overlay.takeRebuilds();

    h.promote(cellAt(2), 7, {{cellAt(2), Tess::Produced}}, 9);
    h.promote(cellAt(2), 8, {{cellAt(2), Tess::Suppressed}}, 10);
    auto const asked = h.overlay.takeRebuilds();
    CHECK(std::count(asked.begin(), asked.end(), cellAt(2)) == 1);
    CHECK(h.overlay.takeRebuilds().empty());
}

TEST_CASE("a newer object takes over the cell and the older one stops drawing") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 5, {}, 6);
    h.overlay.visualEnded(1, 8);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 9);

    h.overlay.start(2, cellAt(2), cellAt(3), 12, 0, 12);
    CHECK(h.overlay.phase(1) == Phase::Done);
    CHECK(h.overlay.endReason(1) == EndReason::Superseded);
    CHECK(h.overlay.phase(2) == Phase::Gated);
    CHECK(h.overlay.gateCell(2) == cellAt(2));
    CHECK(h.overlay.epoch(cellAt(2)) == 1);
    CHECK(h.overlay.epoch(cellAt(3)) == 1);
}

TEST_CASE("an object that never got past its gate hands the old gate to its successor") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    // Nothing has been promoted yet; the block lands and already moves on from the destination.
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 7);
    h.overlay.start(2, cellAt(2), cellAt(3), 9, 0, 9);
    CHECK(h.overlay.phase(1) == Phase::Done);
    CHECK(h.overlay.gateCell(2) == cellAt(1));

    // A build from after the second change says nothing about the first source.
    h.promote(cellAt(1), 4, {}, 10);
    CHECK(h.overlay.phase(2) == Phase::Gated);
    h.promote(cellAt(1), 5, {}, 11);
    CHECK(h.overlay.phase(2) == Phase::Gated); // its own source still needs a build from after seq 9
    h.promote(cellAt(1), 9, {}, 12);
    CHECK(h.overlay.phase(2) == Phase::Animating);
}

TEST_CASE("blocks of one chain move together and do not supersede each other") {
    Harness h;
    // Block B leaves cell 2 while block A arrives there.
    h.overlay.start(2, cellAt(2), cellAt(3), 6, 0, 6);
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 6);
    CHECK(h.overlay.active(1));
    CHECK(h.overlay.active(2));
    CHECK(h.overlay.meshSuppressed(cellAt(2)));
    CHECK(h.overlay.activeCount() == 2);
}

TEST_CASE("a landed block that is picked up while a chain neighbour arrives is superseded by its own push") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 7);
    // The next push moves A into cell 3 and B (the one that just landed in cell 2) further; the neighbour starts first.
    h.overlay.start(3, cellAt(0), cellAt(2), 9, 0, 9);
    CHECK(h.overlay.active(1));
    h.overlay.start(2, cellAt(2), cellAt(3), 9, 0, 9);
    CHECK_FALSE(h.overlay.active(1));
    CHECK(h.overlay.endReason(1) == EndReason::Superseded);
    CHECK(h.overlay.active(2));
    CHECK(h.overlay.active(3));
}

TEST_CASE("every gate must be cleared before the proxy glides") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 7);
    h.overlay.start(2, cellAt(2), cellAt(3), 9, 0, 9);
    CHECK(h.overlay.meshSuppressed(cellAt(1)));
    CHECK(h.overlay.meshSuppressed(cellAt(2)));

    // The first gate clears, the second source has not been proven gone.
    h.promote(cellAt(1), 5, {}, 10);
    CHECK(h.overlay.phase(2) == Phase::Gated);
    CHECK(h.overlay.gateCell(2) == cellAt(2));
    CHECK_FALSE(h.overlay.meshSuppressed(cellAt(1)));
    CHECK(h.overlay.meshSuppressed(cellAt(2)));

    h.promote(cellAt(2), 9, {}, 11);
    CHECK(h.overlay.phase(2) == Phase::Animating);
    CHECK_FALSE(h.overlay.meshSuppressed(cellAt(2)));
}

TEST_CASE("the platform can abandon an object") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.overlay.abandon(1, 6);
    CHECK(h.overlay.phase(1) == Phase::Done);
    CHECK(h.overlay.endReason(1) == EndReason::Abandoned);
    CHECK_FALSE(h.overlay.meshSuppressed(cellAt(2)));
    h.overlay.reap();
    CHECK(h.overlay.phase(1) == Phase::Done);
    CHECK(h.overlay.endReason(1) == EndReason::None);
}

TEST_CASE("the evidence floor never moves back") {
    Harness h;
    h.overlay.start(1, cellAt(1), cellAt(2), 5, 0, 5);
    h.promote(cellAt(1), 5, {}, 6);
    h.overlay.visualEnded(1, 8);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 20);
    CHECK(h.overlay.needSeq(1) == 20);
    h.overlay.worldChanged(cellAt(2), WorldCell::Air, 21);
    h.overlay.worldChanged(cellAt(2), WorldCell::Real, 22);
    CHECK(h.overlay.needSeq(1) == 22);
}

// ---------------------------------------------------------------------------------------------------------------
// Deterministic simulator. A block row per block; pushes follow the server's order: the source empties, the
// destination turns into a moving block, the visual runs, then the destination is air for a while and finally the
// real block. Meshes are built by asynchronous workers that read the chunk when they like, commit late and go live
// after a delay. The checks run after every frame:
//   I1 each block has a visible copy at all times
//   I2 all visible copies of a block sit at the same place
//   I3 the evidence floor and the cell epochs never move back
// ---------------------------------------------------------------------------------------------------------------
namespace {

struct CellLess {
    bool operator()(CellPos const& a, CellPos const& b) const noexcept {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

enum class ReadMode { Snapshot, Live, Mixed };

struct SimConfig {
    ReadMode mode{ReadMode::Mixed};
    int      framesPerTick{3};
    int      debounceMax{2};
    int      buildMin{1};
    int      buildMax{6};
    int      promoteMax{3};
    int      hitchPercent{0};
    int      hitchFrames{0};
    int      landJitter{0}; // extra ticks, per block, before the real block lands
    bool     shuffle{false}; // randomise the order of the world writes and of the object starts within a push
    int      suppressionLag{0}; // frames until a worker sees a newly kept-out cell; always 0 for soundness
    uint64_t seed{1};
};

struct Content {
    WorldCell kind{WorldCell::Air};
    int       block{};
};

class Sim {
public:
    explicit Sim(SimConfig const& config) : mCfg(config), mRng(config.seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}

    // A block sits in `row`; blocks of one row at adjacent cells form a chain that a push moves together.
    void addBlock(int block, int x, int row = -1) {
        int const r  = row < 0 ? block : row;
        mRow[block]  = r;
        mCurX[block] = x;
        mOcc[{r, x}] = block;
        auto const cell = CellPos{x, r, 0};
        mWorld[cell]    = {WorldCell::Real, block};
        mMesh[cell]     = block;
        mCells.insert(cell);
        mBlocks.insert(block);
    }

    // Pushes `block` and every block queued behind it in direction `dir`. Returns the tick the last one lands.
    // A negative gap lands them before the visual ends.
    int push(int block, int tick, int dir, int length, int gap) {
        struct Leg {
            int      block{};
            CellPos  src{};
            CellPos  dst{};
            ObjectId id{};
            int      land{};
        };
        int const        row = mRow.at(block);
        int const        x0  = mCurX.at(block);
        std::vector<Leg> legs;
        for (int x = x0; mOcc.contains({row, x}); x += dir) {
            Leg leg;
            leg.block = mOcc.at({row, x});
            leg.src   = CellPos{x, row, 0};
            leg.dst   = CellPos{x + dir, row, 0};
            leg.id    = ++mLastId;
            leg.land  = std::max(tick + 1, tick + length + gap + (mCfg.landJitter > 0 ? rand(0, mCfg.landJitter) : 0));
            mCells.insert(leg.src);
            mCells.insert(leg.dst);
            mInfo[leg.id] = {leg.block, leg.src, leg.dst, tick * mCfg.framesPerTick, length * mCfg.framesPerTick};
            legs.push_back(leg);
        }
        for (auto const& leg : legs) mOcc.erase({row, leg.src.x});
        for (auto const& leg : legs) {
            mCurX[leg.block]       = leg.dst.x;
            mOcc[{row, leg.dst.x}] = leg.block;
        }

        at(tick, [this, legs] {
            // The back end empties; every other source turns into the moving block of the leg behind it.
            std::vector<std::pair<CellPos, Content>> writes;
            for (auto const& leg : legs) writes.push_back({leg.dst, {WorldCell::Moving, leg.block}});
            writes.push_back({legs.front().src, {WorldCell::Air, 0}});
            if (mCfg.shuffle)
                for (size_t i = writes.size(); i > 1; --i) std::swap(writes[i - 1], writes[static_cast<size_t>(rand(0, static_cast<int>(i) - 1))]);
            std::map<CellPos, Seq, CellLess> changed;
            for (auto const& [cell, content] : writes) changed[cell] = write(cell, content.kind, content.block);
            std::vector<size_t> order(legs.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            if (mCfg.shuffle)
                for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[static_cast<size_t>(rand(0, static_cast<int>(i) - 1))]);
            for (auto const index : order) {
                auto const& leg = legs[index];
                mOverlay.start(leg.id, leg.src, leg.dst, changed.at(leg.src), 0, mSeq);
                trace(leg.src.y, "start id" + std::to_string(leg.id) + " b" + std::to_string(leg.block) + " " + std::to_string(leg.src.x) + "->" + std::to_string(leg.dst.x) + " gate x" + std::to_string(mOverlay.gateCell(leg.id).x));
                noteEpoch(leg.dst);
            }
        });
        at(tick + length, [this, legs, gap] {
            for (auto const& leg : legs) {
                mOverlay.visualEnded(leg.id, mSeq);
                trace(leg.src.y, "visualEnded id" + std::to_string(leg.id));
                if (gap > 0) write(leg.dst, WorldCell::Air, 0);
            }
        });
        int last = tick + length;
        for (auto const& leg : legs) {
            at(leg.land, [this, leg] { write(leg.dst, WorldCell::Real, leg.block); });
            last = std::max(last, leg.land);
        }
        mLastTick = std::max(mLastTick, last);
        return last;
    }

    void run() {
        int const last = (mLastTick + 2) * mCfg.framesPerTick;
        while (mFrame < last) step();
        int const cap = mFrame + 4000;
        while (!idle() && mFrame < cap) step();
        mStuck = !idle();
    }

    // Counters
    int  absent{};
    int  doubled{};
    int  floorRegress{};
    int  epochRegress{};
    int  gatedFrames{};
    int  holdFrames{};
    int  builds{};
    bool stuck() const { return mStuck; }
    [[nodiscard]] std::string dump() const {
        std::string out;
        for (auto const& [id, info] : mInfo) {
            auto const phase = mOverlay.phase(id);
            if (phase == Phase::Done) continue;
            if (auto const h = mHistory.find(info.src.y); h != mHistory.end() && out.empty()) out += h->second;
            out += "  id" + std::to_string(id) + " b" + std::to_string(info.block) + " " + std::to_string(info.src.x) + "->" + std::to_string(info.dst.x) + " row" + std::to_string(info.src.y) + " phase " + std::to_string(static_cast<int>(phase)) + " need " + std::to_string(mOverlay.needSeq(id)) + "\n";
        }
        return out;
    }
    std::string firstViolation;

    [[nodiscard]] bool settledAtGoal() const {
        for (auto const block : mBlocks) {
            int copies = 0;
            for (auto const& cell : mCells)
                if (cell.y == mRow.at(block) && mMesh.contains(cell) && mMesh.at(cell) == block) ++copies;
            if (copies != 1) return false;
            auto const cell = CellPos{mCurX.at(block), mRow.at(block), 0};
            if (!mMesh.contains(cell) || mMesh.at(cell) != block) return false;
        }
        return true;
    }

private:
    struct Info {
        int     block{};
        CellPos src{};
        CellPos dst{};
        int     startFrame{};
        int     lengthFrames{};
    };
    struct Read {
        CellPos cell;
        int     readFrame{};
        bool    snapshot{};
        Content snap;
        bool    done{};
        int     result{};
    };
    struct Build {
        BuildKey          key{};
        Seq               beginSeq{};
        int               commitFrame{};
        int               promoteFrame{};
        std::vector<Read> reads;
    };
    struct SubState {
        bool                 dirty{};
        int                  dirtyFrame{};
        int                  debounce{};
        std::optional<Build> build;
    };

    int rand(int lo, int hi) {
        if (hi <= lo) return lo;
        mRng ^= mRng >> 12;
        mRng ^= mRng << 25;
        mRng ^= mRng >> 27;
        return lo + static_cast<int>((mRng * 0x2545F4914F6CDD1Dull >> 33) % static_cast<uint64_t>(hi - lo + 1));
    }

    void at(int tick, std::function<void()> fn) { mEvents[tick].push_back(std::move(fn)); }

    void trace(int block, std::string const& text) { mHistory[block] += "  f" + std::to_string(mFrame) + " " + text + "\n"; }

    Seq write(CellPos const& cell, WorldCell kind, int block) {
        mWorld[cell] = {kind, block};
        ++mSeq;
        trace(cell.y, "seq" + std::to_string(mSeq) + " world x" + std::to_string(cell.x) + "=" + std::to_string(static_cast<int>(kind)));
        markDirty(subChunkOf(cell));
        mOverlay.worldChanged(cell, kind, mSeq);
        return mSeq;
    }

    void markDirty(CellPos const& sub) {
        auto& state = mSubs[sub];
        if (!state.dirty) {
            state.dirty      = true;
            state.dirtyFrame = mFrame;
            state.debounce   = rand(0, mCfg.debounceMax);
        }
    }

    void noteEpoch(CellPos const& dst) {
        auto const now = mOverlay.epoch(dst);
        if (now <= mEpochSeen[dst]) ++epochRegress;
        mEpochSeen[dst] = now;
    }

    [[nodiscard]] bool idle() const {
        if (mOverlay.activeCount() != 0) return false;
        for (auto const& [sub, state] : mSubs)
            if (state.dirty || state.build) return false;
        return true;
    }

    void doRead(Build& build, Read& read) {
        if (read.done) return;
        read.done = true;
        Content const seen = read.snapshot ? read.snap : mWorld[read.cell];
        if (seen.kind != WorldCell::Real) return;
        bool const suppressed = mOverlay.meshSuppressed(read.cell);
        mLedger.tessellated(build.key, read.cell, suppressed ? Tess::Suppressed : Tess::Produced);
        read.result = suppressed ? 0 : seen.block;
    }

    void step() {
        int const tick = mFrame / mCfg.framesPerTick;
        if (mFrame % mCfg.framesPerTick == 0) {
            auto const found = mEvents.find(tick);
            if (found != mEvents.end()) {
                auto fns = std::move(found->second);
                mEvents.erase(found);
                for (auto& fn : fns) fn();
            }
        }
        promoteDue();
        for (auto const& cell : mOverlay.takeRebuilds()) markDirty(subChunkOf(cell));
        advanceReads();
        startBuilds();
        check();
        ++mFrame;
    }

    void promoteDue() {
        for (auto& [sub, state] : mSubs) {
            if (!state.build || mFrame < state.build->promoteFrame) continue;
            auto& build = *state.build;
            for (auto& read : build.reads) doRead(build, read);
            for (auto const& read : build.reads) mMesh[read.cell] = read.result;
            std::string line = "promote begin" + std::to_string(build.beginSeq) + " now" + std::to_string(mSeq) + " mesh:";
            for (auto const& read : build.reads) line += " x" + std::to_string(read.cell.x) + "y" + std::to_string(read.cell.y) + "=" + std::to_string(read.result) + (read.snapshot ? "s" : "l");
            mOverlay.promoted(build.key, mLedger, mSeq);
            mLedger.drop(build.key);
            for (auto const& [id, info] : mInfo) line += " |id" + std::to_string(id) + ":" + std::to_string(static_cast<int>(mOverlay.phase(id)));
            for (auto const& read : build.reads) trace(read.cell.y, line);
            state.build.reset();
        }
    }

    void advanceReads() {
        for (auto& [sub, state] : mSubs) {
            if (!state.build) continue;
            for (auto& read : state.build->reads)
                if (!read.done && read.readFrame <= mFrame) doRead(*state.build, read);
        }
    }

    void startBuilds() {
        for (auto& [sub, state] : mSubs) {
            if (state.build || !state.dirty || mFrame < state.dirtyFrame + state.debounce) continue;
            state.dirty = false;
            Build build;
            build.key = ++mLastKey;
            ++builds;
            bool const hitch = mCfg.hitchPercent > 0 && rand(1, 100) <= mCfg.hitchPercent;
            int const  dur   = hitch ? mCfg.hitchFrames : rand(mCfg.buildMin, mCfg.buildMax);
            build.commitFrame  = mFrame + dur;
            build.promoteFrame = build.commitFrame + rand(0, mCfg.promoteMax);
            build.beginSeq = mSeq;
            mLedger.buildBegan(build.key, sub, mSeq);
            for (auto const& cell : mCells) {
                if (subChunkOf(cell) != sub) continue;
                Read read;
                read.cell      = cell;
                read.readFrame = mFrame + rand(0, dur);
                read.snapshot  = mCfg.mode == ReadMode::Snapshot || (mCfg.mode == ReadMode::Mixed && rand(0, 1) == 1);
                read.snap      = mWorld[cell];
                build.reads.push_back(read);
            }
            state.build = std::move(build);
        }
    }

    void note(std::string const& text, int block) {
        if (firstViolation.empty()) firstViolation = text + "\n" + mHistory[mRow.at(block)];
    }

    [[nodiscard]] double proxyPos(ObjectId id, Info const& info) const {
        switch (mOverlay.pose(id)) {
        case Pose::Source: return mOverlay.gateCell(id).x;
        case Pose::Dest: return info.dst.x;
        case Pose::Moving: {
            double const t = std::clamp(
                static_cast<double>(mFrame - info.startFrame) / std::max(1, info.lengthFrames), 0.0, 1.0
            );
            return info.src.x + t * (info.dst.x - info.src.x);
        }
        default: return std::nan("");
        }
    }

    void check() {
        for (auto const block : mBlocks) {
            std::vector<double> at;
            for (auto const& cell : mCells)
                if (cell.y == mRow.at(block)) {
                    auto const found = mMesh.find(cell);
                    if (found != mMesh.end() && found->second == block) at.push_back(cell.x);
                }
            for (auto const& [id, info] : mInfo) {
                if (info.block != block) continue;
                auto const pos = proxyPos(id, info);
                if (!std::isnan(pos)) at.push_back(pos);
            }
            auto const tag = "frame " + std::to_string(mFrame) + " block " + std::to_string(block) + " copies "
                           + std::to_string(at.size());
            if (at.empty()) {
                ++absent;
                note("absent: " + tag, block);
            } else if (*std::max_element(at.begin(), at.end()) - *std::min_element(at.begin(), at.end()) > 1e-6) {
                ++doubled;
                note("double: " + tag, block);
            }
        }
        for (auto const& [id, info] : mInfo) {
            auto const phase = mOverlay.phase(id);
            auto const need  = mOverlay.needSeq(id);
            if (need < mNeedSeen[id]) ++floorRegress;
            mNeedSeen[id] = std::max(mNeedSeen[id], need);
            if (phase == Phase::Gated && mFrame >= info.startFrame) ++gatedFrames;
            if (phase == Phase::Holding) ++holdFrames;
        }
    }

    SimConfig mCfg;
    uint64_t  mRng;
    int       mFrame{};
    int       mLastTick{};
    bool      mStuck{};
    Seq       mSeq{};
    ObjectId  mLastId{};
    BuildKey  mLastKey{};

    Overlay        mOverlay;
    EvidenceLedger mLedger;

    std::map<CellPos, Content, CellLess> mWorld;
    std::map<CellPos, int, CellLess>      mMesh;
    std::set<CellPos, CellLess>           mCells;
    std::map<CellPos, SubState, CellLess> mSubs;
    std::map<CellPos, uint64_t, CellLess> mEpochSeen;
    std::map<int, int>                                                    mCurX;
    std::map<int, int>                                                    mRow;
    std::map<std::pair<int, int>, int>                                    mOcc; // (row, x) -> block, logical
    std::set<int>                                                         mBlocks;
    std::map<ObjectId, Info>                                              mInfo;
    std::map<ObjectId, Seq>                                               mNeedSeen;
    std::map<int, std::string>                                            mHistory;
    std::map<int, std::vector<std::function<void()>>>                     mEvents;
};

} // namespace

namespace {

void expectClean(Sim const& sim) {
    INFO(sim.firstViolation);
    INFO(sim.dump());
    CHECK(sim.absent == 0);
    CHECK(sim.doubled == 0);
    CHECK(sim.floorRegress == 0);
    CHECK(sim.epochRegress == 0);
    CHECK_FALSE(sim.stuck());
    CHECK(sim.settledAtGoal());
}

} // namespace

TEST_CASE("sim: one push with fast builds never loses or doubles the block") {
    SimConfig config;
    Sim       sim(config);
    sim.addBlock(1, 5);
    sim.push(1, 10, +1, 4, 0);
    sim.run();
    expectClean(sim);
}

TEST_CASE("sim: an air gap before the landing does not release the proxy") {
    for (int gap : {1, 3, 10, 25}) {
        SimConfig config;
        config.seed = static_cast<uint64_t>(gap);
        Sim sim(config);
        sim.addBlock(1, 5);
        sim.push(1, 10, +1, 4, gap);
        sim.run();
        INFO("gap " << gap);
        expectClean(sim);
    }
}

TEST_CASE("sim: a block that lands before the visual ends stays hidden until the visual is done") {
    SimConfig config;
    Sim       sim(config);
    sim.addBlock(1, 5);
    sim.push(1, 10, +1, 4, -2);
    sim.run();
    expectClean(sim);
}

TEST_CASE("sim: a build that straddles the landing never decides alone") {
    // Long builds begin before the push and finish after the landing; with Snapshot they read the old chunk.
    for (auto mode : {ReadMode::Snapshot, ReadMode::Live, ReadMode::Mixed}) {
        SimConfig config;
        config.mode      = mode;
        config.buildMin  = 20;
        config.buildMax  = 60;
        config.promoteMax = 15;
        Sim sim(config);
        sim.addBlock(1, 5);
        sim.push(1, 6, +1, 4, 3);
        sim.run();
        expectClean(sim);
    }
}

TEST_CASE("sim: a build hitch of several seconds delays the handover but never breaks the invariants") {
    SimConfig config;
    config.hitchPercent = 100;
    config.hitchFrames  = 400; // about 6.7 s at 60 fps
    config.mode         = ReadMode::Snapshot;
    Sim sim(config);
    sim.addBlock(1, 5);
    sim.push(1, 10, +1, 4, 2);
    sim.run();
    expectClean(sim);
    CHECK(sim.gatedFrames > 0);
}

TEST_CASE("sim: the same block pushed again before its mesh is on screen") {
    SimConfig config;
    config.buildMin   = 10;
    config.buildMax   = 30;
    config.promoteMax = 10;
    Sim sim(config);
    sim.addBlock(1, 5);
    int tick = 6;
    for (int i = 0; i < 6; ++i) tick = sim.push(1, tick, i % 3 == 2 ? -1 : +1, 3, 0);
    sim.run();
    expectClean(sim);
}

TEST_CASE("sim: a row of blocks that share one sub-chunk") {
    SimConfig config;
    Sim       sim(config);
    for (int block = 1; block <= 8; ++block) {
        sim.addBlock(block, 6);
        sim.push(block, 10 + block % 3, +1, 4, block % 4);
    }
    sim.run();
    expectClean(sim);
}

TEST_CASE("sim: pushes across a sub-chunk border") {
    SimConfig config;
    Sim       sim(config);
    sim.addBlock(1, 14);
    int tick = 5;
    for (int i = 0; i < 4; ++i) tick = sim.push(1, tick, +1, 3, 1);
    sim.run();
    expectClean(sim);
}

TEST_CASE("sim: seeded fuzz over build speed, read mode, gaps and chained pushes") {
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        SimConfig config;
        config.seed          = seed;
        config.mode          = static_cast<ReadMode>(seed % 3);
        config.framesPerTick = 1 + static_cast<int>(seed % 7);
        config.debounceMax   = static_cast<int>(seed % 5);
        config.buildMin      = 1;
        config.buildMax      = 2 + static_cast<int>(seed % 40);
        config.promoteMax    = static_cast<int>(seed % 20);
        config.hitchPercent  = seed % 4 == 0 ? 8 : 0;
        config.hitchFrames   = 120 + static_cast<int>(seed % 200);
        Sim sim(config);

        uint64_t pick = seed * 2654435761ull;
        auto     next = [&pick](int lo, int hi) {
            pick ^= pick << 13;
            pick ^= pick >> 7;
            pick ^= pick << 17;
            return lo + static_cast<int>(pick % static_cast<uint64_t>(hi - lo + 1));
        };
        int const blocks = next(1, 6);
        for (int block = 1; block <= blocks; ++block) {
            sim.addBlock(block, next(8, 24));
            int tick = next(1, 20);
            int const pushes = next(1, 6);
            for (int i = 0; i < pushes; ++i) {
                int const length = next(2, 4);
                int const gaps[] = {-1, 0, 0, 1, 3, 10, 25};
                int       gap    = gaps[next(0, 6)];
                gap              = std::max(gap, -(length - 1));
                int const land   = sim.push(block, tick, next(0, 1) == 0 ? -1 : +1, length, gap);
                tick             = land + next(0, 5);
            }
        }
        sim.run();
        INFO("seed " << seed);
        expectClean(sim);
        if (sim.absent != 0 || sim.doubled != 0 || sim.stuck()) break;
    }
}

TEST_CASE("sim: a chain of blocks pushed together keeps every block visible") {
    for (auto mode : {ReadMode::Snapshot, ReadMode::Live, ReadMode::Mixed}) {
        SimConfig config;
        config.mode       = mode;
        config.shuffle    = true;
        config.landJitter = 2;
        config.buildMax   = 12;
        Sim sim(config);
        for (int i = 0; i < 6; ++i) sim.addBlock(1 + i, 10 + i, 0);
        int tick = 8;
        tick     = sim.push(1, tick, +1, 4, 2);
        tick     = sim.push(4, tick + 1, -1, 4, 0);
        tick     = sim.push(3, tick + 2, +1, 3, 3);
        sim.run();
        INFO("mode " << static_cast<int>(mode));
        expectClean(sim);
    }
}

TEST_CASE("sim: seeded fuzz over chains with partial runs, shuffled writes and landing jitter") {
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        SimConfig config;
        config.seed          = seed;
        config.mode          = static_cast<ReadMode>(seed % 3);
        config.framesPerTick = 1 + static_cast<int>(seed % 7);
        config.debounceMax   = static_cast<int>(seed % 5);
        config.buildMax      = 2 + static_cast<int>(seed % 40);
        config.promoteMax    = static_cast<int>(seed % 20);
        config.hitchPercent  = seed % 4 == 0 ? 8 : 0;
        config.hitchFrames   = 120 + static_cast<int>(seed % 200);
        config.landJitter    = static_cast<int>(seed % 4);
        config.shuffle       = true;
        Sim sim(config);

        uint64_t pick = seed * 2654435761ull + 77;
        auto     next = [&pick](int lo, int hi) {
            pick ^= pick << 13;
            pick ^= pick >> 7;
            pick ^= pick << 17;
            return lo + static_cast<int>(pick % static_cast<uint64_t>(hi - lo + 1));
        };
        int const rows = next(1, 4);
        int       id   = 0;
        for (int row = 0; row < rows; ++row) {
            int const count = next(1, 8);
            int const x0    = next(8, 20);
            std::vector<int> ids;
            for (int i = 0; i < count; ++i) {
                ids.push_back(++id);
                sim.addBlock(id, x0 + i, row);
            }
            int tick = next(1, 20);
            int const pushes = next(1, 6);
            for (int i = 0; i < pushes; ++i) {
                int const length = next(2, 4);
                int const gaps[] = {-1, 0, 0, 1, 3, 10, 25};
                int       gap    = gaps[next(0, 6)];
                gap              = std::max(gap, -(length - 1));
                int const who    = ids[static_cast<size_t>(next(0, count - 1))];
                int const land   = sim.push(who, tick, next(0, 1) == 0 ? -1 : +1, length, gap);
                tick             = land + next(0, 5);
            }
        }
        sim.run();
        INFO("seed " << seed);
        expectClean(sim);
        if (sim.absent != 0 || sim.doubled != 0 || sim.stuck()) break;
    }
}