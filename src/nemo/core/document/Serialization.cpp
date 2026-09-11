#include "nemo/core/document/Serialization.hpp"

#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace nemo {
namespace {

std::optional<std::uint64_t> optionalId(const nlohmann::json& entry, const char* field, const std::string& context) {
    const auto it = entry.find(field);
    if (it == entry.end())
        return std::nullopt;
    if (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)) {
        throw DeserializeError(context + ": '" + field + "' must be a nonnegative integer");
    }
    const auto id = it->get<std::uint64_t>();
    if (id == 0 || id == std::numeric_limits<std::uint64_t>::max()) {
        throw DeserializeError(context + ": '" + field + "' must be nonzero and below the identity limit");
    }
    return id;
}

std::uint32_t port(const nlohmann::json& endpoint, const std::string& context) {
    if (!endpoint.is_object() || !endpoint.contains("port")) {
        throw DeserializeError(context + ": endpoint requires an integer 'port'");
    }
    const auto& value = endpoint.at("port");
    if ((!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0)) ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) ||
        (!value.is_number_unsigned() && value.is_number_integer() &&
         static_cast<std::uint64_t>(value.get<std::int64_t>()) > std::numeric_limits<std::uint32_t>::max())) {
        throw DeserializeError(context + ": endpoint 'port' must be a nonnegative 32-bit integer");
    }
    return value.get<std::uint32_t>();
}

std::uint64_t requiredEndpointNode(const nlohmann::json& endpoint, const std::string& context) {
    if (!endpoint.is_object())
        throw DeserializeError(context + " endpoint must be an object");
    const auto id = optionalId(endpoint, "node", context + " endpoint");
    if (!id)
        throw DeserializeError(context + " endpoint requires a nonzero integer 'node'");
    return *id;
}

std::optional<std::uint64_t> optionalWatermark(const nlohmann::json& json, const char* field) {
    const auto it = json.find(field);
    if (it == json.end())
        return std::nullopt;
    if (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)) {
        throw DeserializeError("document '" + std::string(field) + "' must be a nonnegative integer");
    }
    const auto value = it->get<std::uint64_t>();
    if (value == 0) {
        throw DeserializeError("document '" + std::string(field) + "' must be nonzero");
    }
    return value;
}
}  // namespace

nlohmann::json saveDocument(const Document& document) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : document.graph.nodes()) {
        nodes.push_back({{"id", node.id}, {"type", node.type}, {"name", node.name}, {"params", node.params}});
    }
    nlohmann::json edges = nlohmann::json::array();
    for (const auto& edge : document.graph.edges()) {
        edges.push_back({{"id", edge.id},
                         {"from", {{"node", edge.from.node}, {"port", edge.from.port}}},
                         {"to", {{"node", edge.to.node}, {"port", edge.to.port}}}});
    }
    // Persistent source media (issue #11): plain reference records only —
    // path, time mapping, interpretation policy. No runtime/decoder state.
    nlohmann::json sources = nlohmann::json::object();
    for (const auto& [key, source] : document.sources) {
        nlohmann::json entry{{"path", source.path},
                             {"frameOffset", source.frameOffset},
                             {"frameStep", source.frameStep}};
        if (source.revision != 0)
            entry["revision"] = source.revision;
        if (!source.interpretation.empty()) {
            entry["interpretation"] = source.interpretation;
        }
        sources[key] = std::move(entry);
    }
    return {{"schema", Document::kSchemaVersion},
            {"name", document.name},
            {"color",
             {{"workingSpace", document.color.workingSpace},
              {"viewerTransform", document.color.viewerTransform},
              {"deliveryTransform", document.color.deliveryTransform}}},
            {"sources", std::move(sources)},
            {"nextNodeId", document.graph.nextNodeId()},
            {"nextEdgeId", document.graph.nextEdgeId()},
            {"nodes", nodes},
            {"edges", edges}};
}

