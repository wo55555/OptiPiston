#include <doctest/doctest.h>

#include "core/MeshView.h"
#include "core/PushTimeline.h"
#include "core/Segment.h"

#include <cstdint>
#include <vector>

using namespace optipiston::core;

namespace {

PushTimeline makeTimeline(bool actor) {
    PushTimeline timeline;
    timeline.start(1, makeSegment(10, 0.0f, 1.0f, 2.0f), actor);
    return timeline;
}

// The answers the mesh hooks used to compute from the timeline itself.
int referenceFaceHidden(PushTimeline const& timeline) {
    if (timeline.state() != PushState::Animating) return 0;
    return timeline.landed() ? 1 : 2;
}

struct Lcg {
    uint64_t state{0x9E3779B97F4A7C15ull};
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    }
};

// Applies one random game-side event to a timeline.
void mutate(PushTimeline& timeline, Lcg& rng, uint64_t& frame, int64_t& tick) {
    switch (rng.next() % 10) {
    case 0:
        timeline.blockLanded();
        break;
    case 1:
        timeline.remeshRequested(10.5);
        break;
    case 2:
        timeline.meshCommitted(frame);
        break;
    case 3:
        timeline.frameAdvanced(++frame);
        break;
    case 4:
        timeline.landedActorSeen(frame);
        break;
    case 5:
        (void)timeline.tick(++tick, 11.0);
        break;
    case 6:
        (void)timeline.visualEnded(11.5);
        break;
    case 7:
        if (rng.next() % 8 == 0) timeline.supersededBy(2);
        break;
    case 8:
        if (rng.next() % 16 == 0) timeline.cleared();
        break;
    default:
        break;
    }
}

CellPos cellAt(size_t index) { return CellPos{static_cast<int32_t>(index), 64, -3}; }

} // namespace

TEST_CASE("MeshView answers per cell kind") {
    MeshView view;
    CHECK(view.empty());
    CHECK(view.at(CellPos{1, 2, 3}) == MeshCell::None);
    CHECK(view.faceHidden(CellPos{1, 2, 3}) == 0);

    view.set(CellPos{0, 0, 0}, MeshCell::Approaching);
    view.set(CellPos{5, 0, 0}, MeshCell::Landed);
    view.set(CellPos{9, 0, 0}, MeshCell::Awaiting);
    view.set(CellPos{12, 0, 0}, MeshCell::Managed);

    CHECK(view.faceHidden(CellPos{0, 0, 0}) == 2);
    CHECK(view.faceHidden(CellPos{5, 0, 0}) == 1);
    CHECK(view.faceHidden(CellPos{9, 0, 0}) == 0);
    CHECK(view.faceHidden(CellPos{12, 0, 0}) == 0);

    CHECK_FALSE(view.suppressed(CellPos{0, 0, 0}));
    CHECK(view.suppressed(CellPos{5, 0, 0}));
    CHECK(view.awaiting(CellPos{9, 0, 0}));
    CHECK_FALSE(view.awaiting(CellPos{5, 0, 0}));
    CHECK(view.managed(CellPos{12, 0, 0}));
    CHECK_FALSE(view.managed(CellPos{13, 0, 0}));
    CHECK(view.size() == 4);
}

TEST_CASE("MeshView touchesSuppressed checks the six face neighbours only") {
    MeshView view;
    CHECK_FALSE(view.touchesSuppressed(CellPos{0, 0, 0}));

    view.set(CellPos{1, 0, 0}, MeshCell::Landed);
    CHECK(view.touchesSuppressed(CellPos{0, 0, 0}));
    CHECK(view.touchesSuppressed(CellPos{2, 0, 0}));
    CHECK_FALSE(view.touchesSuppressed(CellPos{1, 0, 0}));
    CHECK_FALSE(view.touchesSuppressed(CellPos{0, 1, 1}));
    CHECK_FALSE(view.touchesSuppressed(CellPos{3, 0, 0}));

    view.set(CellPos{1, 0, 0}, MeshCell::Awaiting);
    CHECK_FALSE(view.touchesSuppressed(CellPos{0, 0, 0}));

    view.set(CellPos{0, 0, 1}, MeshCell::Landed);
    CHECK(view.touchesSuppressed(CellPos{0, 0, 0}));
    view.erase(CellPos{0, 0, 1});
    CHECK_FALSE(view.touchesSuppressed(CellPos{0, 0, 0}));
    view.clear();
    CHECK(view.empty());
}

TEST_CASE("MeshView set with None erases and keeps the landed count in step") {
    MeshView view;
    view.set(CellPos{1, 0, 0}, MeshCell::Landed);
    view.set(CellPos{1, 0, 0}, MeshCell::Landed);
    view.set(CellPos{1, 0, 0}, MeshCell::None);
    CHECK(view.empty());
    CHECK_FALSE(view.touchesSuppressed(CellPos{0, 0, 0}));

    view.set(CellPos{1, 0, 0}, MeshCell::Landed);
    view.set(CellPos{1, 0, 0}, MeshCell::Awaiting);
    CHECK_FALSE(view.touchesSuppressed(CellPos{0, 0, 0}));
}

