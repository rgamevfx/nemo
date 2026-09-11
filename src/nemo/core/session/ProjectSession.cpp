#include "nemo/core/session/ProjectSession.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nemo {
namespace {
constexpr std::size_t kRequestCapacity = 256;

bool samePorts(const std::vector<PortSpec>& left, const std::vector<PortSpec>& right) {
    if (left.size() != right.size())
        return false;
    return std::equal(left.begin(), left.end(), right.begin(),
                      [](const PortSpec& a, const PortSpec& b) { return a.kind == b.kind && a.name == b.name; });
}

bool sameNode(const NodeInstance& left, const NodeInstance& right) {
    return left.id == right.id && left.type == right.type && left.name == right.name && left.params == right.params &&
           left.layout == right.layout && left.definition == right.definition && left.instance == right.instance &&
           left.hasPortContract == right.hasPortContract && samePorts(left.inputPorts, right.inputPorts) &&
           samePorts(left.outputPorts, right.outputPorts);
}
bool sameEdge(const Edge& left, const Edge& right) {
    return left.id == right.id && left.from == right.from && left.to == right.to && left.route == right.route;
}
}  // namespace

ProjectSession::Subscription::~Subscription() {
    reset();
}

ProjectSession::Subscription::Subscription(Subscription&& other) noexcept
    : session_(std::exchange(other.session_, nullptr)), id_(std::exchange(other.id_, 0)) {}

ProjectSession::Subscription& ProjectSession::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other)
        return *this;
    reset();
    session_ = std::exchange(other.session_, nullptr);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void ProjectSession::Subscription::reset() noexcept {
    ProjectSession* const session = std::exchange(session_, nullptr);
    const std::uint64_t id = std::exchange(id_, 0);
    if (session)
        session->unsubscribe(id);
}

ProjectSession::ProjectSession(Document document, std::size_t historyCapacity)
    : document_(std::move(document)), commands_(document_, historyCapacity), eventCapacity_(historyCapacity) {}

