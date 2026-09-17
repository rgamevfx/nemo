#include "nemo/core/evaluation/NodeContributions.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "nemo/core/Hashing.hpp"
#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
namespace {

[[nodiscard]] const ParameterSpec* findParameter(const NodeDescriptor& descriptor, std::string_view name) {
    const auto found = std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                                    [name](const ParameterSpec& parameter) { return parameter.name == name; });
    return found == descriptor.parameters.end() ? nullptr : &*found;
}

// Namespaced presentation identities never contain whitespace or control
// characters, mirroring the parameter editor-id contract in the schema catalog.
[[nodiscard]] bool isNamespacedIdentifier(std::string_view identifier) {
    if (identifier.empty() || identifier.find('.') == std::string_view::npos)
        return false;
    for (const char character : identifier) {
        const auto value = static_cast<unsigned char>(character);
        if (std::isspace(value) || value < 0x20 || value == 0x7F)
            return false;
    }
    return true;
}

[[nodiscard]] const char* roleName(NodeRole role) {
    switch (role) {
    case NodeRole::Image:
        return "image";
    case NodeRole::Source:
        return "source";
    case NodeRole::Output:
        return "output";
    case NodeRole::Viewer:
        return "viewer";
    case NodeRole::Delivery:
        return "delivery";
    }
    return "unknown";
}

