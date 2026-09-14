#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/SharedContainers.hpp"
#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/ChangeRecorder.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/document/MediaCatalog.hpp"
namespace nemo {

// Persistent color policy names only; runtime OCIO objects belong to
// evaluation/presentation modules.
//
// Registered (built-in) color-configuration reference (issue #75).
//
// A color-configuration reference is either a filesystem path or one registered
// URI naming a configuration compiled into the build. The pinned owner-approved
// default is the OCIO-embedded ACES Studio config below; the media module owns
// what that reference resolves to and which working space/view it is authored
// against. Only registered references receive this treatment: any other
// URI-looking string is an ordinary path, so the file layer never declares an
// unsupported reference present and never bypasses path handling for one.
inline constexpr std::string_view kBuiltinColorConfigUri = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";

[[nodiscard]] bool isRegisteredColorConfigReference(std::string_view reference) noexcept;

struct ColorPolicy {
    std::string workingSpace{"linear"};
    std::string viewerTransform{"sRGB/rec709"};
    std::string deliveryTransform{"sRGB/rec709"};
    // Authored fields of the persisted color policy this build does not model,
    // retained verbatim for lossless save.
    nlohmann::json extension{};

    [[nodiscard]] bool operator==(const ColorPolicy&) const = default;
};
struct SourceReference {
    std::string path;
    std::int64_t frameOffset{0};
    std::int64_t frameStep{1};
    // Authored inclusive source-frame range for a '#'/'@' image sequence
    // (issue #61). Absent means unbounded, so existing references and stills
    // keep the previous behavior exactly; when engaged the shared image
    // adapter reports a frame outside the range instead of clamping it.
    std::optional<std::int64_t> firstFrame;
    std::optional<std::int64_t> lastFrame;
    std::map<std::string, std::string> interpretation;
    std::uint64_t revision{0};
    // Authored fields of the persisted source this build does not model,
    // retained verbatim for lossless save.
    nlohmann::json extension{};

    [[nodiscard]] bool operator==(const SourceReference&) const = default;
    [[nodiscard]] std::int64_t frameAt(std::int64_t localTime) const;
};

// Preserved authored JSON that shares storage across document versions: an
// edit copies the handle, never the payload (issue #72).
class SharedJson {
public:
    SharedJson() = default;
    SharedJson(nlohmann::json value) : value_(std::make_shared<const nlohmann::json>(std::move(value))) {}

    SharedJson& operator=(nlohmann::json value) {
        value_ = std::make_shared<const nlohmann::json>(std::move(value));
        return *this;
    }
    [[nodiscard]] const nlohmann::json& get() const noexcept {
        static const nlohmann::json empty = nlohmann::json::object();
        return value_ ? *value_ : empty;
    }
    operator const nlohmann::json&() const noexcept { return get(); }  // NOLINT(google-explicit-constructor)
    [[nodiscard]] bool empty() const noexcept { return !value_ || value_->empty(); }

private:
    std::shared_ptr<const nlohmann::json> value_;
};

// Persistent document state. Networks are definitions owned by this object;
// instances reference definitions and never copy their topology. No Qt,
// Vulkan, evaluator, decoder, or plugin-runtime object may appear here.
//
// Every collection is structurally shared storage: copying a Document copies
// handles, so history entries, gesture previews and render snapshots retain
// the same records until a controlled mutation replaces the ones it touched.
struct Document {
    static inline constexpr int kSchemaVersion = 5;
    using NetworkStorage = CowVector<Network>;
    using InstanceStorage = CowVector<NetworkInstance>;
    using AnimationStorage = CowVector<AnimationChannel>;

    Document();
    explicit Document(std::shared_ptr<const NodeCatalog> catalog);
    // Ordered sources keyed by the authored source key.
    CowMap<std::string, SourceReference> sources;
    int schemaVersion{kSchemaVersion};
    std::string name;
    ColorPolicy color;
    // Authored top-level fields of the persisted document this build does not
    // model, retained verbatim by the codec so a load/save cycle loses nothing.
    // The session/file envelope's presentation record is deliberately excluded.
    SharedJson extension{};

