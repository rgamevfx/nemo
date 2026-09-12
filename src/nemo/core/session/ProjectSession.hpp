#pragma once

#include <cassert>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/session/ProjectFile.hpp"

namespace nemo {

struct EditOptions {
    std::uint64_t expectedRevision{0};
    std::string requestId{};
};

enum class EditErrorCode { InvalidArgument, MissingObject, RevisionConflict, Unavailable, ReentrantMutation, IoError };

struct EditError {
    std::string message;
    EditErrorCode code{EditErrorCode::InvalidArgument};
    std::optional<GraphError> graphError;
};

struct ScopedNodeId {
    NetworkId network{kInvalidNetwork};
    NodeId id{kInvalidNode};

    friend bool operator==(const ScopedNodeId&, const ScopedNodeId&) = default;
};

struct ScopedEdgeId {
    NetworkId network{kInvalidNetwork};
    EdgeId id{kInvalidEdge};

    friend bool operator==(const ScopedEdgeId&, const ScopedEdgeId&) = default;
};

struct AnimationChannelQueryResult {
    AnimationChannelId id{kInvalidAnimationChannel};
    ParameterAddress address;
    std::size_t keyCount{};
};

struct AnimationKeyQueryResult {
    AnimationChannelId channel{kInvalidAnimationChannel};
    Keyframe key;
};

struct EditResult {
    bool committed{false};
    std::uint64_t revision{1};
    std::optional<EditError> error;
    std::vector<ScopedNodeId> changedNodeIds;
    std::vector<ScopedNodeId> createdNodeIds;
    std::vector<ScopedEdgeId> changedEdgeIds;
    std::vector<ScopedEdgeId> createdEdgeIds;
    std::vector<NetworkId> changedNetworkIds;
    std::vector<NetworkId> createdNetworkIds;
    std::vector<NetworkInstanceId> changedInstanceIds;
    std::vector<NetworkInstanceId> createdInstanceIds;
    std::vector<std::string> changedSourceIds;
    std::vector<MediaSourceId> changedMediaEntryIds;
    std::vector<MediaSourceId> createdMediaEntryIds;
    std::vector<MediaBinId> changedMediaBinIds;
    std::vector<MediaBinId> createdMediaBinIds;
    std::vector<AnimationChannelId> changedAnimationChannelIds;
    std::vector<KeyframeRef> changedAnimationKeyIds;
    bool colorPolicyChanged{false};
};

struct ProjectReplaceResult {
    bool replaced{false};
    std::uint64_t revision{1};
    std::optional<EditError> error;
};

struct ChangeEvent {
    std::uint64_t revision{1};
    std::vector<ScopedNodeId> changedNodeIds;
    std::vector<ScopedNodeId> createdNodeIds;
    std::vector<ScopedEdgeId> changedEdgeIds;
    std::vector<ScopedEdgeId> createdEdgeIds;
    std::vector<NetworkId> changedNetworkIds;
    std::vector<NetworkId> createdNetworkIds;
    std::vector<NetworkInstanceId> changedInstanceIds;
    std::vector<NetworkInstanceId> createdInstanceIds;
    std::vector<std::string> changedSourceIds;
    std::vector<AnimationChannelId> changedAnimationChannelIds;
    std::vector<KeyframeRef> changedAnimationKeyIds;
    bool colorPolicyChanged{false};
    std::vector<MediaSourceId> changedMediaEntryIds;
    std::vector<MediaSourceId> createdMediaEntryIds;
    std::vector<MediaBinId> changedMediaBinIds;
    std::vector<MediaBinId> createdMediaBinIds;
};

struct ChangeHistory {
    std::uint64_t currentRevision{1};
    std::uint64_t oldestRevision{1};
    bool resyncRequired{false};
    std::vector<ChangeEvent> events;
};

struct NodeQueryResult {
    NetworkId network{kInvalidNetwork};
    NodeId id{kInvalidNode};
    std::string type;
    std::string name;
};

struct ValueQueryResult {
    NetworkId network{kInvalidNetwork};
    NodeId node{kInvalidNode};
    std::string key;
    ParameterValue value;
};

using ParameterGestureToken = std::uint64_t;

struct ParameterGestureResult {
    EditResult result;
    ParameterGestureToken token{};
    std::uint64_t expectedRevision{};
    // A successful preview owns an immutable snapshot. Copy the Document if
    // a caller needs a mutable worker-side view.
    std::shared_ptr<const Document> snapshot;
};

struct EdgeQueryResult {
    NetworkId network{kInvalidNetwork};
    Edge edge;
};

struct SourceQueryResult {
    std::string id;
    SourceReference reference;
};

struct MediaQueryResult {
    MediaSourceId id{kInvalidMediaSource};
    std::string sourceKey;
    MediaBinId parent{kInvalidMediaBin};
    MediaMetadata metadata;
};

// All methods and subscriptions are owner-thread-only; the session must
// outlive its subscriptions. Workers receive snapshot() copies, never this
// object. Callbacks run synchronously after publication, must not block, and
// may unsubscribe but not mutate. changesSince() supports polling clients.
class ProjectSession final {
public:
    using ObserverCallback = void (*)(void*) noexcept;

