#include "nemo/extensions/InstalledPackages.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/eval/BindingContract.hpp"
#include "nemo/extensions/EffectAbi.h"

namespace nemo::extensions {
namespace {

namespace fs = std::filesystem;

using detail::kDiagnosticCapacity;
using detail::PackageGpu;
using detail::PackageRecord;
using detail::SharedLibrary;

constexpr std::uint64_t kManifestFormat = 1;
constexpr std::string_view kManifestFile = "manifest.json";
constexpr std::string_view kPointwiseCapability = "nemo.effect.pointwise.v1";
constexpr std::string_view kQmlCapability = "nemo.ui.qml.v1";

[[nodiscard]] std::string pathText(const fs::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] fs::path pathFromText(const std::string& text) {
#if defined(_WIN32)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::path(text);
#endif
}

// The node's content-derived runtime identity (issue #37): the package's state
// identity (its id and the authored-state version), its package version and its
// processing version. It is the descriptor's implementationVersion, so it is
// what a content-derived result key carries (Reuse.cpp) and what the GPU
// implementation must declare — a changed package, state interpretation or
// processing can never serve a cached image, and no presentation-only manifest
// change moves it.
[[nodiscard]] std::uint64_t packageImplementationVersion(const std::string& stateIdentity, std::uint64_t version,
                                                         std::uint64_t processingVersion) {
    std::uint64_t hash = kFnv1a64Basis;
    hashMixText(hash, std::string{"nemo.extensions.package.v1"});
    hashMixText(hash, stateIdentity);
    hashMixWord(hash, version);
    hashMixWord(hash, processingVersion);
    return hash == 0 ? 1 : hash;
}

// A namespaced identity: at least one namespace separator and no whitespace or
// control character, the same rule the schema catalog applies to node types and
// editor ids.
[[nodiscard]] bool isNamespacedIdentifier(std::string_view identifier) {
    if (identifier.empty() || identifier.find('.') == std::string_view::npos) {
        return false;
    }
    for (const unsigned char character : identifier) {
        if (std::isspace(character) != 0 || std::isprint(character) == 0) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void fail(const std::string& context, const std::string& reason) {
    throw std::runtime_error(context + ": " + reason);
}

// ---------------------------------------------------------------------------
// Manifest reading helpers. Every failure names the package and the offending
// key, and every reader rejects a value of the wrong shape instead of coercing
// it, so a malformed manifest is refused rather than half-understood.
// ---------------------------------------------------------------------------

void rejectUnknownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> known,
                       const std::string& context) {
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        const bool allowed =
            std::any_of(known.begin(), known.end(), [&entry](std::string_view key) { return key == entry.key(); });
        if (!allowed) {
            fail(context, "unknown key '" + entry.key() + "'");
        }
    }
}

[[nodiscard]] const nlohmann::json* optionalMember(const nlohmann::json& parent, const char* key) {
    const auto entry = parent.find(key);
    return entry == parent.end() ? nullptr : &*entry;
}

[[nodiscard]] const nlohmann::json& requiredObject(const nlohmann::json& parent, const char* key,
                                                   const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        fail(context, std::string("'") + key + "' is required");
    }
    if (!entry->is_object()) {
        fail(context, std::string("'") + key + "' must be an object");
    }
    return *entry;
}

[[nodiscard]] const nlohmann::json& requiredArray(const nlohmann::json& parent, const char* key,
                                                  const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        fail(context, std::string("'") + key + "' is required");
    }
    if (!entry->is_array()) {
        fail(context, std::string("'") + key + "' must be an array");
    }
    return *entry;
}

[[nodiscard]] std::string requiredString(const nlohmann::json& parent, const char* key, const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        fail(context, std::string("'") + key + "' is required");
    }
    if (!entry->is_string()) {
        fail(context, std::string("'") + key + "' must be a string");
    }
    const std::string value = entry->get<std::string>();
    if (value.empty()) {
        fail(context, std::string("'") + key + "' must not be empty");
    }
    return value;
}

[[nodiscard]] std::optional<std::string> optionalString(const nlohmann::json& parent, const char* key,
                                                        const std::string& context) {
    const nlohmann::json* entry = optionalMember(parent, key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    if (!entry->is_string()) {
        fail(context, std::string("'") + key + "' must be a string");
    }
    return entry->get<std::string>();
}

[[nodiscard]] std::uint64_t requiredUnsigned(const nlohmann::json& parent, const char* key,
                                             const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        fail(context, std::string("'") + key + "' is required");
    }
    if (entry->is_number_unsigned()) {
        return entry->get<std::uint64_t>();
    }
    if (entry->is_number_integer()) {
        const auto value = entry->get<std::int64_t>();
        if (value < 0) {
            fail(context, std::string("'") + key + "' must be a non-negative integer");
        }
        return static_cast<std::uint64_t>(value);
    }
    fail(context, std::string("'") + key + "' must be a non-negative integer");
}

[[nodiscard]] std::optional<double> optionalNumber(const nlohmann::json& parent, const char* key,
                                                   const std::string& context) {
    const nlohmann::json* entry = optionalMember(parent, key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    if (!entry->is_number()) {
        fail(context, std::string("'") + key + "' must be a number");
    }
    return entry->get<double>();
}

[[nodiscard]] std::optional<int> optionalInteger(const nlohmann::json& parent, const char* key,
                                                 const std::string& context) {
    const nlohmann::json* entry = optionalMember(parent, key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    if (entry->is_number_unsigned()) {
        const auto value = entry->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            fail(context, std::string("'") + key + "' is outside the integer range");
        }
        return static_cast<int>(value);
    }
    if (!entry->is_number_integer()) {
        fail(context, std::string("'") + key + "' must be an integer");
    }
    const auto value = entry->get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        fail(context, std::string("'") + key + "' is outside the integer range");
    }
    return static_cast<int>(value);
}

