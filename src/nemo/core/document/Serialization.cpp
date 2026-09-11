#include "nemo/core/document/Serialization.hpp"

#include "nemo/core/document/ParameterValueJson.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace nemo {
namespace {

std::uint64_t requiredId(const nlohmann::json& object, const char* field, const std::string& context) {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)))
        throw DeserializeError(context + ": '" + field + "' must be a nonnegative integer");
    const auto value = it->get<std::uint64_t>();
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max())
        throw DeserializeError(context + ": '" + field + "' must be nonzero and below the identity limit");
    return value;
}

std::optional<std::uint64_t> optionalId(const nlohmann::json& object, const char* field, const std::string& context) {
    if (!object.contains(field))
        return std::nullopt;
    return requiredId(object, field, context);
}

std::uint32_t port(const nlohmann::json& endpoint, const std::string& context) {
    const auto it = endpoint.find("port");
    if (it == endpoint.end() ||
        (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)) ||
        it->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        throw DeserializeError(context + ": 'port' must be an unsigned 32-bit integer");
    return it->get<std::uint32_t>();
}

std::uint64_t endpointNode(const nlohmann::json& endpoint, const std::string& context) {
    if (!endpoint.is_object())
        throw DeserializeError(context + " must be an object");
    return requiredId(endpoint, "node", context);
}

std::optional<std::uint64_t> watermark(const nlohmann::json& object, const char* field) {
    if (!object.contains(field))
        return std::nullopt;
    return requiredId(object, field, "document");
}

PortKind parseKind(const nlohmann::json& value, const std::string& context) {
    if (!value.is_string())
        throw DeserializeError(context + ": port kind must be a string");
    const auto kind = value.get<std::string>();
    if (kind == "image")
        return PortKind::Image;
    if (kind == "mask")
        return PortKind::Mask;
    if (kind == "media")
        return PortKind::Media;
    throw DeserializeError(context + ": unknown port kind '" + kind + "'");
}

std::int64_t signedValue(const nlohmann::json& value, const std::string& context) {
    if (value.is_number_unsigned()) {
        if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw DeserializeError(context + " must be a signed 64-bit integer");
        return static_cast<std::int64_t>(value.get<std::uint64_t>());
    }
    if (!value.is_number_integer())
        throw DeserializeError(context + " must be a signed 64-bit integer");
    return value.get<std::int64_t>();
}

std::uint64_t unsignedValue(const nlohmann::json& value, const std::string& context) {
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
        throw DeserializeError(context + " must be a nonnegative integer");
    return value.get<std::uint64_t>();
}

const char* kindName(PortKind kind) {
    switch (kind) {
    case PortKind::Image:
        return "image";
    case PortKind::Mask:
        return "mask";
    case PortKind::Media:
        return "media";
    }
    return "image";
}

LayoutPosition parseLayout(const nlohmann::json& value, const std::string& context) {
    if (!value.is_object() || !value.contains("x") || !value.contains("y") || !value.at("x").is_number() ||
        !value.at("y").is_number())
        throw DeserializeError(context + ": layout must contain numeric x and y");
    const double x = value.at("x").get<double>();
    const double y = value.at("y").get<double>();
    if (!std::isfinite(x) || !std::isfinite(y))
        throw DeserializeError(context + ": layout coordinates must be finite");
    return LayoutPosition{x, y};
}

nlohmann::json layoutJson(const LayoutPosition& layout) {
    return {{"x", layout.x}, {"y", layout.y}};
}

std::vector<PortSpec> parsePorts(const nlohmann::json& value, const std::string& context) {
    if (!value.is_array())
        throw DeserializeError(context + " must be an array");
    std::vector<PortSpec> ports;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto& entry = value.at(index);
        if (!entry.is_object() || !entry.contains("kind") || !entry.contains("name") || !entry.at("name").is_string())
            throw DeserializeError(context + "[" + std::to_string(index) + "]: kind and string name are required");
        ports.push_back(PortSpec{parseKind(entry.at("kind"), context), entry.at("name").get<std::string>()});
    }

    return ports;
}

