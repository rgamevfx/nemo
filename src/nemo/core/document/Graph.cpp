#include "nemo/core/document/Graph.hpp"
#include "nemo/core/evaluation/Request.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace nemo {
namespace {

std::string describe(PortRef ref) {
    return "node " + std::to_string(ref.node) + " port " + std::to_string(ref.port);
}

const std::vector<PortSpec>& emptyPorts() {
    static const std::vector<PortSpec> none;
    return none;
}

void validatePortSpecs(const std::vector<PortSpec>& ports, const char* direction) {
    std::set<std::string> names;
    for (const auto& port : ports) {
        if (port.kind != PortKind::Image && port.kind != PortKind::Mask && port.kind != PortKind::Media)
            throw GraphException(GraphError::PortType, std::string(direction) + " port has an invalid kind");
        if (port.name.empty())
            throw GraphException(GraphError::InvalidName, std::string(direction) + " port name must not be empty");
        if (!names.insert(port.name).second)
            throw GraphException(GraphError::DuplicateName,
                                 std::string(direction) + " port name '" + port.name + "' is duplicated");
    }
}

}  // namespace

Graph::Graph(std::shared_ptr<const NodeCatalog> catalog) : catalog_(std::move(catalog)) {
    if (!catalog_)
        throw std::invalid_argument("graph catalog must not be null");
}

void Graph::restoreIdentityHighWatermarks(NodeId nextNodeId, EdgeId nextEdgeId) {
    if (nextNodeId == kInvalidNode || nextEdgeId == kInvalidEdge)
        throw GraphException(GraphError::InvalidId, "identity high watermarks must be nonzero");
    nextNodeId_ = std::max(nextNodeId_, nextNodeId);
    nextEdgeId_ = std::max(nextEdgeId_, nextEdgeId);
}

const NodeInstance* Graph::findNode(NodeId id) const {
    const std::size_t index = nodeIndexOf(id);
    return index == nodes_.size() ? nullptr : &nodes_[index];
}

NodeInstance* Graph::mutableNode(NodeId id, std::size_t& index) {
    index = nodeIndexOf(id);
    return index == nodes_.size() ? nullptr : &nodes_.mutableAt(index);
}

std::size_t Graph::cacheLowerBound(NodeId id) const {
    std::size_t low = 0;
    std::size_t high = incomingCache_.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (incomingCache_[middle].first < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

std::size_t Graph::cacheIndexOf(NodeId id) const {
    const std::size_t position = cacheLowerBound(id);
    if (position < incomingCache_.size() && incomingCache_[position].first == id)
        return position;
    return incomingCache_.size();
}

void Graph::recordNode(NodeId id) {
    if (recorder_)
        recorder_->node(recorderNetwork_, id);
}

void Graph::recordEdge(EdgeId id) {
    if (recorder_)
        recorder_->edge(recorderNetwork_, id);
}

void Graph::recordGraph() {
    if (recorder_)
        recorder_->network(recorderNetwork_);
}

EdgeId Graph::appendEdge(EdgeId id, PortRef from, PortRef to) {
    edges_.push_back(Edge{.id = id, .from = from, .to = to});
    try {
        addEdgeToCache(to.node, edges_[edges_.size() - 1]);
    } catch (...) {
        edges_.pop_back();
        throw;
    }
    nextEdgeId_ = std::max(nextEdgeId_, static_cast<EdgeId>(id + 1));
    ++revision_;
    recordEdge(id);
    return id;
}

void Graph::addEdgeToCache(NodeId destination, const Edge& edge) {
    const std::size_t index = cacheLowerBound(destination);
    if (index < incomingCache_.size() && incomingCache_[index].first == destination) {
        incomingCache_.mutableAt(index).second.push_back(edge);
        return;
    }
    EdgeStorage list;
    list.push_back(edge);
    incomingCache_.insert(index, IncomingEntry{destination, std::move(list)});
}

void Graph::removeEdgeFromCache(const Edge& edge) {
    const std::size_t index = cacheIndexOf(edge.to.node);
    if (index == incomingCache_.size())
        return;
    EdgeStorage& into = incomingCache_.mutableAt(index).second;
    const std::size_t position = into.indexOf([&edge](const Edge& candidate) { return candidate.id == edge.id; });
    if (position == into.size())
        return;
    into.erase(position);
    if (into.empty())
        incomingCache_.erase(index);
}

NodeId Graph::addNode(std::string type, std::string name) {
    if (nextNodeId_ == kInvalidNode || nextNodeId_ == std::numeric_limits<NodeId>::max())
        throw GraphException(GraphError::InvalidId, "node identity space is exhausted");
    return addNodeWithId(nextNodeId_, std::move(type), std::move(name));
}

NodeId Graph::addNodeWithId(NodeId id, std::string type, std::string name, ParameterValues params,
                            LayoutPosition layout, NetworkId definition, NetworkInstanceId instance) {
    if (id == kInvalidNode || id == std::numeric_limits<NodeId>::max())
        throw GraphException(GraphError::InvalidId, "node id must be a nonzero value below the identity limit");
    if (findNode(id) != nullptr)
        throw GraphException(GraphError::DuplicateId,
                             "node id " + std::to_string(id) + " already exists in this graph");
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "node name must not be empty");
    if (nodeByName(name) != nullptr)
        throw GraphException(GraphError::DuplicateName, "node name '" + name + "' already exists in this graph");
    if (definition == kInvalidNetwork && instance != kInvalidNetworkInstance)
        throw GraphException(GraphError::InvalidInstance, "a node instance requires a network definition");
    if (definition != kInvalidNetwork && instance == kInvalidNetworkInstance)
        throw GraphException(GraphError::InvalidInstance, "a network definition requires a node instance identity");
    for (const auto& [key, value] : params) {
        if (const auto problem = catalog_->validateParameter(type, key, value))
            throw GraphException(GraphError::ParameterValue,
                                 "node '" + name + "' parameter '" + key + "': " + *problem);
    }
    nodes_.push_back(NodeInstance{.id = id,
                                  .type = std::move(type),
                                  .name = std::move(name),
                                  .params = std::move(params),
                                  .layout = layout,
                                  .definition = definition,
                                  .instance = instance});
    nextNodeId_ = std::max(nextNodeId_, static_cast<NodeId>(id + 1));
    ++revision_;
    recordNode(id);
    return id;
}

void Graph::restoreNodeExtension(NodeId id, nlohmann::json extension, nlohmann::json opaqueParams) {
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode,
                             "cannot attach preserved data to unknown node " + std::to_string(id));
    node->extension = std::move(extension);
    node->opaqueParams = std::move(opaqueParams);
}