    // Catalog mutations are recorded against this document's active recorder.
    [[nodiscard]] const MediaCatalog& mediaCatalog() const noexcept { return mediaCatalog_; }
    [[nodiscard]] MediaCatalog& mediaCatalog() noexcept {
        if (recorder_)
            mediaCatalog_.setChangeRecorder(recorder_);
        return mediaCatalog_;
    }

    // Controlled-edit recording. The recorder is transient session state: it is
    // installed only while a private candidate is being built and never
    // survives publication. Passing nullptr releases it everywhere it was
    // installed, so no published version holds a pointer to a finished
    // transaction.
    void beginRecording(ChangeRecorder* recorder);
    [[nodiscard]] ChangeRecorder* recorder() const noexcept { return recorder_; }

    [[nodiscard]] NetworkId rootNetworkId() const { return rootNetworkId_; }
    [[nodiscard]] const Network& network(NetworkId id) const;
    [[nodiscard]] Network& network(NetworkId id);
    [[nodiscard]] const NetworkStorage& networks() const { return networks_; }
    [[nodiscard]] NetworkId addNetwork(std::string name);
    [[nodiscard]] NetworkId addNetworkWithId(NetworkId id, std::string name);
    void removeNetwork(NetworkId id);
    void setRootNetworkId(NetworkId id);

    [[nodiscard]] const InstanceStorage& instances() const { return instances_; }
    [[nodiscard]] const NetworkInstance* instance(NetworkInstanceId id) const;
    [[nodiscard]] NetworkInstanceId addInstance(NetworkId parentNetwork, NetworkId definition, std::string name);
    [[nodiscard]] NetworkInstanceId addInstanceWithId(NetworkInstanceId id, NetworkId parentNetwork,
                                                      NetworkId definition, NodeId node, std::string name,
                                                      std::map<InterfacePortId, PortRef> inputBindings = {},
                                                      std::map<NodeId, ParameterValues> params = {},
                                                      bool ownsDefinition = false);
    void removeInstance(NetworkInstanceId id);
    void setInstanceOwnership(NetworkInstanceId id, bool ownsDefinition);
    void removeOwnedNetworkIfUnreferenced(NetworkId network);
    void bindInstanceInput(NetworkInstanceId id, InterfacePortId input, PortRef source);
    void bindInstanceInputToParentTerminal(NetworkInstanceId id, InterfacePortId input, InterfacePortId parentInput);
    void eraseInstanceInputBinding(NetworkInstanceId id, InterfacePortId input);
    void setInstanceParam(NetworkInstanceId id, NodeId targetNode, std::string key, ParameterValue value);
    void eraseInstanceParam(NetworkInstanceId id, NodeId targetNode, const std::string& key);
    // Moves an existing occurrence into a different owning network while
    // retaining its document identity and linked definition.
    void reparentInstance(NetworkInstanceId id, NetworkId parentNetwork, NodeId node);
    void setInstanceDefinition(NetworkInstanceId id, NetworkId definition);
    void restoreIdentityHighWatermarks(NetworkId nextNetworkId, NetworkInstanceId nextInstanceId);
    [[nodiscard]] NetworkId nextNetworkId() const { return nextNetworkId_; }
    [[nodiscard]] NetworkInstanceId nextInstanceId() const { return nextInstanceId_; }
    // Keeps allocator watermarks monotonic when a history snapshot is
    // prepared after a delete/undo or a branch edit.
    void preserveIdentityHighWatermarksFrom(const Document& source);
    [[nodiscard]] std::uint64_t stateRevision() const;
    [[nodiscard]] const AnimationStorage& animationChannels() const { return animationChannels_; }
    [[nodiscard]] const AnimationChannel* animationChannel(AnimationChannelId id) const;
    [[nodiscard]] const AnimationChannel* animationChannel(const ParameterAddress& address) const;
    [[nodiscard]] MediaSourceId nextMediaSourceId() const { return mediaCatalog_.nextEntryId(); }
    [[nodiscard]] MediaBinId nextMediaBinId() const { return mediaCatalog_.nextBinId(); }
    void restoreMediaIdentityHighWatermarks(MediaSourceId nextSourceId, MediaBinId nextBinId);
    // Attaches preserved authored JSON to a persisted network occurrence, plus
    // raw parameter overrides for targets this build cannot type. Reserved for
    // deserialization.
    void restoreInstanceExtension(NetworkInstanceId id, nlohmann::json extension,
                                  std::map<NodeId, nlohmann::json> opaqueParams);
    [[nodiscard]] AnimationChannelId nextAnimationChannelId() const { return nextAnimationChannelId_; }
    [[nodiscard]] KeyframeId nextKeyframeId() const { return nextKeyframeId_; }
    // Serialization is the only intended caller. It validates the complete
    // restored set and never lowers identity watermarks.
    void restoreAnimationChannels(AnimationStorage channels, AnimationChannelId nextChannelId, KeyframeId nextKeyId);

