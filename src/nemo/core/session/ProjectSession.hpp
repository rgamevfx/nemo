#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nemo/core/document/Document.hpp"

namespace nemo {

struct EditOptions {
    std::uint64_t expectedRevision{0};
    std::string requestId{};
};

enum class EditErrorCode { InvalidArgument, MissingObject, RevisionConflict, Unavailable, ReentrantMutation };

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
    bool colorPolicyChanged{false};
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
    bool colorPolicyChanged{false};
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
    std::string value;
};

struct EdgeQueryResult {
    NetworkId network{kInvalidNetwork};
    Edge edge;
};

struct SourceQueryResult {
    std::string id;
    SourceReference reference;
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

    [[nodiscard]] const Document& document() const noexcept { return document_; }
    [[nodiscard]] Document snapshot() const { return document_; }

    [[nodiscard]] Subscription subscribe(void* context, ObserverCallback callback);

    // Nonempty request IDs (at most 256 bytes) are session-local operation
    // identities shared by submit/undo/redo. The last 256 successful identified
    // operations replay their original result, even after later edits/undo.
    // Failures are not retained; reuse of an ID means retry, not a new payload.
    // A restarted session has no dedup history. Revision is always required.
    [[nodiscard]] EditResult submit(Command command, EditOptions options);
    [[nodiscard]] EditResult undo(EditOptions options);
    [[nodiscard]] EditResult redo(EditOptions options);

    [[nodiscard]] bool canUndo() const noexcept { return commands_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return commands_.canRedo(); }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

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

private:
    struct Observer {
        void* context;
        ObserverCallback callback;
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
};

}  // namespace nemo
