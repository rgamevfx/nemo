#include "ProjectSessionCommand.hpp"

#include <algorithm>

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/ParameterValueJson.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectFile.hpp"
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

[[nodiscard]] nemo::NetworkId networkIdAt(const Json& object) {
    const auto network = unsignedValue<nemo::NetworkId>(object, "network_id");
    if (network == nemo::kInvalidNetwork)
        throw std::invalid_argument("network_id must be nonzero");
    return network;
}

[[nodiscard]] nemo::NodeId nodeIdAt(const Json& command, const char* idKey) {
    return unsignedValue<nemo::NodeId>(command, idKey);
}

[[nodiscard]] const char* parameterTypeName(nemo::ParameterType type) {
    switch (type) {
    case nemo::ParameterType::Boolean:
        return "boolean";
    case nemo::ParameterType::Integer:
        return "integer";
    case nemo::ParameterType::Float:
        return "float";
    case nemo::ParameterType::Choice:
        return "choice";
    case nemo::ParameterType::Vector2:
        return "vector2";
    case nemo::ParameterType::Vector3:
        return "vector3";
    case nemo::ParameterType::Color:
        return "color";
    case nemo::ParameterType::String:
        return "string";
    }
    throw std::logic_error("unrecognized parameter type");
}

[[nodiscard]] nemo::ParameterEdit parameterEditAt(const Json& edit) {
    if (!edit.is_object())
        throw std::invalid_argument("parameter edit must be an object");
    nemo::ParameterEdit result;
    result.address.network = networkIdAt(edit);
    result.address.node = nodeIdAt(edit, "node_id");
    result.address.key = edit.at("key").get<std::string>();
    if (result.address.key.empty())
        throw std::invalid_argument("parameter edit key must not be empty");
    if (edit.contains("instance_id")) {
        result.address.instance = unsignedValue<nemo::NetworkInstanceId>(edit, "instance_id");
        if (result.address.instance == nemo::kInvalidNetworkInstance)
            throw std::invalid_argument("instance_id must be nonzero");
    }
    if (edit.contains("value") && !edit.at("value").is_null())
        result.value = nemo::parameterValueFromJson(edit.at("value"));
    return result;
}

[[nodiscard]] std::vector<nemo::ParameterEdit> parameterEditsAt(const Json& request) {
    const auto& edits = request.at("edits");
    if (!edits.is_array() || edits.empty())
        throw std::invalid_argument("parameter edits require a nonempty edits array");
    std::vector<nemo::ParameterEdit> result;
    result.reserve(edits.size());
    for (const auto& edit : edits)
        result.push_back(parameterEditAt(edit));
    return result;
}

