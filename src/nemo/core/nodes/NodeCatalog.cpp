#include "nemo/core/nodes/NodeCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace nemo {
namespace {

[[nodiscard]] std::optional<std::string> validateParameterValue(const ParameterSpec& parameter,
                                                                std::string_view value) {
    if (!parameter.choices.empty() &&
        std::find(parameter.choices.begin(), parameter.choices.end(), value) == parameter.choices.end()) {
        return "must be one of the declared choices";
    }
    if (parameter.type == ParameterType::String)
        return std::nullopt;
    if (parameter.type == ParameterType::Boolean) {
        if (value != "true" && value != "false" && value != "0" && value != "1")
            return "must be boolean";
        return std::nullopt;
    }

    std::istringstream stream{std::string(value)};
    auto readNumber = [&stream](long double& number) {
        return static_cast<bool>(stream >> number) && std::isfinite(number);
    };
    auto withinRange = [&parameter](long double number) {
        return (!parameter.minimum || number >= static_cast<long double>(*parameter.minimum)) &&
               (!parameter.maximum || number <= static_cast<long double>(*parameter.maximum));
    };

    if (parameter.type == ParameterType::Color) {
        for (int channel = 0; channel < 4; ++channel) {
            long double number = 0.0L;
            if (!readNumber(number))
                return "must contain finite, float-representable numeric channels";
            const float representable = static_cast<float>(number);
            if (!std::isfinite(representable))
                return "must contain finite, float-representable numeric channels";
            if (!withinRange(number))
                return "is outside the declared range";
        }
        std::string extra;
        return stream >> extra ? std::optional<std::string>{"must contain exactly four channels"} : std::nullopt;
    }

    long double number = 0.0L;
    if (!readNumber(number))
        return parameter.type == ParameterType::Integer ? std::optional<std::string>{"must be an integer"}
                                                        : std::optional<std::string>{"must be finite numeric"};
    std::string extra;
    if (stream >> extra)
        return "contains extra tokens";
    if (parameter.type == ParameterType::Integer && number != std::trunc(number))
        return "must be an integer";
    if (parameter.type == ParameterType::Float && !std::isfinite(static_cast<float>(number)))
        return "must be finite and representable as a float";
    if (!withinRange(number))
        return "is outside the declared range";
    return std::nullopt;
}

void validateDescriptor(const NodeDescriptor& descriptor) {
    const std::string context = "node descriptor '" + descriptor.type + "'";
    if (descriptor.type.empty())
        throw std::invalid_argument("node descriptor type must not be empty");
    for (const unsigned char character : descriptor.type) {
        if (std::isspace(character) || !std::isprint(character))
            throw std::invalid_argument(context + ": type identity contains whitespace or control characters");
    }
    if (descriptor.implementationVersion == 0)
        throw std::invalid_argument(context + ": implementation version must be nonzero");

    auto validatePorts = [&context](const std::vector<PortSpec>& ports, const char* direction) {
        std::set<std::string> names;
        for (const auto& port : ports) {
            if (port.kind != PortKind::Image && port.kind != PortKind::Mask && port.kind != PortKind::Media)
                throw std::invalid_argument(context + ": " + direction + " port has an invalid kind");
            if (port.name.empty())
                throw std::invalid_argument(context + ": " + direction + " port name must not be empty");
            if (!names.insert(port.name).second)
                throw std::invalid_argument(context + ": duplicate " + direction + " port name '" + port.name + "'");
        }
    };
    validatePorts(descriptor.inputs, "input");
    validatePorts(descriptor.outputs, "output");

    std::set<std::string> parameterNames;
    for (const auto& parameter : descriptor.parameters) {
        if (parameter.type != ParameterType::Boolean && parameter.type != ParameterType::Integer &&
            parameter.type != ParameterType::Float && parameter.type != ParameterType::Color &&
            parameter.type != ParameterType::String)
            throw std::invalid_argument(context + ": parameter '" + parameter.name + "' has an invalid type");
        if (parameter.name.empty())
            throw std::invalid_argument(context + ": parameter name must not be empty");
        if (!parameterNames.insert(parameter.name).second)
            throw std::invalid_argument(context + ": duplicate parameter name '" + parameter.name + "'");
        if ((parameter.minimum && !std::isfinite(*parameter.minimum)) ||
            (parameter.maximum && !std::isfinite(*parameter.maximum)) ||
            (parameter.minimum && parameter.maximum && *parameter.minimum > *parameter.maximum)) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name + "' has an invalid range");
        }
        if ((parameter.type == ParameterType::String || parameter.type == ParameterType::Boolean) &&
            (parameter.minimum || parameter.maximum)) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares a range for a non-numeric type");
        }
        std::set<std::string> choices;
        for (const auto& choice : parameter.choices) {
            if (choice.empty() || !choices.insert(choice).second)
                throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                            "' has an empty or duplicate choice");
        }
        if (const auto problem = validateParameterValue(parameter, parameter.defaultValue)) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has an invalid default: " + *problem);
        }
        if (parameter.type == ParameterType::Integer) {
            if ((parameter.minimum && *parameter.minimum != std::trunc(*parameter.minimum)) ||
                (parameter.maximum && *parameter.maximum != std::trunc(*parameter.maximum))) {
                throw std::invalid_argument(context + ": integer parameter '" + parameter.name +
                                            "' has a non-integer bound");
            }
        }
    }

    std::set<int> scales;
    for (const int scale : descriptor.capabilities.samplingScales) {
        if (!isSamplingScale(scale))
            throw std::invalid_argument(context + ": sampling scale " + std::to_string(scale) +
                                        " is unusable (supported scales are 1, 2, and 4)");
        if (!scales.insert(scale).second)
            throw std::invalid_argument(context + ": duplicate sampling scale " + std::to_string(scale));
    }
    std::set<int> qualityModes;
    for (const Quality quality : descriptor.capabilities.qualityModes) {
        const int value = static_cast<int>(quality);
        if (quality != Quality::Draft && quality != Quality::Full)
            throw std::invalid_argument(context + ": invalid quality mode");
        if (!qualityModes.insert(value).second)
            throw std::invalid_argument(context + ": duplicate quality mode '" + qualityName(quality) + "'");
    }
    std::set<std::string> channels;
    for (const auto& channel : descriptor.capabilities.channels) {
        if (channel.empty() || !channels.insert(channel).second)
            throw std::invalid_argument(context + ": channels contain an empty or duplicate name");
    }
}