// ---------------------------------------------------------------------------
// Package files. Every declared file is a plain relative path inside the
// package directory and must be installed; an absolute path or a path escaping
// the directory is refused instead of being resolved against the working
// directory.
// ---------------------------------------------------------------------------

[[nodiscard]] bool resolvePackageFile(const fs::path& root, const std::string& declared, std::string& resolved,
                                      std::string& reason) {
    if (declared.empty()) {
        reason = "declares an empty file path";
        return false;
    }
    const fs::path relative = pathFromText(declared);
    if (relative.has_root_path()) {
        reason = "declared file '" + declared + "' must be a path inside the package";
        return false;
    }
    for (const fs::path& part : relative) {
        if (part == "..") {
            reason = "declared file '" + declared + "' must not escape the package directory";
            return false;
        }
    }
    std::error_code error;
    const fs::path candidate = root / relative;
    if (!fs::is_regular_file(candidate, error)) {
        reason = "declared file '" + declared + "' is not installed";
        return false;
    }
    const fs::path canonical = fs::weakly_canonical(candidate, error);
    if (error) {
        reason = "declared file '" + declared + "' cannot be resolved: " + error.message();
        return false;
    }
    const fs::path packageRoot = fs::weakly_canonical(root, error);
    const fs::path within = canonical.lexically_relative(packageRoot);
    if (error || within.empty() || within.is_absolute() ||
        std::any_of(within.begin(), within.end(), [](const fs::path& part) { return part == ".."; })) {
        reason = "declared file '" + declared + "' resolves outside the package directory";
        return false;
    }
    resolved = pathText(canonical);
    return true;
}

// Absolute file URL of an installed package resource: the form the host's QML
// editor and panel facilities consume, with every byte outside the URL path
// character set percent-encoded so a space or a '#' in an install path cannot
// change the meaning of the resource locator.
[[nodiscard]] std::string fileUrl(const std::string& absolute) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string url{absolute.starts_with("//") ? "file:" : absolute.starts_with('/') ? "file://" : "file:///"};
    url.reserve(absolute.size() + 8);
    for (const unsigned char byte : absolute) {
        const bool unreserved = std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (unreserved || byte == '/' || byte == ':') {
            url.push_back(static_cast<char>(byte));
            continue;
        }
        url.push_back('%');
        url.push_back(kHexDigits[byte >> 4]);
        url.push_back(kHexDigits[byte & 0x0FU]);
    }
    return url;
}

[[nodiscard]] bool readTextFile(const std::string& path, std::string& text) {
    std::ifstream stream(pathFromText(path), std::ios::binary);
    if (!stream) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

[[nodiscard]] bool isSpirvModule(const fs::path& path) {
    std::error_code error;
    const auto bytes = fs::file_size(path, error);
    std::array<std::uint32_t, 5> header{};
    if (error || bytes < sizeof(header) || bytes % sizeof(std::uint32_t) != 0)
        return false;
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    return stream && header[0] == 0x07230203U && header[1] >= 0x00010000U && header[1] <= 0x00010600U &&
           header[3] != 0 && header[4] == 0;
}

// ---------------------------------------------------------------------------
// Manifest schema.
// ---------------------------------------------------------------------------

[[nodiscard]] ParameterType parseParameterType(const std::string& name, const std::string& context) {
    if (name == "float") {
        return ParameterType::Float;
    }
    if (name == "boolean") {
        return ParameterType::Boolean;
    }
    if (name == "integer") {
        return ParameterType::Integer;
    }
    if (name == "string") {
        return ParameterType::String;
    }
    if (name == "vector2") {
        return ParameterType::Vector2;
    }
    if (name == "color") {
        return ParameterType::Color;
    }
    if (name == "choice") {
        return ParameterType::Choice;
    }
    fail(context, "unsupported parameter type '" + name + "'");
}

template <std::size_t N>
[[nodiscard]] ParameterValue vectorDefault(const nlohmann::json& value, const std::string& context) {
    if (!value.is_array() || value.size() != N) {
        fail(context, "'default' must be an array of " + std::to_string(N) + " numbers");
    }
    std::array<float, N> components{};
    for (std::size_t index = 0; index < N; ++index) {
        const nlohmann::json& element = value.at(index);
        if (!element.is_number()) {
            fail(context, "'default' components must be numbers");
        }
        components[index] = element.get<float>();
    }
    if constexpr (N == 2) {
        return Vector2Value{components};
    } else if constexpr (N == 3) {
        return Vector3Value{components};
    } else {
        return ColorValue{components};
    }
}

// One parameter's default as the PLAIN JSON value the manifest authors: a
// boolean, an integer, a number, a string or an array. The typed envelope the
// document layer stores is a host representation and never appears here.
[[nodiscard]] ParameterValue plainDefault(const nlohmann::json& value, ParameterType type, const std::string& context) {
    switch (type) {
    case ParameterType::Boolean:
        if (!value.is_boolean()) {
            fail(context, "'default' must be a boolean");
        }
        return value.get<bool>();
    case ParameterType::Integer: {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                fail(context, "'default' is outside the integer range");
            }
            return static_cast<std::int64_t>(number);
        }
        if (!value.is_number_integer()) {
            fail(context, "'default' must be an integer");
        }
        return value.get<std::int64_t>();
    }
    case ParameterType::Float:
        if (!value.is_number()) {
            fail(context, "'default' must be a number");
        }
        return value.get<double>();
    case ParameterType::String:
        if (!value.is_string()) {
            fail(context, "'default' must be a string");
        }
        return value.get<std::string>();
    case ParameterType::Choice:
        if (!value.is_string()) {
            fail(context, "'default' must name a declared choice");
        }
        return ChoiceValue{value.get<std::string>()};
    case ParameterType::Vector2:
        return vectorDefault<2>(value, context);
    case ParameterType::Vector3:
        return vectorDefault<3>(value, context);
    case ParameterType::Color:
        return vectorDefault<4>(value, context);
    }
    fail(context, "unsupported parameter type");
}

