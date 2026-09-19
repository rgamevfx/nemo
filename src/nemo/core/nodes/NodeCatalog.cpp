#include "nemo/core/nodes/NodeCatalog.hpp"

#include "nemo/core/evaluation/NodeContributions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace nemo {
namespace {

[[nodiscard]] std::optional<std::string> validateParameterValue(const ParameterSpec& parameter,
                                                                const ParameterValue& value) {
    const auto withinRange = [&parameter](long double number) {
        return (!parameter.minimum || number >= static_cast<long double>(*parameter.minimum)) &&
               (!parameter.maximum || number <= static_cast<long double>(*parameter.maximum));
    };
    const auto validateFloat = [&withinRange](float number) -> std::optional<std::string> {
        if (!std::isfinite(number))
            return "must be finite and float-representable";
        if (!withinRange(static_cast<long double>(number)))
            return "is outside the declared range";
        return std::nullopt;
    };
    switch (parameter.type) {
    case ParameterType::Boolean:
        return std::holds_alternative<bool>(value) ? std::nullopt : std::optional<std::string>{"must be boolean"};
    case ParameterType::Integer: {
        if (!std::holds_alternative<std::int64_t>(value))
            return "must be an integer";
        const auto integer = std::get<std::int64_t>(value);
        if (parameter.nonzero && integer == 0)
            return "must be nonzero";
        const long double number = static_cast<long double>(integer);
        return withinRange(number) ? std::nullopt : std::optional<std::string>{"is outside the declared range"};
    }
    case ParameterType::Float: {
        if (!std::holds_alternative<double>(value))
            return "must be finite numeric";
        const double number = std::get<double>(value);
        if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
            return "must be finite and representable as a float";
        if (parameter.nonzero && number == 0.0)
            return "must be nonzero";
        return withinRange(static_cast<long double>(number))
                   ? std::nullopt
                   : std::optional<std::string>{"is outside the declared range"};
    }
    case ParameterType::Choice: {
        if (!std::holds_alternative<ChoiceValue>(value))
            return "must be a choice";
        const auto& choice = std::get<ChoiceValue>(value).value;
        return std::find(parameter.choices.begin(), parameter.choices.end(), choice) != parameter.choices.end()
                   ? std::nullopt
                   : std::optional<std::string>{"must be one of the declared choices"};
    }
    case ParameterType::Vector2: {
        if (!std::holds_alternative<Vector2Value>(value))
            return "must contain two finite, float-representable components";
        for (const float number : std::get<Vector2Value>(value).value)
            if (const auto problem = validateFloat(number))
                return problem;
        return std::nullopt;
    }
    case ParameterType::Vector3: {
        if (!std::holds_alternative<Vector3Value>(value))
            return "must contain three finite, float-representable components";
        for (const float number : std::get<Vector3Value>(value).value)
            if (const auto problem = validateFloat(number))
                return problem;
        return std::nullopt;
    }
    case ParameterType::Color: {
        if (!std::holds_alternative<ColorValue>(value))
            return "must contain four finite, float-representable channels";
        for (const float number : std::get<ColorValue>(value).value)
            if (const auto problem = validateFloat(number))
                return problem;
        return std::nullopt;
    }
    case ParameterType::String:
        return std::holds_alternative<std::string>(value) ? std::nullopt
                                                          : std::optional<std::string>{"must be a string"};
    }
    return "has an invalid type";
}

template <std::size_t N>
[[nodiscard]] std::optional<std::string> parseVectorText(std::string_view text, std::array<float, N>& result,
                                                         const char* description) {
    std::istringstream stream{std::string(text)};
    for (float& component : result) {
        long double number = 0.0L;
        if (!(stream >> number) || !std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
            return std::string("must contain exactly ") + std::to_string(N) + " finite, float-representable " +
                   description;
        component = static_cast<float>(number);
    }
    std::string extra;
    if (stream >> extra)
        return std::string("must contain exactly ") + std::to_string(N) + " " + description;
    return std::nullopt;
}

[[nodiscard]] ParameterValue parseParameterValueText(const ParameterSpec& parameter, std::string_view text) {
    switch (parameter.type) {
    case ParameterType::String:
        return std::string{text};
    case ParameterType::Choice:
        return ChoiceValue{std::string{text}};
    case ParameterType::Boolean:
        if (text == "true" || text == "1")
            return true;
        if (text == "false" || text == "0")
            return false;
        throw std::invalid_argument("must be boolean");
    case ParameterType::Integer: {
        std::istringstream stream{std::string(text)};
        std::int64_t number = 0;
        std::string extra;
        if (!(stream >> number) || (stream >> extra))
            throw std::invalid_argument("must be an integer");
        return number;
    }
    case ParameterType::Float: {
        std::istringstream stream{std::string(text)};
        long double number = 0.0L;
        std::string extra;
        if (!(stream >> number) || !std::isfinite(number) || (stream >> extra) ||
            !std::isfinite(static_cast<float>(number)))
            throw std::invalid_argument("must be finite and representable as a float");
        return static_cast<double>(number);
    }
    case ParameterType::Vector2: {
        Vector2Value result;
        if (const auto problem = parseVectorText(text, result.value, "components"))
            throw std::invalid_argument(*problem);
        return result;
    }
    case ParameterType::Vector3: {
        Vector3Value result;
        if (const auto problem = parseVectorText(text, result.value, "components"))
            throw std::invalid_argument(*problem);
        return result;
    }
    case ParameterType::Color: {
        ColorValue result;
        if (const auto problem = parseVectorText(text, result.value, "channels"))
            throw std::invalid_argument(*problem);
        return result;
    }
    }
    throw std::invalid_argument("has an invalid type");
}

[[nodiscard]] bool hasControlCharacters(std::string_view text) {
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 || value == 0x7F)
            return true;
    }
    return false;
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
    if ((descriptor.inputs.empty() && descriptor.mainInput != 0) ||
        (!descriptor.inputs.empty() && descriptor.mainInput >= descriptor.inputs.size()))
        throw std::invalid_argument(context + ": main input port " + std::to_string(descriptor.mainInput) +
                                    " is not declared");

