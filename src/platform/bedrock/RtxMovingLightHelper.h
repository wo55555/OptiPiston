#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace optipiston::rtx_detail {

// -----------------------------------------------------------------------------
// Reference-counted render resource handle (matches Dragon/RTX resource ptr)
// -----------------------------------------------------------------------------

#if defined(OPTIPISTON_MC) && OPTIPISTON_MC == 2660
struct RtResourceRef {
    void* ptr{};
    void* ctrl{};
};
static_assert(sizeof(RtResourceRef) == 16);
#else
struct RtResourceRef {
    void const* vtable{};
    void*       ptr{};
    void*       ctrl{};
};
static_assert(sizeof(RtResourceRef) == 24);
#endif

[[nodiscard]] inline RtResourceRef makeEmptyRtResourceRef(void const* vtable) noexcept {
#if defined(OPTIPISTON_MC) && OPTIPISTON_MC == 2660
    (void)vtable;
    return {};
#else
    return {vtable, nullptr, nullptr};
#endif
}

[[nodiscard]] inline RtResourceRef makeRtResourceRef(void const* vtable, void* ptr, void* ctrl) noexcept {
#if defined(OPTIPISTON_MC) && OPTIPISTON_MC == 2660
    (void)vtable;
    return {ptr, ctrl};
#else
    return {vtable, ptr, ctrl};
#endif
}

inline void retainCtrl(void* ctrl) noexcept {
    if (!ctrl) return;
    std::atomic_ref<std::int32_t>(*reinterpret_cast<std::int32_t*>(static_cast<char*>(ctrl) + 8))
        .fetch_add(1, std::memory_order_relaxed);
}

inline void releaseCtrl(void* ctrl) noexcept {
    if (!ctrl) return;
    auto* const bytes = static_cast<char*>(ctrl);
    if (std::atomic_ref<std::int32_t>(*reinterpret_cast<std::int32_t*>(bytes + 8))
            .fetch_sub(1, std::memory_order_acq_rel)
        == 1) {
        using CtrlFn = void (*)(void*);
        (*reinterpret_cast<CtrlFn const* const*>(ctrl))[0](ctrl);
        if (std::atomic_ref<std::int32_t>(*reinterpret_cast<std::int32_t*>(bytes + 12))
                .fetch_sub(1, std::memory_order_acq_rel)
            == 1) {
            (*reinterpret_cast<CtrlFn const* const*>(ctrl))[1](ctrl);
        }
    }
}

inline void retainRef(RtResourceRef const& ref) noexcept { retainCtrl(ref.ctrl); }

inline void releaseRef(RtResourceRef& ref) noexcept {
    releaseCtrl(ref.ctrl);
    ref.ptr  = nullptr;
    ref.ctrl = nullptr;
}

// -----------------------------------------------------------------------------
// Two-texture ray-trace object payload
// -----------------------------------------------------------------------------

struct alignas(8) RtObjectPayload {
    RtResourceRef bvh{};
    std::uint8_t  geomType{};
    std::uint64_t motionKey{};
    std::uint16_t pbrFlags{};
    RtResourceRef colorTex{};
    RtResourceRef pbrTex{};
    RtResourceRef tex3{};
    std::int32_t  param9{};
    std::int32_t  param10{};
};

#if !defined(OPTIPISTON_MC) || OPTIPISTON_MC != 2660
static_assert(sizeof(RtObjectPayload) == 128);
static_assert(offsetof(RtObjectPayload, bvh) == 0);
static_assert(offsetof(RtObjectPayload, geomType) == 24);
static_assert(offsetof(RtObjectPayload, motionKey) == 32);
static_assert(offsetof(RtObjectPayload, pbrFlags) == 40);
static_assert(offsetof(RtObjectPayload, colorTex) == 48);
static_assert(offsetof(RtObjectPayload, pbrTex) == 72);
static_assert(offsetof(RtObjectPayload, tex3) == 96);
static_assert(offsetof(RtObjectPayload, param9) == 120);
static_assert(offsetof(RtObjectPayload, param10) == 124);
#endif

inline void releasePayload(RtObjectPayload& payload) noexcept {
    releaseRef(payload.tex3);
    releaseRef(payload.pbrTex);
    releaseRef(payload.colorTex);
    releaseRef(payload.bvh);
}

using BuildRtPayload2TexFn = void* (*)(
    void*         outPayload,
    void const*   bvhRes,
    std::uint8_t  geomType,
    std::uint64_t motionKey,
    void const*   colorTex,
    std::uint16_t pbrFlags,
    void const*   pbrTex,
    void const*   tex3,
    std::int32_t  param9,
    std::int32_t  param10
);

