#include "nemo/core/commands/NetworkCommands.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace nemo {
namespace {

struct IncomingBoundary {
    PortRef destination;
    PortKind kind{PortKind::Image};
    std::optional<PortRef> source;
    std::optional<InterfacePortId> parentTerminal;
    NetworkInstanceId nested{kInvalidNetworkInstance};
    InterfacePortId nestedInput{kInvalidInterfacePort};
    std::vector<LayoutPosition> route;
};
struct OutgoingBoundary {
    PortRef source;
    PortKind kind{PortKind::Image};
    PortRef destination;
    std::optional<InterfacePortId> parentTerminal;
    NetworkInstanceId nested{kInvalidNetworkInstance};
    InterfacePortId nestedInput{kInvalidInterfacePort};
    std::vector<LayoutPosition> route;
};

[[nodiscard]] bool isTerminalNode(const NodeInstance& node, const Graph& graph) {
    if (node.type == "input" || node.type == "Input" || node.type == "INPUT" || node.type == "output" ||
        node.type == "Output" || node.type == "OUTPUT")
        return true;
    const auto* descriptor = graph.descriptor(node.type);
    return descriptor != nullptr && descriptor->isOutput;
}

[[nodiscard]] std::string endpointKey(PortRef ref, PortKind kind) {
    return std::to_string(ref.node) + ":" + std::to_string(ref.port) + ":" + std::to_string(static_cast<int>(kind));
}
[[nodiscard]] std::string terminalKey(InterfacePortId id, PortKind kind) {
    return "terminal:" + std::to_string(id) + ":" + std::to_string(static_cast<int>(kind));
}
[[nodiscard]] std::string uniqueNodeName(const Graph& graph, std::string base) {
    if (base.empty())
        base = "Node";
    if (!graph.nodeByName(base))
        return base;
    for (std::size_t suffix = 2;; ++suffix) {
        std::string candidate = base + " " + std::to_string(suffix);
        if (!graph.nodeByName(candidate))
            return candidate;
    }
}
[[nodiscard]] std::string uniqueNetworkName(const Document& document, std::string base) {
    if (base.empty())
        base = "Network";
    const auto exists = [&](const std::string& candidate) {
        return std::any_of(document.networks().begin(), document.networks().end(),
                           [&](const Network& network) { return network.name() == candidate; });
    };
    if (!exists(base))
        return base;
    for (std::size_t suffix = 2;; ++suffix) {
        const std::string candidate = base + " " + std::to_string(suffix);
        if (!exists(candidate))
            return candidate;
    }
}

void copyNodeExact(Graph& graph, const NodeInstance& node) {
    (void)graph.addNodeWithId(node.id, node.type, node.name, node.params, node.layout, node.definition, node.instance);
    if (node.hasPortContract)
        graph.setPortContract(node.id, node.inputPorts, node.outputPorts);
}

[[nodiscard]] NetworkId addNetworkLike(Document& document, const Network& source, std::string name) {
    const Network sourceSnapshot = source;
    const NetworkId id = document.addNetwork(uniqueNetworkName(document, std::move(name)));
    if (const auto automatic = document.network(id).defaultOutput(); automatic != kInvalidNode)
        document.network(id).graph().removeNode(automatic);
    for (const auto& port : sourceSnapshot.inputs()) {
        const auto created = document.network(id).addInput(port.name, port.kind, port.id, port.allowFanOut);
        document.network(id).setFormalPortLayout(PortDirection::Input, created, port.layout);
    }
    for (const auto& port : sourceSnapshot.outputs()) {
        const auto created = document.network(id).addOutput(port.name, port.kind, port.id, port.allowFanOut);
        document.network(id).setFormalPortLayout(PortDirection::Output, created, port.layout);
    }
    NodeId maxSourceNode = 0;
    for (const auto& node : sourceSnapshot.graph().nodes())
        maxSourceNode = std::max(maxSourceNode, node.id);
    if (maxSourceNode != 0)
        document.network(id).graph().restoreIdentityHighWatermarks(maxSourceNode + 1,
                                                                   document.network(id).graph().nextEdgeId());
    std::map<NodeId, NodeId> nodeMap;
    std::map<NodeId, NetworkInstanceId> nestedMap;
    for (const auto& node : sourceSnapshot.graph().nodes()) {
        if (node.instance == kInvalidNetworkInstance) {
            const auto created = document.network(id).graph().addNodeWithId(
                node.id, node.type, uniqueNodeName(document.network(id).graph(), node.name), node.params, node.layout);
            nodeMap[node.id] = created;
            if (node.hasPortContract)
                document.network(id).graph().setPortContract(created, node.inputPorts, node.outputPorts);
            continue;
        }
        const auto* nested = document.instance(node.instance);
        if (!nested)
            throw GraphException(GraphError::InvalidInstance,
                                 "network definition contains a missing nested occurrence");
        const auto nestedCopy = *nested;
        NetworkId copiedDefinition = nestedCopy.definition;
        if (nestedCopy.ownsDefinition) {
            const auto nestedSnapshot = document.network(nestedCopy.definition);
            copiedDefinition = addNetworkLike(document, nestedSnapshot, nestedSnapshot.name() + " Copy");
        }
        const auto created =
            document.addInstance(id, copiedDefinition, uniqueNodeName(document.network(id).graph(), node.name));
        if (nestedCopy.ownsDefinition)
            document.setInstanceOwnership(created, true);
        nestedMap[node.id] = created;
        const auto* createdNode = document.instance(created);
        nodeMap[node.id] = createdNode->node;
        document.network(id).graph().setLayout(createdNode->node, node.layout);
        for (const auto& [targetNode, values] : nestedCopy.params)
            for (const auto& [key, value] : values)
                document.setInstanceParam(created, targetNode, key, value);
    }
    for (const auto& node : sourceSnapshot.graph().nodes()) {
        if (node.instance == kInvalidNetworkInstance)
            continue;
        const auto* nested = document.instance(node.instance);
        if (!nested)
            continue;
        const auto newInstance = nestedMap.at(node.id);
        for (const auto& [input, binding] : nested->inputBindings) {
            const auto mapped = nodeMap.find(binding.node);
            if (mapped != nodeMap.end())
                document.bindInstanceInput(newInstance, input, {mapped->second, binding.port});
        }
    }
    for (const auto& edge : sourceSnapshot.graph().edges()) {
        const auto created = document.network(id).graph().connect({nodeMap.at(edge.from.node), edge.from.port},
                                                                  {nodeMap.at(edge.to.node), edge.to.port});
        if (!edge.route.empty())
            document.network(id).graph().setRoute(created, edge.route);
    }
    for (const auto& connection : sourceSnapshot.inputConnections())
        document.network(id).connectInput(connection.terminal,
                                          {nodeMap.at(connection.node.node), connection.node.port});
    for (const auto& connection : sourceSnapshot.outputConnections())
        document.network(id).connectOutput({nodeMap.at(connection.node.node), connection.node.port},
                                           connection.terminal);
    for (const auto& [output, input] : sourceSnapshot.outputInputBindings())
        document.network(id).connectOutputToInput(output, input);
    for (const auto& exposed : sourceSnapshot.exposedParameters())
        (void)document.network(id).addExposedParameter(nodeMap.at(exposed.node), exposed.key, exposed.name, exposed.id);
    if (sourceSnapshot.defaultOutput() != kInvalidNode) {
        const auto mapped = nodeMap.find(sourceSnapshot.defaultOutput());
        if (mapped != nodeMap.end())
            document.network(id).setDefaultOutput(mapped->second);
    }
    document.copyAnimationChannels(sourceSnapshot.id(), id, nodeMap);
    return id;
}

void collapse(Document& document, NetworkId parentId, const std::vector<NodeId>& selected,
              const std::string& requestedName, NetworkInstanceId* createdId) {
    if (selected.empty())
        throw GraphException(GraphError::InvalidNetwork, "collapse requires at least one selected node");
    Network& parent = document.network(parentId);
    Graph& parentGraph = parent.graph();
    std::set<NodeId> selectedSet;
    std::vector<NodeInstance> selectedNodes;
    std::vector<std::pair<NetworkInstanceId, NodeId>> selectedNested;
    selectedNodes.reserve(selected.size());
    for (const NodeId id : selected) {
        if (!selectedSet.insert(id).second)
            throw GraphException(GraphError::DuplicateId,
                                 "collapse selection contains node " + std::to_string(id) + " more than once");
        const auto* node = parentGraph.node(id);
        if (!node)
            throw GraphException(GraphError::UnknownNode,
                                 "collapse selection contains unknown node " + std::to_string(id));
        if (isTerminalNode(*node, parentGraph))
            throw GraphException(GraphError::InvalidNetwork, "formal Input/Output terminals cannot be collapsed");
        NodeInstance snapshot = *node;
        if (!snapshot.hasPortContract) {
            snapshot.inputPorts = parentGraph.inputPorts(node->id);
            snapshot.outputPorts = parentGraph.outputPorts(node->id);
            snapshot.hasPortContract = true;
        }
        selectedNodes.push_back(std::move(snapshot));
        if (node->instance != kInvalidNetworkInstance)
            selectedNested.emplace_back(node->instance, node->id);
    }

    std::vector<IncomingBoundary> incoming;
    std::vector<OutgoingBoundary> outgoing;
    for (const auto& edge : parentGraph.edges()) {
        const bool fromInside = selectedSet.contains(edge.from.node);
        const bool toInside = selectedSet.contains(edge.to.node);
        if (fromInside == toInside)
            continue;
        if (!fromInside) {
            const auto& sourcePorts = parentGraph.outputPorts(edge.from.node);
            const auto& destinationPorts = parentGraph.inputPorts(edge.to.node);
            if (edge.from.port >= sourcePorts.size() || edge.to.port >= destinationPorts.size() ||
                sourcePorts[edge.from.port].kind != destinationPorts[edge.to.port].kind)
                throw GraphException(GraphError::PortType,
                                     "collapse found an invalid incoming boundary edge " + std::to_string(edge.id));
            incoming.push_back(IncomingBoundary{edge.to,
                                                destinationPorts[edge.to.port].kind,
                                                edge.from,
                                                {},
                                                kInvalidNetworkInstance,
                                                {},
                                                edge.route});
        } else {
            const auto& sourcePorts = parentGraph.outputPorts(edge.from.node);
            const auto& destinationPorts = parentGraph.inputPorts(edge.to.node);
            if (edge.from.port >= sourcePorts.size() || edge.to.port >= destinationPorts.size() ||
                sourcePorts[edge.from.port].kind != destinationPorts[edge.to.port].kind)
                throw GraphException(GraphError::PortType,
                                     "collapse found an invalid outgoing boundary edge " + std::to_string(edge.id));
            outgoing.push_back(OutgoingBoundary{edge.from,
                                                sourcePorts[edge.from.port].kind,
                                                edge.to,
                                                {},
                                                kInvalidNetworkInstance,
                                                kInvalidInterfacePort,
                                                edge.route});
        }
    }
    // Formal connections are crossing relationships too. They are collected
    // before removing selected nodes because Graph::removeNode clears reserves.
    for (const auto& connection : parent.inputConnections()) {
        if (!selectedSet.contains(connection.node.node))
            continue;
        const auto& ports = parentGraph.inputPorts(connection.node.node);
        if (connection.node.port >= ports.size())
            throw GraphException(GraphError::PortType, "collapse found an invalid formal input boundary");
        incoming.push_back(IncomingBoundary{connection.node,
                                            ports[connection.node.port].kind,
                                            {},
                                            connection.terminal,
                                            kInvalidNetworkInstance,
                                            kInvalidInterfacePort,
                                            {}});
    }
    for (const auto& connection : parent.outputConnections()) {
        if (!selectedSet.contains(connection.node.node))
            continue;
        const auto& ports = parentGraph.outputPorts(connection.node.node);
        if (connection.node.port >= ports.size())
            throw GraphException(GraphError::PortType, "collapse found an invalid formal output boundary");
        outgoing.push_back(OutgoingBoundary{connection.node,
                                            ports[connection.node.port].kind,
                                            {},
                                            connection.terminal,
                                            kInvalidNetworkInstance,
                                            kInvalidInterfacePort,
                                            {}});
    }

    // Instance input bindings are boundary edges even though they are not
    // represented as Graph edges. Selected nested occurrences are reparented
    // into the child while external inputs become child formal terminals.
    for (const auto& [nestedId, nestedNode] : selectedNested) {
        const auto* node = parentGraph.node(nestedNode);
        if (!node)
            throw GraphException(GraphError::InvalidInstance, "selected nested node has no graph occurrence");
        const auto* occurrence = document.instance(nestedId);
        if (!occurrence)
            throw GraphException(GraphError::InvalidInstance, "selected nested node has no occurrence record");
        const auto& definition = document.network(occurrence->definition);
        for (const auto& [inputId, source] : occurrence->inputBindings) {
            if (selectedSet.contains(source.node))
                continue;
            const auto* formal = definition.input(inputId);
            if (!formal)
                throw GraphException(GraphError::PortType,
                                     "nested occurrence binding references an unknown formal input");
            incoming.push_back(IncomingBoundary{
                PortRef{node->id, static_cast<std::uint32_t>(std::distance(
                                      definition.inputs().begin(),
                                      std::find_if(definition.inputs().begin(), definition.inputs().end(),
                                                   [inputId](const FormalPort& p) { return p.id == inputId; })))},
                formal->kind,
                source,
                {},
                occurrence->id,
                inputId,
                {}});
        }
    }

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();

    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const auto& node : selectedNodes) {
        minX = std::min(minX, node.layout.x);
        minY = std::min(minY, node.layout.y);
        maxX = std::max(maxX, node.layout.x + 120.0);
        maxY = std::max(maxY, node.layout.y + 70.0);
    }
    for (const auto& node : parentGraph.nodes()) {
        if (selectedSet.contains(node.id) || node.instance == kInvalidNetworkInstance)
            continue;
        const auto* occurrence = document.instance(node.instance);
        if (!occurrence)
            throw GraphException(GraphError::InvalidInstance, "unselected nested node has no occurrence record");
        const auto& definition = document.network(occurrence->definition);
        for (const auto& [input, source] : occurrence->inputBindings) {
            if (!selectedSet.contains(source.node))
                continue;
            const auto* formal = definition.input(input);
            if (!formal)
                throw GraphException(GraphError::PortType,
                                     "nested outgoing binding references an unknown formal input");
            outgoing.push_back(OutgoingBoundary{source, formal->kind, {}, {}, occurrence->id, input, {}});
        }
    }

