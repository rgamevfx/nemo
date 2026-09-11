#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/core/document/Graph.hpp"

namespace nemo {

// Project color policy per spec section 5: the names the project resolves
// against its OpenColorIO config. Plain name records only — the persistent
// model carries no OCIO runtime objects (spec section 10.2).
//
// Defaults (documented, applied by the loader when a saved document carries
// no policy block): scene-linear working space, standard sRGB display view
// for both viewing and delivery. Viewer and delivery transforms are
// identified as "display/view" pairs; a bare name is interpreted as
// "<name>/<name>" by consumers.
struct ColorPolicy {
    std::string workingSpace{"linear"};
    std::string viewerTransform{"sRGB/rec709"};
    std::string deliveryTransform{"sRGB/rec709"};

    [[nodiscard]] bool operator==(const ColorPolicy&) const = default;
};

// A persistent reference to real source media (issue #11): identity, time
// mapping, and interpretation policy — plain name/value records only. No
// decoder, GPU, or other runtime objects live here; runtime decode state
// belongs to the execution layer (spec section 10.2).
struct SourceReference {
    std::string path;
    // Time mapping per spec section 4.3: composition local time zero reads
    // `frameOffset`; each composition frame advances `frameStep` source
    // frames. frameAt() applies the mapping and rejects results that no
    // source can honor (negative or overflowing frame numbers).
    std::int64_t frameOffset{0};
    std::int64_t frameStep{1};
    // Interpretation policy overrides (spec section 5, issue #21): empty
    // means strict stream tags — the recorded transfer/primaries/matrix/
    // range are honored as-is. Non-empty entries fill only unspecified
    // stream tags; tagged values remain authoritative (media contract #21).
    std::map<std::string, std::string> interpretation;
    // Explicit media revision. Re-importing/replacing bytes at the same
    // path advances this value; file watching is not implicit evaluation.
    std::uint64_t revision{0};

    [[nodiscard]] bool operator==(const SourceReference&) const = default;

    // Mapped source frame for composition `localTime`. Throws
    // std::runtime_error when the mapping would produce a negative frame or
    // overflow 64-bit arithmetic: a request beyond the representable source
    // range is an error, not a wrapped value.
    [[nodiscard]] std::int64_t frameAt(std::int64_t localTime) const;
};

// Document model per spec section 10.2: no Qt, Vulkan, or plugin-runtime
// objects live in the persistent model.
struct Document {
    static inline constexpr int kSchemaVersion = 1;

    Document() = default;
    explicit Document(std::shared_ptr<const NodeCatalog> catalog) : graph(std::move(catalog)) {}

    std::map<std::string, SourceReference> sources;
    int schemaVersion{kSchemaVersion};
    std::string name;
    Graph graph;
    ColorPolicy color;

    // Pure publication token over the command generation and authored setup
    // state. Undo/redo never reuse a generation; content-derived evaluation
    // reuse ignores it. Concurrent queries on immutable snapshots do not write.
    [[nodiscard]] std::uint64_t stateRevision() const;

private:
    friend class CommandStack;
    std::uint64_t freshnessRevision_{1};
};

// A validated edit applied to CommandStack's private candidate. Callbacks
// mutate only that candidate, never external state. The stack is the sole
// undo/redo owner; factories do not retain a second inverse/history.
struct Command {
    std::string label;
    std::function<void(Document&)> apply;
};

// Owner-thread history. Validate on a private candidate, prepare publication,
// then commit without allocation. History retains states rather than mutable
// command closures, so failed preparation cannot damage undo/redo inverses.
class CommandStack {
public:
    explicit CommandStack(Document& document, std::size_t capacity = 256);

    using BeforeCommit = std::function<void(const Document&, const Document&)>;
    void push(Command command, const BeforeCommit& beforeCommit = {});
    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const { return !redo_.empty(); }
    bool undo(const BeforeCommit& beforeCommit = {});
    bool redo(const BeforeCommit& beforeCommit = {});

    [[nodiscard]] std::size_t depth() const { return undo_.size(); }
    void clear();

private:
    void prepare(Document& candidate) const;
    Document& document_;
    std::size_t capacity_;
    std::vector<Document> undo_;
    std::vector<Document> redo_;
};

// Convenience factories: sanctioned graph edits for panels and adapters
// (spec section 10.2 "Commands"). An optional shared_ptr receives a candidate
// identity; consume it only after successful publication. History restores
// the same identity on redo without rerunning creation.
Command addNodeCommand(std::string type, std::string name, std::shared_ptr<NodeId> createdId = {});
Command connectCommand(PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId = {});

// Convenience factory: edits one parameter on a stable node identity.
Command setParamCommand(NodeId nodeId, std::string key, std::string value);

// Convenience factory: changes a display label without changing identity.
Command renameNodeCommand(NodeId nodeId, std::string name);

// Convenience factory: atomically groups validated child commands into one
// document change and one undo step.
Command transactionCommand(std::string label, std::vector<Command> commands);

// Convenience factory: replaces the whole color policy (the sanctioned edit
// for viewer/delivery/working-space changes, issue #9 acceptance example 3).
Command setColorPolicyCommand(ColorPolicy value);

// Convenience factory: adds or replaces one source reference by key. The
// previous reference is snapshotted for undo; an empty key, empty path, or
// zero frameStep is rejected before the document is touched.
Command setSourceCommand(std::string id, SourceReference value);

}  // namespace nemo