ParameterValues parseParameterValues(const nlohmann::json& value, int schema, std::string_view type,
                                     const NodeCatalog& catalog, const std::string& context) {
    if (!value.is_object())
        throw DeserializeError(context + ": 'params' must be an object");
    ParameterValues params;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string fieldContext = context + " key '" + it.key() + "'";
        try {
            ParameterValue parsed;
            if (schema >= 3) {
                parsed = parameterValueFromJson(it.value());
            } else {
                if (!it.value().is_string())
                    throw DeserializeError(fieldContext + ": legacy parameter must be a string");
                parsed = catalog.parseParameterText(type, it.key(), it.value().get<std::string>());
            }
            if (const auto problem = catalog.validateParameter(type, it.key(), parsed))
                throw DeserializeError(fieldContext + ": " + *problem);
            params.emplace(it.key(), std::move(parsed));
        } catch (const DeserializeError&) {
            throw;
        } catch (const std::exception& error) {
            throw DeserializeError(fieldContext + ": " + error.what());
        }
    }
    return params;
}

nlohmann::json parameterValuesJson(const ParameterValues& params) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [key, value] : params)
        result[key] = parameterValueToJson(value);
    return result;
}

const char* interpolationName(KeyInterpolation value) {
    switch (value) {
    case KeyInterpolation::Hold:
        return "hold";
    case KeyInterpolation::Linear:
        return "linear";
    case KeyInterpolation::Bezier:
        return "bezier";
    }
    throw std::logic_error("invalid animation interpolation");
}

double animationNumber(const nlohmann::json& value, const std::string& context) {
    if (!value.is_number())
        throw DeserializeError(context + " must be numeric");
    const auto number = value.get<double>();
    if (!std::isfinite(number))
        throw DeserializeError(context + " must be finite");
    return number;
}

std::array<double, 4> animationSlopes(const nlohmann::json& value, const std::string& context) {
    if (!value.is_array() || value.size() != 4)
        throw DeserializeError(context + " must contain four slope components");
    std::array<double, 4> slopes;
    for (std::size_t i = 0; i < slopes.size(); ++i)
        slopes[i] = animationNumber(value.at(i), context + "[" + std::to_string(i) + "]");
    return slopes;
}

void loadAnimation(const nlohmann::json& json, Document& document, int schema) {
    if (schema < 4) {
        if (json.contains("animationChannels") || json.contains("nextAnimationChannelId") ||
            json.contains("nextKeyframeId"))
            throw DeserializeError("animation data requires document schema 4");
        return;
    }
    std::vector<AnimationChannel> channels;
    if (json.contains("animationChannels")) {
        const auto& entries = json.at("animationChannels");
        if (!entries.is_array())
            throw DeserializeError("document animationChannels must be an array");
        channels.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const std::string context = "animationChannels[" + std::to_string(index) + "]";
            try {
                const auto& entry = entries.at(index);
                AnimationChannel channel;
                channel.id = requiredId(entry, "id", context);
                const auto& address = entry.at("address");
                channel.address.network = requiredId(address, "network", context + " address");
                channel.address.node = requiredId(address, "node", context + " address");
                channel.address.key = address.at("key").get<std::string>();
                if (address.contains("instance"))
                    channel.address.instance = requiredId(address, "instance", context + " address");
                const auto& keys = entry.at("keys");
                if (!keys.is_array())
                    throw DeserializeError("keys must be an array");
                channel.keys.reserve(keys.size());
                for (const auto& value : keys) {
                    Keyframe key;
                    key.id = requiredId(value, "id", context + " key");
                    const auto keyContext = context + " key " + std::to_string(key.id);
                    key.time = animationNumber(value.at("time"), keyContext + " time");
                    key.value = parameterValueFromJson(value.at("value"));
                    const auto interpolation = value.at("interpolation").get<std::string>();
                    if (interpolation == "hold")
                        key.interpolation = KeyInterpolation::Hold;
                    else if (interpolation == "linear")
                        key.interpolation = KeyInterpolation::Linear;
                    else if (interpolation == "bezier")
                        key.interpolation = KeyInterpolation::Bezier;
                    else
                        throw DeserializeError(keyContext + ": unknown interpolation '" + interpolation + "'");
                    const auto mode = value.at("tangentMode").get<std::string>();
                    if (mode == "smooth")
                        key.tangentMode = TangentMode::Smooth;
                    else if (mode == "broken")
                        key.tangentMode = TangentMode::Broken;
                    else
                        throw DeserializeError(keyContext + ": unknown tangent mode '" + mode + "'");
                    key.inSlope = animationSlopes(value.at("inSlope"), keyContext + " incoming tangent");
                    key.outSlope = animationSlopes(value.at("outSlope"), keyContext + " outgoing tangent");
                    channel.keys.push_back(std::move(key));
                }
                channels.push_back(std::move(channel));
            } catch (const std::exception& error) {
                throw DeserializeError(context + ": " + error.what());
            }
        }
    }
    const auto nextId = [&](const char* field) {
        if (!json.contains(field))
            return std::uint64_t{1};
        const auto value = unsignedValue(json.at(field), field);
        if (value == 0)
            throw DeserializeError(std::string(field) + " must be nonzero");
        return value;
    };
    try {
        // Restore validates the entire set, including catalog/address/type,
        // identities, collisions and tangents, before installing any channels.
        document.restoreAnimationChannels(std::move(channels), nextId("nextAnimationChannelId"),
                                          nextId("nextKeyframeId"));
    } catch (const std::exception& error) {
        throw DeserializeError("document animation: " + std::string(error.what()));
    }
}

