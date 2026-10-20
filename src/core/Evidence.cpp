#include "core/Evidence.h"

namespace optipiston::core {

CellPos subChunkOf(CellPos const& cell) noexcept { return CellPos{cell.x >> 4, cell.y >> 4, cell.z >> 4}; }

void EvidenceLedger::buildBegan(BuildKey build, CellPos const& subChunk, Seq beginSeq) {
    auto& entry    = mBuilds[build];
    entry.sub      = subChunk;
    entry.beginSeq = beginSeq;
    entry.cells.clear();
}

void EvidenceLedger::tessellated(BuildKey build, CellPos const& cell, Tess what) {
    auto const found = mBuilds.find(build);
    if (found == mBuilds.end()) return;
    auto& slot = found->second.cells[cell];
    if (static_cast<uint8_t>(what) > static_cast<uint8_t>(slot)) slot = what;
}

Seen EvidenceLedger::judge(BuildKey build, CellPos const& cell, Seq needSeq) const noexcept {
    auto const found = mBuilds.find(build);
    if (found == mBuilds.end() || found->second.beginSeq < needSeq) return Seen::Stale;
    auto const slot = found->second.cells.find(cell);
    if (slot == found->second.cells.end()) return Seen::Absent;
    switch (slot->second) {
    case Tess::Produced: return Seen::Present;
    case Tess::Suppressed: return Seen::Suppressed;
    default: return Seen::Absent;
    }
}

CellPos EvidenceLedger::subChunk(BuildKey build) const noexcept {
    auto const found = mBuilds.find(build);
    return found == mBuilds.end() ? CellPos{} : found->second.sub;
}

void EvidenceLedger::drop(BuildKey build) { mBuilds.erase(build); }

} // namespace optipiston::core
