#include "nemo/core/document/Graph.hpp"
#include "nemo/core/evaluation/Request.hpp"

#include <algorithm>
#include <limits>
#include <utility>
namespace nemo {
namespace {
std::string describe(PortRef ref) {
    return "node " + std::to_string(ref.node) + " port " + std::to_string(ref.port);
}
}  // namespace

Graph::Graph(std::shared_ptr<const NodeCatalog> catalog) : catalog_(std::move(catalog)) {
    if (!catalog_) {
        throw std::invalid_argument("graph catalog must not be null");
    }
}

void Graph::restoreIdentityHighWatermarks(NodeId nextNodeId, EdgeId nextEdgeId) {
    if (nextNodeId == kInvalidNode || nextEdgeId == kInvalidEdge) {
        throw GraphException(GraphError::InvalidId, "identity high watermarks must be nonzero");
    }
    nextNodeId_ = std::max(nextNodeId_, nextNodeId);
    nextEdgeId_ = std::max(nextEdgeId_, nextEdgeId);
}

const Node* Graph::findNode(NodeId id) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [id](const Node& n) { return n.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}

NodeId Graph::addNode(std::string type, std::string name) {
    if (nextNodeId_ == kInvalidNode || nextNodeId_ == std::numeric_limits<NodeId>::max()) {
        throw GraphException(GraphError::InvalidId, "node identity space is exhausted");
    }
    return addNodeWithId(nextNodeId_, std::move(type), std::move(name));
}

NodeId Graph::addNodeWithId(NodeId id, std::string type, std::string name, std::map<std::string, std::string> params) {
    if (id == kInvalidNode || id == std::numeric_limits<NodeId>::max()) {
        throw GraphException(GraphError::InvalidId, "node id must be a nonzero value below the identity limit");
    }
    if (findNode(id) != nullptr) {
        throw GraphException(GraphError::DuplicateId,
                             "node id " + std::to_string(id) + " already exists in this graph");
    }
    if (nodeByName(name) != nullptr) {
        throw GraphException(GraphError::DuplicateName, "node name '" + name + "' already exists in this graph");
    }
    nodes_.push_back(Node{.id = id, .type = std::move(type), .name = std::move(name), .params = std::move(params)});
    nextNodeId_ = std::max(nextNodeId_, static_cast<NodeId>(id + 1));
    ++revision_;
    return id;
}

void Graph::renameNode(NodeId id, std::string name) {
    Node* node = const_cast<Node*>(findNode(id));
    if (node == nullptr) {
        throw GraphException(GraphError::UnknownNode, "cannot rename unknown node " + std::to_string(id));
    }
    if (name.empty()) {
        throw GraphException(GraphError::InvalidName, "node name must not be empty");
    }
    if (const Node* existing = nodeByName(name); existing != nullptr && existing->id != id) {
        throw GraphException(GraphError::DuplicateName, "node name '" + name + "' already exists in this graph");
    }
    if (node->name == name)
        return;
    node->name = std::move(name);
    ++revision_;
}

void Graph::removeNode(NodeId id) {
    if (!findNode(id)) {
        throw GraphException(GraphError::UnknownNode, "cannot remove unknown node " + std::to_string(id));
    }
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [id](const Edge& e) { return e.from.node == id || e.to.node == id; }),
                 edges_.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [id](const Node& n) { return n.id == id; }),
                 nodes_.end());
    // Removing a source can change the incoming list of every destination.
    incomingCache_.clear();
    ++revision_;
}

const Node* Graph::node(NodeId id) const {
    return findNode(id);
}

const Node* Graph::nodeByName(const std::string& name) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [&name](const Node& n) { return n.name == name; });
    return it == nodes_.end() ? nullptr : &*it;
}

bool Graph::reachable(NodeId origin, NodeId target) const {
    if (origin == target) {
        return true;
    }
    // Depth-first traversal over output edges of visited nodes.
    std::vector<NodeId> stack{origin};
    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();
        for (const auto& edge : edges_) {
            if (edge.from.node != current) {
                continue;
            }
            if (edge.to.node == target) {
                return true;
            }
            stack.push_back(edge.to.node);
        }
    }
    return false;
}