void Graph::setRoto(NodeId id, RotoData roto) {
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode, "cannot set roto data on unknown node " + std::to_string(id));
    if (const auto problem = validateRotoData(roto))
        throw GraphException(GraphError::InvalidRoto, "node '" + node->name + "' roto data: " + *problem);
    if (node->roto && rotoContentEquals(*node->roto, roto))
        return;
    node->roto = std::make_shared<const RotoData>(std::move(roto));
    ++revision_;
    recordNode(id);
}

void Graph::renameNode(NodeId id, std::string name) {
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "node name must not be empty");
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode, "cannot rename unknown node " + std::to_string(id));
    if (const NodeInstance* existing = nodeByName(name); existing != nullptr && existing->id != id)
        throw GraphException(GraphError::DuplicateName, "node name '" + name + "' already exists in this graph");
    if (node->name == name)
        return;
    node->name = std::move(name);
    ++revision_;
    recordNode(id);
}
void Graph::setInstanceDefinition(NodeId id, NetworkId definition, NetworkInstanceId instance) {
    std::size_t index = 0;
    NodeInstance* target = mutableNode(id, index);
    if (!target)
        throw GraphException(GraphError::UnknownNode, "cannot update definition on unknown node " + std::to_string(id));
    if (definition == kInvalidNetwork || instance == kInvalidNetworkInstance)
        throw GraphException(GraphError::InvalidInstance, "an occurrence requires definition and instance identities");
    if (target->definition == definition && target->instance == instance)
        return;
    target->definition = definition;
    target->instance = instance;
    ++revision_;
    recordNode(id);
}

void Graph::removeNode(NodeId id) {
    const std::size_t nodeIndex = nodeIndexOf(id);
    if (nodeIndex == nodes_.size())
        throw GraphException(GraphError::UnknownNode, "cannot remove unknown node " + std::to_string(id));
    // Incident edges leave with the node; each removed edge is a touched
    // identity, so publication reports exactly the relationships that died.
    std::vector<EdgeId> incident;
    for (const Edge& edge : edges_)
        if (edge.from.node == id || edge.to.node == id)
            incident.push_back(edge.id);
    for (const EdgeId edgeId : incident) {
        const std::size_t edgeIndex = edges_.indexOf([edgeId](const Edge& e) { return e.id == edgeId; });
        removeEdgeFromCache(edges_[edgeIndex]);
        edges_.erase(edgeIndex);
        recordEdge(edgeId);
    }
    reservedInputs_.eraseIf([id](PortRef ref) { return ref.node == id; });
    nodes_.erase(nodeIndex);
    ++revision_;
    recordNode(id);
}

void Graph::setPortContract(NodeId id, std::vector<PortSpec> inputs, std::vector<PortSpec> outputs) {
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode,
                             "cannot set a port contract on unknown node " + std::to_string(id));
    validatePortSpecs(inputs, "input");
    validatePortSpecs(outputs, "output");
    // A contract change cannot invalidate authored wires. Validate all
    // existing touching edges against the proposed contract before publishing.
    for (const auto& edge : edges_) {
        if (edge.from.node == id && edge.from.port >= outputs.size())
            throw GraphException(GraphError::PortType, "port contract removes output endpoint " + describe(edge.from));
        if (edge.to.node == id && edge.to.port >= inputs.size())
            throw GraphException(GraphError::PortType, "port contract removes input endpoint " + describe(edge.to));
        const auto& sourcePorts = edge.from.node == id ? outputs : outputPorts(edge.from.node);
        const auto& destinationPorts = edge.to.node == id ? inputs : inputPorts(edge.to.node);
        if (edge.from.port >= sourcePorts.size() || edge.to.port >= destinationPorts.size() ||
            !portKindsCompatible(sourcePorts[edge.from.port].kind, destinationPorts[edge.to.port].kind))
            throw GraphException(GraphError::PortType, "port contract would invalidate edge " +
                                                           std::to_string(edge.id) + " (" + describe(edge.from) +
                                                           " -> " + describe(edge.to) + ")");
    }
    node->inputPorts = std::move(inputs);
    node->outputPorts = std::move(outputs);
    node->hasPortContract = true;
    ++revision_;
    recordNode(id);
}