[[nodiscard]] nemo::Command makeCommand(const nemo::ProjectSession& session, const Json& command) {
    const std::string op = command.at("op").get<std::string>();
    if (op == "add-node") {
        return nemo::addNodeCommand(networkIdAt(command), command.at("type").get<std::string>(),
                                    command.at("name").get<std::string>());
    }
    if (op == "set-param") {
        return nemo::setParamCommand(networkIdAt(command), nodeIdAt(command, "node_id"),
                                     command.at("key").get<std::string>(),
                                     nemo::parameterValueFromJson(command.at("value")));
    }
    if (op == "set-instance-param") {
        return nemo::setInstanceParamCommand(unsignedValue<nemo::NetworkInstanceId>(command, "instance_id"),
                                             nodeIdAt(command, "node_id"), command.at("key").get<std::string>(),
                                             nemo::parameterValueFromJson(command.at("value")));
    }
    if (op == "reset-param") {
        return nemo::resetParamCommand(networkIdAt(command), nodeIdAt(command, "node_id"),
                                       command.at("key").get<std::string>());
    }
    if (op == "reset-instance-param") {
        return nemo::resetInstanceParamCommand(unsignedValue<nemo::NetworkInstanceId>(command, "instance_id"),
                                               nodeIdAt(command, "node_id"), command.at("key").get<std::string>());
    }
    if (op == "set-parameters") {
        return nemo::setParametersCommand(parameterEditsAt(command));
    }
    if (op == "rename-node") {
        const nemo::NetworkId network = networkIdAt(command);
        const nemo::NodeId id = nodeIdAt(command, "node_id");
        return nemo::renameNodeCommand(network, id, command.at("name").get<std::string>());
    }
    if (op == "connect") {
        const nemo::NetworkId network = networkIdAt(command);
        const nemo::NodeId from = nodeIdAt(command, "from_node_id");
        const nemo::NodeId to = nodeIdAt(command, "to_node_id");
        return nemo::connectCommand(network, {from, unsignedValue<std::uint32_t>(command, "from_port")},
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
    case nemo::EditErrorCode::IoError:
        return "io_error";
    }
    throw std::logic_error("unrecognized edit error code");
}

void putNodeIds(Json& target, const char* key, const std::vector<nemo::ScopedNodeId>& ids) {
    target[key] = Json::array();
    for (const auto& id : ids)
        target[key].push_back(Json{{"network", id.network}, {"id", id.id}});
}

void putEdgeIds(Json& target, const char* key, const std::vector<nemo::ScopedEdgeId>& ids) {
    target[key] = Json::array();
    for (const auto& id : ids)
        target[key].push_back(Json{{"network", id.network}, {"id", id.id}});
}

void putNetworkIds(Json& target, const char* key, const std::vector<nemo::NetworkId>& ids) {
    target[key] = ids;
}

void putAnimationKeyIds(Json& target, const char* key, const std::vector<nemo::KeyframeRef>& ids) {
    target[key] = Json::array();
    for (const auto& id : ids)
        target[key].push_back(Json{{"channel", id.channel}, {"key", id.key}});
}

[[nodiscard]] Json editResultJson(const nemo::EditResult& result) {
    Json output{{"committed", result.committed}, {"revision", result.revision}};
    if (result.error)
        output["error"] = Json{{"code", errorCode(result.error->code)}, {"message", result.error->message}};
    else if (!result.committed)
        output["error"] = Json{{"code", "edit_rejected"}, {"message", "edit was rejected"}};
    putNodeIds(output, "changed_node_ids", result.changedNodeIds);
    putNodeIds(output, "created_node_ids", result.createdNodeIds);
    putEdgeIds(output, "changed_edge_ids", result.changedEdgeIds);
    putEdgeIds(output, "created_edge_ids", result.createdEdgeIds);
    putNetworkIds(output, "changed_network_ids", result.changedNetworkIds);
    putNetworkIds(output, "created_network_ids", result.createdNetworkIds);
    output["changed_instance_ids"] = result.changedInstanceIds;
    output["created_instance_ids"] = result.createdInstanceIds;
    output["changed_source_ids"] = result.changedSourceIds;
    output["changed_animation_channel_ids"] = result.changedAnimationChannelIds;
    putAnimationKeyIds(output, "changed_animation_key_ids", result.changedAnimationKeyIds);
    output["color_policy_changed"] = result.colorPolicyChanged;
    return output;
}

[[nodiscard]] Json gestureResultJson(const nemo::ParameterGestureResult& result) {
    Json output = editResultJson(result.result);
    output["token"] = result.token;
    output["expected_revision"] = result.expectedRevision;
    if (!result.result.error)
        output.erase("error");
    if (result.snapshot)
        output["preview"] = nemo::saveDocument(*result.snapshot);
    output["preview_only"] = true;
    return output;
}

[[nodiscard]] Json gestureCancelResultJson(const nemo::EditResult& result) {
    Json output = editResultJson(result);
    if (!result.error)
        output.erase("error");
    output["preview_only"] = true;
    return output;
}

[[nodiscard]] Json query(const nemo::ProjectSession& session, const Json& request) {
    const nemo::NetworkId network = networkIdAt(request);
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
        const auto page = session.queryNodes(network, filter, limit - nodes.size(), nodeAfter);
        if (page.empty())
            break;
        for (const auto& node : page) {
            nodeAfter = node.id;
            if ((touching != 0 && node.id != touching) || (!type.empty() && node.type != type) ||
                (!name.empty() && node.name != name))
                continue;
            Json params = Json::object();
            for (const auto& value : session.queryValues(network, node.id, keyFilter, limit, keyAfter))
                params[value.key] = nemo::parameterValueToJson(value.value);
            nodes.push_back(Json{{"network", node.network},
                                 {"id", node.id},
                                 {"type", node.type},
                                 {"name", node.name},
                                 {"params", std::move(params)}});
        }
    }
    Json edges = Json::array();
    auto nextEdge = edgeAfter;
    for (const auto& result : session.queryEdges(network, touching, limit, edgeAfter)) {
        edges.push_back(Json{{"network", result.network},
                             {"id", result.edge.id},
                             {"from_node_id", result.edge.from.node},
                             {"from_port", result.edge.from.port},
                             {"to_node_id", result.edge.to.node},
                             {"to_port", result.edge.to.port}});
        nextEdge = result.edge.id;
    }
    Json sources = Json::array();
    for (const auto& source : session.querySources(request.value("source_filter", std::string{}), limit,
                                                   request.value("source_after", std::string{})))
        sources.push_back(Json{{"id", source.id},
                               {"path", source.reference.path},
                               {"frame_offset", source.reference.frameOffset},
                               {"frame_step", source.reference.frameStep}});
    return Json{{"revision", session.revision()}, {"network", network},
                {"nodes", std::move(nodes)},      {"edges", std::move(edges)},
                {"sources", std::move(sources)},  {"next_node_after", nodeAfter},
                {"next_edge_after", nextEdge}};
}

