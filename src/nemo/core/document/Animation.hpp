#pragma once

#include "nemo/core/document/Ids.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <vector>

namespace nemo {

struct ParameterAddress {
    NetworkId network{kInvalidNetwork};
    NodeId node{kInvalidNode};
    std::string key;
    NetworkInstanceId instance{kInvalidNetworkInstance};

    friend bool operator==(const ParameterAddress&, const ParameterAddress&) = default;
};

using AnimationChannelId = std::uint64_t;
using KeyframeId = std::uint64_t;
inline constexpr AnimationChannelId kInvalidAnimationChannel = 0;
inline constexpr KeyframeId kInvalidKeyframe = 0;

enum class KeyInterpolation { Hold, Linear, Bezier };
enum class TangentMode { Smooth, Broken };

struct Keyframe {
    KeyframeId id{};
    double time{};
    ParameterValue value{0.0};
    KeyInterpolation interpolation{KeyInterpolation::Linear};
    TangentMode tangentMode{TangentMode::Smooth};
    std::array<double, 4> inSlope{};
    std::array<double, 4> outSlope{};
    // A key value record this build cannot type (an unavailable node type or a
    // future tag), preserved verbatim. When set it supersedes `value` on save
    // and is never evaluated because its owning node type is unavailable.
    nlohmann::json opaqueValue{};
    // Authored fields of the persisted key this build does not model, retained
    // verbatim for lossless save.
    nlohmann::json extension{};

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

struct AnimationChannel {
    AnimationChannelId id{};
    ParameterAddress address;
    std::vector<Keyframe> keys;
    // Authored fields of the persisted channel this build does not model,
    // retained verbatim for lossless save.
    nlohmann::json extension{};

    friend bool operator==(const AnimationChannel&, const AnimationChannel&) = default;
};

struct KeyframeRef {
    AnimationChannelId channel{};
    KeyframeId key{};

    friend bool operator==(const KeyframeRef&, const KeyframeRef&) = default;
};
struct Document;

namespace animation_detail {
[[nodiscard]] inline std::size_t componentCount(ParameterType type) {
    switch (type) {
    case ParameterType::Float:
        return 1;
    case ParameterType::Vector2:
        return 2;
    case ParameterType::Vector3:
        return 3;
    case ParameterType::Color:
        return 4;
    default:
        return 0;
    }
}

[[nodiscard]] inline std::array<double, 4> valueComponents(const ParameterValue& value) {
    return std::visit(
        [](const auto& item) {
            std::array<double, 4> result{};
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, double>)
                result[0] = item;
            else if constexpr (std::is_same_v<T, Vector2Value>) {
                result[0] = item.value[0];
                result[1] = item.value[1];
            } else if constexpr (std::is_same_v<T, Vector3Value>) {
                for (std::size_t i = 0; i < 3; ++i)
                    result[i] = item.value[i];
            } else if constexpr (std::is_same_v<T, ColorValue>) {
                for (std::size_t i = 0; i < 4; ++i)
                    result[i] = item.value[i];
            }
            return result;
        },
        value);
}
}  // namespace animation_detail

// Returns the authored static/default value resolved through the catalog and
// animation channel at a document-local frame. This query never mutates the
// document and uses constant endpoint extrapolation.
[[nodiscard]] ParameterValue animatedParameterValue(const Document&, const ParameterAddress&, double time);

// Applies definition and occurrence animation over an already-composed static
// parameter map. Explicit occurrence overrides suppress their definition
// animation, while occurrence animation remains authoritative.
void applyAnimationParameters(const Document&, NetworkId network, NodeId node, NetworkInstanceId instance, double time,
                              ParameterValues& parameters);

}  // namespace nemo
