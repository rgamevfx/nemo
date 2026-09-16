#pragma once

// Evaluator graph reuse and invalidation (issue #9, spec sections 8/10.3,
// ADR-0004).
//
// Reuse identity is content-derived, never history-derived:
//
//   * A node result's ResultKey covers the implementation version, node
//     type, its effective parameter state at the requested local time, the
//     ResultKeys of its effective inputs in port order, and the request's
//     mapped local time, region, channels, quality, plus the working-space
//     color interpretation. Keys are computed before execution from a local
//     resolved-parameter snapshot: static/default values and authored
//     animation are included, while the Document itself is never mutated.
//     A node with an explicit default value and one relying on the default
//     therefore get different keys — conservative: this can only miss reuse,
//     never serve a wrong result.
//   * Sharing is by effective state, so shared VFX with different grades
//     reuse the shared upstream results, structurally identical occurrences
//     share results, and different representations (time, region, quality)
//     coexist as distinct keys.
//
// Viewer and delivery transform state is deliberately excluded from
// scene-linear keys (ADR-0004: viewing transforms are downstream of the
// reusable composition results). viewerResultKey() bakes viewing state in;
// it is the identity seam for the viewer-cache tickets (#11/#12).
//
// Publication freshness (spec section 10.2: stale work never overwrites
// newer results) is guarded by EvaluationTicket: a result computed against
// a captured document revision and request generation is published only
// when neither has moved on. Superseded publications are discarded and
// counted, never stored.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

struct EffectiveSourceRequest;

// Canonical, content-derived identity of one node's reusable result.
// `hash` is the FNV-1a 64 of `canonical`; equality compares the canonical
// form, so hash collisions cannot cause wrong reuse.
struct ResultKey {
    std::uint64_t hash{0};
    std::string canonical;

    [[nodiscard]] bool operator==(const ResultKey&) const = default;
};

// Executor-supplied implementation identity mixed into every key: the GPU
// path fingerprints its effect library so Slang and GLSL front ends (or any
// change to either) never share cache entries; the CPU reference uses 0.
//
// `colorConfigIdentity` is the opaque identity of the color conversion
// configuration actually in effect (issue #75): the OCIO config/context cache
// ID plus the descriptor of the resolved transform, computed by the media
// module that owns the config. It is empty when no configuration is in effect
// (the legacy fixed interpretation), which is a defined value rather than a
// fallback: a result produced under an unknown configuration is never shared
// with one produced under a known configuration, and editing a config in place
// cannot alias a previously cached result.
//
// `description` is the described image this key is computed for (issue #88):
// its logical format and signed data bounds, its retained-edge-domain claim
// (issue #92), its pixel aspect, channel naming, precision, alpha association
// and colour interpretation all participate, so a result can never be served
// for an image with different meaning. `source` is the node's pre-resolved
// effective source request, supplied for a source node so the key carries
// exactly the frame the executor reads instead of resolving that request a
// second time.
struct KeyContext {
    std::uint64_t implementationTag{0};
    std::string colorConfigIdentity;
    const ImageDescription* description{nullptr};
    const EffectiveSourceRequest* source{nullptr};
};

// Version of the image/region coordinate contract (issue #88) mixed into every
// content key. A description states what an image is; this states what the
// numbers in it mean, so a cached result produced under a different coordinate
// or window convention can never be reused. Bump it whenever the meaning of a
// region, a data window, a sampling lattice or a described image changes.
// v2 (issue #90): channel naming is a named set, not a four-letter string —
// an alpha-only or multilayer image and a request's channel demand carry
// different identity than the fixed four-channel form.
// v3 (issue #92): a description states whether its finite data bounds are a
// retained edge domain the producer answers outside of, so an image whose
// samples continue past those bounds is a different image than one that is
// transparent there.
inline constexpr std::string_view kImageCoordinateContract = "image-space-v3";

// Input-key contribution for a declared-but-absent optional input slot. It is
// a fixed, executor-independent token that keeps the slot's position in the
// key: a missing mask and a connected mask (even one whose maskChannel
// selects none) always produce different keys, while the port order of real
// producers is preserved.
inline constexpr std::uint64_t kAbsentInputKeyHash = 0x1F3D5B79AB0C2E4DULL;

// the key hashes of the node's effective inputs in declared port order (empty
// for source nodes). The request's network scope is part of the identity.
[[nodiscard]] ResultKey nodeResultKey(const Document& document, const NodeInstance& node,
                                      const std::vector<std::uint64_t>& inputKeyHashes,
                                      const EvaluationRequest& request, const KeyContext& context = {});

// Image meaning is independent of which rectangle currently backs it. Inputs
// contribute content keys, never allocation/coverage keys; otherwise a pan
// would invalidate downstream pixels whose dependencies have not changed.
[[nodiscard]] ResultKey nodeContentKey(const Document& document, const NodeInstance& node,
                                       const std::vector<std::uint64_t>& inputContentHashes,
                                       const EvaluationRequest& request, const KeyContext& context = {});
[[nodiscard]] ResultKey regionResultKey(const ResultKey& contentKey, const EvaluationRequest& request);

// Bakes the document's viewing state into a scene-linear key: the identity
// of a viewer representation, not a composition result. A viewer-transform
// edit changes this key while the scene-linear key — and therefore upstream
// scene-linear reuse — stays valid.
[[nodiscard]] ResultKey viewerResultKey(const ResultKey& sceneLinearKey, const ColorPolicy& policy);

// Publication ticket: the request generation plus the document revision
// captured when evaluation began.
struct EvaluationTicket {
    std::uint64_t generation{0};
    std::uint64_t documentRevision{0};
};

