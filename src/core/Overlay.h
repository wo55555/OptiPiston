#pragma once

#include "core/Evidence.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace optipiston::core {

using ObjectId = uint64_t;

// What the world says about one cell.
enum class WorldCell : uint8_t { Air, Moving, Real };

// One pushed block, from the cell it left to the cell it lands in.
enum class Phase : uint8_t {
    Gated,     // the old mesh may still show the block; the proxy waits where it was
    Animating, // the proxy glides; the destination cell stays out of the mesh
    Holding,   // the proxy covers the destination until a build that began after the landing is on screen
    Done       // nothing of ours draws
};

enum class Pose : uint8_t { None, Source, Moving, Dest };

enum class EndReason : uint8_t {
    None,
    Mesh,       // a build that began after the landing shows the block
    Culled,     // such a build produced nothing for the cell, so nothing is left to cover
    Superseded, // a newer object took the cell
    Abandoned   // the platform gave up on the object
};

// Decides, per pushed block, who draws it. No clocks: every release waits for evidence from a mesh build.
//   Gate: the block left a cell the old mesh may still show. The proxy waits there, and builds keep the cell out of the
//         mesh, until a build that began after the block left is live. Then the proxy glides.
//   Hold: cover the landing cell until a build that began after the landing (and after the mod stopped keeping the
//         cell out of the mesh) is live. An empty destination is not an end: the block lands later.
// Single writer. The platform feeds events in order and applies the answers.
class Overlay {
public:
    // dstLandSeq is the change that made the destination real, or 0 while it has not landed.
    void start(ObjectId id, CellPos const& src, CellPos const& dst, Seq srcRelease, Seq dstLandSeq, Seq now);
    // The visual ran out its last tick.
    void visualEnded(ObjectId id, Seq now);
    // The platform gives up: the actor vanished, the chunk unloaded, the dimension changed.
    void abandon(ObjectId id, Seq now);
    void worldChanged(CellPos const& cell, WorldCell kind, Seq seq);
    // A build went live. The ledger must still hold it.
    void promoted(BuildKey build, EvidenceLedger const& ledger, Seq now);
    void clear();
    // Removes finished objects; their answers revert to Done.
    void reap();

    [[nodiscard]] Phase     phase(ObjectId id) const noexcept;
    [[nodiscard]] Pose      pose(ObjectId id) const noexcept;
    [[nodiscard]] EndReason endReason(ObjectId id) const noexcept;
    [[nodiscard]] bool      active(ObjectId id) const noexcept { return phase(id) != Phase::Done; }
    // Where the proxy waits while Gated: the oldest cell whose mesh copy is not yet known to be gone.
    [[nodiscard]] CellPos gateCell(ObjectId id) const noexcept;
    // Builds tessellating this cell now must leave the block out.
    [[nodiscard]] bool meshSuppressed(CellPos const& cell) const noexcept;
    // Cells whose chunk should be rebuilt; the platform marks them dirty. Clears the list.
    [[nodiscard]] std::vector<CellPos> takeRebuilds();
    // Evidence floor of the object's landing cell; 0 until it is known. Never decreases.
    [[nodiscard]] Seq needSeq(ObjectId id) const noexcept;
    // Counts the objects that took this destination cell; strictly increasing per cell.
    [[nodiscard]] uint64_t epoch(CellPos const& dst) const noexcept;
    [[nodiscard]] size_t   activeCount() const noexcept;

private:
    // A cell the old mesh may show the block in, until a build that began at or after `release` says otherwise.
    struct Gate {
        CellPos cell{};
        Seq     release{};
    };
    struct Object {
        CellPos           src{};
        CellPos           dst{};
        std::vector<Gate> gates; // oldest first; an object that never got past its gate hands its gates on
        Phase             phase{Phase::Gated};
        EndReason         end{EndReason::None};
        Seq               landSeq{};
        Seq               need{};
        bool              landed{};
        bool              visualDone{};
    };

    void          finish(ObjectId id, Object& object, EndReason why);
    void          dropGates(ObjectId id, Object& object);
    void          releaseGate(CellPos const& cell);
    void          enterHolding(Object& object, Seq now);
    void          ask(CellPos const& cell);
    Object*       find(ObjectId id) noexcept;
    Object const* find(ObjectId id) const noexcept;

    std::unordered_map<ObjectId, Object>               mObjects;
    std::unordered_map<CellPos, ObjectId, CellPosHash> mByDst;   // newest object heading for the cell
    std::unordered_map<CellPos, ObjectId, CellPosHash> mHolder;  // object whose block landed in the cell
    std::unordered_map<CellPos, int, CellPosHash> mGated; // how many Gated objects wait on each cell
    std::unordered_map<CellPos, uint64_t, CellPosHash> mEpoch;
    std::unordered_set<CellPos, CellPosHash>           mAsked;
    std::vector<CellPos>                               mRebuilds;
};

} // namespace optipiston::core
