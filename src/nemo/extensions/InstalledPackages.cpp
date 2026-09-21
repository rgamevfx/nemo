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

// A refused manifest carries the state the Settings surface shows for it, so a
// presentation layer never parses diagnostic text.
class ManifestRefusal : public std::runtime_error {
public:
    ManifestRefusal(PackageStatus status, const std::string& message) : std::runtime_error(message), status_(status) {}

    [[nodiscard]] PackageStatus status() const noexcept { return status_; }

private:
    PackageStatus status_;
};

// The declaration is not supported by this build: manifest format, effect API
// range, capability, declared file, node schema or GPU binding contract.
[[noreturn]] void fail(const std::string& context, const std::string& reason) {
    throw ManifestRefusal{PackageStatus::Incompatible, context + ": " + reason};
}

// manifest.json is not a valid declaration at all: unreadable, not JSON, not an
// object, a wrong value type, an unknown key.
[[noreturn]] void malformed(const std::string& context, const std::string& reason) {
    throw ManifestRefusal{PackageStatus::MalformedManifest, context + ": " + reason};
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
            malformed(context, "unknown key '" + entry.key() + "'");
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
        malformed(context, std::string("'") + key + "' is required");
    }
    if (!entry->is_object()) {
        malformed(context, std::string("'") + key + "' must be an object");
    }
    return *entry;
}

[[nodiscard]] const nlohmann::json& requiredArray(const nlohmann::json& parent, const char* key,
                                                  const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        malformed(context, std::string("'") + key + "' is required");
    }
    if (!entry->is_array()) {
        malformed(context, std::string("'") + key + "' must be an array");
    }
    return *entry;
}

[[nodiscard]] std::string requiredString(const nlohmann::json& parent, const char* key, const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        malformed(context, std::string("'") + key + "' is required");
    }
    if (!entry->is_string()) {
        malformed(context, std::string("'") + key + "' must be a string");
    }
    const std::string value = entry->get<std::string>();
    if (value.empty()) {
        malformed(context, std::string("'") + key + "' must not be empty");
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
        malformed(context, std::string("'") + key + "' must be a string");
    }
    return entry->get<std::string>();
}

[[nodiscard]] std::uint64_t requiredUnsigned(const nlohmann::json& parent, const char* key,
                                             const std::string& context) {
    const auto entry = parent.find(key);
    if (entry == parent.end()) {
        malformed(context, std::string("'") + key + "' is required");
    }
    if (entry->is_number_unsigned()) {
        return entry->get<std::uint64_t>();
    }
    if (entry->is_number_integer()) {
        const auto value = entry->get<std::int64_t>();
        if (value < 0) {
            malformed(context, std::string("'") + key + "' must be a non-negative integer");
        }
        return static_cast<std::uint64_t>(value);
    }
    malformed(context, std::string("'") + key + "' must be a non-negative integer");
}

[[nodiscard]] std::optional<double> optionalNumber(const nlohmann::json& parent, const char* key,
                                                   const std::string& context) {
    const nlohmann::json* entry = optionalMember(parent, key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    if (!entry->is_number()) {
        malformed(context, std::string("'") + key + "' must be a number");
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
            malformed(context, std::string("'") + key + "' is outside the integer range");
        }
        return static_cast<int>(value);
    }
    if (!entry->is_number_integer()) {
        malformed(context, std::string("'") + key + "' must be an integer");
    }
    const auto value = entry->get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        malformed(context, std::string("'") + key + "' is outside the integer range");
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
        malformed(context, "'default' must be an array of " + std::to_string(N) + " numbers");
    }
    std::array<float, N> components{};
    for (std::size_t index = 0; index < N; ++index) {
        const nlohmann::json& element = value.at(index);
        if (!element.is_number()) {
            malformed(context, "'default' components must be numbers");
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
            malformed(context, "'default' must be a boolean");
        }
        return value.get<bool>();
    case ParameterType::Integer: {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                malformed(context, "'default' is outside the integer range");
            }
            return static_cast<std::int64_t>(number);
        }
        if (!value.is_number_integer()) {
            malformed(context, "'default' must be an integer");
        }
        return value.get<std::int64_t>();
    }
    case ParameterType::Float:
        if (!value.is_number()) {
            malformed(context, "'default' must be a number");
        }
        return value.get<double>();
    case ParameterType::String:
        if (!value.is_string()) {
            malformed(context, "'default' must be a string");
        }
        return value.get<std::string>();
    case ParameterType::Choice:
        if (!value.is_string()) {
            malformed(context, "'default' must name a declared choice");
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
        malformed(context, "every parameter must be an object");
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
        malformed(parameterContext, "'default' is required");
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
            malformed(parameterContext, "'choices' must be an array");
        }
        for (const nlohmann::json& choice : *choices) {
            if (!choice.is_string()) {
                malformed(parameterContext, "'choices' entries must be strings");
            }
            parameter.choices.push_back(choice.get<std::string>());
        }
    }
    return parameter;
}

struct ParsedManifest {
    PackageRecord record;
    // Declared presentation metadata. Empty means the manifest declared none:
    // the host never invents an author or a description for a package.
    std::string name;
    std::string author;
    std::string description;
    std::vector<std::string> dependencies;
    std::vector<PanelContribution> panels;
    std::string library;  // absolute path of the package's library
};

