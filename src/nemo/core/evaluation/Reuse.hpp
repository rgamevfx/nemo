#pragma once

// Evaluator graph reuse and invalidation (issue #9, spec sections 8/10.3,
// ADR-0004).
//
// Reuse identity is content-derived, never history-derived:
//
//   * A node result's ResultKey covers the implementation version, node
//     type, the node's authored parameter state, the ResultKeys of its
//     effective inputs in port order, and the request's mapped local time,
//     region, channels, quality, plus the working-space color
//     interpretation. Keys are computed before execution, so authored
//     state is what is hashed: executor-injected defaults are a
//     deterministic function of the implementation version (recorded into
//     effectiveParams at execution). A node with an explicit default value
//     and one relying on the default therefore get different keys —
//     conservative: this can only miss reuse, never serve a wrong result.
//   * Sharing is by effective state, so shared VFX with different grades
//     reuse the shared upstream results, structurally identical
//     occurrences share results, and different representations (time,
//     region, quality) coexist as distinct keys.
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
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Plan.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

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
struct KeyContext {
    std::uint64_t implementationTag{0};
};

// Version of a node type's evaluation semantics. Bump when an operation's
// meaning changes in a way keys must observe (different sampling, defaults,
// alpha convention) so cached results from the older semantics miss.
[[nodiscard]] std::uint64_t implementationVersion(const std::string& nodeType);

// The reuse key of `node`'s result under `request`. `inputKeyHashes` are the
// key hashes of the node's effective inputs in declared port order (empty
// for source nodes).
[[nodiscard]] ResultKey nodeResultKey(const Document& document, const Node& node,
                                      const std::vector<std::uint64_t>& inputKeyHashes,
                                      const EvaluationRequest& request, const KeyContext& context = {});

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
        if (ticket.generation != generation_ || ticket.documentRevision != document.stateRevision()) {
            ++counts_.staleRejected;
            return false;
        }
        std::erase_if(entries_, [&key, this](const Entry& entry) {
            if (entry.key == key) {
                ++counts_.evicted;
                return true;
            }
            return false;
        });
        while (entries_.size() >= maxEntries_) {
            entries_.pop_front();
            ++counts_.evicted;
        }
        entries_.push_back(Entry{std::move(key), std::move(image), identity});
        ++counts_.published;
        return true;
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

    // Explicit invalidation of one identity's representations.
    void evict(const ResultKey& key) {
        const std::size_t before = entries_.size();
        std::erase_if(entries_, [&key](const Entry& entry) { return entry.key == key; });
        counts_.evicted += before - entries_.size();
    }

    void clear() { entries_.clear(); }

    [[nodiscard]] CacheCounts counts() const { return counts_; }

private:
    std::size_t maxEntries_;
    std::uint64_t generation_{0};
    mutable CacheCounts counts_;
    std::deque<Entry> entries_;  // insertion order for the bounded cap
};

}  // namespace nemo
