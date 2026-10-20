#pragma once

#include "core/PushTimeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace optipiston::core {

struct CellPos {
    int32_t     x{};
    int32_t     y{};
    int32_t     z{};
    friend bool operator==(CellPos const&, CellPos const&) noexcept = default;
};

struct CellPosHash {
    [[nodiscard]] size_t operator()(CellPos const& pos) const noexcept {
        return (static_cast<size_t>(static_cast<uint32_t>(pos.x)) * 73856093u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.y)) * 19349663u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.z)) * 83492791u);
    }
};

// What the chunk mesher needs to know about one managed cell.
enum class MeshCell : uint8_t {
    None,
    Approaching, // the proxy is still gliding in; faces against the cell stay
    Landed,      // the real block arrived; the chunk mesh must not show it yet
    Awaiting,    // re-mesh requested; a build that tessellates the block is wanted
    Managed      // tracked, but the mesher has nothing to do (diagnostics only)
};

[[nodiscard]] MeshCell meshCellOf(PushTimeline const& timeline) noexcept;

// Immutable once published; mesh threads answer their per-block and per-face questions from it without locking.
class MeshView {
public:
    void set(CellPos const& pos, MeshCell kind);
    void erase(CellPos const& pos);
    void clear() noexcept;

    [[nodiscard]] MeshCell at(CellPos const& pos) const noexcept;
    [[nodiscard]] bool     suppressed(CellPos const& pos) const noexcept { return at(pos) == MeshCell::Landed; }
    [[nodiscard]] bool     awaiting(CellPos const& pos) const noexcept { return at(pos) == MeshCell::Awaiting; }
    [[nodiscard]] bool     managed(CellPos const& pos) const noexcept { return at(pos) != MeshCell::None; }
    // 0 shown, 1 landed, 2 still approaching.
    [[nodiscard]] int faceHidden(CellPos const& pos) const noexcept;
    // Any of the six neighbours has its mesh suppressed.
    [[nodiscard]] bool touchesSuppressed(CellPos const& pos) const noexcept;

    [[nodiscard]] size_t size() const noexcept { return mCells.size(); }
    [[nodiscard]] bool   empty() const noexcept { return mCells.empty(); }

    friend bool operator==(MeshView const& a, MeshView const& b) noexcept { return a.mCells == b.mCells; }

private:
    std::unordered_map<CellPos, MeshCell, CellPosHash> mCells;
    size_t                                             mLanded{};
};

enum PushTrait : uint8_t {
    TraitAnimating  = 1 << 0, // the proxy is gliding in
    TraitSuppressed = 1 << 1, // landed cell whose chunk mesh must stay empty
    TraitAwaiting   = 1 << 2, // re-mesh requested, no build containing the cell live yet
    TraitWantsActor = 1 << 3, // waiting for the landed cell's own actor
    TraitDone       = 1 << 4  // nothing of ours draws
};

inline constexpr int PushTraitCount = 5;

[[nodiscard]] uint8_t pushTraits(PushTimeline const& timeline) noexcept;

// Counts cells per trait, kept current by moving one cell at a time instead of rescanning the table.
class TraitCensus {
public:
    void replace(uint8_t before, uint8_t after) noexcept;
    void clear() noexcept { mCounts = {}; }

    [[nodiscard]] size_t count(PushTrait trait) const noexcept;
    [[nodiscard]] bool   any(PushTrait trait) const noexcept { return count(trait) != 0; }

    friend bool operator==(TraitCensus const&, TraitCensus const&) noexcept = default;

private:
    std::array<size_t, PushTraitCount> mCounts{};
};

} // namespace optipiston::core
