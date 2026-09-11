#include "ProjectSessionCommand.hpp"

#include <algorithm>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectSession.hpp"

namespace {
using Json = nlohmann::json;

template <typename T>
[[nodiscard]] T unsignedValue(const Json& object, const char* key) {
    if (!object.contains(key) || !object.at(key).is_number_unsigned())
        throw std::invalid_argument(std::string{"expected nonnegative integer field "} + key);
    const auto value = object.at(key).get<std::uint64_t>();
    if (value > std::numeric_limits<T>::max())
        throw std::invalid_argument(std::string{"integer field out of range: "} + key);
    return static_cast<T>(value);
}

[[nodiscard]] nemo::NodeId nodeIdAt(const Json& command, const char* idKey) {
    return unsignedValue<nemo::NodeId>(command, idKey);
}

[[nodiscard]] nemo::Command makeCommand(const nemo::ProjectSession& session, const Json& command) {
    const std::string op = command.at("op").get<std::string>();
    if (op == "add-node")
        return nemo::addNodeCommand(command.at("type").get<std::string>(), command.at("name").get<std::string>());
    if (op == "set-param") {
        const nemo::NodeId id = nodeIdAt(command, "node_id");
        return nemo::setParamCommand(id, command.at("key").get<std::string>(), command.at("value").get<std::string>());
    }
    if (op == "rename-node") {
        const nemo::NodeId id = nodeIdAt(command, "node_id");
        return nemo::renameNodeCommand(id, command.at("name").get<std::string>());
    }
    if (op == "connect") {
        const nemo::NodeId from = nodeIdAt(command, "from_node_id");
        const nemo::NodeId to = nodeIdAt(command, "to_node_id");
        return nemo::connectCommand({from, unsignedValue<std::uint32_t>(command, "from_port")},
                                    {to, unsignedValue<std::uint32_t>(command, "to_port")});
    }
    if (op == "transaction") {
        const auto& commands = command.at("commands");
        if (!commands.is_array() || commands.empty())
            throw std::invalid_argument("transaction requires a nonempty commands array");
        std::vector<nemo::Command> nested;
        nested.reserve(commands.size());
        for (const auto& nestedCommand : commands)
            nested.push_back(makeCommand(session, nestedCommand));
        return nemo::transactionCommand(command.value("label", std::string{"transaction"}), std::move(nested));
    }
    throw std::invalid_argument("unsupported edit operation '" + op + "'");
}

[[nodiscard]] nemo::EditOptions editOptions(const Json& request) {
    nemo::EditOptions options;
    options.expectedRevision = unsignedValue<std::uint64_t>(request, "expected_revision");
    if (request.contains("request_id")) {
        if (!request.at("request_id").is_string())
            throw std::invalid_argument("request_id must be a string");
        options.requestId = request.at("request_id").get<std::string>();
    }
    return options;
}

[[nodiscard]] const char* errorCode(nemo::EditErrorCode code) {
    switch (code) {
    case nemo::EditErrorCode::InvalidArgument:
        return "invalid_argument";
    case nemo::EditErrorCode::MissingObject:
        return "missing_object";
    case nemo::EditErrorCode::RevisionConflict:
        return "revision_conflict";
    case nemo::EditErrorCode::Unavailable:
        return "unavailable";
    case nemo::EditErrorCode::ReentrantMutation:
        return "reentrant_mutation";
    }
    throw std::logic_error("unrecognized edit error code");
}

void putIds(Json& target, const char* key, const auto& ids) {
    target[key] = Json::array();
    for (const auto& id : ids)
        target[key].push_back(id);
}

[[nodiscard]] Json editResultJson(const nemo::EditResult& result) {
    Json output{{"committed", result.committed}, {"revision", result.revision}};
    if (result.error)
        output["error"] = Json{{"code", errorCode(result.error->code)}, {"message", result.error->message}};
    else if (!result.committed)
        output["error"] = Json{{"code", "edit_rejected"}, {"message", "edit was rejected"}};
    putIds(output, "changed_node_ids", result.changedNodeIds);
    putIds(output, "created_node_ids", result.createdNodeIds);
    putIds(output, "changed_edge_ids", result.changedEdgeIds);
    putIds(output, "created_edge_ids", result.createdEdgeIds);
    putIds(output, "changed_source_ids", result.changedSourceIds);
    output["color_policy_changed"] = result.colorPolicyChanged;
    return output;
}

[[nodiscard]] Json query(const nemo::ProjectSession& session, const Json& request) {
    const std::string filter = request.value("filter", std::string{});
    const std::string type = request.value("type", std::string{});
    const std::string name = request.value("name", std::string{});
    const std::string keyFilter = request.value("key_filter", std::string{});
    const std::size_t limit =
        std::min<std::size_t>(request.contains("limit") ? unsignedValue<std::size_t>(request, "limit") : 256, 256);
    auto nodeAfter = request.contains("node_after") ? unsignedValue<nemo::NodeId>(request, "node_after") : 0;
    const auto edgeAfter = request.contains("edge_after") ? unsignedValue<nemo::EdgeId>(request, "edge_after") : 0;
    const auto touching = request.contains("node_id") ? unsignedValue<nemo::NodeId>(request, "node_id") : 0;
    const auto keyAfter = request.value("key_after", std::string{});
    Json nodes = Json::array();
    while (nodes.size() < limit) {
        const auto page = session.queryNodes(filter, limit - nodes.size(), nodeAfter);
        if (page.empty())
            break;
        for (const auto& node : page) {
            nodeAfter = node.id;
            if ((touching != 0 && node.id != touching) || (!type.empty() && node.type != type) ||
                (!name.empty() && node.name != name))
                continue;
            Json params = Json::object();
            for (const auto& value : session.queryValues(node.id, keyFilter, limit, keyAfter))
                params[value.key] = value.value;
            nodes.push_back(
                Json{{"id", node.id}, {"type", node.type}, {"name", node.name}, {"params", std::move(params)}});
        }
    }
    Json edges = Json::array();
    auto nextEdge = edgeAfter;
    for (const auto& edge : session.queryEdges(touching, limit, edgeAfter)) {
        edges.push_back(Json{{"id", edge.id},
                             {"from_node_id", edge.from.node},
                             {"from_port", edge.from.port},
                             {"to_node_id", edge.to.node},
                             {"to_port", edge.to.port}});
        nextEdge = edge.id;
    }
    Json sources = Json::array();
    for (const auto& source : session.querySources(request.value("source_filter", std::string{}), limit,
                                                   request.value("source_after", std::string{})))
        sources.push_back(Json{{"id", source.id},
                               {"path", source.reference.path},
                               {"frame_offset", source.reference.frameOffset},
                               {"frame_step", source.reference.frameStep}});
    return Json{{"revision", session.revision()}, {"nodes", std::move(nodes)},    {"edges", std::move(edges)},
                {"sources", std::move(sources)},  {"next_node_after", nodeAfter}, {"next_edge_after", nextEdge}};
}

}  // namespace