    std::set<std::string> parameterNames;
    for (const auto& parameter : descriptor.parameters) {
        if (parameter.name.empty())
            throw std::invalid_argument(context + ": parameter name must not be empty");
        if (!parameterNames.insert(parameter.name).second)
            throw std::invalid_argument(context + ": duplicate parameter name '" + parameter.name + "'");
        if ((parameter.minimum && !std::isfinite(*parameter.minimum)) ||
            (parameter.maximum && !std::isfinite(*parameter.maximum)) ||
            (parameter.minimum && parameter.maximum && *parameter.minimum > *parameter.maximum)) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name + "' has an invalid range");
        }
        const bool numeric = parameter.type == ParameterType::Integer || parameter.type == ParameterType::Float ||
                             parameter.type == ParameterType::Vector2 || parameter.type == ParameterType::Vector3 ||
                             parameter.type == ParameterType::Color;
        if (!numeric && (parameter.minimum || parameter.maximum))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares a range for a non-numeric type");
        if (parameter.step && !numeric)
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares a step for a non-numeric type");
        if (parameter.step && (!std::isfinite(*parameter.step) || *parameter.step <= 0.0))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has a step that is not finite and positive");
        if ((parameter.softMinimum && !std::isfinite(*parameter.softMinimum)) ||
            (parameter.softMaximum && !std::isfinite(*parameter.softMaximum)) ||
            (parameter.softMinimum && parameter.softMaximum && *parameter.softMinimum > *parameter.softMaximum)) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name + "' has an invalid soft range");
        }
        if (!numeric && (parameter.softMinimum || parameter.softMaximum))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares a soft range for a non-numeric type");
        if (parameter.nonzero && !numeric)
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares nonzero for a non-numeric type");
        if (parameter.initialValue != ParameterInitialValue::Default && parameter.type != ParameterType::Integer &&
            parameter.type != ParameterType::Float) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares a creation-time initial value for a parameter that is not a "
                                        "scalar Integer or Float");
        }
        if ((parameter.softMinimum && parameter.minimum && *parameter.softMinimum < *parameter.minimum) ||
            (parameter.softMinimum && parameter.maximum && *parameter.softMinimum > *parameter.maximum) ||
            (parameter.softMaximum && parameter.maximum && *parameter.softMaximum > *parameter.maximum) ||
            (parameter.softMaximum && parameter.minimum && *parameter.softMaximum < *parameter.minimum)) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has a soft range outside its declared range");
        }
        if (parameter.displayDecimals && (*parameter.displayDecimals < 0 || *parameter.displayDecimals > 9))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has display decimals outside the 0..9 range");
        if (parameter.displayDecimals && !numeric)
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares display decimals for a non-numeric type");
        if (!parameter.row.empty() && hasControlCharacters(parameter.row))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has a row containing control characters");
        if (parameter.channels && parameter.type != ParameterType::Color && parameter.type != ParameterType::Vector2 &&
            parameter.type != ParameterType::Vector3) {
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' declares channel semantics for a non-tuple type");
        }
        if (!parameter.label.empty() && hasControlCharacters(parameter.label))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has a label containing control characters");
        if (!parameter.section.empty() && hasControlCharacters(parameter.section))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has a section containing control characters");
        if (!parameter.editor.empty()) {
            if (parameter.editor.find('.') == std::string::npos)
                throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                            "' has an editor id without a namespace separator");
            bool malformed = false;
            for (const char character : parameter.editor) {
                const auto value = static_cast<unsigned char>(character);
                if (std::isspace(value) || value < 0x20 || value == 0x7F) {
                    malformed = true;
                    break;
                }
            }
            if (malformed)
                throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                            "' has an editor id containing whitespace or control characters");
        }
        if (parameter.type != ParameterType::Choice && !parameter.choices.empty())
            throw std::invalid_argument(context + ": only choice parameters may declare choices");
        if (parameter.type == ParameterType::Choice && parameter.choices.empty())
            throw std::invalid_argument(context + ": choice parameter '" + parameter.name + "' has no choices");
        std::set<std::string> choices;
        for (const auto& choice : parameter.choices) {
            if (choice.empty() || !choices.insert(choice).second)
                throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                            "' has an empty or duplicate choice");
        }
        if (parameter.type == ParameterType::Integer &&
            ((parameter.minimum && *parameter.minimum != std::trunc(*parameter.minimum)) ||
             (parameter.maximum && *parameter.maximum != std::trunc(*parameter.maximum)))) {
            throw std::invalid_argument(context + ": integer parameter '" + parameter.name +
                                        "' has a non-integer bound");
        }
        if (const auto problem = validateParameterValue(parameter, parameter.defaultValue))
            throw std::invalid_argument(context + ": parameter '" + parameter.name +
                                        "' has an invalid default: " + *problem);
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

