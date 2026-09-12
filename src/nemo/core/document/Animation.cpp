#include "nemo/core/document/Animation.hpp"

#include "nemo/core/document/Document.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace nemo {
namespace {

[[noreturn]] void invalid(const std::string& message) {
    throw GraphException(GraphError::ParameterValue, message);
}

const NodeInstance& addressNode(const Document& document, const ParameterAddress& address) {
    if (address.network == kInvalidNetwork || address.node == kInvalidNode || address.key.empty())
        invalid("animation address requires network, node, and parameter key");
    if (address.instance == kInvalidNetworkInstance) {
        const auto& network = document.network(address.network);
        const auto* node = network.graph().node(address.node);
        if (!node)
            invalid("animation address references unknown node " + std::to_string(address.node));
        return *node;
    }
    const auto* instance = document.instance(address.instance);
    if (!instance)
        invalid("animation address references unknown instance " + std::to_string(address.instance));
    if (instance->definition != address.network)
        invalid("animation address network does not match instance definition");
    const auto* node = document.network(address.network).graph().node(address.node);
    if (!node)
        invalid("animation address references unknown definition node " + std::to_string(address.node));
    return *node;
}

const ParameterSpec& addressSpec(const Document& document, const ParameterAddress& address, const NodeInstance& node) {
    const auto* spec = document.network(address.network).graph().catalog().parameterSpec(node.type, address.key);
    if (!spec)
        invalid("animation address references unknown parameter '" + address.key + "'");
    return *spec;
}

ParameterValue fromComponents(ParameterType type, const std::array<double, 4>& value) {
    switch (type) {
    case ParameterType::Float:
        return value[0];
    case ParameterType::Vector2:
        return Vector2Value{{static_cast<float>(value[0]), static_cast<float>(value[1])}};
    case ParameterType::Vector3:
        return Vector3Value{{static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])}};
    case ParameterType::Color:
        return ColorValue{{static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2]),
                           static_cast<float>(value[3])}};
    default:
        return value[0];
    }
}

void validateChannel(const Document& document, const AnimationChannel& channel) {
    if (channel.id == kInvalidAnimationChannel || channel.keys.empty())
        invalid("channel must have a nonzero identity and at least one key");
    const auto& node = addressNode(document, channel.address);
    const auto& catalog = document.network(channel.address.network).graph().catalog();
    // A node type this build does not model has no catalog contract. Its channel
    // is preserved as authored disabled data: structure, identities and times
    // are still validated, but spec-dependent checks are skipped. A known node
    // type with an unknown parameter remains an error.
    const ParameterSpec* spec = catalog.parameterSpec(node.type, channel.address.key);
    const bool unavailable = catalog.find(node.type) == nullptr;
    // A future parameter record this build cannot type is preserved even when
    // the node type is known but the parameter is not; typed keys referencing an
    // unknown parameter on a known type remain an error.
    const bool hasOpaqueValue = std::any_of(channel.keys.begin(), channel.keys.end(),
                                            [](const Keyframe& key) { return !key.opaqueValue.is_null(); });
    if (spec == nullptr && !unavailable && !hasOpaqueValue)
        invalid("animation address references unknown parameter '" + channel.address.key + "'");
    double previous = -std::numeric_limits<double>::infinity();
    for (const auto& key : channel.keys) {
        const auto fail = [&](const std::string& message) {
            invalid("key " + std::to_string(key.id) + " at time " + std::to_string(key.time) + ": " + message);
        };
        if (key.id == kInvalidKeyframe)
            fail("identity must be nonzero");
        if (key.time <= previous)
            fail("another key occupies this time");
        previous = key.time;
        if (key.interpolation != KeyInterpolation::Hold && key.interpolation != KeyInterpolation::Linear &&
            key.interpolation != KeyInterpolation::Bezier)
            fail("unknown interpolation mode");
        if (key.tangentMode != TangentMode::Smooth && key.tangentMode != TangentMode::Broken)
            fail("unknown tangent mode");
        for (std::size_t i = 0; i < key.inSlope.size(); ++i) {
            if (!std::isfinite(key.inSlope[i]) || !std::isfinite(key.outSlope[i]))
                fail("tangent slopes must be finite");
        }
        if (!key.opaqueValue.is_null()) {
            // Preserved authored value this build cannot type; the codec already
            // validated its tagged shape. No catalog or interpolation semantics
            // can be applied to it.
            continue;
        }
        if (spec != nullptr) {
            if (const auto problem = catalog.validateParameter(node.type, channel.address.key, key.value))
                fail(*problem);
            const auto count = animation_detail::componentCount(spec->type);
            if (count == 0 && key.interpolation != KeyInterpolation::Hold)
                fail("discrete parameters require Hold interpolation");
            for (std::size_t i = 0; i < key.inSlope.size(); ++i) {
                if (i >= count && (key.inSlope[i] != 0.0 || key.outSlope[i] != 0.0))
                    fail("unused tangent components must be zero");
                if (key.tangentMode == TangentMode::Smooth && key.inSlope[i] != key.outSlope[i])
                    fail("Smooth tangents require equal incoming and outgoing slopes");
            }
            continue;
        }
        // Unavailable node type with a standard typed value: the representation
        // is still checked so malformed data cannot silently survive.
        if (const auto problem = validateParameterValueRepresentation(key.value))
            fail(*problem);
    }
}