    class Subscription final {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

    private:
        friend class ProjectSession;
        Subscription(ProjectSession* session, std::uint64_t id) noexcept : session_(session), id_(id) {}
        void reset() noexcept;
        ProjectSession* session_{};
        std::uint64_t id_{};
    };

    explicit ProjectSession(Document document = {}, std::size_t historyCapacity = 256);
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) = delete;
    ProjectSession& operator=(ProjectSession&&) = delete;
    ~ProjectSession();

    [[nodiscard]] const Document& document() const noexcept {
        assertOwnerThread();
        return document_;
    }
    [[nodiscard]] Document snapshot() const {
        assertOwnerThread();
        return document_;
    }

    [[nodiscard]] Subscription subscribe(void* context, ObserverCallback callback);

    // Nonempty request IDs (at most 256 bytes) are session-local operation
    // identities shared by submit/undo/redo. The last 256 successful identified
    // operations replay their original result, even after later edits/undo.
    // Failures are not retained; reuse of an ID means retry, not a new payload.
    // A restarted session has no dedup history. Revision is always required.
    [[nodiscard]] EditResult submit(Command command, EditOptions options);
    [[nodiscard]] EditResult undo(EditOptions options);
    [[nodiscard]] EditResult redo(EditOptions options);

    // Parameter gestures validate and apply edits to an owner-thread-only
    // transient snapshot. The published document, revision, event journal,
    // request deduplication and undo history are untouched until commit.
    [[nodiscard]] ParameterGestureResult beginParameterGesture(std::vector<ParameterEdit> edits, EditOptions options);
    // Keyed gestures share the parameter gesture token, transient snapshot,
    // commit/cancel lifecycle, and one history entry.
    [[nodiscard]] ParameterGestureResult beginKeyedParameterGesture(double time, std::vector<ParameterEdit> edits,
                                                                    EditOptions options);
    [[nodiscard]] ParameterGestureResult updateKeyedParameterGesture(ParameterGestureToken token,
                                                                     std::vector<ParameterEdit> edits);
    [[nodiscard]] std::vector<AnimationChannelQueryResult>
    queryAnimationChannels(std::size_t limit = 256, AnimationChannelId after = kInvalidAnimationChannel) const;
    [[nodiscard]] std::vector<AnimationKeyQueryResult>
    queryAnimationKeys(AnimationChannelId channel, std::size_t limit = 256, KeyframeId after = kInvalidKeyframe) const;
    [[nodiscard]] ParameterGestureResult updateParameterGesture(ParameterGestureToken token,
                                                                std::vector<ParameterEdit> edits);
    [[nodiscard]] EditResult commitParameterGesture(ParameterGestureToken token, EditOptions options);
    [[nodiscard]] EditResult cancelParameterGesture(ParameterGestureToken token);

    [[nodiscard]] bool canUndo() const noexcept {
        assertOwnerThread();
        return commands_.canUndo();
    }
    [[nodiscard]] bool canRedo() const noexcept {
        assertOwnerThread();
        return commands_.canRedo();
    }
    [[nodiscard]] std::uint64_t revision() const noexcept {
        assertOwnerThread();
        return revision_;
    }

