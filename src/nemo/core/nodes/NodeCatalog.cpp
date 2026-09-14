#include "nemo/core/nodes/NodeCatalog.hpp"

#include "nemo/core/evaluation/SourceRequest.hpp"

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
                          .parameters = {{.name = "color",
                                          .type = ParameterType::Color,
                                          .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                                          .label = "Color",
                                          .section = "Color",
                                          .editor = {}}},
                          .capabilities = allBuiltinCapabilities()};
}

NodeDescriptor mergeDescriptor() {
    return NodeDescriptor{
        .type = "merge",
        .displayName = "Merge",
        .group = "Compositing",
        .implementationVersion = 2,
        .inputs = {{PortKind::Image, "A", false}, {PortKind::Image, "B", false}, {PortKind::Mask, "mask", true}},
        .outputs = {{PortKind::Image, "out"}},
        .parameters = {{.name = "operation",
                        .type = ParameterType::Choice,
                        .defaultValue = ParameterValue{ChoiceValue{"over"}},
                        .choices = {"over", "plus", "multiply", "screen", "difference"},
                        .label = "Operation",
                        .section = "Composite",
                        .editor = "nemo.merge.operation"},
                       {.name = "mix",
                        .type = ParameterType::Float,
                        .defaultValue = ParameterValue{1.0},
                        .minimum = 0.0,
                        .maximum = 1.0,
                        .label = "Mix",
                        .section = "Composite",
                        .editor = {}},
                       {.name = "maskChannel",
                        .type = ParameterType::Choice,
                        .defaultValue = ParameterValue{ChoiceValue{"A"}},
                        .choices = {"none", "R", "G", "B", "A"},
                        .label = "Mask Channel",
                        .section = "Mask",
                        .editor = {}},
                       {.name = "invertMask",
                        .type = ParameterType::Boolean,
                        .defaultValue = ParameterValue{false},
                        .label = "Invert Mask",
                        .section = "Mask",
                        .editor = {}}},
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

NodeDescriptor viewerDescriptor() {
    return NodeDescriptor{.type = "viewer",
                          .displayName = "Viewer",
                          .group = "I/O",
                          .isOutput = false,
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "color"}},
                          .outputs = {},
                          .parameters = {},
                          .capabilities = allBuiltinCapabilities()};
}