[[nodiscard]] ParameterSpec parseParameter(const nlohmann::json& entry, const std::string& context) {
    if (!entry.is_object()) {
        fail(context, "every parameter must be an object");
    }
    rejectUnknownKeys(entry,
                      {"name", "type", "default", "minimum", "maximum", "softMinimum", "softMaximum", "step", "label",
                       "section", "editor", "displayDecimals", "choices", "row"},
                      context);
    ParameterSpec parameter;
    parameter.name = requiredString(entry, "name", context);
    const std::string parameterContext = context + " parameter '" + parameter.name + "'";
    parameter.type = parseParameterType(requiredString(entry, "type", parameterContext), parameterContext);
    const auto defaultValue = entry.find("default");
    if (defaultValue == entry.end()) {
        fail(parameterContext, "'default' is required");
    }
    parameter.defaultValue = plainDefault(*defaultValue, parameter.type, parameterContext);
    parameter.minimum = optionalNumber(entry, "minimum", parameterContext);
    parameter.maximum = optionalNumber(entry, "maximum", parameterContext);
    parameter.softMinimum = optionalNumber(entry, "softMinimum", parameterContext);
    parameter.softMaximum = optionalNumber(entry, "softMaximum", parameterContext);
    parameter.step = optionalNumber(entry, "step", parameterContext);
    parameter.displayDecimals = optionalInteger(entry, "displayDecimals", parameterContext);
    parameter.label = optionalString(entry, "label", parameterContext).value_or(std::string{});
    parameter.section = optionalString(entry, "section", parameterContext).value_or(std::string{});
    parameter.editor = optionalString(entry, "editor", parameterContext).value_or(std::string{});
    parameter.row = optionalString(entry, "row", parameterContext).value_or(std::string{});
    if (const nlohmann::json* choices = optionalMember(entry, "choices")) {
        if (!choices->is_array()) {
            fail(parameterContext, "'choices' must be an array");
        }
        for (const nlohmann::json& choice : *choices) {
            if (!choice.is_string()) {
                fail(parameterContext, "'choices' entries must be strings");
            }
            parameter.choices.push_back(choice.get<std::string>());
        }
    }
    return parameter;
}

struct ParsedManifest {
    PackageRecord record;
    std::vector<std::string> dependencies;
    std::vector<PanelContribution> panels;
    std::string library;  // absolute path of the package's library
};