// Reads one declaration into `parsed`. The metadata is filled in as far as the
// declaration can be read, so a package refused later — an unsupported API
// range, a missing declared file, an unknown key — still reports the identity,
// name, version, description and contributions it did declare. A throw leaves
// exactly that partial metadata in `parsed`.
void parseManifest(const fs::path& root, const std::string& text, ParsedManifest& parsed) {
    std::string context = "installed package at '" + pathText(root) + "'";
    const nlohmann::json manifest = nlohmann::json::parse(text, nullptr, false);
    if (manifest.is_discarded()) {
        malformed(context, "manifest.json is not valid JSON");
    }
    if (!manifest.is_object()) {
        malformed(context, "manifest.json must contain an object");
    }
    rejectUnknownKeys(manifest,
                      {"format", "id", "name", "author", "description", "version", "stateVersion", "processingVersion",
                       "api", "dependencies", "capabilities", "library", "node", "gpu", "editors", "panels"},
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
        malformed(context, "package id '" + parsed.record.id +
                               "' must be namespaced and free of whitespace or control "
                               "characters");
    }
    context = "installed package '" + parsed.record.id + "' at '" + pathText(root) + "'";
    parsed.name = optionalString(manifest, "name", context).value_or(std::string{});
    parsed.author = optionalString(manifest, "author", context).value_or(std::string{});
    parsed.description = optionalString(manifest, "description", context).value_or(std::string{});
    parsed.record.version = requiredUnsigned(manifest, "version", context);
    parsed.record.stateVersion = requiredUnsigned(manifest, "stateVersion", context);
    parsed.record.processingVersion = requiredUnsigned(manifest, "processingVersion", context);
    if (parsed.record.version == 0 || parsed.record.stateVersion == 0 || parsed.record.processingVersion == 0) {
        malformed(context, "'version', 'stateVersion' and 'processingVersion' must be positive");
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
            malformed(context, "'dependencies' entries must be package ids");
        }
        const std::string id = dependency.get<std::string>();
        if (!isNamespacedIdentifier(id)) {
            malformed(context, "dependency '" + id + "' is not a namespaced package id");
        }
        if (id == parsed.record.id) {
            malformed(context, "declares itself as a dependency");
        }
        parsed.dependencies.push_back(id);
    }
    {
        std::set<std::string, std::less<>> declared;
        for (const std::string& dependency : parsed.dependencies) {
            if (!declared.insert(dependency).second) {
                malformed(context, "declares duplicate dependency '" + dependency + "'");
            }
        }
    }

    const nlohmann::json& capabilities = requiredArray(manifest, "capabilities", context);
    std::set<std::string, std::less<>> declaredCapabilities;
    for (const nlohmann::json& capability : capabilities) {
        if (!capability.is_string()) {
            malformed(context, "'capabilities' entries must be strings");
        }
        const std::string name = capability.get<std::string>();
        if (name != kPointwiseCapability && name != kQmlCapability) {
            fail(context, "declares unsupported capability '" + name + "'");
        }
        if (!declaredCapabilities.insert(name).second) {
            malformed(context, "declares duplicate capability '" + name + "'");
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
        malformed(nodeContext, "type '" + descriptor.type +
                                   "' must be namespaced and free of whitespace or control "
                                   "characters");
    }
    descriptor.displayName = optionalString(node, "displayName", nodeContext).value_or(descriptor.type);
    descriptor.group = optionalString(node, "group", nodeContext).value_or(std::string{});
    const nlohmann::json* parameters = optionalMember(node, "parameters");
    if (parameters == nullptr) {
        malformed(nodeContext, "'parameters' is required");
    }
    if (!parameters->is_array()) {
        malformed(nodeContext, "'parameters' must be an array");
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
            malformed(context, "'editors' must be an array");
        }
        std::set<std::string, std::less<>> declaredEditors;
        for (const nlohmann::json& editor : *editors) {
            if (!editor.is_object()) {
                malformed(context, "every editor must be an object");
            }
            rejectUnknownKeys(editor, {"id", "source", "presentation", "consumes"}, context + " editor");
            NodeEditorContribution contribution;
            contribution.id = requiredString(editor, "id", context + " editor");
            if (!isNamespacedIdentifier(contribution.id)) {
                malformed(context, "editor id '" + contribution.id +
                                       "' must be namespaced and free of whitespace or "
                                       "control characters");
            }
            if (!declaredEditors.insert(contribution.id).second) {
                malformed(context, "declares duplicate editor '" + contribution.id + "'");
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
                    malformed(editorContext, "'consumes' must be an array of declared parameter keys");
                }
                std::set<std::string, std::less<>> consumed;
                for (const nlohmann::json& key : *consumes) {
                    if (!key.is_string()) {
                        malformed(editorContext, "'consumes' entries must be parameter keys");
                    }
                    const std::string name = key.get<std::string>();
                    if (name.empty() || !consumed.insert(name).second) {
                        malformed(editorContext, "consumes an empty or duplicate parameter key");
                    }
                    if (!parameterNames.contains(name)) {
                        malformed(editorContext, "consumes undeclared parameter '" + name + "'");
                    }
                    contribution.consumes.push_back(name);
                }
            }
            parsed.record.editors.push_back(std::move(contribution));
        }
    }

    if (const nlohmann::json* panels = optionalMember(manifest, "panels")) {
        if (!panels->is_array()) {
            malformed(context, "'panels' must be an array");
        }
        std::set<std::string, std::less<>> declaredPanels;
        for (const nlohmann::json& panel : *panels) {
            if (!panel.is_object()) {
                malformed(context, "every panel must be an object");
            }
            rejectUnknownKeys(panel, {"id", "title", "source"}, context + " panel");
            PanelContribution contribution;
            contribution.id = requiredString(panel, "id", context + " panel");
            if (!isNamespacedIdentifier(contribution.id)) {
                malformed(context, "panel id '" + contribution.id +
                                       "' must be namespaced and free of whitespace or "
                                       "control characters");
            }
            if (!declaredPanels.insert(contribution.id).second) {
                malformed(context, "declares duplicate panel '" + contribution.id + "'");
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
            malformed(context, "'gpu' must be an object");
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
            malformed(gpuContext, "'payloadLayout' must be a namespaced layout identity");
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
}

void parseManifestFile(const fs::path& root, ParsedManifest& parsed) {
    std::string text;
    if (!readTextFile(pathText((root / kManifestFile)), text)) {
        malformed("installed package at '" + pathText(root) + "'", "manifest.json could not be read");
    }
    parseManifest(root, text, parsed);
}

// ---------------------------------------------------------------------------
// Per-user package settings. One file holds the registered linked folders and
// the explicit enablements; both are canonical absolute locations, so a folder
// reached by two routes or spelled two ways is never registered or enabled
// twice, and disabling or removing one location never touches another.
// ---------------------------------------------------------------------------

constexpr std::uint64_t kPreferencesFormat = 1;

// Canonical text location of a package folder: the resolved path when the
// folder exists, its normalized spelling otherwise, so a folder that moved is
// still compared by one deterministic location instead of by a stale string.
[[nodiscard]] std::string canonicalDirectoryText(const fs::path& path) {
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    return pathText(error ? path : canonical);
}

// The explicit NEMO_EXTENSION_PATH roots, or nullopt when that developer/test
// override is not set. An empty or separator-only value is not an override.
[[nodiscard]] std::optional<std::vector<fs::path>> configuredRootOverride() {
#if defined(_WIN32)
    const wchar_t* configured = _wgetenv(L"NEMO_EXTENSION_PATH");
    constexpr wchar_t separator = L';';
#else
    const char* configured = std::getenv("NEMO_EXTENSION_PATH");
    constexpr char separator = ':';
#endif
    if (configured == nullptr) {
        return std::nullopt;
    }
    std::vector<fs::path> roots;
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
    if (roots.empty()) {
        return std::nullopt;
    }
    return roots;
}

// What one normal startup scans. A standard root's direct child folders are
// packages; a registered linked folder IS a package folder, whether or not it
// still exists, so a moved or emptied registration stays visible instead of
// disappearing from the Settings list.
struct DiscoverySource {
    std::vector<fs::path> roots;
    std::vector<fs::path> linkedFolders;
};

[[nodiscard]] DiscoverySource discoverySource(const PackagePreferences& preferences) {
    DiscoverySource source;
    source.roots = standardPackageRoots();
    std::set<std::string, std::less<>> seen;
    for (const fs::path& root : source.roots) {
        seen.insert(canonicalDirectoryText(root));
    }
    for (const std::string& folder : preferences.linkedFolders) {
        if (!seen.insert(folder).second) {
            continue;
        }
        source.linkedFolders.push_back(pathFromText(folder));
    }
    return source;
}

// The settings object. `context` names the file in every failure, so a caller
// never has to guess which settings file was unusable.
[[nodiscard]] PackagePreferences parsePreferences(const nlohmann::json& settings, const std::string& context) {
    rejectUnknownKeys(settings, {"format", "linkedFolders", "enabled"}, context);
    const std::uint64_t format = requiredUnsigned(settings, "format", context);
    if (format != kPreferencesFormat) {
        fail(context, "package settings format " + std::to_string(format) +
                          " is not supported by this build (this build reads format " +
                          std::to_string(kPreferencesFormat) + ")");
    }
    PackagePreferences preferences;
    std::set<std::string, std::less<>> folders;
    for (const nlohmann::json& folder : requiredArray(settings, "linkedFolders", context)) {
        if (!folder.is_string()) {
            fail(context, "'linkedFolders' entries must be package folder paths");
        }
        const std::string declared = folder.get<std::string>();
        if (declared.empty()) {
            fail(context, "'linkedFolders' entries must not be empty");
        }
        const std::string canonical = canonicalDirectoryText(pathFromText(declared));
        if (!folders.insert(canonical).second) {
            fail(context, "registers the package folder '" + canonical + "' twice");
        }
        preferences.linkedFolders.push_back(canonical);
    }
    std::set<std::pair<std::string, std::string>> enablements;
    for (const nlohmann::json& entry : requiredArray(settings, "enabled", context)) {
        const std::string entryContext = context + " enabled package";
        if (!entry.is_object()) {
            fail(context, "every enabled package must be an object");
        }
        rejectUnknownKeys(entry, {"id", "directory"}, entryContext);
        EnabledPackage package;
        package.id = requiredString(entry, "id", entryContext);
        if (!isNamespacedIdentifier(package.id)) {
            fail(entryContext, "identity '" + package.id + "' is not a namespaced package id");
        }
        package.directory = canonicalDirectoryText(pathFromText(requiredString(entry, "directory", entryContext)));
        if (!enablements.emplace(package.id, package.directory).second) {
            fail(context, "enables '" + package.id + "' at '" + package.directory + "' twice");
        }
        preferences.enabled.push_back(std::move(package));
    }
    return preferences;
}

// ---------------------------------------------------------------------------
// Discovery and metadata analysis. Nothing in this section opens a library,
// calls an entrypoint or reads a file other than a package's manifest.json, so
// inspection, discovery and disabled packages never execute package code. The
// analysis is shared verbatim by metadata inspection and by activation: a
// package the Settings surface shows as admitted is admitted.
// ---------------------------------------------------------------------------

// One discovered package folder with the analysis' conclusion. `manifest` holds
// the metadata the declaration carried — including the partial metadata of a
// refused package; `reason` and `status` describe why it contributes nothing;
// `requested` says whether the persisted policy (or an explicit override) asks
// for it; `admitted` says the declaration itself is usable, which a dependency
// problem does not change.
struct Candidate {
    fs::path directory;
    std::optional<ParsedManifest> manifest;
    bool requested{false};
    bool admitted{false};
    PackageStatus status{PackageStatus::Disabled};
    std::string reason;
};

[[nodiscard]] std::string describeCandidate(const std::vector<Candidate>& candidates, std::size_t index) {
    const std::string identity =
        candidates[index].manifest ? "'" + candidates[index].manifest->record.id + "' at " : "at ";
    return "installed package " + identity + "'" + pathText(candidates[index].directory) + "'";
}

// The first reason wins: a package is refused once, with the reason that
// explains the discovery order it was found in. `declarationAdmitted` states
// whether the package's own declaration survived the refusal — a dependency
// problem leaves it admitted, the package's own declaration does not.
void refuseCandidate(std::vector<Candidate>& candidates, std::size_t index, PackageStatus status,
                     const std::string& reason, bool declarationAdmitted) {
    if (!candidates[index].reason.empty()) {
        return;
    }
    candidates[index].reason = reason;
    candidates[index].status = status;
    candidates[index].admitted = declarationAdmitted;
}

// The other discovered locations of one colliding group, so a duplicate is
// explained by naming every offender instead of letting scan order pick one.
[[nodiscard]] std::string otherLocations(const std::vector<Candidate>& candidates,
                                         const std::vector<std::size_t>& group, std::size_t index) {
    std::string others;
    for (const std::size_t member : group) {
        if (member == index) {
            continue;
        }
        if (!others.empty()) {
            others += ", ";
        }
        others += "'" + pathText(candidates[member].directory) + "'";
    }
    return others;
}

// Discovery: every root's direct child package folders, by name, in root order
// and then every linked package folder. A child folder is a package only when
// it carries a manifest; a root that is not an existing directory is skipped,
// the working directory is never searched implicitly, and one canonical folder
// reached through two routes is one package. A registered linked folder that
// moved or holds no manifest stays a candidate with a MissingPackage status, so
// the registration never reads as "no such extension was ever selected".
[[nodiscard]] std::vector<Candidate> discoverCandidates(const DiscoverySource& source) {
    std::vector<Candidate> candidates;
    std::set<std::string, std::less<>> visited;
    const auto addPackage = [&candidates, &visited](const fs::path& directory) {
        const std::string canonical = canonicalDirectoryText(directory);
        if (!visited.insert(canonical).second) {
            return;
        }
        const fs::path folder = pathFromText(canonical);
        ParsedManifest parsed;
        try {
            parseManifestFile(folder, parsed);
            candidates.push_back(
                Candidate{folder, std::move(parsed), false, true, PackageStatus::Disabled, std::string{}});
        } catch (const ManifestRefusal& refusal) {
            // The metadata the declaration did carry is kept, so a refused
            // package is still listed with its identity, name and version beside
            // the reason it cannot run.
            candidates.push_back(
                Candidate{folder, std::move(parsed), false, false, refusal.status(), std::string{refusal.what()}});
        } catch (const std::exception& failure) {
            candidates.push_back(Candidate{folder, std::nullopt, false, false, PackageStatus::MalformedManifest,
                                           std::string{failure.what()}});
        }
    };
    const auto addMissing = [&candidates, &visited](const fs::path& directory, const std::string& reason) {
        const std::string canonical = canonicalDirectoryText(directory);
        if (!visited.insert(canonical).second) {
            return;
        }
        Candidate candidate{pathFromText(canonical), std::nullopt, false, false, PackageStatus::MissingPackage, reason};
        candidates.push_back(std::move(candidate));
    };
    for (const fs::path& root : source.roots) {
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
            if (fs::is_regular_file(child / kManifestFile, manifestError)) {
                addPackage(child);
            }
        }
    }
    for (const fs::path& folder : source.linkedFolders) {
        std::error_code error;
        if (!fs::is_directory(folder, error)) {
            addMissing(folder, "the registered package folder '" + canonicalDirectoryText(folder) + "' does not exist");
            continue;
        }
        if (!fs::is_regular_file(folder / kManifestFile, error)) {
            addMissing(folder,
                       "the registered package folder '" + canonicalDirectoryText(folder) + "' holds no manifest.json");
            continue;
        }
        addPackage(folder);
    }
    return candidates;
}

// The persisted policy asks for `candidate` when it names that canonical
// location and, once a manifest declared one, its identity too. An identity
// alone never enables a package installed somewhere the user did not enable,
// and a package whose manifest cannot be read keeps the decision the user made
// for its folder.
[[nodiscard]] bool requestedByPolicy(const Candidate& candidate, const PackagePreferences& preferences) {
    const std::string directory = pathText(candidate.directory);
    const std::string identity = candidate.manifest ? candidate.manifest->record.id : std::string{};
    return std::any_of(preferences.enabled.begin(), preferences.enabled.end(),
                       [&directory, &identity](const EnabledPackage& enabled) {
                           return enabled.directory == directory && (identity.empty() || enabled.id == identity);
                       });
}

void markRequested(std::vector<Candidate>& candidates, const PackagePreferences& preferences, bool enableAll) {
    for (Candidate& candidate : candidates) {
        candidate.requested = enableAll || requestedByPolicy(candidate, preferences);
    }
}

// The metadata conclusions and the order in which the packages that will really
// run must be activated.
struct Analysis {
    std::vector<std::size_t> activationOrder;
    std::map<std::string, std::size_t, std::less<>> packageIndex;
};

// Duplicate identities/types/editors/panels (every offender, never a discovery
// order winner), built-in collisions, node schema legality, dependency presence
// and cycles. A dependency the user has not enabled is diagnosed as disabled
// rather than activated implicitly, and a package that is not requested keeps
// its metadata but is never ordered for activation.
[[nodiscard]] Analysis analyzeCandidates(std::vector<Candidate>& candidates) {
    Analysis analysis;

    std::map<std::string, std::vector<std::size_t>, std::less<>> byPackageId;
    std::map<std::string, std::vector<std::size_t>, std::less<>> byNodeType;
    std::map<std::string, std::vector<std::size_t>, std::less<>> byEditorId;
    std::map<std::string, std::vector<std::size_t>, std::less<>> byPanelId;
    std::set<std::string, std::less<>> builtinTypes;
    std::set<std::string, std::less<>> builtinEditors;
    for (const NodeContribution& contribution : builtinContributions()) {
        builtinTypes.insert(contribution.descriptor.type);
        for (const NodeEditorContribution& editor : contribution.editors) {
            builtinEditors.insert(editor.id);
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const std::optional<ParsedManifest>& declared = candidates[index].manifest;
        if (!declared || declared->record.id.empty()) {
            continue;  // the declaration did not get far enough to name anything
        }
        const PackageRecord& record = declared->record;
        byPackageId[record.id].push_back(index);
        if (!record.descriptor.type.empty()) {
            byNodeType[record.descriptor.type].push_back(index);
        }
        for (const NodeEditorContribution& editor : record.editors) {
            byEditorId[editor.id].push_back(index);
        }
        for (const PanelContribution& panel : declared->panels) {
            byPanelId[panel.id].push_back(index);
        }
    }
    for (const auto& [id, group] : byPackageId) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuseCandidate(candidates, index, PackageStatus::DuplicateIdentity,
                                describeCandidate(candidates, index) + ": duplicate package identity '" + id +
                                    "'; every package with that identity is refused, also installed at " +
                                    otherLocations(candidates, group, index),
                                false);
            }
        }
    }
    for (const auto& [type, group] : byNodeType) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuseCandidate(candidates, index, PackageStatus::DuplicateIdentity,
                                describeCandidate(candidates, index) + ": duplicate node type '" + type +
                                    "', also declared at " + otherLocations(candidates, group, index),
                                false);
            }
        }
        if (builtinTypes.contains(type)) {
            for (const std::size_t index : group) {
                refuseCandidate(candidates, index, PackageStatus::DuplicateIdentity,
                                describeCandidate(candidates, index) + ": node type '" + type +
                                    "' is already declared by this build",
                                false);
            }
        }
    }
    for (const auto& [id, group] : byEditorId) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuseCandidate(candidates, index, PackageStatus::DuplicateIdentity,
                                describeCandidate(candidates, index) + ": duplicate editor identity '" + id +
                                    "', also declared at " + otherLocations(candidates, group, index),
                                false);
            }
        }
        if (builtinEditors.contains(id)) {
            for (const std::size_t index : group) {
                refuseCandidate(candidates, index, PackageStatus::DuplicateIdentity,
                                describeCandidate(candidates, index) + ": editor identity '" + id +
                                    "' is already declared by this build",
                                false);
            }
        }
    }
    for (const auto& [id, group] : byPanelId) {
        if (group.size() > 1) {
            for (const std::size_t index : group) {
                refuseCandidate(candidates, index, PackageStatus::DuplicateIdentity,
                                describeCandidate(candidates, index) + ": duplicate panel identity '" + id +
                                    "', also declared at " + otherLocations(candidates, group, index),
                                false);
            }
        }
    }

    // Schema validation: the descriptor the manifest declares must be a legal
    // node schema on its own, before any library is opened.
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].admitted || !candidates[index].reason.empty()) {
            continue;
        }
        try {
            const std::vector<NodeDescriptor> declared{candidates[index].manifest->record.descriptor};
            const NodeCatalog catalog(declared);
            (void)catalog;
        } catch (const std::invalid_argument& error) {
            refuseCandidate(candidates, index, PackageStatus::Incompatible,
                            describeCandidate(candidates, index) + ": " + error.what(), false);
        }
    }

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].manifest && !candidates[index].manifest->record.id.empty()) {
            analysis.packageIndex.emplace(candidates[index].manifest->record.id, index);
        }
    }

    // Dependency presence, before any native code is considered: a missing or
    // disabled prerequisite is a diagnostic, never an implicit enablement. The
    // package's own declaration stays admitted, so the Settings surface reports
    // the prerequisite instead of hiding the package.
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].manifest || !candidates[index].reason.empty()) {
            continue;
        }
        for (const std::string& dependency : candidates[index].manifest->dependencies) {
            const auto found = analysis.packageIndex.find(dependency);
            if (found == analysis.packageIndex.end()) {
                refuseCandidate(
                    candidates, index, PackageStatus::MissingDependency,
                    describeCandidate(candidates, index) + ": dependency '" + dependency + "' is not installed", true);
                break;
            }
            if (!candidates[found->second].requested) {
                refuseCandidate(candidates, index, PackageStatus::DisabledDependency,
                                describeCandidate(candidates, index) + ": dependency '" + dependency +
                                    "' is disabled; enable it explicitly",
                                true);
                break;
            }
        }
    }
    // A refused package refuses everything that depends on it, transitively.
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                if (!candidates[index].manifest || !candidates[index].reason.empty()) {
                    continue;
                }
                for (const std::string& dependency : candidates[index].manifest->dependencies) {
                    const auto found = analysis.packageIndex.find(dependency);
                    if (found != analysis.packageIndex.end() && !candidates[found->second].reason.empty()) {
                        refuseCandidate(candidates, index, PackageStatus::RefusedDependency,
                                        describeCandidate(candidates, index) + ": depends on refused package '" +
                                            dependency + "'",
                                        true);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    // Resolve the entire dependency order before executing any native code, over
    // the packages that will really run. Kahn's residual nodes lie on, or depend
    // on, a cycle; neither can activate.
    std::vector<std::vector<std::size_t>> dependents(candidates.size());
    std::vector<std::size_t> remaining(candidates.size(), 0);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].requested || !candidates[index].reason.empty()) {
            continue;
        }
        for (const std::string& dependency : candidates[index].manifest->dependencies) {
            const std::size_t source = analysis.packageIndex.at(dependency);
            dependents[source].push_back(index);
            ++remaining[index];
        }
        if (remaining[index] == 0) {
            analysis.activationOrder.push_back(index);
        }
    }
    for (std::size_t position = 0; position < analysis.activationOrder.size(); ++position) {
        for (const std::size_t dependent : dependents[analysis.activationOrder[position]]) {
            if (--remaining[dependent] == 0) {
                analysis.activationOrder.push_back(dependent);
            }
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index].requested || !candidates[index].reason.empty() || remaining[index] == 0) {
            continue;
        }
        std::string involved;
        for (const std::string& dependency : candidates[index].manifest->dependencies) {
            if (remaining[analysis.packageIndex.at(dependency)] != 0) {
                if (!involved.empty()) {
                    involved += ", ";
                }
                involved += "'" + dependency + "'";
            }
        }
        refuseCandidate(candidates, index, PackageStatus::DependencyCycle,
                        describeCandidate(candidates, index) + ": dependency cycle through " + involved, true);
    }
    return analysis;
}

