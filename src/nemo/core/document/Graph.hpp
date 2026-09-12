#pragma once

#include "nemo/core/document/Ids.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nemo {

struct LayoutPosition {
    double x{0.0};
    double y{0.0};

    friend bool operator==(const LayoutPosition&, const LayoutPosition&) = default;
};

struct PortRef {
    NodeId node{kInvalidNode};
    std::uint32_t port{0};

    friend bool operator==(const PortRef&, const PortRef&) = default;
};

// A persistent node occurrence. Its identity is local to the owning Network.
// For a nested occurrence, definition/instance identify the referenced shared
// definition and the document-owned occurrence respectively. Unknown types
// are retained as data and have hasPortContract == false.
struct NodeInstance {
    NodeId id{kInvalidNode};
    std::string type;
    std::string name;
    ParameterValues params;
    LayoutPosition layout;
    NetworkId definition{kInvalidNetwork};
    NetworkInstanceId instance{kInvalidNetworkInstance};
    bool hasPortContract{false};
    std::vector<PortSpec> inputPorts{};
    std::vector<PortSpec> outputPorts{};
    // Authored fields of the persisted node object this build does not model,
    // retained verbatim by the codec so a load/save cycle loses nothing. Never
    // consulted by evaluation or commands.
    nlohmann::json extension{};
    // Parameter records of an unavailable node type that this build cannot
    // interpret as typed parameters. Re-emitted verbatim alongside the typed
    // parameters; a later typed edit of the same key wins.
    nlohmann::json opaqueParams{};
};

struct Edge {
    EdgeId id{kInvalidEdge};
    PortRef from;
    PortRef to;
    // Empty means no authored route; points are in graph-local coordinates.
    std::vector<LayoutPosition> route{};
    // Authored fields of the persisted edge this build does not model, retained
    // verbatim for lossless save. Never consulted by evaluation or commands.
    nlohmann::json extension{};
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
    DuplicateId,
    InvalidNetwork,
    UnknownNetwork,
    DuplicateNetwork,
    UnknownInstance,
    InvalidInstance,
    UnknownMediaEntry,
    UnknownMediaBin,
    MediaDuplicateName,
    MediaCycle,
    MediaSourceInUse,
    InvalidMediaMark,
    InvalidMediaQuery,
    MissingMediaSource,
    // A command carried an expected SourceReference that no longer matches the
    // document's current reference (path, revision, interpretation, or frame
    // mapping changed underneath it), so the edit would publish stale data.
    StaleMediaSource
};

struct GraphErrorDetails {
    GraphError code;
    std::string message;
};

class GraphException : public std::runtime_error {
public:
    GraphException(GraphError code, std::string message) : std::runtime_error(message), code(code) {}

    [[nodiscard]] GraphError errorCode() const { return code; }

private:
    GraphError code;
};

