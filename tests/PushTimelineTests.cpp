#include <doctest/doctest.h>

#include "core/PushTimeline.h"
#include "core/Segment.h"

#include <cmath>
#include <optional>

using namespace optipiston::core;

namespace {

// Drives one pushed cell frame by frame against a model of the chunk mesh and checks the single-drawer rule.
struct Scene {
    PushTimeline timeline;
    Segment      segment{};
    bool         carriesActor{};

    double   dt{0.25};     // visual ticks per frame
    double   latency{0.3}; // request to commit, in visual ticks
    double   time{};       // visual time of the current frame
    int64_t  tick{};
    uint64_t frame{1};

    bool                    landed{}; // the real block exists in the cell
    bool                    visible{true};
    std::optional<double>   commitAt; // when the pending build goes live
    std::optional<uint64_t> commitFrame;
    std::optional<uint64_t> actorFrom; // the landed cell's own actor draws from this frame on

    int zeroFrames{};
    int mismatches{};
    int proxyFrames{};
    int meshFrames{};

    Scene(int64_t startTick, float duration, bool actor) : carriesActor(actor) {
        segment = makeSegment(startTick, 0.0f, 1.0f, duration);
        timeline.start(1, segment, actor);
        time = static_cast<double>(startTick) - 1.0;
        tick = startTick;
    }

    [[nodiscard]] bool   meshShows() const { return commitFrame && frame > *commitFrame; }
    [[nodiscard]] double offset() const { return segmentValue(segment, time) - segment.to; }

    void requestRemesh(bool accepted) {
        if (accepted) commitAt = time + latency;
    }

    void land() {
        landed = true;
        timeline.blockLanded();
    }

    void check() {
        auto const drawer = timeline.drawer(frame);
        bool const proxy  = drawer == PushDrawer::Proxy;
        bool const mesh   = drawer == PushDrawer::Mesh;
        bool const game   = drawer == PushDrawer::Vanilla;
        if (visible && drawer == PushDrawer::None) ++zeroFrames;
        if (visible && !proxy && !mesh && !game) ++mismatches;
        // The mesh may only be the drawer once it really shows the block.
        if (mesh && !meshShows()) ++mismatches;
        // The game only draws a cell whose real block has not been suppressed.
        if (game && timeline.meshSuppressed()) ++mismatches;
        // Two drawers at once must agree on the position.
        if (proxy && meshShows() && std::abs(offset()) > 0.2) ++mismatches;
        // The suppressed mesh never shows the block.
        if (timeline.meshSuppressed() && meshShows()) ++mismatches;
        if (proxy) ++proxyFrames;
        if (mesh) ++meshFrames;
    }

    // One rendered frame; events are applied in the order the hooks run.
    void step() {
        time           += dt;
        auto const now  = static_cast<int64_t>(std::floor(time)) + 1;
        if (now != tick) {
            tick = now;
            requestRemesh(timeline.tick(tick, time));
        }
        if (timeline.shouldRequestRemesh(time, latency + dt)) requestRemesh(timeline.remeshRequested(time));
        if (commitAt && time >= *commitAt && timeline.awaitingMesh()) {
            timeline.meshCommitted(frame);
            commitFrame = frame;
            commitAt.reset();
        }
        if (actorFrom && frame >= *actorFrom) timeline.landedActorSeen(frame);
        check();
        ++frame;
        timeline.frameAdvanced(frame);
    }

    void run(int frames) {
        for (int i = 0; i < frames; ++i) step();
    }

    void runUntilTick(int64_t until) {
        while (tick < until) step();
    }
};

} // namespace

TEST_CASE("a plain push has one drawer per frame and ends on the mesh") {
    for (float duration : {2.0f, 3.0f, 4.0f}) {
        Scene scene(100, duration, false);
        scene.runUntilTick(101);
        scene.land();
        scene.runUntilTick(110);
        CHECK(scene.zeroFrames == 0);
        CHECK(scene.mismatches == 0);
        CHECK(scene.proxyFrames > 0);
        CHECK(scene.meshFrames > 0);
        CHECK(scene.timeline.state() == PushState::Done);
        CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Mesh);
    }
}

TEST_CASE("the proxy keeps drawing until the mesh with the block is on screen") {
    Scene scene(50, 4.0f, false);
    scene.latency = 1.6; // slow build
    scene.runUntilTick(51);
    scene.land();
    scene.runUntilTick(58);
    CHECK(scene.mismatches == 0);
    CHECK(scene.zeroFrames == 0);
    CHECK(scene.timeline.state() == PushState::Done);
}

TEST_CASE("the mesh is suppressed only between landing and the re-mesh request") {
    Scene scene(10, 4.0f, false);
    CHECK_FALSE(scene.timeline.meshSuppressed());
    scene.land();
    CHECK(scene.timeline.meshSuppressed());
    scene.runUntilTick(16);
    CHECK_FALSE(scene.timeline.meshSuppressed());
    CHECK(scene.timeline.state() != PushState::Animating);
}

