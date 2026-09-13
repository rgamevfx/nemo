#pragma once

#include <set>
#include <string>
#include <utility>

#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Ids.hpp"

namespace nemo {

// Identities touched by one controlled transaction on the private candidate.
//
// The recorder is a superset of the identities whose authored state differs
// between the version before and after the transaction: a mutation records the
// records it wrote, not a precomputed answer. Publication then compares the
// stored values of the touched identities between the two versions, so a
// recorded-but-unchanged write (a parameter set to its current value, a
// position applied twice) still reports nothing.
//
// Recording is the whole reason publication no longer diffs the document:
// ordinary edits report cost proportional to what the command touched, and
// undo/redo reuse the same set for their reverse transition.
class ChangeRecorder {
public:
    void node(NetworkId networkId, NodeId id) {
        nodes_.emplace(networkId, id);
        network(networkId);
    }
    void edge(NetworkId networkId, EdgeId id) {
        edges_.emplace(networkId, id);
        network(networkId);
    }
    void network(NetworkId id) {
        if (id != kInvalidNetwork)
            networks_.insert(id);
    }
    void instance(NetworkInstanceId id) {
        if (id != kInvalidNetworkInstance)
            instances_.insert(id);
    }
    void source(std::string id) {
        if (!id.empty())
            sources_.insert(std::move(id));
    }
    void mediaEntry(MediaSourceId id) {
        if (id != kInvalidMediaSource)
            mediaEntries_.insert(id);
    }
    void mediaBin(MediaBinId id) {
        if (id != kInvalidMediaBin)
            mediaBins_.insert(id);
    }
    void animationChannel(AnimationChannelId id) {
        if (id != kInvalidAnimationChannel)
            animationChannels_.insert(id);
    }
    void colorPolicy() { colorPolicyChanged_ = true; }

    void clear() {
        nodes_.clear();
        edges_.clear();
        networks_.clear();
        instances_.clear();
        sources_.clear();
        mediaEntries_.clear();
        mediaBins_.clear();
        animationChannels_.clear();
        colorPolicyChanged_ = false;
    }

    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty() && edges_.empty() && networks_.empty() && instances_.empty() && sources_.empty() &&
               mediaEntries_.empty() && mediaBins_.empty() && animationChannels_.empty() && !colorPolicyChanged_;
    }

    [[nodiscard]] const std::set<std::pair<NetworkId, NodeId>>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::set<std::pair<NetworkId, EdgeId>>& edges() const noexcept { return edges_; }
    [[nodiscard]] const std::set<NetworkId>& networks() const noexcept { return networks_; }
    [[nodiscard]] const std::set<NetworkInstanceId>& instances() const noexcept { return instances_; }
    [[nodiscard]] const std::set<std::string>& sources() const noexcept { return sources_; }
    [[nodiscard]] const std::set<MediaSourceId>& mediaEntries() const noexcept { return mediaEntries_; }
    [[nodiscard]] const std::set<MediaBinId>& mediaBins() const noexcept { return mediaBins_; }
    [[nodiscard]] const std::set<AnimationChannelId>& animationChannels() const noexcept { return animationChannels_; }
    [[nodiscard]] bool colorPolicyChanged() const noexcept { return colorPolicyChanged_; }

private:
    std::set<std::pair<NetworkId, NodeId>> nodes_;
    std::set<std::pair<NetworkId, EdgeId>> edges_;
    std::set<NetworkId> networks_;
    std::set<NetworkInstanceId> instances_;
    std::set<std::string> sources_;
    std::set<MediaSourceId> mediaEntries_;
    std::set<MediaBinId> mediaBins_;
    std::set<AnimationChannelId> animationChannels_;
    bool colorPolicyChanged_{false};
};

}  // namespace nemo