TEST_CASE("meshCellOf follows the timeline through a landing") {
    auto timeline = makeTimeline(false);
    CHECK(meshCellOf(timeline) == MeshCell::Approaching);
    timeline.blockLanded();
    CHECK(meshCellOf(timeline) == MeshCell::Landed);
    REQUIRE(timeline.remeshRequested(11.0));
    CHECK(meshCellOf(timeline) == MeshCell::Awaiting);
    timeline.meshCommitted(5);
    CHECK(meshCellOf(timeline) == MeshCell::None);

    auto cleared = makeTimeline(true);
    cleared.cleared();
    CHECK(meshCellOf(cleared) == MeshCell::None);
}

TEST_CASE("MeshView built from timelines agrees with the timeline predicates under random events") {
    constexpr size_t          Count = 24;
    std::vector<PushTimeline> timelines;
    for (size_t i = 0; i < Count; ++i) timelines.push_back(makeTimeline(i % 3 == 0));

    Lcg      rng;
    uint64_t frame   = 1;
    int64_t  tick    = 10;
    int      checked = 0;

    for (int step = 0; step < 4000; ++step) {
        auto const index = rng.next() % Count;
        mutate(timelines[index], rng, frame, tick);

        MeshView view;
        for (size_t i = 0; i < Count; ++i) view.set(cellAt(i), meshCellOf(timelines[i]));

        for (size_t i = 0; i < Count; ++i) {
            auto const& timeline = timelines[i];
            auto const  pos      = cellAt(i);
            REQUIRE(view.suppressed(pos) == timeline.meshSuppressed());
            REQUIRE(view.awaiting(pos) == timeline.awaitingMesh());
            REQUIRE(view.faceHidden(pos) == referenceFaceHidden(timeline));
            ++checked;
        }
        // Neighbours along x; the cells sit on a line.
        for (int x = -1; x <= static_cast<int>(Count); ++x) {
            bool expected = false;
            for (int d : {-1, 1}) {
                auto const n = x + d;
                if (n >= 0 && n < static_cast<int>(Count) && timelines[static_cast<size_t>(n)].meshSuppressed())
                    expected = true;
            }
            REQUIRE(view.touchesSuppressed(CellPos{x, 64, -3}) == expected);
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("TraitCensus kept by replace() equals a recount") {
    constexpr size_t          Count = 20;
    std::vector<PushTimeline> timelines;
    std::vector<uint8_t>      traits(Count, 0);
    TraitCensus               census;
    for (size_t i = 0; i < Count; ++i) {
        timelines.push_back(makeTimeline(i % 2 == 0));
        traits[i] = pushTraits(timelines[i]);
        census.replace(0, traits[i]);
    }
    CHECK(census.count(TraitAnimating) == Count);

    Lcg      rng;
    uint64_t frame = 1;
    int64_t  tick  = 10;
    for (int step = 0; step < 4000; ++step) {
        auto const index = rng.next() % Count;
        mutate(timelines[index], rng, frame, tick);
        auto const after = pushTraits(timelines[index]);
        census.replace(traits[index], after);
        traits[index] = after;

        TraitCensus recount;
        for (auto const& timeline : timelines) recount.replace(0, pushTraits(timeline));
        REQUIRE(census == recount);
    }

    for (size_t i = 0; i < Count; ++i) census.replace(traits[i], 0);
    CHECK(census == TraitCensus{});
}

TEST_CASE("TraitCensus any() and clear()") {
    TraitCensus census;
    CHECK_FALSE(census.any(TraitSuppressed));
    census.replace(0, TraitSuppressed | TraitAnimating);
    CHECK(census.any(TraitSuppressed));
    CHECK(census.any(TraitAnimating));
    CHECK_FALSE(census.any(TraitAwaiting));
    census.replace(TraitSuppressed | TraitAnimating, TraitAwaiting);
    CHECK_FALSE(census.any(TraitSuppressed));
    CHECK(census.any(TraitAwaiting));
    census.clear();
    CHECK_FALSE(census.any(TraitAwaiting));
}

TEST_CASE("pushTraits describes the timeline state") {
    auto timeline = makeTimeline(true);
    CHECK(pushTraits(timeline) == TraitAnimating);
    timeline.blockLanded();
    CHECK(pushTraits(timeline) == (TraitAnimating | TraitSuppressed));
    REQUIRE(timeline.remeshRequested(11.0));
    CHECK(pushTraits(timeline) == (TraitAwaiting | TraitWantsActor));
    timeline.cleared();
    CHECK(pushTraits(timeline) == TraitDone);
}

TEST_CASE("drawer with a pending sighting equals applying the sighting first") {
    Lcg      rng;
    uint64_t frame    = 1;
    int64_t  tick     = 10;
    int      compared = 0;
    for (int run = 0; run < 300; ++run) {
        auto timeline = makeTimeline(run % 2 == 0);
        for (int step = 0; step < 40; ++step) {
            mutate(timeline, rng, frame, tick);
            for (uint64_t at = (frame > 2 ? frame - 2 : 1); at <= frame + 2; ++at) {
                // Sightings are monotonic: the pending one is never older than one already applied.
                for (uint64_t pending : {uint64_t{0}, frame, frame + 1}) {
                    auto applied = timeline;
                    if (pending != 0) applied.landedActorSeen(pending);
                    REQUIRE(timeline.drawer(at, pending) == applied.drawer(at));
                    ++compared;
                }
            }
        }
    }
    CHECK(compared > 0);
}
