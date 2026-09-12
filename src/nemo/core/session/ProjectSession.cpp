#include "nemo/core/session/ProjectSession.hpp"

#include "nemo/core/commands/AnimationCommands.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nemo {
namespace {
constexpr std::size_t kRequestCapacity = 256;

bool samePorts(const std::vector<PortSpec>& left, const std::vector<PortSpec>& right) {
    if (left.size() != right.size())
        return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](const PortSpec& a, const PortSpec& b) {
        return a.kind == b.kind && a.name == b.name && a.optional == b.optional;
    });
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
bool isDiscreteValue(const ParameterValue& value) {
    return std::holds_alternative<bool>(value) || std::holds_alternative<std::int64_t>(value) ||
           std::holds_alternative<std::string>(value) || std::holds_alternative<ChoiceValue>(value);
}

Keyframe keyframeForEdit(const Document& document, const Document* priorSnapshot, const ParameterEdit& edit,
                         double time) {
    Keyframe key;
    key.time = time;
    key.value = *edit.value;
    if (const auto* channel = document.animationChannel(edit.address)) {
        const auto found = std::find_if(channel->keys.begin(), channel->keys.end(),
                                        [time](const Keyframe& candidate) { return candidate.time == time; });
        if (found != channel->keys.end()) {
            key = *found;
            key.value = *edit.value;
            return key;
        }
    }
    if (priorSnapshot != nullptr) {
        if (const auto* channel = priorSnapshot->animationChannel(edit.address)) {
            const auto found = std::find_if(channel->keys.begin(), channel->keys.end(),
                                            [time](const Keyframe& candidate) { return candidate.time == time; });
            if (found != channel->keys.end()) {
                key = *found;
                key.id = kInvalidKeyframe;
                key.value = *edit.value;
                return key;
            }
        }
    }
    key.interpolation = isDiscreteValue(*edit.value) ? KeyInterpolation::Hold : KeyInterpolation::Linear;
    return key;
}