// A directed acyclic processing graph. Input ports accept one edge each;
// output ports may fan out. Node and edge identities are local to this graph,
// and their high-water marks are never lowered by deletion or restoration.
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
    // Inserts a persisted node with its exact identity. Reserved for
    // deserialization and command-history restoration.
    [[nodiscard]] NodeId addNodeWithId(NodeId id, std::string type, std::string name, ParameterValues params = {},
                                       LayoutPosition layout = {}, NetworkId definition = kInvalidNetwork,
                                       NetworkInstanceId instance = kInvalidNetworkInstance);
    // Attaches preserved authored JSON to a persisted node: fields this build
    // does not model and parameter records of an unavailable type. Reserved for
    // deserialization.
    void restoreNodeExtension(NodeId id, nlohmann::json extension, nlohmann::json opaqueParams);
    void removeNode(NodeId id);
    void renameNode(NodeId id, std::string name);

    // Sets a dynamic typed contract (used by a nested NetworkInstance). A
    // native/unknown node keeps the catalog/unknown contract when unset.
    void setPortContract(NodeId id, std::vector<PortSpec> inputs, std::vector<PortSpec> outputs);
    // Reserve a destination for a formal input terminal. Ordinary graph
    // connections then cannot bypass that terminal binding.
    void reserveInput(PortRef destination);
    void releaseInput(PortRef destination);
    void clearInputReservations(NodeId node);
    [[nodiscard]] bool inputReserved(PortRef destination) const;
    void setLayout(NodeId id, LayoutPosition position);
    [[nodiscard]] const NodeInstance* node(NodeId id) const;
    [[nodiscard]] const NodeInstance* nodeByName(const std::string& name) const;

    [[nodiscard]] EdgeId connectWithId(EdgeId id, PortRef from, PortRef to);
    // Inserts a persisted edge with its exact identity without port-contract
    // validation, so a connection authored against a node type this build does
    // not model survives a load. Endpoint existence, duplicate inputs and
    // cycles are still rejected. Reserved for deserialization.
    [[nodiscard]] EdgeId restoreEdgeWithId(EdgeId id, PortRef from, PortRef to);
    void restoreEdgeExtension(EdgeId id, nlohmann::json extension);
    [[nodiscard]] EdgeId connect(PortRef from, PortRef to);
    void disconnect(EdgeId id);
    void setRoute(EdgeId id, std::vector<LayoutPosition> route);
    [[nodiscard]] std::optional<GraphErrorDetails> validateEdge(PortRef from, PortRef to) const;
    // Incoming adjacency owned by this graph: every edge whose destination is
    // `node`, in insertion order. Empty for an unknown or unfed node.
    [[nodiscard]] const std::vector<Edge>& edgesInto(NodeId node) const;
    // True when `target` is reachable from `origin` by following directed
    // edges. Walks the graph-owned incoming adjacency backward from `target`
    // with query-local visited state, so each discovered node is expanded once.
    // An origin equal to target is always reachable, including unknown ids.
    [[nodiscard]] bool reachable(NodeId origin, NodeId target) const;
    [[nodiscard]] const std::vector<NodeInstance>& nodes() const { return nodes_; }
    [[nodiscard]] const std::vector<Edge>& edges() const { return edges_; }
    void setParam(NodeId id, const std::string& key, ParameterValue value);
    void eraseParam(NodeId id, const std::string& key);
    void restoreIdentityHighWatermarks(NodeId nextNodeId, EdgeId nextEdgeId);
    [[nodiscard]] NodeId nextNodeId() const { return nextNodeId_; }
    [[nodiscard]] EdgeId nextEdgeId() const { return nextEdgeId_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }

    [[nodiscard]] const std::vector<PortSpec>& inputPorts(NodeId id) const;
    [[nodiscard]] const std::vector<PortSpec>& outputPorts(NodeId id) const;

private:
    [[nodiscard]] const NodeInstance* findNode(NodeId id) const;
    // Mutable lookup for this graph's own mutation helpers; callers never
    // receive mutable node access.
    [[nodiscard]] NodeInstance* findNode(NodeId id);
    void eraseIncomingEdge(const Edge& edge) noexcept;
    [[nodiscard]] const std::vector<PortSpec>* declaredInputs(const NodeInstance& node) const;
    [[nodiscard]] const std::vector<PortSpec>* declaredOutputs(const NodeInstance& node) const;

    std::shared_ptr<const NodeCatalog> catalog_;
    std::vector<NodeInstance> nodes_;
    std::vector<Edge> edges_;
    std::map<NodeId, std::vector<Edge>> incomingCache_;
    std::vector<PortRef> reservedInputs_;
    NodeId nextNodeId_{1};
    EdgeId nextEdgeId_{1};
    std::uint64_t revision_{1};
};

enum class PortDirection { Input, Output };

// A named, typed network terminal. IDs are stable and local to the owning
// Network; names are display/query keys and may be changed only through an
// explicit authored edit.
struct FormalPort {
    InterfacePortId id{kInvalidInterfacePort};
    PortKind kind{PortKind::Image};
    std::string name;
    bool allowFanOut{true};
    // Authored fields of the persisted terminal this build does not model,
    // retained verbatim for lossless save. Never consulted by evaluation.
    nlohmann::json extension{};

    friend bool operator==(const FormalPort&, const FormalPort&) = default;
};

struct TerminalConnection {
    InterfacePortId terminal{kInvalidInterfacePort};
    PortRef node;

    friend bool operator==(const TerminalConnection&, const TerminalConnection&) = default;
};

// A persistent Composition Network definition. Graph topology is held once;
// NetworkInstance occurrences reference it without copying the definition.
class Network {
public:
    explicit Network(NetworkId id = kInvalidNetwork, std::string name = {},
                     std::shared_ptr<const NodeCatalog> catalog = builtinNodeCatalogPtr());

