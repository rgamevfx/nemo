#include "nemo/core/commands/ReadSourceCommands.hpp"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace nemo {
namespace {

void reject(GraphError code, const std::string& message) {
    throw GraphException(code, "read source command: " + message);
}

[[nodiscard]] bool probeCommittable(const MediaProbeMetadata& probe) {
    if (const auto problem = probeFactProblem(probe))
        reject(GraphError::InvalidMediaQuery, "probe is not admissible: " + *problem);
    return !probe.provenance.empty();
}

[[nodiscard]] std::string baseName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    // A leading dot is part of the name, not an extension separator.
    const std::string_view stem = dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
    return stem.empty() ? std::string{"read"} : std::string{stem};
}

[[nodiscard]] std::string sanitizeKey(std::string_view stem) {
    std::string key;
    key.reserve(stem.size());
    for (const char character : stem) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (std::isalnum(value) != 0 || character == '.' || character == '_' || character == '-')
            key.push_back(character);
        else
            key.push_back('_');
    }
    return key.empty() ? std::string{"read"} : key;
}

// The document's own key namespace: a derived key never collides with an
// authored one, and it stays stable across save/reopen.
[[nodiscard]] std::string uniqueSourceKey(const Document& document, const std::string& path) {
    const std::string base = sanitizeKey(baseName(path));
    std::string key = base;
    for (int suffix = 2; document.sources.contains(key); ++suffix)
        key = base + "-" + std::to_string(suffix);
    return key;
}

// One media reference per normalized path: that reference is what the Media Bin
// and every non-Read consumer address, so a second Read of the same file joins
// it instead of duplicating the media identity. A Read's own interpretation
// choices live on the node, so they never take part in this match.
[[nodiscard]] const SourceReference* findReusableReference(const Document& document, const std::string& normalized,
                                                           std::string& key) {
    for (const auto& [candidateKey, reference] : document.sources) {
        if (normalizedSourcePath(reference.path) == normalized) {
            key = candidateKey;
            return &reference;
        }
    }
    return nullptr;
}

[[nodiscard]] const SourceReference& requireReference(const Document& document, const std::string& key,
                                                      std::string_view action) {
    const auto found = document.sources.find(key);
    if (found == document.sources.end())
        reject(GraphError::MissingMediaSource,
               std::string(action) + ": source '" + key + "' is not in Document::sources");
    return found->second;
}

void requireExpected(const SourceReference& current, const SourceReference& expected, const std::string& key,
                     std::string_view action) {
    if (current != expected)
        reject(GraphError::StaleMediaSource,
               std::string(action) + ": source '" + key + "' no longer matches the expected reference");
}

// Commits the (already validated) probe on every catalog entry that names the
// source key, so a shared reference never shows stale metadata.
void commitProbe(Document& document, const std::string& key, MediaKind kind, const MediaProbeMetadata& probe) {
    if (probe.provenance.empty())
        return;
    for (const auto& entry : document.mediaCatalog().entries()) {
        if (entry.sourceKey != key)
            continue;
        MediaMetadata metadata = entry.metadata;
        metadata.committedProbe = probe;
        if (kind != MediaKind::Unknown)
            metadata.kind = kind;
        document.mediaCatalog().setMetadata(entry.id, std::move(metadata));
    }
}

[[nodiscard]] std::string overrideProblemMessage(const ReadNodeOverrides& overrides, std::string_view what) {
    if (const auto problem = readOverridesProblem(overrides))
        return std::string(what) + ": " + *problem;
    return {};
}

}  // namespace

std::string normalizedSourcePath(std::string_view path) {
    if (path.empty())
        return {};
    // lexically_normal() is pure string math: it never touches the filesystem,
    // so a redo resolves the same reference as the original apply.
    return std::filesystem::path(path).lexically_normal().generic_string();
}