NodeDescriptor sourceDescriptor() {
    // Node-scoped Read settings. Names/defaults are the resolver's frozen
    // semantic owner (evaluation/SourceRequest.hpp): the catalog declares the
    // same keys so authoring/validation/presentation cannot drift from the one
    // effective-request owner.
    const auto choice = [](std::string_view name, std::string_view label, std::string_view section, std::string value,
                           std::vector<std::string> choices) {
        return ParameterSpec{.name = std::string{name},
                             .type = ParameterType::Choice,
                             .defaultValue = ParameterValue{ChoiceValue{std::string{value}}},
                             .choices = std::move(choices),
                             .label = std::string{label},
                             .section = std::string{section},
                             .editor = {}};
    };
    const auto integer = [](std::string_view name, std::string_view label, std::string_view section, std::int64_t value,
                            bool nonzero = false) {
        return ParameterSpec{.name = std::string{name},
                             .type = ParameterType::Integer,
                             .defaultValue = ParameterValue{value},
                             .label = std::string{label},
                             .section = std::string{section},
                             .editor = {},
                             .nonzero = nonzero};
    };
    return NodeDescriptor{
        .type = "source",
        .displayName = "Read",
        .group = "I/O",
        .implementationVersion = 2,
        .inputs = {},
        .outputs = {{PortKind::Image, "color"}},
        .parameters =
            {{.name = "source",
              .type = ParameterType::String,
              .defaultValue = ParameterValue{std::string{}},
              .label = "File",
              .section = "Source",
              .editor = "nemo.read.source"},
             choice(kReadParamRangeMode, "Range Mode", "Timing", "auto", {"auto", "custom"}),
             integer(kReadParamRangeFirst, "First Frame", "Timing", 0),
             integer(kReadParamRangeLast, "Last Frame", "Timing", 0),
             integer(kReadParamFrameOffset, "Offset", "Timing", 0),
             // Step is a nonzero signed integer; the catalog's
             // nonzero constraint is the generic-edit validator, and
             // no bounds are declared so a typed value is never
             // clamped.
             integer(kReadParamFrameStep, "Step", "Timing", 1, /*nonzero=*/true),
             choice(kReadParamBeforePolicy, "Before", "Policies", "error", {"error", "hold", "black"}),
             choice(kReadParamAfterPolicy, "After", "Policies", "error", {"error", "hold", "black"}),
             choice(kReadParamMissingPolicy, "Missing Frames", "Policies", "error", {"error", "black"}),
             choice(kReadParamInputTransform, "Input Transform", "Color", "auto", {"auto", "explicit", "raw"}),
             {.name = std::string{kReadParamInputColorSpace},
              .type = ParameterType::String,
              .defaultValue = ParameterValue{std::string{}},
              .label = "Input Color Space",
              .section = "Color",
              .editor = {}},
             choice(kReadParamAlphaMode, "Alpha Mode", "Color", "auto", {"auto", "straight", "premultiplied"}),
             choice(kReadParamSourceTransfer, "Transfer", "Encoding Hints", "auto",
                    {"auto", "bt709", "srgb", "gamma22", "gamma28", "linear"}),
             choice(kReadParamSourcePrimaries, "Primaries", "Encoding Hints", "auto", {"auto", "bt709"}),
             choice(kReadParamSourceMatrix, "Matrix", "Encoding Hints", "auto", {"auto", "bt709", "bt601"}),
             choice(kReadParamSourceRange, "Range", "Encoding Hints", "auto", {"auto", "limited", "full"}),
             choice(kReadParamSourceChromaLocation, "Chroma Location", "Encoding Hints", "auto", {"auto", "left"})},
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

// Every native effect shares the same optional-mask contract: a required
// image at input 0 and an optional mask at input 1, plus the mask controls
// below. The pixel math lives in the executor; only the schema is shared.
std::vector<PortSpec> effectInputs() {
    return {{PortKind::Image, "image", false}, {PortKind::Mask, "mask", true}};
}

std::vector<ParameterSpec> maskParameterSpecs() {
    return {
        {.name = "maskChannel",
         .type = ParameterType::Choice,
         .defaultValue = ParameterValue{ChoiceValue{"A"}},
         .choices = {"none", "R", "G", "B", "A"},
         .label = "Mask Channel",
         .section = "Mask",
         .editor = {}},
        {.name = "invertMask",
         .type = ParameterType::Boolean,
         .defaultValue = ParameterValue{false},
         .label = "Invert Mask",
         .section = "Mask",
         .editor = {}},
    };
}

// Mix is an ordinary effect control, not a mask control: it must stay reachable
// and usable with no mask connected (stories 38 and 71). Each effect appends it
// to its primary list and the shared mask specs stay the only "Mask" section
// members.
ParameterSpec mixParameterSpec(std::string section) {
    return {.name = "mix",
            .type = ParameterType::Float,
            .defaultValue = ParameterValue{1.0},
            .minimum = 0.0,
            .maximum = 1.0,
            .label = "Mix",
            .section = std::move(section),
            .editor = {}};
}

std::vector<ParameterSpec> withMaskParameters(std::vector<ParameterSpec> specific) {
    auto mask = maskParameterSpecs();
    specific.reserve(specific.size() + mask.size());
    for (auto& parameter : mask)
        specific.push_back(std::move(parameter));
    return specific;
}

// Whole-image effects reject region requests through the existing capability
// validation; their spatial parameters stay full-resolution.
NodeCapabilities wholeImageCapabilities() {
    NodeCapabilities capabilities = allBuiltinCapabilities();
    capabilities.supportsRegion = false;
    return capabilities;
}

NodeDescriptor gradeDescriptor() {
    const auto channelEditor = std::string{"nemo.channels.rgb"};
    const ChannelHint additive{ChannelLink::Additive, true};
    const ChannelHint multiplicative{ChannelLink::Multiplicative, true};
    return NodeDescriptor{.type = "grade",
                          .displayName = "Grade",
                          .group = "Color",
                          .implementationVersion = 1,
                          .inputs = effectInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              {.name = "lift",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
                               .label = "Lift",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = -1.0,
                               .softMaximum = 1.0,
                               .channels = additive},
                              {.name = "gain",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Gain",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 2.0,
                               .channels = multiplicative},
                              {.name = "multiply",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Multiply",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 2.0,
                               .channels = multiplicative},
                              {.name = "offset",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
                               .label = "Offset",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = -1.0,
                               .softMaximum = 1.0,
                               .channels = additive},
                              {.name = "gamma",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Gamma",
                               .section = "Primary",
                               .editor = channelEditor,
                               .softMinimum = 0.01,
                               .softMaximum = 4.0,
                               .channels = multiplicative},
                              mixParameterSpec("Primary"),
                              {.name = "blackpoint",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
                               .label = "Blackpoint",
                               .section = "Range",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 1.0,
                               .channels = additive},
                              {.name = "whitepoint",
                               .type = ParameterType::Color,
                               .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                               .label = "Whitepoint",
                               .section = "Range",
                               .editor = channelEditor,
                               .softMinimum = 0.0,
                               .softMaximum = 2.0,
                               .channels = additive},
                              {.name = "channels",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"RGB"}},
                               .choices = {"RGB", "RGBA", "R", "G", "B", "Alpha", "None"},
                               .label = "Channels",
                               .section = "Options",
                               .editor = {}},
                              {.name = "reverse",
                               .type = ParameterType::Boolean,
                               .defaultValue = ParameterValue{false},
                               .label = "Reverse",
                               .section = "Options",
                               .editor = {}},
                              {.name = "clampBlack",
                               .type = ParameterType::Boolean,
                               .defaultValue = ParameterValue{true},
                               .label = "Clamp Black",
                               .section = "Options",
                               .editor = {}},
                              {.name = "clampWhite",
                               .type = ParameterType::Boolean,
                               .defaultValue = ParameterValue{false},
                               .label = "Clamp White",
                               .section = "Options",
                               .editor = {}},
                          }),
                          .capabilities = allBuiltinCapabilities()};
}