// The inspected metadata of one discovered package. `active` is only true for a
// package of the startup snapshot that really contributed; a package that is
// admitted but not requested is simply inactive, and one that a dependency
// blocks keeps its admitted declaration.
[[nodiscard]] PackageInfo infoFor(const Candidate& candidate, bool active) {
    PackageInfo info;
    info.directory = pathText(candidate.directory);
    info.diagnostic = candidate.reason;
    info.requestedEnabled = candidate.requested;
    info.active = active;
    info.admitted = candidate.admitted;
    info.status = active ? PackageStatus::Active : candidate.status;
    const bool blocked = info.status == PackageStatus::MissingDependency ||
                         info.status == PackageStatus::RefusedDependency ||
                         info.status == PackageStatus::DependencyCycle || info.status == PackageStatus::FailedToLoad;
    info.canEnable = candidate.admitted && !blocked;
    if (!candidate.manifest) {
        return info;
    }
    const ParsedManifest& parsed = *candidate.manifest;
    info.id = parsed.record.id;
    info.name = parsed.name;
    info.author = parsed.author;
    info.description = parsed.description;
    info.version = parsed.record.version;
    if (!parsed.record.descriptor.type.empty()) {
        info.nodeTypes.push_back(parsed.record.descriptor.type);
    }
    for (const NodeEditorContribution& editor : parsed.record.editors) {
        info.editors.push_back(editor.id);
    }
    for (const PanelContribution& panel : parsed.panels) {
        info.panels.push_back(panel.id);
    }
    info.dependencies = parsed.dependencies;
    return info;
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

std::vector<std::filesystem::path> standardPackageRoots() {
    std::vector<fs::path> roots;
#if defined(_WIN32)
    if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData != nullptr && *localAppData != L'\0') {
        const fs::path base{localAppData};
        if (base.is_absolute()) {
            roots.push_back(base / "Nemo" / "extensions");
        }
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

std::filesystem::path packagePreferencesPath() {
#if defined(_WIN32)
    if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData != nullptr && *localAppData != L'\0') {
        const fs::path base{localAppData};
        if (base.is_absolute()) {
            return base / "Nemo" / "extensions.json";
        }
    }
#else
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME"); configHome != nullptr && *configHome != '\0') {
        const fs::path base{configHome};
        if (base.is_absolute()) {
            return base / "nemo" / "extensions.json";
        }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const fs::path base{home};
        if (base.is_absolute()) {
            return base / ".config" / "nemo" / "extensions.json";
        }
    }
#endif
    return {};
}