    const std::vector<Edge> parentEdges(parentGraph.edges().begin(), parentGraph.edges().end());
    const NetworkId childId =
        document.addNetwork(uniqueNetworkName(document, requestedName.empty() ? "Subnet" : requestedName));
    Network& child = document.network(childId);
    if (const auto automatic = child.defaultOutput(); automatic != kInvalidNode)
        child.graph().removeNode(automatic);
    for (const auto& node : selectedNodes)
        copyNodeExact(child.graph(), node);
    for (const auto& edge : parentEdges) {
        if (!selectedSet.contains(edge.from.node) || !selectedSet.contains(edge.to.node))
            continue;
        (void)child.graph().connectWithId(edge.id, edge.from, edge.to);
        if (!edge.route.empty())
            child.graph().setRoute(edge.id, edge.route);
    }

    std::map<std::string, InterfacePortId> inputPorts;
    std::map<std::string, InterfacePortId> outputPorts;
    std::vector<std::pair<std::string, InterfacePortId>> inputOrder;
    std::vector<std::pair<std::string, InterfacePortId>> outputOrder;
    std::vector<std::pair<InterfacePortId, PortKind>> viewerPassThroughInputs;
    std::map<std::pair<NetworkInstanceId, InterfacePortId>, InterfacePortId> nestedInputMap;
    std::size_t inputIndex = 0;
    for (auto& boundary : incoming) {
        const std::string key = boundary.parentTerminal ? terminalKey(*boundary.parentTerminal, boundary.kind)
                                                        : endpointKey(*boundary.source, boundary.kind);
        auto found = inputPorts.find(key);
        InterfacePortId terminal = kInvalidInterfacePort;
        if (found == inputPorts.end()) {
            std::string terminalName =
                boundary.kind == PortKind::Mask ? "Mask" : "Input " + std::to_string(inputIndex + 1);
            for (std::size_t suffix = 2; child.input(terminalName) != nullptr; ++suffix)
                terminalName = (boundary.kind == PortKind::Mask ? "Mask " : "Input ") + std::to_string(suffix);
            terminal = child.addInput(terminalName, boundary.kind);
            inputPorts.emplace(key, terminal);
            inputOrder.emplace_back(key, terminal);
            ++inputIndex;
        } else {
            terminal = found->second;
        }
        if (boundary.nested != kInvalidNetworkInstance) {
            nestedInputMap.emplace(std::make_pair(boundary.nested, boundary.nestedInput), terminal);
            continue;
        }
        child.connectInput(terminal, boundary.destination);
        if (boundary.source) {
            const auto* destinationNode = child.graph().node(boundary.destination.node);
            std::string type = destinationNode ? destinationNode->type : std::string{};
            std::transform(type.begin(), type.end(), type.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (type == "viewer" && boundary.destination.port == 0 && boundary.kind == PortKind::Image)
                viewerPassThroughInputs.emplace_back(terminal, boundary.kind);
        }
    }
    // Every subnet has a stable explicit mask terminal, including selections
    // whose image-only boundary has no mask crossing.
    if (std::none_of(inputOrder.begin(), inputOrder.end(),
                     [&](const auto& value) { return child.input(value.second)->kind == PortKind::Mask; })) {
        const auto terminal = child.addInput("Mask", PortKind::Mask);
        inputOrder.emplace_back("implicit-mask", terminal);
    }

    std::size_t outputIndex = 0;
    auto addOutputBoundary = [&](PortRef source, PortKind kind) {
        const std::string key = endpointKey(source, kind);
        if (const auto found = outputPorts.find(key); found != outputPorts.end())
            return found->second;
        const auto terminal = child.addOutput("Output " + std::to_string(outputIndex + 1), kind);
        outputPorts.emplace(key, terminal);
        outputOrder.emplace_back(key, terminal);
        child.connectOutput(source, terminal);
        ++outputIndex;
        return terminal;
    };
    auto addPassThroughBoundary = [&](InterfacePortId input, PortKind kind) {
        const auto terminal = child.addOutput("Output " + std::to_string(outputIndex + 1), kind);
        outputOrder.emplace_back("input:" + std::to_string(input), terminal);
        child.connectOutputToInput(terminal, input);
        ++outputIndex;
        return terminal;
    };
    for (const auto& boundary : outgoing)
        static_cast<void>(addOutputBoundary(boundary.source, boundary.kind));
    if (outgoing.empty()) {
        std::vector<std::pair<PortRef, PortKind>> candidates;
        auto addCandidate = [&](PortRef source, PortKind kind) {
            if (std::none_of(candidates.begin(), candidates.end(),
                             [&](const auto& value) { return value.first == source && value.second == kind; }))
                candidates.emplace_back(source, kind);
        };
        for (const auto& node : selectedNodes) {
            const auto& ports = node.outputPorts;
            for (std::uint32_t port = 0; port < ports.size(); ++port) {
                const bool consumed = std::any_of(parentEdges.begin(), parentEdges.end(), [&](const Edge& edge) {
                    return edge.from == PortRef{node.id, port} && selectedSet.contains(edge.to.node);
                });
                if (!consumed)
                    addCandidate(PortRef{node.id, port}, ports[port].kind);
            }
            std::string lower = node.type;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (lower == "viewer") {
                for (const auto& edge : parentEdges) {
                    if (edge.to.node != node.id || edge.to.port != 0 || !selectedSet.contains(edge.from.node))
                        continue;
                    const auto sourceNode =
                        std::find_if(selectedNodes.begin(), selectedNodes.end(),
                                     [edge](const NodeInstance& value) { return value.id == edge.from.node; });
                    if (sourceNode != selectedNodes.end() && edge.from.port < sourceNode->outputPorts.size())
                        addCandidate(edge.from, sourceNode->outputPorts[edge.from.port].kind);
                }
            }
        }
        if (candidates.empty() && !viewerPassThroughInputs.empty()) {
            for (const auto& [input, kind] : viewerPassThroughInputs)
                static_cast<void>(addPassThroughBoundary(input, kind));
        } else if (candidates.empty()) {
            throw GraphException(GraphError::InvalidNetwork,
                                 "collapse rejected: selected nodes have no connected image or output to expose");
        } else {
            for (const auto& candidate : candidates)
                static_cast<void>(addOutputBoundary(candidate.first, candidate.second));
        }
    }

    const double inputSpacing =
        std::max(160.0, (maxX - minX - 112.0) / std::max<std::size_t>(1, inputOrder.size() - 1));
    for (std::size_t index = 0; index < inputOrder.size(); ++index)
        child.setFormalPortLayout(PortDirection::Input, inputOrder[index].second,
                                  {minX + index * inputSpacing, minY - 110.0});
    std::map<NodeId, NodeId> movedAnimationNodes;
    for (const auto& node : selectedNodes)
        movedAnimationNodes.emplace(node.id, node.id);
    document.remapAnimationChannels(parentId, childId, movedAnimationNodes);
    const double outputSpacing =
        std::max(160.0, (maxX - minX - 112.0) / std::max<std::size_t>(1, outputOrder.size() - 1));
    for (std::size_t index = 0; index < outputOrder.size(); ++index)
        child.setFormalPortLayout(PortDirection::Output, outputOrder[index].second,
                                  {minX + index * outputSpacing, maxY + 80.0});

    // Parent and child edits are validated on CommandStack's private
    // candidate before either network is published.
    Network& committedParent = document.network(parentId);
    Graph& committedParentGraph = committedParent.graph();
    const auto parentInputs = committedParent.inputConnections();
    for (const auto& connection : parentInputs)
        if (selectedSet.contains(connection.node.node))
            committedParent.disconnectInput(connection.terminal, connection.node);
    const auto parentOutputs = committedParent.outputConnections();
    for (const auto& connection : parentOutputs)
        if (selectedSet.contains(connection.node.node))
            committedParent.disconnectOutput(connection.terminal);
    for (const auto& [nestedId, nestedNode] : selectedNested) {
        if (!committedParentGraph.node(nestedNode))
            throw GraphException(GraphError::InvalidInstance,
                                 "selected nested node disappeared during collapse validation");
        const auto* occurrence = document.instance(nestedId);
        if (!occurrence)
            continue;
        std::vector<InterfacePortId> toErase;
        for (const auto& [inputId, source] : occurrence->inputBindings)
            if (!selectedSet.contains(source.node))
                toErase.push_back(inputId);
        for (const auto inputId : toErase)
            document.eraseInstanceInputBinding(nestedId, inputId);
    }
    for (const NodeId id : selected)
        committedParentGraph.removeNode(id);

    const auto subnetInstance = document.addInstance(
        parentId, childId, uniqueNodeName(committedParentGraph, requestedName.empty() ? "Subnet" : requestedName));
    document.setInstanceOwnership(subnetInstance, true);
    const auto* subnetOccurrence = document.instance(subnetInstance);
    if (!subnetOccurrence)
        throw GraphException(GraphError::InvalidInstance, "failed to create collapsed subnet occurrence");
    const NodeId subnetNode = subnetOccurrence->node;
    committedParentGraph.setLayout(subnetNode, {minX, minY});
    for (const auto& boundary : incoming) {
        const std::string key = boundary.parentTerminal ? terminalKey(*boundary.parentTerminal, boundary.kind)
                                                        : endpointKey(*boundary.source, boundary.kind);
        const auto terminal = inputPorts.at(key);
        if (boundary.parentTerminal) {
            document.bindInstanceInputToParentTerminal(subnetInstance, terminal, *boundary.parentTerminal);
        } else {
            document.bindInstanceInput(subnetInstance, terminal, *boundary.source);
        }
    }
    for (const auto& boundary : outgoing) {
        const auto terminal = outputPorts.at(endpointKey(boundary.source, boundary.kind));
        const auto outputIndexIt = std::find_if(child.outputs().begin(), child.outputs().end(),
                                                [terminal](const FormalPort& value) { return value.id == terminal; });
        const auto index = static_cast<std::uint32_t>(std::distance(child.outputs().begin(), outputIndexIt));
        if (boundary.nested != kInvalidNetworkInstance) {
            document.bindInstanceInput(boundary.nested, boundary.nestedInput, {subnetNode, index});
        } else if (boundary.parentTerminal) {
            committedParent.connectOutput({subnetNode, index}, *boundary.parentTerminal);
        } else {
            const auto edgeId = committedParentGraph.connect(PortRef{subnetNode, index}, boundary.destination);
            if (!boundary.route.empty())
                committedParentGraph.setRoute(edgeId, boundary.route);
        }
    }
    for (const auto& [nestedId, nestedNode] : selectedNested)
        document.reparentInstance(nestedId, childId, nestedNode);
    for (const auto& [nestedAndInput, terminal] : nestedInputMap)
        document.bindInstanceInputToParentTerminal(nestedAndInput.first, nestedAndInput.second, terminal);
    if (createdId)
        *createdId = subnetInstance;
}

void unpack(Document& document, NetworkInstanceId instanceId) {
    const auto* occurrence = document.instance(instanceId);
    if (!occurrence)
        throw GraphException(GraphError::UnknownInstance,
                             "cannot unpack unknown network instance " + std::to_string(instanceId));
    const NetworkId parentId = occurrence->parentNetwork;
    const NetworkId definitionId = occurrence->definition;
    const NodeId occurrenceNode = occurrence->node;
    const Network& definition = document.network(definitionId);
    Network& parent = document.network(parentId);
    Graph& graph = parent.graph();

    std::map<NodeId, NodeId> nodeMap;
    std::map<NetworkInstanceId, NetworkInstanceId> instanceMap;
    const std::vector<Edge> parentEdges(graph.edges().begin(), graph.edges().end());
    std::vector<Edge> incomingEdges;
    std::vector<Edge> outgoingEdges;
    for (const auto& edge : parentEdges) {
        if (edge.to.node == occurrenceNode)
            incomingEdges.push_back(edge);
        if (edge.from.node == occurrenceNode)
            outgoingEdges.push_back(edge);
    }
    std::vector<TerminalConnection> parentInputConnections;
    for (const auto& connection : parent.inputConnections())
        if (connection.node.node == occurrenceNode)
            parentInputConnections.push_back(connection);
    std::vector<TerminalConnection> parentOutputConnections;
    for (const auto& connection : parent.outputConnections())
        if (connection.node.node == occurrenceNode)
            parentOutputConnections.push_back(connection);

    const auto originalOccurrence = *occurrence;
    document.setInstanceOwnership(instanceId, false);
    document.removeInstance(instanceId);
    for (const auto& node : definition.graph().nodes()) {
        if (node.instance != kInvalidNetworkInstance) {
            const auto* nested = document.instance(node.instance);
            if (!nested)
                throw GraphException(GraphError::InvalidInstance, "definition contains a missing nested occurrence");
            const auto nestedCopy = *nested;
            const auto newNested =
                document.addInstance(parentId, nestedCopy.definition, uniqueNodeName(graph, nestedCopy.name));
            const auto* created = document.instance(newNested);
            nodeMap[node.id] = created->node;
            instanceMap[node.instance] = newNested;
            graph.setLayout(created->node, node.layout);
            if (nestedCopy.ownsDefinition)
                document.setInstanceOwnership(newNested, true);
            for (const auto& [target, values] : nestedCopy.params)
                for (const auto& [key, value] : values)
                    document.setInstanceParam(newNested, target, key, value);
        } else {
            const auto newNode = graph.addNode(node.type, uniqueNodeName(graph, node.name));
            nodeMap[node.id] = newNode;
            graph.setLayout(newNode, node.layout);
            for (const auto& [key, value] : node.params)
                graph.setParam(newNode, key, value);
            if (node.hasPortContract)
                graph.setPortContract(newNode, node.inputPorts, node.outputPorts);
        }
    }
    for (const auto& [target, values] : originalOccurrence.params) {
        const auto mapped = nodeMap.find(target);
        if (mapped == nodeMap.end() || graph.node(mapped->second)->instance != kInvalidNetworkInstance)
            continue;
        for (const auto& [key, value] : values)
            graph.setParam(mapped->second, key, value);
    }
    for (const auto& node : definition.graph().nodes()) {
        if (node.instance == kInvalidNetworkInstance)
            continue;
        const auto oldNested = document.instance(node.instance);
        const auto newNestedIt = instanceMap.find(node.instance);
        if (!oldNested || newNestedIt == instanceMap.end())
            continue;
        for (const auto& [input, source] : oldNested->inputBindings) {
            const auto mappedSource = nodeMap.find(source.node);
            if (mappedSource != nodeMap.end())
                document.bindInstanceInput(newNestedIt->second, input, {mappedSource->second, source.port});
        }
    }
    for (const auto& edge : definition.graph().edges()) {
        const auto newEdge =
            graph.connect({nodeMap.at(edge.from.node), edge.from.port}, {nodeMap.at(edge.to.node), edge.to.port});
        if (!edge.route.empty())
            graph.setRoute(newEdge, edge.route);
    }
    // Expand each definition input connection using the occurrence's authored
    // binding first, then a direct parent edge/terminal when present.
    for (std::size_t index = 0; index < definition.inputs().size(); ++index) {
        const auto& formal = definition.inputs()[index];
        std::vector<TerminalConnection> destinations;
        for (const auto& connection : definition.inputConnections())
            if (connection.terminal == formal.id)
                destinations.push_back(connection);
        const auto binding = originalOccurrence.inputBindings.find(formal.id);
        const auto incoming = std::find_if(incomingEdges.begin(), incomingEdges.end(),
                                           [index](const Edge& value) { return value.to.port == index; });
        const auto parentInput =
            std::find_if(parentInputConnections.begin(), parentInputConnections.end(),
                         [index](const TerminalConnection& value) { return value.node.port == index; });
        for (const auto& destination : destinations) {
            const PortRef mappedDestination{nodeMap.at(destination.node.node), destination.node.port};
            if (binding != originalOccurrence.inputBindings.end()) {
                static_cast<void>(graph.connect(binding->second, mappedDestination));
            } else if (incoming != incomingEdges.end()) {
                const auto newEdge = graph.connect(incoming->from, mappedDestination);
                if (!incoming->route.empty())
                    graph.setRoute(newEdge, incoming->route);
            } else if (parentInput != parentInputConnections.end()) {
                parent.connectInput(parentInput->terminal, mappedDestination);
            }
        }
    }
    // Reattach each occurrence output only to the destinations that consumed
    // that particular formal port, preserving routes on direct graph edges.
    for (std::size_t index = 0; index < definition.outputs().size(); ++index) {
        const auto& formal = definition.outputs()[index];
        std::optional<PortRef> mappedSource;
        const auto source =
            std::find_if(definition.outputConnections().begin(), definition.outputConnections().end(),
                         [formal](const TerminalConnection& value) { return value.terminal == formal.id; });
        if (source != definition.outputConnections().end())
            mappedSource = PortRef{nodeMap.at(source->node.node), source->node.port};
        else if (const auto pass = definition.outputInputBindings().find(formal.id);
                 pass != definition.outputInputBindings().end()) {
            const auto inputIt = std::find_if(definition.inputs().begin(), definition.inputs().end(),
                                              [pass](const FormalPort& value) { return value.id == pass->second; });
            if (inputIt != definition.inputs().end()) {
                const auto inputIndex = static_cast<std::uint32_t>(std::distance(definition.inputs().begin(), inputIt));
                const auto incoming =
                    std::find_if(incomingEdges.begin(), incomingEdges.end(),
                                 [inputIndex](const Edge& value) { return value.to.port == inputIndex; });
                if (incoming != incomingEdges.end())
                    mappedSource = incoming->from;
                else if (const auto binding = originalOccurrence.inputBindings.find(pass->second);
                         binding != originalOccurrence.inputBindings.end())
                    mappedSource = binding->second;
            }
        }
        if (!mappedSource)
            continue;
        for (const auto& edge : outgoingEdges)
            if (edge.from.port == index) {
                const auto newEdge = graph.connect(*mappedSource, edge.to);
                if (!edge.route.empty())
                    graph.setRoute(newEdge, edge.route);
            }
        for (const auto& output : parentOutputConnections)
            if (output.node.port == index)
                parent.connectOutput(*mappedSource, output.terminal);
    }
    if (originalOccurrence.ownsDefinition)
        document.removeOwnedNetworkIfUnreferenced(definitionId);
}
void copySelected(Document& document, NetworkId sourceId, const std::vector<NodeId>& selected, NetworkId destinationId,
                  LayoutPosition offset, std::vector<NodeId>* created) {
    const Network sourceSnapshot = document.network(sourceId);
    std::set<NodeId> selectedSet(selected.begin(), selected.end());
    std::vector<NodeInstance> selectedNodes;
    selectedNodes.reserve(selected.size());
    for (NodeId id : selected) {
        const auto* node = sourceSnapshot.graph().node(id);
        if (!node)
            throw GraphException(GraphError::UnknownNode, "copy selection contains unknown node " + std::to_string(id));
        selectedNodes.push_back(*node);
    }
    const std::vector<Edge> sourceEdges(sourceSnapshot.graph().edges().begin(), sourceSnapshot.graph().edges().end());
    std::map<NodeId, NodeId> mapping;
    std::map<NodeId, NetworkInstanceId> copiedInstances;
    for (const auto& node : selectedNodes) {
        if (node.instance != kInvalidNetworkInstance) {
            const auto* occurrence = document.instance(node.instance);
            if (!occurrence)
                throw GraphException(GraphError::InvalidInstance,
                                     "copy selection contains an invalid nested occurrence");
            const auto occurrenceCopy = *occurrence;
            NetworkId copiedDefinition = occurrenceCopy.definition;
            if (occurrenceCopy.ownsDefinition) {
                const auto nestedSnapshot = document.network(occurrenceCopy.definition);
                copiedDefinition = addNetworkLike(document, nestedSnapshot, nestedSnapshot.name() + " Copy");
            }
            const auto newInstance = document.addInstance(
                destinationId, copiedDefinition, uniqueNodeName(document.network(destinationId).graph(), node.name));
            if (occurrenceCopy.ownsDefinition)
                document.setInstanceOwnership(newInstance, true);
            copiedInstances[node.id] = newInstance;
            const auto* newNode = document.instance(newInstance);
            mapping[node.id] = newNode->node;
            document.network(destinationId)
                .graph()
                .setLayout(newNode->node, {node.layout.x + offset.x, node.layout.y + offset.y});
            for (const auto& [target, values] : occurrenceCopy.params)
                for (const auto& [key, value] : values)
                    document.setInstanceParam(newInstance, target, key, value);
        } else {
            auto& destinationGraph = document.network(destinationId).graph();
            const auto newNode = destinationGraph.addNode(node.type, uniqueNodeName(destinationGraph, node.name));
            mapping[node.id] = newNode;
            destinationGraph.setLayout(newNode, {node.layout.x + offset.x, node.layout.y + offset.y});
            for (const auto& [key, value] : node.params)
                destinationGraph.setParam(newNode, key, value);
            if (node.hasPortContract)
                destinationGraph.setPortContract(newNode, node.inputPorts, node.outputPorts);
        }
        if (created)
            created->push_back(mapping[node.id]);
    }
    for (const auto& [oldNode, newInstance] : copiedInstances) {
        const auto* sourceNode = sourceSnapshot.graph().node(oldNode);
        const auto* occurrence = sourceNode ? document.instance(sourceNode->instance) : nullptr;
        if (!occurrence)
            continue;
        for (const auto& [input, sourcePort] : occurrence->inputBindings) {
            if (!selectedSet.contains(sourcePort.node))
                continue;
            const auto mappedSource = mapping.find(sourcePort.node);
            if (mappedSource != mapping.end())
                document.bindInstanceInput(newInstance, input, {mappedSource->second, sourcePort.port});
        }
    }
    for (const auto& edge : sourceEdges)
        if (selectedSet.contains(edge.from.node) && selectedSet.contains(edge.to.node)) {
            auto& destinationGraph = document.network(destinationId).graph();
            const auto id = destinationGraph.connect({mapping.at(edge.from.node), edge.from.port},
                                                     {mapping.at(edge.to.node), edge.to.port});
            if (!edge.route.empty()) {
                std::vector<LayoutPosition> route;
                route.reserve(edge.route.size());
                for (const auto point : edge.route)
                    route.push_back({point.x + offset.x, point.y + offset.y});
                destinationGraph.setRoute(id, std::move(route));
            }
        }
    document.copyAnimationChannels(sourceId, destinationId, mapping);
}

}  // namespace

