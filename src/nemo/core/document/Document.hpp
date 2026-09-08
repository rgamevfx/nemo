#pragma once

#include <deque>
#include <functional>
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

// Document model per spec section 10.2: no Qt, Vulkan, or plugin-runtime
// objects live in the persistent model.
struct Document {
    static inline constexpr int kSchemaVersion = 1;

    int schemaVersion{kSchemaVersion};
    std::string name;
    Graph graph;
    ColorPolicy color;

    // Freshness token for evaluation publication (issue #9, spec section 8):
    // combines the graph edit revision with the color policy values. Any
    // sanctioned mutation changes it; equality with a value captured at
    // request start is the stale-publication check. Reuse identity itself is
    // content-derived (see evaluation/Reuse.hpp) and deliberately ignores
    // this value, so unrelated edits never invalidate branch reuse.
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

// Convenience factory: renames an existing node's parameter.
Command setParamCommand(std::string nodeName, std::string key, std::string value);

// Convenience factory: replaces the whole color policy (the sanctioned edit
// for viewer/delivery/working-space changes, issue #9 acceptance example 3).
Command setColorPolicyCommand(ColorPolicy value);

}  // namespace nemo