ParameterValue staticValue(const Document& document, const ParameterAddress& address) {
    const auto& node = addressNode(document, address);
    const auto& spec = addressSpec(document, address, node);
    ParameterValue result = spec.defaultValue;
    if (const auto it = node.params.find(address.key); it != node.params.end())
        result = it->second;
    if (address.instance != kInvalidNetworkInstance) {
        const auto* instance = document.instance(address.instance);
        if (!instance)
            invalid("animation address references unknown instance " + std::to_string(address.instance));
        const auto nodeIt = instance->params.find(address.node);
        if (nodeIt != instance->params.end()) {
            if (const auto valueIt = nodeIt->second.find(address.key); valueIt != nodeIt->second.end())
                result = valueIt->second;
        }
    }
    return result;
}

ParameterValue evaluateChannel(const AnimationChannel& channel, ParameterType type, double time) {
    const auto& keys = channel.keys;
    if (keys.empty())
        throw std::logic_error("animation channel has no keys");
    if (time <= keys.front().time)
        return keys.front().value;
    if (time >= keys.back().time)
        return keys.back().value;
    const auto right = std::upper_bound(keys.begin(), keys.end(), time,
                                        [](double value, const Keyframe& key) { return value < key.time; });
    const auto& b = *right;
    const auto& a = *std::prev(right);
    if (time == a.time)
        return a.value;
    if (a.interpolation == KeyInterpolation::Hold)
        return a.value;
    const long double dt = static_cast<long double>(b.time) - a.time;
    const long double u = (static_cast<long double>(time) - a.time) / dt;
    const auto av = animation_detail::valueComponents(a.value);
    const auto bv = animation_detail::valueComponents(b.value);
    std::array<double, 4> result{};
    const std::size_t count = animation_detail::componentCount(type);
    const auto storeComponent = [&](std::size_t index, long double value) {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            invalid("animation channel " + std::to_string(channel.id) + " parameter '" + channel.address.key +
                    "' between keys " + std::to_string(a.id) + " and " + std::to_string(b.id) +
                    ": interpolation exceeds the finite parameter range");
        result[index] = static_cast<double>(value);
    };
    if (a.interpolation == KeyInterpolation::Linear) {
        for (std::size_t i = 0; i < count; ++i)
            storeComponent(i, std::lerp(static_cast<long double>(av[i]), static_cast<long double>(bv[i]), u));
    } else {
        const auto u2 = u * u;
        const auto u3 = u2 * u;
        const auto h00 = 2 * u3 - 3 * u2 + 1;
        const auto h10 = u3 - 2 * u2 + u;
        const auto h01 = -2 * u3 + 3 * u2;
        const auto h11 = u3 - u2;
        for (std::size_t i = 0; i < count; ++i)
            storeComponent(i, h00 * av[i] + h10 * dt * a.outSlope[i] + h01 * bv[i] + h11 * dt * b.inSlope[i]);
    }
    return fromComponents(type, result);
}

}  // namespace

