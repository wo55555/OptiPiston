#include "core/Overlay.h"

#include <algorithm>

namespace optipiston::core {

Overlay::Object* Overlay::find(ObjectId id) noexcept {
    auto const found = mObjects.find(id);
    return found == mObjects.end() ? nullptr : &found->second;
}

Overlay::Object const* Overlay::find(ObjectId id) const noexcept {
    auto const found = mObjects.find(id);
    return found == mObjects.end() ? nullptr : &found->second;
}

void Overlay::ask(CellPos const& cell) {
    if (mAsked.insert(cell).second) mRebuilds.push_back(cell);
}

void Overlay::releaseGate(CellPos const& cell) {
    auto const slot = mGated.find(cell);
    if (slot != mGated.end() && --slot->second <= 0) mGated.erase(slot);
}

void Overlay::dropGates(ObjectId id, Object& object) {
    for (auto const& gate : object.gates) {
        releaseGate(gate.cell);
    }
    object.gates.clear();
}

void Overlay::finish(ObjectId id, Object& object, EndReason why) {
    if (object.phase == Phase::Done) return;
    dropGates(id, object);
    object.phase    = Phase::Done;
    object.end      = why;
    auto const slot = mByDst.find(object.dst);
    if (slot != mByDst.end() && slot->second == id) mByDst.erase(slot);
    auto const holder = mHolder.find(object.dst);
    if (holder != mHolder.end() && holder->second == id) mHolder.erase(holder);
}

void Overlay::enterHolding(Object& object, Seq now) {
    object.phase = Phase::Holding;
    if (object.landed) {
        // A build that began before the mod stopped keeping the cell out of the mesh proves nothing.
        object.need = std::max({object.need, object.landSeq, now});
        ask(object.dst);
    }
}

void Overlay::start(ObjectId id, CellPos const& src, CellPos const& dst, Seq srcRelease, Seq dstLandSeq, Seq now) {
    (void)now;
    Object object;
    object.src     = src;
    object.dst     = dst;
    object.landed  = dstLandSeq != 0;
    object.landSeq = dstLandSeq;

    // An object that never got past its gate leaves the old mesh copy behind; the newcomer must wait for that too.
    std::vector<Gate> inherited;
    auto              take = [&](ObjectId otherId, Object* other) {
        if (!other || other->phase == Phase::Done) return;
        if (other->phase == Phase::Gated) inherited.insert(inherited.end(), other->gates.begin(), other->gates.end());
        finish(otherId, *other, EndReason::Superseded);
    };
    take(id, find(id));
    // The block this push picks up is the one that landed in the source cell. A block still on its way into that cell
    // is another block (a chain moves together), so it stays.
    if (auto const slot = mHolder.find(src); slot != mHolder.end()) take(slot->second, find(slot->second));
    if (object.landed)
        if (auto const slot = mHolder.find(dst); slot != mHolder.end()) take(slot->second, find(slot->second));
    inherited.push_back({src, srcRelease});
    for (auto const& gate : inherited) {
        auto const same = std::find_if(object.gates.begin(), object.gates.end(), [&](Gate const& have) {
            return have.cell == gate.cell;
        });
        if (same == object.gates.end()) object.gates.push_back(gate);
        else same->release = std::max(same->release, gate.release);
    }

    for (auto const& gate : object.gates) {
        ++mGated[gate.cell];
        ask(gate.cell);
    }
    bool const landed = object.landed;
    mObjects[id]      = std::move(object);
    mByDst[dst]       = id;
    if (landed) mHolder[dst] = id;
    ++mEpoch[dst];
}

void Overlay::visualEnded(ObjectId id, Seq now) {
    auto* object = find(id);
    if (!object || object->phase == Phase::Done) return;
    object->visualDone = true;
    if (object->phase == Phase::Animating) enterHolding(*object, now);
}

void Overlay::abandon(ObjectId id, Seq now) {
    (void)now;
    if (auto* object = find(id)) finish(id, *object, EndReason::Abandoned);
}

void Overlay::worldChanged(CellPos const& cell, WorldCell kind, Seq seq) {
    auto const slot = mByDst.find(cell);
    if (slot == mByDst.end()) return;
    auto* object = find(slot->second);
    if (!object || object->phase == Phase::Done) return;
    if (kind == WorldCell::Real) {
        object->landed  = true;
        object->landSeq = seq;
        mHolder[cell]   = slot->second;
        if (object->phase == Phase::Holding) {
            object->need = std::max(object->need, seq);
            ask(cell);
        }
    } else {
        // Air or a moving block: the block is on its way, so the proxy keeps covering.
        object->landed = false;
    }
}

void Overlay::promoted(BuildKey build, EvidenceLedger const& ledger, Seq now) {
    if (!ledger.known(build)) return;
    auto const sub = ledger.subChunk(build);
    // The table can change while we walk it only through finish(), which never inserts.
    for (auto& [id, object] : mObjects) {
        if (object.phase == Phase::Gated) {
            for (auto it = object.gates.begin(); it != object.gates.end();) {
                if (subChunkOf(it->cell) != sub) {
                    ++it;
                    continue;
                }
                auto const seen = ledger.judge(build, it->cell, it->release);
                if (seen == Seen::Absent || seen == Seen::Suppressed) {
                    releaseGate(it->cell);
                    it = object.gates.erase(it);
                } else {
                    ask(it->cell);
                    ++it;
                }
            }
            if (object.gates.empty()) {
                if (object.visualDone) enterHolding(object, now);
                else object.phase = Phase::Animating;
            }
            continue;
        }
        if (object.phase == Phase::Holding && object.landed && subChunkOf(object.dst) == sub) {
            switch (ledger.judge(build, object.dst, object.need)) {
            case Seen::Present: finish(id, object, EndReason::Mesh); break;
            case Seen::Absent: finish(id, object, EndReason::Culled); break;
            default: ask(object.dst); break;
            }
        }
    }
}

void Overlay::clear() {
    mObjects.clear();
    mByDst.clear();
    mHolder.clear();
    mGated.clear();
    mAsked.clear();
    mRebuilds.clear();
}

void Overlay::reap() {
    std::erase_if(mObjects, [](auto const& entry) { return entry.second.phase == Phase::Done; });
}

Phase Overlay::phase(ObjectId id) const noexcept {
    auto const* object = find(id);
    return object ? object->phase : Phase::Done;
}

Pose Overlay::pose(ObjectId id) const noexcept {
    switch (phase(id)) {
    case Phase::Gated: return Pose::Source;
    case Phase::Animating: return Pose::Moving;
    case Phase::Holding: return Pose::Dest;
    default: return Pose::None;
    }
}

CellPos Overlay::gateCell(ObjectId id) const noexcept {
    auto const* object = find(id);
    if (!object) return CellPos{};
    return object->gates.empty() ? object->src : object->gates.front().cell;
}

EndReason Overlay::endReason(ObjectId id) const noexcept {
    auto const* object = find(id);
    return object ? object->end : EndReason::None;
}

bool Overlay::meshSuppressed(CellPos const& cell) const noexcept {
    // The cell a pushed block is heading for, and the cells it left while the old mesh copy may still be on screen.
    if (auto const slot = mByDst.find(cell); slot != mByDst.end()) {
        auto const* object = find(slot->second);
        if (object && (object->phase == Phase::Gated || object->phase == Phase::Animating)) return true;
    }
    return mGated.contains(cell);
}

std::vector<CellPos> Overlay::takeRebuilds() {
    std::vector<CellPos> out;
    out.swap(mRebuilds);
    mAsked.clear();
    return out;
}

Seq Overlay::needSeq(ObjectId id) const noexcept {
    auto const* object = find(id);
    return object ? object->need : 0;
}

size_t Overlay::activeCount() const noexcept {
    return static_cast<size_t>(std::count_if(mObjects.begin(), mObjects.end(), [](auto const& entry) {
        return entry.second.phase != Phase::Done;
    }));
}

uint64_t Overlay::epoch(CellPos const& dst) const noexcept {
    auto const found = mEpoch.find(dst);
    return found == mEpoch.end() ? 0 : found->second;
}

} // namespace optipiston::core
