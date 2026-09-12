#include "nemo/core/document/Document.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "nemo/core/Hashing.hpp"
#include "nemo/core/document/ParameterValue.hpp"

namespace nemo {
namespace {

void checkAllocatable(std::uint64_t value, const char* what) {
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max())
        throw GraphException(GraphError::InvalidId, std::string(what) + " identity space is exhausted");
}

void hashLayout(std::uint64_t& hash, LayoutPosition position) {
    hashMixWord(hash, std::bit_cast<std::uint64_t>(position.x));
    hashMixWord(hash, std::bit_cast<std::uint64_t>(position.y));
}

void hashPort(std::uint64_t& hash, const PortSpec& port) {
    hashMixWord(hash, static_cast<std::uint64_t>(port.kind));
    hashMixText(hash, port.name);
}

void hashFormalPort(std::uint64_t& hash, const FormalPort& port) {
    hashMixWord(hash, port.id);
    hashMixWord(hash, static_cast<std::uint64_t>(port.kind));
    hashMixText(hash, port.name);
    hashMixWord(hash, port.allowFanOut ? 1 : 0);
}

void hashPortRef(std::uint64_t& hash, PortRef ref) {
    hashMixWord(hash, ref.node);
    hashMixWord(hash, ref.port);
}
void hashParameterAddress(std::uint64_t& hash, const ParameterAddress& address) {
    hashMixWord(hash, address.network);
    hashMixWord(hash, address.node);
    hashMixText(hash, address.key);
    hashMixWord(hash, address.instance);
}

void hashAnimationKey(std::uint64_t& hash, const Keyframe& key) {
    hashMixWord(hash, key.id);
    hashMixWord(hash, std::bit_cast<std::uint64_t>(key.time));
    hashMixText(hash, canonicalParameterValue(key.value));
    hashMixWord(hash, static_cast<std::uint64_t>(key.interpolation));
    hashMixWord(hash, static_cast<std::uint64_t>(key.tangentMode));
    for (const double slope : key.inSlope)
        hashMixWord(hash, std::bit_cast<std::uint64_t>(slope));
    for (const double slope : key.outSlope)
        hashMixWord(hash, std::bit_cast<std::uint64_t>(slope));
}
std::string describePortRef(PortRef ref) {
    return "node " + std::to_string(ref.node) + " port " + std::to_string(ref.port);
}
void validateParameterEdit(const Document& document, const ParameterEdit& edit) {
    const auto& address = edit.address;
    if (address.network == kInvalidNetwork)
        throw GraphException(GraphError::InvalidNetwork, "parameter edit requires a network scope");
    if (address.node == kInvalidNode)
        throw GraphException(GraphError::UnknownNode, "parameter edit requires a node scope");
    if (address.key.empty())
        throw GraphException(GraphError::InvalidName, "parameter key must not be empty");

    if (address.instance == kInvalidNetworkInstance) {
        const auto& graph = document.network(address.network).graph();
        const auto* node = graph.node(address.node);
        if (!node)
            throw GraphException(GraphError::UnknownNode, "cannot edit unknown node " + std::to_string(address.node));
        if (edit.value) {
            if (const auto problem = graph.catalog().validateParameter(node->type, address.key, *edit.value))
                throw GraphException(GraphError::ParameterValue,
                                     "node '" + node->name + "' parameter '" + address.key + "': " + *problem);
        }
        return;
    }

    const auto* instance = document.instance(address.instance);
    if (!instance)
        throw GraphException(GraphError::UnknownInstance,
                             "cannot edit unknown network instance " + std::to_string(address.instance));
    if (address.network != instance->definition)
        throw GraphException(GraphError::InvalidInstance,
                             "instance parameter scope network " + std::to_string(address.network) +
                                 " does not match definition network " + std::to_string(instance->definition));
    const auto& graph = document.network(instance->definition).graph();
    const auto* node = graph.node(address.node);
    if (!node)
        throw GraphException(GraphError::UnknownNode, "instance parameter target node " + std::to_string(address.node) +
                                                          " is not in definition");
    if (edit.value) {
        if (const auto problem = graph.catalog().validateParameter(node->type, address.key, *edit.value))
            throw GraphException(GraphError::ParameterValue, "instance parameter target node " +
                                                                 std::to_string(address.node) + " key '" + address.key +
                                                                 "': " + *problem);
    }
}

void applyParameterEdit(Document& document, const ParameterEdit& edit) {
    validateParameterEdit(document, edit);
    const auto& address = edit.address;
    if (address.instance == kInvalidNetworkInstance) {
        auto& graph = document.network(address.network).graph();
        if (edit.value)
            graph.setParam(address.node, address.key, *edit.value);
        else
            graph.eraseParam(address.node, address.key);
    } else if (edit.value) {
        document.setInstanceParam(address.instance, address.node, address.key, *edit.value);
    } else {
        document.eraseInstanceParam(address.instance, address.node, address.key);
    }
}

}  // namespace

Document::Document() : Document(builtinNodeCatalogPtr()) {}

Document::Document(std::shared_ptr<const NodeCatalog> catalog) : catalog_(std::move(catalog)) {
    if (!catalog_)
        throw std::invalid_argument("document catalog must not be null");
    static_cast<void>(addNetworkWithId(1, "Root"));
}

Network* Document::findNetwork(NetworkId id) {
    const auto it =
        std::find_if(networks_.begin(), networks_.end(), [id](const Network& network) { return network.id() == id; });
    return it == networks_.end() ? nullptr : &*it;
}

const Network* Document::findNetwork(NetworkId id) const {
    const auto it =
        std::find_if(networks_.begin(), networks_.end(), [id](const Network& network) { return network.id() == id; });
    return it == networks_.end() ? nullptr : &*it;
}

const Network& Document::network(NetworkId id) const {
    const Network* found = findNetwork(id);
    if (!found)
        throw std::out_of_range("document has no network " + std::to_string(id));
    return *found;
}

Network& Document::network(NetworkId id) {
    Network* found = findNetwork(id);
    if (!found)
        throw std::out_of_range("document has no network " + std::to_string(id));
    return *found;
}

const std::vector<Network>& Document::networks() const {
    return networks_;
}

NetworkId Document::addNetwork(std::string name) {
    checkAllocatable(nextNetworkId_, "network");
    return addNetworkWithId(nextNetworkId_, std::move(name));
}
NetworkId Document::addNetworkWithId(NetworkId id, std::string name) {
    checkAllocatable(id, "network");
    if (findNetwork(id))
        throw GraphException(GraphError::DuplicateNetwork, "network id " + std::to_string(id) + " already exists");
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "network name must not be empty");
    const auto duplicate = std::find_if(networks_.begin(), networks_.end(),
                                        [&name](const Network& network) { return network.name() == name; });
    if (duplicate != networks_.end())
        throw GraphException(GraphError::DuplicateNetwork, "network name '" + name + "' already exists");
    networks_.emplace_back(id, std::move(name), catalog_);
    nextNetworkId_ = std::max(nextNetworkId_, static_cast<NetworkId>(id + 1));
    if (rootNetworkId_ == kInvalidNetwork)
        rootNetworkId_ = id;
    return id;
}

bool Document::networkDependsOn(NetworkId candidate, NetworkId target) const {
    if (candidate == target)
        return true;
    std::set<NetworkId> visited;
    std::vector<NetworkId> pending{candidate};
    while (!pending.empty()) {
        const NetworkId current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second)
            continue;
        for (const auto& instance : instances_) {
            if (instance.parentNetwork != current)
                continue;
            if (instance.definition == target)
                return true;
            pending.push_back(instance.definition);
        }
    }
    return false;
}