Command promoteInterfaceCommand(NetworkId network, PortDirection direction, PortKind kind, std::string name,
                                std::shared_ptr<InterfacePortId> created) {
    return Command{"promote network interface",
                   [network, direction, kind, name = std::move(name), created](Document& document) {
                       const auto id = document.network(network).addFormalPort(direction, name, kind);
                       if (created)
                           *created = id;
                   }};
}

Command collapseSelectionCommand(NetworkId network, std::vector<NodeId> selected, std::string name,
                                 std::shared_ptr<NetworkInstanceId> created) {
    return Command{"collapse selection into subnet",
                   [network, selected = std::move(selected), name = std::move(name), created](Document& candidate) {
                       NetworkInstanceId id = kInvalidNetworkInstance;
                       collapse(candidate, network, selected, name, &id);
                       if (created)
                           *created = id;
                   }};
}

Command unpackInstanceCommand(NetworkInstanceId instance) {
    return Command{"unpack network instance", [instance](Document& candidate) { unpack(candidate, instance); }};
}

Command renameInterfaceCommand(NetworkId network, PortDirection direction, InterfacePortId port, std::string name) {
    return Command{"rename network interface", [network, direction, port, name = std::move(name)](Document& candidate) {
                       candidate.network(network).renameFormalPort(direction, port, name);
                   }};
}

