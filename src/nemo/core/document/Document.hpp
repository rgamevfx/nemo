#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "nemo/core/document/Graph.hpp"

namespace nemo {

// Document model per spec section 10.2: no Qt, Vulkan, or plugin-runtime
// objects live in the persistent model.
struct Document {
    static inline constexpr int kSchemaVersion = 1;

    int schemaVersion{kSchemaVersion};
    std::string name;
    Graph graph;
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
    explicit CommandStack(Document& document, std::size_t capacity = 256)
        : document_(document), capacity_(capacity) {}

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

} // namespace nemo