bool PackagePreferences::isEnabled(const std::string& id, const std::string& canonicalDirectory) const {
    return std::any_of(enabled.begin(), enabled.end(), [&id, &canonicalDirectory](const EnabledPackage& package) {
        return package.id == id && package.directory == canonicalDirectory;
    });
}

void PackagePreferences::setEnabled(const std::string& id, const std::string& canonicalDirectory, bool enable) {
    const auto match = [&id, &canonicalDirectory](const EnabledPackage& package) {
        return package.id == id && package.directory == canonicalDirectory;
    };
    const auto found = std::find_if(enabled.begin(), enabled.end(), match);
    if (!enable) {
        if (found != enabled.end()) {
            enabled.erase(found);
        }
        return;
    }
    if (found == enabled.end()) {
        enabled.push_back(EnabledPackage{id, canonicalDirectory});
    }
}

void PackagePreferences::removeLinkedFolder(const std::string& canonicalDirectory) {
    linkedFolders.erase(std::remove(linkedFolders.begin(), linkedFolders.end(), canonicalDirectory),
                        linkedFolders.end());
    // A removed registration forgets the enablement of the package installed
    // there, and nothing else. The folder and its files are left untouched.
    enabled.erase(std::remove_if(enabled.begin(), enabled.end(),
                                 [&canonicalDirectory](const EnabledPackage& package) {
                                     return package.directory == canonicalDirectory;
                                 }),
                  enabled.end());
}

