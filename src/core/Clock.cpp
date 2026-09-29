#include "core/Clock.h"

namespace optipiston::core {

ExternalClock& externalClock() noexcept {
    static ExternalClock clock;
    return clock;
}

void ExternalClock::set(int64_t tick, uint64_t epoch) noexcept {
    std::lock_guard const guard(mMutex);
    mActive = true;
    mTick   = tick;
    mEpoch  = epoch;
}

void ExternalClock::setPartial(std::optional<float> partial) noexcept {
    if (partial && !(*partial >= 0.0f && *partial <= 1.0f)) partial.reset();
    std::lock_guard const guard(mMutex);
    mPartial = partial;
}

void ExternalClock::clear() noexcept {
    std::lock_guard const guard(mMutex);
    mActive = false;
    mPartial.reset();
}

std::optional<ClockSample> ExternalClock::sample() const noexcept {
    std::lock_guard const guard(mMutex);
    if (!mActive) return std::nullopt;
    return ClockSample{mTick, mEpoch, true, mPartial};
}

bool ClockTracker::invalidated(ClockSample const& sample) noexcept {
    bool const stale =
        mLast && (mLast->external != sample.external || mLast->epoch != sample.epoch || sample.tick < mLast->tick);
    mLast = sample;
    if (stale) mLastSweep.reset();
    return stale;
}

void ClockTracker::reset() noexcept {
    mLast.reset();
    mLastSweep.reset();
}

bool ClockTracker::advance(int64_t tick) noexcept {
    if (mLastSweep == tick) return false;
    mLastSweep = tick;
    return true;
}

} // namespace optipiston::core
