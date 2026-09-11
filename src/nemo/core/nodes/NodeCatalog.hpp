#pragma once

#include "nemo/core/evaluation/Request.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nemo {

enum class PortKind { Image, Mask, Media };

struct PortSpec {
    PortKind kind;
    std::string name;

    friend bool operator==(const PortSpec&, const PortSpec&) = default;
};

enum class ParameterType { Boolean, Integer, Float, Color, String };

struct ParameterSpec {
    std::string name;
    ParameterType type{ParameterType::String};
    std::string defaultValue;
    std::optional<double> minimum{};
    std::optional<double> maximum{};
    std::vector<std::string> choices{};
};

// Capabilities are schema facts only. They do not contain executor, Qt,
// Vulkan, plugin, or image objects. Executor registration remains owned by
// the relevant evaluation module.
struct NodeCapabilities {
    std::vector<int> samplingScales;
    std::vector<Quality> qualityModes;
    std::vector<std::string> channels;
    bool supportsRegion{true};
    bool temporal{false};
};

// A descriptor is immutable once accepted by NodeCatalog. `type` is the
// persistent, namespaced node identity; displayName and grouping are
// presentation metadata and never participate in identity.
struct NodeDescriptor {
    std::string type;
    std::string displayName;
    std::string group;
    bool isOutput{false};
    std::uint64_t implementationVersion{1};
    std::vector<PortSpec> inputs;
    std::vector<PortSpec> outputs;
    std::vector<ParameterSpec> parameters;
    NodeCapabilities capabilities;
};
class NodeCatalog {
public:
    // The default catalog contains only the immutable built-in descriptors.
    NodeCatalog();
    // A snapshot is assembled once from built-ins plus explicitly supplied
    // extension/fixture descriptors. There is no mutation or unregister API.
    // Invalid descriptors throw std::invalid_argument before the snapshot is
    // observable.
    explicit NodeCatalog(std::vector<NodeDescriptor> extensions);
    [[nodiscard]] const std::deque<NodeDescriptor>& descriptors() const { return descriptors_; }
    [[nodiscard]] const NodeDescriptor* find(std::string_view type) const;
    [[nodiscard]] std::span<const int> samplingScalesSupported(std::string_view type) const;
    [[nodiscard]] const std::vector<PortSpec>& inputPorts(std::string_view type) const;
    [[nodiscard]] const std::vector<PortSpec>& outputPorts(std::string_view type) const;
    [[nodiscard]] std::optional<std::string> validateParameter(std::string_view type, std::string_view key,
                                                               std::string_view value) const;
    [[nodiscard]] std::optional<std::string_view> parameterDefault(std::string_view type, std::string_view key) const;
    [[nodiscard]] std::optional<std::uint64_t> implementationVersion(std::string_view type) const;

private:
    std::deque<NodeDescriptor> descriptors_;
};
[[nodiscard]] const NodeCatalog& builtinNodeCatalog();
[[nodiscard]] std::shared_ptr<const NodeCatalog> builtinNodeCatalogPtr();

}  // namespace nemo