void Graph::reserveInput(PortRef destination) {
    if (!findNode(destination.node))
        throw GraphException(GraphError::UnknownNode, "cannot reserve input on unknown " + describe(destination));
    if (inputReserved(destination))
        throw GraphException(GraphError::PortOccupied, "input " + describe(destination) + " is already reserved");
    if (edges_.find([destination](const Edge& edge) { return edge.to == destination; }) != nullptr)
        throw GraphException(GraphError::PortOccupied, "input " + describe(destination) + " is already occupied");
    reservedInputs_.push_back(destination);
    ++revision_;
    recordGraph();
}

void Graph::releaseInput(PortRef destination) {
    const std::size_t index = reservedInputs_.indexOf([destination](PortRef ref) { return ref == destination; });
    if (index == reservedInputs_.size())
        throw GraphException(GraphError::UnknownEdge, "input reservation does not exist for " + describe(destination));
    reservedInputs_.erase(index);
    ++revision_;
    recordGraph();
}
void Graph::clearInputReservations(NodeId node) {
    const std::size_t oldSize = reservedInputs_.size();
    reservedInputs_.eraseIf([node](PortRef ref) { return ref.node == node; });
    if (reservedInputs_.size() != oldSize) {
        ++revision_;
        recordGraph();
    }
}

bool Graph::inputReserved(PortRef destination) const {
    return reservedInputs_.find([destination](PortRef ref) { return ref == destination; }) != nullptr;
}

void Graph::setLayout(NodeId id, LayoutPosition position) {
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode, "cannot position unknown node " + std::to_string(id));
    if (node->layout == position)
        return;
    node->layout = position;
    ++revision_;
    recordNode(id);
}

const NodeInstance* Graph::node(NodeId id) const {
    return findNode(id);
}

const NodeInstance* Graph::nodeByName(const std::string& name) const {
    const auto it =
        std::find_if(nodes_.begin(), nodes_.end(), [&name](const NodeInstance& n) { return n.name == name; });
    return it == nodes_.end() ? nullptr : &*it;
}

const std::vector<PortSpec>* Graph::declaredInputs(const NodeInstance& node) const {
    if (node.hasPortContract)
        return &node.inputPorts;
    const NodeDescriptor* schema = catalog_->find(node.type);
    return schema == nullptr ? nullptr : &schema->inputs;
}

const std::vector<PortSpec>* Graph::declaredOutputs(const NodeInstance& node) const {
    if (node.hasPortContract)
        return &node.outputPorts;
    const NodeDescriptor* schema = catalog_->find(node.type);
    return schema == nullptr ? nullptr : &schema->outputs;
}

const std::vector<PortSpec>& Graph::inputPorts(NodeId id) const {
    const NodeInstance* node = findNode(id);
    if (!node)
        return emptyPorts();
    const auto* ports = declaredInputs(*node);
    return ports ? *ports : emptyPorts();
}

const std::vector<PortSpec>& Graph::outputPorts(NodeId id) const {
    const NodeInstance* node = findNode(id);
    if (!node)
        return emptyPorts();
    const auto* ports = declaredOutputs(*node);
    return ports ? *ports : emptyPorts();
}

bool Graph::reachable(NodeId origin, NodeId target) const {
    if (origin == target)
        return true;
    // Reverse traversal over the graph-owned incoming adjacency. Both the
    // worklist and the visited set are query-local and sized by discovered
    // nodes, never by an identity high-water mark: identities are sparse.
    std::vector<NodeId> worklist{target};
    std::set<NodeId> discovered{target};
    while (!worklist.empty()) {
        const NodeId current = worklist.back();
        worklist.pop_back();
        for (const auto& edge : edgesInto(current)) {
            if (edge.from.node == origin)
                return true;
            if (discovered.insert(edge.from.node).second)
                worklist.push_back(edge.from.node);
        }
    }
    return false;
}

std::optional<GraphErrorDetails> Graph::validateEdge(PortRef from, PortRef to) const {
    const NodeInstance* fromNode = findNode(from.node);
    const NodeInstance* toNode = findNode(to.node);
    if (!fromNode || !toNode)
        return GraphErrorDetails{GraphError::UnknownNode,
                                 "connect references an unknown node: " + describe(from) + " -> " + describe(to)};
    if (toNode->instance != kInvalidNetworkInstance)
        return GraphErrorDetails{GraphError::InvalidInstance, "nested instance input " + describe(to) +
                                                                  " must be edited through its explicit binding"};

    const auto* fromPorts = declaredOutputs(*fromNode);
    const auto* toPorts = declaredInputs(*toNode);
    if (!fromPorts)
        return GraphErrorDetails{GraphError::PortType, "cannot connect from " + describe(from) + ": node '" +
                                                           fromNode->name + "' of unknown type '" + fromNode->type +
                                                           "' has no port contract"};
    if (!toPorts)
        return GraphErrorDetails{GraphError::PortType, "cannot connect into " + describe(to) + ": node '" +
                                                           toNode->name + "' of unknown type '" + toNode->type +
                                                           "' has no port contract"};
    if (static_cast<std::size_t>(from.port) >= fromPorts->size())
        return GraphErrorDetails{GraphError::PortType, "cannot connect from " + describe(from) + ": node '" +
                                                           fromNode->name + "' declares " +
                                                           std::to_string(fromPorts->size()) + " output port(s)"};
    if (static_cast<std::size_t>(to.port) >= toPorts->size())
        return GraphErrorDetails{GraphError::PortType, "cannot connect into " + describe(to) + ": node '" +
                                                           toNode->name + "' declares " +
                                                           std::to_string(toPorts->size()) + " input port(s)"};
    if (!portKindsCompatible((*fromPorts)[from.port].kind, (*toPorts)[to.port].kind))
        return GraphErrorDetails{GraphError::PortType,
                                 "cannot connect " + describe(from) + " -> " + describe(to) + ": port kind " +
                                     std::to_string(static_cast<int>((*fromPorts)[from.port].kind)) +
                                     " is not compatible with port kind " +
                                     std::to_string(static_cast<int>((*toPorts)[to.port].kind))};
    for (const auto& edge : edges_) {
        if (edge.to == to)
            return GraphErrorDetails{GraphError::PortOccupied, "input " + describe(to) + " is already fed by node " +
                                                                   std::to_string(edge.from.node)};
    }
    if (inputReserved(to))
        return GraphErrorDetails{GraphError::PortOccupied,
                                 "input " + describe(to) + " is reserved by a formal network terminal"};
    if (reachable(to.node, from.node))
        return GraphErrorDetails{GraphError::Cycle, "connecting " + describe(from) + " -> " + describe(to) +
                                                        " would create a circular dependency through node " +
                                                        std::to_string(to.node)};
    return std::nullopt;
}