NodeCapabilities allBuiltinCapabilities(bool temporal = false) {
    return NodeCapabilities{.samplingScales = {1, 2, 4},
                            .qualityModes = {Quality::Full},
                            .channels = {"RGBA"},
                            .supportsRegion = true,
                            .temporal = temporal};
}

NodeDescriptor constColorDescriptor() {
    return NodeDescriptor{.type = "constcolor",
                          .displayName = "Constant Color",
                          .group = "Generators",
                          .implementationVersion = 1,
                          .inputs = {},
                          .outputs = {{PortKind::Image, "color"}},
                          .parameters = {{.name = "color", .type = ParameterType::Color, .defaultValue = "1 1 1 1"}},
                          .capabilities = allBuiltinCapabilities()};
}

NodeDescriptor mergeDescriptor() {
    return NodeDescriptor{.type = "merge",
                          .displayName = "Merge",
                          .group = "Compositing",
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "A"}, {PortKind::Image, "B"}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = {{.name = "operation",
                                          .type = ParameterType::String,
                                          .defaultValue = "over",
                                          .choices = {"over"}}},
                          .capabilities = allBuiltinCapabilities()};
}

NodeDescriptor outputDescriptor() {
    return NodeDescriptor{.type = "output",
                          .displayName = "Output",
                          .group = "I/O",
                          .isOutput = true,
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "color"}},
                          .outputs = {},
                          .parameters = {},
                          .capabilities = allBuiltinCapabilities()};
}