void Document::removeNetwork(NetworkId id) {
    if (id == rootNetworkId_)
        throw GraphException(GraphError::InvalidNetwork, "cannot remove the document root network");
    if (!findNetwork(id))
        throw GraphException(GraphError::UnknownNetwork, "cannot remove unknown network " + std::to_string(id));
    for (const auto& instance : instances_) {
        if (instance.parentNetwork == id || instance.definition == id)
            throw GraphException(GraphError::InvalidNetwork, "cannot remove network " + std::to_string(id) +
                                                                 " while network instance " +
                                                                 std::to_string(instance.id) + " references it");
    }
    const Network& removed = *findNetwork(id);
    retiredNetworkWatermarks_[id] =
        NetworkWatermarks{removed.graph().nextNodeId(), removed.graph().nextEdgeId(), removed.nextInterfacePortId()};
    networks_.erase(
        std::remove_if(networks_.begin(), networks_.end(), [id](const Network& network) { return network.id() == id; }),
        networks_.end());
}
void Document::setRootNetworkId(NetworkId id) {
    if (!findNetwork(id))
        throw GraphException(GraphError::UnknownNetwork, "cannot select unknown root network " + std::to_string(id));
    rootNetworkId_ = id;
}

const std::vector<NetworkInstance>& Document::instances() const {
    return instances_;
}

const NetworkInstance* Document::instance(NetworkInstanceId id) const {
    const auto it = std::find_if(instances_.begin(), instances_.end(),
                                 [id](const NetworkInstance& instance) { return instance.id == id; });
    return it == instances_.end() ? nullptr : &*it;
}

NetworkInstance* Document::findInstanceMutable(NetworkInstanceId id) {
    const auto it = std::find_if(instances_.begin(), instances_.end(),
                                 [id](const NetworkInstance& instance) { return instance.id == id; });
    return it == instances_.end() ? nullptr : &*it;
}

NetworkInstanceId Document::addInstance(NetworkId parentNetwork, NetworkId definition, std::string name) {
    checkAllocatable(nextInstanceId_, "network instance");
    return addInstanceWithId(nextInstanceId_, parentNetwork, definition, kInvalidNode, std::move(name));
}
NetworkInstanceId Document::addInstanceWithId(NetworkInstanceId id, NetworkId parentNetwork, NetworkId definition,
                                              NodeId node, std::string name,
                                              std::map<InterfacePortId, PortRef> inputBindings,
                                              std::map<NodeId, ParameterValues> params) {
    checkAllocatable(id, "network instance");
    if (instance(id))
        throw GraphException(GraphError::DuplicateId, "network instance id " + std::to_string(id) + " already exists");
    const Network& parent = network(parentNetwork);
    const Network& definitionNetwork = network(definition);
    if (networkDependsOn(definition, parentNetwork))
        throw GraphException(GraphError::Cycle, "network instance " + std::to_string(parentNetwork) + " -> " +
                                                    std::to_string(definition) +
                                                    " would create a nested dependency cycle");
    if (name.empty())
        throw GraphException(GraphError::InvalidName, "network instance name must not be empty");
    if (const auto* named = parent.graph().nodeByName(name); named != nullptr && named->id != node)
        throw GraphException(GraphError::DuplicateName,
                             "node name '" + name + "' already exists in network '" + parent.name() + "'");

    std::vector<PortSpec> inputPorts;
    std::vector<PortSpec> outputPorts;
    inputPorts.reserve(definitionNetwork.inputs().size());
    outputPorts.reserve(definitionNetwork.outputs().size());
    for (const auto& port : definitionNetwork.inputs())
        inputPorts.push_back(PortSpec{port.kind, port.name});
    for (const auto& port : definitionNetwork.outputs())
        outputPorts.push_back(PortSpec{port.kind, port.name});

    for (const auto& [terminal, source] : inputBindings) {
        const FormalPort* formal = definitionNetwork.input(terminal);
        if (!formal)
            throw GraphException(GraphError::PortType,
                                 "instance binding references unknown formal input " + std::to_string(terminal));
        const NodeInstance* sourceNode = parent.graph().node(source.node);
        if (!sourceNode)
            throw GraphException(GraphError::UnknownNode,
                                 "instance binding references unknown source node " + std::to_string(source.node));
        const auto& sourcePorts = parent.graph().outputPorts(source.node);
        if (static_cast<std::size_t>(source.port) >= sourcePorts.size() ||
            sourcePorts[source.port].kind != formal->kind)
            throw GraphException(GraphError::PortType, "instance binding for formal input '" + formal->name +
                                                           "' has incompatible source node " +
                                                           std::to_string(source.node) + " port " +
                                                           std::to_string(source.port));
    }

    for (const auto& [targetNode, parameterValues] : params) {
        const NodeInstance* target = definitionNetwork.graph().node(targetNode);
        if (!target)
            throw GraphException(GraphError::UnknownNode, "instance parameter target node " +
                                                              std::to_string(targetNode) + " is not in definition " +
                                                              std::to_string(definition));
        for (const auto& [key, value] : parameterValues) {
            if (key.empty())
                throw GraphException(GraphError::InvalidName, "instance parameter key must not be empty");
            if (const auto problem = definitionNetwork.graph().catalog().validateParameter(target->type, key, value))
                throw GraphException(GraphError::ParameterValue, "instance parameter target node " +
                                                                     std::to_string(targetNode) + " key '" + key +
                                                                     "': " + *problem);
        }
    }
    Network& mutableParent = network(parentNetwork);
    const NodeInstance* existing = node == kInvalidNode ? nullptr : mutableParent.graph().node(node);
    if (existing) {
        if (existing->instance != id || existing->definition != definition)
            throw GraphException(GraphError::InvalidInstance, "persisted node " + std::to_string(node) +
                                                                  " does not match network instance " +
                                                                  std::to_string(id));
    } else if (node != kInvalidNode) {
        throw GraphException(GraphError::UnknownNode,
                             "network instance references unknown persisted node " + std::to_string(node));
    } else {
        node = mutableParent.graph().addNodeWithId(mutableParent.graph().nextNodeId(), "network.instance", name, {}, {},
                                                   definition, id);
    }
    mutableParent.graph().setPortContract(node, std::move(inputPorts), std::move(outputPorts));
    for (const auto& binding : inputBindings) {
        const auto formalIndex =
            std::find_if(definitionNetwork.inputs().begin(), definitionNetwork.inputs().end(),
                         [terminal = binding.first](const FormalPort& candidate) { return candidate.id == terminal; });
        if (formalIndex != definitionNetwork.inputs().end())
            mutableParent.graph().reserveInput(PortRef{
                node, static_cast<std::uint32_t>(std::distance(definitionNetwork.inputs().begin(), formalIndex))});
    }
    instances_.push_back(NetworkInstance{.id = id,
                                         .parentNetwork = parentNetwork,
                                         .definition = definition,
                                         .node = node,
                                         .name = std::move(name),
                                         .inputBindings = std::move(inputBindings),
                                         .params = std::move(params)});
    nextInstanceId_ = std::max(nextInstanceId_, static_cast<NetworkInstanceId>(id + 1));
    return id;
}
void Document::removeInstance(NetworkInstanceId id) {
    NetworkInstance* target = findInstanceMutable(id);
    if (!target)
        throw GraphException(GraphError::UnknownInstance,
                             "cannot remove unknown network instance " + std::to_string(id));
    network(target->parentNetwork).graph().removeNode(target->node);
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [id](const NetworkInstance& instance) { return instance.id == id; }),
                     instances_.end());
}

