#include "core/PushTimeline.h"

namespace optipiston::core {

void PushTimeline::start(uint64_t action, Segment const& visual, bool carriesActor, uint64_t handoverTicks) noexcept {
    *this            = PushTimeline{};
    mState           = PushState::Animating;
    mAction          = action;
    mSegment         = visual;
    mCarriesActor    = carriesActor;
    mHandoverTicks   = handoverTicks;
    mTick            = visual.startTick;
    mOverrunDeadline = segmentEndTick(visual) + DefaultOverrunTicks;
}

void PushTimeline::blockLanded() noexcept {
    if (mState != PushState::Animating || mLanded) return;
    mLanded   = true;
    mLandTick = mTick;
}

bool PushTimeline::remeshRequested(double visualTime) noexcept {
    if (mState != PushState::Animating || !mLanded) return false;
    mState      = PushState::Landing;
    mRemeshTime = visualTime;
    return true;
}

void PushTimeline::meshCommitted(uint64_t frame) noexcept {
    // A build that went live before the request cannot contain the released block.
    if (mState != PushState::Landing || mCommitted) return;
    mCommitted   = true;
    mCommitFrame = frame;
}

void PushTimeline::frameAdvanced(uint64_t frame) noexcept {
    mFrame = frame;
    resolve();
}

void PushTimeline::landedActorSeen(uint64_t frame) noexcept {
    if (mState != PushState::Landing && mState != PushState::Handover) return;
    mActorSeen  = true;
    mActorFrame = frame;
}

bool PushTimeline::tick(int64_t tick, double visualTime) noexcept {
    mTick = tick;
    if (mState == PushState::Animating && segmentExpired(mSegment, tick)) return visualEnded(visualTime);
    resolve();
    return false;
}

bool PushTimeline::visualEnded(double visualTime) noexcept {
    if (mState != PushState::Animating) return false;
    if (!mLanded) {
        // The real block is not there yet, so the game would draw an empty cell or the vanilla moving block.
        // Hold the end pose; blockLanded() then goes through the normal re-mesh on the next tick.
        // A clock that jumped back (segmentExpired is also true then) must not hold the cell for ages.
        if (mTick >= mOverrunDeadline || mTick < mSegment.startTick) finish(false, HandoverEnd::None);
        return false;
    }
    return remeshRequested(visualTime);
}

void PushTimeline::supersededBy(uint64_t) noexcept {
    mState      = PushState::Done;
    mSuperseded = true;
    mMeshShown  = false;
}

void PushTimeline::cleared() noexcept {
    mState      = PushState::Done;
    mSuperseded = false;
    mMeshShown  = false;
}


PushDrawer PushTimeline::drawer(uint64_t frame, uint64_t pendingActorFrame) const noexcept {
    bool const sighted =
        mActorSeen || (pendingActorFrame != 0 && (mState == PushState::Landing || mState == PushState::Handover));
    auto const actorAt = pendingActorFrame != 0 && pendingActorFrame > mActorFrame ? pendingActorFrame : mActorFrame;
    bool const recent  = sighted && actorAt + 1 >= frame;
    switch (mState) {
    case PushState::Animating:
        return PushDrawer::Proxy;
    case PushState::Landing:
        if (!meshLive(frame)) return PushDrawer::Proxy;
        return mCarriesActor && !recent ? PushDrawer::Proxy : PushDrawer::Mesh;
    case PushState::Handover:
        return recent || mTick >= mDeadline ? PushDrawer::Mesh : PushDrawer::Proxy;
    case PushState::Done:
        break;
    }
    if (mSuperseded) return PushDrawer::None;
    return mMeshShown ? PushDrawer::Mesh : PushDrawer::Vanilla;
}

bool PushTimeline::shouldRequestRemesh(double visualTime, double latency) const noexcept {
    if (mState != PushState::Animating || !mLanded || mCarriesActor) return false;
    return static_cast<double>(mSegment.startTick) + mSegment.length - visualTime <= latency;
}

void PushTimeline::resolve() noexcept {
    if (mState == PushState::Landing && meshLive(mFrame)) {
        if (!mCarriesActor) {
            finish(true, HandoverEnd::None);
            return;
        }
        mState    = PushState::Handover;
        mDeadline = mTick + static_cast<int64_t>(mHandoverTicks);
    }
    if (mState != PushState::Handover) return;
    if (actorRecent(mFrame)) finish(true, HandoverEnd::ActorSeen);
    else if (mTick >= mDeadline) finish(true, HandoverEnd::Timeout);
}

void PushTimeline::finish(bool meshShown, HandoverEnd end) noexcept {
    mState       = PushState::Done;
    mMeshShown   = meshShown;
    mHandoverEnd = end;
}

} // namespace optipiston::core