ParameterValue animatedParameterValue(const Document& document, const ParameterAddress& address, double time) {
    if (!std::isfinite(time))
        invalid("animation query time must be finite");
    const auto& node = addressNode(document, address);
    const auto& spec = addressSpec(document, address, node);
    const auto evaluate = [&](const AnimationChannel& channel) {
        if (std::any_of(channel.keys.begin(), channel.keys.end(),
                        [](const Keyframe& key) { return !key.opaqueValue.is_null(); }))
            invalid("animation channel " + std::to_string(channel.id) + " parameter '" + address.key +
                    "' has values this build cannot interpret");
        return evaluateChannel(channel, spec.type, time);
    };
    if (const auto* channel = document.animationChannel(address))
        return evaluate(*channel);
    if (address.instance != kInvalidNetworkInstance) {
        const auto* occurrence = document.instance(address.instance);
        const auto overrideNode = occurrence->params.find(address.node);
        const bool explicitlyOverridden =
            overrideNode != occurrence->params.end() && overrideNode->second.count(address.key) != 0;
        if (!explicitlyOverridden) {
            const ParameterAddress definitionAddress{address.network, address.node, address.key,
                                                     kInvalidNetworkInstance};
            if (const auto* definitionChannel = document.animationChannel(definitionAddress))
                return evaluate(*definitionChannel);
        }
    }
    return staticValue(document, address);
}

void applyAnimationParameters(const Document& document, NetworkId network, NodeId node, NetworkInstanceId instance,
                              double time, ParameterValues& parameters) {
    if (!std::isfinite(time))
        invalid("animation query time must be finite");
    if (instance != kInvalidNetworkInstance && !document.instance(instance))
        invalid("animation evaluation references unknown instance " + std::to_string(instance));
    const auto occurrence = instance == kInvalidNetworkInstance ? nullptr : document.instance(instance);
    const auto applyPass = [&](bool occurrenceChannels) {
        for (const auto& channel : document.animationChannels()) {
            if (channel.address.network != network || channel.address.node != node ||
                (channel.address.instance != kInvalidNetworkInstance) != occurrenceChannels)
                continue;
            if (occurrenceChannels) {
                if (channel.address.instance != instance)
                    continue;
            } else if (occurrence) {
                const auto nodeIt = occurrence->params.find(node);
                if (nodeIt != occurrence->params.end() && nodeIt->second.count(channel.address.key) != 0)
                    continue;
                const auto& channels = document.animationChannels();
                if (std::any_of(channels.begin(), channels.end(), [&](const AnimationChannel& overrideChannel) {
                        const auto& address = overrideChannel.address;
                        return address.instance == instance && address.network == network && address.node == node &&
                               address.key == channel.address.key;
                    }))
                    continue;
            }
            // Only a channel that is actually active for this evaluation is
            // checked. Authored values this build cannot interpret must make the
            // affected evaluation unavailable; silently leaving the static
            // parameter would render a wrong image as success.
            if (std::any_of(channel.keys.begin(), channel.keys.end(),
                            [](const Keyframe& key) { return !key.opaqueValue.is_null(); }))
                invalid("animation channel " + std::to_string(channel.id) + " address network " +
                        std::to_string(channel.address.network) + " node " + std::to_string(channel.address.node) +
                        " instance " + std::to_string(channel.address.instance) + " parameter '" + channel.address.key +
                        "' has values this build cannot interpret; unsupported authored animation cannot be evaluated");
            const auto* nodeValue = document.network(network).graph().node(node);
            if (!nodeValue)
                continue;
            const auto* spec =
                document.network(network).graph().catalog().parameterSpec(nodeValue->type, channel.address.key);
            if (!spec)
                continue;
            parameters[channel.address.key] = evaluateChannel(channel, spec->type, time);
        }
    };
    applyPass(false);
    applyPass(true);
}