TEST_CASE("a block that has not landed when the visual ends stays on the proxy for a while") {
    Scene scene(20, 2.0f, false);
    scene.runUntilTick(20 + 4 + 5);
    CHECK(scene.timeline.state() == PushState::Animating);
    CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Proxy);
    CHECK_FALSE(scene.timeline.meshSuppressed());
    CHECK(scene.offset() == doctest::Approx(0.0));
    CHECK(scene.mismatches == 0);
}

TEST_CASE("a block that lands during the overrun re-meshes and ends on the mesh") {
    for (bool actor : {false, true}) {
        Scene scene(20, 2.0f, actor);
        scene.runUntilTick(20 + 4 + 5);
        scene.land();
        CHECK(scene.timeline.meshSuppressed());
        scene.actorFrom = scene.frame + 8;
        scene.runUntilTick(20 + 4 + 5 + 10);
        CHECK(scene.timeline.state() == PushState::Done);
        CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Mesh);
        CHECK(scene.mismatches == 0);
        CHECK(scene.zeroFrames == 0);
    }
}

TEST_CASE("a block that never lands goes to the game after the overrun") {
    Scene scene(20, 2.0f, false);
    scene.runUntilTick(20 + 4 + DefaultOverrunTicks - 1);
    CHECK(scene.timeline.state() == PushState::Animating);
    scene.runUntilTick(20 + 4 + DefaultOverrunTicks + 1);
    CHECK(scene.timeline.state() == PushState::Done);
    CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Vanilla);
    CHECK_FALSE(scene.timeline.meshSuppressed());
    CHECK(scene.mismatches == 0);
}


TEST_CASE("only an expired, unlanded cell awaits its landing") {
    PushTimeline timeline;
    timeline.start(1, makeSegment(100, 0.0f, 1.0f, 4.0f), false);
    CHECK_FALSE(timeline.awaitsLanding(102));
    CHECK(timeline.awaitsLanding(106));
    timeline.blockLanded();
    CHECK_FALSE(timeline.awaitsLanding(106));
}

TEST_CASE("a clock that jumps back does not hold an unlanded cell") {
    PushTimeline timeline;
    timeline.start(1, makeSegment(500, 0.0f, 1.0f, 4.0f), false);
    CHECK_FALSE(timeline.tick(3, 2.0));
    CHECK(timeline.state() == PushState::Done);
}

TEST_CASE("a carried actor stays on the proxy until the landed actor is drawn") {
    Scene scene(100, 3.0f, true);
    scene.runUntilTick(101);
    scene.land();
    CHECK(scene.timeline.coversLandedActor());
    // No early re-mesh for a cell with an actor: the request only comes when the visual is over.
    scene.runUntilTick(103);
    CHECK(scene.timeline.state() == PushState::Animating);
    scene.actorFrom = scene.frame + 16;
    scene.runUntilTick(105);
    CHECK_FALSE(scene.timeline.coversLandedActor());
    scene.run(200);
    CHECK(scene.timeline.state() == PushState::Done);
    CHECK(scene.timeline.handoverEnd() == HandoverEnd::ActorSeen);
    CHECK(scene.zeroFrames == 0);
    CHECK(scene.mismatches == 0);
}

TEST_CASE("a carried actor that never shows is dropped by the deadline") {
    Scene scene(100, 4.0f, true);
    scene.runUntilTick(101);
    scene.land();
    scene.runUntilTick(125);
    CHECK(scene.timeline.state() == PushState::Done);
    CHECK(scene.timeline.handoverEnd() == HandoverEnd::Timeout);
    CHECK(scene.zeroFrames == 0);
    CHECK(scene.mismatches == 0);
}

TEST_CASE("a handover ends at once when the landed actor was already drawing") {
    Scene scene(100, 4.0f, true);
    scene.actorFrom = 1;
    scene.runUntilTick(101);
    scene.land();
    scene.runUntilTick(112);
    CHECK(scene.timeline.state() == PushState::Done);
    CHECK(scene.timeline.handoverEnd() == HandoverEnd::ActorSeen);
    CHECK(scene.mismatches == 0);
}

TEST_CASE("a landed block waits for its mesh for as long as the build takes") {
    for (bool actor : {false, true}) {
        Scene scene(100, 3.0f, actor);
        scene.latency = 1.0e6;
        scene.runUntilTick(101);
        scene.land();
        scene.runUntilTick(100 + 3 + 1000);
        CHECK(scene.timeline.state() == PushState::Landing);
        CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Proxy);
        scene.timeline.meshCommitted(scene.frame);
        scene.actorFrom = scene.frame + 1;
        scene.run(200);
        CHECK(scene.timeline.state() == PushState::Done);
        CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Mesh);
        CHECK(scene.zeroFrames == 0);
    }
}