// Attaches a built ray-trace payload to a scene object's ray-trace slot.
using AttachRtPayloadFn = void* (*)(void* sceneObjRtSlot, void const* payload);

[[nodiscard]] constexpr std::int32_t packUniformMers(
    std::uint8_t metalness,
    std::uint8_t emissive,
    std::uint8_t roughness,
    std::uint8_t subsurface
) noexcept {
    auto const packed = (static_cast<std::uint32_t>(metalness) << 24)
                      | (static_cast<std::uint32_t>(emissive) << 16)
                      | (static_cast<std::uint32_t>(roughness) << 8)
                      | static_cast<std::uint32_t>(subsurface);
    return static_cast<std::int32_t>(packed);
}

inline void* submitTwoTexRtObject(
    BuildRtPayload2TexFn buildFn,
    AttachRtPayloadFn    attachFn,
    void const*          resVtable,
    void*                src,
    std::size_t          sceneObjRtSlotOffset,
    RtResourceRef const& bvh,
    std::uint8_t         geomType,
    std::uint64_t        motionKey,
    RtResourceRef const& colorTex,
    std::uint16_t        pbrFlags,
    RtResourceRef const& pbrTex,
    RtResourceRef const& tex3,
    std::int32_t         param9  = -256,
    std::int32_t         param10 = -256
) noexcept {
    if (!buildFn || !attachFn || !src || sceneObjRtSlotOffset == 0) return nullptr;

    auto normalizeVtable = [resVtable](RtResourceRef ref) noexcept {
#if !defined(OPTIPISTON_MC) || OPTIPISTON_MC != 2660
        if (!ref.vtable) ref.vtable = resVtable;
#else
        (void)resVtable;
#endif
        return ref;
    };

    auto const bvhArg   = normalizeVtable(bvh);
    auto const colorArg = normalizeVtable(colorTex);
    auto const pbrArg   = normalizeVtable(pbrTex);
    auto const tex3Arg  = normalizeVtable(tex3);

    RtObjectPayload payload{};
    buildFn(
        &payload,
        &bvhArg,
        geomType,
        motionKey,
        &colorArg,
        pbrFlags,
        &pbrArg,
        &tex3Arg,
        param9,
        param10
    );
    void* const result = attachFn(static_cast<std::uint8_t*>(src) + sceneObjRtSlotOffset, &payload);
    releasePayload(payload);
    return result;
}

// -----------------------------------------------------------------------------
// Live terrain texture atlas cache
// -----------------------------------------------------------------------------

class TerrainAtlasCache {
public:
    TerrainAtlasCache()                                    = default;
    ~TerrainAtlasCache() { clear(); }
    TerrainAtlasCache(TerrainAtlasCache const&)            = delete;
    TerrainAtlasCache& operator=(TerrainAtlasCache const&) = delete;
    TerrainAtlasCache(TerrainAtlasCache&&)                 = delete;
    TerrainAtlasCache& operator=(TerrainAtlasCache&&)      = delete;

    void update(void const* vtable, void* ptr, void* ctrl) noexcept {
        if (!ptr) return;
        void* oldCtrl = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (ptr == ptr_) return;
            // Retain the incoming control block before releasing the previous one.
            retainCtrl(ctrl);
            oldCtrl = ctrl_;
            ptr_    = ptr;
            ctrl_   = ctrl;
            if (vtable) vtable_ = vtable;
        }
        releaseCtrl(oldCtrl);
    }

    [[nodiscard]] RtResourceRef acquireCopy(void const* vtable) const noexcept {
        std::lock_guard   lock(mutex_);
        void const* const effectiveVtable = vtable ? vtable : vtable_;
        if (!ptr_) return makeEmptyRtResourceRef(effectiveVtable);
        retainCtrl(ctrl_);
        return makeRtResourceRef(effectiveVtable, ptr_, ctrl_);
    }

    void clear() noexcept {
        void* oldCtrl = nullptr;
        {
            std::lock_guard lock(mutex_);
            oldCtrl = ctrl_;
            ptr_    = nullptr;
            ctrl_   = nullptr;
            vtable_ = nullptr;
        }
        releaseCtrl(oldCtrl);
    }

private:
    mutable std::mutex mutex_;
    void const*        vtable_{nullptr};
    void*              ptr_{nullptr};
    void*              ctrl_{nullptr};
};

} // namespace optipiston::rtx_detail