const AnimationChannel* Document::animationChannel(AnimationChannelId id) const {
    const auto it = std::find_if(animationChannels_.begin(), animationChannels_.end(),
                                 [id](const AnimationChannel& channel) { return channel.id == id; });
    return it == animationChannels_.end() ? nullptr : &*it;
}

const AnimationChannel* Document::animationChannel(const ParameterAddress& address) const {
    const auto it = std::find_if(animationChannels_.begin(), animationChannels_.end(),
                                 [&address](const AnimationChannel& channel) { return channel.address == address; });
    return it == animationChannels_.end() ? nullptr : &*it;
}

void Document::restoreAnimationChannels(std::vector<AnimationChannel> channels, AnimationChannelId nextChannelId,
                                        KeyframeId nextKeyId) {
    if (nextChannelId == kInvalidAnimationChannel || nextKeyId == kInvalidKeyframe)
        throw GraphException(GraphError::InvalidId, "animation identity watermarks must be nonzero");
    std::set<AnimationChannelId> channelIds;
    std::set<KeyframeId> keyIds;
    for (auto& channel : channels) {
        for (const auto& key : channel.keys)
            if (!std::isfinite(key.time))
                throw GraphException(GraphError::ParameterValue, "animation channel " + std::to_string(channel.id) +
                                                                     " contains non-finite key time");
        std::sort(channel.keys.begin(), channel.keys.end(),
                  [](const Keyframe& left, const Keyframe& right) { return left.time < right.time; });
        try {
            validateChannel(*this, channel);
        } catch (const GraphException& error) {
            throw GraphException(error.errorCode(), "animation channel " + std::to_string(channel.id) + " network " +
                                                        std::to_string(channel.address.network) + " node " +
                                                        std::to_string(channel.address.node) + " instance " +
                                                        std::to_string(channel.address.instance) + " parameter '" +
                                                        channel.address.key + "': " + error.what());
        }
        if (!channelIds.insert(channel.id).second)
            throw GraphException(GraphError::DuplicateId,
                                 "duplicate animation channel id " + std::to_string(channel.id));
        for (const auto& key : channel.keys) {
            if (!keyIds.insert(key.id).second)
                throw GraphException(GraphError::DuplicateId, "duplicate keyframe id " + std::to_string(key.id));
            if (key.id == std::numeric_limits<KeyframeId>::max())
                throw GraphException(GraphError::InvalidId, "keyframe identity space is exhausted");
            if (key.id >= nextKeyId)
                nextKeyId = key.id + 1;
        }
        if (channel.id >= nextChannelId) {
            if (channel.id == std::numeric_limits<AnimationChannelId>::max())
                throw GraphException(GraphError::InvalidId, "animation channel identity space is exhausted");
            nextChannelId = channel.id + 1;
        }
    }
    for (std::size_t i = 0; i < channels.size(); ++i)
        for (std::size_t j = i + 1; j < channels.size(); ++j)
            if (channels[i].address == channels[j].address)
                throw GraphException(GraphError::DuplicateId, "duplicate animation channel address for network " +
                                                                  std::to_string(channels[i].address.network) +
                                                                  " node " + std::to_string(channels[i].address.node) +
                                                                  " parameter '" + channels[i].address.key + "'");
    std::sort(channels.begin(), channels.end(),
              [](const AnimationChannel& left, const AnimationChannel& right) { return left.id < right.id; });
    animationChannels_ = std::move(channels);
    nextAnimationChannelId_ = std::max(nextAnimationChannelId_, nextChannelId);
    nextKeyframeId_ = std::max(nextKeyframeId_, nextKeyId);
}

}  // namespace nemo
