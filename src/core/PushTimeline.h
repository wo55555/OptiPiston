#pragma once

#include "core/Segment.h"

#include <cstdint>

namespace optipiston::core {

enum class PushState : uint8_t {
    Animating, // the proxy draws the block and its actor at the visual offset
    Landing,   // re-mesh requested; the proxy keeps drawing until that mesh is on screen
    Handover,  // the mesh shows the block; the proxy only covers the carried actor
    Done       // nothing of ours draws
};

enum class PushDrawer : uint8_t {
    Proxy,
    Mesh,
    None,   // the block moved on to a newer cell
    Vanilla // the game draws the cell
};

enum class HandoverEnd : uint8_t { None, ActorSeen, Timeout };

inline constexpr uint64_t DefaultHandoverTicks = 4;
// How long a cell whose visual already ended keeps drawing at its end pose while the real block has not landed.
// A busy machine lands its blocks well over ten ticks late; the world poll ends the wait early when the cell is empty.
inline constexpr int64_t DefaultOverrunTicks = 100;

// One pushed cell. Pure state, so the single-drawer rule can be tested without the game.
class PushTimeline {
public:
    // carriesActor: the pushed block has its own block actor (chest, piston head...).
    void start(
        uint64_t       action,
        Segment const& visual,
        bool           carriesActor,
        uint64_t       handoverTicks = DefaultHandoverTicks
    ) noexcept;

    // The real block arrived in the cell; its chunk mesh stays suppressed until the re-mesh is requested.
    void blockLanded() noexcept;
    // Returns true if the request was accepted (the block has landed and the visual is still running).
    bool remeshRequested(double visualTime) noexcept;
    // A build that contains this cell, started after the re-mesh request, went live during `frame`.
    void meshCommitted(uint64_t frame) noexcept;
    void frameAdvanced(uint64_t frame) noexcept;
    void landedActorSeen(uint64_t frame) noexcept;
    // Returns true if the visual ended with the block landed, which requests the re-mesh at `visualTime`.
    [[nodiscard]] bool tick(int64_t tick, double visualTime) noexcept;
    // The visual ran out. A landed block requests its re-mesh (returns true). An unlanded one keeps the proxy at the
    // end pose until it lands or DefaultOverrunTicks pass, then goes to the game.
    [[nodiscard]] bool visualEnded(double visualTime) noexcept;
    // A newer action picked the block up from this cell.
    void supersededBy(uint64_t newAction) noexcept;
    void cleared() noexcept;

    // The visual ran out but the real block has not been seen in the cell.
    [[nodiscard]] bool awaitsLanding(int64_t tick) const noexcept {
        return mState == PushState::Animating && !mLanded && segmentExpired(mSegment, tick);
    }

    [[nodiscard]] PushState  state() const noexcept { return mState; }
    [[nodiscard]] PushDrawer drawer(uint64_t frame) const noexcept { return drawer(frame, 0); }
    // Same as drawer(frame) after landedActorSeen(pendingActorFrame); 0 means no sighting is pending.
    [[nodiscard]] PushDrawer drawer(uint64_t frame, uint64_t pendingActorFrame) const noexcept;
    // The landed cell's chunk mesh must not show the block yet.
    [[nodiscard]] bool meshSuppressed() const noexcept { return mState == PushState::Animating && mLanded; }
    // Re-mesh requested but no build containing the cell has gone live yet.
    [[nodiscard]] bool awaitingMesh() const noexcept { return mState == PushState::Landing && !mCommitted; }
    // `latency` is the expected time from request to the mesh being on screen, frame step included.
    [[nodiscard]] bool shouldRequestRemesh(double visualTime, double latency) const noexcept;
    // The landed cell's own actor would show up ahead of the carried copy.
    [[nodiscard]] bool coversLandedActor() const noexcept {
        return mState == PushState::Animating && mLanded && mCarriesActor;
    }
    [[nodiscard]] bool wantsActorSeen() const noexcept {
        return (mState == PushState::Landing || mState == PushState::Handover) && mCarriesActor;
    }

    [[nodiscard]] uint64_t    action() const noexcept { return mAction; }
    [[nodiscard]] bool        landed() const noexcept { return mLanded; }
    [[nodiscard]] bool        carriesActor() const noexcept { return mCarriesActor; }
    [[nodiscard]] bool        superseded() const noexcept { return mSuperseded; }
    [[nodiscard]] double      remeshTime() const noexcept { return mRemeshTime; }
    [[nodiscard]] HandoverEnd handoverEnd() const noexcept { return mHandoverEnd; }
    // Ticks between the end of the visual and the block landing; negative when it landed first.
    [[nodiscard]] int64_t landDelay() const noexcept { return mLandTick - segmentEndTick(mSegment); }

private:
    [[nodiscard]] bool meshLive(uint64_t frame) const noexcept { return mCommitted && frame > mCommitFrame; }
    [[nodiscard]] bool actorRecent(uint64_t frame) const noexcept { return mActorSeen && mActorFrame + 1 >= frame; }
    void               resolve() noexcept;
    void               finish(bool meshShown, HandoverEnd end) noexcept;

    PushState   mState{PushState::Done};
    uint64_t    mAction{};
    Segment     mSegment{};
    bool        mCarriesActor{};
    bool        mLanded{};
    bool        mCommitted{};
    bool        mActorSeen{};
    bool        mMeshShown{};
    bool        mSuperseded{};
    uint64_t    mCommitFrame{};
    uint64_t    mActorFrame{};
    uint64_t    mFrame{};
    int64_t     mTick{};
    int64_t     mDeadline{};
    int64_t     mOverrunDeadline{};
    int64_t     mLandTick{};
    uint64_t    mHandoverTicks{DefaultHandoverTicks};
    double      mRemeshTime{};
    HandoverEnd mHandoverEnd{HandoverEnd::None};
};

} // namespace optipiston::core
