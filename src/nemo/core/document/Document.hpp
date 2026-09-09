#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
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

    // Persistent source media, addressed by key (issue #11). Source nodes
    // reference entries through their `source` parameter; the reference is
    // part of the node's reuse identity, so changing it invalidates only
    // the dependent branch. Direct writes are fixture/setup only — the
    // sanctioned edit is setSourceCommand().
    std::map<std::string, SourceReference> sources;

    int schemaVersion{kSchemaVersion};
    std::string name;
    Graph graph;
    ColorPolicy color;

    // Freshness token for evaluation publication (issue #9, spec section 8):
    // combines the graph edit revision with the color policy values and the
    // persistent source state. Any sanctioned mutation changes it; equality
    // with a value captured at request start is the stale-publication
    // check. Reuse identity itself is content-derived (see
    // evaluation/Reuse.hpp) and deliberately ignores this value, so
    // unrelated edits never invalidate branch reuse.
    [[nodiscard]] std::uint64_t stateRevision() const;
};

// A validated edit with its inverse. Commands are the only sanctioned way to
// mutate a Document: UI, scripts, and agent adapters share this mutation API
// (spec section 10.2 "Commands").
struct Command {
    std::string label;
    std::function<void(Document&)> apply;
    std::function<void(Document&)> revert;
};

// Undo/redo stack with a bounded history. push() applies immediately;
// a failed apply leaves the document untouched and throws through.
class CommandStack {
public:
    explicit CommandStack(Document& document, std::size_t capacity = 256) : document_(document), capacity_(capacity) {}

    void push(Command command);
    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const { return !redo_.empty(); }
    bool undo();
    bool redo();

    [[nodiscard]] std::size_t depth() const { return undo_.size(); }
    void clear();

private:
    Document& document_;
    std::size_t capacity_;
    std::deque<Command> undo_;
    std::deque<Command> redo_;
};

// Convenience factories: sanctioned graph edits for panels and adapters
// (spec section 10.2 "Commands"). Pass a shared_ptr to receive the created
// identity out of the command; undo removes the node / disconnects the
// edge, redo recreates it. Creation failures (unknown type, occupied port,
// cycle) throw from apply and leave the document untouched.
Command addNodeCommand(std::string type, std::string name, std::shared_ptr<NodeId> createdId = {});
Command connectCommand(PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId = {});

// Convenience factory: renames an existing node's parameter.
Command setParamCommand(std::string nodeName, std::string key, std::string value);

// Convenience factory: replaces the whole color policy (the sanctioned edit
// for viewer/delivery/working-space changes, issue #9 acceptance example 3).
Command setColorPolicyCommand(ColorPolicy value);

// Convenience factory: adds or replaces one source reference by key. The
// previous reference is snapshotted for undo; an empty key, empty path, or
// zero frameStep is rejected before the document is touched.
Command setSourceCommand(std::string id, SourceReference value);

}  // namespace nemo