[[nodiscard]] Json catalog(const nemo::ProjectSession& session) {
    const auto& graph = session.document().network(session.document().rootNetworkId()).graph();
    Json descriptors = Json::array();
    for (const auto& descriptor : graph.catalog().descriptors()) {
        Json inputs = Json::array();
        for (const auto& port : descriptor.inputs)
            inputs.push_back(Json{{"name", port.name},
                                  {"kind", port.kind == nemo::PortKind::Image  ? "image"
                                           : port.kind == nemo::PortKind::Mask ? "mask"
                                                                               : "media"}});
        Json outputs = Json::array();
        for (const auto& port : descriptor.outputs)
            outputs.push_back(Json{{"name", port.name},
                                   {"kind", port.kind == nemo::PortKind::Image  ? "image"
                                            : port.kind == nemo::PortKind::Mask ? "mask"
                                                                                : "media"}});
        Json parameters = Json::array();
        for (const auto& parameter : descriptor.parameters) {
            Json value{{"name", parameter.name},
                       {"type", parameterTypeName(parameter.type)},
                       {"default_value", nemo::parameterValueToJson(parameter.defaultValue)},
                       {"choices", parameter.choices}};
            if (parameter.minimum)
                value["minimum"] = *parameter.minimum;
            if (parameter.maximum)
                value["maximum"] = *parameter.maximum;
            parameters.push_back(std::move(value));
        }
        descriptors.push_back(Json{{"type", descriptor.type},
                                   {"display_name", descriptor.displayName},
                                   {"group", descriptor.group},
                                   {"is_output", descriptor.isOutput},
                                   {"version", descriptor.implementationVersion},
                                   {"inputs", std::move(inputs)},
                                   {"outputs", std::move(outputs)},
                                   {"parameters", std::move(parameters)},
                                   {"sampling_scales", descriptor.capabilities.samplingScales},
                                   {"channels", descriptor.capabilities.channels},
                                   {"temporal", descriptor.capabilities.temporal}});
    }
    return Json{{"revision", session.revision()}, {"descriptors", std::move(descriptors)}};
}

[[nodiscard]] const char* projectErrorCode(nemo::ProjectFileError::Code code) {
    switch (code) {
    case nemo::ProjectFileError::Code::None:
        return "none";
    case nemo::ProjectFileError::Code::InvalidArgument:
        return "invalid_argument";
    case nemo::ProjectFileError::Code::NotFound:
        return "not_found";
    case nemo::ProjectFileError::Code::NotAFile:
        return "not_a_file";
    case nemo::ProjectFileError::Code::ReadFailed:
        return "read_failed";
    case nemo::ProjectFileError::Code::ParseFailed:
        return "parse_failed";
    case nemo::ProjectFileError::Code::Unsupported:
        return "unsupported";
    case nemo::ProjectFileError::Code::WriteFailed:
        return "write_failed";
    case nemo::ProjectFileError::Code::ReplaceFailed:
        return "replace_failed";
    case nemo::ProjectFileError::Code::BackupFailed:
        return "backup_failed";
    case nemo::ProjectFileError::Code::TargetIsDirectory:
        return "target_is_directory";
    case nemo::ProjectFileError::Code::ProtectedTarget:
        return "protected_target";
    case nemo::ProjectFileError::Code::UnsupportedTarget:
        return "unsupported_target";
    }
    return "unknown";
}