NodeDescriptor sourceDescriptor() {
    return NodeDescriptor{.type = "source",
                          .displayName = "Source",
                          .group = "Generators",
                          .implementationVersion = 1,
                          .inputs = {},
                          .outputs = {{PortKind::Image, "color"}},
                          .parameters = {{.name = "source", .type = ParameterType::String, .defaultValue = ""}},
                          .capabilities = allBuiltinCapabilities(true)};
}

NodeDescriptor testPatternDescriptor() {
    return NodeDescriptor{.type = "testpattern",
                          .displayName = "Test Pattern",
                          .group = "Generators",
                          .implementationVersion = 2,
                          .inputs = {},
                          .outputs = {{PortKind::Image, "color"}},
                          .parameters = {},
                          .capabilities = allBuiltinCapabilities(true)};
}

}  // namespace
NodeCatalog::NodeCatalog() {
    const auto append = [this](NodeDescriptor descriptor) {
        validateDescriptor(descriptor);
        if (find(descriptor.type) != nullptr)
            throw std::invalid_argument("duplicate node descriptor type '" + descriptor.type + "'");
        descriptors_.push_back(std::move(descriptor));
    };
    append(constColorDescriptor());
    append(mergeDescriptor());
    append(outputDescriptor());
    append(sourceDescriptor());
    append(testPatternDescriptor());
}

NodeCatalog::NodeCatalog(std::vector<NodeDescriptor> extensions) : NodeCatalog() {
    for (auto& extension : extensions) {
        if (extension.displayName.empty())
            extension.displayName = extension.type;
        validateDescriptor(extension);
        if (find(extension.type) != nullptr)
            throw std::invalid_argument("duplicate node descriptor type '" + extension.type + "'");
        descriptors_.push_back(std::move(extension));
    }
}

std::shared_ptr<const NodeCatalog> builtinNodeCatalogPtr() {
    static const auto catalog = std::make_shared<const NodeCatalog>();
    return catalog;
}

const NodeCatalog& builtinNodeCatalog() {
    return *builtinNodeCatalogPtr();
}

const NodeDescriptor* NodeCatalog::find(std::string_view type) const {
    const auto it = std::find_if(descriptors_.begin(), descriptors_.end(),
                                 [type](const NodeDescriptor& descriptor) { return descriptor.type == type; });
    return it == descriptors_.end() ? nullptr : &*it;
}

std::span<const int> NodeCatalog::samplingScalesSupported(std::string_view type) const {
    const auto* descriptor = find(type);
    return descriptor ? std::span<const int>(descriptor->capabilities.samplingScales) : std::span<const int>{};
}

const std::vector<PortSpec>& NodeCatalog::inputPorts(std::string_view type) const {
    static const std::vector<PortSpec> none;
    const auto* descriptor = find(type);
    return descriptor ? descriptor->inputs : none;
}

const std::vector<PortSpec>& NodeCatalog::outputPorts(std::string_view type) const {
    static const std::vector<PortSpec> none;
    const auto* descriptor = find(type);
    return descriptor ? descriptor->outputs : none;
}

std::optional<std::string> NodeCatalog::validateParameter(std::string_view type, std::string_view key,
                                                          std::string_view value) const {
    const auto* descriptor = find(type);
    if (descriptor == nullptr)
        return std::nullopt;
    const auto it = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
                                 [key](const ParameterSpec& p) { return p.name == key; });
    if (it == descriptor->parameters.end())
        return std::nullopt;  // Unknown authored fields remain recoverable data.
    return validateParameterValue(*it, value);
}

std::optional<std::string_view> NodeCatalog::parameterDefault(std::string_view type, std::string_view key) const {
    const auto* descriptor = find(type);
    if (descriptor == nullptr)
        return std::nullopt;
    const auto it = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
                                 [key](const ParameterSpec& p) { return p.name == key; });
    return it == descriptor->parameters.end() ? std::nullopt : std::optional<std::string_view>{it->defaultValue};
}
std::optional<std::uint64_t> NodeCatalog::implementationVersion(std::string_view type) const {
    const auto* descriptor = find(type);
    return descriptor == nullptr ? std::nullopt : std::optional<std::uint64_t>{descriptor->implementationVersion};
}

}  // namespace nemo