EdgeId Graph::connect(PortRef from, PortRef to) {
    if (nextEdgeId_ == kInvalidEdge || nextEdgeId_ == std::numeric_limits<EdgeId>::max())
        throw GraphException(GraphError::InvalidId, "edge identity space is exhausted");
    return connectWithId(nextEdgeId_, from, to);
}

EdgeId Graph::connectWithId(EdgeId id, PortRef from, PortRef to) {
    if (id == kInvalidEdge || id == std::numeric_limits<EdgeId>::max())
        throw GraphException(GraphError::InvalidId, "edge id must be a nonzero value below the identity limit");
    if (edges_.find([id](const Edge& edge) { return edge.id == id; }) != nullptr)
        throw GraphException(GraphError::DuplicateId,
                             "edge id " + std::to_string(id) + " already exists in this graph");
    if (const auto problem = validateEdge(from, to))
        throw GraphException(problem->code, problem->message);

    return appendEdge(id, from, to);
}

EdgeId Graph::restoreEdgeWithId(EdgeId id, PortRef from, PortRef to) {
    if (id == kInvalidEdge || id == std::numeric_limits<EdgeId>::max())
        throw GraphException(GraphError::InvalidId, "edge id must be a nonzero value below the identity limit");
    if (edges_.find([id](const Edge& edge) { return edge.id == id; }) != nullptr)
        throw GraphException(GraphError::DuplicateId,
                             "edge id " + std::to_string(id) + " already exists in this graph");
    const NodeInstance* fromNode = findNode(from.node);
    const NodeInstance* toNode = findNode(to.node);
    if (!fromNode || !toNode)
        throw GraphException(GraphError::UnknownNode,
                             "cannot restore edge " + std::to_string(id) + ": endpoint node is not present");
    if (toNode->instance != kInvalidNetworkInstance)
        throw GraphException(GraphError::InvalidInstance,
                             "nested instance input " + describe(to) + " must be edited through its explicit binding");
    for (const auto& edge : edges_)
        if (edge.to == to)
            throw GraphException(GraphError::PortOccupied,
                                 "input " + describe(to) + " is already fed by node " + std::to_string(edge.from.node));
    if (reachable(to.node, from.node))
        throw GraphException(GraphError::Cycle, "restoring edge " + std::to_string(id) +
                                                    " would create a circular dependency through node " +
                                                    std::to_string(to.node));

    return appendEdge(id, from, to);
}

void Graph::restoreEdgeExtension(EdgeId id, nlohmann::json extension) {
    const std::size_t index = edges_.indexOf([id](const Edge& edge) { return edge.id == id; });
    if (index == edges_.size())
        throw GraphException(GraphError::UnknownEdge,
                             "cannot attach preserved data to unknown edge " + std::to_string(id));
    edges_.mutableAt(index).extension = std::move(extension);
    const std::size_t cacheIndex = cacheIndexOf(edges_[index].to.node);
    if (cacheIndex == incomingCache_.size())
        return;
    EdgeStorage& into = incomingCache_.mutableAt(cacheIndex).second;
    const std::size_t cached = into.indexOf([id](const Edge& edge) { return edge.id == id; });
    if (cached != into.size())
        into.mutableAt(cached).extension = edges_[index].extension;
}

void Graph::disconnect(EdgeId id) {
    const std::size_t index = edges_.indexOf([id](const Edge& edge) { return edge.id == id; });
    if (index == edges_.size())
        throw GraphException(GraphError::UnknownEdge, "cannot disconnect unknown edge " + std::to_string(id));
    const Edge removed = edges_[index];
    removeEdgeFromCache(removed);
    edges_.erase(index);
    ++revision_;
    recordEdge(id);
}