// Implementation identity of a declared parameter: name, typed kind, default,
// hard range, choices, the nonzero constraint and the creation-time initial
// value rule. Labels, sections, rows, steps, soft ranges, decimals, channel
// hints and editor ids are presentation and never make two declarations
// different implementations.
[[nodiscard]] std::optional<std::string> parameterMismatch(const ParameterSpec& document,
                                                           const ParameterSpec& registered) {
    if (document.name != registered.name)
        return "the declared parameter order or identities differ";
    if (document.type != registered.type)
        return "parameter '" + document.name + "' has a different type";
    if (document.defaultValue != registered.defaultValue)
        return "parameter '" + document.name + "' has a different default";
    if (document.minimum != registered.minimum || document.maximum != registered.maximum)
        return "parameter '" + document.name + "' has a different hard range";
    if (document.choices != registered.choices)
        return "parameter '" + document.name + "' has different choices";
    if (document.nonzero != registered.nonzero)
        return "parameter '" + document.name + "' has a different nonzero constraint";
    // A creation-time initial value rule changes what a created node stores, so
    // it is implementation identity, not presentation (issue #92).
    if (document.initialValue != registered.initialValue)
        return "parameter '" + document.name + "' has a different creation-time initial value rule";
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> portsMismatch(const std::vector<PortSpec>& document,
                                                       const std::vector<PortSpec>& registered, const char* direction) {
    if (document.size() != registered.size())
        return std::string{"the declared "} + direction + " port count differs";
    for (std::size_t index = 0; index < document.size(); ++index) {
        if (!(document[index] == registered[index]))
            return std::string{"the declared "} + direction + " port " + std::to_string(index) + " differs";
    }
    return std::nullopt;
}

// Identity-relevant schema difference between what a document's catalog
// declares and what the registration implements. Presentation metadata never
// participates, so a label-only difference is not an implementation mismatch.
[[nodiscard]] std::optional<std::string> schemaMismatch(const NodeDescriptor& document,
                                                        const NodeDescriptor& registered) {
    if (document.type != registered.type)
        return "the persistent node identity differs";
    if (document.implementationVersion != registered.implementationVersion) {
        return "the implementation version differs (document " + std::to_string(document.implementationVersion) +
               ", registered " + std::to_string(registered.implementationVersion) + ")";
    }
    if (document.isOutput != registered.isOutput)
        return "the network-output declaration differs";
    if (document.isDeliverySink != registered.isDeliverySink)
        return "the delivery-sink declaration differs";
    if (const auto problem = portsMismatch(document.inputs, registered.inputs, "input"))
        return problem;
    if (const auto problem = portsMismatch(document.outputs, registered.outputs, "output"))
        return problem;
    if (document.parameters.size() != registered.parameters.size())
        return "the declared parameter count differs";
    for (std::size_t index = 0; index < document.parameters.size(); ++index) {
        if (const auto problem = parameterMismatch(document.parameters[index], registered.parameters[index]))
            return problem;
    }
    const NodeCapabilities& declared = document.capabilities;
    const NodeCapabilities& declaredRegistration = registered.capabilities;
    if (declared.samplingScales != declaredRegistration.samplingScales)
        return "the declared sampling scales differ";
    if (declared.qualityModes != declaredRegistration.qualityModes)
        return "the declared quality modes differ";
    if (declared.channels != declaredRegistration.channels)
        return "the declared channels differ";
    if (declared.supportsRegion != declaredRegistration.supportsRegion)
        return "the declared region support differs";
    if (declared.temporal != declaredRegistration.temporal)
        return "the declared temporal support differs";
    return std::nullopt;
}

// Validates one declaration and folds its editor identity into the shared
// declaration map: the same namespaced id may be declared by more than one node
// only when the declarations are identical.
void validateDeclaration(const NodeContribution& contribution,
                         std::map<std::string, const NodeEditorContribution*, std::less<>>& editorDeclarations) {
    const NodeDescriptor& descriptor = contribution.descriptor;
    const std::string context = "node contribution '" + descriptor.type + "'";

    // A Delivery node has real pixels — a pass-through CPU adapter — so it is a
    // pixel role like an ordinary effect (issue #94); the display-only Viewer
    // stays the one role that never implements any.
    const bool producesPixels = contribution.role == NodeRole::Image || contribution.role == NodeRole::Source ||
                                contribution.role == NodeRole::Delivery;
    if (producesPixels && !contribution.cpu && contribution.cpuUnavailableReason.empty()) {
        throw std::invalid_argument(context + ": the " + roleName(contribution.role) +
                                    " role promises a CPU implementation without an adapter or an explicit "
                                    "unavailability reason");
    }
    if (contribution.cpu) {
        if (!contribution.cpuUnavailableReason.empty())
            throw std::invalid_argument(context + ": CPU implementation conflicts with an unavailability declaration");
        if (!producesPixels)
            throw std::invalid_argument(context + ": a nonpixel role declares a CPU pixel implementation");
        if (!contribution.cpu->execute)
            throw std::invalid_argument(context + ": CPU implementation has no execute callback");
        if (contribution.cpu->version != descriptor.implementationVersion) {
            throw std::invalid_argument(context + ": CPU implementation version " +
                                        std::to_string(contribution.cpu->version) + " conflicts with schema version " +
                                        std::to_string(descriptor.implementationVersion));
        }
    }
    if ((contribution.role == NodeRole::Output) != descriptor.isOutput) {
        throw std::invalid_argument(context + ": the " + roleName(contribution.role) +
                                    " role conflicts with the descriptor's network-output declaration");
    }
    if ((contribution.role == NodeRole::Delivery) != descriptor.isDeliverySink) {
        throw std::invalid_argument(context + ": the " + roleName(contribution.role) +
                                    " role conflicts with the descriptor's delivery-sink declaration");
    }

    std::set<std::string> declaredEditors;
    for (const NodeEditorContribution& editor : contribution.editors) {
        if (!isNamespacedIdentifier(editor.id)) {
            throw std::invalid_argument(context + ": editor id '" + editor.id +
                                        "' must be namespaced and free of whitespace or control characters");
        }
        if (!declaredEditors.insert(editor.id).second)
            throw std::invalid_argument(context + ": duplicate editor declaration '" + editor.id + "'");
        if (editor.source.empty())
            throw std::invalid_argument(context + ": editor '" + editor.id + "' has an empty source locator");
        if (editor.presentation != "row" && editor.presentation != "section") {
            throw std::invalid_argument(context + ": editor '" + editor.id + "' declares unsupported presentation '" +
                                        editor.presentation + "'");
        }
        std::set<std::string> consumed;
        for (const std::string& key : editor.consumes) {
            if (key.empty() || !consumed.insert(key).second)
                throw std::invalid_argument(context + ": editor '" + editor.id +
                                            "' consumes an empty or duplicate parameter key");
            if (findParameter(descriptor, key) == nullptr) {
                throw std::invalid_argument(context + ": editor '" + editor.id + "' consumes undeclared parameter '" +
                                            key + "'");
            }
        }
        const auto [entry, inserted] = editorDeclarations.emplace(editor.id, &editor);
        if (!inserted &&
            (entry->second->source != editor.source || entry->second->presentation != editor.presentation ||
             entry->second->consumes != editor.consumes)) {
            throw std::invalid_argument(context + ": editor '" + editor.id +
                                        "' conflicts with an identical-id declaration from another node");
        }
    }
}

[[nodiscard]] std::uint64_t computeFingerprint(const std::vector<NodeContribution>& contributions) {
    std::uint64_t hash = kFnv1a64Basis;
    for (const NodeContribution& contribution : contributions) {
        hashMixText(hash, contribution.descriptor.type);
        hashMixWord(hash, contribution.descriptor.implementationVersion);
        hashMixWord(hash, static_cast<std::uint64_t>(contribution.role));
        hashMixWord(hash, contribution.nativeGpu ? 1U : 0U);
        hashMixWord(hash, contribution.ownsChannelLayout ? 1U : 0U);
        hashMixWord(hash, contribution.cpu ? 1U : 0U);
        if (contribution.cpu)
            hashMixWord(hash, contribution.cpu->version);
    }
    return hash;
}

}  // namespace