std::vector<KeyframeEdit> makeKeyframeEdits(const Document& document, const Document* priorSnapshot, double time,
                                            const std::vector<ParameterEdit>& edits) {
    std::vector<KeyframeEdit> result;
    result.reserve(edits.size());
    for (const auto& edit : edits) {
        if (!edit.value)
            throw std::invalid_argument("keyed parameter edits cannot reset a parameter");
        result.push_back(KeyframeEdit{edit.address, keyframeForEdit(document, priorSnapshot, edit, time)});
    }
    return result;
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
    : document_(std::move(document)), commands_(document_, historyCapacity), eventCapacity_(historyCapacity) {
    captureSavedBaseline();
}

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
        result.changedSourceIds.insert(result.changedSourceIds.end(), event.changedSourceIds.begin(),
                                       event.changedSourceIds.end());
        result.changedMediaEntryIds.insert(result.changedMediaEntryIds.end(), event.changedMediaEntryIds.begin(),
                                           event.changedMediaEntryIds.end());
        result.changedMediaBinIds.insert(result.changedMediaBinIds.end(), event.changedMediaBinIds.begin(),
                                         event.changedMediaBinIds.end());
        result.changedAnimationChannelIds.insert(result.changedAnimationChannelIds.end(),
                                                 event.changedAnimationChannelIds.begin(),
                                                 event.changedAnimationChannelIds.end());
        result.changedAnimationKeyIds.insert(result.changedAnimationKeyIds.end(), event.changedAnimationKeyIds.begin(),
                                             event.changedAnimationKeyIds.end());
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
    for (const auto& value : before.mediaCatalog.entries()) {
        const auto* current = after.mediaCatalog.entry(value.id);
        if (current == nullptr || *current != value)
            result.changedMediaEntryIds.push_back(value.id);
    }
    for (const auto& value : after.mediaCatalog.entries()) {
        if (before.mediaCatalog.entry(value.id) == nullptr) {
            result.createdMediaEntryIds.push_back(value.id);
            result.changedMediaEntryIds.push_back(value.id);
        }
    }
    for (const auto& value : before.mediaCatalog.bins()) {
        const auto* current = after.mediaCatalog.bin(value.id);
        if (current == nullptr || *current != value)
            result.changedMediaBinIds.push_back(value.id);
    }
    for (const auto& value : after.mediaCatalog.bins()) {
        if (before.mediaCatalog.bin(value.id) == nullptr) {
            result.createdMediaBinIds.push_back(value.id);
            result.changedMediaBinIds.push_back(value.id);
        }
    }

    const auto addAnimationChange = [&](const AnimationChannel& channel) {
        result.changedAnimationChannelIds.push_back(channel.id);
        const ScopedNodeId node{channel.address.network, channel.address.node};
        if (std::find(result.changedNodeIds.begin(), result.changedNodeIds.end(), node) == result.changedNodeIds.end())
            result.changedNodeIds.push_back(node);
        addNetworkChange(channel.address.network);
        if (channel.address.instance != kInvalidNetworkInstance &&
            std::find(result.changedInstanceIds.begin(), result.changedInstanceIds.end(), channel.address.instance) ==
                result.changedInstanceIds.end())
            result.changedInstanceIds.push_back(channel.address.instance);
    };
    for (const auto& channel : before.animationChannels()) {
        const auto* current = after.animationChannel(channel.id);
        if (current == nullptr || *current != channel)
            addAnimationChange(channel);
        if (current == nullptr)
            for (const auto& key : channel.keys)
                result.changedAnimationKeyIds.push_back(KeyframeRef{channel.id, key.id});
        else {
            for (const auto& key : channel.keys) {
                const auto prior = std::find_if(current->keys.begin(), current->keys.end(),
                                                [&](const Keyframe& value) { return value.id == key.id; });
                if (prior == current->keys.end() || *prior != key)
                    result.changedAnimationKeyIds.push_back(KeyframeRef{channel.id, key.id});
            }
        }
    }
    for (const auto& channel : after.animationChannels()) {
        const auto* previous = before.animationChannel(channel.id);
        if (previous == nullptr) {
            addAnimationChange(channel);
            for (const auto& key : channel.keys)
                result.changedAnimationKeyIds.push_back(KeyframeRef{channel.id, key.id});
        } else {
            for (const auto& key : channel.keys) {
                const auto prior = std::find_if(previous->keys.begin(), previous->keys.end(),
                                                [&](const Keyframe& value) { return value.id == key.id; });
                if (prior == previous->keys.end())
                    result.changedAnimationKeyIds.push_back(KeyframeRef{channel.id, key.id});
            }
        }
    }
    result.colorPolicyChanged = before.color != after.color;

    ChangeEvent event{result.revision,
                      result.changedNodeIds,
                      result.createdNodeIds,
                      result.changedEdgeIds,
                      result.createdEdgeIds,
                      result.changedNetworkIds,
                      result.createdNetworkIds,
                      result.changedInstanceIds,
                      result.createdInstanceIds,
                      result.changedSourceIds,
                      result.changedAnimationChannelIds,
                      result.changedAnimationKeyIds,
                      result.colorPolicyChanged,
                      result.changedMediaEntryIds,
                      result.createdMediaEntryIds,
                      result.changedMediaBinIds,
                      result.createdMediaBinIds};
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
                                                      error.errorCode() == GraphError::UnknownInstance ||
                                                      error.errorCode() == GraphError::UnknownMediaEntry ||
                                                      error.errorCode() == GraphError::UnknownMediaBin ||
                                                      error.errorCode() == GraphError::MissingMediaSource
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
    invalidateDirtyCache();
    notifyObservers();
    return result;
}

EditResult ProjectSession::submit(Command command, EditOptions options) {
    return execute(Operation::Submit, &command, options);
}

EditResult ProjectSession::undo(EditOptions options) {
    return execute(Operation::Undo, nullptr, options);
}

ParameterGestureResult ProjectSession::gestureFailure(std::string message, EditErrorCode code) const {
    ParameterGestureResult result;
    result.result = failure(std::move(message), code);
    return result;
}

ParameterGestureResult ProjectSession::gestureFailure(const EditResult& result) const {
    ParameterGestureResult gesture;
    gesture.result = result;
    return gesture;
}

ParameterGestureResult ProjectSession::makeGesturePreview(ParameterGestureToken token, std::uint64_t expectedRevision,
                                                          std::shared_ptr<const Document> snapshot) const {
    ParameterGestureResult result;
    result.result.revision = revision_;
    result.token = token;
    result.expectedRevision = expectedRevision;
    result.snapshot = std::move(snapshot);
    return result;
}

