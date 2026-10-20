#pragma once

#include "core/MeshView.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace optipiston::core {

// Monotonic counter of world changes; a change is counted after its chunk write.
using Seq = uint64_t;
using BuildKey = uint64_t;

// What one mesh build did with one cell.
enum class Tess : uint8_t { Skipped, Produced, Suppressed };

// What a promoted build proves about one cell, relative to the change that must be visible in it.
enum class Seen : uint8_t {
    Stale,     // unknown build, or it began before the change: proves nothing
    Absent,    // began after the change, the cell produced no geometry
    Present,   // began after the change, the cell produced geometry
    Suppressed // began after the change, the mod kept the cell out of the mesh
};

// Sub-chunk index of a cell.
[[nodiscard]] CellPos subChunkOf(CellPos const& cell) noexcept;

// Which cells each mesh build could have seen. The platform feeds only cells that are being tracked.
class EvidenceLedger {
public:
    // A reused key starts a fresh record.
    void buildBegan(BuildKey build, CellPos const& subChunk, Seq beginSeq);
    // Calls for one cell merge: Suppressed beats Produced beats Skipped.
    void tessellated(BuildKey build, CellPos const& cell, Tess what);

    // A build that began at or after `needSeq` read every change up to needSeq, whichever way it reads the chunk.
    [[nodiscard]] Seen judge(BuildKey build, CellPos const& cell, Seq needSeq) const noexcept;
    [[nodiscard]] bool known(BuildKey build) const noexcept { return mBuilds.contains(build); }
    // Sub-chunk of a known build; (0,0,0) otherwise.
    [[nodiscard]] CellPos subChunk(BuildKey build) const noexcept;

    void   drop(BuildKey build);
    void   clear() noexcept { mBuilds.clear(); }
    [[nodiscard]] size_t size() const noexcept { return mBuilds.size(); }

private:
    struct Build {
        CellPos                                        sub{};
        Seq                                            beginSeq{};
        std::unordered_map<CellPos, Tess, CellPosHash> cells;
    };
    std::unordered_map<BuildKey, Build> mBuilds;
};

} // namespace optipiston::core
