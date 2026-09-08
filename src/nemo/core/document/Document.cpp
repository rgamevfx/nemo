#include "nemo/core/document/Document.hpp"

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
    // FNV-1a 64 over the graph edit revision and the color policy names.
    // Content-derived: no mutation path can forget to bump it.
    std::uint64_t hash = kFnv1a64Basis;
    hashMixWord(hash, graph.revision());
    hashMixText(hash, color.workingSpace);
    hashMixText(hash, color.viewerTransform);
    hashMixText(hash, color.deliveryTransform);
    return hash;
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

}  // namespace nemo
