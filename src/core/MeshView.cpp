#include "core/MeshView.h"

#include <bit>

namespace optipiston::core {

MeshCell meshCellOf(PushTimeline const& timeline) noexcept {
    if (timeline.meshSuppressed()) return MeshCell::Landed;
    if (timeline.state() == PushState::Animating) return MeshCell::Approaching;
    if (timeline.awaitingMesh()) return MeshCell::Awaiting;
    return MeshCell::None;
}

void MeshView::set(CellPos const& pos, MeshCell kind) {
    if (kind == MeshCell::None) {
        erase(pos);
        return;
    }
    auto const [it, inserted] = mCells.try_emplace(pos, kind);
    if (!inserted) {
        if (it->second == MeshCell::Landed) --mLanded;
        it->second = kind;
    }
    if (kind == MeshCell::Landed) ++mLanded;
}

void MeshView::erase(CellPos const& pos) {
    auto const it = mCells.find(pos);
    if (it == mCells.end()) return;
    if (it->second == MeshCell::Landed) --mLanded;
    mCells.erase(it);
}

void MeshView::clear() noexcept {
    mCells.clear();
    mLanded = 0;
}

MeshCell MeshView::at(CellPos const& pos) const noexcept {
    if (mCells.empty()) return MeshCell::None;
    auto const it = mCells.find(pos);
    return it == mCells.end() ? MeshCell::None : it->second;
}

int MeshView::faceHidden(CellPos const& pos) const noexcept {
    switch (at(pos)) {
    case MeshCell::Landed:
        return 1;
    case MeshCell::Approaching:
        return 2;
    default:
        return 0;
    }
}

bool MeshView::touchesSuppressed(CellPos const& pos) const noexcept {
    if (mLanded == 0) return false;
    static constexpr CellPos dirs[6]{
        {0,  -1, 0 },
        {0,  1,  0 },
        {0,  0,  -1},
        {0,  0,  1 },
        {-1, 0,  0 },
        {1,  0,  0 }
    };
    for (auto const& dir : dirs)
        if (suppressed(CellPos{pos.x + dir.x, pos.y + dir.y, pos.z + dir.z})) return true;
    return false;
}

uint8_t pushTraits(PushTimeline const& timeline) noexcept {
    uint8_t traits = 0;
    if (timeline.state() == PushState::Animating) traits |= TraitAnimating;
    if (timeline.meshSuppressed()) traits |= TraitSuppressed;
    if (timeline.awaitingMesh()) traits |= TraitAwaiting;
    if (timeline.wantsActorSeen()) traits |= TraitWantsActor;
    if (timeline.state() == PushState::Done) traits |= TraitDone;
    return traits;
}

void TraitCensus::replace(uint8_t before, uint8_t after) noexcept {
    uint8_t const gone   = before & static_cast<uint8_t>(~after);
    uint8_t const gained = after & static_cast<uint8_t>(~before);
    for (int bit = 0; bit < PushTraitCount; ++bit) {
        auto const mask = static_cast<uint8_t>(1u << bit);
        if (gone & mask) --mCounts[static_cast<size_t>(bit)];
        if (gained & mask) ++mCounts[static_cast<size_t>(bit)];
    }
}

size_t TraitCensus::count(PushTrait trait) const noexcept {
    return mCounts[static_cast<size_t>(std::countr_zero(static_cast<unsigned>(trait)))];
}

} // namespace optipiston::core