    // Records one source reference write. Used by the controlled command path
    // so publication reports the source identity instead of diffing sources.
    void setSourceReference(const std::string& id, SourceReference value);
    void removeSourceReference(const std::string& id);
    // Records one animation channel as touched. The controlled editing path
    // calls this for every channel it rewrote, so publication compares only
    // those channels' keys.
    void touchAnimationChannel(AnimationChannelId id) { recordAnimationChannel(id); }
    // Removes the channels addressing `node` (directly, or through the
    // occurrence `instance`) and records each removed channel. Used by node
    // deletion, which must retire animation with the node in one transaction.
    void removeAnimationChannelsFor(NetworkId network, NodeId node, NetworkInstanceId instance);

    // Reconciles occurrence-local terminal contracts after a shared
    // definition edit and removes references to intentionally deleted nodes.
    // The optional recorder restricts the pass to the relationships this
    // transaction could have affected; without one the complete document is
    // reconciled (deserialization and replacement). Call this on the owner
    // thread before publishing a snapshot.
    void synchronizeReferences(const ChangeRecorder* touched = nullptr);

    // Installs this document's recorder into the network's graph so graph
    // mutations report the network they touched, remembering the install so
    // releasing the recorder can clear it again.
    void installRecorder(Network& network, NetworkId id);
    void remapAnimationChannels(NetworkId sourceNetwork, NetworkId destinationNetwork,
                                const std::map<NodeId, NodeId>& nodes,
                                std::optional<NetworkInstanceId> onlyInstance = std::nullopt);
    void copyAnimationChannels(NetworkId sourceNetwork, NetworkId destinationNetwork,
                               const std::map<NodeId, NodeId>& nodes);

private:
    friend class CommandStack;
    struct NetworkWatermarks {
        NodeId nextNodeId{1};
        EdgeId nextEdgeId{1};
        InterfacePortId nextInterfacePortId{1};
    };
    [[nodiscard]] Network* findNetwork(NetworkId id);
    [[nodiscard]] const Network* findNetwork(NetworkId id) const;
    [[nodiscard]] std::size_t networkIndexOf(NetworkId id) const;
    [[nodiscard]] NetworkInstance* findInstanceMutable(NetworkInstanceId id);
    [[nodiscard]] std::size_t instanceIndexOf(NetworkInstanceId id) const;
    [[nodiscard]] bool networkDependsOn(NetworkId candidate, NetworkId target) const;
    [[nodiscard]] bool instanceBindingDependsOn(NetworkInstanceId origin, NetworkInstanceId target) const;
    void recordNetwork(NetworkId id);
    void recordInstance(NetworkInstanceId id);
    void recordNode(NetworkId network, NodeId id);
    void recordAnimationChannel(AnimationChannelId id);