void clearNetwork(Network& network) {
    std::vector<NodeId> nodes;
    for (const auto& node : network.graph().nodes())
        nodes.push_back(node.id);
    for (const auto id : nodes)
        network.graph().removeNode(id);
}

void loadNetwork(const nlohmann::json& entry, Network& network, LoadResult& result, int schema) {
    std::string context = "network '" + network.name() + "'";
    if (!entry.is_object())
        throw DeserializeError(context + " must be an object");
    if (entry.contains("name")) {
        if (!entry.at("name").is_string())
            throw DeserializeError(context + ": 'name' must be a string");
        network.rename(entry.at("name").get<std::string>());
    }
    context = "network '" + network.name() + "'";
    clearNetwork(network);
    if (entry.contains("inputs")) {
        if (!entry.at("inputs").is_array())
            throw DeserializeError(context + ": 'inputs' must be an array");
        for (std::size_t i = 0; i < entry.at("inputs").size(); ++i) {
            const auto& p = entry.at("inputs").at(i);
            if (!p.is_object() || !p.contains("id") || !p.contains("name") || !p.contains("kind") ||
                !p.at("name").is_string())
                throw DeserializeError(context + ": malformed input terminal");
            if (p.contains("allowFanOut") && !p.at("allowFanOut").is_boolean())
                throw DeserializeError(context + ": allowFanOut must be boolean");
            const bool allowFanOut = p.value("allowFanOut", true);
            (void)network.addInput(p.at("name").get<std::string>(), parseKind(p.at("kind"), context + " input"),
                                   requiredId(p, "id", context + " input"), allowFanOut);
        }
    }
    if (entry.contains("outputs")) {
        if (!entry.at("outputs").is_array())
            throw DeserializeError(context + ": 'outputs' must be an array");
        for (std::size_t i = 0; i < entry.at("outputs").size(); ++i) {
            const auto& p = entry.at("outputs").at(i);
            if (!p.is_object() || !p.contains("id") || !p.contains("name") || !p.contains("kind") ||
                !p.at("name").is_string())
                throw DeserializeError(context + ": malformed output terminal");
            if (p.contains("allowFanOut") && !p.at("allowFanOut").is_boolean())
                throw DeserializeError(context + ": allowFanOut must be boolean");
            const bool allowFanOut = p.value("allowFanOut", true);
            (void)network.addOutput(p.at("name").get<std::string>(), parseKind(p.at("kind"), context + " output"),
                                    requiredId(p, "id", context + " output"), allowFanOut);
        }
    }
    const auto nodes = entry.find("nodes");
    if (nodes != entry.end() && !nodes->is_array())
        throw DeserializeError(context + ": 'nodes' must be an array");
    const auto edges = entry.find("edges");
    if (edges != entry.end() && !edges->is_array())
        throw DeserializeError(context + ": 'edges' must be an array");
    std::set<EdgeId> seenEdges;
    std::set<NodeId> seenNodes;
    std::set<NodeId> declaredNodes;
    std::map<NodeId, NodeId> idMap;
    const auto& nodeEntries = nodes == entry.end() ? nlohmann::json::array() : *nodes;
    for (const auto& n : nodeEntries) {
        if (n.is_object() && n.contains("id")) {
            const auto id = requiredId(n, "id", context + " node");
            if (!declaredNodes.insert(id).second)
                throw DeserializeError("duplicate node id in file: " + std::to_string(id));
        }
    }
    for (std::size_t i = 0; i < nodeEntries.size(); ++i) {
        const auto& n = nodeEntries.at(i);
        const std::string nc = context + " node " + std::to_string(i);
        if (!n.is_object() || !n.contains("type") || !n.contains("name") || !n.at("type").is_string() ||
            !n.at("name").is_string())
            throw DeserializeError(nc + ": string 'type' and 'name' are required");
        std::optional<NodeId> persistedId;
        if (n.contains("id"))
            persistedId = requiredId(n, "id", nc);
        NodeId id = persistedId.value_or(1);
        while (!persistedId && (seenNodes.contains(id) || declaredNodes.contains(id))) {
            if (id == std::numeric_limits<NodeId>::max())
                throw DeserializeError(nc + ": no available identity for legacy node");
            ++id;
        }
        if (!seenNodes.insert(id).second)
            throw DeserializeError("duplicate node id in file: " + std::to_string(id));
        ParameterValues params;
        if (n.contains("params"))
            params = parseParameterValues(n.at("params"), schema, n.at("type").get<std::string>(),
                                          network.graph().catalog(), nc + " id " + std::to_string(id) + " parameters");
        LayoutPosition layout;
        if (n.contains("layout"))
            layout = parseLayout(n.at("layout"), nc);
        const auto definition = n.contains("definition") ? requiredId(n, "definition", nc) : kInvalidNetwork;
        const auto instance = n.contains("instance") ? requiredId(n, "instance", nc) : kInvalidNetworkInstance;
        try {
            (void)network.graph().addNodeWithId(id, n.at("type").get<std::string>(), n.at("name").get<std::string>(),
                                                std::move(params), layout, definition, instance);
            if (n.contains("inputPorts") || n.contains("outputPorts")) {
                if (!n.contains("inputPorts") || !n.contains("outputPorts"))
                    throw DeserializeError(nc + ": both inputPorts and outputPorts are required");
                network.graph().setPortContract(id, parsePorts(n.at("inputPorts"), nc + " inputPorts"),
                                                parsePorts(n.at("outputPorts"), nc + " outputPorts"));
            }
        } catch (const GraphException& error) {
            throw DeserializeError(nc + ": " + error.what());
        }
        idMap.emplace(persistedId.value_or(id), id);
        if (!persistedId)
            result.warnings.push_back(nc + " has no id; allocated a stable identity");
        if (!network.graph().descriptor(n.at("type").get<std::string>()) && definition == kInvalidNetwork)
            result.warnings.push_back("unknown node type '" + n.at("type").get<std::string>() + "' (node '" +
                                      n.at("name").get<std::string>() + "'); retained as data, not evaluated");
    }
    const auto& edgeEntries = edges == entry.end() ? nlohmann::json::array() : *edges;
    std::set<EdgeId> declaredEdges;
    for (const auto& e : edgeEntries)
        if (e.is_object() && e.contains("id"))
            declaredEdges.insert(requiredId(e, "id", context + " edge"));
    for (std::size_t i = 0; i < edgeEntries.size(); ++i) {
        const auto& e = edgeEntries.at(i);
        const std::string ec = context + " edge " + std::to_string(i);
        if (!e.is_object() || !e.contains("from") || !e.contains("to"))
            throw DeserializeError(ec + ": from and to are required");
        const auto persistedId = optionalId(e, "id", ec);
        EdgeId id = persistedId.value_or(network.graph().nextEdgeId());
        while (!persistedId && (seenEdges.contains(id) || declaredEdges.contains(id))) {
            if (id == std::numeric_limits<EdgeId>::max())
                throw DeserializeError(ec + ": no available identity for legacy edge");
            ++id;
        }
        if (!seenEdges.insert(id).second)
            throw DeserializeError("duplicate edge id in file: " + std::to_string(id));
        if (!persistedId)
            result.warnings.push_back(ec + " has no id; allocated a stable identity");
        const auto& from = e.at("from");
        const auto& to = e.at("to");
        const auto sourceFileId = endpointNode(from, ec + " from");
        const auto destinationFileId = endpointNode(to, ec + " to");
        const auto sourceIt = idMap.find(sourceFileId);
        const auto destinationIt = idMap.find(destinationFileId);
        if (sourceIt == idMap.end() || destinationIt == idMap.end()) {
            result.warnings.push_back(ec + " references a node that is not present in the file; dropped");
            continue;
        }
        PortRef source{sourceIt->second, port(from, ec + " from")};
        PortRef destination{destinationIt->second, port(to, ec + " to")};
        try {
            (void)network.graph().connectWithId(id, source, destination);
            if (e.contains("route")) {
                if (!e.at("route").is_array())
                    throw DeserializeError(ec + ": route must be an array");
                std::vector<LayoutPosition> route;
                for (std::size_t r = 0; r < e.at("route").size(); ++r)
                    route.push_back(parseLayout(e.at("route").at(r), ec + " route"));
                network.graph().setRoute(id, std::move(route));
            }
        } catch (const GraphException& error) {
            result.warnings.push_back(ec + " rejected: " + error.what());
        }
    }
    if (entry.contains("inputConnections")) {
        if (!entry.at("inputConnections").is_array())
            throw DeserializeError(context + ": inputConnections must be an array");
        for (const auto& c : entry.at("inputConnections")) {
            if (!c.is_object() || !c.contains("terminal") || !c.contains("node"))
                throw DeserializeError(context + ": malformed input connection");
            const auto t = requiredId(c, "terminal", context + " input connection");
            const auto& node = c.at("node");
            try {
                network.connectInput(t, PortRef{endpointNode(node, context + " input connection"),
                                                port(node, context + " input connection")});
            } catch (const GraphException& error) {
                throw DeserializeError(context + " input connection: " + std::string(error.what()));
            }
        }
    }
    if (entry.contains("outputConnections")) {
        if (!entry.at("outputConnections").is_array())
            throw DeserializeError(context + ": outputConnections must be an array");
        for (const auto& c : entry.at("outputConnections")) {
            if (!c.is_object() || !c.contains("terminal") || !c.contains("node"))
                throw DeserializeError(context + ": malformed output connection");
            const auto t = requiredId(c, "terminal", context + " output connection");
            const auto& node = c.at("node");
            try {
                network.connectOutput(PortRef{endpointNode(node, context + " output connection"),
                                              port(node, context + " output connection")},
                                      t);
            } catch (const GraphException& error) {
                throw DeserializeError(context + " output connection: " + std::string(error.what()));
            }
        }
    }
    if (entry.contains("defaultOutput")) {
        const auto selected = unsignedValue(entry.at("defaultOutput"), context + " defaultOutput");
        if (selected != kInvalidNode) {
            try {
                network.setDefaultOutput(selected);
            } catch (const GraphException& error) {
                throw DeserializeError(context + ": " + std::string(error.what()));
            }
        }
    }
    network.restoreIdentityHighWatermarks(
        watermark(entry, "nextNodeId").value_or(network.graph().nextNodeId()),
        watermark(entry, "nextEdgeId").value_or(network.graph().nextEdgeId()),
        watermark(entry, "nextInterfacePortId").value_or(network.nextInterfacePortId()));
}

}  // namespace

