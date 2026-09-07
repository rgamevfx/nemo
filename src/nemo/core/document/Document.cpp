#include "nemo/core/document/Document.hpp"

#include <stdexcept>
#include <utility>

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

Command setParamCommand(std::string nodeName, std::string key, std::string value) {
    // Capture enough state to restore the previous value without keeping a
    // reference into the document across undo.
    std::string previous;
    bool hadPrevious = false;

    Command command;
    command.label = "set " + key + " on " + nodeName;
    command.apply = [nodeName, key, value](Document& doc) {
        for (auto& node : doc.graph.nodes()) {
            if (node.name != nodeName) {
                continue;
            }
            node.params[key] = value;
            return;
        }
        throw std::runtime_error("setParam: no node named '" + nodeName + "'");
    };
    command.revert = [nodeName, key, previous, hadPrevious](Document& doc) {
        for (auto& node : doc.graph.nodes()) {
            if (node.name != nodeName) {
                continue;
            }
            if (hadPrevious) {
                node.params[key] = previous;
            } else {
                node.params.erase(key);
            }
            return;
        }
        throw std::runtime_error("setParam revert: no node named '" + nodeName + "'");
    };
    return command;
}

} // namespace nemo