PackagePreferencesLoad loadPackagePreferences(const std::filesystem::path& path) {
    PackagePreferencesLoad loaded;
    if (path.empty()) {
        loaded.diagnostic = "the per-user package settings location cannot be determined on this system";
        return loaded;
    }
    std::error_code error;
    const bool present = fs::exists(path, error);
    if (error) {
        loaded.diagnostic = "package settings at '" + pathText(path) + "' could not be read: " + error.message();
        return loaded;
    }
    if (!present) {
        // A first launch: nothing is linked and nothing is enabled, so no
        // package that was merely discovered becomes trusted.
        return loaded;
    }
    if (!fs::is_regular_file(path, error)) {
        loaded.diagnostic = "package settings at '" + pathText(path) + "' are not a regular file";
        return loaded;
    }
    std::string text;
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            loaded.diagnostic = "package settings at '" + pathText(path) + "' could not be read";
            return loaded;
        }
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    const std::string context = "package settings at '" + pathText(path) + "'";
    try {
        const nlohmann::json settings = nlohmann::json::parse(text, nullptr, false);
        if (settings.is_discarded()) {
            malformed(context, "the file is not valid JSON");
        }
        if (!settings.is_object()) {
            malformed(context, "the file must contain an object");
        }
        loaded.preferences = parsePreferences(settings, context);
    } catch (const std::exception& failure) {
        // Fail closed: an unreadable trust decision enables nothing, and the
        // file is left exactly as it was found so a later build — or the user —
        // can still read it.
        loaded.preferences = PackagePreferences{};
        loaded.diagnostic = failure.what();
    }
    return loaded;
}

