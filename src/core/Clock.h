#pragma once

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
    mutable std::mutex   mMutex;
    bool                 mActive{};
    int64_t              mTick{};
    uint64_t             mEpoch{};
    std::optional<float> mPartial;
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
