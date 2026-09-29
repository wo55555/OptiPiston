#pragma once

// Header-only C++ helper over api.h; safe to include without linking OptiPiston.

#include "optipiston/api.h"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace optipiston {

class Api {
public:
    // Null if OptiPiston is not loaded or too old.
    [[nodiscard]] static OptiPistonApiV1 const* find(uint32_t minVersion = OPTIPISTON_API_VERSION) noexcept {
        auto* module = GetModuleHandleW(L"OptiPiston.dll");
        if (!module) return nullptr;
        auto* proc = GetProcAddress(module, OPTIPISTON_API_EXPORT_NAME);
        if (!proc) return nullptr;
        auto const  getApi = reinterpret_cast<OptiPistonGetApiFn>(reinterpret_cast<void*>(proc));
        auto const* api    = getApi(minVersion);
        if (!api || api->size < sizeof(OptiPistonApiV1)) return nullptr;
        return api;
    }
};

// Holds the external clock while alive.
class ExternalClockLease {
public:
    ExternalClockLease() noexcept = default;
    explicit ExternalClockLease(OptiPistonApiV1 const* api) noexcept : mApi(api) {}
    ~ExternalClockLease() { release(); }
    ExternalClockLease(ExternalClockLease const&)            = delete;
    ExternalClockLease& operator=(ExternalClockLease const&) = delete;
    ExternalClockLease(ExternalClockLease&& other) noexcept : mApi(other.mApi), mHeld(other.mHeld) {
        other.mHeld = false;
    }
    ExternalClockLease& operator=(ExternalClockLease&& other) noexcept {
        if (this != &other) {
            release();
            mApi        = other.mApi;
            mHeld       = other.mHeld;
            other.mHeld = false;
        }
        return *this;
    }

    void push(int64_t tick, uint64_t epoch) noexcept {
        if (!mApi) return;
        mApi->set_external_clock(tick, epoch);
        mHeld = true;
    }

    void release() noexcept {
        if (mApi && mHeld) mApi->clear_external_clock();
        mHeld = false;
    }

    [[nodiscard]] bool valid() const noexcept { return mApi != nullptr; }

private:
    OptiPistonApiV1 const* mApi{};
    bool                   mHeld{};
};

} // namespace optipiston