[[nodiscard]] ParsedManifest parseManifest(const fs::path& root, const std::string& text) {
    ParsedManifest parsed;
    std::string context = "installed package at '" + pathText(root) + "'";
    const nlohmann::json manifest = nlohmann::json::parse(text, nullptr, false);
    if (manifest.is_discarded()) {
        fail(context, "manifest.json is not valid JSON");
    }
    if (!manifest.is_object()) {
        fail(context, "manifest.json must contain an object");
    }
    rejectUnknownKeys(manifest,
                      {"format", "id", "version", "stateVersion", "processingVersion", "api", "dependencies",
                       "capabilities", "library", "node", "gpu", "editors", "panels"},
                      context);

    const std::uint64_t format = requiredUnsigned(manifest, "format", context);
    if (format != kManifestFormat) {
        fail(context, "manifest format " + std::to_string(format) +
                          " is not supported by this build (this build "
                          "reads format " +
                          std::to_string(kManifestFormat) + ")");
    }
    parsed.record.id = requiredString(manifest, "id", context);
    if (!isNamespacedIdentifier(parsed.record.id)) {
        fail(context, "package id '" + parsed.record.id +
                          "' must be namespaced and free of whitespace or control "
                          "characters");
    }
    context = "installed package '" + parsed.record.id + "' at '" + pathText(root) + "'";
    parsed.record.version = requiredUnsigned(manifest, "version", context);
    parsed.record.stateVersion = requiredUnsigned(manifest, "stateVersion", context);
    parsed.record.processingVersion = requiredUnsigned(manifest, "processingVersion", context);
    if (parsed.record.version == 0 || parsed.record.stateVersion == 0 || parsed.record.processingVersion == 0) {
        fail(context, "'version', 'stateVersion' and 'processingVersion' must be positive");
    }

    const nlohmann::json& api = requiredObject(manifest, "api", context);
    rejectUnknownKeys(api, {"minimum", "maximum"}, context + " api");
    const std::uint64_t minimum = requiredUnsigned(api, "minimum", context + " api");
    const std::uint64_t maximum = requiredUnsigned(api, "maximum", context + " api");
    if (minimum == 0 || minimum > maximum || NEMO_EFFECT_ABI_VERSION < minimum || NEMO_EFFECT_ABI_VERSION > maximum) {
        fail(context, "declares effect API range [" + std::to_string(minimum) + ", " + std::to_string(maximum) +
                          "]; this build implements ABI " + std::to_string(NEMO_EFFECT_ABI_VERSION));
    }

    const nlohmann::json& dependencies = requiredArray(manifest, "dependencies", context);
    for (const nlohmann::json& dependency : dependencies) {
        if (!dependency.is_string()) {
            fail(context, "'dependencies' entries must be package ids");
        }
        const std::string id = dependency.get<std::string>();
        if (!isNamespacedIdentifier(id)) {
            fail(context, "dependency '" + id + "' is not a namespaced package id");
        }
        if (id == parsed.record.id) {
            fail(context, "declares itself as a dependency");
        }
        parsed.dependencies.push_back(id);
    }
    {
        std::set<std::string, std::less<>> declared;
        for (const std::string& dependency : parsed.dependencies) {
            if (!declared.insert(dependency).second) {
                fail(context, "declares duplicate dependency '" + dependency + "'");
            }
        }
    }

    const nlohmann::json& capabilities = requiredArray(manifest, "capabilities", context);
    std::set<std::string, std::less<>> declaredCapabilities;
    for (const nlohmann::json& capability : capabilities) {
        if (!capability.is_string()) {
            fail(context, "'capabilities' entries must be strings");
        }
        const std::string name = capability.get<std::string>();
        if (name != kPointwiseCapability && name != kQmlCapability) {
            fail(context, "declares unsupported capability '" + name + "'");
        }
        if (!declaredCapabilities.insert(name).second) {
            fail(context, "declares duplicate capability '" + name + "'");
        }
    }
    if (!declaredCapabilities.contains(std::string{kPointwiseCapability})) {
        fail(context, "must declare the '" + std::string{kPointwiseCapability} + "' capability");
    }

    std::string reason;
    if (!resolvePackageFile(root, requiredString(manifest, "library", context), parsed.library, reason)) {
        fail(context, reason);
    }

    const nlohmann::json& node = requiredObject(manifest, "node", context);
    const std::string nodeContext = context + " node";
    rejectUnknownKeys(node, {"type", "displayName", "group", "parameters"}, nodeContext);
    NodeDescriptor& descriptor = parsed.record.descriptor;
    descriptor.type = requiredString(node, "type", nodeContext);
    if (!isNamespacedIdentifier(descriptor.type)) {
        fail(nodeContext, "type '" + descriptor.type +
                              "' must be namespaced and free of whitespace or control "
                              "characters");
    }
    descriptor.displayName = optionalString(node, "displayName", nodeContext).value_or(descriptor.type);
    descriptor.group = optionalString(node, "group", nodeContext).value_or(std::string{});
    const nlohmann::json* parameters = optionalMember(node, "parameters");
    if (parameters == nullptr) {
        fail(nodeContext, "'parameters' is required");
    }
    if (!parameters->is_array()) {
        fail(nodeContext, "'parameters' must be an array");
    }
    for (const nlohmann::json& parameter : *parameters) {
        descriptor.parameters.push_back(parseParameter(parameter, nodeContext));
    }
    std::set<std::string, std::less<>> parameterNames;
    for (const ParameterSpec& parameter : descriptor.parameters) {
        parameterNames.insert(parameter.name);
    }

    // The declared effect schema this ABI defines: one required image input and
    // one image output. A pointwise package has no mask port (the ABI carries no
    // mask word) and declares the same execution capabilities the built-in
    // effects do — every sampling scale, full quality, any named channel,
    // regional work, no time.
    descriptor.inputs = {{PortKind::Image, "image", false}};
    descriptor.mainInput = 0;
    descriptor.outputs = {{PortKind::Image, "out", false}};
    descriptor.capabilities = NodeCapabilities{.samplingScales = {1, 2, 4},
                                               .qualityModes = {Quality::Full},
                                               .channels = {std::string{kAnyChannelCapability}},
                                               .supportsRegion = true,
                                               .temporal = false};
    parsed.record.stateIdentity = parsed.record.id + ".state." + std::to_string(parsed.record.stateVersion);
    descriptor.stateIdentity = parsed.record.stateIdentity;
    parsed.record.implementationVersion = packageImplementationVersion(
        parsed.record.stateIdentity, parsed.record.version, parsed.record.processingVersion);
    descriptor.implementationVersion = parsed.record.implementationVersion;

    if (const nlohmann::json* editors = optionalMember(manifest, "editors")) {
        if (!editors->is_array()) {
            fail(context, "'editors' must be an array");
        }
        std::set<std::string, std::less<>> declaredEditors;
        for (const nlohmann::json& editor : *editors) {
            if (!editor.is_object()) {
                fail(context, "every editor must be an object");
            }
            rejectUnknownKeys(editor, {"id", "source", "presentation", "consumes"}, context + " editor");
            NodeEditorContribution contribution;
            contribution.id = requiredString(editor, "id", context + " editor");
            if (!isNamespacedIdentifier(contribution.id)) {
                fail(context, "editor id '" + contribution.id +
                                  "' must be namespaced and free of whitespace or "
                                  "control characters");
            }
            if (!declaredEditors.insert(contribution.id).second) {
                fail(context, "declares duplicate editor '" + contribution.id + "'");
            }
            const std::string editorContext = context + " editor '" + contribution.id + "'";
            std::string source;
            if (!resolvePackageFile(root, requiredString(editor, "source", editorContext), source, reason)) {
                fail(editorContext, reason);
            }
            contribution.source = fileUrl(source);
            contribution.presentation = optionalString(editor, "presentation", editorContext).value_or("row");
            if (contribution.presentation != "row" && contribution.presentation != "section") {
                fail(editorContext, "declares unsupported presentation '" + contribution.presentation + "'");
            }
            if (const nlohmann::json* consumes = optionalMember(editor, "consumes")) {
                if (!consumes->is_array()) {
                    fail(editorContext, "'consumes' must be an array of declared parameter keys");
                }
                std::set<std::string, std::less<>> consumed;
                for (const nlohmann::json& key : *consumes) {
                    if (!key.is_string()) {
                        fail(editorContext, "'consumes' entries must be parameter keys");
                    }
                    const std::string name = key.get<std::string>();
                    if (name.empty() || !consumed.insert(name).second) {
                        fail(editorContext, "consumes an empty or duplicate parameter key");
                    }
                    if (!parameterNames.contains(name)) {
                        fail(editorContext, "consumes undeclared parameter '" + name + "'");
                    }
                    contribution.consumes.push_back(name);
                }
            }
            parsed.record.editors.push_back(std::move(contribution));
        }
    }

    if (const nlohmann::json* panels = optionalMember(manifest, "panels")) {
        if (!panels->is_array()) {
            fail(context, "'panels' must be an array");
        }
        std::set<std::string, std::less<>> declaredPanels;
        for (const nlohmann::json& panel : *panels) {
            if (!panel.is_object()) {
                fail(context, "every panel must be an object");
            }
            rejectUnknownKeys(panel, {"id", "title", "source"}, context + " panel");
            PanelContribution contribution;
            contribution.id = requiredString(panel, "id", context + " panel");
            if (!isNamespacedIdentifier(contribution.id)) {
                fail(context, "panel id '" + contribution.id +
                                  "' must be namespaced and free of whitespace or "
                                  "control characters");
            }
            if (!declaredPanels.insert(contribution.id).second) {
                fail(context, "declares duplicate panel '" + contribution.id + "'");
            }
            const std::string panelContext = context + " panel '" + contribution.id + "'";
            contribution.title = requiredString(panel, "title", panelContext);
            std::string source;
            if (!resolvePackageFile(root, requiredString(panel, "source", panelContext), source, reason)) {
                fail(panelContext, reason);
            }
            contribution.source = fileUrl(source);
            parsed.panels.push_back(std::move(contribution));
        }
    }
    if ((!parsed.record.editors.empty() || !parsed.panels.empty()) &&
        !declaredCapabilities.contains(std::string{kQmlCapability})) {
        fail(context, "declares editors or panels without the '" + std::string{kQmlCapability} + "' capability");
    }

    if (const nlohmann::json* gpu = optionalMember(manifest, "gpu")) {
        if (!gpu->is_object()) {
            fail(context, "'gpu' must be an object");
        }
        const std::string gpuContext = context + " gpu";
        rejectUnknownKeys(*gpu, {"bindings", "payloadBytes", "payloadLayout", "spirv", "glsl"}, gpuContext);
        PackageGpu declared;
        const std::string bindings = requiredString(*gpu, "bindings", gpuContext);
        if (bindings != eval::kEffectBindingContractVersion) {
            fail(gpuContext, "binding contract '" + bindings + "' is unsupported; expected '" +
                                 std::string{eval::kEffectBindingContractVersion} + "'");
        }
        const std::uint64_t payloadBytes = requiredUnsigned(*gpu, "payloadBytes", gpuContext);
        if (payloadBytes == 0 || payloadBytes % 16 != 0 ||
            payloadBytes > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(gpuContext, "'payloadBytes' must be a positive multiple of 16 within the 32-bit payload range");
        }
        declared.payloadBytes = static_cast<std::uint32_t>(payloadBytes);
        declared.payloadLayout = requiredString(*gpu, "payloadLayout", gpuContext);
        if (declared.payloadLayout.find('.') == std::string::npos) {
            fail(gpuContext, "'payloadLayout' must be a namespaced layout identity");
        }
        std::string spirv;
        if (!resolvePackageFile(root, requiredString(*gpu, "spirv", gpuContext), spirv, reason)) {
            fail(gpuContext, reason);
        }
        declared.spirv = pathFromText(spirv);
        if (!isSpirvModule(declared.spirv))
            fail(gpuContext, "the declared SPIR-V file '" + spirv + "' has an invalid module header");
        std::string glsl;
        if (!resolvePackageFile(root, requiredString(*gpu, "glsl", gpuContext), glsl, reason)) {
            fail(gpuContext, reason);
        }
        if (!readTextFile(glsl, declared.glsl) || declared.glsl.empty()) {
            fail(gpuContext, "the declared GLSL reference shader '" + glsl + "' is not a complete shader");
        }
        parsed.record.gpu = std::move(declared);
    }
    return parsed;
}

