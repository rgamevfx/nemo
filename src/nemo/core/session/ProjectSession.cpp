#include "nemo/core/session/ProjectSession.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nemo {
namespace {
constexpr std::size_t kRequestCapacity = 256;

bool sameNode(const Node& left, const Node& right) {
    return left.id == right.id && left.type == right.type && left.name == right.name && left.params == right.params;
}

bool sameEdge(const Edge& left, const Edge& right) {
    return left.id == right.id && left.from == right.from && left.to == right.to;
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

    for (const auto& node : before.graph.nodes()) {
        const Node* current = after.graph.node(node.id);
        if (!current || !sameNode(node, *current))
            result.changedNodeIds.push_back(node.id);
    }
    for (const auto& node : after.graph.nodes()) {
        const Node* previous = before.graph.node(node.id);
        if (!previous) {
            result.createdNodeIds.push_back(node.id);
            result.changedNodeIds.push_back(node.id);
        }
    }
    for (const auto& edge : before.graph.edges()) {
        const auto current = std::find_if(after.graph.edges().begin(), after.graph.edges().end(),
                                          [&](const Edge& candidate) { return candidate.id == edge.id; });
        if (current == after.graph.edges().end() || !sameEdge(edge, *current))
            result.changedEdgeIds.push_back(edge.id);
    }
    for (const auto& edge : after.graph.edges()) {
        const auto previous = std::find_if(before.graph.edges().begin(), before.graph.edges().end(),
                                           [&](const Edge& candidate) { return candidate.id == edge.id; });
        if (previous == before.graph.edges().end()) {
            result.createdEdgeIds.push_back(edge.id);
            result.changedEdgeIds.push_back(edge.id);
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

    // Complete all allocation before CommandStack's noexcept publication.
    // If either bounded journal append fails, roll back the other append.
    ChangeEvent event{result.revision,       result.changedNodeIds,   result.createdNodeIds,    result.changedEdgeIds,
                      result.createdEdgeIds, result.changedSourceIds, result.colorPolicyChanged};
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
                                                      error.errorCode() == GraphError::UnknownEdge
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

std::vector<NodeQueryResult> ProjectSession::queryNodes(std::string_view filter, std::size_t limit,
                                                        NodeId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<const Node*> selected;
    for (const auto& node : document_.graph.nodes()) {
        if (limit == 0 || node.id <= after ||
            (!filter.empty() && node.name.find(filter) == std::string::npos &&
             node.type.find(filter) == std::string::npos))
            continue;
        const auto where = std::lower_bound(selected.begin(), selected.end(), node.id,
                                            [](const Node* value, NodeId id) { return value->id < id; });
        if (where == selected.end() && selected.size() == limit)
            continue;
        selected.insert(where, &node);
        if (selected.size() > limit)
            selected.pop_back();
    }
    std::vector<NodeQueryResult> result;
    result.reserve(selected.size());
    for (const auto* node : selected)
        result.push_back(NodeQueryResult{node->id, node->type, node->name});
    return result;
}

std::vector<ValueQueryResult> ProjectSession::queryValues(NodeId nodeId, std::string_view keyFilter, std::size_t limit,
                                                          std::string_view after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<ValueQueryResult> result;
    if (limit == 0)
        return result;
    const Node* node = document_.graph.node(nodeId);
    if (!node)
        return result;
    for (const auto& [key, value] : node->params) {
        if ((!after.empty() && key <= after) || (!keyFilter.empty() && key.find(keyFilter) == std::string::npos))
            continue;
        result.push_back(ValueQueryResult{nodeId, key, value});
        if (result.size() == limit)
            break;
    }
    return result;
}

std::vector<Edge> ProjectSession::queryEdges(NodeId touching, std::size_t limit, EdgeId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<Edge> result;
    for (const auto& edge : document_.graph.edges()) {
        if (limit == 0 || edge.id <= after ||
            (touching != kInvalidNode && edge.from.node != touching && edge.to.node != touching))
            continue;
        const auto where = std::lower_bound(result.begin(), result.end(), edge.id,
                                            [](const Edge& value, EdgeId id) { return value.id < id; });
        if (where == result.end() && result.size() == limit)
            continue;
        result.insert(where, edge);
        if (result.size() > limit)
            result.pop_back();
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
