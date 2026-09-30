#include "core/Clock.h"

namespace optipiston::core {

ExternalClock& externalClock() noexcept {
    static ExternalClock clock;
    return clock;
}

// Caller holds mWriteMutex.
void ExternalClock::beginWrite() noexcept {
    mSeq.store(mSeq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void ExternalClock::endWrite() noexcept {
    mSeq.store(mSeq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void ExternalClock::set(int64_t tick, uint64_t epoch) noexcept {
    std::lock_guard const guard(mWriteMutex);
    beginWrite();
    mActive.store(true, std::memory_order_relaxed);
    mTick.store(tick, std::memory_order_relaxed);
    mEpoch.store(epoch, std::memory_order_relaxed);
    endWrite();
}

void ExternalClock::setPartial(std::optional<float> partial) noexcept {
    if (partial && !(*partial >= 0.0f && *partial <= 1.0f)) partial.reset();
    std::lock_guard const guard(mWriteMutex);
    beginWrite();
    mPartial.store(partial.value_or(-1.0f), std::memory_order_relaxed);
    endWrite();
}

void ExternalClock::clear() noexcept {
    std::lock_guard const guard(mWriteMutex);
    beginWrite();
    mActive.store(false, std::memory_order_relaxed);
    mPartial.store(-1.0f, std::memory_order_relaxed);
    endWrite();
}

std::optional<ClockSample> ExternalClock::sample() const noexcept {
    for (;;) {
        auto const before = mSeq.load(std::memory_order_acquire);
        if (before & 1) continue;
        bool const  active  = mActive.load(std::memory_order_relaxed);
        auto const  tick    = mTick.load(std::memory_order_relaxed);
        auto const  epoch   = mEpoch.load(std::memory_order_relaxed);
        float const partial = mPartial.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (mSeq.load(std::memory_order_relaxed) != before) continue;
        if (!active) return std::nullopt;
        return ClockSample{tick, epoch, true, partial >= 0.0f ? std::optional<float>{partial} : std::nullopt};
    }
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