Command promoteParameterCommand(NetworkId network, NodeId node, std::string key, std::string exposedName,
                                std::shared_ptr<InterfacePortId> created) {
    return Command{"promote network parameter", [network, node, key = std::move(key),
                                                 exposedName = std::move(exposedName), created](Document& candidate) {
                       const auto id = candidate.network(network).addExposedParameter(node, key, exposedName);
                       if (created)
                           *created = id;
                   }};
}

Command renameExposedParameterCommand(NetworkId network, InterfacePortId parameter, std::string name) {
    return Command{"rename exposed parameter", [network, parameter, name = std::move(name)](Document& candidate) {
                       candidate.network(network).renameExposedParameter(parameter, name);
                   }};
}

Command removeExposedParameterCommand(NetworkId network, InterfacePortId parameter) {
    return Command{"remove exposed parameter", [network, parameter](Document& candidate) {
                       candidate.network(network).removeExposedParameter(parameter);
                   }};
}
Command moveExposedParameterCommand(NetworkId network, InterfacePortId parameter, std::size_t index) {
    return Command{"reorder exposed parameter", [network, parameter, index](Document& candidate) {
                       candidate.network(network).moveExposedParameter(parameter, index);
                   }};
}
Command setInterfaceLayoutCommand(NetworkId network, PortDirection direction, InterfacePortId port,
                                  LayoutPosition layout) {
    return Command{"position network interface", [network, direction, port, layout](Document& candidate) {
                       candidate.network(network).setFormalPortLayout(direction, port, layout);
                   }};
}

