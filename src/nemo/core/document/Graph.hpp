#pragma once

#include "nemo/core/document/Ids.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Rejected graph edits always explain the offending relationship or value.
enum class GraphError {
    UnknownNode,
    UnknownEdge,
    PortOccupied,
    Cycle,
    DuplicateName,
    InvalidName,
    PortType,
    ParameterValue,
    InvalidId,
    DuplicateId
};

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
    explicit Graph(std::shared_ptr<const NodeCatalog> catalog = builtinNodeCatalogPtr());
    [[nodiscard]] const NodeCatalog& catalog() const { return *catalog_; }
    [[nodiscard]] const NodeDescriptor* descriptor(std::string_view type) const { return catalog_->find(type); }
    [[nodiscard]] const std::vector<PortSpec>& inputPortsFor(std::string_view type) const {
        return catalog_->inputPorts(type);
    }
    [[nodiscard]] std::span<const int> samplingScalesFor(std::string_view type) const {
        return catalog_->samplingScalesSupported(type);
    }
    [[nodiscard]] NodeId addNode(std::string type, std::string name);
    // Inserts a persisted node with its exact identity. This is reserved for
    // deserialization and command history restoration; regular creation uses
    // addNode and always allocates above the graph high watermark.
    [[nodiscard]] NodeId addNodeWithId(NodeId id, std::string type, std::string name,
                                       std::map<std::string, std::string> params = {});
    // Detaches and removes the node together with every edge touching it.
    void removeNode(NodeId id);
    // Changes the display label without changing the node identity.
    void renameNode(NodeId id, std::string name);

    [[nodiscard]] const Node* node(NodeId id) const;
    [[nodiscard]] const Node* nodeByName(const std::string& name) const;

    // Inserts a persisted edge with its exact identity.
    [[nodiscard]] EdgeId connectWithId(EdgeId id, PortRef from, PortRef to);
    // Throws GraphException on UnknownNode, PortOccupied, or Cycle.
    [[nodiscard]] EdgeId connect(PortRef from, PortRef to);
    // Throws GraphException on UnknownEdge.
    void disconnect(EdgeId id);
    [[nodiscard]] std::optional<GraphErrorDetails> validateEdge(PortRef from, PortRef to) const;
    [[nodiscard]] const std::vector<Edge>& edgesInto(NodeId node) const;
    [[nodiscard]] bool reachable(NodeId origin, NodeId target) const;
    [[nodiscard]] const std::vector<Node>& nodes() const { return nodes_; }
    [[nodiscard]] const std::vector<Edge>& edges() const { return edges_; }

    // Throws GraphException::ParameterValue for invalid declared values;
    // unknown authored keys remain recoverable data.
    void setParam(NodeId id, const std::string& key, const std::string& value);
    void eraseParam(NodeId id, const std::string& key);
    // Identity allocation watermarks are persisted separately from live
    // objects so deleting the newest object can never make its ID reusable.
    void restoreIdentityHighWatermarks(NodeId nextNodeId, EdgeId nextEdgeId);
    [[nodiscard]] NodeId nextNodeId() const { return nextNodeId_; }
    [[nodiscard]] EdgeId nextEdgeId() const { return nextEdgeId_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }

private:
    [[nodiscard]] const Node* findNode(NodeId id) const;

    std::shared_ptr<const NodeCatalog> catalog_;

    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    mutable std::map<NodeId, std::vector<Edge>> incomingCache_;
    NodeId nextNodeId_{1};
    EdgeId nextEdgeId_{1};
    std::uint64_t revision_{1};
};

}  // namespace nemo
