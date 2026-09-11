#pragma once

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

#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/document/MediaCatalog.hpp"
namespace nemo {

// Persistent color policy names only; runtime OCIO objects belong to
// evaluation/presentation modules.
struct ColorPolicy {
    std::string workingSpace{"linear"};
    std::string viewerTransform{"sRGB/rec709"};
    std::string deliveryTransform{"sRGB/rec709"};

    [[nodiscard]] bool operator==(const ColorPolicy&) const = default;
};
struct SourceReference {
    std::string path;
    std::int64_t frameOffset{0};
    std::int64_t frameStep{1};
    std::map<std::string, std::string> interpretation;
    std::uint64_t revision{0};

    [[nodiscard]] bool operator==(const SourceReference&) const = default;
    [[nodiscard]] std::int64_t frameAt(std::int64_t localTime) const;
};

// Persistent document state. Networks are definitions owned by this object;
// instances reference definitions and never copy their topology. No Qt,
// Vulkan, evaluator, decoder, or plugin-runtime object may appear here.
struct Document {
    static inline constexpr int kSchemaVersion = 4;
    Document();
    explicit Document(std::shared_ptr<const NodeCatalog> catalog);
    std::map<std::string, SourceReference> sources;
    // Catalog entries retain only source keys and schema data; runtime media
    // and probe/decode objects are owned by media/evaluation modules.
    MediaCatalog mediaCatalog;
    int schemaVersion{kSchemaVersion};
    std::string name;
    ColorPolicy color;

    [[nodiscard]] NetworkId rootNetworkId() const { return rootNetworkId_; }
    [[nodiscard]] const Network& network(NetworkId id) const;
    [[nodiscard]] Network& network(NetworkId id);
    [[nodiscard]] const std::vector<Network>& networks() const;
    [[nodiscard]] NetworkId addNetwork(std::string name);
    [[nodiscard]] NetworkId addNetworkWithId(NetworkId id, std::string name);
    void removeNetwork(NetworkId id);
    void setRootNetworkId(NetworkId id);

    [[nodiscard]] const std::vector<NetworkInstance>& instances() const;
    [[nodiscard]] const NetworkInstance* instance(NetworkInstanceId id) const;
    [[nodiscard]] NetworkInstanceId addInstance(NetworkId parentNetwork, NetworkId definition, std::string name);
    [[nodiscard]] NetworkInstanceId addInstanceWithId(NetworkInstanceId id, NetworkId parentNetwork,
                                                      NetworkId definition, NodeId node, std::string name,
                                                      std::map<InterfacePortId, PortRef> inputBindings = {},
                                                      std::map<NodeId, ParameterValues> params = {});
    void removeInstance(NetworkInstanceId id);
    void bindInstanceInput(NetworkInstanceId id, InterfacePortId input, PortRef source);
    void eraseInstanceInputBinding(NetworkInstanceId id, InterfacePortId input);
    void setInstanceParam(NetworkInstanceId id, NodeId targetNode, std::string key, ParameterValue value);
    void eraseInstanceParam(NetworkInstanceId id, NodeId targetNode, const std::string& key);
    void restoreIdentityHighWatermarks(NetworkId nextNetworkId, NetworkInstanceId nextInstanceId);
    [[nodiscard]] NetworkId nextNetworkId() const { return nextNetworkId_; }
    [[nodiscard]] NetworkInstanceId nextInstanceId() const { return nextInstanceId_; }
    // Keeps allocator watermarks monotonic when a history snapshot is
    // prepared after a delete/undo or a branch edit.
    void preserveIdentityHighWatermarksFrom(const Document& source);
    [[nodiscard]] std::uint64_t stateRevision() const;
    [[nodiscard]] const std::vector<AnimationChannel>& animationChannels() const { return animationChannels_; }
    [[nodiscard]] const AnimationChannel* animationChannel(AnimationChannelId id) const;
    [[nodiscard]] const AnimationChannel* animationChannel(const ParameterAddress& address) const;
    [[nodiscard]] MediaSourceId nextMediaSourceId() const { return mediaCatalog.nextEntryId(); }
    [[nodiscard]] MediaBinId nextMediaBinId() const { return mediaCatalog.nextBinId(); }
    void restoreMediaIdentityHighWatermarks(MediaSourceId nextSourceId, MediaBinId nextBinId);
    [[nodiscard]] AnimationChannelId nextAnimationChannelId() const { return nextAnimationChannelId_; }
    [[nodiscard]] KeyframeId nextKeyframeId() const { return nextKeyframeId_; }
    // Serialization is the only intended caller. It validates the complete
    // restored set and never lowers identity watermarks.
    void restoreAnimationChannels(std::vector<AnimationChannel> channels, AnimationChannelId nextChannelId,
                                  KeyframeId nextKeyId);