int commandProjectSession(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "usage: nemo-cli project-session <project.json>\n";
        return 2;
    }
    try {
        std::ifstream input(args.front());
        if (!input)
            throw std::runtime_error("cannot open project: " + args.front());
        const auto loaded = nemo::loadDocument(nlohmann::json::parse(input));
        for (const auto& warning : loaded.warnings)
            std::cerr << "project-session: warning: " << warning << '\n';
        nemo::ProjectSession session(loaded.document);
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty())
                continue;
            try {
                const Json request = Json::parse(line);
                const std::string op = request.at("op").get<std::string>();
                Json response;
                if (op == "query") {
                    response = query(session, request);
                } else if (op == "changes") {
                    const auto history = session.changesSince(unsignedValue<std::uint64_t>(request, "since"));
                    Json events = Json::array();
                    for (const auto& event : history.events)
                        events.push_back(Json{{"revision", event.revision},
                                              {"changed_node_ids", event.changedNodeIds},
                                              {"changed_edge_ids", event.changedEdgeIds},
                                              {"created_node_ids", event.createdNodeIds},
                                              {"created_edge_ids", event.createdEdgeIds},
                                              {"changed_source_ids", event.changedSourceIds},
                                              {"color_policy_changed", event.colorPolicyChanged}});
                    response = Json{{"revision", history.currentRevision},
                                    {"resync_required", history.resyncRequired},
                                    {"events", std::move(events)}};
                } else if (op == "undo") {
                    response = editResultJson(session.undo(editOptions(request)));
                } else if (op == "redo") {
                    response = editResultJson(session.redo(editOptions(request)));
                } else {
                    response = editResultJson(session.submit(makeCommand(session, request), editOptions(request)));
                }
                response["ok"] = !response.contains("committed") || response.at("committed").get<bool>();
                std::cout << response.dump() << '\n' << std::flush;
            } catch (const std::exception& error) {
                std::cout << Json{{"ok", false},
                                  {"error", Json{{"code", "invalid_request"}, {"message", error.what()}}},
                                  {"revision", session.revision()}}
                          << '\n'
                          << std::flush;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project-session: " << error.what() << '\n';
        return 1;
    }
}