void Graph::setRoute(EdgeId id, std::vector<LayoutPosition> route) {
    const std::size_t index = edges_.indexOf([id](const Edge& edge) { return edge.id == id; });
    if (index == edges_.size())
        throw GraphException(GraphError::UnknownEdge, "cannot route unknown edge " + std::to_string(id));
    if (edges_[index].route == route)
        return;
    const std::size_t cacheIndex = cacheIndexOf(edges_[index].to.node);
    if (cacheIndex != incomingCache_.size()) {
        EdgeStorage& into = incomingCache_.mutableAt(cacheIndex).second;
        const std::size_t cached = into.indexOf([id](const Edge& edge) { return edge.id == id; });
        if (cached != into.size())
            into.mutableAt(cached).route = route;
    }
    edges_.mutableAt(index).route = std::move(route);
    ++revision_;
    recordEdge(id);
}

void Graph::setParam(NodeId id, const std::string& key, ParameterValue value) {
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode, "cannot set a parameter on unknown node " + std::to_string(id));
    if (const auto problem = catalog_->validateParameter(node->type, key, value))
        throw GraphException(GraphError::ParameterValue,
                             "node '" + node->name + "' parameter '" + key + "': " + *problem);
    node->params[key] = std::move(value);
    ++revision_;
    recordNode(id);
}

void Graph::eraseParam(NodeId id, const std::string& key) {
    std::size_t index = 0;
    NodeInstance* node = mutableNode(id, index);
    if (node == nullptr)
        throw GraphException(GraphError::UnknownNode, "cannot erase a parameter on unknown node " + std::to_string(id));
    node->params.erase(key);
    ++revision_;
    recordNode(id);
}

const Graph::EdgeStorage& Graph::edgesInto(NodeId node) const {
    static const EdgeStorage empty;
    const std::size_t index = cacheIndexOf(node);
    return index == incomingCache_.size() ? empty : incomingCache_[index].second;
}

Network::Network(NetworkId id, std::string name, std::shared_ptr<const NodeCatalog> catalog)
    : id_(id), name_(std::move(name)), graph_(std::move(catalog)) {
    if (id_ == kInvalidNetwork)
        throw GraphException(GraphError::InvalidNetwork, "network id must be nonzero");
    if (name_.empty())
        throw GraphException(GraphError::InvalidName, "network name must not be empty");
    defaultOutput_ = graph_.addNode("output", "Output");
}

std::optional<std::string> validateImageFormat(const ImageFormat& format, std::string_view context) {
    const std::string prefix = context.empty() ? std::string{} : std::string(context) + ": ";
    if (format.width <= 0 || format.height <= 0)
        return prefix + "image format must have positive width and height, got " + std::to_string(format.width) + "x" +
               std::to_string(format.height);
    if (!std::isfinite(format.pixelAspect) || format.pixelAspect <= 0.0F)
        return prefix + "image format pixel aspect must be finite and positive, got " +
               std::to_string(format.pixelAspect);
    return std::nullopt;
}

void Network::setFormat(ImageFormat format) {
    if (const auto problem = validateImageFormat(format, "network " + std::to_string(id_)))
        throw GraphException(GraphError::InvalidImageFormat, *problem);
    if (format_ == format)
        return;
    format_ = std::move(format);
    ++revision_;
    record();
}

void Network::rename(std::string name) {
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "network name must not be empty");
    if (name_ == name)
        return;
    name_ = std::move(name);
    ++revision_;
    record();
}

NodeId Network::defaultOutput() const {
    const NodeInstance* selected = graph_.node(defaultOutput_);
    const NodeDescriptor* descriptor = selected ? graph_.descriptor(selected->type) : nullptr;
    return descriptor && descriptor->isOutput ? defaultOutput_ : kInvalidNode;
}

void Network::setDefaultOutput(NodeId output) {
    const NodeInstance* node = graph_.node(output);
    if (!node)
        throw GraphException(GraphError::UnknownNode,
                             "cannot select unknown default output node " + std::to_string(output));
    const auto* descriptor = graph_.descriptor(node->type);
    if (!descriptor || !descriptor->isOutput)
        throw GraphException(GraphError::PortType,
                             "default output must identify an Output node (node " + std::to_string(output) + ")");
    if (defaultOutput_ == output)
        return;
    defaultOutput_ = output;
    ++revision_;
    record();
}

const FormalPort* Network::findPort(const std::vector<FormalPort>& ports, InterfacePortId id) const {
    const auto it = std::find_if(ports.begin(), ports.end(), [id](const FormalPort& port) { return port.id == id; });
    return it == ports.end() ? nullptr : &*it;
}

const FormalPort* Network::findPort(const std::vector<FormalPort>& ports, std::string_view name) const {
    const auto it =
        std::find_if(ports.begin(), ports.end(), [name](const FormalPort& port) { return port.name == name; });
    return it == ports.end() ? nullptr : &*it;
}

FormalPort* Network::findPort(std::vector<FormalPort>& ports, InterfacePortId id) {
    const auto it = std::find_if(ports.begin(), ports.end(), [id](const FormalPort& port) { return port.id == id; });
    return it == ports.end() ? nullptr : &*it;
}

const ExposedParameter* Network::findExposedParameter(InterfacePortId id) const {
    const auto it = std::find_if(exposedParameters_.begin(), exposedParameters_.end(),
                                 [id](const ExposedParameter& value) { return value.id == id; });
    return it == exposedParameters_.end() ? nullptr : &*it;
}

ExposedParameter* Network::findExposedParameter(InterfacePortId id) {
    const auto it = std::find_if(exposedParameters_.begin(), exposedParameters_.end(),
                                 [id](const ExposedParameter& value) { return value.id == id; });
    return it == exposedParameters_.end() ? nullptr : &*it;
}