ProjectSession::Subscription ProjectSession::subscribe(void* context, ObserverCallback callback) {
    if (!callback)
        throw std::invalid_argument("project session observer callback must not be null");
    if (nextObserverId_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("project session observer id exhausted");
    const auto id = nextObserverId_++;
    observers_.emplace(id, Observer{context, callback});
    return Subscription(this, id);
}

void ProjectSession::unsubscribe(std::uint64_t id) noexcept {
    observers_.erase(id);
}

void ProjectSession::notifyObservers() noexcept {
    notifying_ = true;
    const auto boundary = nextObserverId_ - 1;
    auto observer = observers_.begin();
    while (observer != observers_.end() && observer->first <= boundary) {
        const auto id = observer->first;
        const auto callback = observer->second.callback;
        void* const context = observer->second.context;
        callback(context);
        observer = observers_.upper_bound(id);
    }
    notifying_ = false;
}

EditResult ProjectSession::failure(std::string message, EditErrorCode code) const {
    EditResult result;
    result.revision = revision_;
    result.error = EditError{std::move(message), code, {}};
    return result;
}

EditResult ProjectSession::conflict(std::uint64_t expected) const {
    auto result =
        failure("revision conflict: expected " + std::to_string(expected) + ", current " + std::to_string(revision_),
                EditErrorCode::RevisionConflict);
    for (const auto& event : changesSince(expected).events) {
        result.changedNodeIds.insert(result.changedNodeIds.end(), event.changedNodeIds.begin(),
                                     event.changedNodeIds.end());
        result.changedEdgeIds.insert(result.changedEdgeIds.end(), event.changedEdgeIds.begin(),
                                     event.changedEdgeIds.end());
        result.changedNetworkIds.insert(result.changedNetworkIds.end(), event.changedNetworkIds.begin(),
                                        event.changedNetworkIds.end());
        result.changedInstanceIds.insert(result.changedInstanceIds.end(), event.changedInstanceIds.begin(),
                                         event.changedInstanceIds.end());
        result.changedSourceIds.insert(result.changedSourceIds.end(), event.changedSourceIds.begin(),
                                       event.changedSourceIds.end());
        result.colorPolicyChanged |= event.colorPolicyChanged;
    }
    return result;
}

std::optional<EditResult> ProjectSession::duplicate(const std::string& requestId) const {
    if (!requestId.empty()) {
        for (const auto& request : requests_)
            if (request.id == requestId)
                return request.result;
    }
    return std::nullopt;
}

void ProjectSession::preparePublication(const Document& before, const Document& after, EditResult& result,
                                        const std::string& requestId) {
    result.committed = true;
    result.revision = revision_ + 1;

    const auto findNetwork = [](const Document& document, NetworkId id) -> const Network* {
        for (const auto& network : document.networks())
            if (network.id() == id)
                return &network;
        return nullptr;
    };
    const auto addNetworkChange = [&result](NetworkId id) {
        if (id != kInvalidNetwork)
            result.changedNetworkIds.push_back(id);
    };
    const auto addNodeChange = [&result](NetworkId network, NodeId id) {
        result.changedNodeIds.push_back(ScopedNodeId{network, id});
    };
    const auto addEdgeChange = [&result](NetworkId network, EdgeId id) {
        result.changedEdgeIds.push_back(ScopedEdgeId{network, id});
    };

    for (const auto& beforeNetwork : before.networks()) {
        const Network* current = findNetwork(after, beforeNetwork.id());
        if (current == nullptr || current->name() != beforeNetwork.name() ||
            current->defaultOutput() != beforeNetwork.defaultOutput() ||
            current->revision() != beforeNetwork.revision()) {
            addNetworkChange(beforeNetwork.id());
        }
        if (current == nullptr)
            continue;
        const auto& beforeGraph = beforeNetwork.graph();
        const auto& afterGraph = current->graph();
        for (const auto& node : beforeGraph.nodes()) {
            const NodeInstance* twin = afterGraph.node(node.id);
            if (twin == nullptr || !sameNode(node, *twin))
                addNodeChange(beforeNetwork.id(), node.id);
        }
        for (const auto& node : afterGraph.nodes()) {
            const NodeInstance* previous = beforeGraph.node(node.id);
            if (previous == nullptr) {
                result.createdNodeIds.push_back(ScopedNodeId{beforeNetwork.id(), node.id});
                addNodeChange(beforeNetwork.id(), node.id);
            }
        }
        for (const auto& edge : beforeGraph.edges()) {
            const auto currentEdge = std::find_if(afterGraph.edges().begin(), afterGraph.edges().end(),
                                                  [&](const Edge& candidate) { return candidate.id == edge.id; });
            if (currentEdge == afterGraph.edges().end() || !sameEdge(edge, *currentEdge))
                addEdgeChange(beforeNetwork.id(), edge.id);
        }
        for (const auto& edge : afterGraph.edges()) {
            const auto previous = std::find_if(beforeGraph.edges().begin(), beforeGraph.edges().end(),
                                               [&](const Edge& candidate) { return candidate.id == edge.id; });
            if (previous == beforeGraph.edges().end()) {
                result.createdEdgeIds.push_back(ScopedEdgeId{beforeNetwork.id(), edge.id});
                addEdgeChange(beforeNetwork.id(), edge.id);
            }
        }
    }
    for (const auto& afterNetwork : after.networks()) {
        if (findNetwork(before, afterNetwork.id()) == nullptr) {
            result.createdNetworkIds.push_back(afterNetwork.id());
            addNetworkChange(afterNetwork.id());
            for (const auto& node : afterNetwork.graph().nodes()) {
                result.createdNodeIds.push_back(ScopedNodeId{afterNetwork.id(), node.id});
                addNodeChange(afterNetwork.id(), node.id);
            }
            for (const auto& edge : afterNetwork.graph().edges()) {
                result.createdEdgeIds.push_back(ScopedEdgeId{afterNetwork.id(), edge.id});
                addEdgeChange(afterNetwork.id(), edge.id);
            }
        }
    }
    if (before.rootNetworkId() != after.rootNetworkId()) {
        addNetworkChange(before.rootNetworkId());
        addNetworkChange(after.rootNetworkId());
    }

    for (const auto& instance : before.instances()) {
        const NetworkInstance* current = after.instance(instance.id);
        if (current == nullptr || *current != instance)
            result.changedInstanceIds.push_back(instance.id);
    }
    for (const auto& instance : after.instances()) {
        if (before.instance(instance.id) == nullptr) {
            result.createdInstanceIds.push_back(instance.id);
            result.changedInstanceIds.push_back(instance.id);
        }
    }

    for (const auto& [id, source] : before.sources) {
        const auto current = after.sources.find(id);
        if (current == after.sources.end() || current->second != source)
            result.changedSourceIds.push_back(id);
    }
    for (const auto& [id, source] : after.sources)
        if (!before.sources.contains(id))
            result.changedSourceIds.push_back(id);
    result.colorPolicyChanged = before.color != after.color;

    ChangeEvent event{result.revision,          result.changedNodeIds,     result.createdNodeIds,
                      result.changedEdgeIds,    result.createdEdgeIds,     result.changedNetworkIds,
                      result.createdNetworkIds, result.changedInstanceIds, result.createdInstanceIds,
                      result.changedSourceIds,  result.colorPolicyChanged};
    if (!requestId.empty())
        requests_.push_back(RequestRecord{requestId, result});
    try {
        if (eventCapacity_ != 0)
            events_.push_back(std::move(event));
    } catch (...) {
        if (!requestId.empty())
            requests_.pop_back();
        throw;
    }
    if (requests_.size() > kRequestCapacity)
        requests_.pop_front();
    if (events_.size() > eventCapacity_)
        events_.pop_front();
}

EditResult ProjectSession::execute(Operation operation, Command* command, const EditOptions& options) {
    if (mutating_ || notifying_)
        return failure("project session mutation is not allowed during an edit or notification",
                       EditErrorCode::ReentrantMutation);
    if (options.requestId.size() > 256)
        return failure("request identity exceeds the 256-byte session limit");
    if (const auto replay = duplicate(options.requestId))
        return *replay;
    if (options.expectedRevision != revision_)
        return conflict(options.expectedRevision);
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return failure("project session revision exhausted", EditErrorCode::Unavailable);
    if ((operation == Operation::Undo && !commands_.canUndo()) ||
        (operation == Operation::Redo && !commands_.canRedo()))
        return failure("no history entry for requested operation", EditErrorCode::Unavailable);

    struct MutationGuard {
        bool& active;
        explicit MutationGuard(bool& value) : active(value) { active = true; }
        ~MutationGuard() { active = false; }
    } guard(mutating_);
    EditResult result;
    try {
        const CommandStack::BeforeCommit prepare = [&](const Document& before, const Document& after) {
            preparePublication(before, after, result, options.requestId);
        };
        switch (operation) {
        case Operation::Submit:
            commands_.push(std::move(*command), prepare);
            break;
        case Operation::Undo:
            static_cast<void>(commands_.undo(prepare));
            break;
        case Operation::Redo:
            static_cast<void>(commands_.redo(prepare));
            break;
        }
    } catch (const GraphException& error) {
        auto rejected = failure(error.what(), error.errorCode() == GraphError::UnknownNode ||
                                                      error.errorCode() == GraphError::UnknownEdge ||
                                                      error.errorCode() == GraphError::UnknownNetwork ||
                                                      error.errorCode() == GraphError::UnknownInstance
                                                  ? EditErrorCode::MissingObject
                                                  : EditErrorCode::InvalidArgument);
        rejected.error->graphError = error.errorCode();
        return rejected;
    } catch (const std::exception& error) {
        return failure(error.what());
    } catch (...) {
        return failure("command failed with an unknown error");
    }
    revision_ = result.revision;
    notifyObservers();
    return result;
}

EditResult ProjectSession::submit(Command command, EditOptions options) {
    return execute(Operation::Submit, &command, options);
}

EditResult ProjectSession::undo(EditOptions options) {
    return execute(Operation::Undo, nullptr, options);
}

EditResult ProjectSession::redo(EditOptions options) {
    return execute(Operation::Redo, nullptr, options);
}

std::vector<NodeQueryResult> ProjectSession::queryNodes(NetworkId network, std::string_view filter, std::size_t limit,
                                                        NodeId after) const {
    limit = std::min<std::size_t>(limit, 256);
    const auto& graph = document_.network(network).graph();
    std::vector<const NodeInstance*> selected;
    for (const auto& node : graph.nodes()) {
        if (limit == 0 || node.id <= after ||
            (!filter.empty() && node.name.find(filter) == std::string::npos &&
             node.type.find(filter) == std::string::npos))
            continue;
        const auto where = std::lower_bound(selected.begin(), selected.end(), node.id,
                                            [](const NodeInstance* value, NodeId id) { return value->id < id; });
        if (where == selected.end() && selected.size() == limit)
            continue;
        selected.insert(where, &node);
        if (selected.size() > limit)
            selected.pop_back();
    }
    std::vector<NodeQueryResult> result;
    result.reserve(selected.size());
    for (const auto* node : selected)
        result.push_back(NodeQueryResult{network, node->id, node->type, node->name});
    return result;
}

std::vector<ValueQueryResult> ProjectSession::queryValues(NetworkId network, NodeId nodeId, std::string_view keyFilter,
                                                          std::size_t limit, std::string_view after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<ValueQueryResult> result;
    if (limit == 0)
        return result;
    const NodeInstance* node = document_.network(network).graph().node(nodeId);
    if (!node)
        return result;
    for (const auto& [key, value] : node->params) {
        if ((!after.empty() && key <= after) || (!keyFilter.empty() && key.find(keyFilter) == std::string::npos))
            continue;
        result.push_back(ValueQueryResult{network, nodeId, key, value});
        if (result.size() == limit)
            break;
    }
    return result;
}

std::vector<EdgeQueryResult> ProjectSession::queryEdges(NetworkId network, NodeId touching, std::size_t limit,
                                                        EdgeId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<EdgeQueryResult> result;
    for (const auto& edge : document_.network(network).graph().edges()) {
        if (limit == 0 || edge.id <= after ||
            (touching != kInvalidNode && edge.from.node != touching && edge.to.node != touching))
            continue;
        const auto where = std::lower_bound(result.begin(), result.end(), edge.id,
                                            [](const EdgeQueryResult& value, EdgeId id) { return value.edge.id < id; });
        if (where == result.end() && result.size() == limit)
            continue;
        result.insert(where, EdgeQueryResult{network, edge});
        if (result.size() > limit)
            result.pop_back();
    }
    return result;
}
ChangeHistory ProjectSession::changesSince(std::uint64_t revision) const {
    ChangeHistory result;
    result.currentRevision = revision_;
    result.oldestRevision = events_.empty() ? revision_ : events_.front().revision;
    result.resyncRequired =
        revision > revision_ || (events_.empty() ? revision < revision_ : revision < events_.front().revision - 1);
    for (const auto& event : events_) {
        if (event.revision > revision)
            result.events.push_back(event);
    }
    return result;
}

std::vector<SourceQueryResult> ProjectSession::querySources(std::string_view filter, std::size_t limit,
                                                            std::string_view after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<SourceQueryResult> result;
    if (limit == 0)
        return result;
    for (auto it = document_.sources.upper_bound(std::string(after)); it != document_.sources.end(); ++it) {
        if (!filter.empty() && it->first.find(filter) == std::string::npos)
            continue;
        result.push_back(SourceQueryResult{it->first, it->second});
        if (result.size() == limit)
            break;
    }
    return result;
}

}  // namespace nemo