LoadResult loadDocument(const nlohmann::json& json, std::shared_ptr<const NodeCatalog> catalog) {
    if (!json.is_object()) {
        throw DeserializeError("document root is not an object");
    }
    if (!json.contains("schema") || !json.at("schema").is_number_integer()) {
        throw DeserializeError("document has no integer 'schema' field");
    }
    const int schema = json.at("schema").get<int>();
    if (schema > Document::kSchemaVersion) {
        throw DeserializeError("document schema " + std::to_string(schema) + " is newer than this build supports (" +
                               std::to_string(Document::kSchemaVersion) + ")");
    }
    // schema < kSchemaVersion migrates here, one step at a time, before load.

    LoadResult result{Document(std::move(catalog)), {}};
    result.document.name = json.value("name", std::string{});

    // Color policy: default-constructed ColorPolicy is the documented default
    // (spec section 5), so a document saved without the block loads with
    // defaults. Individual fields fall back the same way, keeping older
    // partial blocks loadable.
    if (auto color = json.find("color"); color != json.end() && color->is_object()) {
        result.document.color.workingSpace = color->value("workingSpace", result.document.color.workingSpace);
        result.document.color.viewerTransform = color->value("viewerTransform", result.document.color.viewerTransform);
        result.document.color.deliveryTransform =
            color->value("deliveryTransform", result.document.color.deliveryTransform);
    } else if (color != json.end()) {
        result.warnings.push_back("document 'color' field is not an object; using default color policy");
    }

    // Persistent sources (issue #11): plain reference records. A missing
    // block loads with no sources; malformed entries are structural errors
    // (silently dropping media would hide broken references), and an
    // empty path is rejected the same way setSourceCommand rejects it.
    if (auto sources = json.find("sources"); sources != json.end() && sources->is_object()) {
        for (auto it = sources->begin(); it != sources->end(); ++it) {
            if (it.key().empty())
                throw DeserializeError("source key must not be empty");
            const nlohmann::json& entry = it.value();
            if (!entry.is_object() || !entry.contains("path") || !entry.at("path").is_string()) {
                throw DeserializeError("malformed source '" + it.key() + "': string 'path' is required");
            }
            SourceReference source;
            source.path = entry.at("path").get<std::string>();
            if (source.path.empty()) {
                throw DeserializeError("source '" + it.key() + "' has an empty path");
            }
            if (entry.contains("revision")) {
                const auto& revision = entry.at("revision");
                if (!revision.is_number_unsigned() &&
                    (!revision.is_number_integer() || revision.get<std::int64_t>() < 0))
                    throw DeserializeError("source '" + it.key() + "': 'revision' must be a nonnegative integer");
                source.revision = revision.get<std::uint64_t>();
            }
            if (entry.contains("frameOffset")) {
                const auto& value = entry.at("frameOffset");
                if (!value.is_number_integer() ||
                    (value.is_number_unsigned() &&
                     value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                    throw DeserializeError("source '" + it.key() + "': 'frameOffset' must be a signed 64-bit integer");
                source.frameOffset = entry.at("frameOffset").get<std::int64_t>();
            }
            if (entry.contains("frameStep")) {
                const auto& value = entry.at("frameStep");
                if (!value.is_number_integer() ||
                    (value.is_number_unsigned() &&
                     value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                    throw DeserializeError("source '" + it.key() + "': 'frameStep' must be a signed 64-bit integer");
                source.frameStep = entry.at("frameStep").get<std::int64_t>();
                if (source.frameStep == 0) {
                    throw DeserializeError("source '" + it.key() + "': 'frameStep' must not be zero");
                }
            }
            if (auto interpretation = entry.find("interpretation");
                interpretation != entry.end() && interpretation->is_object()) {
                for (auto tag = interpretation->begin(); tag != interpretation->end(); ++tag) {
                    if (!tag.value().is_string())
                        throw DeserializeError("source '" + it.key() + "': interpretation '" + tag.key() +
                                               "' must be a string");
                    source.interpretation[tag.key()] = tag.value().get<std::string>();
                }
            } else if (interpretation != entry.end()) {
                throw DeserializeError("source '" + it.key() + "': 'interpretation' must be an object");
            }
            result.document.sources[it.key()] = std::move(source);
        }
    } else if (sources != json.end()) {
        throw DeserializeError("document 'sources' field must be an object");
    }

    // Pass 1: nodes. Persisted IDs are restored verbatim; files from before
    // IDs were required receive a generated identity and a migration warning.
    const auto nodes = json.find("nodes");
    if (nodes != json.end() && !nodes->is_array()) {
        throw DeserializeError("document 'nodes' field must be an array");
    }
    const auto edges = json.find("edges");
    if (edges != json.end() && !edges->is_array()) {
        throw DeserializeError("document 'edges' field must be an array");
    }
    std::set<std::string> seenNames;
    std::set<NodeId> seenNodeIds;
    std::set<NodeId> declaredNodeIds;
    std::map<NodeId, NodeId> idMap;
    std::size_t nodeIndex = 0;
    static const nlohmann::json emptyArray = nlohmann::json::array();
    const auto& nodeEntries = nodes == json.end() ? emptyArray : *nodes;
    std::size_t declarationIndex = 0;
    for (const auto& entry : nodeEntries) {
        const std::string context = "node " + std::to_string(declarationIndex++);
        if (entry.is_object() && entry.contains("id")) {
            const auto id = optionalId(entry, "id", context);
            if (!declaredNodeIds.insert(*id).second) {
                throw DeserializeError("duplicate node id in file: " + std::to_string(*id));
            }
        }
    }
    nodeIndex = 0;
    for (const auto& entry : nodeEntries) {
        const std::string context = "node " + std::to_string(nodeIndex++);
        if (!entry.is_object() || !entry.contains("type") || !entry.contains("name") || !entry.at("type").is_string() ||
            !entry.at("name").is_string()) {
            throw DeserializeError(context + ": string 'type' and 'name' are required");
        }
        Node node;
        node.type = entry.at("type").get<std::string>();
        node.name = entry.at("name").get<std::string>();
        if (!seenNames.insert(node.name).second) {
            throw DeserializeError("duplicate node name in file: '" + node.name + "'");
        }
        if (entry.contains("params")) {
            if (!entry.at("params").is_object()) {
                throw DeserializeError(context + ": 'params' must be an object");
            }
            for (auto it = entry.at("params").begin(); it != entry.at("params").end(); ++it) {
                if (!it.value().is_string()) {
                    throw DeserializeError(context + ": parameter '" + it.key() + "' must be a string");
                }
                node.params[it.key()] = it.value().get<std::string>();
            }
        }
        const auto persistedId = optionalId(entry, "id", context);
        NodeId id = kInvalidNode;
        if (persistedId) {
            id = *persistedId;
            if (!seenNodeIds.insert(id).second) {
                throw DeserializeError("duplicate node id in file: " + std::to_string(id));
            }
        } else {
            id = result.document.graph.nextNodeId();
            while (declaredNodeIds.contains(id)) {
                if (id == std::numeric_limits<NodeId>::max()) {
                    throw DeserializeError(context + ": no available identity for legacy node");
                }
                ++id;
            }
            result.warnings.push_back(context + " has no id; allocated a new identity for compatibility");
        }
        try {
            id = result.document.graph.addNodeWithId(id, std::move(node.type), std::move(node.name),
                                                     std::move(node.params));
        } catch (const GraphException& error) {
            throw DeserializeError(context + ": " + error.what());
        }
        // Legacy files used insertion-order identities, so map the generated
        // identity as the endpoint key as well as the current node identity.
        idMap.emplace(persistedId.value_or(id), id);
        if (result.document.graph.descriptor(result.document.graph.node(id)->type) == nullptr) {
            result.warnings.push_back("unknown node type '" + result.document.graph.node(id)->type + "' (node '" +
                                      result.document.graph.node(id)->name + "'); retained as data, not evaluated");
        }
    }

    std::set<EdgeId> seenEdgeIds;
    std::size_t edgeIndex = 0;
    const auto& edgeEntries = edges == json.end() ? emptyArray : *edges;
    for (const auto& entry : edgeEntries) {
        const std::string context = "edge " + std::to_string(edgeIndex++);
        if (!entry.is_object() || !entry.contains("from") || !entry.contains("to")) {
            throw DeserializeError(context + ": 'from' and 'to' are required");
        }
        const auto& fromEntry = entry.at("from");
        const auto& toEntry = entry.at("to");
        const auto fromFileId = requiredEndpointNode(fromEntry, context + " from");
        const auto toFileId = requiredEndpointNode(toEntry, context + " to");
        const auto fromIt = idMap.find(fromFileId);
        const auto toIt = idMap.find(toFileId);
        const auto from = fromIt == idMap.end()
                              ? std::optional<PortRef>{}
                              : std::optional<PortRef>{{fromIt->second, port(fromEntry, context + " from")}};
        const auto to = toIt == idMap.end() ? std::optional<PortRef>{}
                                            : std::optional<PortRef>{{toIt->second, port(toEntry, context + " to")}};
        const auto persistedId = optionalId(entry, "id", context);
        if (persistedId && !seenEdgeIds.insert(*persistedId).second) {
            throw DeserializeError("duplicate edge id in file: " + std::to_string(*persistedId));
        }
        if (!from || !to) {
            result.warnings.push_back(context + " references a node that is not present in the file; dropped");
            continue;
        }
        if (auto problem = result.document.graph.validateEdge(*from, *to)) {
            result.warnings.push_back("invalid edge in file: " + problem->message);
            continue;
        }
        try {
            if (persistedId) {
                static_cast<void>(result.document.graph.connectWithId(*persistedId, *from, *to));
            } else {
                result.warnings.push_back(context + " has no id; allocated a new identity for compatibility");
                static_cast<void>(result.document.graph.connect(*from, *to));
            }
        } catch (const GraphException& error) {

            throw DeserializeError(context + ": " + error.what());
        }
    }

    const auto nextNodeId = optionalWatermark(json, "nextNodeId");
    const auto nextEdgeId = optionalWatermark(json, "nextEdgeId");
    result.document.graph.restoreIdentityHighWatermarks(nextNodeId.value_or(result.document.graph.nextNodeId()),
                                                        nextEdgeId.value_or(result.document.graph.nextEdgeId()));
    return result;
}

}  // namespace nemo