    std::shared_ptr<const NodeCatalog> catalog_;
    MediaCatalog mediaCatalog_;
    NetworkStorage networks_;
    InstanceStorage instances_;
    AnimationStorage animationChannels_;
    AnimationChannelId nextAnimationChannelId_{1};
    KeyframeId nextKeyframeId_{1};
    CowMap<NetworkId, NetworkWatermarks> retiredNetworkWatermarks_;
    ChangeRecorder* recorder_{};
    // Networks that currently carry an installed recorder, so releasing the
    // recorder clears exactly those and not the shared storage of the rest.
    std::vector<NetworkId> recorderInstalls_;
    NetworkId rootNetworkId_{kInvalidNetwork};
    NetworkId nextNetworkId_{1};
    NetworkInstanceId nextInstanceId_{1};
    std::uint64_t freshnessRevision_{1};
};

// Content equality that matches what the project codec persists: two versions
// are equal exactly when they would serialize identically. The comparison
// treats storage that still shares chunks as identical, so it costs what the
// edit changed rather than the project size, and it never serializes.
[[nodiscard]] bool nodeContentEquals(const NodeInstance& left, const NodeInstance& right);
[[nodiscard]] bool edgeContentEquals(const Edge& left, const Edge& right);
[[nodiscard]] bool networkContentEquals(const Network& left, const Network& right);
[[nodiscard]] bool documentContentEquals(const Document& left, const Document& right);

struct Command {
    std::string label;
    std::function<void(Document&)> apply;
};

class CommandStack {
public:
    explicit CommandStack(Document& document, std::size_t capacity = 256);

    // Called with the version before and after a transition plus the
    // identities that transition touched, so publication never diffs the
    // project.
    using BeforeCommit = std::function<void(const Document&, const Document&, const ChangeRecorder&)>;
    void push(Command command, const BeforeCommit& beforeCommit = {});
    [[nodiscard]] bool canUndo() const { return undo_.size() != 0; }
    [[nodiscard]] bool canRedo() const { return redo_.size() != 0; }
    bool undo(const BeforeCommit& beforeCommit = {});
    bool redo(const BeforeCommit& beforeCommit = {});

    [[nodiscard]] std::size_t depth() const { return undo_.size(); }
    void clear();

private:
    // One retained version handle plus the identities the transition between
    // the adjacent versions touched. The version is a set of shared storage
    // handles, not a second document payload.
    struct Entry {
        Document document;
        ChangeRecorder touched;
    };
    // Bounded ring storage: reaching capacity overwrites the oldest entry
    // instead of moving every remaining one. Slots are occupied only while an
    // entry is retained, so a popped or cleared entry releases the version
    // storage it was keeping alive.
    class Ring {
    public:
        explicit Ring(std::size_t capacity);
        [[nodiscard]] std::size_t size() const noexcept { return count_; }
        [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
        [[nodiscard]] Entry& back() { return *slots_[slotOf(count_ - 1)]; }
        [[nodiscard]] const Entry& back() const { return *slots_[slotOf(count_ - 1)]; }
        void push(Entry entry);
        void popBack();
        void clear();

    private:
        [[nodiscard]] std::size_t slotOf(std::size_t offset) const { return (head_ + offset) % slots_.size(); }
        std::vector<std::optional<Entry>> slots_;
        std::size_t head_{0};
        std::size_t count_{0};
    };