[[nodiscard]] const char* referenceStateName(nemo::ReferenceState state) {
    switch (state) {
    case nemo::ReferenceState::Present:
        return "present";
    case nemo::ReferenceState::Missing:
        return "missing";
    case nemo::ReferenceState::Unresolved:
        return "unresolved";
    }
    return "unknown";
}

// Explicit relative/absolute policy for how stored external references are
// written. Headless processing never interprets presentation records; it only
// preserves them across open/save.
[[nodiscard]] nemo::PathPolicy pathPolicyAt(const Json& request) {
    const std::string policy = request.value("path_policy", std::string{"relative"});
    if (policy == "relative")
        return nemo::PathPolicy::RebaseRelative;
    if (policy == "absolute")
        return nemo::PathPolicy::RebaseAbsolute;
    if (policy == "keep")
        return nemo::PathPolicy::KeepStored;
    throw std::invalid_argument("path_policy must be relative, absolute or keep");
}

[[nodiscard]] Json projectErrorJson(const char* code, const std::string& message, std::uint64_t revision) {
    return Json{{"ok", false}, {"error", Json{{"code", code}, {"message", message}}}, {"revision", revision}};
}

[[nodiscard]] Json referenceListJson(const std::vector<nemo::ExternalReference>& references) {
    Json output = Json::array();
    for (const auto& reference : references)
        output.push_back(Json{{"identity", reference.identity},
                              {"stored_path", reference.storedPath},
                              {"resolved_path", reference.resolvedPath},
                              {"state", referenceStateName(reference.state)},
                              {"relative_capable", reference.relativeCapable}});
    return output;
}

// Missing and unresolved reference diagnostics from the shared owner; used to
// refresh warnings after a save without duplicating dependency classification.
[[nodiscard]] std::vector<std::string> dependencyWarnings(const nemo::Document& document,
                                                          const std::string& colorConfigPath,
                                                          const std::filesystem::path& projectBase) {
    const std::vector<nemo::ExternalReference> references =
        nemo::ProjectFile::referenceState(document, colorConfigPath, projectBase);
    std::vector<std::string> warnings = nemo::ProjectFile::missingDependencyWarnings(references);
    const std::vector<std::string> unresolved = nemo::ProjectFile::unresolvedDependencyWarnings(references);
    warnings.insert(warnings.end(), unresolved.begin(), unresolved.end());
    return warnings;
}

[[nodiscard]] Json fileStateJson(const nemo::ProjectSession& session, const std::vector<std::string>& warnings) {
    Json output{{"ok", true},
                {"op", "file-state"},
                {"dirty", session.isDirty()},
                {"revision", session.revision()},
                {"saved_revision", session.savedRevision()},
                {"can_undo", session.canUndo()},
                {"can_redo", session.canRedo()},
                {"path", session.projectPath().string()},
                {"recovered", session.recovered()},
                {"recovery_original", session.recoveryOriginal().string()},
                {"color_config", session.colorConfigPath()},
                {"has_presentation", !session.presentation().is_null()},
                {"warnings", warnings}};
    if (!session.lastFileError().empty())
        output["error"] = Json{{"code", "file_error"}, {"message", session.lastFileError()}};
    return output;
}

// Open or recover a project as an explicit revision replacement: the caller
// must state the live revision it intends to discard, so a stale client cannot
// silently replace a document that advanced.
[[nodiscard]] Json openProjectJson(nemo::ProjectSession& session, const Json& request, bool recovery,
                                   std::vector<std::string>& warnings) {
    const std::string path = request.at(recovery ? "slot" : "path").get<std::string>();
    if (path.empty())
        throw std::invalid_argument(std::string{recovery ? "slot" : "path"} + " must not be empty");
    const std::uint64_t expected = unsignedValue<std::uint64_t>(request, "expected_revision");
    if (expected != session.revision()) {
        return projectErrorJson("revision_conflict",
                                "expected revision " + std::to_string(expected) + " but session is at " +
                                    std::to_string(session.revision()),
                                session.revision());
    }
    nemo::ProjectReadResult loaded = recovery ? nemo::ProjectFile::readRecovery(path) : nemo::ProjectFile::read(path);
    if (!loaded.ok) {
        session.setLastFileError(loaded.error.message);
        return projectErrorJson(projectErrorCode(loaded.error.code), loaded.error.message, session.revision());
    }
    warnings = loaded.warnings;
    Json references = referenceListJson(loaded.references);
    const nemo::ProjectReplaceResult replaced = session.open(std::move(loaded));
    if (!replaced.replaced) {
        const std::string message =
            replaced.error ? replaced.error->message : std::string{"project replacement failed"};
        session.setLastFileError(message);
        return projectErrorJson(replaced.error ? errorCode(replaced.error->code) : "replace_failed", message,
                                replaced.revision);
    }
    return Json{{"ok", true},
                {"op", recovery ? "recover" : "open"},
                {"revision", replaced.revision},
                {"path", session.projectPath().string()},
                {"recovered", session.recovered()},
                {"recovery_original", session.recoveryOriginal().string()},
                {"color_config", session.colorConfigPath()},
                {"has_presentation", !session.presentation().is_null()},
                {"references", std::move(references)},
                {"warnings", warnings}};
}