bool savePackagePreferences(const std::filesystem::path& path, const PackagePreferences& preferences,
                            std::string& diagnostic) {
    if (path.empty()) {
        diagnostic = "the per-user package settings location cannot be determined on this system";
        return false;
    }
    nlohmann::json settings;
    settings["format"] = kPreferencesFormat;
    settings["linkedFolders"] = preferences.linkedFolders;
    nlohmann::json enabled = nlohmann::json::array();
    for (const EnabledPackage& package : preferences.enabled) {
        enabled.push_back(nlohmann::json{{"id", package.id}, {"directory", package.directory}});
    }
    settings["enabled"] = std::move(enabled);
    const std::string text = settings.dump(2) + "\n";

    const fs::path directory = path.parent_path();
    std::error_code error;
    if (!directory.empty()) {
        fs::create_directories(directory, error);
        if (error) {
            diagnostic =
                "cannot create the package settings directory '" + pathText(directory) + "': " + error.message();
            return false;
        }
    }
    // One atomic replacement: a temporary file beside the target is renamed over
    // it, so a reader sees either the previous or the complete new settings and
    // never a half-written file.
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            diagnostic = "cannot write the package settings at '" + pathText(temporary) + "'";
            return false;
        }
        stream << text;
        stream.flush();
        if (!stream) {
            diagnostic = "cannot write the package settings at '" + pathText(temporary) + "'";
            std::error_code cleanup;
            fs::remove(temporary, cleanup);
            return false;
        }
    }
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    }
#else
    fs::rename(temporary, path, error);
