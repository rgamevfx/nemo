#include "nemo/core/document/Document.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "nemo/core/Hashing.hpp"

namespace nemo {

void CommandStack::push(Command command) {
    command.apply(document_);
    undo_.push_back(std::move(command));
    redo_.clear();
    while (undo_.size() > capacity_) {
        undo_.pop_front();
    }
}

bool CommandStack::undo() {
    if (undo_.empty()) {
        return false;
    }
    Command command = std::move(undo_.back());
    undo_.pop_back();
    command.revert(document_);
    redo_.push_back(std::move(command));
    return true;
}

bool CommandStack::redo() {
    if (redo_.empty()) {
        return false;
    }
    Command command = std::move(redo_.back());
    redo_.pop_back();
    command.apply(document_);
    undo_.push_back(std::move(command));
    return true;
}

void CommandStack::clear() {
    undo_.clear();
    redo_.clear();
}

std::uint64_t Document::stateRevision() const {
    // FNV-1a 64 over the graph edit revision, the color policy names, and
    // the persistent source state. Content-derived: no mutation path can
    // forget to bump it. The sources contribution is the full reference
    // content (key, path, time mapping, interpretation policy), so a
    // source edit moves the publication ticket while reuse identity —
    // which keys source state per source node — only invalidates the
    // dependent branch (issue #11).
    std::uint64_t hash = kFnv1a64Basis;
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

Command setParamCommand(std::string nodeName, std::string key, std::string value) {
    // The previous value is snapshotted by the first apply and shared with
    // revert; redo re-applies without re-snapshotting.
    auto previous = std::make_shared<std::optional<std::string>>();

    Command command;
    command.label = "set " + key + " on " + nodeName;
    command.apply = [nodeName, key, value, previous](Document& doc) {
        Node* node = doc.graph.nodeByName(nodeName);
        if (!node) {
            throw std::runtime_error("setParam: no node named '" + nodeName + "'");
        }
        if (!*previous) {
            const auto it = node->params.find(key);
            *previous = it != node->params.end() ? std::optional<std::string>{it->second} : std::nullopt;
        }
        doc.graph.setParam(node->id, key, value);
    };
    command.revert = [nodeName, key, previous](Document& doc) {
        Node* node = doc.graph.nodeByName(nodeName);
        if (!node) {
            throw std::runtime_error("setParam revert: no node named '" + nodeName + "'");
        }
        if (*previous) {
            doc.graph.setParam(node->id, key, **previous);
        } else {
            doc.graph.eraseParam(node->id, key);
        }
    };
    return command;
}

Command setColorPolicyCommand(ColorPolicy value) {
    // The previous policy is snapshotted by the first apply and shared with
    // revert; redo re-applies without re-snapshotting.
    auto previous = std::make_shared<std::optional<ColorPolicy>>();

    Command command;
    command.label = "set color policy";
    command.apply = [value, previous](Document& doc) {
        if (!*previous) {
            *previous = doc.color;
        }
        doc.color = value;
    };
    command.revert = [previous](Document& doc) {
        if (*previous) {
            doc.color = **previous;
        }
    };
    return command;
}

namespace {
Command graphEdit(std::string label, std::function<void(Graph&)> edit) {
    auto checkpoint = std::make_shared<std::optional<Graph>>();
    return Command{std::move(label),
                   [checkpoint, edit = std::move(edit)](Document& document) {
                       if (*checkpoint) {
                           document.graph.exchangeState(**checkpoint);
                       } else {
                           Graph changed = document.graph;
                           edit(changed);  // Validate before mutating the Document.
                           document.graph.exchangeState(changed);
                           checkpoint->emplace(std::move(changed));
                       }
                   },
                   [checkpoint](Document& document) { document.graph.exchangeState(**checkpoint); }};
}
}  // namespace

Command addNodeCommand(std::string type, std::string name, std::shared_ptr<NodeId> createdId) {
    return graphEdit("add node '" + name + "'",
                     [type = std::move(type), name = std::move(name), createdId](Graph& graph) {
                         const auto id = graph.addNode(type, name);
                         if (createdId)
                             *createdId = id;
                     });
}

Command connectCommand(PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId) {
    return graphEdit("connect node " + std::to_string(from.node) + " to " + std::to_string(to.node),
                     [from, to, createdId](Graph& graph) {
                         const auto id = graph.connect(from, to);
                         if (createdId)
                             *createdId = id;
                     });
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
    // The previous reference (or its absence) is snapshotted by the first
    // apply and shared with revert; redo re-applies without re-snapshotting.
    auto previous = std::make_shared<std::optional<SourceReference>>();
    Command command;
    command.label = "set source '" + id + "'";
    command.apply = [id, value, previous](Document& doc) {
        if (!*previous) {
            const auto it = doc.sources.find(id);
            *previous = it != doc.sources.end() ? std::optional<SourceReference>{it->second} : std::nullopt;
        }
        doc.sources[id] = value;
    };
    command.revert = [id, previous](Document& doc) {
        if (*previous) {
            doc.sources[id] = **previous;
        } else {
            doc.sources.erase(id);
        }
    };
    return command;
}

}  // namespace nemo