const FormalPort* Network::input(InterfacePortId id) const {
    return findPort(inputs_, id);
}
const FormalPort* Network::output(InterfacePortId id) const {
    return findPort(outputs_, id);
}
const FormalPort* Network::input(std::string_view name) const {
    return findPort(inputs_, name);
}
const FormalPort* Network::output(std::string_view name) const {
    return findPort(outputs_, name);
}
InterfacePortId Network::addFormalPortImpl(PortDirection direction, std::string name, PortKind kind, InterfacePortId id,
                                           bool allowFanOut) {
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "formal port name must not be empty");
    auto& ports = direction == PortDirection::Input ? inputs_ : outputs_;
    if (findPort(ports, name) != nullptr)
        throw GraphException(GraphError::DuplicateName,
                             "formal port name '" + name + "' already exists in network '" + name_ + "'");
    if (id == kInvalidInterfacePort) {
        if (nextInterfacePortId_ == std::numeric_limits<InterfacePortId>::max())
            throw GraphException(GraphError::InvalidId, "formal port identity space is exhausted");
        id = nextInterfacePortId_;
    }
    if (id == kInvalidInterfacePort || id == std::numeric_limits<InterfacePortId>::max() ||
        findPort(inputs_, id) != nullptr || findPort(outputs_, id) != nullptr)
        throw GraphException(GraphError::DuplicateId, "formal port id " + std::to_string(id) + " is already in use");
    ports.push_back(FormalPort{.id = id, .kind = kind, .name = std::move(name), .allowFanOut = allowFanOut});
    nextInterfacePortId_ = std::max(nextInterfacePortId_, static_cast<InterfacePortId>(id + 1));
    ++revision_;
    record();
    return id;
}

InterfacePortId Network::addInput(std::string name, PortKind kind, InterfacePortId id, bool allowFanOut) {
    return addFormalPortImpl(PortDirection::Input, std::move(name), kind, id, allowFanOut);
}

InterfacePortId Network::addOutput(std::string name, PortKind kind, InterfacePortId id, bool allowFanOut) {
    return addFormalPortImpl(PortDirection::Output, std::move(name), kind, id, allowFanOut);
}

InterfacePortId Network::addFormalPort(PortDirection direction, std::string name, PortKind kind, InterfacePortId id,
                                       bool allowFanOut) {
    return addFormalPortImpl(direction, std::move(name), kind, id, allowFanOut);
}
void Network::renameFormalPort(PortDirection direction, InterfacePortId id, std::string name) {
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "formal port name must not be empty");
    auto& ports = direction == PortDirection::Input ? inputs_ : outputs_;
    FormalPort* target = findPort(ports, id);
    if (!target)
        throw GraphException(GraphError::UnknownEdge, "cannot rename unknown formal port " + std::to_string(id));
    if (std::any_of(ports.begin(), ports.end(),
                    [&](const FormalPort& candidate) { return candidate.id != id && candidate.name == name; }))
        throw GraphException(GraphError::DuplicateName, "formal port name '" + name + "' is already in use");
    if (target->name == name)
        return;
    target->name = std::move(name);
    ++revision_;
}

void Network::setFormalPortLayout(PortDirection direction, InterfacePortId id, LayoutPosition layout) {
    auto& ports = direction == PortDirection::Input ? inputs_ : outputs_;
    FormalPort* target = findPort(ports, id);
    if (!target)
        throw GraphException(GraphError::UnknownEdge, "cannot position unknown formal port " + std::to_string(id));
    if (target->layout == layout)
        return;
    target->layout = layout;
    ++revision_;
}

InterfacePortId Network::addExposedParameter(NodeId node, std::string key, std::string name, InterfacePortId id) {
    if (!graph_.node(node))
        throw GraphException(GraphError::UnknownNode,
                             "cannot expose parameter on unknown node " + std::to_string(node));
    if (key.empty() || name.empty())
        throw GraphException(GraphError::InvalidName, "exposed parameter key and name must not be empty");
    if (!graph_.catalog().parameterSpec(graph_.node(node)->type, key))
        throw GraphException(GraphError::ParameterValue,
                             "node '" + std::to_string(node) + "' has no parameter '" + key + "'");
    if (std::any_of(exposedParameters_.begin(), exposedParameters_.end(),
                    [&](const ExposedParameter& value) { return value.node == node && value.key == key; }))
        throw GraphException(GraphError::DuplicateName,
                             "parameter '" + key + "' of node " + std::to_string(node) + " is already exposed");
    if (std::any_of(exposedParameters_.begin(), exposedParameters_.end(),
                    [&](const ExposedParameter& value) { return value.name == name; }))
        throw GraphException(GraphError::DuplicateName, "exposed parameter name '" + name + "' is already in use");
    if (id == kInvalidInterfacePort) {
        if (nextInterfacePortId_ == std::numeric_limits<InterfacePortId>::max())
            throw GraphException(GraphError::InvalidId, "exposed parameter identity space is exhausted");
        id = nextInterfacePortId_;
    }
    if (id == kInvalidInterfacePort || id == std::numeric_limits<InterfacePortId>::max() || input(id) || output(id) ||
        findExposedParameter(id))
        throw GraphException(GraphError::DuplicateId,
                             "interface identity " + std::to_string(id) + " is already in use");
    const auto* spec = graph_.catalog().parameterSpec(graph_.node(node)->type, key);
    exposedParameters_.push_back(
        ExposedParameter{.id = id, .node = node, .key = std::move(key), .name = std::move(name), .type = spec->type});
    nextInterfacePortId_ = std::max(nextInterfacePortId_, static_cast<InterfacePortId>(id + 1));
    ++revision_;
    return id;
}

