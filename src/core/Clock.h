#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace optipiston::core {

// Tick source for piston visuals at one instant.
struct ClockSample {
    int64_t  tick{};
    uint64_t epoch{};
    bool     external{};
    // Frame fraction from the clock owner; replaces the native render alpha when set.
    std::optional<float> partial;
};

// Clock pushed by another mod (e.g. a replay); overrides the local level tick while set.
class ExternalClock {
public:
    void set(int64_t tick, uint64_t epoch) noexcept;
    // Empty or out of [0, 1] falls back to the native render alpha.
    void                                     setPartial(std::optional<float> partial) noexcept;
    void                                     clear() noexcept;
    [[nodiscard]] std::optional<ClockSample> sample() const noexcept;

private:
    // Seqlock: render hooks read this many times per frame, so readers never take the writers' mutex.
    void beginWrite() noexcept;
    void endWrite() noexcept;

    std::mutex            mWriteMutex;
    std::atomic<uint64_t> mSeq{0};
    std::atomic<bool>     mActive{};
    std::atomic<int64_t>  mTick{};
    std::atomic<uint64_t> mEpoch{};
    std::atomic<float>    mPartial{-1.0f}; // negative: none
};

[[nodiscard]] ExternalClock& externalClock() noexcept;

// Detects when timings recorded on the previous clock no longer mean anything.
class ClockTracker {
public:
    // True if the source or epoch changed, or the tick went backwards since the last call.
    [[nodiscard]] bool invalidated(ClockSample const& sample) noexcept;
    void               reset() noexcept;
    // Returns false if this tick was already seen.
    [[nodiscard]] bool advance(int64_t tick) noexcept;

private:
    std::optional<ClockSample> mLast;
    std::optional<int64_t>     mLastSweep;
};

} // namespace optipiston::core