NodeDescriptor blurDescriptor() {
    return NodeDescriptor{.type = "blur",
                          .displayName = "Blur",
                          .group = "Blur",
                          .implementationVersion = 1,
                          .inputs = effectInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              {.name = "size",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .minimum = 0.0,
                               .maximum = 100.0,
                               .step = 0.1,
                               .label = "Size",
                               .section = "Blur",
                               .editor = {}},
                              {.name = "channels",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"RGBA"}},
                               .choices = {"RGBA", "RGB", "Alpha"},
                               .label = "Channels",
                               .section = "Blur",
                               .editor = {}},
                              mixParameterSpec("Blur"),
                          }),
                          .capabilities = wholeImageCapabilities()};
}

NodeDescriptor transformDescriptor() {
    return NodeDescriptor{.type = "transform",
                          .displayName = "Transform",
                          .group = "Transform",
                          .implementationVersion = 1,
                          .inputs = effectInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              {.name = "translateX",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .step = 1.0,
                               .label = "X",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = -200.0,
                               .softMaximum = 200.0,
                               .row = "Translate"},
                              {.name = "translateY",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .step = 1.0,
                               .label = "Y",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = -200.0,
                               .softMaximum = 200.0,
                               .row = "Translate"},
                              {.name = "scale",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{1.0},
                               .minimum = 0.0,
                               .step = 0.001,
                               .label = "Scale",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = 0.1,
                               .softMaximum = 3.0,
                               .nonzero = true},
                              {.name = "rotate",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .step = 0.1,
                               .label = "Rotate",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = -180.0,
                               .softMaximum = 180.0},
                              mixParameterSpec("Transform"),
                              {.name = "filter",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"Cubic"}},
                               .choices = {"Cubic", "Linear", "Nearest"},
                               .label = "Filter",
                               .section = "Sampling",
                               .editor = {}},
                          }),
                          .capabilities = wholeImageCapabilities()};
}

}  // namespace
NodeCatalog::NodeCatalog() {
    const auto append = [this](NodeDescriptor descriptor) {
        validateDescriptor(descriptor);
        if (find(descriptor.type) != nullptr)
            throw std::invalid_argument("duplicate node descriptor type '" + descriptor.type + "'");
        descriptors_.push_back(std::move(descriptor));
    };
    append(blurDescriptor());
    append(constColorDescriptor());
    append(gradeDescriptor());
    append(mergeDescriptor());
    append(outputDescriptor());
    append(sourceDescriptor());
    append(testPatternDescriptor());
    append(transformDescriptor());
    append(viewerDescriptor());
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
