#pragma once

#include "nemo/core/document/ParameterValue.hpp"
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

// Directional compatibility between a source output kind and a destination
// input kind. An Image output may feed a Mask input (a mask is a
// single-channel interpretation of an image), but a Mask output never
// satisfies an Image input. Every other pairing, including Media, must match
// exactly. This is a schema fact, not an executor decision.
[[nodiscard]] constexpr bool portKindsCompatible(PortKind from, PortKind to) {
    return from == to || (from == PortKind::Image && to == PortKind::Mask);
}

struct PortSpec {
    PortKind kind;
    std::string name;
    // Optional inputs may be left unconnected. An absent optional slot is
    // represented by the evaluation sentinel, never by a manufactured source.
    // Outputs and required inputs keep their existing contract.
    bool optional{false};

    friend bool operator==(const PortSpec&, const PortSpec&) = default;
};

enum class ParameterType { Boolean, Integer, Float, Choice, Vector2, Vector3, Color, String };

struct ParameterSpec {
    std::string name;
    ParameterType type{ParameterType::String};
    ParameterValue defaultValue{std::string{}};
    std::optional<double> minimum{};
    std::optional<double> maximum{};
    std::vector<std::string> choices{};
    // Presentation metadata. `step` is an incremental step for numeric
    // controls; `label`/`section` are display text (empty means the presenter
    // derives them); `editor` names a namespaced custom-editor id (empty
    // means the generic control).
    std::optional<double> step{};
    std::string label;
    std::string section;
    std::string editor;
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
                                                               const ParameterValue& value) const;
    [[nodiscard]] const ParameterValue* parameterDefault(std::string_view type, std::string_view key) const;
    [[nodiscard]] const ParameterSpec* parameterSpec(std::string_view type, std::string_view key) const;
    [[nodiscard]] ParameterValue parseParameterText(std::string_view type, std::string_view key,
                                                    std::string_view value) const;
    [[nodiscard]] std::optional<std::uint64_t> implementationVersion(std::string_view type) const;

private:
    std::deque<NodeDescriptor> descriptors_;
};
[[nodiscard]] const NodeCatalog& builtinNodeCatalog();
[[nodiscard]] std::shared_ptr<const NodeCatalog> builtinNodeCatalogPtr();

}  // namespace nemo