nlohmann::json saveDocument(const Document& document) {
    nlohmann::json networks = nlohmann::json::array();
    for (const auto& network : document.networks()) {
        nlohmann::json nodes = nlohmann::json::array();
        for (const auto& node : network.graph().nodes()) {
            nlohmann::json value{{"id", node.id},
                                 {"type", node.type},
                                 {"name", node.name},
                                 {"params", parameterValuesJson(node.params)},
                                 {"layout", layoutJson(node.layout)}};
            if (node.definition != kInvalidNetwork)
                value["definition"] = node.definition;
            if (node.instance != kInvalidNetworkInstance)
                value["instance"] = node.instance;
            if (node.hasPortContract) {
                value["inputPorts"] = nlohmann::json::array();
                for (const auto& p : node.inputPorts)
                    value["inputPorts"].push_back({{"kind", kindName(p.kind)}, {"name", p.name}});
                value["outputPorts"] = nlohmann::json::array();
                for (const auto& p : node.outputPorts)
                    value["outputPorts"].push_back({{"kind", kindName(p.kind)}, {"name", p.name}});
            }
            nodes.push_back(std::move(value));
        }
        nlohmann::json edges = nlohmann::json::array();
        for (const auto& edge : network.graph().edges()) {
            nlohmann::json value{{"id", edge.id},
                                 {"from", {{"node", edge.from.node}, {"port", edge.from.port}}},
                                 {"to", {{"node", edge.to.node}, {"port", edge.to.port}}}};
            if (!edge.route.empty()) {
                value["route"] = nlohmann::json::array();
                for (const auto& p : edge.route)
                    value["route"].push_back(layoutJson(p));
            }
            edges.push_back(std::move(value));
        }
        auto formal = [](const std::vector<FormalPort>& ports) {
            nlohmann::json result = nlohmann::json::array();
            for (const auto& p : ports)
                result.push_back(
                    {{"id", p.id}, {"name", p.name}, {"kind", kindName(p.kind)}, {"allowFanOut", p.allowFanOut}});
            return result;
        };
        nlohmann::json value{{"id", network.id()},
                             {"name", network.name()},
                             {"defaultOutput", network.defaultOutput()},
                             {"nextNodeId", network.graph().nextNodeId()},
                             {"nextEdgeId", network.graph().nextEdgeId()},
                             {"nextInterfacePortId", network.nextInterfacePortId()},
                             {"inputs", formal(network.inputs())},
                             {"outputs", formal(network.outputs())},
                             {"nodes", nodes},
                             {"edges", edges}};
        value["inputConnections"] = nlohmann::json::array();
        for (const auto& c : network.inputConnections())
            value["inputConnections"].push_back(
                {{"terminal", c.terminal}, {"node", {{"node", c.node.node}, {"port", c.node.port}}}});
        value["outputConnections"] = nlohmann::json::array();
        for (const auto& c : network.outputConnections())
            value["outputConnections"].push_back(
                {{"terminal", c.terminal}, {"node", {{"node", c.node.node}, {"port", c.node.port}}}});
        networks.push_back(std::move(value));
    }
    nlohmann::json sources = nlohmann::json::object();
    for (const auto& [key, source] : document.sources) {
        sources[key] = {{"path", source.path},
                        {"frameOffset", source.frameOffset},
                        {"frameStep", source.frameStep},
                        {"revision", source.revision},
                        {"interpretation", source.interpretation}};
    }
    nlohmann::json instances = nlohmann::json::array();
    for (const auto& instance : document.instances()) {
        nlohmann::json instanceParams = nlohmann::json::object();
        for (const auto& [target, values] : instance.params)
            instanceParams[std::to_string(target)] = parameterValuesJson(values);
        nlohmann::json value{{"id", instance.id},
                             {"parentNetwork", instance.parentNetwork},
                             {"definition", instance.definition},
                             {"node", instance.node},
                             {"name", instance.name},
                             {"params", std::move(instanceParams)}};
        value["inputBindings"] = nlohmann::json::array();
        for (const auto& [terminal, source] : instance.inputBindings)
            value["inputBindings"].push_back(
                {{"terminal", terminal}, {"node", {{"node", source.node}, {"port", source.port}}}});
        instances.push_back(std::move(value));
    }
    nlohmann::json animation = nlohmann::json::array();
    for (const auto& channel : document.animationChannels()) {
        nlohmann::json address{{"network", channel.address.network},
                               {"node", channel.address.node},
                               {"key", channel.address.key}};
        if (channel.address.instance != kInvalidNetworkInstance)
            address["instance"] = channel.address.instance;
        nlohmann::json keys = nlohmann::json::array();
        for (const auto& key : channel.keys)
            keys.push_back({{"id", key.id},
                            {"time", key.time},
                            {"value", parameterValueToJson(key.value)},
                            {"interpolation", interpolationName(key.interpolation)},
                            {"tangentMode", key.tangentMode == TangentMode::Smooth ? "smooth" : "broken"},
                            {"inSlope", key.inSlope},
                            {"outSlope", key.outSlope}});
        animation.push_back({{"id", channel.id}, {"address", std::move(address)}, {"keys", std::move(keys)}});
    }
    return {{"schema", Document::kSchemaVersion},
            {"name", document.name},
            {"color",
             {{"workingSpace", document.color.workingSpace},
              {"viewerTransform", document.color.viewerTransform},
              {"deliveryTransform", document.color.deliveryTransform}}},
            {"sources", sources},
            {"rootNetworkId", document.rootNetworkId()},
            {"nextNetworkId", document.nextNetworkId()},
            {"nextInstanceId", document.nextInstanceId()},
            {"networks", networks},
            {"instances", instances},
            {"animationChannels", std::move(animation)},
            {"nextAnimationChannelId", document.nextAnimationChannelId()},
            {"nextKeyframeId", document.nextKeyframeId()}};
}