    void prepare(Document& candidate, ChangeRecorder* recorder) const;
    Document& document_;
    Ring undo_;
    Ring redo_;
};

// All graph command factories require an explicit network scope. This keeps
// accidental single-graph edits impossible and makes command history records
// unambiguous when node/edge identities are local to a network.

struct ParameterEdit {
    ParameterAddress address;
    std::optional<ParameterValue> value;
};

struct LayoutEdit {
    NodeId node{kInvalidNode};
    LayoutPosition position{};
};

Command addNodeCommand(NetworkId network, std::string type, std::string name, std::shared_ptr<NodeId> createdId = {},
                       LayoutPosition position = {}, NodeId anchor = kInvalidNode,
                       std::vector<LayoutEdit> shiftedNodes = {});
Command removeNodeCommand(NetworkId network, NodeId nodeId);
Command connectCommand(NetworkId network, PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId = {});
Command disconnectCommand(NetworkId network, EdgeId edgeId);
Command replaceInputCommand(NetworkId network, PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId = {});
// Swaps the sources feeding two declared input ports of one node (issue #75,
// the Merge "Swap A/B" action) as ONE atomic command. Both ports occupied
// exchanges their sources; exactly one occupied moves that source to the
// other port; neither occupied, identical ports, an undeclared port, or two
// ports fed by the same output are rejected with a GraphException before any
// connection changes, so a meaningless or invalid request writes no history
// entry. The node, its parameters (operation/mask/mix), its layout, and every
// other edge - including the optional mask port - are retained; the two
// affected edges are re-created, so their authored routes are dropped. The
// complete swap is validated on a trial graph and published whole, and the
// command is a single undo/redo step.
Command swapInputsCommand(NetworkId network, NodeId nodeId, std::uint32_t firstPort, std::uint32_t secondPort);
Command rewireGraphEdgeCommand(NetworkId network, EdgeId edgeId, PortRef from, PortRef to);
Command insertNodeOnEdgeCommand(NetworkId network, EdgeId edgeId, std::string type, std::string name,
                                LayoutPosition position = {}, std::shared_ptr<NodeId> createdNode = {},
                                std::shared_ptr<EdgeId> upstreamEdge = {}, std::shared_ptr<EdgeId> downstreamEdge = {});
Command insertExistingNodeOnEdgeCommand(NetworkId network, EdgeId edgeId, NodeId nodeId, LayoutPosition position);
Command setParamCommand(NetworkId network, NodeId nodeId, std::string key, ParameterValue value);
Command resetParamCommand(NetworkId network, NodeId nodeId, std::string key);
Command setParametersCommand(std::vector<ParameterEdit> edits);
Command renameNodeCommand(NetworkId network, NodeId nodeId, std::string name);
Command setLayoutCommand(NetworkId network, NodeId nodeId, LayoutPosition position);
Command setLayoutsCommand(NetworkId network, std::vector<LayoutEdit> edits);
Command setRouteCommand(NetworkId network, EdgeId edgeId, std::vector<LayoutPosition> route);
Command insertRoutePointCommand(NetworkId network, EdgeId edgeId, std::size_t index, LayoutPosition position);
Command moveRoutePointCommand(NetworkId network, EdgeId edgeId, std::size_t index, LayoutPosition position);
Command removeRoutePointCommand(NetworkId network, EdgeId edgeId, std::size_t index);
Command setDefaultOutputCommand(NetworkId network, NodeId output);
// Attaches `sourceNode`'s image output to viewer(viewerIndex)'s input, creating
// any missing viewer nodes first. An invalid source detaches the viewer when
// already fed by it; re-assigning the currently attached source toggles the
// edge off. The whole edit is validated on a trial graph and committed as one
// undo step. New viewer ids are reported in creation order through
// `createdViewer` when provided.
Command assignViewerCommand(NetworkId network, std::size_t viewerIndex, NodeId sourceNode,
                            std::shared_ptr<NodeId> createdViewer = {});
Command connectInputCommand(NetworkId network, InterfacePortId input, PortRef destination);
Command connectOutputCommand(NetworkId network, PortRef source, InterfacePortId output);

Command addNetworkCommand(std::string name, std::shared_ptr<NetworkId> createdId = {});
Command removeNetworkCommand(NetworkId network);
Command addInstanceCommand(NetworkId parentNetwork, NetworkId definition, std::string name,
                           std::shared_ptr<NetworkInstanceId> createdId = {});
Command removeInstanceCommand(NetworkInstanceId instance);
Command bindInstanceInputCommand(NetworkInstanceId instance, InterfacePortId input, PortRef source);
Command setInstanceParamCommand(NetworkInstanceId instance, NodeId targetNode, std::string key, ParameterValue value);
Command resetInstanceParamCommand(NetworkInstanceId instance, NodeId targetNode, std::string key);

Command transactionCommand(std::string label, std::vector<Command> commands);
Command setColorPolicyCommand(ColorPolicy value);
Command setSourceCommand(std::string id, SourceReference value);
Command removeSourceCommand(std::string id);
}  // namespace nemo