ParameterGestureResult ProjectSession::previewFailure(const GraphException& error) const {
    const auto code = error.errorCode() == GraphError::UnknownNode || error.errorCode() == GraphError::UnknownEdge ||
                              error.errorCode() == GraphError::UnknownNetwork ||
                              error.errorCode() == GraphError::UnknownInstance
                          ? EditErrorCode::MissingObject
                          : EditErrorCode::InvalidArgument;
    auto result = gestureFailure(error.what(), code);
    result.result.error->graphError = error.errorCode();
    return result;
}

ParameterGestureResult ProjectSession::beginParameterGestureInternal(std::vector<ParameterEdit> edits,
                                                                     EditOptions options,
                                                                     std::optional<double> keyedTime) {
    if (keyedTime && !std::isfinite(*keyedTime))
        return gestureFailure("keyed parameter gesture time must be finite");
    if (edits.empty())
        return gestureFailure("parameter batch must contain at least one edit");
    if (mutating_ || notifying_)
        return gestureFailure("project session mutation is not allowed during an edit or notification",
                              EditErrorCode::ReentrantMutation);
    if (options.requestId.size() > 256)
        return gestureFailure("request identity exceeds the 256-byte session limit");
    if (options.expectedRevision != revision_)
        return gestureFailure(conflict(options.expectedRevision));
    if (gesture_)
        return gestureFailure("a parameter gesture is already active", EditErrorCode::Unavailable);
    if (nextGestureToken_ == std::numeric_limits<ParameterGestureToken>::max())
        return gestureFailure("parameter gesture token space exhausted", EditErrorCode::Unavailable);
    if (keyedTime) {
        for (const auto& edit : edits)
            if (!edit.value)
                return gestureFailure("keyed parameter edits cannot reset a parameter");
    }
    try {
        auto snapshot = std::make_shared<Document>(document_);
        if (keyedTime) {
            auto keyEdits = makeKeyframeEdits(document_, nullptr, *keyedTime, edits);
            Command preview = setKeyframesCommand(std::move(keyEdits));
            preview.apply(*snapshot);
        } else {
            Command preview = setParametersCommand(edits);
            preview.apply(*snapshot);
        }
        snapshot->synchronizeReferences();
        const auto token = nextGestureToken_++;
        ParameterGestureState state{token,
                                    options.expectedRevision,
                                    snapshot,
                                    std::move(edits),
                                    keyedTime.has_value(),
                                    keyedTime.value_or(0.0)};
        gesture_.emplace(std::move(state));
        return makeGesturePreview(token, options.expectedRevision, std::move(snapshot));
    } catch (const GraphException& error) {
        return previewFailure(error);
    } catch (const std::exception& error) {
        return previewFailure(error);
    } catch (...) {
        return gestureFailure("parameter gesture preview failed with an unknown error");
    }
}

ParameterGestureResult ProjectSession::beginKeyedParameterGesture(double time, std::vector<ParameterEdit> edits,
                                                                  EditOptions options) {
    return beginParameterGestureInternal(std::move(edits), std::move(options), time);
}

ParameterGestureResult ProjectSession::beginParameterGesture(std::vector<ParameterEdit> edits, EditOptions options) {
    return beginParameterGestureInternal(std::move(edits), std::move(options), std::nullopt);
}

ParameterGestureResult ProjectSession::previewFailure(const std::exception& error) const {
    return gestureFailure(error.what());
}

