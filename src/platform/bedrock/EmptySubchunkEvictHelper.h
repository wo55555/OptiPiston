#pragma once

#if OPTIPISTON_MC >= 2620

#include "mc/client/renderer/chunks/RenderChunkBuilder.h"
#include "mc/client/renderer/chunks/RenderChunkGeometry.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#if OPTIPISTON_MC >= 2651
#include "mc/client/renderer/chunks/RenderChunkInstanced.h"
#include "mc/client/renderer/chunks/RenderChunkShared.h"
#endif
#if OPTIPISTON_MC != 2651
#include "ll/api/memory/Signature.h"
#endif

#include <cstddef>
#include <cstdint>
#include <variant>

namespace optipiston::subchunk_detail {

// Samples builder.mBuiltMesh before RenderChunkGeometry::endRebuild consumes it.
[[nodiscard]] inline bool isBuiltMeshEmptyOrInvalid(::RenderChunkBuilder const& builder, bool isBuilding) {
    if (!isBuilding) {
        return false;
    }
    ::mce::Mesh const* mesh = builder.mBuiltMesh.get();
    return mesh == nullptr || !mesh->isValid();
}

[[nodiscard]] inline bool isGeometryEmpty(::RenderChunkGeometry const& geometry) {
    return std::holds_alternative<std::monostate>(geometry.mMeshData.get());
}

[[nodiscard]] inline bool hasRenderableMesh(::RenderChunkGeometry const& geometry) {
    return !isGeometryEmpty(geometry)
        && geometry.mFlags.get().test(static_cast<std::size_t>(::RenderChunkGeometry::Bit::CanRender));
}

// Resets geometry flags and bounds when a rebuild finishes with an empty or invalid mesh.
inline bool finalizeEmptyRebuildIfNeeded(
    ::RenderChunkGeometry& geometry,
    bool                   isBuilding,
    bool                   preSampledEmptyOrInvalid
) {
    if (!isBuilding || (!preSampledEmptyOrInvalid && !isGeometryEmpty(geometry))) {
        return false;
    }
    geometry.reset();
    return true;
}

#if OPTIPISTON_MC != 2651
inline void clearInstancedMeshDataBySig(void* instanced) {
    using ll::memory_literals::operator""_sig;
    using ClearMeshDataFn = void (*)(void*);
    static ClearMeshDataFn const fn = reinterpret_cast<ClearMeshDataFn>(
        "56 48 83 EC 20 48 89 CE 48 8B 41 10 48 85 C0 74 0E 48 8B 48 28 48 85 C9"_sig.resolve(true)
    );
    if (fn != nullptr && instanced != nullptr) {
        fn(instanced);
    }
}
#endif

#if OPTIPISTON_MC >= 2651
inline void clearInstancedMeshData(::RenderChunkInstanced& instanced) {
#if OPTIPISTON_MC == 2660
    clearInstancedMeshDataBySig(&instanced);
#else
    instanced.clearMeshData();
#endif
}

// Clears stale instanced mesh data when the shared chunk geometry is missing, empty, or unrenderable.
inline bool evictStaleInstancedIfSharedEmpty(::RenderChunkInstanced& instanced) {
    auto const* shared = instanced.mRenderChunkShared.get();
    if (shared == nullptr) {
        return false;
    }
    auto const* instancedCurrent = instanced.mCurrentRenderChunkGeometry.get();
    if (instancedCurrent == nullptr) {
        return false;
    }
    auto const* sharedCurrent = shared->mCurrentRenderChunkGeometry.get();
    if (sharedCurrent == nullptr) {
        clearInstancedMeshData(instanced);
        return true;
    }
    if (!hasRenderableMesh(*sharedCurrent)) {
        if (instancedCurrent != sharedCurrent || !isGeometryEmpty(*instancedCurrent)) {
            clearInstancedMeshData(instanced);
            return true;
        }
    }
    return false;
}
#else
// Clears stale instanced mesh data when the shared chunk geometry is missing, empty, or unrenderable.
inline bool evictStaleInstancedIfSharedEmpty(void* instanced) {
    if (instanced == nullptr) {
        return false;
    }
    auto const* const     bytes           = static_cast<std::uint8_t const*>(instanced);
    constexpr std::size_t kInstGeomOffset = 0x20;
    auto const* const     shared          = *reinterpret_cast<void* const*>(bytes + 0x10);
    auto const* const     instGeom = *reinterpret_cast<::RenderChunkGeometry const* const*>(bytes + kInstGeomOffset);
    if (instGeom == nullptr) {
        return false;
    }
    auto const* const sharedGeom =
        shared != nullptr
            ? *reinterpret_cast<::RenderChunkGeometry const* const*>(static_cast<std::uint8_t const*>(shared) + 0x28)
            : nullptr;
    if (sharedGeom == nullptr || !hasRenderableMesh(*sharedGeom)) {
        if (instGeom != sharedGeom || !isGeometryEmpty(*instGeom)) {
            clearInstancedMeshDataBySig(instanced);
            return true;
        }
    }
    return false;
}
#endif

} // namespace optipiston::subchunk_detail

#endif