[[nodiscard]] ParsedManifest parseManifestFile(const fs::path& root) {
    std::string text;
    if (!readTextFile(pathText((root / kManifestFile)), text)) {
        fail("installed package at '" + pathText(root) + "'", "manifest.json could not be read");
    }
    return parseManifest(root, text);
}

}  // namespace

namespace detail {

namespace {

// Effective parameters as the plain JSON object the ABI carries: the host's
// typed envelope (tag plus payload) is a document representation and never
// crosses the boundary.
[[nodiscard]] nlohmann::json plainParameterValue(const ParameterValue& value) {
    return std::visit(
        [](const auto& typed) -> nlohmann::json {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, bool>) {
                return typed;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return typed;
            } else if constexpr (std::is_same_v<T, double>) {
                return typed;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return typed;
            } else if constexpr (std::is_same_v<T, ChoiceValue>) {
                return typed.value;
            } else {
                return std::vector<float>(typed.value.begin(), typed.value.end());
            }
        },
        value);
}

// Writes the package's interleaved RGBA result into the produced raster's own
// roles. A raster whose described channels identify no role of a component
// simply does not store it, and the shared auxiliary-channel preservation rule
// then keeps every named channel the effect does not address.
void storeRgba(CpuImage& output, const std::vector<float>& rgba, int width, int height) {
    const std::array<int, kImageChannels>& roles = output.rgbaIndices();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float* pixel = rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                static_cast<std::size_t>(x)) *
                                                   kImageChannels;
            for (std::size_t role = 0; role < roles.size(); ++role) {
                if (roles[role] >= 0) {
                    output.setChannel(x, y, roles[role], pixel[role]);
                }
            }
        }
    }
}