NodeContributions::NodeContributions(std::vector<NodeContribution> contributions) {
    std::vector<NodeDescriptor> descriptors;
    descriptors.reserve(contributions.size());
    std::map<std::string, const NodeEditorContribution*, std::less<>> editorDeclarations;
    for (NodeContribution& contribution : contributions) {
        // An unnamed display label falls back to the persistent identity, the
        // same rule the schema catalog applies, so projection and catalog agree.
        if (contribution.descriptor.displayName.empty())
            contribution.descriptor.displayName = contribution.descriptor.type;
        validateDeclaration(contribution, editorDeclarations);
        descriptors.push_back(contribution.descriptor);
    }
    // Atomic publication: the schema catalog applies the descriptor validation
    // and rejects invalid or duplicate identities, and it must succeed before
    // this snapshot is observable.
    catalog_ = std::make_shared<const NodeCatalog>(std::move(descriptors));
    std::sort(contributions.begin(), contributions.end(),
              [](const auto& left, const auto& right) { return left.descriptor.type < right.descriptor.type; });
    contributions_ = std::move(contributions);
    fingerprint_ = computeFingerprint(contributions_);
}

const NodeContribution* NodeContributions::find(std::string_view type) const {
    const auto found = std::lower_bound(contributions_.begin(), contributions_.end(), type,
                                        [](const NodeContribution& contribution, std::string_view identity) {
                                            return contribution.descriptor.type < identity;
                                        });
    return found != contributions_.end() && found->descriptor.type == type ? &*found : nullptr;
}

void NodeContributions::validate(const NodeCatalog& catalog, const NodeInstance& node) const {
    const NodeContribution* contribution = find(node.type);
    if (contribution == nullptr) {
        throw EvaluationException(describeNode(node) + ": node type '" + node.type +
                                      "' is not part of the supplied node registration",
                                  node.id, node.name);
    }
    const NodeDescriptor* declared = catalog.find(node.type);
    if (declared == nullptr) {
        throw EvaluationException(describeNode(node) + ": the document catalog declares no schema for node type '" +
                                      node.type + "'",
                                  node.id, node.name);
    }
    if (const auto problem = schemaMismatch(*declared, contribution->descriptor)) {
        throw EvaluationException(describeNode(node) + ": the registered '" + node.type +
                                      "' implementation does not match the document schema: " + *problem,
                                  node.id, node.name);
    }
}

std::optional<std::string> NodeContributions::validateParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                                 const ParameterValues& effectiveParams) const {
    const NodeContribution* contribution = find(node.type);
    if (contribution == nullptr || !contribution->validateParameters)
        return std::nullopt;
    return contribution->validateParameters(catalog, node, effectiveParams);
}

}  // namespace nemo