void Network::renameExposedParameter(InterfacePortId id, std::string name) {
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "exposed parameter name must not be empty");
    ExposedParameter* target = findExposedParameter(id);
    if (!target)
        throw GraphException(GraphError::UnknownEdge, "cannot rename unknown exposed parameter " + std::to_string(id));
    if (std::any_of(exposedParameters_.begin(), exposedParameters_.end(),
                    [&](const ExposedParameter& value) { return value.id != id && value.name == name; }))
        throw GraphException(GraphError::DuplicateName, "exposed parameter name '" + name + "' is already in use");
    if (target->name == name)
        return;
    target->name = std::move(name);
    ++revision_;
}

void Network::removeExposedParameter(InterfacePortId id) {
    const auto it = std::find_if(exposedParameters_.begin(), exposedParameters_.end(),
                                 [id](const ExposedParameter& value) { return value.id == id; });
    if (it == exposedParameters_.end())
        throw GraphException(GraphError::UnknownEdge, "cannot remove unknown exposed parameter " + std::to_string(id));
    exposedParameters_.erase(it);
    ++revision_;
}

void Network::moveExposedParameter(InterfacePortId id, std::size_t index) {
    const auto it = std::find_if(exposedParameters_.begin(), exposedParameters_.end(),
                                 [id](const ExposedParameter& value) { return value.id == id; });
    if (it == exposedParameters_.end())
        throw GraphException(GraphError::UnknownEdge, "cannot reorder unknown exposed parameter " + std::to_string(id));
    const std::size_t current = static_cast<std::size_t>(std::distance(exposedParameters_.begin(), it));
    const std::size_t destination = std::min(index, exposedParameters_.size() - 1);
    if (current == destination)
        return;
    ExposedParameter moved = std::move(*it);
    exposedParameters_.erase(exposedParameters_.begin() + static_cast<std::ptrdiff_t>(current));
    exposedParameters_.insert(exposedParameters_.begin() + static_cast<std::ptrdiff_t>(destination), std::move(moved));
    ++revision_;
}

void Network::restorePortExtension(PortDirection direction, InterfacePortId id, nlohmann::json extension) {
    auto& ports = direction == PortDirection::Input ? inputs_ : outputs_;
    FormalPort* found = findPort(ports, id);
    if (found == nullptr)
        throw GraphException(GraphError::InvalidId,
                             "cannot attach preserved data to unknown formal terminal " + std::to_string(id));
    found->extension = std::move(extension);
}

std::optional<GraphErrorDetails> Network::validateInputConnection(InterfacePortId input, PortRef destination) const {
    const FormalPort* terminal = this->input(input);
    if (!terminal)
        return GraphErrorDetails{GraphError::PortType,
                                 "network '" + name_ + "' has no formal input " + std::to_string(input)};
    const NodeInstance* node = graph_.node(destination.node);
    if (!node)
        return GraphErrorDetails{GraphError::UnknownNode,
                                 "formal input '" + terminal->name + "' targets unknown " + describe(destination)};
    const auto& ports = graph_.inputPorts(destination.node);
    if (static_cast<std::size_t>(destination.port) >= ports.size())
        return GraphErrorDetails{GraphError::PortType, "formal input '" + terminal->name + "' targets " +
                                                           describe(destination) + " outside its input contract"};
    if (!portKindsCompatible(terminal->kind, ports[destination.port].kind))
        return GraphErrorDetails{GraphError::PortType,
                                 "formal input '" + terminal->name + "' (" +
                                     std::to_string(static_cast<int>(terminal->kind)) + ") cannot feed " +
                                     describe(destination) + " (" +
                                     std::to_string(static_cast<int>(ports[destination.port].kind)) + ")"};
    for (const auto& edge : graph_.edgesInto(destination.node)) {
        if (edge.to == destination)
            return GraphErrorDetails{GraphError::PortOccupied, "formal input '" + terminal->name +
                                                                   "' cannot feed occupied " + describe(destination)};
    }
    for (const auto& connection : inputConnections_) {
        if (connection.node == destination)
            return GraphErrorDetails{GraphError::PortOccupied, "formal input '" + terminal->name +
                                                                   "' cannot feed occupied " + describe(destination)};
        if (connection.terminal == input && !terminal->allowFanOut)
            return GraphErrorDetails{GraphError::PortOccupied,
                                     "formal input '" + terminal->name + "' does not allow fan-out"};
    }
    return std::nullopt;
}

void Network::connectInput(InterfacePortId input, PortRef destination) {
    syncTerminalConnections();
    if (const auto problem = validateInputConnection(input, destination))
        throw GraphException(problem->code, problem->message);
    inputConnections_.push_back(TerminalConnection{.terminal = input, .node = destination});
    try {
        graph_.reserveInput(destination);
    } catch (...) {
        inputConnections_.pop_back();
        throw;
    }
    ++revision_;
    record();
}