Command disconnectInputCommand(NetworkId network, InterfacePortId input, PortRef destination) {
    return Command{"disconnect network input", [network, input, destination](Document& candidate) {
                       candidate.network(network).disconnectInput(input, destination);
                   }};
}

Command disconnectOutputCommand(NetworkId network, InterfacePortId output) {
    return Command{"disconnect network output",
                   [network, output](Document& candidate) { candidate.network(network).disconnectOutput(output); }};
}
Command connectInputToOutputCommand(NetworkId network, InterfacePortId input, InterfacePortId output) {
    return Command{"route network input to output", [network, input, output](Document& candidate) {
                       candidate.network(network).connectOutputToInput(output, input);
                   }};
}

Command replaceInputConnectionCommand(NetworkId network, InterfacePortId input, PortRef destination) {
    return Command{"replace network input connection", [network, input, destination](Document& candidate) {
                       auto& target = candidate.network(network);
                       const auto existing = target.inputConnections();
                       for (const auto& connection : existing)
                           if (connection.terminal == input)
                               target.disconnectInput(input, connection.node);
                       target.connectInput(input, destination);
                   }};
}

Command replaceOutputConnectionCommand(NetworkId network, InterfacePortId output, PortRef source) {
    return Command{"replace network output connection", [network, output, source](Document& candidate) {
                       auto& target = candidate.network(network);
                       target.disconnectOutput(output);
                       target.connectOutput(source, output);
                   }};
}
Command bindInstanceInputToParentTerminalCommand(NetworkInstanceId instance, InterfacePortId input,
                                                 InterfacePortId parentInput) {
    return Command{"bind instance input to parent terminal", [instance, input, parentInput](Document& candidate) {
                       candidate.bindInstanceInputToParentTerminal(instance, input, parentInput);
                   }};
}