// One bulk CPU invocation: the effect's real input geometry (the producer's
// raster may be larger than this node's own raster, and a sample the raster does
// not hold is transparent black), the input's channel roles (an arbitrary
// channel set is projected onto RGBA by name, never by storage order) and one
// interleaved RGBA buffer pair for the whole raster. The package is called
// once per raster, never once per pixel, and the host owns both buffers.
[[nodiscard]] CpuImage executePackage(const std::shared_ptr<const SharedLibrary>& library, const std::string& id,
                                      std::uint32_t mainInput, const CpuNodeContext& context) {
    const NemoEffectV1* entry = library->entry();
    const CpuImage& input = requiredImageInput(context, mainInput, "the installed effect has no input image");
    const InputAnchor anchor = anchorInput(context, mainInput, input);
    CpuImage output(effectRasterLayout(context));
    const int width = output.width();
    const int height = output.height();
    if (width <= 0 || height <= 0) {
        return output;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> source(pixels * kImageChannels, 0.0F);
    std::vector<float> result(pixels * kImageChannels, 0.0F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, kImageChannels> pixel = sampledPixel(input, anchor.offsetX + x, anchor.offsetY + y);
            float* target = source.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                             static_cast<std::size_t>(x)) *
                                                kImageChannels;
            std::copy(pixel.begin(), pixel.end(), target);
        }
    }
    const ImageDescription* described =
        mainInput < context.inputDescriptions.size() ? context.inputDescriptions[mainInput] : nullptr;
    const ImageAssociation association =
        described != nullptr ? described->association : context.description.association;
    const std::span<const std::string> channels = described != nullptr
                                                      ? std::span<const std::string>{described->channels}
                                                      : std::span<const std::string>{input.layout().channels};
    const ColorInterpretation color = described != nullptr ? described->color : input.layout().color;
    const std::uint32_t flags = effectFlags(association, color, hasPrimaryRgb(channels));
    const std::string parameters = parametersJson(context.effectiveParams);
    std::array<char, kDiagnosticCapacity> error{};
    const int status =
        entry->process(parameters.c_str(), source.data(), result.data(), static_cast<std::uint64_t>(pixels), flags,
                       error.data(), static_cast<std::uint32_t>(error.size()));
    error.back() = '\0';
    if (status != 0) {
        failNode(context.node, "installed package '" + id + "' refused these pixels: " + diagnosticText(error.data()));
    }
    storeRgba(output, result, width, height);
    return output;
}

// The generic authoring validation seam: the package's own admissibility rule
// for one resolved parameter set (defaults, instance overrides and animation
// already folded in), so an editor gesture and the evaluator refuse exactly the
// same states. A package without a rule returns "admissible".
[[nodiscard]] std::optional<std::string> validatePackageParameters(const std::shared_ptr<const SharedLibrary>& library,
                                                                   const std::string& id,
                                                                   const ParameterValues& effectiveParams) {
    const NemoEffectV1* entry = library->entry();
    const std::string parameters = parametersJson(effectiveParams);
    std::array<char, kDiagnosticCapacity> error{};
    const int status = entry->validate(parameters.c_str(), error.data(), static_cast<std::uint32_t>(error.size()));
    error.back() = '\0';
    if (status == 0) {
        return std::nullopt;
    }
    return "installed package '" + id + "' refused these parameters: " + diagnosticText(error.data());
}

}  // namespace

void LibraryCloser::operator()(void* handle) const noexcept {
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

std::shared_ptr<const SharedLibrary> SharedLibrary::open(const fs::path& library, bool needsPrepare,
                                                         std::string& error) {
#if defined(_WIN32)
    NativeLibraryHandle handle{::LoadLibraryExW(library.c_str(), nullptr,
                                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)};
    if (!handle) {
        error = "cannot load '" + pathText(library) + "': Windows error " + std::to_string(::GetLastError());
        return nullptr;
    }
    const auto symbol = ::GetProcAddress(static_cast<HMODULE>(handle.get()), "nemo_effect_v1");
#else
    NativeLibraryHandle handle{::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL)};
    if (!handle) {
        const char* message = ::dlerror();
        error = "cannot load '" + pathText(library) +
                "': " + (message != nullptr ? std::string{message} : std::string{"unknown dlopen failure"});
        return nullptr;
    }
    ::dlerror();
    void* symbol = ::dlsym(handle.get(), "nemo_effect_v1");
#endif
    if (symbol == nullptr) {
        error = "exposes no 'nemo_effect_v1' entrypoint";
        return nullptr;
    }
    const auto entry = reinterpret_cast<NemoEffectEntryV1>(symbol);
    const NemoEffectV1* effect = entry();
    if (effect == nullptr) {
        error = "returned no effect metadata from 'nemo_effect_v1'";
        return nullptr;
    }
    if (effect->abi_version != NEMO_EFFECT_ABI_VERSION) {
        error = "declares effect ABI version " + std::to_string(effect->abi_version) +
                " at the entrypoint, but its manifest was validated for ABI " + std::to_string(NEMO_EFFECT_ABI_VERSION);
        return nullptr;
    }
    if (effect->struct_size < sizeof(NemoEffectV1)) {
        error = "declares an effect structure of " + std::to_string(effect->struct_size) + " bytes, smaller than ABI " +
                std::to_string(NEMO_EFFECT_ABI_VERSION) + "'s " + std::to_string(sizeof(NemoEffectV1));
        return nullptr;
    }
    if (effect->validate == nullptr || effect->process == nullptr) {
        error = "does not supply the validate and process callbacks the declared capabilities require";
        return nullptr;
    }
    if (needsPrepare && effect->prepare == nullptr) {
        error = "declares a GPU implementation but supplies no prepare callback";
        return nullptr;
    }
    return std::make_shared<SharedLibrary>(std::move(handle), effect);
}