// The authoritative built-in inventory: the schema projection of the single
// explicit contribution list. Nothing else assembles built-in descriptors.
[[nodiscard]] std::vector<NodeDescriptor> builtinDescriptorInventory() {
    const auto& descriptors = builtinNodeCatalogPtr()->descriptors();
    return std::vector<NodeDescriptor>(descriptors.begin(), descriptors.end());
}

}  // namespace

NodeCatalog::NodeCatalog() : NodeCatalog(builtinDescriptorInventory()) {}

NodeCatalog::NodeCatalog(std::vector<NodeDescriptor> descriptors) {
    for (auto& descriptor : descriptors) {
        // An unnamed display label falls back to the persistent identity; it is
        // presentation only and never participates in identity.
        if (descriptor.displayName.empty())
            descriptor.displayName = descriptor.type;
        validateDescriptor(descriptor);
        if (find(descriptor.type) != nullptr)
            throw std::invalid_argument("duplicate node descriptor type '" + descriptor.type + "'");
        descriptors_.push_back(std::move(descriptor));
    }
}

std::vector<NodeDescriptor> extendedBuiltinSchema(std::vector<NodeDescriptor> extensions) {
    const auto& builtins = builtinNodeCatalog().descriptors();
    std::vector<NodeDescriptor> inventory;
    inventory.reserve(builtins.size() + extensions.size());
    inventory.insert(inventory.end(), builtins.begin(), builtins.end());
    for (NodeDescriptor& extension : extensions)
        inventory.push_back(std::move(extension));
    return inventory;
}

std::shared_ptr<const NodeCatalog> builtinNodeCatalogPtr() {
    // The authoritative built-in inventory is the contribution projection: the
    // schema catalog is derived from the single explicit node list, so there is
    // no second runtime inventory and no construction recursion.
    return builtinNodeContributions()->catalog();
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
                                                          const ParameterValue& value) const {
    const auto* parameter = parameterSpec(type, key);
    if (parameter == nullptr)
        return validateParameterValueRepresentation(value);
    return validateParameterValue(*parameter, value);
}

const ParameterValue* NodeCatalog::parameterDefault(std::string_view type, std::string_view key) const {
    const auto* parameter = parameterSpec(type, key);
    return parameter == nullptr ? nullptr : &parameter->defaultValue;
}

const ParameterSpec* NodeCatalog::parameterSpec(std::string_view type, std::string_view key) const {
    const auto* descriptor = find(type);
    if (descriptor == nullptr)
        return nullptr;
    const auto it = std::find_if(descriptor->parameters.begin(), descriptor->parameters.end(),
                                 [key](const ParameterSpec& parameter) { return parameter.name == key; });
    return it == descriptor->parameters.end() ? nullptr : &*it;
}

ParameterValue NodeCatalog::parseParameterText(std::string_view type, std::string_view key,
                                               std::string_view value) const {
    const auto* parameter = parameterSpec(type, key);
    if (parameter == nullptr)
        return std::string{value};  // Unknown schemas remain recoverable typed data.
    ParameterValue parsed;
    try {
        parsed = parseParameterValueText(*parameter, value);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument("parameter '" + std::string(key) + "': " + error.what());
    }
    if (const auto problem = validateParameterValue(*parameter, parsed))
        throw std::invalid_argument("parameter '" + std::string(key) + "': " + *problem);
    return parsed;
}
std::optional<std::uint64_t> NodeCatalog::implementationVersion(std::string_view type) const {
    const auto* descriptor = find(type);
    return descriptor == nullptr ? std::nullopt : std::optional<std::uint64_t>{descriptor->implementationVersion};
}

}  // namespace nemo