LoadResult loadDocument(const nlohmann::json& json, std::shared_ptr<const NodeCatalog> catalog) {
    if (!json.is_object())
        throw DeserializeError("document root is not an object");
    if (!json.contains("schema") || !json.at("schema").is_number_integer())
        throw DeserializeError("document has no integer 'schema' field");
    const int schema = json.at("schema").get<int>();
    if (schema > Document::kSchemaVersion)
        throw DeserializeError("document schema " + std::to_string(schema) + " is newer than this build supports (" +
                               std::to_string(Document::kSchemaVersion) + ")");
    LoadResult result{Document(std::move(catalog)), {}};
    result.document.name = json.value("name", std::string{});
    if (auto color = json.find("color"); color != json.end()) {
        if (!color->is_object()) {
            result.warnings.push_back("document 'color' field is not an object; using default color policy");
        } else {
            result.document.color.workingSpace = color->value("workingSpace", result.document.color.workingSpace);
            result.document.color.viewerTransform =
                color->value("viewerTransform", result.document.color.viewerTransform);
            result.document.color.deliveryTransform =
                color->value("deliveryTransform", result.document.color.deliveryTransform);
        }
    }
    if (auto sources = json.find("sources"); sources != json.end()) {
        if (!sources->is_object())
            throw DeserializeError("document 'sources' field must be an object");
        for (auto it = sources->begin(); it != sources->end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("path") || !it.value().at("path").is_string())
                throw DeserializeError("malformed source '" + it.key() + "'");
            SourceReference source;
            const auto& e = it.value();
            source.path = e.at("path").get<std::string>();
            if (source.path.empty())
                throw DeserializeError("source '" + it.key() + "' has an empty path");
            if (e.contains("frameOffset"))
                source.frameOffset = signedValue(e.at("frameOffset"), "source frameOffset");
            if (e.contains("frameStep"))
                source.frameStep = signedValue(e.at("frameStep"), "source frameStep");
            if (e.contains("revision"))
                source.revision = unsignedValue(e.at("revision"), "source revision");
            if (source.frameStep == 0)
                throw DeserializeError("source '" + it.key() + "': frameStep must not be zero");
            if (e.contains("interpretation")) {
                if (!e.at("interpretation").is_object())
                    throw DeserializeError("source interpretation must be an object");
                for (auto tag = e.at("interpretation").begin(); tag != e.at("interpretation").end(); ++tag) {
                    if (!tag.value().is_string())
                        throw DeserializeError("source interpretation values must be strings");
                    source.interpretation[tag.key()] = tag.value().get<std::string>();
                }
            }
            result.document.sources[it.key()] = std::move(source);
        }
    }
    if (json.contains("networks")) {
        const auto& entries = json.at("networks");
        if (!entries.is_array() || entries.empty())
            throw DeserializeError("document 'networks' must be a nonempty array");
        const auto rootId = requiredId(json, "rootNetworkId", "document");
        const auto initialRoot = result.document.rootNetworkId();
        std::set<NetworkId> seen;
        std::set<std::string> names;
        for (const auto& entry : entries) {
            const auto id = requiredId(entry, "id", "network");
            if (!seen.insert(id).second)
                throw DeserializeError("duplicate network id in file: " + std::to_string(id));
            if (!entry.contains("name") || !entry.at("name").is_string())
                throw DeserializeError("network name must be a string");
            if (!names.insert(entry.at("name").get<std::string>()).second)
                throw DeserializeError("duplicate network name in file");
        }
        if (!seen.contains(rootId))
            throw DeserializeError("document networks do not contain root " + std::to_string(rootId));
        std::string temporaryName = "__loading_root__";
        while (names.contains(temporaryName))
            temporaryName += '_';
        result.document.network(initialRoot).rename(std::move(temporaryName));
        for (const auto& entry : entries) {
            const auto id = requiredId(entry, "id", "network");
            if (id != initialRoot)
                (void)result.document.addNetworkWithId(id, entry.at("name").get<std::string>());
            loadNetwork(entry, result.document.network(id), result, schema);
        }
        result.document.setRootNetworkId(rootId);
        if (!seen.contains(initialRoot))
            result.document.removeNetwork(initialRoot);
    } else if (schema < 2) {
        // Schema 1 used the root graph directly. Migrate it into the default root network.
        nlohmann::json legacy{{"name", "Root"},
                              {"nodes", json.value("nodes", nlohmann::json::array())},
                              {"edges", json.value("edges", nlohmann::json::array())}};
        if (json.contains("nextNodeId"))
            legacy["nextNodeId"] = json.at("nextNodeId");
        if (json.contains("nextEdgeId"))
            legacy["nextEdgeId"] = json.at("nextEdgeId");
        loadNetwork(legacy, result.document.network(result.document.rootNetworkId()), result, schema);
        for (const auto& node : result.document.network(result.document.rootNetworkId()).graph().nodes()) {
            const auto* descriptor =
                result.document.network(result.document.rootNetworkId()).graph().descriptor(node.type);
            if (descriptor && descriptor->isOutput) {
                result.document.network(result.document.rootNetworkId()).setDefaultOutput(node.id);
                break;
            }
        }
        result.warnings.push_back("legacy single-graph document migrated to the root network");
    } else {
        throw DeserializeError("schema " + std::to_string(schema) + " document has no 'networks' field");
    }
    if (json.contains("instances")) {
        if (!json.at("instances").is_array())
            throw DeserializeError("document 'instances' field must be an array");
        for (std::size_t i = 0; i < json.at("instances").size(); ++i) {
            const auto& e = json.at("instances").at(i);
            const std::string context = "instance " + std::to_string(i);
            const auto id = requiredId(e, "id", context);
            const auto parent = requiredId(e, "parentNetwork", context);
            const auto definition = requiredId(e, "definition", context);
            const auto node = requiredId(e, "node", context);
            if (!e.contains("name") || !e.at("name").is_string())
                throw DeserializeError(context + ": name is required");
            std::map<InterfacePortId, PortRef> bindings;
            if (e.contains("inputBindings")) {
                if (!e.at("inputBindings").is_array())
                    throw DeserializeError(context + ": inputBindings must be an array");
                for (const auto& b : e.at("inputBindings")) {
                    const auto terminal = requiredId(b, "terminal", context);
                    const auto& ref = b.at("node");
                    bindings.emplace(terminal, PortRef{endpointNode(ref, context), port(ref, context)});
                }
            }
            std::map<NodeId, ParameterValues> params;
            if (e.contains("params")) {
                if (!e.at("params").is_object())
                    throw DeserializeError(context + ": params must be an object");
                for (auto p = e.at("params").begin(); p != e.at("params").end(); ++p) {
                    NodeId target{};
                    try {
                        std::size_t consumed = 0;
                        target = static_cast<NodeId>(std::stoull(p.key(), &consumed));
                        if (consumed != p.key().size())
                            throw std::invalid_argument("not a node id");
                    } catch (...) {
                        throw DeserializeError(context + ": parameter target must be a node id");
                    }
                    if (target == kInvalidNode || target == std::numeric_limits<NodeId>::max() ||
                        !p.value().is_object())
                        throw DeserializeError(context + " node " + std::to_string(target) +
                                               ": malformed parameter target");
                    const NodeInstance* targetNode = nullptr;
                    try {
                        targetNode = result.document.network(definition).graph().node(target);
                    } catch (const std::exception&) {
                    }
                    const std::string type = targetNode == nullptr ? std::string{} : targetNode->type;
                    params.emplace(
                        target,
                        parseParameterValues(p.value(), schema, type,
                                             result.document.network(result.document.rootNetworkId()).graph().catalog(),
                                             context + " id " + std::to_string(id) + " node " + std::to_string(target) +
                                                 " parameters"));
                }
            }
            try {
                (void)result.document.addInstanceWithId(id, parent, definition, node, e.at("name").get<std::string>(),
                                                        std::move(bindings), std::move(params));
            } catch (const std::exception& error) {
                throw DeserializeError(context + ": " + error.what());
            }
        }
    }
    for (const auto& network : result.document.networks()) {
        for (const auto& node : network.graph().nodes()) {
            if (node.definition == kInvalidNetwork)
                continue;
            const auto* occurrence = result.document.instance(node.instance);
            if (!occurrence || occurrence->parentNetwork != network.id() || occurrence->definition != node.definition ||
                occurrence->node != node.id)
                throw DeserializeError("network " + std::to_string(network.id()) + " node " + std::to_string(node.id) +
                                       " references an invalid instance " + std::to_string(node.instance));
        }
    }
    loadAnimation(json, result.document, schema);
    result.document.synchronizeReferences();
    result.document.restoreIdentityHighWatermarks(
        watermark(json, "nextNetworkId").value_or(result.document.nextNetworkId()),
        watermark(json, "nextInstanceId").value_or(result.document.nextInstanceId()));
    return result;
}

}  // namespace nemo