Command unbindInstanceInputCommand(NetworkInstanceId instance, InterfacePortId input) {
    return Command{"unbind instance input",
                   [instance, input](Document& candidate) { candidate.eraseInstanceInputBinding(instance, input); }};
}

Command createLinkedInstanceCommand(NetworkId parentNetwork, NetworkId definition, std::string name,
                                    LayoutPosition position, std::shared_ptr<NetworkInstanceId> created) {
    return Command{"create linked network instance",
                   [parentNetwork, definition, name = std::move(name), position, created](Document& candidate) {
                       for (const auto& occurrence : candidate.instances())
                           if (occurrence.definition == definition && occurrence.ownsDefinition)
                               candidate.setInstanceOwnership(occurrence.id, false);
                       const auto id = candidate.addInstance(parentNetwork, definition, name);
                       const auto* occurrence = candidate.instance(id);
                       if (occurrence)
                           candidate.network(parentNetwork).graph().setLayout(occurrence->node, position);
                       if (created)
                           *created = id;
                   }};
}

Command makeIndependentCommand(NetworkInstanceId instance, std::shared_ptr<NetworkId> createdDefinition) {
    return Command{
        "make network instance independent", [instance, createdDefinition](Document& candidate) {
            const auto* occurrence = candidate.instance(instance);
            if (!occurrence)
                throw GraphException(GraphError::UnknownInstance, "cannot make unknown network instance independent");
            const auto originalDefinition = occurrence->definition;
            std::map<NodeId, NodeId> animationNodes;
            for (const auto& node : candidate.network(originalDefinition).graph().nodes())
                if (node.instance == kInvalidNetworkInstance)
                    animationNodes.emplace(node.id, node.id);
            const auto newDefinition = addNetworkLike(candidate, candidate.network(originalDefinition),
                                                      candidate.network(originalDefinition).name() + " Independent");
            candidate.remapAnimationChannels(originalDefinition, newDefinition, animationNodes, instance);
            candidate.setInstanceOwnership(instance, true);
            candidate.setInstanceDefinition(instance, newDefinition);
            if (createdDefinition)
                *createdDefinition = newDefinition;
        }};
}

Command copySelectionCommand(NetworkId sourceNetwork, std::vector<NodeId> selected, NetworkId destinationNetwork,
                             LayoutPosition offset, std::shared_ptr<std::vector<NodeId>> created) {
    return Command{"copy network selection", [sourceNetwork, selected = std::move(selected), destinationNetwork, offset,
                                              created](Document& candidate) {
                       std::vector<NodeId> ids;
                       copySelected(candidate, sourceNetwork, selected, destinationNetwork, offset, &ids);
                       if (created)
                           *created = std::move(ids);
                   }};
}

}  // namespace nemo