    // File ownership. Opening or saving a project never creates an undo entry
    // and never mutates the document outside replaceDocument(). All accessors
    // are owner-thread-only.
    //
    // Dirty is authored-content equality: the current document, presentation
    // envelope and color configuration serialize identically to the baseline
    // captured when the project was last opened or written. Undoing back to the
    // saved state therefore returns clean, and a save that completes for an
    // older snapshot never clears newer edits. The result is cached per state
    // stamp; isDirty() is not noexcept because serializing the baseline can
    // throw.
    [[nodiscard]] bool isDirty() const;
    [[nodiscard]] std::uint64_t savedRevision() const noexcept {
        assertOwnerThread();
        return savedRevision_;
    }
    // Identity of the currently published project. Replacement/opening advances
    // it so a save prepared against an earlier project cannot publish into the
    // newly opened one.
    [[nodiscard]] std::uint64_t projectGeneration() const noexcept {
        assertOwnerThread();
        return projectGeneration_;
    }
    [[nodiscard]] const std::filesystem::path& projectPath() const noexcept {
        assertOwnerThread();
        return projectPath_;
    }
    [[nodiscard]] const nlohmann::json& presentation() const noexcept {
        assertOwnerThread();
        return presentation_;
    }
    // Presentation is versioned independently and never interpreted by headless
    // callers. Changing it is not an undoable document edit.
    void setPresentation(nlohmann::json presentation);
    [[nodiscard]] const std::string& colorConfigPath() const noexcept {
        assertOwnerThread();
        return colorConfigPath_;
    }
    void setColorConfigPath(std::string colorConfigPath);
    [[nodiscard]] const std::string& lastFileError() const noexcept {
        assertOwnerThread();
        return lastFileError_;
    }
    void setLastFileError(std::string message);
    [[nodiscard]] bool recovered() const noexcept {
        assertOwnerThread();
        return recovered_;
    }
    // Original project file an autosave recovery copy came from; empty unless
    // recovered().
    [[nodiscard]] const std::filesystem::path& recoveryOriginal() const noexcept {
        assertOwnerThread();
        return recoveryOriginal_;
    }

    // Captures an owned immutable snapshot plus the session file state for a
    // worker write. Pure owner-thread work; no I/O and no document mutation.
    [[nodiscard]] ProjectWriteRequest prepareSave(std::filesystem::path target,
                                                  PathPolicy pathPolicy = PathPolicy::RebaseRelative,
                                                  bool backup = true) const;
    // Publishes the outcome of a prepared write on the owner thread. The
    // written snapshot becomes the saved baseline; if the session advanced past
    // it the session stays dirty. Never creates an undo entry.
    [[nodiscard]] EditResult commitSave(const ProjectWriteRequest& request, const ProjectWriteResult& result);

    // Replaces the published document in place, keeping observer
    // registrations. Session-local history, gestures and request dedup are
    // cleared, the revision advances and changesSince() reports a resync.
    [[nodiscard]] ProjectReplaceResult replaceDocument(Document document, std::filesystem::path path = {},
                                                       nlohmann::json presentation = nlohmann::json{},
                                                       std::string colorConfigPath = {});
    // Replacement from a read result. The recovery guard is preserved whenever
    // either flag or result.recovered is set, so a recovered copy can never
    // silently replace its original target.
    [[nodiscard]] ProjectReplaceResult replaceDocument(ProjectReadResult result, bool recovered = false);
    // Opens a read result and adopts its recovery state.
    [[nodiscard]] ProjectReplaceResult open(ProjectReadResult result);

    [[nodiscard]] ChangeHistory changesSince(std::uint64_t revision) const;
    // Filters match label/type or parameter/source key substrings. Limits are
    // clamped to 256; zero is empty. Cursor IDs/keys are exclusive. Every
    // graph query requires an explicit network because node/edge IDs are
    // local to that network.
    [[nodiscard]] std::vector<NodeQueryResult> queryNodes(NetworkId network, std::string_view filter = {},
                                                          std::size_t limit = 256, NodeId after = kInvalidNode) const;
    [[nodiscard]] std::vector<ValueQueryResult> queryValues(NetworkId network, NodeId node,
                                                            std::string_view keyFilter = {}, std::size_t limit = 256,
                                                            std::string_view after = {}) const;
    [[nodiscard]] std::vector<EdgeQueryResult> queryEdges(NetworkId network, NodeId touching = kInvalidNode,
                                                          std::size_t limit = 256, EdgeId after = kInvalidEdge) const;
    [[nodiscard]] std::vector<SourceQueryResult> querySources(std::string_view filter = {}, std::size_t limit = 256,
                                                              std::string_view after = {}) const;
    [[nodiscard]] std::vector<MediaQueryResult>
    queryMedia(std::string_view filter = {}, std::optional<MediaKind> kind = {}, std::optional<bool> offline = {},
               std::optional<bool> unused = {}, MediaBinId scope = kInvalidMediaBin, std::size_t limit = 256,
               MediaSourceId after = kInvalidMediaSource) const;
    [[nodiscard]] std::vector<MediaBin> queryMediaBins(MediaBinId parent = kInvalidMediaBin, std::size_t limit = 256,
                                                       MediaBinId after = kInvalidMediaBin) const;

private:
    // Development-only owner-thread diagnostic, reused by every session entry
    // point that touches session-owned state. The constructing thread is
    // captured once and never reassigned, so replacing or opening a document
    // does not transfer session ownership. With NDEBUG the check compiles to
    // nothing, so release builds keep the documented single-owner contract and
    // add no synchronization.
    void assertOwnerThread() const noexcept {
#ifndef NDEBUG
        assert(ownerThread_ == std::this_thread::get_id() && "ProjectSession state accessed from a non-owner thread");
#endif
    }