ParameterGestureResult ProjectSession::updateParameterGesture(ParameterGestureToken token,
                                                              std::vector<ParameterEdit> edits) {
    if (mutating_ || notifying_)
        return gestureFailure("project session mutation is not allowed during an edit or notification",
                              EditErrorCode::ReentrantMutation);
    if (!gesture_ || gesture_->token != token)
        return gestureFailure("unknown parameter gesture token", EditErrorCode::Unavailable);
    if (gesture_->expectedRevision != revision_)
        return gestureFailure(conflict(gesture_->expectedRevision));
    if (edits.empty())
        return gestureFailure("parameter batch must contain at least one edit");

    try {
        std::vector<ParameterEdit> merged = gesture_->edits;
        for (const auto& edit : edits) {
            if (gesture_->keyed && !edit.value)
                throw std::invalid_argument("keyed parameter edits cannot reset a parameter");
            const auto existing = std::find_if(merged.begin(), merged.end(), [&](const ParameterEdit& previous) {
                return previous.address == edit.address;
            });
            if (existing == merged.end())
                merged.push_back(edit);
            else
                existing->value = edit.value;
        }
        auto snapshot = std::make_shared<Document>(document_);
        if (gesture_->keyed) {
            auto keyEdits = makeKeyframeEdits(document_, gesture_->snapshot.get(), gesture_->time, merged);
            Command preview = setKeyframesCommand(std::move(keyEdits));
            preview.apply(*snapshot);
        } else {
            Command preview = setParametersCommand(merged);
            preview.apply(*snapshot);
        }
        snapshot->synchronizeReferences();
        gesture_->edits = std::move(merged);
        gesture_->snapshot = snapshot;
        return makeGesturePreview(gesture_->token, gesture_->expectedRevision, std::move(snapshot));
    } catch (const GraphException& error) {
        return previewFailure(error);
    } catch (const std::exception& error) {
        return previewFailure(error);
    } catch (...) {
        return gestureFailure("parameter gesture preview failed with an unknown error");
    }
}

ParameterGestureResult ProjectSession::updateKeyedParameterGesture(ParameterGestureToken token,
                                                                   std::vector<ParameterEdit> edits) {
    if (!gesture_ || !gesture_->keyed)
        return gestureFailure("unknown keyed parameter gesture token", EditErrorCode::Unavailable);
    return updateParameterGesture(token, std::move(edits));
}

EditResult ProjectSession::commitParameterGesture(ParameterGestureToken token, EditOptions options) {
    if (mutating_ || notifying_)
        return failure("project session mutation is not allowed during an edit or notification",
                       EditErrorCode::ReentrantMutation);
    if (!gesture_ || gesture_->token != token)
        return failure("unknown parameter gesture token", EditErrorCode::Unavailable);
    if (gesture_->expectedRevision != revision_)
        return conflict(gesture_->expectedRevision);
    if (options.expectedRevision != gesture_->expectedRevision)
        return conflict(options.expectedRevision);

    Command command;
    try {
        if (gesture_->keyed) {
            auto keyEdits = makeKeyframeEdits(document_, gesture_->snapshot.get(), gesture_->time, gesture_->edits);
            command = setKeyframesCommand(std::move(keyEdits));
        } else {
            command = setParametersCommand(gesture_->edits);
        }
    } catch (const std::exception& error) {
        return failure(error.what());
    }
    auto result = execute(Operation::Submit, &command, options);
    if (result.committed)
        gesture_.reset();
    return result;
}