// Save or Save As through the shared file owner: prepareSave captures an owned
// snapshot and the session file state, writeAtomic performs the worker write,
// commitSave publishes the outcome on the owner thread. A failed write never
// replaces the document and never clears newer edits.
[[nodiscard]] Json saveProjectJson(nemo::ProjectSession& session, const Json& request, bool saveAs,
                                   std::vector<std::string>& warnings) {
    std::filesystem::path target;
    if (saveAs) {
        target = request.at("path").get<std::string>();
        if (target.empty())
            throw std::invalid_argument("save-as requires a nonempty path");
    } else {
        target = session.projectPath();
        if (target.empty())
            throw std::invalid_argument("session has no project path; use save-as");
    }
    nemo::ProjectWriteRequest writeRequest =
        session.prepareSave(target, pathPolicyAt(request), request.value("backup", true));
    const nemo::ProjectWriteResult writeResult = nemo::ProjectFile::writeAtomic(writeRequest);
    const nemo::EditResult saved = session.commitSave(writeRequest, writeResult);
    if (!saved.committed) {
        // A file-level failure keeps its specific machine-readable code; the
        // session's generic io_error is reserved for a post-write rejection
        // such as a stale project generation.
        if (!writeResult.ok) {
            const std::string message =
                writeResult.error.message.empty() ? std::string{"save failed"} : writeResult.error.message;
            return projectErrorJson(projectErrorCode(writeResult.error.code), message, saved.revision);
        }
        const std::string message = saved.error ? saved.error->message : std::string{"save failed"};
        return projectErrorJson(saved.error ? errorCode(saved.error->code) : "io_error", message, saved.revision);
    }
    warnings = dependencyWarnings(session.document(), session.colorConfigPath(), writeResult.target.parent_path());
    return Json{{"ok", true},
                {"op", saveAs ? "save-as" : "save"},
                {"path", writeResult.target.string()},
                {"backup", writeResult.backup.string()},
                {"revision", saved.revision},
                {"saved_revision", session.savedRevision()},
                {"dirty", session.isDirty()},
                {"recovered", session.recovered()},
                {"recovery_original", session.recoveryOriginal().string()},
                {"warnings", warnings}};
}

