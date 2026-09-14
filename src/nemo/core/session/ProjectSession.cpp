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

// Parameter authoring shares the animation owner's keyframe/value command
// helpers (AnimationCommands): the key identity, interpolation and tangent
// policy has one implementation there.

[[nodiscard]] std::optional<std::string> duplicateAddressProblem(const std::vector<ParameterEdit>& edits) {
    for (std::size_t index = 0; index < edits.size(); ++index) {
        for (std::size_t other = index + 1; other < edits.size(); ++other) {
            if (edits[other].address == edits[index].address)
                return "a parameter batch cannot edit '" + edits[index].address.key + "' twice";
        }
    }
    return std::nullopt;
}

// True when this edit actually moves its target away from the value frozen at
// gesture begin (a reset counts as a change only where a reset is meaningful).
[[nodiscard]] bool valueEditChanged(const ParameterEdit& edit, const std::vector<ParameterEdit>& frozen) {
    const auto baseline = std::find_if(frozen.begin(), frozen.end(), [&](const ParameterEdit& candidate) {
        return candidate.address == edit.address;
    });
    if (baseline == frozen.end())
        return true;
    if (!edit.value && !baseline->value)
        return false;
    if (!edit.value || !baseline->value)
        return true;
    return *edit.value != *baseline->value;
}

// Splits one mixed value batch by the routing captured at gesture begin. An
// address that had animation keeps the keyed contract; every other address is a
// plain static value and never creates a channel.
void partitionValueEdits(const std::vector<ParameterEdit>& edits, const std::vector<ParameterAddress>& keyedAddresses,
                         std::vector<ParameterEdit>& keyed, std::vector<ParameterEdit>& staticValues) {
    for (const ParameterEdit& edit : edits) {
        const bool animated =
            std::find(keyedAddresses.begin(), keyedAddresses.end(), edit.address) != keyedAddresses.end();
        (animated ? keyed : staticValues).push_back(edit);
    }
}

}  // namespace

ProjectSession::Subscription::~Subscription() {
    reset();
}

ProjectSession::Subscription::Subscription(Subscription&& other) noexcept {
    // Validate the live source session before either handle's identity is
    // exchanged.
    if (other.session_)
        other.session_->assertOwnerThread();
    session_ = other.session_;
    id_ = other.id_;
    other.session_ = nullptr;
    other.id_ = 0;
}

ProjectSession::Subscription& ProjectSession::Subscription::operator=(Subscription&& other) noexcept {
    // Both live sessions are validated before anything is mutated, including a
    // self-move, which is a no-op after the diagnostic.
    if (session_)
        session_->assertOwnerThread();
    if (other.session_)
        other.session_->assertOwnerThread();
    if (this == &other)
        return *this;
    // reset() unsubscribes from this handle's session; the handle then adopts
    // the other session.
    reset();
    session_ = other.session_;
    id_ = other.id_;
    other.session_ = nullptr;
    other.id_ = 0;
    return *this;
}

void ProjectSession::Subscription::reset() noexcept {
    if (session_)
        session_->assertOwnerThread();
    ProjectSession* const session = std::exchange(session_, nullptr);
    const std::uint64_t id = std::exchange(id_, 0);
    if (session)
        session->unsubscribe(id);
}

ProjectSession::~ProjectSession() {
    // Diagnosed before any member is torn down, so off-owner teardown is
    // reported against a fully constructed session. The destructor stays
    // implicitly noexcept: no member destructor is potentially throwing.
    assertOwnerThread();
}

ProjectSession::ProjectSession(Document document, std::size_t historyCapacity)
    : document_(std::move(document)), commands_(document_, historyCapacity), eventCapacity_(historyCapacity) {
    captureSavedBaseline();
}