Command registerReadSourceCommand(ParameterAddress target, double time, std::string path,
                                  std::optional<ReadNodeOverrides> initializeOverrides, MediaKind kind,
                                  MediaProbeMetadata probe, std::shared_ptr<std::string> assignedKey) {
    if (path.empty())
        throw std::invalid_argument("register read source: media path must not be empty");
    if (target.key != kReadParamSourceKey)
        throw std::invalid_argument("register read source: target parameter must be '" +
                                    std::string(kReadParamSourceKey) + "', got '" + target.key + "'");
    if (!std::isfinite(time))
        throw std::invalid_argument("register read source: gesture frame must be finite");
    if (initializeOverrides) {
        if (const std::string problem = overrideProblemMessage(*initializeOverrides, "register read source");
            !problem.empty())
            throw std::invalid_argument(problem);
    }
    const bool wantsProbe = probeCommittable(probe);
    return Command{
        "read media '" + path + "'",
        [target, time, path = std::move(path), initializeOverrides = std::move(initializeOverrides), kind,
         probe = std::move(probe), wantsProbe, assignedKey](Document& document) -> void {
            auto& graph = document.network(target.network).graph();
            const NodeInstance* node = graph.node(target.node);
            if (node == nullptr)
                reject(GraphError::UnknownNode, "unknown Read node " + std::to_string(target.node) + " in network " +
                                                    std::to_string(target.network));
            if (node->type != "source")
                reject(GraphError::ParameterValue, "node '" + node->name + "' is not a Read node (" + node->type + ")");
            if (target.instance != kInvalidNetworkInstance) {
                const NetworkInstance* occurrence = document.instance(target.instance);
                if (occurrence == nullptr)
                    reject(GraphError::InvalidNetwork,
                           "unknown network instance " + std::to_string(target.instance) + " for this Read binding");
                if (occurrence->definition != target.network)
                    reject(GraphError::InvalidNetwork,
                           "network instance " + std::to_string(target.instance) + " does not instantiate network " +
                               std::to_string(target.network) + ", so it cannot address node " +
                               std::to_string(target.node));
            }
            if (initializeOverrides) {
                // Authored choices describe the Read definition. Initializing them
                // through an occurrence would rewrite every other occurrence, and
                // re-initializing a bound Read would silently discard the choices
                // already authored on it.
                if (target.instance != kInvalidNetworkInstance)
                    reject(GraphError::ParameterValue,
                           "authored Read choices can be initialized only on the first binding of the definition "
                           "node, not through occurrence " +
                               std::to_string(target.instance));
                // First binding is an admission edge, not a string test: a
                // cleared Read authors an empty source, and an animated source
                // address holds its value in a channel, so both are already bound.
                const bool authorsSource = node->params.find(std::string(kReadParamSourceKey)) != node->params.end();
                const bool animatesSource =
                    document.animationChannel(ParameterAddress{target.network, target.node, "source"}) != nullptr;
                if (authorsSource || animatesSource)
                    reject(GraphError::ParameterValue,
                           "node '" + node->name +
                               "' already binds or animates its media source; replace it without re-initializing "
                               "authored choices");
            }

            const std::string normalized = normalizedSourcePath(path);
            std::string key;
            const SourceReference* reused = findReusableReference(document, normalized, key);
            if (reused == nullptr) {
                key = uniqueSourceKey(document, path);
                // The shared reference carries media identity only (path and
                // content revision): mapping, policies and interpretation are the
                // Read's own choices now, so a new Read authors nothing shared.
                SourceReference reference;
                reference.path = path;
                reference.revision = 1;
                document.setSourceReference(key, std::move(reference));

                MediaMetadata metadata;
                metadata.userName = document.mediaCatalog().nextAvailableName(kInvalidMediaBin, baseName(path));
                metadata.kind = kind;
                document.mediaCatalog().addEntry(key, kInvalidMediaBin, std::move(metadata));
            }
            if (assignedKey)
                *assignedKey = key;

            if (wantsProbe)
                commitProbe(document, key, kind, probe);

            // One batch, one command: the binding and the initial choices travel
            // together, and each address follows the same rule parameter authoring
            // always uses - an address that already has animation is keyed at the
            // gesture frame, every other address holds a static value.
            std::vector<ParameterEdit> edits;
            edits.push_back(ParameterEdit{target, ParameterValue{std::string{key}}});
            if (initializeOverrides) {
                // Fill only the fields this Read does not already hold, so
                // initialization never overwrites an authored or animated choice.
                for (auto& [name, value] : readInitializationParameters(
                         document, ParameterAddress{target.network, target.node, "source"}, *initializeOverrides))
                    edits.push_back(
                        ParameterEdit{ParameterAddress{target.network, target.node, name}, std::move(value)});
            }
            std::vector<ParameterEdit> keyed;
            std::vector<ParameterEdit> staticValues;
            for (auto& edit : edits) {
                if (document.animationChannel(edit.address) != nullptr)
                    keyed.push_back(std::move(edit));
                else
                    staticValues.push_back(std::move(edit));
            }
            parameterValueCommand(document, nullptr, time, keyed, staticValues).apply(document);
        }};
}

