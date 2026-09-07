#include "nemo/core/document/Graph.hpp"

#include <algorithm>
#include <utility>

namespace nemo {

namespace {

std::string describe(PortRef ref) {
    return "node " + std::to_string(ref.node) + " port " + std::to_string(ref.port);
}

} // namespace

const Node* Graph::findNode(NodeId id) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                 [id](const Node& n) { return n.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}

NodeId Graph::addNode(std::string type, std::string name) {
    for (const auto& existing : nodes_) {
        if (existing.name == name) {
            throw GraphException(GraphError::DuplicateName,
                                 "node name '" + name + "' already exists in this graph");
        }
    }
    const NodeId id = nextNodeId_++;
    nodes_.push_back(Node{.id = id, .type = std::move(type), .name = std::move(name), .params = {}});
    return id;
}

void Graph::removeNode(NodeId id) {
    if (!findNode(id)) {
        throw GraphException(GraphError::UnknownNode, "cannot remove unknown node " + std::to_string(id));
    }
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [id](const Edge& e) {
                                    return e.from.node == id || e.to.node == id;
                                }),
                 edges_.end());
    incomingCache_.erase(id);
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [id](const Node& n) { return n.id == id; }),
                 nodes_.end());
}

const Node* Graph::node(NodeId id) const { return findNode(id); }

Node* Graph::node(NodeId id) { return const_cast<Node*>(findNode(id)); }

const Node* Graph::nodeByName(const std::string& name) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                 [&name](const Node& n) { return n.name == name; });
    return it == nodes_.end() ? nullptr : &*it;
}

Node* Graph::nodeByName(const std::string& name) {
    return const_cast<Node*>(static_cast<const Graph*>(this)->nodeByName(name));
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
    if (!findNode(from.node) || !findNode(to.node)) {
        return GraphErrorDetails{GraphError::UnknownNode,
                                 "connect references an unknown node: " + describe(from) + " -> " + describe(to)};
    }
    for (const auto& edge : edges_) {
        if (edge.to == to) {
            return GraphErrorDetails{GraphError::PortOccupied,
                                     "input " + describe(to) + " is already fed by node " +
                                         std::to_string(edge.from.node)};
        }
    }
    if (reachable(to.node, from.node)) {
        return GraphErrorDetails{GraphError::Cycle,
                                 "connecting " + describe(from) + " -> " + describe(to) +
                                     " would create a circular dependency through node " +
                                     std::to_string(to.node)};
    }
    return std::nullopt;
}

EdgeId Graph::connect(PortRef from, PortRef to) {
    if (const auto problem = validateEdge(from, to)) {
        throw GraphException(problem->code, problem->message);
    }
    const EdgeId id = nextEdgeId_++;
    edges_.push_back(Edge{.id = id, .from = from, .to = to});
    incomingCache_.erase(to.node);
    return id;
}

void Graph::disconnect(EdgeId id) {
    const auto it = std::find_if(edges_.begin(), edges_.end(), [id](const Edge& e) { return e.id == id; });
    if (it == edges_.end()) {
        throw GraphException(GraphError::UnknownEdge, "cannot disconnect unknown edge " + std::to_string(id));
    }
    incomingCache_.erase(it->to.node);
    edges_.erase(it);
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

} // namespace nemo
