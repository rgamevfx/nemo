#include "nemo/core/document/Serialization.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace nemo {

namespace {

// Node types this build knows how to evaluate come from the graph's port
// interface table (one source of truth). Unknown types load fine and surface
// as warnings; they are never silently dropped.

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
    return {{"schema", Document::kSchemaVersion},
            {"name", document.name},
            {"color",
             {{"workingSpace", document.color.workingSpace},
              {"viewerTransform", document.color.viewerTransform},
              {"deliveryTransform", document.color.deliveryTransform}}},
            {"nodes", nodes},
            {"edges", edges}};
}

LoadResult loadDocument(const nlohmann::json& json) {
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

    LoadResult result;
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

    // Pass 1: nodes (ids are remapped through insertion order to keep Graph's
    // identity guarantees; edges are re-linked by the same map).
    std::set<std::string> seenNames;
    std::map<std::uint64_t, NodeId> idMap;
    for (const auto& entry : json.value("nodes", nlohmann::json::array())) {
        if (!entry.is_object() || !entry.contains("type") || !entry.contains("name")) {
            throw DeserializeError("malformed node entry: 'type' and 'name' are required");
        }
        Node node;
        node.type = entry.at("type").get<std::string>();
        node.name = entry.at("name").get<std::string>();
        if (!seenNames.insert(node.name).second) {
            throw DeserializeError("duplicate node name in file: '" + node.name + "'");
        }
        if (entry.contains("params")) {
            for (auto it = entry.at("params").begin(); it != entry.at("params").end(); ++it) {
                node.params[it.key()] = it.value().get<std::string>();
            }
        }
        const std::uint64_t fileId = entry.value("id", std::uint64_t{0});
        const NodeId newId = result.document.graph.addNode(node.type, node.name);
        result.document.graph.node(newId)->params = node.params;
        if (fileId != 0) {
            idMap.emplace(fileId, newId);
        }
        if (!isKnownNodeType(node.type)) {
            result.warnings.push_back("unknown node type '" + node.type + "' (node '" + node.name +
                                      "'); retained as data, not evaluated");
        }
    }

    // Pass 2: edges, remapped onto the freshly inserted node identities.
    for (const auto& entry : json.value("edges", nlohmann::json::array())) {
        if (!entry.is_object() || !entry.contains("from") || !entry.contains("to")) {
            throw DeserializeError("malformed edge entry: 'from' and 'to' are required");
        }
        const auto mapEnd = [idMap](const nlohmann::json& ref) -> std::optional<PortRef> {
            const auto nodeIt = idMap.find(ref.value("node", std::uint64_t{0}));
            if (nodeIt == idMap.end()) {
                return std::nullopt;
            }
            return PortRef{nodeIt->second, ref.value("port", std::uint32_t{0})};
        };
        const auto from = mapEnd(entry.at("from"));
        const auto to = mapEnd(entry.at("to"));
        if (!from || !to) {
            result.warnings.push_back("dropped edge referencing a node that is not present in the file");
            continue;
        }
        if (auto problem = result.document.graph.validateEdge(*from, *to)) {
            // A file whose stored edges are invalid still loads; the graph
            // keeps everything else and the violation is reported.
            result.warnings.push_back("invalid edge in file: " + problem->message);
            continue;
        }
        static_cast<void>(result.document.graph.connect(*from, *to));
    }

    return result;
}

}  // namespace nemo