std::string parametersJson(const ParameterValues& params) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : params) {
        object[key] = plainParameterValue(value);
    }
    return object.dump();
}

NodeContribution packageContribution(const PackageRecord& record) {
    const std::shared_ptr<const SharedLibrary> library = record.library;
    NodeContribution contribution;
    contribution.descriptor = record.descriptor;
    contribution.role = NodeRole::Image;
    contribution.nativeGpu = record.gpu.has_value();
    contribution.ownsChannelLayout = false;
    contribution.editors = record.editors;
    CpuImplementation implementation;
    implementation.version = record.implementationVersion;
    implementation.execute = [library, id = record.id,
                              mainInput = record.descriptor.mainInput](const CpuNodeContext& context) {
        return executePackage(library, id, mainInput, context);
    };
    contribution.cpu = std::move(implementation);
    contribution.validateParameters = [library, id = record.id](const NodeCatalog&, const NodeInstance&,
                                                                const ParameterValues& effectiveParams) {
        return validatePackageParameters(library, id, effectiveParams);
    };
    return contribution;
}

}  // namespace detail

std::vector<std::filesystem::path> installedPackageRoots() {
    std::vector<fs::path> roots;
#if defined(_WIN32)
    const wchar_t* configured = _wgetenv(L"NEMO_EXTENSION_PATH");
    constexpr wchar_t separator = L';';
#else
    const char* configured = std::getenv("NEMO_EXTENSION_PATH");
    constexpr char separator = ':';
#endif
    if (configured != nullptr) {
        const std::basic_string_view list{configured};
        std::size_t begin = 0;
        while (begin <= list.size()) {
            const std::size_t end = list.find(separator, begin);
            const auto entry = list.substr(begin, end == decltype(list)::npos ? end : end - begin);
            if (!entry.empty()) {
                roots.emplace_back(entry);
            }
            if (end == decltype(list)::npos) {
                break;
            }
            begin = end + 1;
        }
        if (!roots.empty()) {
            return roots;
        }
    }
#if defined(_WIN32)
    if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData != nullptr && *localAppData != L'\0') {
        const fs::path base{localAppData};
        if (base.is_absolute())
            roots.push_back(base / "Nemo" / "extensions");
    }
#else
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
        const fs::path base{dataHome};
        if (base.is_absolute()) {
            roots.push_back(base / "nemo" / "extensions");
            return roots;
        }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const fs::path base{home};
        if (base.is_absolute()) {
            roots.push_back(base / ".local" / "share" / "nemo" / "extensions");
        }
    }
#endif
    return roots;
}