void Document::bindInstanceInput(NetworkInstanceId id, InterfacePortId input, PortRef source) {
    NetworkInstance* target = findInstanceMutable(id);
    if (!target)
        throw GraphException(GraphError::UnknownInstance, "cannot bind unknown network instance " + std::to_string(id));
    const Network& definition = network(target->definition);
    const FormalPort* formal = definition.input(input);
    if (!formal)
        throw GraphException(GraphError::PortType,
                             "instance binding references unknown formal input " + std::to_string(input));
    Network& parent = network(target->parentNetwork);
    const NodeInstance* sourceNode = parent.graph().node(source.node);
    if (!sourceNode)
        throw GraphException(GraphError::UnknownNode,
                             "instance binding references unknown source node " + std::to_string(source.node));
    const auto& sourcePorts = parent.graph().outputPorts(source.node);
    if (static_cast<std::size_t>(source.port) >= sourcePorts.size() || sourcePorts[source.port].kind != formal->kind)
        throw GraphException(GraphError::PortType, "instance binding for formal input '" + formal->name +
                                                       "' has incompatible source endpoint " + describePortRef(source));
    const auto sourceInstance =
        std::find_if(instances_.begin(), instances_.end(), [&](const NetworkInstance& candidate) {
            return candidate.parentNetwork == target->parentNetwork && candidate.node == source.node;
        });
    if (sourceInstance != instances_.end() &&
        (sourceInstance->id == id || instanceBindingDependsOn(sourceInstance->id, id)))
        throw GraphException(GraphError::Cycle, "binding instance " + std::to_string(id) + " from " +
                                                    std::to_string(sourceInstance->id) + " would create a cycle");
    const auto formalIndex = std::find_if(definition.inputs().begin(), definition.inputs().end(),
                                          [input](const FormalPort& candidate) { return candidate.id == input; });
    if (formalIndex == definition.inputs().end())
        throw GraphException(GraphError::PortType,
                             "instance binding references unknown formal input " + std::to_string(input));
    const PortRef destination{target->node,
                              static_cast<std::uint32_t>(std::distance(definition.inputs().begin(), formalIndex))};
    const auto existingBinding = target->inputBindings.find(input);
    if (existingBinding != target->inputBindings.end()) {
        existingBinding->second = source;
        return;
    }
    target->inputBindings.emplace(input, source);
    try {
        parent.graph().reserveInput(destination);
    } catch (...) {
        target->inputBindings.erase(input);
        throw;
    }
}
void Document::eraseInstanceInputBinding(NetworkInstanceId id, InterfacePortId input) {
    NetworkInstance* target = findInstanceMutable(id);
    if (!target)
        throw GraphException(GraphError::UnknownInstance, "cannot edit unknown network instance " + std::to_string(id));
    const Network& definition = network(target->definition);
    const auto formal = definition.input(input);
    if (formal) {
        const auto formalIndex = std::find_if(definition.inputs().begin(), definition.inputs().end(),
                                              [input](const FormalPort& candidate) { return candidate.id == input; });
        if (formalIndex != definition.inputs().end())
            network(target->parentNetwork)
                .graph()
                .releaseInput(PortRef{
                    target->node, static_cast<std::uint32_t>(std::distance(definition.inputs().begin(), formalIndex))});
    }
    target->inputBindings.erase(input);
}

void Document::setInstanceParam(NetworkInstanceId id, NodeId targetNode, std::string key, ParameterValue value) {
    NetworkInstance* target = findInstanceMutable(id);
    if (!target)
        throw GraphException(GraphError::UnknownInstance,
                             "cannot set a parameter on unknown network instance " + std::to_string(id));
    if (key.empty())
        throw GraphException(GraphError::InvalidName, "instance parameter key must not be empty");
    const Network& definition = network(target->definition);
    const NodeInstance* node = definition.graph().node(targetNode);
    if (!node)
        throw GraphException(GraphError::UnknownNode,
                             "instance parameter target node " + std::to_string(targetNode) + " is not in definition");
    if (const auto problem = definition.graph().catalog().validateParameter(node->type, key, value))
        throw GraphException(GraphError::ParameterValue, "instance parameter target node " +
                                                             std::to_string(targetNode) + " key '" + key +
                                                             "': " + *problem);
    target->params[targetNode][std::move(key)] = std::move(value);
}

void Document::eraseInstanceParam(NetworkInstanceId id, NodeId targetNode, const std::string& key) {
    NetworkInstance* target = findInstanceMutable(id);
    if (!target)
        throw GraphException(GraphError::UnknownInstance,
                             "cannot erase a parameter on unknown network instance " + std::to_string(id));
    if (key.empty())
        throw GraphException(GraphError::InvalidName, "instance parameter key must not be empty");
    const Network& definition = network(target->definition);
    if (!definition.graph().node(targetNode))
        throw GraphException(GraphError::UnknownNode,
                             "instance parameter target node " + std::to_string(targetNode) + " is not in definition");
    const auto targetIt = target->params.find(targetNode);
    if (targetIt == target->params.end())
        return;
    targetIt->second.erase(key);
    if (targetIt->second.empty())
        target->params.erase(targetIt);
}

bool Document::instanceBindingDependsOn(NetworkInstanceId origin, NetworkInstanceId target) const {
    std::set<NetworkInstanceId> visited;
    std::vector<NetworkInstanceId> pending{origin};
    while (!pending.empty()) {
        const NetworkInstanceId currentId = pending.back();
        pending.pop_back();
        if (!visited.insert(currentId).second)
            continue;
        const auto currentIt =
            std::find_if(instances_.begin(), instances_.end(),
                         [currentId](const NetworkInstance& candidate) { return candidate.id == currentId; });
        const NetworkInstance* current = currentIt == instances_.end() ? nullptr : &*currentIt;
        if (!current)
            continue;
        for (const auto& [unusedTerminal, source] : current->inputBindings) {
            const auto nested =
                std::find_if(instances_.begin(), instances_.end(), [&](const NetworkInstance& candidate) {
                    return candidate.parentNetwork == current->parentNetwork && candidate.node == source.node;
                });
            if (nested == instances_.end())
                continue;
            if (nested->id == target)
                return true;
            pending.push_back(nested->id);
        }
    }
    return false;
}
void Document::synchronizeReferences() {
    for (auto& networkValue : networks_)
        networkValue.syncTerminalConnections();

    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [&](const NetworkInstance& value) {
                                        const Network* parent = findNetwork(value.parentNetwork);
                                        const Network* definition = findNetwork(value.definition);
                                        return parent == nullptr || definition == nullptr ||
                                               parent->graph().node(value.node) == nullptr;
                                    }),
                     instances_.end());
    for (auto& value : instances_) {
        Network* parent = findNetwork(value.parentNetwork);
        const Network* definition = findNetwork(value.definition);
        if (!parent || !definition)
            continue;
        const NodeInstance* nestedNode = parent->graph().node(value.node);
        if (!nestedNode)
            continue;
        std::vector<PortSpec> inputs;
        std::vector<PortSpec> outputs;
        for (const auto& port : definition->inputs())
            inputs.push_back(PortSpec{port.kind, port.name});
        for (const auto& port : definition->outputs())
            outputs.push_back(PortSpec{port.kind, port.name});
        const bool contractChanged =
            nestedNode->inputPorts != inputs || nestedNode->outputPorts != outputs || !nestedNode->hasPortContract;
        if (contractChanged) {
            parent->graph().clearInputReservations(value.node);
            parent->graph().setPortContract(value.node, std::move(inputs), std::move(outputs));
        }
        for (auto binding = value.inputBindings.begin(); binding != value.inputBindings.end();) {
            const auto formal = definition->input(binding->first);
            const auto formalIndex =
                std::find_if(definition->inputs().begin(), definition->inputs().end(),
                             [id = binding->first](const FormalPort& candidate) { return candidate.id == id; });
            const NodeInstance* source = parent->graph().node(binding->second.node);
            const auto& sourcePorts = parent->graph().outputPorts(binding->second.node);
            const bool valid = formal && source && formalIndex != definition->inputs().end() &&
                               static_cast<std::size_t>(binding->second.port) < sourcePorts.size() &&
                               sourcePorts[binding->second.port].kind == formal->kind;
            if (!valid) {
                if (formalIndex != definition->inputs().end()) {
                    const PortRef destination{value.node, static_cast<std::uint32_t>(std::distance(
                                                              definition->inputs().begin(), formalIndex))};
                    if (parent->graph().inputReserved(destination))
                        parent->graph().releaseInput(destination);
                }
                binding = value.inputBindings.erase(binding);
                continue;
            }
            const PortRef destination{
                value.node, static_cast<std::uint32_t>(std::distance(definition->inputs().begin(), formalIndex))};
            if (!parent->graph().inputReserved(destination))
                parent->graph().reserveInput(destination);
            ++binding;
        }
    }
    animationChannels_.erase(
        std::remove_if(animationChannels_.begin(), animationChannels_.end(),
                       [&](const AnimationChannel& channel) {
                           const auto& address = channel.address;
                           const Network* network = findNetwork(address.network);
                           if (!network)
                               return true;
                           const NodeInstance* node = network->graph().node(address.node);
                           if (!node || network->graph().catalog().parameterSpec(node->type, address.key) == nullptr)
                               return true;
                           if (address.instance != kInvalidNetworkInstance) {
                               const NetworkInstance* occurrence = instance(address.instance);
                               if (!occurrence || occurrence->definition != address.network)
                                   return true;
                           }
                           return false;
                       }),
        animationChannels_.end());
}