std::optional<GraphErrorDetails> Graph::validateEdge(PortRef from, PortRef to) const {
    const Node* fromNode = findNode(from.node);
    const Node* toNode = findNode(to.node);
    if (!fromNode || !toNode) {
        return GraphErrorDetails{GraphError::UnknownNode,
                                 "connect references an unknown node: " + describe(from) + " -> " + describe(to)};
    }
    // Typed ports come from the immutable schema catalog. Unknown persisted
    // node types declare no ports and remain loadable recovery data.
    const NodeDescriptor* fromInterface = catalog_->find(fromNode->type);
    const NodeDescriptor* toInterface = catalog_->find(toNode->type);
    if (fromInterface && static_cast<std::size_t>(from.port) >= fromInterface->outputs.size()) {
        return GraphErrorDetails{GraphError::PortType,
                                 "cannot connect from " + describe(from) + ": node '" + fromNode->name + "' of type '" +
                                     fromNode->type + "' declares " + std::to_string(fromInterface->outputs.size()) +
                                     " output port(s)"};
    }
    if (toInterface && static_cast<std::size_t>(to.port) >= toInterface->inputs.size()) {
        return GraphErrorDetails{GraphError::PortType, "cannot connect into " + describe(to) + ": node '" +
                                                           toNode->name + "' of type '" + toNode->type + "' declares " +
                                                           std::to_string(toInterface->inputs.size()) +
                                                           " input port(s)"};
    }
    if (fromInterface && toInterface && fromInterface->outputs[from.port].kind != toInterface->inputs[to.port].kind) {
        return GraphErrorDetails{GraphError::PortType,
                                 "cannot connect " + describe(from) + " -> " + describe(to) + ": port kind " +
                                     std::to_string(static_cast<int>(fromInterface->outputs[from.port].kind)) +
                                     " does not match port kind " +
                                     std::to_string(static_cast<int>(toInterface->inputs[to.port].kind))};
    }
    for (const auto& edge : edges_) {
        if (edge.to == to) {
            return GraphErrorDetails{GraphError::PortOccupied, "input " + describe(to) + " is already fed by node " +
                                                                   std::to_string(edge.from.node)};
        }
    }
    if (reachable(to.node, from.node)) {
        return GraphErrorDetails{GraphError::Cycle, "connecting " + describe(from) + " -> " + describe(to) +
                                                        " would create a circular dependency through node " +
                                                        std::to_string(to.node)};
    }
    return std::nullopt;
}

EdgeId Graph::connect(PortRef from, PortRef to) {
    if (nextEdgeId_ == kInvalidEdge || nextEdgeId_ == std::numeric_limits<EdgeId>::max()) {
        throw GraphException(GraphError::InvalidId, "edge identity space is exhausted");
    }
    return connectWithId(nextEdgeId_, from, to);
}

EdgeId Graph::connectWithId(EdgeId id, PortRef from, PortRef to) {
    if (id == kInvalidEdge || id == std::numeric_limits<EdgeId>::max()) {
        throw GraphException(GraphError::InvalidId, "edge id must be a nonzero value below the identity limit");
    }
    if (std::find_if(edges_.begin(), edges_.end(), [id](const Edge& edge) { return edge.id == id; }) != edges_.end()) {
        throw GraphException(GraphError::DuplicateId,
                             "edge id " + std::to_string(id) + " already exists in this graph");
    }
    if (const auto problem = validateEdge(from, to)) {
        throw GraphException(problem->code, problem->message);
    }
    edges_.push_back(Edge{.id = id, .from = from, .to = to});
    nextEdgeId_ = std::max(nextEdgeId_, static_cast<EdgeId>(id + 1));
    incomingCache_.erase(to.node);
    ++revision_;
    return id;
}

void Graph::disconnect(EdgeId id) {
    const auto it = std::find_if(edges_.begin(), edges_.end(), [id](const Edge& e) { return e.id == id; });
    if (it == edges_.end()) {
        throw GraphException(GraphError::UnknownEdge, "cannot disconnect unknown edge " + std::to_string(id));
    }
    incomingCache_.erase(it->to.node);
    edges_.erase(it);
    ++revision_;
}

void Graph::setParam(NodeId id, const std::string& key, const std::string& value) {
    Node* node = const_cast<Node*>(findNode(id));
    if (node == nullptr) {
        throw GraphException(GraphError::UnknownNode, "cannot set a parameter on unknown node " + std::to_string(id));
    }
    if (const auto problem = catalog_->validateParameter(node->type, key, value)) {
        throw GraphException(GraphError::ParameterValue,
                             "node '" + node->name + "' parameter '" + key + "': " + *problem);
    }
    node->params[key] = value;
    ++revision_;
}

void Graph::eraseParam(NodeId id, const std::string& key) {
    Node* node = const_cast<Node*>(findNode(id));
    if (node == nullptr) {
        throw GraphException(GraphError::UnknownNode, "cannot erase a parameter on unknown node " + std::to_string(id));
    }
    node->params.erase(key);
    ++revision_;
}

const std::vector<Edge>& Graph::edgesInto(NodeId node) const {
    auto it = incomingCache_.find(node);
    if (it == incomingCache_.end()) {
        std::vector<Edge> incoming;
        for (const auto& edge : edges_) {
            if (edge.to.node == node) {
                incoming.push_back(edge);
            }
        }
        it = incomingCache_.emplace(node, std::move(incoming)).first;
    }
    return it->second;
}

}  // namespace nemo