    struct Observer {
        void* context;
        ObserverCallback callback;
    };

    [[nodiscard]] ParameterGestureResult gestureFailure(std::string message,
                                                        EditErrorCode code = EditErrorCode::InvalidArgument) const;
    [[nodiscard]] ParameterGestureResult gestureFailure(const EditResult& result) const;
    [[nodiscard]] ParameterGestureResult makeGesturePreview(ParameterGestureToken token, std::uint64_t expectedRevision,
                                                            std::shared_ptr<const Document> snapshot) const;
    [[nodiscard]] ParameterGestureResult previewFailure(const std::exception& error) const;
    [[nodiscard]] ParameterGestureResult previewFailure(const GraphException& error) const;
    [[nodiscard]] ParameterGestureResult beginParameterGestureInternal(std::vector<ParameterEdit> edits,
                                                                       EditOptions options,
                                                                       std::optional<double> keyedTime);
    struct ParameterGestureState {
        ParameterGestureToken token{};
        std::uint64_t expectedRevision{};
        std::shared_ptr<const Document> snapshot;
        std::vector<ParameterEdit> edits;
        bool keyed{false};
        double time{};
    };
    void unsubscribe(std::uint64_t id) noexcept;
    void notifyObservers() noexcept;
    enum class Operation { Submit, Undo, Redo };
    [[nodiscard]] EditResult execute(Operation operation, Command* command, const EditOptions& options);
    [[nodiscard]] EditResult failure(std::string message, EditErrorCode code = EditErrorCode::InvalidArgument) const;
    [[nodiscard]] EditResult conflict(std::uint64_t expected) const;
    [[nodiscard]] std::optional<EditResult> duplicate(const std::string& requestId) const;
    void preparePublication(const Document& before, const Document& after, EditResult& result,
                            const std::string& requestId);
    [[nodiscard]] ProjectReplaceResult replaceInternal(Document document, std::filesystem::path path,
                                                       nlohmann::json presentation, std::string colorConfigPath,
                                                       bool recovered, std::filesystem::path recoveryOriginal);
    void captureSavedBaseline() noexcept;
    void invalidateDirtyCache() noexcept { ++stateStamp_; }

    Document document_;
    CommandStack commands_;
    std::map<std::uint64_t, Observer> observers_;
    std::uint64_t nextObserverId_{1};
    bool notifying_{false};
    bool mutating_{false};
    std::uint64_t revision_{1};
    std::size_t eventCapacity_;
    std::deque<ChangeEvent> events_;
    struct RequestRecord {
        std::string id;
        EditResult result;
    };
    std::deque<RequestRecord> requests_;
    std::optional<ParameterGestureState> gesture_;
    ParameterGestureToken nextGestureToken_{1};

    std::filesystem::path projectPath_;
    std::filesystem::path recoveryOriginal_;
    nlohmann::json presentation_;
    std::string colorConfigPath_;
    std::string lastFileError_;
    std::string savedContent_;
    bool savedContentValid_{false};
    bool recovered_{false};
    std::uint64_t savedRevision_{1};
    std::uint64_t projectGeneration_{1};
    std::uint64_t stateStamp_{1};
    mutable std::uint64_t dirtyCacheStamp_{0};
    mutable bool dirtyCacheValue_{false};

    // Always present so the class layout is identical with and without NDEBUG.
    // Debug captures the constructing thread; release keeps the default
    // identity and performs no thread lookup.
#ifndef NDEBUG
    const std::thread::id ownerThread_{std::this_thread::get_id()};
#else
    const std::thread::id ownerThread_{};
#endif
};

}  // namespace nemo