#endif
    if (error) {
        diagnostic = "cannot replace the package settings at '" + pathText(path) + "': " + error.message();
        std::error_code cleanup;
        fs::remove(temporary, cleanup);
        return false;
    }
    diagnostic.clear();
    return true;
}

bool canonicalPackageFolder(const std::filesystem::path& folder, std::string& canonical, std::string& reason) {
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        reason = "'" + pathText(folder) + "' is not a package folder";
        return false;
    }
    if (!fs::is_regular_file(folder / kManifestFile, error)) {
        reason = "'" + pathText(folder) + "' holds no manifest.json; select the package folder itself";
        return false;
    }
    canonical = canonicalDirectoryText(folder);
    reason.clear();
    return true;
}

std::vector<PackageInfo> inspectInstalledPackages(const PackagePreferences& preferences) {
    std::vector<Candidate> candidates = discoverCandidates(discoverySource(preferences));
    markRequested(candidates, preferences, false);
    (void)analyzeCandidates(candidates);
    std::vector<PackageInfo> inventory;
    inventory.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        inventory.push_back(infoFor(candidate, false));
    }
    return inventory;
}

InstalledPackages::InstalledPackages() {
    if (auto override = configuredRootOverride()) {
        initialize(std::move(*override), true);
        return;
    }
    initialize({}, false);
}

