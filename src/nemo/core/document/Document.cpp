#include "nemo/core/document/Document.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "nemo/core/Hashing.hpp"

namespace nemo {

CommandStack::CommandStack(Document& document, std::size_t capacity) : document_(document), capacity_(capacity) {
    undo_.reserve(capacity_);
    redo_.reserve(capacity_);
}

void CommandStack::prepare(Document& candidate) const {
    if (document_.freshnessRevision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("document freshness revision exhausted");
    candidate.freshnessRevision_ = document_.freshnessRevision_ + 1;
    candidate.graph.restoreIdentityHighWatermarks(document_.graph.nextNodeId(), document_.graph.nextEdgeId());
}

void CommandStack::push(Command command, const BeforeCommit& beforeCommit) {
    if (!command.apply)
        throw std::invalid_argument("command must provide an apply operation");
    Document candidate = document_;
    command.apply(candidate);
    prepare(candidate);
    if (beforeCommit)
        beforeCommit(document_, candidate);

    // All potentially throwing work precedes publication. Reserved history
    // slots and noexcept document moves keep history and document atomic.
    static_assert(std::is_nothrow_move_assignable_v<Document>);
    static_assert(std::is_nothrow_move_constructible_v<Document>);
    if (capacity_ != 0) {
        if (undo_.size() == capacity_)
            undo_.erase(undo_.begin());
        undo_.push_back(std::move(document_));
    }
    document_ = std::move(candidate);
    redo_.clear();
}

bool CommandStack::undo(const BeforeCommit& beforeCommit) {
    if (undo_.empty())
        return false;
    Document candidate = undo_.back();
    prepare(candidate);
    if (beforeCommit)
        beforeCommit(document_, candidate);
    redo_.push_back(std::move(document_));
    document_ = std::move(candidate);
    undo_.pop_back();
    return true;
}

bool CommandStack::redo(const BeforeCommit& beforeCommit) {
    if (redo_.empty())
        return false;
    Document candidate = redo_.back();
    prepare(candidate);
    if (beforeCommit)
        beforeCommit(document_, candidate);
    undo_.push_back(std::move(document_));
    document_ = std::move(candidate);
    redo_.pop_back();
    return true;
}

void CommandStack::clear() {
    undo_.clear();
    redo_.clear();
}

std::uint64_t Document::stateRevision() const {
    std::uint64_t hash = kFnv1a64Basis;
    hashMixWord(hash, freshnessRevision_);
    hashMixWord(hash, graph.revision());
    hashMixText(hash, color.workingSpace);
    hashMixText(hash, color.viewerTransform);
    hashMixText(hash, color.deliveryTransform);
    for (const auto& [key, source] : sources) {
        hashMixText(hash, key);
        hashMixText(hash, source.path);
        hashMixWord(hash, static_cast<std::uint64_t>(source.frameOffset));
        hashMixWord(hash, static_cast<std::uint64_t>(source.frameStep));
        hashMixWord(hash, source.revision);
        for (const auto& [tag, value] : source.interpretation) {
            hashMixText(hash, tag);
            hashMixText(hash, value);
        }
        hashMixWord(hash, static_cast<std::uint64_t>(source.interpretation.size()));
    }
    hashMixWord(hash, static_cast<std::uint64_t>(sources.size()));
    return hash;
}

std::int64_t SourceReference::frameAt(std::int64_t localTime) const {
    // frame = frameOffset + localTime * frameStep, overflow-checked: a
    // negative frame or a wrap past 64-bit range is a request error, never
    // a silently wrapped value (issue #11 time mapping validation).
    if (frameStep == 0) {
        throw std::runtime_error("source '" + path + "': frameStep must not be zero");
    }
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const bool overflow =
        localTime > 0
            ? (frameStep > 0 ? localTime > maximum / frameStep : frameStep < minimum / localTime)
            : (localTime < 0 && (frameStep > 0 ? localTime < minimum / frameStep : localTime < maximum / frameStep));
    if (overflow)
        throw std::runtime_error("source '" + path + "': frame mapping multiplication overflows the 64-bit range");
    const auto product = localTime * frameStep;
    if ((product > 0 && frameOffset > maximum - product) || (product < 0 && frameOffset < minimum - product)) {
        throw std::runtime_error("source '" + path + "': frame mapping overflows the 64-bit range");
    }
    const auto frame = frameOffset + product;
    if (frame < 0) {
        throw std::runtime_error("source '" + path + "': local time " + std::to_string(localTime) +
                                 " maps to negative source frame " + std::to_string(frame));
    }
    return frame;
}

Command setParamCommand(NodeId nodeId, std::string key, std::string value) {
    return Command{"set " + key + " on node " + std::to_string(nodeId),
                   [nodeId, key = std::move(key), value = std::move(value)](Document& document) {
                       document.graph.setParam(nodeId, key, value);
                   }};
}

Command renameNodeCommand(NodeId nodeId, std::string name) {
    return Command{"rename node " + std::to_string(nodeId),
                   [nodeId, name = std::move(name)](Document& document) { document.graph.renameNode(nodeId, name); }};
}

Command transactionCommand(std::string label, std::vector<Command> commands) {
    if (commands.empty())
        throw std::invalid_argument("transaction must contain at least one command");
    for (const auto& command : commands)
        if (!command.apply)
            throw std::invalid_argument("transaction contains an incomplete command");
    return Command{std::move(label), [commands = std::move(commands)](Document& document) {
                       for (const auto& command : commands)
                           command.apply(document);
                   }};
}

Command setColorPolicyCommand(ColorPolicy value) {
    return Command{"set color policy", [value = std::move(value)](Document& document) { document.color = value; }};
}

Command addNodeCommand(std::string type, std::string name, std::shared_ptr<NodeId> createdId) {
    return Command{"add node '" + name + "'",
                   [type = std::move(type), name = std::move(name), createdId](Document& document) {
                       const auto id = document.graph.addNode(type, name);
                       if (createdId)
                           *createdId = id;
                   }};
}

Command connectCommand(PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId) {
    return Command{"connect node " + std::to_string(from.node) + " to " + std::to_string(to.node),
                   [from, to, createdId](Document& document) {
                       const auto id = document.graph.connect(from, to);
                       if (createdId)
                           *createdId = id;
                   }};
}

Command setSourceCommand(std::string id, SourceReference value) {
    if (id.empty()) {
        throw std::runtime_error("setSource: source key must not be empty");
    }
    if (value.path.empty()) {
        throw std::runtime_error("setSource: source '" + id + "' must reference a media path");
    }
    if (value.frameStep == 0) {
        throw std::runtime_error("setSource: source '" + id + "' frameStep must not be zero");
    }
    return Command{"set source '" + id + "'", [id = std::move(id), value = std::move(value)](Document& document) {
                       document.sources[id] = value;
                   }};
}

}  // namespace nemo