// Bounded autosave: writes a sibling copy, never the project target, and does
// not commit a save baseline, so the session stays dirty.
[[nodiscard]] Json autosaveJson(nemo::ProjectSession& session, const Json& request,
                                std::vector<std::string>& warnings) {
    if (session.projectPath().empty())
        throw std::invalid_argument("autosave requires a project path; save the project first");
    const std::size_t slots =
        request.contains("slots") ? unsignedValue<std::size_t>(request, "slots") : nemo::kDefaultAutosaveSlots;
    if (slots == 0)
        throw std::invalid_argument("slots must be at least 1");
    nemo::AutosaveStore store(session.projectPath(), slots);
    const nemo::ProjectWriteRequest writeRequest =
        session.prepareSave(session.projectPath(), pathPolicyAt(request), false);
    const nemo::ProjectWriteResult result = store.write(writeRequest);
    if (!result.ok) {
        session.setLastFileError(result.error.message);
        return projectErrorJson(projectErrorCode(result.error.code), result.error.message, session.revision());
    }
    warnings = dependencyWarnings(session.document(), session.colorConfigPath(), session.projectPath().parent_path());
    return Json{{"ok", true},
                {"op", "autosave"},
                {"slot", result.target.string()},
                {"bound", store.slotLimit()},
                {"slot_count", store.existingSlots().size()},
                {"revision", session.revision()},
                {"dirty", session.isDirty()},
                {"warnings", warnings}};
}

}  // namespace
int commandProjectSession(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "usage: nemo-cli project-session <project.json> "
                     "(graph queries and edits require network_id)\n"
                     "  file ops: file-state | open {path,expected_revision} | save {path_policy,backup} | "
                     "save-as {path,path_policy,backup} | autosave {slots} | recover {slot,expected_revision}\n";
        return 2;
    }
    try {
        nemo::ProjectReadResult loaded = nemo::ProjectFile::read(args.front());
        if (!loaded.ok)
            throw std::runtime_error(loaded.error.message.empty() ? "cannot open project: " + args.front()
                                                                  : loaded.error.message);
        std::vector<std::string> warnings = loaded.warnings;
        for (const auto& warning : warnings)
            std::cerr << "project-session: warning: " << warning << '\n';
        nemo::ProjectSession session;
        const nemo::ProjectReplaceResult opened = session.open(std::move(loaded));
        if (!opened.replaced)
            throw std::runtime_error(opened.error ? opened.error->message : "cannot open project: " + args.front());
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
                } else if (op == "catalog") {
                    response = catalog(session);
                } else if (op == "changes") {
                    const auto history = session.changesSince(unsignedValue<std::uint64_t>(request, "since"));
                    Json events = Json::array();
                    for (const auto& event : history.events) {
                        Json encoded{{"revision", event.revision},
                                     {"changed_source_ids", event.changedSourceIds},
                                     {"color_policy_changed", event.colorPolicyChanged}};
                        putNodeIds(encoded, "changed_node_ids", event.changedNodeIds);
                        putNodeIds(encoded, "created_node_ids", event.createdNodeIds);
                        putEdgeIds(encoded, "changed_edge_ids", event.changedEdgeIds);
                        putEdgeIds(encoded, "created_edge_ids", event.createdEdgeIds);
                        putNetworkIds(encoded, "changed_network_ids", event.changedNetworkIds);
                        putNetworkIds(encoded, "created_network_ids", event.createdNetworkIds);
                        encoded["changed_instance_ids"] = event.changedInstanceIds;
                        encoded["created_instance_ids"] = event.createdInstanceIds;
                        encoded["changed_animation_channel_ids"] = event.changedAnimationChannelIds;
                        putAnimationKeyIds(encoded, "changed_animation_key_ids", event.changedAnimationKeyIds);
                        events.push_back(std::move(encoded));
                    }
                    response = Json{{"revision", history.currentRevision},
                                    {"resync_required", history.resyncRequired},
                                    {"events", std::move(events)}};
                } else if (op == "begin-parameter-gesture") {
                    response = gestureResultJson(
                        session.beginParameterGesture(parameterEditsAt(request), editOptions(request)));
                } else if (op == "update-parameter-gesture") {
                    response = gestureResultJson(session.updateParameterGesture(
                        unsignedValue<nemo::ParameterGestureToken>(request, "token"), parameterEditsAt(request)));
                } else if (op == "commit-parameter-gesture") {
                    response = editResultJson(session.commitParameterGesture(
                        unsignedValue<nemo::ParameterGestureToken>(request, "token"), editOptions(request)));
                } else if (op == "cancel-parameter-gesture") {
                    response = gestureCancelResultJson(
                        session.cancelParameterGesture(unsignedValue<nemo::ParameterGestureToken>(request, "token")));
                } else if (op == "undo") {
                    response = editResultJson(session.undo(editOptions(request)));
                } else if (op == "redo") {
                    response = editResultJson(session.redo(editOptions(request)));
                } else if (op == "file-state") {
                    response = fileStateJson(session, warnings);
                } else if (op == "open") {
                    response = openProjectJson(session, request, false, warnings);
                } else if (op == "recover") {
                    response = openProjectJson(session, request, true, warnings);
                } else if (op == "save") {
                    response = saveProjectJson(session, request, false, warnings);
                } else if (op == "save-as") {
                    response = saveProjectJson(session, request, true, warnings);
                } else if (op == "autosave") {
                    response = autosaveJson(session, request, warnings);
                } else {
                    response = editResultJson(session.submit(makeCommand(session, request), editOptions(request)));
                }
                if (!response.contains("ok"))
                    response["ok"] = response.contains("preview_only")
                                         ? !response.contains("error")
                                         : (!response.contains("committed") || response.at("committed").get<bool>());
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