Command relinkReadSourceCommand(std::string sourceKey, SourceReference expected, std::string path, MediaKind kind,
                                MediaProbeMetadata probe) {
    if (sourceKey.empty())
        throw std::invalid_argument("relink read source: source key must not be empty");
    if (path.empty())
        throw std::invalid_argument("relink read source: source '" + sourceKey + "' must reference a non-empty path");
    const bool wantsProbe = probeCommittable(probe);
    return Command{"relink read source '" + sourceKey + "'",
                   [sourceKey = std::move(sourceKey), expected = std::move(expected), path = std::move(path), kind,
                    probe = std::move(probe), wantsProbe](Document& document) {
                       const SourceReference& current = requireReference(document, sourceKey, "cannot relink");
                       requireExpected(current, expected, sourceKey, "cannot relink");
                       if (current.revision == std::numeric_limits<std::uint64_t>::max())
                           reject(GraphError::InvalidMediaQuery, "source '" + sourceKey + "' revision exhausted");
                       SourceReference relinked = current;
                       relinked.path = path;
                       relinked.revision = current.revision + 1;
                       document.setSourceReference(sourceKey, std::move(relinked));

                       // The committed probe described the previous file; it is
                       // obsolete for every entry that shares the source key.
                       for (const auto& entry : document.mediaCatalog().entries()) {
                           if (entry.sourceKey != sourceKey)
                               continue;
                           MediaMetadata metadata = entry.metadata;
                           metadata.committedProbe.reset();
                           metadata.kind = kind;
                           document.mediaCatalog().setMetadata(entry.id, std::move(metadata));
                       }
                       if (wantsProbe)
                           commitProbe(document, sourceKey, kind, probe);
                   }};
}

Command reloadReadSourceCommand(std::string sourceKey, SourceReference expected, MediaKind kind,
                                MediaProbeMetadata probe) {
    if (sourceKey.empty())
        throw std::invalid_argument("reload read source: source key must not be empty");
    const bool wantsProbe = probeCommittable(probe);
    if (!wantsProbe)
        throw std::invalid_argument("reload read source: source '" + sourceKey +
                                    "' reload requires a probe identifying its provenance");
    return Command{"reload read source '" + sourceKey + "'",
                   [sourceKey = std::move(sourceKey), expected = std::move(expected), kind,
                    probe = std::move(probe)](Document& document) {
                       const SourceReference& current = requireReference(document, sourceKey, "cannot reload");
                       requireExpected(current, expected, sourceKey, "cannot reload");
                       if (current.revision == std::numeric_limits<std::uint64_t>::max())
                           reject(GraphError::InvalidMediaQuery, "source '" + sourceKey + "' revision exhausted");
                       // Exactly one revision advance per reload: overwritten
                       // media becomes a new content identity, and dependents of
                       // this source are the only results invalidated.
                       SourceReference reloaded = current;
                       reloaded.revision = current.revision + 1;
                       document.setSourceReference(sourceKey, std::move(reloaded));
                       commitProbe(document, sourceKey, kind, probe);
                   }};
}

}  // namespace nemo