void Document::restoreIdentityHighWatermarks(NetworkId nextNetworkId, NetworkInstanceId nextInstanceId) {
    if (nextNetworkId == kInvalidNetwork || nextInstanceId == kInvalidNetworkInstance)
        throw GraphException(GraphError::InvalidId, "document identity high watermarks must be nonzero");
    nextNetworkId_ = std::max(nextNetworkId_, nextNetworkId);
    nextInstanceId_ = std::max(nextInstanceId_, nextInstanceId);
}
void Document::restoreMediaIdentityHighWatermarks(MediaSourceId nextSourceId, MediaBinId nextBinId) {
    mediaCatalog.restoreIdentityHighWatermarks(nextSourceId, nextBinId);
}

void Document::preserveIdentityHighWatermarksFrom(const Document& source) {
    restoreIdentityHighWatermarks(source.nextNetworkId_, source.nextInstanceId_);
    mediaCatalog.preserveIdentityHighWatermarksFrom(source.mediaCatalog);
    for (const auto& [id, watermark] : source.retiredNetworkWatermarks_) {
        auto& candidate = retiredNetworkWatermarks_[id];
        candidate.nextNodeId = std::max(candidate.nextNodeId, watermark.nextNodeId);
        candidate.nextEdgeId = std::max(candidate.nextEdgeId, watermark.nextEdgeId);
        candidate.nextInterfacePortId = std::max(candidate.nextInterfacePortId, watermark.nextInterfacePortId);
    }
    for (const auto& candidateNetwork : networks_) {
        Network* mutableNetwork = findNetwork(candidateNetwork.id());
        const Network* sourceNetwork = source.findNetwork(candidateNetwork.id());
        if (sourceNetwork) {
            mutableNetwork->restoreIdentityHighWatermarks(sourceNetwork->graph().nextNodeId(),
                                                          sourceNetwork->graph().nextEdgeId(),
                                                          sourceNetwork->nextInterfacePortId());
            continue;
        }
        const auto retired = source.retiredNetworkWatermarks_.find(candidateNetwork.id());
        if (retired != source.retiredNetworkWatermarks_.end())
            mutableNetwork->restoreIdentityHighWatermarks(retired->second.nextNodeId, retired->second.nextEdgeId,
                                                          retired->second.nextInterfacePortId);
    }
    nextAnimationChannelId_ = std::max(nextAnimationChannelId_, source.nextAnimationChannelId_);
    nextKeyframeId_ = std::max(nextKeyframeId_, source.nextKeyframeId_);
}

std::uint64_t Document::stateRevision() const {
    std::uint64_t hash = kFnv1a64Basis;
    hashMixWord(hash, freshnessRevision_);
    hashMixWord(hash, rootNetworkId_);
    hashMixWord(hash, nextNetworkId_);
    hashMixWord(hash, nextInstanceId_);
    hashMixText(hash, color.workingSpace);
    hashMixText(hash, color.viewerTransform);
    hashMixText(hash, color.deliveryTransform);
    hashMixText(hash, name);
    for (const auto& [key, source] : sources) {
        hashMixText(hash, key);
        hashMixText(hash, source.path);
        hashMixWord(hash, static_cast<std::uint64_t>(source.frameOffset));
        hashMixWord(hash, static_cast<std::uint64_t>(source.frameStep));
        hashMixWord(hash, source.revision);
        for (const auto& [tag, value] : source.interpretation) {
            hashMixText(hash, tag);
            hashMixText(hash, value);
        }
        hashMixWord(hash, static_cast<std::uint64_t>(source.interpretation.size()));
    }
    hashMixWord(hash, static_cast<std::uint64_t>(sources.size()));

    hashMixWord(hash, mediaCatalog.stateHash());

    for (const auto& networkValue : networks_) {
        hashMixWord(hash, networkValue.id());
        hashMixText(hash, networkValue.name());
        hashMixWord(hash, networkValue.defaultOutput());
        for (const auto& port : networkValue.inputs())
            hashFormalPort(hash, port);
        hashMixWord(hash, static_cast<std::uint64_t>(networkValue.inputs().size()));
        for (const auto& port : networkValue.outputs())
            hashFormalPort(hash, port);
        hashMixWord(hash, static_cast<std::uint64_t>(networkValue.outputs().size()));
        for (const auto& terminal : networkValue.inputConnections()) {
            hashMixWord(hash, terminal.terminal);
            hashPortRef(hash, terminal.node);
        }
        for (const auto& terminal : networkValue.outputConnections()) {
            hashMixWord(hash, terminal.terminal);
            hashPortRef(hash, terminal.node);
        }
        for (const auto& nodeValue : networkValue.graph().nodes()) {
            hashMixWord(hash, nodeValue.id);
            hashMixText(hash, nodeValue.type);
            hashMixText(hash, nodeValue.name);
            hashLayout(hash, nodeValue.layout);
            hashMixWord(hash, nodeValue.definition);
            hashMixWord(hash, nodeValue.instance);
            hashMixWord(hash, nodeValue.hasPortContract ? 1 : 0);
            for (const auto& port : nodeValue.inputPorts)
                hashPort(hash, port);
            hashMixWord(hash, static_cast<std::uint64_t>(nodeValue.inputPorts.size()));
            for (const auto& port : nodeValue.outputPorts)
                hashPort(hash, port);
            hashMixWord(hash, static_cast<std::uint64_t>(nodeValue.outputPorts.size()));
            for (const auto& [key, value] : nodeValue.params) {
                hashMixText(hash, key);
                hashMixText(hash, canonicalParameterValue(value));
            }
            hashMixWord(hash, static_cast<std::uint64_t>(nodeValue.params.size()));
        }
        for (const auto& edge : networkValue.graph().edges()) {
            hashMixWord(hash, edge.id);
            hashPortRef(hash, edge.from);
            hashPortRef(hash, edge.to);
            for (const auto& point : edge.route)
                hashLayout(hash, point);
            hashMixWord(hash, static_cast<std::uint64_t>(edge.route.size()));
        }
        hashMixWord(hash, static_cast<std::uint64_t>(networkValue.graph().nodes().size()));
        hashMixWord(hash, static_cast<std::uint64_t>(networkValue.graph().edges().size()));
    }
    hashMixWord(hash, static_cast<std::uint64_t>(networks_.size()));
    for (const auto& [id, watermark] : retiredNetworkWatermarks_) {
        hashMixWord(hash, id);
        hashMixWord(hash, watermark.nextNodeId);
        hashMixWord(hash, watermark.nextEdgeId);
        hashMixWord(hash, watermark.nextInterfacePortId);
    }
    hashMixWord(hash, static_cast<std::uint64_t>(retiredNetworkWatermarks_.size()));
    for (const auto& value : instances_) {
        hashMixWord(hash, value.id);
        hashMixWord(hash, value.parentNetwork);
        hashMixWord(hash, value.definition);
        hashMixWord(hash, value.node);
        hashMixText(hash, value.name);
        for (const auto& [port, source] : value.inputBindings) {
            hashMixWord(hash, port);
            hashPortRef(hash, source);
        }
        for (const auto& [targetNode, parameterValues] : value.params) {
            hashMixWord(hash, targetNode);
            for (const auto& [key, parameter] : parameterValues) {
                hashMixText(hash, key);
                hashMixText(hash, canonicalParameterValue(parameter));
            }
            hashMixWord(hash, static_cast<std::uint64_t>(parameterValues.size()));
        }
        hashMixWord(hash, static_cast<std::uint64_t>(value.params.size()));
    }
    hashMixWord(hash, static_cast<std::uint64_t>(instances_.size()));
    hashMixWord(hash, nextAnimationChannelId_);
    hashMixWord(hash, nextKeyframeId_);
    for (const auto& channel : animationChannels_) {
        hashMixWord(hash, channel.id);
        hashParameterAddress(hash, channel.address);
        for (const auto& key : channel.keys)
            hashAnimationKey(hash, key);
        hashMixWord(hash, static_cast<std::uint64_t>(channel.keys.size()));
    }
    hashMixWord(hash, static_cast<std::uint64_t>(animationChannels_.size()));
    return hash;
}