    [[nodiscard]] NetworkId id() const { return id_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    void rename(std::string name);
    [[nodiscard]] const Graph& graph() const { return graph_; }
    [[nodiscard]] Graph& graph() { return graph_; }
    [[nodiscard]] NodeId defaultOutput() const;
    void setDefaultOutput(NodeId output);

    [[nodiscard]] const std::vector<FormalPort>& inputs() const { return inputs_; }
    [[nodiscard]] const std::vector<FormalPort>& outputs() const { return outputs_; }
    [[nodiscard]] const FormalPort* input(InterfacePortId id) const;
    [[nodiscard]] const FormalPort* output(InterfacePortId id) const;
    [[nodiscard]] const FormalPort* input(std::string_view name) const;
    [[nodiscard]] const FormalPort* output(std::string_view name) const;
    [[nodiscard]] InterfacePortId addInput(std::string name, PortKind kind, InterfacePortId id = kInvalidInterfacePort,
                                           bool allowFanOut = true);
    [[nodiscard]] InterfacePortId addOutput(std::string name, PortKind kind, InterfacePortId id = kInvalidInterfacePort,
                                            bool allowFanOut = true);
    [[nodiscard]] InterfacePortId addFormalPort(PortDirection direction, std::string name, PortKind kind,
                                                InterfacePortId id = kInvalidInterfacePort, bool allowFanOut = true);

    // Formal input terminals may fan out to internal inputs. Each destination
    // remains subject to Graph's one-edge-per-input invariant.
    [[nodiscard]] std::optional<GraphErrorDetails> validateInputConnection(InterfacePortId input,
                                                                           PortRef destination) const;
    void connectInput(InterfacePortId input, PortRef destination);
    void disconnectInput(InterfacePortId input, PortRef destination);
    // A formal output has one source. Multiple outputs are represented by
    // multiple formal terminals, each independently selecting a source.
    [[nodiscard]] std::optional<GraphErrorDetails> validateOutputConnection(PortRef source,
                                                                            InterfacePortId output) const;
    void connectOutput(PortRef source, InterfacePortId output);
    void disconnectOutput(InterfacePortId output);
    [[nodiscard]] const std::vector<TerminalConnection>& inputConnections() const;
    [[nodiscard]] const std::vector<TerminalConnection>& outputConnections() const;

    void restoreIdentityHighWatermarks(NodeId nextNodeId, EdgeId nextEdgeId, InterfacePortId nextInterfacePortId);
    [[nodiscard]] InterfacePortId nextInterfacePortId() const { return nextInterfacePortId_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_ + graph_.revision(); }

    // Persisted fields of the network object this build does not model, retained
    // verbatim by the codec so a load/save cycle loses nothing.
    [[nodiscard]] const nlohmann::json& extension() const noexcept { return extension_; }
    void setExtension(nlohmann::json extension) { extension_ = std::move(extension); }
    // Attaches preserved authored JSON to a persisted formal terminal. Reserved
    // for deserialization.
    void restorePortExtension(PortDirection direction, InterfacePortId id, nlohmann::json extension);

private:
    friend struct Document;
    void syncTerminalConnections();
    [[nodiscard]] InterfacePortId addFormalPortImpl(PortDirection direction, std::string name, PortKind kind,
                                                    InterfacePortId id, bool allowFanOut);
    [[nodiscard]] const FormalPort* findPort(const std::vector<FormalPort>& ports, InterfacePortId id) const;
    [[nodiscard]] const FormalPort* findPort(const std::vector<FormalPort>& ports, std::string_view name) const;
    // Mutable lookup for this network's own mutation helpers; public terminal
    // lookup stays read-only.
    [[nodiscard]] FormalPort* findPort(std::vector<FormalPort>& ports, InterfacePortId id);

    NetworkId id_{kInvalidNetwork};
    std::string name_;
    Graph graph_;
    NodeId defaultOutput_{kInvalidNode};
    std::vector<FormalPort> inputs_;
    std::vector<FormalPort> outputs_;
    std::vector<TerminalConnection> inputConnections_;
    std::vector<TerminalConnection> outputConnections_;
    std::uint64_t terminalSyncRevision_{0};
    InterfacePortId nextInterfacePortId_{1};
    std::uint64_t revision_{1};
    nlohmann::json extension_{};
};

struct NetworkInstance {
    NetworkInstanceId id{kInvalidNetworkInstance};
    NetworkId parentNetwork{kInvalidNetwork};
    NetworkId definition{kInvalidNetwork};
    NodeId node{kInvalidNode};
    std::string name;
    std::map<InterfacePortId, PortRef> inputBindings;
    // Definition node identity -> authored parameter overrides. Keeping the
    // target node explicit prevents an instance-local key from being applied
    // to an unrelated node in a shared definition.
    std::map<NodeId, ParameterValues> params;
    // Authored fields of the persisted instance object this build does not
    // model, retained verbatim for lossless save.
    nlohmann::json extension{};
    // Raw parameter overrides for targets inside an unavailable definition type
    // that this build cannot interpret as typed parameters.
    std::map<NodeId, nlohmann::json> opaqueParams{};

    friend bool operator==(const NetworkInstance&, const NetworkInstance&) = default;
};

}  // namespace nemo