// Observable reuse counters (issue #9 verification: counts defend avoided
// work, not storage layout).
struct CacheCounts {
    std::uint64_t hits{0};           // results served from the cache
    std::uint64_t misses{0};         // results that had to be computed
    std::uint64_t published{0};      // results accepted into the cache
    std::uint64_t staleRejected{0};  // publications discarded (stale ticket)
    std::uint64_t evicted{0};        // entries dropped by the bounded cap or explicit eviction

    [[nodiscard]] bool operator==(const CacheCounts&) const = default;
};

// Bounded reuse store for one residency (CpuImage in core, GpuNodeImage in
// the eval module — the template keeps the policy identical for both).
// Entries are indexed by ResultKey across nodes: identity is content, not
// node id. The entry cap is a residency bound (drop-oldest); the LRU/disk
// eviction policy of issue #14 builds elsewhere on this bound.
template <typename ImageT>
class ResultCache {
public:
    struct Entry {
        ResultKey key;
        std::shared_ptr<const ImageT> image;
        ImageIdentity identity;
        std::optional<ResultKey> contentKey;
        EvaluationRequest request;
    };

    explicit ResultCache(std::size_t maxEntries = 512) : maxEntries_(maxEntries) {}

    // Issues a fresh publication ticket for a new evaluation request.
    [[nodiscard]] EvaluationTicket beginTicket(const Document& document) {
        ++generation_;
        return EvaluationTicket{generation_, document.stateRevision()};
    }

    // Publishes one computed result. Rejected (and counted) when the ticket
    // is stale — a newer request began, or the document changed since the
    // ticket captured its revision (spec section 8: obsolete writes are
    // discarded, never published).
    bool publish(const Document& document, const EvaluationTicket& ticket, ResultKey key,
                 std::shared_ptr<const ImageT> image, ImageIdentity identity) {
        return publishEntry(document, ticket, Entry{std::move(key), std::move(image), std::move(identity), {}, {}});
    }

    // Geometry travels with storage, so a covering hit can be consumed in
    // place without copying a cropped input into a second allocation.
    bool publishRegion(const Document& document, const EvaluationTicket& ticket, ResultKey contentKey,
                       const EvaluationRequest& request, std::shared_ptr<const ImageT> image, ImageIdentity identity) {
        auto key = regionResultKey(contentKey, request);
        return publishEntry(
            document, ticket,
            Entry{std::move(key), std::move(image), std::move(identity), std::move(contentKey), request});
    }

    // Exact-key lookup. Counts one hit or miss per call; executors look up
    // each scheduled node exactly once.
    [[nodiscard]] std::optional<Entry> find(const ResultKey& key) const {
        for (const Entry& entry : entries_) {
            if (entry.key == key) {
                ++counts_.hits;
                return entry;
            }
        }
        ++counts_.misses;
        return std::nullopt;
    }

    // Prefer the smallest resident rectangle that covers this demand. An
    // overlapping but incomplete rectangle is NOT a hit: missing samples must
    // never be mistaken for an image border.
    [[nodiscard]] std::optional<Entry> findRegion(const ResultKey& contentKey, const EvaluationRequest& needed) const {
        const Entry* best = nullptr;
        for (const Entry& entry : entries_) {
            if (!entry.contentKey || *entry.contentKey != contentKey || !covers(entry.request, needed))
                continue;
            if (best == nullptr || area(entry.request.region) < area(best->request.region))
                best = &entry;
        }
        if (best != nullptr) {
            ++counts_.hits;
            return *best;
        }
        ++counts_.misses;
        return std::nullopt;
    }

    // Explicit invalidation of one identity's representations.
    void evict(const ResultKey& key) {
        const std::size_t before = entries_.size();
        std::erase_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
        counts_.evicted += before - entries_.size();
    }

    void clear() { entries_.clear(); }

    [[nodiscard]] CacheCounts counts() const { return counts_; }

private:
    [[nodiscard]] static std::int64_t area(const Region& region) {
        return static_cast<std::int64_t>(region.width) * region.height;
    }

    [[nodiscard]] static bool covers(const EvaluationRequest& stored, const EvaluationRequest& needed) {
        if (stored.samplingScale <= 0 || stored.samplingScale != needed.samplingScale ||
            stored.imageWidth() != needed.imageWidth() || stored.imageHeight() != needed.imageHeight())
            return false;
        const auto dx = static_cast<std::int64_t>(needed.region.x) - stored.region.x;
        const auto dy = static_cast<std::int64_t>(needed.region.y) - stored.region.y;
        return needed.region.width > 0 && needed.region.height > 0 && dx >= 0 && dy >= 0 &&
               dx % stored.samplingScale == 0 && dy % stored.samplingScale == 0 &&
               dx + needed.region.width <= stored.region.width && dy + needed.region.height <= stored.region.height;
    }

    bool publishEntry(const Document& document, const EvaluationTicket& ticket, Entry entry) {
        if (ticket.generation != generation_ || ticket.documentRevision != document.stateRevision()) {
            ++counts_.staleRejected;
            return false;
        }
        if (maxEntries_ == 0)
            return false;
        std::erase_if(entries_, [&entry, this](const Entry& existing) {
            if (existing.key == entry.key) {
                ++counts_.evicted;
                return true;
            }
            return false;
        });
        while (entries_.size() >= maxEntries_) {
            entries_.pop_front();
            ++counts_.evicted;
        }
        entries_.push_back(std::move(entry));
        ++counts_.published;
        return true;
    }
    std::size_t maxEntries_;
    std::uint64_t generation_{0};
    mutable CacheCounts counts_;
    std::deque<Entry> entries_;  // insertion order for the bounded cap
};

}  // namespace nemo