    // Reconciles occurrence-local terminal contracts after a shared
    // definition edit and removes references to intentionally deleted nodes.
    // Call this on the owner thread before publishing a snapshot.
    void synchronizeReferences();

private:
    friend class CommandStack;
    struct NetworkWatermarks {
        NodeId nextNodeId{1};
        EdgeId nextEdgeId{1};
        InterfacePortId nextInterfacePortId{1};
    };
    [[nodiscard]] Network* findNetwork(NetworkId id);
    [[nodiscard]] const Network* findNetwork(NetworkId id) const;
    [[nodiscard]] NetworkInstance* findInstanceMutable(NetworkInstanceId id);
    [[nodiscard]] bool networkDependsOn(NetworkId candidate, NetworkId target) const;
    [[nodiscard]] bool instanceBindingDependsOn(NetworkInstanceId origin, NetworkInstanceId target) const;
    std::shared_ptr<const NodeCatalog> catalog_;
    std::vector<Network> networks_;
    std::vector<NetworkInstance> instances_;
    std::vector<AnimationChannel> animationChannels_;
    AnimationChannelId nextAnimationChannelId_{1};
    KeyframeId nextKeyframeId_{1};
    std::map<NetworkId, NetworkWatermarks> retiredNetworkWatermarks_;
    NetworkId rootNetworkId_{kInvalidNetwork};
    NetworkId nextNetworkId_{1};
    NetworkInstanceId nextInstanceId_{1};
    std::uint64_t freshnessRevision_{1};
};

struct Command {
    std::string label;
    std::function<void(Document&)> apply;
};

class CommandStack {
public:
    explicit CommandStack(Document& document, std::size_t capacity = 256);

    using BeforeCommit = std::function<void(const Document&, const Document&)>;
    void push(Command command, const BeforeCommit& beforeCommit = {});
    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const { return !redo_.empty(); }
    bool undo(const BeforeCommit& beforeCommit = {});
    bool redo(const BeforeCommit& beforeCommit = {});

    [[nodiscard]] std::size_t depth() const { return undo_.size(); }
    void clear();

private:
    void prepare(Document& candidate) const;
    Document& document_;
    std::size_t capacity_;
    std::vector<Document> undo_;
    std::vector<Document> redo_;
};

// All graph command factories require an explicit network scope. This keeps
// accidental single-graph edits impossible and makes command history records
// unambiguous when node/edge identities are local to a network.

struct ParameterEdit {
    ParameterAddress address;
    std::optional<ParameterValue> value;
};

Command addNodeCommand(NetworkId network, std::string type, std::string name, std::shared_ptr<NodeId> createdId = {});
Command connectCommand(NetworkId network, PortRef from, PortRef to, std::shared_ptr<EdgeId> createdId = {});
Command setParamCommand(NetworkId network, NodeId nodeId, std::string key, ParameterValue value);
Command resetParamCommand(NetworkId network, NodeId nodeId, std::string key);
Command setParametersCommand(std::vector<ParameterEdit> edits);
Command renameNodeCommand(NetworkId network, NodeId nodeId, std::string name);
Command setLayoutCommand(NetworkId network, NodeId nodeId, LayoutPosition position);
Command setRouteCommand(NetworkId network, EdgeId edgeId, std::vector<LayoutPosition> route);
Command setDefaultOutputCommand(NetworkId network, NodeId output);
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
