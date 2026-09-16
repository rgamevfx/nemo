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

// Where a parameter's authored value comes from when a node is CREATED
// (issue #92). It is a generic schema fact, not a node-type branch: the creation
// owner seeds every parameter that declares a rule from the owning network's
// authored state, so a new Crop's box is the network's saved canvas instead of
// whatever the application happened to be showing. `Default` is the schema
// default and the behavior of every parameter that declares no rule, so an
// ordinary node's creation is unchanged.
enum class ParameterInitialValue {
    Default,
    // The owning network's saved canvas width (`ImageFormat::width`).
    OwningNetworkWidth,
    // The owning network's saved canvas height (`ImageFormat::height`).
    OwningNetworkHeight,
};

// How a linked multichannel editor composes one shared adjustment onto a typed
// tuple. Additive applies one common delta; Multiplicative applies one common
// factor. It is presentation semantics consumed by the registered tuple editor,
// never a second stored representation of the parameter value.
enum class ChannelLink { None, Additive, Multiplicative };

struct ChannelHint {
    ChannelLink linked{ChannelLink::None};
    // Alpha is always presented separately from the linked RGB adjustment.
    bool alphaSeparate{false};

    friend bool operator==(const ChannelHint&, const ChannelHint&) = default;
};

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
    // Soft adjustment travel for scrubbing and sliders. It is an interaction
    // hint, never a legal-value bound: typed values are not clamped or
    // quantized to it. When both are present they must be finite, ordered, and
    // inside the declared hard range.
    std::optional<double> softMinimum{};
    std::optional<double> softMaximum{};
    // Display rounding hint for numeric controls (0..9). Stored precision is
    // unchanged; integer parameters ignore it.
    std::optional<int> displayDecimals{};
    // Presentation row id. Consecutive parameters in one section sharing a
    // non-empty row render side-by-side and their `label` becomes the per-field
    // component label (Transform: row "Translate", labels "X"/"Y"). The
    // persisted parameter identities remain independent.
    std::string row{};
    // Multichannel editing semantics for Color/Vector2/Vector3 parameters.
    std::optional<ChannelHint> channels{};
    // Semantic constraint for numeric parameters: zero is not a legal value.
    // The catalog is the authoritative validator, so a generic parameter edit
    // cannot create an invalid mapping that only the evaluator would catch.
    bool nonzero{false};
    // Creation-time initial value rule (issue #92). `Default` keeps the schema
    // default; a rule that derives from the owning network's saved canvas is
    // resolved by the creation owner when the node is created, so the captured
    // value is authored state from that moment on (a later canvas edit never
    // rewrites it). Only a scalar Integer/Float parameter may declare a rule,
    // because the derived value is one whole-pixel dimension.
    ParameterInitialValue initialValue{ParameterInitialValue::Default};
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
    // The built-in catalog: exactly the authoritative built-in descriptor
    // projection (the schema side of the single explicit contribution list,
    // src/nemo/nodes/BuiltinNodes.inc). It assembles no descriptor of its own.
    NodeCatalog();
    // An exact schema inventory: the snapshot contains exactly `descriptors`
    // and nothing else. There is no mutation or unregister API, and invalid
    // descriptors throw std::invalid_argument before the snapshot is
    // observable. Fixtures that need the built-ins as well build their
    // inventory with extendedBuiltinSchema() and say so explicitly.
    explicit NodeCatalog(std::vector<NodeDescriptor> descriptors);
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

// The built-in schema inventory plus explicitly supplied extension descriptors,
// ready for the exact-inventory constructor. Duplicate identities, including a
// redeclared built-in, remain duplicates and are rejected by the catalog.
[[nodiscard]] std::vector<NodeDescriptor> extendedBuiltinSchema(std::vector<NodeDescriptor> extensions);

}  // namespace nemo
