#include "nemo/core/commands/AnimationCommands.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace nemo {
namespace {

[[noreturn]] void reject(const std::string& message, GraphError code = GraphError::ParameterValue) {
    throw GraphException(code, "animation: " + message);
}

std::uint64_t allocateId(std::uint64_t& next, const char* kind) {
    if (next == 0 || next == std::numeric_limits<std::uint64_t>::max())
        reject(std::string(kind) + " identity space is exhausted", GraphError::InvalidId);
    return next++;
}

AnimationChannel* channelAt(std::vector<AnimationChannel>& channels, const ParameterAddress& address) {
    const auto found = std::find_if(channels.begin(), channels.end(),
                                    [&](const AnimationChannel& channel) { return channel.address == address; });
    return found == channels.end() ? nullptr : &*found;
}

Keyframe& keyAt(std::vector<AnimationChannel>& channels, KeyframeRef ref) {
    const auto channel = std::find_if(channels.begin(), channels.end(),
                                      [&](const AnimationChannel& value) { return value.id == ref.channel; });
    if (channel == channels.end())
        reject("unknown channel " + std::to_string(ref.channel), GraphError::UnknownNode);
    const auto key = std::find_if(channel->keys.begin(), channel->keys.end(),
                                  [&](const Keyframe& value) { return value.id == ref.key; });
    if (key == channel->keys.end())
        reject("channel " + std::to_string(ref.channel) + " has no key " + std::to_string(ref.key),
               GraphError::UnknownNode);
    return *key;
}

std::set<KeyframeId> validateTargets(std::vector<AnimationChannel>& channels, const std::vector<KeyframeRef>& refs) {
    std::set<KeyframeId> seen;
    for (const auto& ref : refs) {
        if (!seen.insert(ref.key).second)
            reject("duplicate key target " + std::to_string(ref.key));
        (void)keyAt(channels, ref);
    }
    return seen;
}

}  // namespace

Command setKeyframesCommand(std::vector<KeyframeEdit> edits) {
    if (edits.empty())
        throw std::invalid_argument("animation key batch must contain at least one edit");
    return {"set animation keyframes", [edits = std::move(edits)](Document& document) {
                auto channels = document.animationChannels();
                auto nextChannel = document.nextAnimationChannelId();
                auto nextKey = document.nextKeyframeId();
                std::set<KeyframeId> targets;
                for (const auto& edit : edits) {
                    if (!std::isfinite(edit.key.time))
                        reject("parameter '" + edit.address.key + "' key time must be finite");
                    // Resolve every target against the original state, not earlier batch
                    // edits. This permits time swaps and rejects id/time aliases atomically.
                    const auto* originalChannel = document.animationChannel(edit.address);
                    const Keyframe* original = nullptr;
                    if (originalChannel) {
                        const auto found = std::find_if(
                            originalChannel->keys.begin(), originalChannel->keys.end(), [&](const Keyframe& key) {
                                return edit.key.id ? key.id == edit.key.id : key.time == edit.key.time;
                            });
                        if (found != originalChannel->keys.end())
                            original = &*found;
                    }
                    if (edit.key.id && !original)
                        reject("parameter '" + edit.address.key + "' has no key " + std::to_string(edit.key.id),
                               GraphError::UnknownNode);
                    Keyframe replacement = edit.key;
                    replacement.id = original ? original->id : allocateId(nextKey, "keyframe");
                    if (!targets.insert(replacement.id).second)
                        reject("duplicate key target " + std::to_string(replacement.id));
                    auto* channel = channelAt(channels, edit.address);
                    if (!channel) {
                        channels.push_back({allocateId(nextChannel, "channel"), edit.address, {}});
                        channel = &channels.back();
                    }
                    if (original)
                        keyAt(channels, {channel->id, original->id}) = std::move(replacement);
                    else
                        channel->keys.push_back(std::move(replacement));
                }
                // The document owns all channel/type/tangent/final-time validation.
                document.restoreAnimationChannels(std::move(channels), nextChannel, nextKey);
            }};
}