TEST_CASE("a commit before the request does not count") {
    Scene scene(100, 4.0f, false);
    scene.runUntilTick(101);
    scene.land();
    // A build that started before the release goes live while the block is still suppressed.
    scene.timeline.meshCommitted(scene.frame);
    scene.run(4);
    CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Proxy);
    CHECK(scene.timeline.state() == PushState::Animating);
    scene.runUntilTick(112);
    CHECK(scene.timeline.state() == PushState::Done);
    CHECK(scene.mismatches == 0);
    CHECK(scene.zeroFrames == 0);
}

TEST_CASE("only the first commit after the request counts") {
    Scene scene(100, 4.0f, false);
    scene.latency = 100.0; // the scene never commits by itself
    scene.runUntilTick(101);
    scene.land();
    while (scene.timeline.state() == PushState::Animating) scene.step();
    REQUIRE(scene.timeline.state() == PushState::Landing);
    auto const now = scene.frame;
    scene.timeline.meshCommitted(now + 40);
    scene.timeline.meshCommitted(now + 1); // a second commit must not move the live frame earlier
    CHECK(scene.timeline.drawer(now + 1) == PushDrawer::Proxy);
    CHECK(scene.timeline.drawer(now + 40) == PushDrawer::Proxy);
    CHECK(scene.timeline.drawer(now + 41) == PushDrawer::Mesh);
}

TEST_CASE("a block picked up before landing goes to the newer cell") {
    Scene first(100, 4.0f, false);
    first.runUntilTick(101);
    first.timeline.supersededBy(2);
    first.visible = false; // the newer cell's proxy carries it on
    CHECK(first.timeline.drawer(first.frame) == PushDrawer::None);
    CHECK_FALSE(first.timeline.meshSuppressed());
    first.run(8);
    CHECK(first.timeline.drawer(first.frame) == PushDrawer::None);

    // The successor starts mid-flight and draws through to its own landing.
    Scene second(102, 4.0f, false);
    second.runUntilTick(104);
    second.land();
    second.runUntilTick(114);
    CHECK(second.zeroFrames == 0);
    CHECK(second.mismatches == 0);
    CHECK(second.timeline.state() == PushState::Done);
}

TEST_CASE("a landed block picked up again stops drawing from the old cell") {
    Scene scene(100, 4.0f, false);
    scene.runUntilTick(101);
    scene.land();
    scene.runUntilTick(104);
    scene.timeline.supersededBy(9);
    CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::None);
    CHECK_FALSE(scene.timeline.meshSuppressed());
    CHECK_FALSE(scene.timeline.awaitingMesh());
}

TEST_CASE("clearing mid-animation hands the cell back to the game") {
    Scene scene(100, 4.0f, true);
    scene.runUntilTick(101);
    scene.land();
    scene.run(3);
    scene.timeline.cleared();
    CHECK(scene.timeline.state() == PushState::Done);
    CHECK(scene.timeline.drawer(scene.frame) == PushDrawer::Vanilla);
    CHECK_FALSE(scene.timeline.meshSuppressed());
    CHECK_FALSE(scene.timeline.coversLandedActor());
}

TEST_CASE("an action cut short lands the block at once or gives it to the game") {
    Scene landed(100, 4.0f, false);
    landed.runUntilTick(101);
    landed.land();
    CHECK(landed.timeline.visualEnded(landed.time));
    CHECK(landed.timeline.state() == PushState::Landing);

    Scene flying(100, 4.0f, false);
    flying.runUntilTick(101);
    CHECK_FALSE(flying.timeline.visualEnded(flying.time));
    CHECK(flying.timeline.state() == PushState::Animating);
    CHECK(flying.timeline.drawer(flying.frame) == PushDrawer::Proxy);
}

TEST_CASE("early re-mesh only when the block landed, has no actor and little visual time is left") {
    PushTimeline plain;
    plain.start(1, makeSegment(100, 0.0f, 1.0f, 4.0f), false);
    CHECK_FALSE(plain.shouldRequestRemesh(103.9, 0.5));
    plain.blockLanded();
    CHECK_FALSE(plain.shouldRequestRemesh(102.0, 0.5));
    CHECK(plain.shouldRequestRemesh(103.6, 0.5));
    CHECK(plain.remeshRequested(103.6));
    CHECK_FALSE(plain.shouldRequestRemesh(103.9, 0.5));

    PushTimeline chest;
    chest.start(2, makeSegment(100, 0.0f, 1.0f, 4.0f), true);
    chest.blockLanded();
    CHECK_FALSE(chest.shouldRequestRemesh(103.9, 0.5));
    CHECK(chest.coversLandedActor());

    PushTimeline unlanded;
    unlanded.start(3, makeSegment(100, 0.0f, 1.0f, 4.0f), false);
    CHECK_FALSE(unlanded.remeshRequested(103.9));
}