EditResult ProjectSession::cancelParameterGesture(ParameterGestureToken token) {
    if (mutating_ || notifying_)
        return failure("project session mutation is not allowed during an edit or notification",
                       EditErrorCode::ReentrantMutation);
    if (!gesture_ || gesture_->token != token)
        return failure("unknown parameter gesture token", EditErrorCode::Unavailable);
    gesture_.reset();
    EditResult result;
    result.revision = revision_;
    return result;
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
    const auto& graph = document_.network(network).graph();
    const NodeInstance* node = graph.node(nodeId);
    if (!node)
        return result;

    struct EffectiveValue {
        std::string_view key;
        const ParameterValue* value;
    };
    std::vector<EffectiveValue> effective;
    effective.reserve(node->params.size());
    for (const auto& [key, value] : node->params)
        effective.push_back(EffectiveValue{key, &value});
    if (const auto* descriptor = graph.catalog().find(node->type)) {
        for (const auto& spec : descriptor->parameters) {
            if (node->params.contains(spec.name))
                continue;
            if (const auto* value = graph.catalog().parameterDefault(node->type, spec.name))
                effective.push_back(EffectiveValue{spec.name, value});
        }
    }
    std::sort(effective.begin(), effective.end(),
              [](const EffectiveValue& left, const EffectiveValue& right) { return left.key < right.key; });
    for (const auto& entry : effective) {
        if ((!after.empty() && entry.key <= after) ||
            (!keyFilter.empty() && entry.key.find(keyFilter) == std::string_view::npos))
            continue;
        result.push_back(ValueQueryResult{network, nodeId, std::string(entry.key), *entry.value});
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
std::vector<AnimationChannelQueryResult> ProjectSession::queryAnimationChannels(std::size_t limit,
                                                                                AnimationChannelId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<AnimationChannelQueryResult> result;
    if (limit == 0)
        return result;
    for (const auto& channel : document_.animationChannels()) {
        if (channel.id <= after)
            continue;
        result.push_back(AnimationChannelQueryResult{channel.id, channel.address, channel.keys.size()});
        if (result.size() == limit)
            break;
    }
    return result;
}

std::vector<AnimationKeyQueryResult> ProjectSession::queryAnimationKeys(AnimationChannelId channelId, std::size_t limit,
                                                                        KeyframeId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<AnimationKeyQueryResult> result;
    if (limit == 0)
        return result;
    const auto* channel = document_.animationChannel(channelId);
    if (channel == nullptr)
        return result;
    for (const auto& key : channel->keys) {
        if (key.id <= after)
            continue;
        const auto where =
            std::lower_bound(result.begin(), result.end(), key.id,
                             [](const AnimationKeyQueryResult& value, KeyframeId id) { return value.key.id < id; });
        if (where == result.end() && result.size() == limit)
            continue;
        result.insert(where, AnimationKeyQueryResult{channelId, key});
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
std::vector<MediaQueryResult> ProjectSession::queryMedia(std::string_view filter, std::optional<MediaKind> kind,
                                                         std::optional<bool> offline, std::optional<bool> unused,
                                                         MediaBinId scope, std::size_t limit,
                                                         MediaSourceId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<MediaQueryResult> result;
    if (limit == 0)
        return result;
    for (const auto id : document_.mediaCatalog.search(document_, filter, kind, offline, unused, scope)) {
        if (id <= after)
            continue;
        const auto* value = document_.mediaCatalog.entry(id);
        result.push_back(MediaQueryResult{id, value->sourceKey, value->parent, value->metadata});
        if (result.size() == limit)
            break;
    }
    return result;
}

std::vector<MediaBin> ProjectSession::queryMediaBins(MediaBinId parent, std::size_t limit, MediaBinId after) const {
    limit = std::min<std::size_t>(limit, 256);
    std::vector<MediaBin> result;
    if (limit == 0)
        return result;
    for (const auto id : document_.mediaCatalog.childBins(parent)) {
        if (id <= after)
            continue;
        result.push_back(*document_.mediaCatalog.bin(id));
        if (result.size() == limit)
            break;
    }
    return result;
}

void ProjectSession::captureSavedBaseline() noexcept {
    savedRevision_ = revision_;
    try {
        savedContent_ = ProjectFile::serializeContent(document_, presentation_, colorConfigPath_);
        savedContentValid_ = true;
    } catch (...) {
        savedContent_.clear();
        savedContentValid_ = false;
    }
}

bool ProjectSession::isDirty() const {
    if (dirtyCacheStamp_ == stateStamp_)
        return dirtyCacheValue_;
    if (savedContentValid_)
        dirtyCacheValue_ = ProjectFile::serializeContent(document_, presentation_, colorConfigPath_) != savedContent_;
    else
        dirtyCacheValue_ = revision_ != savedRevision_;
    dirtyCacheStamp_ = stateStamp_;
    return dirtyCacheValue_;
}

void ProjectSession::setPresentation(nlohmann::json presentation) {
    presentation_ = std::move(presentation);
    invalidateDirtyCache();
}

void ProjectSession::setColorConfigPath(std::string colorConfigPath) {
    colorConfigPath_ = std::move(colorConfigPath);
    invalidateDirtyCache();
}

void ProjectSession::setLastFileError(std::string message) {
    lastFileError_ = std::move(message);
}

ProjectWriteRequest ProjectSession::prepareSave(std::filesystem::path target, PathPolicy pathPolicy,
                                                bool backup) const {
    ProjectWriteRequest request;
    request.snapshot = std::make_shared<const Document>(document_);
    request.target = std::move(target);
    request.presentation = presentation_;
    request.colorConfigPath = colorConfigPath_;
    request.pathPolicy = pathPolicy;
    request.projectBase = request.target.parent_path();
    request.backup = backup;
    request.protectedTarget = recovered_ ? recoveryOriginal_ : std::filesystem::path{};
    request.expectedRevision = revision_;
    request.projectGeneration = projectGeneration_;
    request.baseline = ProjectFile::serializeContent(document_, presentation_, colorConfigPath_);
    return request;
}

EditResult ProjectSession::commitSave(const ProjectWriteRequest& request, const ProjectWriteResult& result) {
    if (mutating_ || notifying_)
        return failure("project session mutation is not allowed during an edit or notification",
                       EditErrorCode::ReentrantMutation);
    if (!result.ok) {
        lastFileError_ = result.error.message.empty() ? std::string("project save failed") : result.error.message;
        return failure(lastFileError_, EditErrorCode::IoError);
    }
    // A completion prepared before the session was replaced belongs to a
    // project that is no longer open: never adopt its path or baseline, even if
    // the revision happens to line up.
    if (request.projectGeneration != projectGeneration_) {
        lastFileError_ = "project save completed for a project that is no longer open; the result was not applied";
        return failure(lastFileError_, EditErrorCode::IoError);
    }
    const std::filesystem::path target = result.target.empty() ? request.target : result.target;
    if (!target.empty()) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(target, ec);
        projectPath_ = (ec ? target : absolute).lexically_normal();
    }
    // The written snapshot is the saved baseline. When the session advanced
    // past that snapshot the session stays dirty; newer edits are never
    // cleared by a stale completion.
    savedRevision_ = request.expectedRevision;
    savedContent_ = request.baseline;
    savedContentValid_ = !savedContent_.empty();
    if (!savedContentValid_)
        savedRevision_ = revision_;
    // The recovered-copy guard persists for this session's lifetime: even after
    // saving to a copy, an explicit later choice of the original target is
    // still refused. Opening or creating another project retires it.
    lastFileError_.clear();
    invalidateDirtyCache();
    EditResult committed;
    committed.committed = true;
    committed.revision = revision_;
    return committed;
}

ProjectReplaceResult ProjectSession::replaceInternal(Document document, std::filesystem::path path,
                                                     nlohmann::json presentation, std::string colorConfigPath,
                                                     bool recovered, std::filesystem::path recoveryOriginal) {
    if (mutating_ || notifying_)
        return {false, revision_,
                EditError{"project session mutation is not allowed during an edit or notification",
                          EditErrorCode::ReentrantMutation,
                          {}}};
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return {false, revision_, EditError{"project session revision exhausted", EditErrorCode::Unavailable, {}}};
    try {
        document.synchronizeReferences();
    } catch (const std::exception& error) {
        return {false, revision_, EditError{error.what(), EditErrorCode::InvalidArgument, {}}};
    } catch (...) {
        return {false, revision_,
                EditError{"replacement document failed validation", EditErrorCode::InvalidArgument, {}}};
    }

    document_ = std::move(document);
    commands_.clear();
    gesture_.reset();
    requests_.clear();
    events_.clear();
    ++revision_;
    ++projectGeneration_;
    projectPath_ = std::move(path);
    presentation_ = std::move(presentation);
    colorConfigPath_ = std::move(colorConfigPath);
    recovered_ = recovered;
    recoveryOriginal_ = recovered ? std::move(recoveryOriginal) : std::filesystem::path{};
    lastFileError_.clear();
    if (recovered) {
        // A recovery copy has no saved file: it is an unsaved document.
        savedContent_.clear();
        savedContentValid_ = false;
        savedRevision_ = 0;
    } else {
        captureSavedBaseline();
    }
    invalidateDirtyCache();
    notifyObservers();
    return {true, revision_, std::nullopt};
}

ProjectReplaceResult ProjectSession::replaceDocument(Document document, std::filesystem::path path,
                                                     nlohmann::json presentation, std::string colorConfigPath) {
    return replaceInternal(std::move(document), std::move(path), std::move(presentation), std::move(colorConfigPath),
                           false, {});
}

ProjectReplaceResult ProjectSession::replaceDocument(ProjectReadResult result, bool recovered) {
    if (!result.ok) {
        const std::string message =
            result.error.message.empty() ? std::string("project read failed") : result.error.message;
        lastFileError_ = message;
        return {false, revision_, EditError{message, EditErrorCode::IoError, {}}};
    }
    const bool asRecovery = recovered || result.recovered;
    return replaceInternal(std::move(result.document), std::move(result.sourcePath), std::move(result.presentation),
                           std::move(result.colorConfigPath), asRecovery, std::move(result.recoveryOriginal));
}

ProjectReplaceResult ProjectSession::open(ProjectReadResult result) {
    return replaceDocument(std::move(result), false);
}

}  // namespace nemo