Command removeKeyframesCommand(std::vector<KeyframeRef> refs) {
    if (refs.empty())
        throw std::invalid_argument("animation key removal must contain at least one key");
    return {"remove animation keyframes", [refs = std::move(refs)](Document& document) {
                auto channels = document.animationChannels();
                const auto removed = validateTargets(channels, refs);
                for (auto& channel : channels)
                    std::erase_if(channel.keys, [&](const Keyframe& key) { return removed.contains(key.id); });
                std::erase_if(channels, [](const AnimationChannel& channel) { return channel.keys.empty(); });
                document.restoreAnimationChannels(std::move(channels), document.nextAnimationChannelId(),
                                                  document.nextKeyframeId());
            }};
}

Command moveKeyframesCommand(std::vector<KeyframeRef> refs, double deltaTime) {
    if (refs.empty() || !std::isfinite(deltaTime))
        throw std::invalid_argument("animation key move requires keys and a finite time delta");
    return {"move animation keyframes", [refs = std::move(refs), deltaTime](Document& document) {
                auto channels = document.animationChannels();
                validateTargets(channels, refs);
                for (const auto& ref : refs)
                    keyAt(channels, ref).time += deltaTime;
                document.restoreAnimationChannels(std::move(channels), document.nextAnimationChannelId(),
                                                  document.nextKeyframeId());
            }};
}

Command insertKeyframeCommand(ParameterAddress address, double time) {
    if (!std::isfinite(time))
        throw std::invalid_argument("animation insertion time must be finite");
    return {"insert animation keyframe", [address = std::move(address), time](Document& document) {
                Keyframe key;
                key.time = time;
                // This query validates the scoped address and uses instance precedence.
                key.value = animatedParameterValue(document, address, time);
                const auto& graph = document.network(address.network).graph();
                const auto* spec = graph.catalog().parameterSpec(graph.node(address.node)->type, address.key);
                const auto count = animation_detail::componentCount(spec->type);
                key.interpolation = count ? KeyInterpolation::Linear : KeyInterpolation::Hold;
                auto nextChannel = document.nextAnimationChannelId();
                auto nextKey = document.nextKeyframeId();
                key.id = allocateId(nextKey, "keyframe");
                auto channels = document.animationChannels();
                auto* channel = channelAt(channels, address);
                if (!channel) {
                    channels.push_back({allocateId(nextChannel, "channel"), address, {}});
                    channel = &channels.back();
                } else {
                    const auto right =
                        std::lower_bound(channel->keys.begin(), channel->keys.end(), time,
                                         [](const Keyframe& value, double frame) { return value.time < frame; });
                    if (right != channel->keys.end() && right->time == time)
                        reject("channel " + std::to_string(channel->id) + " already has a key at time " +
                               std::to_string(time));
                    if (right == channel->keys.begin()) {
                        key.interpolation = KeyInterpolation::Hold;
                    } else if (right == channel->keys.end()) {
                        // The old endpoint's outgoing segment did not exist before;
                        // its new segment must retain the old constant extrapolation.
                        channel->keys.back().interpolation = KeyInterpolation::Hold;
                        key.interpolation = KeyInterpolation::Hold;
                    } else {
                        const auto& left = *std::prev(right);
                        key.interpolation = left.interpolation;
                        const long double dt = static_cast<long double>(right->time) - left.time;
                        const long double u = (static_cast<long double>(time) - left.time) / dt;
                        const auto a = animation_detail::valueComponents(left.value);
                        const auto b = animation_detail::valueComponents(right->value);
                        for (std::size_t i = 0; i < count; ++i) {
                            long double slope = 0;
                            if (left.interpolation == KeyInterpolation::Linear)
                                slope = (static_cast<long double>(b[i]) - a[i]) / dt;
                            else if (left.interpolation == KeyInterpolation::Bezier) {
                                const auto u2 = u * u;
                                slope = ((6 * u2 - 6 * u) * a[i] + (-6 * u2 + 6 * u) * b[i]) / dt +
                                        (3 * u2 - 4 * u + 1) * left.outSlope[i] + (3 * u2 - 2 * u) * right->inSlope[i];
                            }
                            key.inSlope[i] = key.outSlope[i] = static_cast<double>(slope);
                        }
                    }
                }
                channel->keys.push_back(std::move(key));
                document.restoreAnimationChannels(std::move(channels), nextChannel, nextKey);
            }};
}

}  // namespace nemo