ProjectSession::Subscription ProjectSession::subscribe(void* context, ObserverCallback callback) {
    assertOwnerThread();
    if (!callback)
        throw std::invalid_argument("project session observer callback must not be null");
    if (nextObserverId_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("project session observer id exhausted");
    const auto id = nextObserverId_++;
    observers_.emplace(id, Observer{context, callback});
    return Subscription(this, id);
}

void ProjectSession::unsubscribe(std::uint64_t id) noexcept {
    assertOwnerThread();
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

namespace {

// Identity lists for one network, already in ascending touched order.
template <class Id>
std::vector<Id> touchedIdsIn(const std::set<std::pair<NetworkId, Id>>& touched, NetworkId network) {
    std::vector<Id> ids;
    for (auto it = touched.lower_bound({network, Id{}}); it != touched.end() && it->first == network; ++it)
        ids.push_back(it->second);
    return ids;
}

// One pass over a version's records per touched network, matching the touched
// identities by binary search: publication costs one scan per version instead
// of one scan per touched identity, while still comparing only touched values.
template <class Record, class IdOf>
std::vector<const Record*> touchedRecords(const CowVector<Record>& records, const std::vector<std::uint64_t>& ids,
                                          IdOf idOf) {
    std::vector<const Record*> result(ids.size(), nullptr);
    for (const Record& record : records) {
        const std::uint64_t id = idOf(record);
        const auto position = std::lower_bound(ids.begin(), ids.end(), id);
        if (position != ids.end() && *position == id)
            result[static_cast<std::size_t>(position - ids.begin())] = &record;
    }
    return result;
}

// Applies a preview command to a private shared candidate and reconciles
// exactly the relationships it touched. The candidate is never published, and
// the transient recorder is released on every path so no retained snapshot
// holds a pointer to a finished transaction.
template <class Apply>
void applyPreview(Document& snapshot, ChangeRecorder& touched, Apply&& apply) {
    snapshot.beginRecording(&touched);
    try {
        apply();
    } catch (...) {
        snapshot.beginRecording(nullptr);
        throw;
    }
    snapshot.synchronizeReferences(&touched);
    snapshot.beginRecording(nullptr);
}

}  // namespace

void ProjectSession::derivePublication(const Document& before, const Document& after, const ChangeRecorder& touched,
                                       EditResult& result, const std::string& requestId) {
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

    // Networks that already existed: network-level change, then the touched
    // nodes and edges of that network.
    for (const NetworkId id : touched.networks()) {
        const Network* previous = findNetwork(before, id);
        const Network* current = findNetwork(after, id);
        if (previous != nullptr) {
            if (current == nullptr || previous->name() != current->name() ||
                previous->defaultOutput() != current->defaultOutput() || previous->revision() != current->revision())
                addNetworkChange(id);
            if (current == nullptr)
                continue;
            const std::vector<NodeId> nodeIds = touchedIdsIn(touched.nodes(), id);
            if (!nodeIds.empty()) {
                const auto previousNodes = touchedRecords(previous->graph().nodes(),
                                                          std::vector<std::uint64_t>(nodeIds.begin(), nodeIds.end()),
                                                          [](const NodeInstance& node) { return node.id; });
                const auto currentNodes =
                    touchedRecords(current->graph().nodes(), std::vector<std::uint64_t>(nodeIds.begin(), nodeIds.end()),
                                   [](const NodeInstance& node) { return node.id; });
                for (std::size_t position = 0; position < nodeIds.size(); ++position) {
                    const NodeInstance* prior = previousNodes[position];
                    const NodeInstance* twin = currentNodes[position];
                    if (twin == nullptr) {
                        addNodeChange(id, nodeIds[position]);
                    } else if (prior == nullptr) {
                        result.createdNodeIds.push_back(ScopedNodeId{id, nodeIds[position]});
                        addNodeChange(id, nodeIds[position]);
                    } else if (!nodeContentEquals(*prior, *twin)) {
                        addNodeChange(id, nodeIds[position]);
                    }
                }
            }
            const std::vector<EdgeId> edgeIds = touchedIdsIn(touched.edges(), id);
            if (!edgeIds.empty()) {
                const auto previousEdges = touchedRecords(previous->graph().edges(),
                                                          std::vector<std::uint64_t>(edgeIds.begin(), edgeIds.end()),
                                                          [](const Edge& edge) { return edge.id; });
                const auto currentEdges =
                    touchedRecords(current->graph().edges(), std::vector<std::uint64_t>(edgeIds.begin(), edgeIds.end()),
                                   [](const Edge& edge) { return edge.id; });
                for (std::size_t position = 0; position < edgeIds.size(); ++position) {
                    const Edge* prior = previousEdges[position];
                    const Edge* twin = currentEdges[position];
                    if (twin == nullptr) {
                        addEdgeChange(id, edgeIds[position]);
                    } else if (prior == nullptr) {
                        result.createdEdgeIds.push_back(ScopedEdgeId{id, edgeIds[position]});
                        addEdgeChange(id, edgeIds[position]);
                    } else if (!edgeContentEquals(*prior, *twin)) {
                        addEdgeChange(id, edgeIds[position]);
                    }
                }
            }
        }
    }
    // Networks created by this transition report every node and edge they hold.
    for (const NetworkId id : touched.networks()) {
        if (findNetwork(before, id) != nullptr)
            continue;
        const Network* current = findNetwork(after, id);
        if (current == nullptr)
            continue;
        result.createdNetworkIds.push_back(id);
        addNetworkChange(id);
        for (const auto& node : current->graph().nodes()) {
            result.createdNodeIds.push_back(ScopedNodeId{id, node.id});
            addNodeChange(id, node.id);
        }
        for (const auto& edge : current->graph().edges()) {
            result.createdEdgeIds.push_back(ScopedEdgeId{id, edge.id});
            addEdgeChange(id, edge.id);
        }
    }
    if (before.rootNetworkId() != after.rootNetworkId()) {
        addNetworkChange(before.rootNetworkId());
        addNetworkChange(after.rootNetworkId());
    }

    for (const NetworkInstanceId id : touched.instances()) {
        const NetworkInstance* prior = before.instance(id);
        const NetworkInstance* current = after.instance(id);
        if (prior != nullptr) {
            if (current == nullptr || *current != *prior)
                result.changedInstanceIds.push_back(id);
        } else if (current != nullptr) {
            result.createdInstanceIds.push_back(id);
            result.changedInstanceIds.push_back(id);
        }
    }

    for (const std::string& id : touched.sources()) {
        const auto prior = before.sources.find(id);
        const auto current = after.sources.find(id);
        if (prior == before.sources.end() && current == after.sources.end())
            continue;
        if (prior == before.sources.end() || current == after.sources.end() || current->second != prior->second)
            result.changedSourceIds.push_back(id);
    }

    for (const MediaSourceId id : touched.mediaEntries()) {
        const MediaCatalogEntry* prior = before.mediaCatalog().entry(id);
        const MediaCatalogEntry* current = after.mediaCatalog().entry(id);
        if (prior != nullptr) {
            if (current == nullptr || *current != *prior)
                result.changedMediaEntryIds.push_back(id);
        } else if (current != nullptr) {
            result.createdMediaEntryIds.push_back(id);
            result.changedMediaEntryIds.push_back(id);
        }
    }
    for (const MediaBinId id : touched.mediaBins()) {
        const MediaBin* prior = before.mediaCatalog().bin(id);
        const MediaBin* current = after.mediaCatalog().bin(id);
        if (prior != nullptr) {
            if (current == nullptr || *current != *prior)
                result.changedMediaBinIds.push_back(id);
        } else if (current != nullptr) {
            result.createdMediaBinIds.push_back(id);
            result.changedMediaBinIds.push_back(id);
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
    for (const AnimationChannelId id : touched.animationChannels()) {
        const AnimationChannel* prior = before.animationChannel(id);
        const AnimationChannel* current = after.animationChannel(id);
        if (prior != nullptr && (current == nullptr || *current != *prior))
            addAnimationChange(*prior);
        if (prior == nullptr && current != nullptr)
            addAnimationChange(*current);
        if (prior != nullptr) {
            for (const auto& key : prior->keys) {
                if (current == nullptr) {
                    result.changedAnimationKeyIds.push_back(KeyframeRef{prior->id, key.id});
                    continue;
                }
                const auto found = std::find_if(current->keys.begin(), current->keys.end(),
                                                [&](const Keyframe& value) { return value.id == key.id; });
                if (found == current->keys.end() || *found != key)
                    result.changedAnimationKeyIds.push_back(KeyframeRef{prior->id, key.id});
            }
        }
        if (current != nullptr) {
            for (const auto& key : current->keys) {
                if (prior != nullptr &&
                    std::find_if(prior->keys.begin(), prior->keys.end(),
                                 [&](const Keyframe& value) { return value.id == key.id; }) != prior->keys.end())
                    continue;
                result.changedAnimationKeyIds.push_back(KeyframeRef{current->id, key.id});
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

bool ProjectSession::documentContentDiverged() const {
    if (!hasSavedBaseline_)
        return true;
    return !documentContentEquals(document_, savedBaseline_);
}

EditResult ProjectSession::execute(Operation operation, Command* command, const EditOptions& options) {
    assertOwnerThread();
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
        const CommandStack::BeforeCommit prepare = [&](const Document& before, const Document& after,
                                                       const ChangeRecorder& touched) {
            derivePublication(before, after, touched, result, options.requestId);
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
    assertOwnerThread();
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
        ChangeRecorder touched;
        applyPreview(*snapshot, touched, [&] {
            if (keyedTime) {
                Command preview =
                    setKeyframesCommand(keyframeEditsForParameters(document_, nullptr, *keyedTime, edits));
                preview.apply(*snapshot);
            } else {
                Command preview = setParametersCommand(edits);
                preview.apply(*snapshot);
            }
        });
        const auto token = nextGestureToken_++;
        ParameterGestureState state;
        state.token = token;
        state.expectedRevision = options.expectedRevision;
        state.snapshot = snapshot;
        state.edits = std::move(edits);
        state.mode = keyedTime ? GestureMode::KeyedAll : GestureMode::Static;
        state.time = keyedTime.value_or(0.0);
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

ParameterGestureResult ProjectSession::beginValueParameterGesture(double time, std::vector<ParameterEdit> edits,
                                                                  EditOptions options) {
    assertOwnerThread();
    if (!std::isfinite(time))
        return gestureFailure("value parameter gesture time must be finite");
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
    try {
        if (const auto problem = duplicateAddressProblem(edits))
            return gestureFailure(*problem);
        // Freeze the routing and the effective values once, here: which addresses
        // are keyed and which counts as "changed" cannot move while the gesture
        // lives.
        std::vector<ParameterAddress> keyedAddresses;
        keyedAddresses.reserve(edits.size());
        std::vector<ParameterEdit> frozen;
        frozen.reserve(edits.size());
        std::vector<ParameterEdit> changed;
        changed.reserve(edits.size());
        for (const ParameterEdit& edit : edits) {
            // animatedParameterValue already resolves the authored static/default or animated
            // value through the catalog and animation owner, including definition
            // animation for an occurrence address and occurrence-override precedence.
            frozen.push_back(ParameterEdit{edit.address, animatedParameterValue(document_, edit.address, time)});
            if (document_.animationChannel(edit.address) != nullptr)
                keyedAddresses.push_back(edit.address);
            if (valueEditChanged(edit, frozen))
                changed.push_back(edit);
        }
        std::vector<ParameterEdit> keyed;
        std::vector<ParameterEdit> staticValues;
        partitionValueEdits(changed, keyedAddresses, keyed, staticValues);
        auto snapshot = std::make_shared<Document>(document_);
        ChangeRecorder touched;
        if (!changed.empty()) {
            applyPreview(*snapshot, touched, [&] {
                Command preview = parameterValueCommand(document_, nullptr, time, keyed, staticValues);
                preview.apply(*snapshot);
            });
        }
        const auto token = nextGestureToken_++;
        ParameterGestureState state;
        state.token = token;
        state.expectedRevision = options.expectedRevision;
        state.snapshot = snapshot;
        state.edits = std::move(edits);
        state.time = time;
        state.mode = GestureMode::Mixed;
        state.keyedAddresses = std::move(keyedAddresses);
        state.frozenValues = std::move(frozen);
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

ParameterGestureResult ProjectSession::previewFailure(const std::exception& error) const {
    return gestureFailure(error.what());
}

ParameterGestureResult ProjectSession::updateParameterGesture(ParameterGestureToken token,
                                                              std::vector<ParameterEdit> edits) {
    assertOwnerThread();
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
        merged.reserve(gesture_->edits.size() + edits.size());
        for (const auto& edit : edits) {
            if (gesture_->mode == GestureMode::KeyedAll && !edit.value)
                throw std::invalid_argument("keyed parameter edits cannot reset a parameter");
            if (gesture_->mode == GestureMode::Mixed) {
                // Routing was frozen at begin: an address that was not admitted
                // there could otherwise join with no routing (and an animated one
                // would shadow its channel with a static write).
                const bool admitted = std::find_if(gesture_->frozenValues.begin(), gesture_->frozenValues.end(),
                                                   [&](const ParameterEdit& candidate) {
                                                       return candidate.address == edit.address;
                                                   }) != gesture_->frozenValues.end();
                if (!admitted)
                    throw std::invalid_argument("parameter '" + edit.address.key +
                                                "' was not part of this value gesture");
                if (!edit.value && std::find(gesture_->keyedAddresses.begin(), gesture_->keyedAddresses.end(),
                                             edit.address) != gesture_->keyedAddresses.end())
                    throw std::invalid_argument("a parameter with animation cannot be reset by a value gesture");
            }
            const auto existing = std::find_if(merged.begin(), merged.end(), [&](const ParameterEdit& previous) {
                return previous.address == edit.address;
            });
            if (existing == merged.end())
                merged.push_back(edit);
            else
                existing->value = edit.value;
        }
        auto snapshot = std::make_shared<Document>(document_);
        ChangeRecorder touched;
        applyPreview(*snapshot, touched, [&] {
            if (gesture_->mode == GestureMode::KeyedAll) {
                Command preview = setKeyframesCommand(
                    keyframeEditsForParameters(document_, gesture_->snapshot.get(), gesture_->time, merged));
                preview.apply(*snapshot);
            } else if (gesture_->mode == GestureMode::Mixed) {
                std::vector<ParameterEdit> changed;
                for (const ParameterEdit& edit : merged)
                    if (valueEditChanged(edit, gesture_->frozenValues))
                        changed.push_back(edit);
                std::vector<ParameterEdit> keyed;
                std::vector<ParameterEdit> staticValues;
                partitionValueEdits(changed, gesture_->keyedAddresses, keyed, staticValues);
                if (!changed.empty()) {
                    Command preview =
                        parameterValueCommand(document_, gesture_->snapshot.get(), gesture_->time, keyed, staticValues);
                    preview.apply(*snapshot);
                }
            } else {
                Command preview = setParametersCommand(merged);
                preview.apply(*snapshot);
            }
        });
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
    assertOwnerThread();
    if (!gesture_ || gesture_->mode != GestureMode::KeyedAll)
        return gestureFailure("unknown keyed parameter gesture token", EditErrorCode::Unavailable);
    return updateParameterGesture(token, std::move(edits));
}

EditResult ProjectSession::commitParameterGesture(ParameterGestureToken token, EditOptions options) {
    assertOwnerThread();
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
        if (gesture_->mode == GestureMode::KeyedAll) {
            auto keyEdits =
                keyframeEditsForParameters(document_, gesture_->snapshot.get(), gesture_->time, gesture_->edits);
            command = setKeyframesCommand(std::move(keyEdits));
        } else if (gesture_->mode == GestureMode::Mixed) {
            std::vector<ParameterEdit> changed;
            for (const ParameterEdit& edit : gesture_->edits)
                if (valueEditChanged(edit, gesture_->frozenValues))
                    changed.push_back(edit);
            if (changed.empty()) {
                // Every target still holds the value frozen at begin: a no-op
                // gesture publishes no history entry and does not move the
                // revision.
                gesture_.reset();
                EditResult unchanged;
                unchanged.revision = revision_;
                return unchanged;
            }
            std::vector<ParameterEdit> keyed;
            std::vector<ParameterEdit> staticValues;
            partitionValueEdits(changed, gesture_->keyedAddresses, keyed, staticValues);
            command = parameterValueCommand(document_, gesture_->snapshot.get(), gesture_->time, keyed, staticValues);
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
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
    assertOwnerThread();
    limit = std::min<std::size_t>(limit, 256);
    std::vector<MediaQueryResult> result;
    if (limit == 0)
        return result;
    for (const auto id : document_.mediaCatalog().search(document_, filter, kind, offline, unused, scope)) {
        if (id <= after)
            continue;
        const auto* value = document_.mediaCatalog().entry(id);
        result.push_back(MediaQueryResult{id, value->sourceKey, value->parent, value->metadata});
        if (result.size() == limit)
            break;
    }
    return result;
}

std::vector<MediaBin> ProjectSession::queryMediaBins(MediaBinId parent, std::size_t limit, MediaBinId after) const {
    assertOwnerThread();
    limit = std::min<std::size_t>(limit, 256);
    std::vector<MediaBin> result;
    if (limit == 0)
        return result;
    for (const auto id : document_.mediaCatalog().childBins(parent)) {
        if (id <= after)
            continue;
        result.push_back(*document_.mediaCatalog().bin(id));
        if (result.size() == limit)
            break;
    }
    return result;
}

void ProjectSession::captureSavedBaseline() {
    savedRevision_ = revision_;
    savedBaseline_ = document_;
    savedPresentation_ = presentation_;
    savedColorConfigPath_ = colorConfigPath_;
    hasSavedBaseline_ = true;
}

bool ProjectSession::isDirty() const {
    assertOwnerThread();
    if (presentation_ != savedPresentation_ || colorConfigPath_ != savedColorConfigPath_)
        return true;
    return documentContentDiverged();
}

void ProjectSession::setPresentation(nlohmann::json presentation) {
    assertOwnerThread();
    presentation_ = std::move(presentation);
    invalidateDirtyCache();
}

void ProjectSession::setColorConfigPath(std::string colorConfigPath) {
    assertOwnerThread();
    colorConfigPath_ = std::move(colorConfigPath);
    invalidateDirtyCache();
}

void ProjectSession::setLastFileError(std::string message) {
    assertOwnerThread();
    lastFileError_ = std::move(message);
}

ProjectWriteRequest ProjectSession::prepareSave(std::filesystem::path target, PathPolicy pathPolicy,
                                                bool backup) const {
    assertOwnerThread();
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
    return request;
}

EditResult ProjectSession::commitSave(const ProjectWriteRequest& request, const ProjectWriteResult& result) {
    assertOwnerThread();
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
    if (request.snapshot) {
        // The written version is the baseline. A completion for a snapshot the
        // session has already advanced past leaves the session dirty, because
        // the current content is compared against that exact version.
        savedBaseline_ = *request.snapshot;
        savedPresentation_ = request.presentation;
        savedColorConfigPath_ = request.colorConfigPath;
        hasSavedBaseline_ = true;
    }
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
    assertOwnerThread();
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
        hasSavedBaseline_ = false;
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
    assertOwnerThread();
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