std::int64_t SourceReference::frameAt(std::int64_t localTime) const {
    if (frameStep == 0)
        throw std::runtime_error("source '" + path + "': frameStep must not be zero");
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const bool overflow =
        localTime > 0
            ? (frameStep > 0 ? localTime > maximum / frameStep : frameStep < minimum / localTime)
            : (localTime < 0 && (frameStep > 0 ? localTime < minimum / frameStep : localTime < maximum / frameStep));
    if (overflow)
        throw std::runtime_error("source '" + path + "': frame mapping multiplication overflows the 64-bit range");
    const auto product = localTime * frameStep;
    if ((product > 0 && frameOffset > maximum - product) || (product < 0 && frameOffset < minimum - product))
        throw std::runtime_error("source '" + path + "': frame mapping overflows the 64-bit range");
    const auto frame = frameOffset + product;
    if (frame < 0)
        throw std::runtime_error("source '" + path + "': local time " + std::to_string(localTime) +
                                 " maps to negative source frame " + std::to_string(frame));
    return frame;
}

CommandStack::CommandStack(Document& document, std::size_t capacity) : document_(document), capacity_(capacity) {
    undo_.reserve(capacity_);
    redo_.reserve(capacity_);
}

void CommandStack::prepare(Document& candidate) const {
    if (document_.freshnessRevision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("document freshness revision exhausted");
    candidate.synchronizeReferences();
    candidate.freshnessRevision_ = document_.freshnessRevision_ + 1;
    candidate.preserveIdentityHighWatermarksFrom(document_);
}

void CommandStack::push(Command command, const BeforeCommit& beforeCommit) {
    if (!command.apply)
        throw std::invalid_argument("command must provide an apply operation");
    Document candidate = document_;
    command.apply(candidate);
    prepare(candidate);
    if (beforeCommit)
        beforeCommit(document_, candidate);
    static_assert(std::is_nothrow_move_assignable_v<Document>);
    static_assert(std::is_nothrow_move_constructible_v<Document>);
    if (capacity_ != 0) {
        if (undo_.size() == capacity_)
            undo_.erase(undo_.begin());
        undo_.push_back(std::move(document_));
    }
    document_ = std::move(candidate);
    redo_.clear();
}

bool CommandStack::undo(const BeforeCommit& beforeCommit) {
    if (undo_.empty())
        return false;
    Document candidate = undo_.back();
    prepare(candidate);
    if (beforeCommit)
        beforeCommit(document_, candidate);
    redo_.push_back(std::move(document_));
    document_ = std::move(candidate);
    undo_.pop_back();
    return true;
}

bool CommandStack::redo(const BeforeCommit& beforeCommit) {
    if (redo_.empty())
        return false;
    Document candidate = redo_.back();
    prepare(candidate);
    if (beforeCommit)
        beforeCommit(document_, candidate);
    undo_.push_back(std::move(document_));
    document_ = std::move(candidate);
    redo_.pop_back();
    return true;
}

void CommandStack::clear() {
    undo_.clear();
    redo_.clear();
}
Command setParamCommand(NetworkId network, NodeId nodeId, std::string key, ParameterValue value) {
    return Command{"set " + key + " on node " + std::to_string(nodeId),
                   [network, nodeId, key = std::move(key), value = std::move(value)](Document& document) {
                       const ParameterEdit edit{ParameterAddress{network, nodeId, key}, value};
                       validateParameterEdit(document, edit);
                       document.network(network).graph().setParam(nodeId, key, value);
                   }};
}

Command resetParamCommand(NetworkId network, NodeId nodeId, std::string key) {
    return Command{"reset " + key + " on node " + std::to_string(nodeId),
                   [network, nodeId, key = std::move(key)](Document& document) {
                       const ParameterEdit edit{ParameterAddress{network, nodeId, key}, std::nullopt};
                       validateParameterEdit(document, edit);
                       document.network(network).graph().eraseParam(nodeId, key);
                   }};
}

Command setParametersCommand(std::vector<ParameterEdit> edits) {
    if (edits.empty())
        throw std::invalid_argument("parameter batch must contain at least one edit");
    return Command{"set parameters", [edits = std::move(edits)](Document& document) {
                       for (const auto& edit : edits)
                           validateParameterEdit(document, edit);
                       for (const auto& edit : edits)
                           applyParameterEdit(document, edit);
                   }};
}
Command renameNodeCommand(NetworkId network, NodeId nodeId, std::string name) {
    return Command{"rename node " + std::to_string(nodeId),
                   [network, nodeId, name = std::move(name)](Document& document) {
                       document.network(network).graph().renameNode(nodeId, name);
                   }};
}

Command setLayoutCommand(NetworkId network, NodeId nodeId, LayoutPosition position) {
    return Command{"position node " + std::to_string(nodeId), [network, nodeId, position](Document& document) {
                       document.network(network).graph().setLayout(nodeId, position);
                   }};
}
Command setLayoutsCommand(NetworkId network, std::vector<LayoutEdit> edits) {
    if (edits.empty())
        throw std::invalid_argument("layout batch must contain at least one edit");
    return Command{"position nodes", [network, edits = std::move(edits)](Document& document) {
                       auto& graph = document.network(network).graph();
                       std::set<NodeId> seen;
                       for (const auto& edit : edits) {
                           if (!seen.insert(edit.node).second)
                               throw GraphException(GraphError::DuplicateId, "layout batch targets node " +
                                                                                 std::to_string(edit.node) +
                                                                                 " more than once");
                           if (!graph.node(edit.node))
                               throw GraphException(GraphError::UnknownNode,
                                                    "layout batch targets unknown node " + std::to_string(edit.node));
                       }
                       for (const auto& edit : edits)
                           graph.setLayout(edit.node, edit.position);
                   }};
}

Command setRouteCommand(NetworkId network, EdgeId edgeId, std::vector<LayoutPosition> route) {
    return Command{"route edge " + std::to_string(edgeId),
                   [network, edgeId, route = std::move(route)](Document& document) {
                       document.network(network).graph().setRoute(edgeId, route);
                   }};
}

Command insertRoutePointCommand(NetworkId network, EdgeId edgeId, std::size_t index, LayoutPosition position) {
    return Command{"insert route point on edge " + std::to_string(edgeId),
                   [network, edgeId, index, position](Document& document) {
                       auto& graph = document.network(network).graph();
                       const auto edge = std::find_if(graph.edges().begin(), graph.edges().end(),
                                                      [edgeId](const Edge& value) { return value.id == edgeId; });
                       if (edge == graph.edges().end())
                           throw GraphException(GraphError::UnknownEdge,
                                                "cannot insert route point on unknown edge " + std::to_string(edgeId));
                       if (index > edge->route.size())
                           throw GraphException(GraphError::InvalidId, "route point index " + std::to_string(index) +
                                                                           " is outside edge " +
                                                                           std::to_string(edgeId) + " route");
                       auto route = edge->route;
                       route.insert(route.begin() + static_cast<std::ptrdiff_t>(index), position);
                       graph.setRoute(edgeId, std::move(route));
                   }};
}

Command moveRoutePointCommand(NetworkId network, EdgeId edgeId, std::size_t index, LayoutPosition position) {
    return Command{
        "move route point on edge " + std::to_string(edgeId), [network, edgeId, index, position](Document& document) {
            auto& graph = document.network(network).graph();
            const auto edge = std::find_if(graph.edges().begin(), graph.edges().end(),
                                           [edgeId](const Edge& value) { return value.id == edgeId; });
            if (edge == graph.edges().end())
                throw GraphException(GraphError::UnknownEdge,
                                     "cannot move route point on unknown edge " + std::to_string(edgeId));
            if (index >= edge->route.size())
                throw GraphException(GraphError::InvalidId, "route point index " + std::to_string(index) +
                                                                " is not present on edge " + std::to_string(edgeId));
            auto route = edge->route;
            route[index] = position;
            graph.setRoute(edgeId, std::move(route));
        }};
}

Command removeRoutePointCommand(NetworkId network, EdgeId edgeId, std::size_t index) {
    return Command{
        "remove route point on edge " + std::to_string(edgeId), [network, edgeId, index](Document& document) {
            auto& graph = document.network(network).graph();
            const auto edge = std::find_if(graph.edges().begin(), graph.edges().end(),
                                           [edgeId](const Edge& value) { return value.id == edgeId; });
            if (edge == graph.edges().end())
                throw GraphException(GraphError::UnknownEdge,
                                     "cannot remove route point on unknown edge " + std::to_string(edgeId));
            if (index >= edge->route.size())
                throw GraphException(GraphError::InvalidId, "route point index " + std::to_string(index) +
                                                                " is not present on edge " + std::to_string(edgeId));
            auto route = edge->route;
            route.erase(route.begin() + static_cast<std::ptrdiff_t>(index));
            graph.setRoute(edgeId, std::move(route));
        }};
}

Command setDefaultOutputCommand(NetworkId network, NodeId output) {
    return Command{"select output " + std::to_string(output),
                   [network, output](Document& document) { document.network(network).setDefaultOutput(output); }};
}

Command assignViewerCommand(NetworkId network, std::size_t viewerIndex, NodeId sourceNode,
                            std::shared_ptr<NodeId> createdViewer) {
    return Command{
        "assign viewer " + std::to_string(viewerIndex),
        [network, viewerIndex, sourceNode, createdViewer](Document& document) {
            Graph& graph = document.network(network).graph();

            // Viewer occurrences are ordered by identity so viewerIndex stays
            // stable across renames and unrelated insertions.
            const auto collectViewers = [](const Graph& candidate) {
                std::vector<NodeId> viewers;
                for (const auto& node : candidate.nodes()) {
                    const NodeDescriptor* descriptor = candidate.descriptor(node.type);
                    if (descriptor != nullptr && descriptor->type == "viewer")
                        viewers.push_back(node.id);
                }
                std::sort(viewers.begin(), viewers.end());
                return viewers;
            };

            // Validate and stage the complete edit on a trial graph: a failed
            // creation or rejected relationship leaves the document untouched.
            Graph trial = graph;
            std::vector<NodeId> viewers = collectViewers(trial);
            std::vector<NodeId> created;
            LayoutPosition sourceLayout{};
            bool hasSourceLayout = false;
            if (const NodeInstance* source = trial.node(sourceNode)) {
                sourceLayout = source->layout;
                hasSourceLayout = true;
            }
            while (viewers.size() <= viewerIndex && sourceNode != kInvalidNode) {
                const std::size_t ordinal = viewers.size();
                std::size_t suffix = ordinal + 1;
                std::string name = "Viewer" + std::to_string(suffix);
                while (trial.nodeByName(name) != nullptr)
                    name = "Viewer" + std::to_string(++suffix);
                LayoutPosition position{0.0, 60.0 * static_cast<double>(ordinal + 1)};
                if (hasSourceLayout)
                    position = LayoutPosition{sourceLayout.x, sourceLayout.y + 60.0 * static_cast<double>(ordinal + 1)};
                const NodeId inserted = trial.addNodeWithId(trial.nextNodeId(), "viewer", name, {}, position);
                created.push_back(inserted);
                viewers.push_back(inserted);
            }

            // Detaching a viewer that does not exist is a no-op; it must never
            // fabricate a viewer occurrence.
            if (viewers.size() <= viewerIndex)
                return;

            const PortRef viewerInput{viewers[viewerIndex], 0};
            EdgeId occupant = kInvalidEdge;
            PortRef occupantSource{};
            for (const auto& edge : trial.edges()) {
                if (edge.to == viewerInput) {
                    occupant = edge.id;
                    occupantSource = edge.from;
                    break;
                }
            }

            if (sourceNode == kInvalidNode) {
                if (occupant != kInvalidEdge)
                    trial.disconnect(occupant);
            } else {
                const PortRef sourceOutput{sourceNode, 0};
                const bool attached = occupant != kInvalidEdge && occupantSource == sourceOutput;
                if (occupant != kInvalidEdge)
                    trial.disconnect(occupant);
                // Re-assigning the attached source toggles the edge off.
                if (!attached)
                    static_cast<void>(trial.connect(sourceOutput, viewerInput));
            }

            graph = std::move(trial);
            if (createdViewer && !created.empty())
                *createdViewer = created.back();
        }};
}

Command connectInputCommand(NetworkId network, InterfacePortId input, PortRef destination) {
    return Command{"connect formal input " + std::to_string(input), [network, input, destination](Document& document) {
                       document.network(network).connectInput(input, destination);
                   }};
}

Command connectOutputCommand(NetworkId network, PortRef source, InterfacePortId output) {
    return Command{"connect formal output " + std::to_string(output), [network, source, output](Document& document) {
                       document.network(network).connectOutput(source, output);
                   }};
}

Command addNodeCommand(NetworkId network, std::string type, std::string name, std::shared_ptr<NodeId> createdId,
                       LayoutPosition position, NodeId anchor, std::vector<LayoutEdit> shiftedNodes) {
    return Command{
        "add node '" + name + "'", [network, type = std::move(type), name = std::move(name), createdId, position,
                                    anchor, shiftedNodes = std::move(shiftedNodes)](Document& document) {
            auto& graph = document.network(network).graph();

            // Validate all supplied layout edits before changing the candidate.
            // The command stack already applies against a private document copy,
            // but this also keeps direct command application failure-atomic.
            std::set<NodeId> shifted;
            for (const auto& edit : shiftedNodes) {
                if (!shifted.insert(edit.node).second)
                    throw GraphException(GraphError::DuplicateId, "node layout batch targets node " +
                                                                      std::to_string(edit.node) + " more than once");
                if (!graph.node(edit.node))
                    throw GraphException(GraphError::UnknownNode,
                                         "node layout batch targets unknown node " + std::to_string(edit.node));
            }

            Graph candidate = graph;
            const NodeId inserted = candidate.addNodeWithId(candidate.nextNodeId(), type, name, {}, position);

            // Selected creation is deliberately tolerant of stale or
            // incompatible anchors: QML may have rendered against an older
            // snapshot, so such a request remains a valid disconnected node.
            if (anchor != kInvalidNode && candidate.node(anchor) != nullptr &&
                !candidate.inputPorts(inserted).empty() && !candidate.outputPorts(anchor).empty()) {
                const PortRef anchorOutput{anchor, 0};
                const PortRef insertedInput{inserted, 0};
                if (!candidate.validateEdge(anchorOutput, insertedInput)) {
                    Graph routed = candidate;
                    std::vector<Edge> fanout;
                    for (const auto& edge : routed.edges())
                        if (edge.from == anchorOutput)
                            fanout.push_back(edge);

                    if (!routed.outputPorts(inserted).empty()) {
                        for (const auto& edge : fanout)
                            routed.disconnect(edge.id);
                        static_cast<void>(routed.connect(anchorOutput, insertedInput));
                        for (const auto& edge : fanout)
                            static_cast<void>(routed.connect(PortRef{inserted, 0}, edge.to));
                    } else {
                        // A sink is a branch: keep every existing fanout and
                        // add only the anchor-to-sink edge.
                        static_cast<void>(routed.connect(anchorOutput, insertedInput));
                    }
                    candidate = std::move(routed);
                }
            }

            for (const auto& edit : shiftedNodes)
                candidate.setLayout(edit.node, edit.position);
            graph = std::move(candidate);
            if (createdId)
                *createdId = inserted;
        }};
}

Command connectCommand(NetworkId network, PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId) {
    return Command{"connect node " + std::to_string(from.node) + " to " + std::to_string(to.node),
                   [network, from, to, createdId](Document& document) {
                       const EdgeId id = document.network(network).graph().connect(from, to);
                       if (createdId)
                           *createdId = id;
                   }};
}

Command removeNodeCommand(NetworkId network, NodeId nodeId) {
    return Command{"remove node " + std::to_string(nodeId), [network, nodeId](Document& document) {
                       auto& scoped = document.network(network);
                       auto& graph = scoped.graph();
                       const auto* node = graph.node(nodeId);
                       if (!node)
                           throw GraphException(GraphError::UnknownNode, "cannot remove unknown node " +
                                                                             std::to_string(nodeId) + " from network " +
                                                                             std::to_string(network));
                       if (scoped.defaultOutput() == nodeId)
                           throw GraphException(GraphError::InvalidNetwork,
                                                "cannot remove formal output terminal node " + std::to_string(nodeId));

                       const NetworkInstanceId instanceId = node->instance;
                       auto channels = document.animationChannels();
                       const bool removesAnimation =
                           std::erase_if(channels, [network, nodeId, instanceId](const AnimationChannel& channel) {
                               return (channel.address.network == network && channel.address.node == nodeId) ||
                                      (instanceId != kInvalidNetworkInstance && channel.address.instance == instanceId);
                           }) != 0;
                       // Validate and publish animation cleanup first. Graph
                       // removal is non-throwing after the node lookup above,
                       // keeping direct command application atomic on failure.
                       if (removesAnimation)
                           document.restoreAnimationChannels(std::move(channels), document.nextAnimationChannelId(),
                                                             document.nextKeyframeId());
                       if (instanceId != kInvalidNetworkInstance)
                           document.removeInstance(instanceId);
                       else
                           graph.removeNode(nodeId);
                   }};
}

Command disconnectCommand(NetworkId network, EdgeId edgeId) {
    return Command{"disconnect edge " + std::to_string(edgeId),
                   [network, edgeId](Document& document) { document.network(network).graph().disconnect(edgeId); }};
}

Command replaceInputCommand(NetworkId network, PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId) {
    return Command{"replace input node " + std::to_string(to.node) + " port " + std::to_string(to.port),
                   [network, from, to, createdId](Document& document) {
                       auto& graph = document.network(network).graph();
                       const auto existing = std::find_if(graph.edges().begin(), graph.edges().end(),
                                                          [to](const Edge& edge) { return edge.to == to; });
                       if (existing == graph.edges().end())
                           throw GraphException(GraphError::UnknownEdge, "cannot replace unoccupied input node " +
                                                                             std::to_string(to.node) + " port " +
                                                                             std::to_string(to.port));

                       // Validate the complete replacement before changing the live
                       // candidate, so direct command application is atomic too.
                       Graph trial = graph;
                       trial.disconnect(existing->id);
                       static_cast<void>(trial.connect(from, to));
                       graph.disconnect(existing->id);
                       const EdgeId replacement = graph.connect(from, to);
                       if (createdId)
                           *createdId = replacement;
                   }};
}
Command rewireGraphEdgeCommand(NetworkId network, EdgeId edgeId, PortRef from, PortRef to) {
    return Command{"rewire edge " + std::to_string(edgeId), [network, edgeId, from, to](Document& document) {
                       auto& graph = document.network(network).graph();
                       const auto existing = std::find_if(graph.edges().begin(), graph.edges().end(),
                                                          [edgeId](const Edge& edge) { return edge.id == edgeId; });
                       if (existing == graph.edges().end())
                           throw GraphException(GraphError::UnknownEdge,
                                                "cannot rewire unknown edge " + std::to_string(edgeId));

                       // Identical endpoints are intentionally a true no-op:
                       // in particular, retain the edge identity and authored
                       // route points.
                       if (existing->from == from && existing->to == to)
                           return;

                       Graph candidate = graph;
                       candidate.disconnect(edgeId);
                       const auto occupied = std::find_if(candidate.edges().begin(), candidate.edges().end(),
                                                          [to](const Edge& edge) { return edge.to == to; });
                       if (occupied != candidate.edges().end())
                           candidate.disconnect(occupied->id);
                       static_cast<void>(candidate.connect(from, to));
                       graph = std::move(candidate);
                   }};
}

Command insertNodeOnEdgeCommand(NetworkId network, EdgeId edgeId, std::string type, std::string name,
                                LayoutPosition position, std::shared_ptr<NodeId> createdNode,
                                std::shared_ptr<EdgeId> upstreamEdge, std::shared_ptr<EdgeId> downstreamEdge) {
    return Command{"insert node '" + name + "' on edge " + std::to_string(edgeId),
                   [network, edgeId, type = std::move(type), name = std::move(name), position, createdNode,
                    upstreamEdge, downstreamEdge](Document& document) {
                       auto& graph = document.network(network).graph();
                       const auto existing = std::find_if(graph.edges().begin(), graph.edges().end(),
                                                          [edgeId](const Edge& edge) { return edge.id == edgeId; });
                       if (existing == graph.edges().end())
                           throw GraphException(GraphError::UnknownEdge,
                                                "cannot insert on unknown edge " + std::to_string(edgeId));
                       const Edge original = *existing;

                       // Exercise all catalog, endpoint, cycle, name and identity
                       // validation against an isolated graph before publication.
                       Graph trial = graph;
                       trial.disconnect(edgeId);
                       const NodeId trialNode = trial.addNodeWithId(trial.nextNodeId(), type, name, {}, position);
                       const EdgeId trialUpstream = trial.connect(original.from, PortRef{trialNode, 0});
                       const EdgeId trialDownstream = trial.connect(PortRef{trialNode, 0}, original.to);

                       const NodeId inserted = graph.addNodeWithId(trialNode, type, name, {}, position);
                       graph.disconnect(edgeId);
                       const EdgeId actualUpstream = graph.connect(original.from, PortRef{inserted, 0});
                       const EdgeId actualDownstream = graph.connect(PortRef{inserted, 0}, original.to);
                       (void)trialUpstream;
                       (void)trialDownstream;
                       if (createdNode)
                           *createdNode = inserted;
                       if (upstreamEdge)
                           *upstreamEdge = actualUpstream;
                       if (downstreamEdge)
                           *downstreamEdge = actualDownstream;
                   }};
}
Command insertExistingNodeOnEdgeCommand(NetworkId network, EdgeId edgeId, NodeId nodeId, LayoutPosition position) {
    return Command{
        "insert existing node " + std::to_string(nodeId) + " on edge " + std::to_string(edgeId),
        [network, edgeId, nodeId, position](Document& document) {
            auto& graph = document.network(network).graph();
            const auto edge = std::find_if(graph.edges().begin(), graph.edges().end(),
                                           [edgeId](const Edge& value) { return value.id == edgeId; });
            if (edge == graph.edges().end())
                throw GraphException(GraphError::UnknownEdge, "cannot insert existing node " + std::to_string(nodeId) +
                                                                  " on unknown edge " + std::to_string(edgeId));
            if (!graph.node(nodeId))
                throw GraphException(GraphError::UnknownNode, "cannot insert unknown node " + std::to_string(nodeId) +
                                                                  " on edge " + std::to_string(edgeId));
            const bool connected = std::any_of(graph.edges().begin(), graph.edges().end(), [nodeId](const Edge& value) {
                return value.from.node == nodeId || value.to.node == nodeId;
            });
            if (connected)
                throw GraphException(GraphError::PortOccupied, "cannot insert node " + std::to_string(nodeId) +
                                                                   " because it already has an incident edge");
            if (graph.inputPorts(nodeId).empty() || graph.outputPorts(nodeId).empty())
                throw GraphException(GraphError::PortType, "node " + std::to_string(nodeId) +
                                                               " must have primary input port 0 and output port 0");

            const Edge original = *edge;
            Graph candidate = graph;
            candidate.setLayout(nodeId, position);
            candidate.disconnect(edgeId);
            static_cast<void>(candidate.connect(original.from, PortRef{nodeId, 0}));
            static_cast<void>(candidate.connect(PortRef{nodeId, 0}, original.to));
            graph = std::move(candidate);
        }};
}
Command addNetworkCommand(std::string name, std::shared_ptr<NetworkId> createdId) {
    return Command{"add network '" + name + "'", [name = std::move(name), createdId](Document& document) {
                       const NetworkId id = document.addNetwork(name);
                       if (createdId)
                           *createdId = id;
                   }};
}

Command removeNetworkCommand(NetworkId network) {
    return Command{"remove network " + std::to_string(network),
                   [network](Document& document) { document.removeNetwork(network); }};
}

Command addInstanceCommand(NetworkId parentNetwork, NetworkId definition, std::string name,
                           std::shared_ptr<NetworkInstanceId> createdId) {
    return Command{"add network instance '" + name + "'",
                   [parentNetwork, definition, name = std::move(name), createdId](Document& document) {
                       const auto id = document.addInstance(parentNetwork, definition, name);
                       if (createdId)
                           *createdId = id;
                   }};
}

Command removeInstanceCommand(NetworkInstanceId instance) {
    return Command{"remove network instance " + std::to_string(instance),
                   [instance](Document& document) { document.removeInstance(instance); }};
}

Command bindInstanceInputCommand(NetworkInstanceId instance, InterfacePortId input, PortRef source) {
    return Command{
        "bind network instance input " + std::to_string(input),
        [instance, input, source](Document& document) { document.bindInstanceInput(instance, input, source); }};
}

Command setInstanceParamCommand(NetworkInstanceId instance, NodeId targetNode, std::string key, ParameterValue value) {
    return Command{"set instance parameter " + key,
                   [instance, targetNode, key = std::move(key), value = std::move(value)](Document& document) {
                       document.setInstanceParam(instance, targetNode, key, value);
                   }};
}

Command resetInstanceParamCommand(NetworkInstanceId instance, NodeId targetNode, std::string key) {
    return Command{"reset instance parameter " + key, [instance, targetNode, key = std::move(key)](Document& document) {
                       const auto* target = document.instance(instance);
                       if (!target)
                           throw GraphException(GraphError::UnknownInstance,
                                                "cannot reset a parameter on unknown network instance " +
                                                    std::to_string(instance));
                       document.eraseInstanceParam(instance, targetNode, key);
                   }};
}

Command transactionCommand(std::string label, std::vector<Command> commands) {
    if (commands.empty())
        throw std::invalid_argument("transaction must contain at least one command");
    for (const auto& command : commands)
        if (!command.apply)
            throw std::invalid_argument("transaction contains an incomplete command");
    return Command{std::move(label), [commands = std::move(commands)](Document& document) {
                       for (const auto& command : commands)
                           command.apply(document);
                   }};
}

Command setColorPolicyCommand(ColorPolicy value) {
    return Command{"set color policy", [value = std::move(value)](Document& document) { document.color = value; }};
}

Command setSourceCommand(std::string id, SourceReference value) {
    if (id.empty())
        throw std::runtime_error("setSource: source key must not be empty");
    if (value.path.empty())
        throw std::runtime_error("setSource: source '" + id + "' must reference a media path");
    if (value.frameStep == 0)
        throw std::runtime_error("setSource: source '" + id + "' frameStep must not be zero");
    return Command{"set source '" + id + "'", [id = std::move(id), value = std::move(value)](Document& document) {
                       document.sources[id] = value;
                   }};
}
Command removeSourceCommand(std::string id) {
    if (id.empty())
        throw std::runtime_error("removeSource: source key must not be empty");
    return Command{"remove source '" + id + "'", [id = std::move(id)](Document& document) {
                       if (!document.sources.contains(id))
                           throw GraphException(GraphError::MissingMediaSource,
                                                "cannot remove unknown source '" + id + "'");
                       if (document.mediaCatalog.sourceUsed(document, id))
                           throw GraphException(GraphError::MediaSourceInUse,
                                                "cannot remove source '" + id + "': it is addressed by a source node");
                       for (const auto& entry : document.mediaCatalog.entries())
                           if (entry.sourceKey == id)
                               throw GraphException(GraphError::MediaSourceInUse,
                                                    "cannot remove source '" + id + "': media entry " +
                                                        std::to_string(entry.id) + " references it");
                       document.sources.erase(id);
                   }};
}

}  // namespace nemo
