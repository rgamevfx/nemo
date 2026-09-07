#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Ids.hpp"

namespace nemo {

struct PortRef {
    NodeId node{kInvalidNode};
    std::uint32_t port{0};

    friend bool operator==(const PortRef&, const PortRef&) = default;
};

struct Node {
    NodeId id{kInvalidNode};
    std::string type;
    std::string name;
    std::map<std::string, std::string> params;
};

struct Edge {
    EdgeId id{kInvalidEdge};
    PortRef from;
    PortRef to;
};

// Rejected graph edits always explain the offending relationship.
enum class GraphError { UnknownNode, UnknownEdge, PortOccupied, Cycle, DuplicateName };

struct GraphErrorDetails {
    GraphError code;
    std::string message;
};

class GraphException : public std::runtime_error {
public:
    GraphException(GraphError code, std::string message) : std::runtime_error(std::move(message)), code(code) {}

    [[nodiscard]] GraphError errorCode() const { return code; }

private:
    GraphError code;
};

// A directed acyclic processing graph: the Composition Network model from
// spec section 3. Input ports accept one edge each; output ports may fan out.
// Node identities are never reused within a graph instance.
class Graph {
public:
    [[nodiscard]] NodeId addNode(std::string type, std::string name);
    // Detaches and removes the node together with every edge touching it.
    void removeNode(NodeId id);

    [[nodiscard]] const Node* node(NodeId id) const;
    [[nodiscard]] Node* node(NodeId id);

    [[nodiscard]] const Node* nodeByName(const std::string& name) const;
    [[nodiscard]] Node* nodeByName(const std::string& name);

    // Throws GraphException on UnknownNode, PortOccupied, or Cycle.
    [[nodiscard]] EdgeId connect(PortRef from, PortRef to);
    // Throws GraphException on UnknownEdge.
    void disconnect(EdgeId id);

    // Returns the offending relationship when the edge would be rejected.
    [[nodiscard]] std::optional<GraphErrorDetails> validateEdge(PortRef from, PortRef to) const;

    [[nodiscard]] const std::vector<Node>& nodes() const { return nodes_; }
    [[nodiscard]] const std::vector<Edge>& edges() const { return edges_; }
    [[nodiscard]] const std::vector<Edge>& edgesInto(NodeId node) const;

    // True when `target` is reachable from `origin` through existing edges.
    [[nodiscard]] bool reachable(NodeId origin, NodeId target) const;

private:
    [[nodiscard]] const Node* findNode(NodeId id) const;

    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    mutable std::map<NodeId, std::vector<Edge>> incomingCache_;
    NodeId nextNodeId_{1};
    EdgeId nextEdgeId_{1};
};

}  // namespace nemo
