#include "nemo/core/document/Graph.hpp"

#include <algorithm>
#include <utility>

namespace nemo {

namespace {

// The CPU reference inventory and any declared no-op node types live here so
// edge validation, serialization warnings, and the evaluator share one table.
struct NodeInterface {
    std::string type;
    std::vector<PortSpec> inputs;
    std::vector<PortSpec> outputs;
};

const std::vector<NodeInterface>& nodeInterfaces() {
    static const std::vector<NodeInterface> interfaces{
        {"constcolor", {}, {{PortKind::Color, "color"}}},
        {"merge",
         {{PortKind::Color, "A"}, {PortKind::Color, "B"}},  // A = over base, B = over source
         {{PortKind::Color, "out"}}},
        {"output", {{PortKind::Color, "color"}}, {}},
        {"testpattern", {}, {{PortKind::Color, "color"}}},
    };
    return interfaces;
}

const NodeInterface* interfaceOf(const std::string& type) {
    for (const auto& interface : nodeInterfaces()) {
        if (interface.type == type) {
            return &interface;
        }
    }
    return nullptr;
}

std::string describe(PortRef ref) {
    return "node " + std::to_string(ref.node) + " port " + std::to_string(ref.port);
}

}  // namespace

const std::vector<PortSpec>& inputPorts(const std::string& type) {
    static const std::vector<PortSpec> none;
    const NodeInterface* interface = interfaceOf(type);
    return interface ? interface->inputs : none;
}

const std::vector<PortSpec>& outputPorts(const std::string& type) {
    static const std::vector<PortSpec> none;
    const NodeInterface* interface = interfaceOf(type);
    return interface ? interface->outputs : none;
}

bool isKnownNodeType(const std::string& type) {
    return interfaceOf(type) != nullptr;
}

const Node* Graph::findNode(NodeId id) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [id](const Node& n) { return n.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}

NodeId Graph::addNode(std::string type, std::string name) {
    for (const auto& existing : nodes_) {
        if (existing.name == name) {
            throw GraphException(GraphError::DuplicateName, "node name '" + name + "' already exists in this graph");
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
                                [id](const Edge& e) { return e.from.node == id || e.to.node == id; }),
                 edges_.end());
    incomingCache_.erase(id);
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [id](const Node& n) { return n.id == id; }),
                 nodes_.end());
}

const Node* Graph::node(NodeId id) const {
    return findNode(id);
}

Node* Graph::node(NodeId id) {
    return const_cast<Node*>(findNode(id));
}

const Node* Graph::nodeByName(const std::string& name) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [&name](const Node& n) { return n.name == name; });
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
    const Node* fromNode = findNode(from.node);
    const Node* toNode = findNode(to.node);
    if (!fromNode || !toNode) {
        return GraphErrorDetails{GraphError::UnknownNode,
                                 "connect references an unknown node: " + describe(from) + " -> " + describe(to)};
    }
    // Typed ports: reject connections whose source is not a declared output
    // port, or whose destination is not a declared input port of the same
    // kind. Unknown node types declare no ports and are not type-checked
    // (spec section 10.7 recovery rule).
    const NodeInterface* fromInterface = interfaceOf(fromNode->type);
    const NodeInterface* toInterface = interfaceOf(toNode->type);
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

}  // namespace nemo