InstalledPackages::InstalledPackages(std::vector<fs::path> roots) {
    // Every discovered package folder keeps one slot, in discovery order, so a
    // package whose manifest cannot even be parsed is still reported where it
    // was found instead of being appended after the packages that were refused
    // later.
    struct Candidate {
        fs::path directory;
        std::optional<ParsedManifest> manifest;
        bool refused{false};
        std::string reason;
    };
    std::vector<Candidate> candidates;

    // Discovery: the configured roots in order, each root's child package
    // folders by name. A child folder is a package only when it carries a
    // manifest; a root that is not an existing directory is skipped, and the
    // working directory is never searched implicitly.
    for (const fs::path& root : roots) {
        std::error_code error;
        if (!fs::is_directory(root, error)) {
            continue;
        }
        std::vector<fs::path> children;
        fs::directory_iterator iterator(root, error);
        const fs::directory_iterator end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code entryError;
            if (iterator->is_directory(entryError)) {
                children.push_back(iterator->path());
            }
        }
        std::sort(children.begin(), children.end());
        for (const fs::path& child : children) {
            std::error_code manifestError;
            if (!fs::is_regular_file(child / kManifestFile, manifestError)) {
                continue;
            }
            fs::path directory = child;
            const fs::path canonical = fs::weakly_canonical(child, manifestError);
            if (!manifestError) {
                directory = canonical;
            }
            try {
                candidates.push_back(Candidate{directory, parseManifestFile(directory), false, std::string{}});
            } catch (const std::exception& failure) {
                candidates.push_back(Candidate{directory, std::nullopt, true, failure.what()});
            }
        }
    }

    const auto refuse = [&candidates](std::size_t index, const std::string& reason) {
        if (!candidates[index].refused) {
            candidates[index].refused = true;
            candidates[index].reason = reason;
        }
    };
    const auto describe = [&candidates](std::size_t index) {
        const std::string identity =
            candidates[index].manifest ? "'" + candidates[index].manifest->record.id + "' at " : "at ";
        return "installed package " + identity + "'" + pathText(candidates[index].directory) + "'";
    };

    // Duplicate identities refuse every offender: which package wins is never
    // decided by discovery order. A package identity, a node type (including a
    // built-in type), an editor identity and a panel identity are all unique
    // across the installed set.
    std::map<std::string, std::vector<std::size_t>, std::less<>> byPackageId;
    std::map<std::string, std::vector<std::size_t>, std::less<>> byNodeType;
    std::map<std::string, std::vector<std::size_t>, std::less<>> byEditorId;
    std::map<std::string, std::vector<std::size_t>, std::less<>> byPanelId;
    std::set<std::string, std::less<>> builtinTypes;
    std::set<std::string, std::less<>> builtinEditors;
    std::vector<NodeContribution> contributions = builtinContributions();
    for (const NodeContribution& contribution : contributions) {
        builtinTypes.insert(contribution.descriptor.type);
        for (const NodeEditorContribution& editor : contribution.editors)
            builtinEditors.insert(editor.id);
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].manifest) {
            continue;
        }
        const PackageRecord& record = candidates[index].manifest->record;
        byPackageId[record.id].push_back(index);
        byNodeType[record.descriptor.type].push_back(index);
        for (const NodeEditorContribution& editor : record.editors) {
            byEditorId[editor.id].push_back(index);
        }
        for (const PanelContribution& panel : candidates[index].manifest->panels) {
            byPanelId[panel.id].push_back(index);
        }
    }
    for (const auto& [id, group] : byPackageId) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuse(index, describe(index) + ": duplicate package identity '" + id + "'");
            }
        }
    }
    for (const auto& [type, group] : byNodeType) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuse(index, describe(index) + ": duplicate node type '" + type + "'");
            }
        }
        if (builtinTypes.contains(type)) {
            for (const std::size_t index : group) {
                refuse(index, describe(index) + ": node type '" + type + "' is already declared by this build");
            }
        }
    }
    for (const auto& [id, group] : byEditorId) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuse(index, describe(index) + ": duplicate editor identity '" + id + "'");
            }
        }
        if (builtinEditors.contains(id)) {
            for (const std::size_t index : group)
                refuse(index, describe(index) + ": editor identity '" + id + "' is already declared by this build");
        }
    }
    for (const auto& [id, group] : byPanelId) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuse(index, describe(index) + ": duplicate panel identity '" + id + "'");
            }
        }
    }

    // Schema validation: the descriptor the manifest declares must be a legal
    // node schema on its own, before any library is opened.
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].refused) {
            continue;
        }
        try {
            const std::vector<NodeDescriptor> declared{candidates[index].manifest->record.descriptor};
            const NodeCatalog catalog(declared);
            (void)catalog;
        } catch (const std::invalid_argument& error) {
            refuse(index, describe(index) + ": " + error.what());
        }
    }

    std::map<std::string, std::size_t, std::less<>> packageIndex;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].manifest) {
            packageIndex.emplace(candidates[index].manifest->record.id, index);
        }
    }
    // A refused package refuses everything that depends on it, transitively.
    const auto propagate = [&candidates, &packageIndex, &refuse, &describe]() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                if (candidates[index].refused) {
                    continue;
                }
                for (const std::string& dependency : candidates[index].manifest->dependencies) {
                    const auto found = packageIndex.find(dependency);
                    if (found != packageIndex.end() && candidates[found->second].refused) {
                        refuse(index, describe(index) + ": depends on refused package '" + dependency + "'");
                        changed = true;
                        break;
                    }
                }
            }
        }
    };
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].refused) {
            continue;
        }
        for (const std::string& dependency : candidates[index].manifest->dependencies) {
            const auto found = packageIndex.find(dependency);
            if (found == packageIndex.end()) {
                refuse(index, describe(index) + ": dependency '" + dependency + "' is not installed");
                break;
            }
        }
    }
    propagate();

    // Resolve the entire dependency order before executing any native code.
    // Kahn's residual nodes lie on, or depend on, a cycle; neither can activate.
    std::vector<std::vector<std::size_t>> dependents(candidates.size());
    std::vector<std::size_t> remaining(candidates.size(), 0);
    std::vector<std::size_t> activationOrder;
    activationOrder.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].refused)
            continue;
        for (const std::string& dependency : candidates[index].manifest->dependencies) {
            dependents[packageIndex.at(dependency)].push_back(index);
            ++remaining[index];
        }
        if (remaining[index] == 0)
            activationOrder.push_back(index);
    }
    for (std::size_t position = 0; position < activationOrder.size(); ++position) {
        for (const std::size_t dependent : dependents[activationOrder[position]]) {
            if (--remaining[dependent] == 0)
                activationOrder.push_back(dependent);
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].refused || remaining[index] == 0)
            continue;
        std::string involved;
        for (const std::string& dependency : candidates[index].manifest->dependencies) {
            if (remaining[packageIndex.at(dependency)] != 0) {
                if (!involved.empty())
                    involved += ", ";
                involved += "'" + dependency + "'";
            }
        }
        refuse(index, describe(index) + ": dependency cycle through " + involved);
    }

    std::vector<std::shared_ptr<const SharedLibrary>> libraries(candidates.size());
    for (const std::size_t index : activationOrder) {
        ParsedManifest& manifest = *candidates[index].manifest;
        for (const std::string& dependency : manifest.dependencies) {
            if (candidates[packageIndex.at(dependency)].refused) {
                refuse(index, describe(index) + ": depends on refused package '" + dependency + "'");
                break;
            }
        }
        if (candidates[index].refused)
            continue;
        std::string reason;
        libraries[index] = SharedLibrary::open(pathFromText(manifest.library), manifest.record.gpu.has_value(), reason);
        if (!libraries[index]) {
            refuse(index, describe(index) + ": " + reason);
            continue;
        }
        ParameterValues defaults;
        for (const ParameterSpec& parameter : manifest.record.descriptor.parameters)
            defaults.emplace(parameter.name, parameter.defaultValue);
        if (const auto problem = detail::validatePackageParameters(libraries[index], manifest.record.id, defaults)) {
            refuse(index, describe(index) + ": invalid default parameters: " + *problem);
            libraries[index].reset();
        }
    }

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].refused) {
            continue;
        }
        ParsedManifest& manifest = *candidates[index].manifest;
        manifest.record.library = libraries[index];
        for (PanelContribution& panel : manifest.panels) {
            panels_.push_back(std::move(panel));
        }
        records_.push_back(std::move(manifest.record));
    }
    for (const Candidate& candidate : candidates) {
        if (candidate.refused) {
            diagnostics_.push_back(candidate.reason);
        }
    }

    contributions.reserve(contributions.size() + records_.size());
    for (const PackageRecord& record : records_) {
        contributions.push_back(detail::packageContribution(record));
    }
    contributions_ = std::make_shared<const NodeContributions>(std::move(contributions));
}

}  // namespace nemo::extensions