InstalledPackages::InstalledPackages(std::vector<fs::path> roots) {
    initialize(std::move(roots), true);
}

void InstalledPackages::initialize(std::vector<fs::path> trustedRoots, bool trusted) {
    trustedOverride_ = trusted;
    DiscoverySource source;
    PackagePreferences preferences;
    std::string settingsDiagnostic;
    if (trusted) {
        source.roots = std::move(trustedRoots);
    } else {
        const PackagePreferencesLoad settings = loadPackagePreferences(packagePreferencesPath());
        preferences = settings.preferences;
        settingsDiagnostic = settings.diagnostic;
        source = discoverySource(preferences);
    }
    if (!settingsDiagnostic.empty()) {
        // An unreadable settings file is reported like any other refusal: the
        // application never silently replaces the user's decision with a guess.
        diagnostics_.push_back(std::move(settingsDiagnostic));
    }

    // Every discovered package folder - including one that carries no readable
    // manifest - keeps one slot, in discovery order, so a package that is
    // malformed, refused or merely disabled is still reported where it was
    // found.
    std::vector<Candidate> candidates = discoverCandidates(source);
    markRequested(candidates, preferences, trusted);
    const Analysis analysis = analyzeCandidates(candidates);

    // Activation: the enabled and admitted packages, in dependency order, and
    // nothing else. Every other package's library is never opened, and a
    // failure is retained as an inactive inventory entry instead of failing the
    // application or hiding an unrelated supported package.
    std::vector<std::shared_ptr<const detail::SharedLibrary>> libraries(candidates.size());
    std::vector<bool> active(candidates.size(), false);
    for (const std::size_t index : analysis.activationOrder) {
        const ParsedManifest& manifest = *candidates[index].manifest;
        // A dependency that failed to activate in this snapshot refuses its
        // dependant before its own native library is opened.
        for (const std::string& dependency : manifest.dependencies) {
            const auto found = analysis.packageIndex.find(dependency);
            if (found == analysis.packageIndex.end() || !candidates[found->second].reason.empty()) {
                refuseCandidate(
                    candidates, index, PackageStatus::RefusedDependency,
                    describeCandidate(candidates, index) + ": depends on refused package '" + dependency + "'", true);
                break;
            }
        }
        if (!candidates[index].reason.empty()) {
            continue;
        }
        std::string reason;
        libraries[index] =
            detail::SharedLibrary::open(pathFromText(manifest.library), manifest.record.gpu.has_value(), reason);
        if (!libraries[index]) {
            refuseCandidate(candidates, index, PackageStatus::FailedToLoad,
                            describeCandidate(candidates, index) + ": " + reason, true);
            continue;
        }
        ParameterValues defaults;
        for (const ParameterSpec& parameter : manifest.record.descriptor.parameters) {
            defaults.emplace(parameter.name, parameter.defaultValue);
        }
        if (const auto problem = detail::validatePackageParameters(libraries[index], manifest.record.id, defaults)) {
            refuseCandidate(candidates, index, PackageStatus::FailedToLoad,
                            describeCandidate(candidates, index) + ": invalid default parameters: " + *problem, true);
            libraries[index].reset();
            continue;
        }
        active[index] = true;
    }

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        inventory_.push_back(infoFor(candidates[index], active[index]));
        if (!active[index]) {
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
        if (!candidate.reason.empty()) {
            diagnostics_.push_back(candidate.reason);
        }
    }

    std::vector<NodeContribution> contributions = builtinContributions();
    contributions.reserve(contributions.size() + records_.size());
    for (const PackageRecord& record : records_) {
        contributions.push_back(detail::packageContribution(record));
    }
    contributions_ = std::make_shared<const NodeContributions>(std::move(contributions));
}

}  // namespace nemo::extensions