void Network::disconnectInput(InterfacePortId input, PortRef destination) {
    syncTerminalConnections();
    const auto it = std::find_if(inputConnections_.begin(), inputConnections_.end(),
                                 [input, destination](const TerminalConnection& connection) {
                                     return connection.terminal == input && connection.node == destination;
                                 });
    if (it == inputConnections_.end())
        throw GraphException(GraphError::UnknownEdge, "formal input connection does not exist");
    graph_.releaseInput(destination);
    inputConnections_.erase(it);
    ++revision_;
    record();
}

std::optional<GraphErrorDetails> Network::validateOutputConnection(PortRef source, InterfacePortId output) const {
    const FormalPort* terminal = this->output(output);
    if (!terminal)
        return GraphErrorDetails{GraphError::PortType,
                                 "network '" + name_ + "' has no formal output " + std::to_string(output)};
    const NodeInstance* node = graph_.node(source.node);
    if (!node)
        return GraphErrorDetails{GraphError::UnknownNode,
                                 "formal output '" + terminal->name + "' reads unknown " + describe(source)};
    const auto& ports = graph_.outputPorts(source.node);
    if (static_cast<std::size_t>(source.port) >= ports.size())
        return GraphErrorDetails{GraphError::PortType, "formal output '" + terminal->name + "' reads " +
                                                           describe(source) + " outside its output contract"};
    if (!portKindsCompatible(ports[source.port].kind, terminal->kind))
        return GraphErrorDetails{GraphError::PortType, "formal output '" + terminal->name + "' (" +
                                                           std::to_string(static_cast<int>(terminal->kind)) +
                                                           ") cannot read " + describe(source) + " (" +
                                                           std::to_string(static_cast<int>(ports[source.port].kind)) +
                                                           ")"};
    if (std::find_if(outputConnections_.begin(), outputConnections_.end(),
                     [output](const TerminalConnection& connection) { return connection.terminal == output; }) !=
            outputConnections_.end() ||
        outputInputBindings_.contains(output))
        return GraphErrorDetails{GraphError::PortOccupied,
                                 "formal output '" + terminal->name + "' is already selected"};
    return std::nullopt;
}

void Network::connectOutput(PortRef source, InterfacePortId output) {
    syncTerminalConnections();
    if (const auto problem = validateOutputConnection(source, output))
        throw GraphException(problem->code, problem->message);
    outputConnections_.push_back(TerminalConnection{.terminal = output, .node = source});
    ++revision_;
    record();
}
void Network::disconnectOutput(InterfacePortId output) {
    syncTerminalConnections();
    const auto it =
        std::find_if(outputConnections_.begin(), outputConnections_.end(),
                     [output](const TerminalConnection& connection) { return connection.terminal == output; });
    if (it != outputConnections_.end()) {
        outputConnections_.erase(it);
        ++revision_;
        return;
    }
    if (outputInputBindings_.erase(output) != 0) {
        ++revision_;
        return;
    }
    throw GraphException(GraphError::UnknownEdge, "formal output connection does not exist");
}

void Network::connectOutputToInput(InterfacePortId output, InterfacePortId input) {
    const auto* outputPort = output ? this->output(output) : nullptr;
    const auto* inputPort = input ? this->input(input) : nullptr;
    if (!outputPort || !inputPort)
        throw GraphException(GraphError::PortType, "output pass-through references an unknown terminal");
    if (outputPort->kind != inputPort->kind)
        throw GraphException(GraphError::PortType, "output pass-through terminal kinds do not match");
    if (outputConnections_.end() !=
            std::find_if(outputConnections_.begin(), outputConnections_.end(),
                         [output](const TerminalConnection& connection) { return connection.terminal == output; }) ||
        outputInputBindings_.contains(output))
        throw GraphException(GraphError::PortOccupied, "formal output already has a connection");
    outputInputBindings_.emplace(output, input);
    ++revision_;
}

void Network::disconnectOutputToInput(InterfacePortId output) {
    if (outputInputBindings_.erase(output) == 0)
        throw GraphException(GraphError::UnknownEdge, "formal output pass-through does not exist");
    ++revision_;
    record();
}
void Network::syncTerminalConnections() {
    if (terminalSyncRevision_ == graph_.revision())
        return;
    inputConnections_.erase(std::remove_if(inputConnections_.begin(), inputConnections_.end(),
                                           [this](const TerminalConnection& connection) {
                                               return input(connection.terminal) == nullptr ||
                                                      graph_.node(connection.node.node) == nullptr;
                                           }),
                            inputConnections_.end());
    outputConnections_.erase(std::remove_if(outputConnections_.begin(), outputConnections_.end(),
                                            [this](const TerminalConnection& connection) {
                                                return output(connection.terminal) == nullptr ||
                                                       graph_.node(connection.node.node) == nullptr;
                                            }),
                             outputConnections_.end());
    terminalSyncRevision_ = graph_.revision();
}

const std::vector<TerminalConnection>& Network::inputConnections() const {
    return inputConnections_;
}

const std::vector<TerminalConnection>& Network::outputConnections() const {
    return outputConnections_;
}

void Network::restoreIdentityHighWatermarks(NodeId nextNodeId, EdgeId nextEdgeId, InterfacePortId nextInterfacePortId) {
    graph_.restoreIdentityHighWatermarks(nextNodeId, nextEdgeId);
    if (nextInterfacePortId == kInvalidInterfacePort)
        throw GraphException(GraphError::InvalidId, "formal port identity watermark must be nonzero");
    nextInterfacePortId_ = std::max(nextInterfacePortId_, nextInterfacePortId);
}

}  // namespace nemo
