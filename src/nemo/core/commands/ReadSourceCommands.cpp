#include "nemo/core/commands/ReadSourceCommands.hpp"

#include <cctype>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace nemo {
namespace {

void reject(GraphError code, const std::string& message) {
    throw GraphException(code, "read source command: " + message);
}

[[nodiscard]] bool probeCommittable(const MediaProbeMetadata& probe) {
    if (probe.width < 0 || probe.height < 0 || probe.duration < 0)
        reject(GraphError::InvalidMediaQuery, "probe dimensions and duration must be nonnegative");
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

// Reuse is by normalized path + interpretation, which is exactly the pair that
// names one media reference; authored timing belongs to that shared reference.
[[nodiscard]] const SourceReference* findReusableReference(const Document& document, const std::string& normalized,
                                                           const std::map<std::string, std::string>& interpretation,
                                                           std::string& key) {
    for (const auto& [candidateKey, reference] : document.sources) {
        if (reference.interpretation == interpretation && normalizedSourcePath(reference.path) == normalized) {
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
void commitProbe(Document& document, const std::string& key, const MediaProbeMetadata& probe) {
    if (probe.provenance.empty())
        return;
    for (const auto& entry : document.mediaCatalog().entries()) {
        if (entry.sourceKey != key)
            continue;
        MediaMetadata metadata = entry.metadata;
        metadata.committedProbe = probe;
        document.mediaCatalog().setMetadata(entry.id, std::move(metadata));
    }
}

void requireValidTiming(const ReadSourceTiming& timing, const std::string& what) {
    if (timing.frameStep == 0)
        throw std::invalid_argument("read source command: " + what + " frameStep must not be zero");
    if (timing.firstFrame && timing.lastFrame && *timing.firstFrame > *timing.lastFrame)
        throw std::invalid_argument("read source command: " + what + " firstFrame must not exceed lastFrame");
}

}  // namespace

std::string normalizedSourcePath(std::string_view path) {
    if (path.empty())
        return {};
    // lexically_normal() is pure string math: it never touches the filesystem,
    // so a redo resolves the same reference as the original apply.
    return std::filesystem::path(path).lexically_normal().generic_string();
}

Command registerReadSourceCommand(NetworkId network, NodeId node, std::string path, ReadSourceTiming timing,
                                  MediaProbeMetadata probe, std::shared_ptr<std::string> assignedKey) {
    if (path.empty())
        throw std::invalid_argument("register read source: media path must not be empty");
    requireValidTiming(timing, "media path");
    const bool wantsProbe = probeCommittable(probe);
    return Command{
        "read media '" + path + "'", [network, node, path = std::move(path), timing = std::move(timing),
                                      probe = std::move(probe), wantsProbe, assignedKey](Document& document) {
            const NodeInstance* target = document.network(network).graph().node(node);
            if (target == nullptr)
                reject(GraphError::UnknownNode, "unknown Read node " + std::to_string(node));
            if (target->type != "source")
                reject(GraphError::ParameterValue,
                       "node '" + target->name + "' is not a Read node (" + target->type + ")");

            const std::string normalized = normalizedSourcePath(path);
            std::string key;
            const SourceReference* reused = findReusableReference(document, normalized, timing.interpretation, key);
            if (reused == nullptr) {
                key = uniqueSourceKey(document, path);
                SourceReference reference;
                reference.path = path;
                reference.frameOffset = timing.frameOffset;
                reference.frameStep = timing.frameStep;
                reference.firstFrame = timing.firstFrame;
                reference.lastFrame = timing.lastFrame;
                reference.interpretation = timing.interpretation;
                reference.revision = 1;
                document.setSourceReference(key, std::move(reference));

                MediaMetadata metadata;
                metadata.userName = document.mediaCatalog().nextAvailableName(kInvalidMediaBin, baseName(path));
                metadata.kind = MediaKind::Image;
                document.mediaCatalog().addEntry(key, kInvalidMediaBin, std::move(metadata));
            }
            if (assignedKey)
                *assignedKey = key;

            if (wantsProbe)
                commitProbe(document, key, probe);
            document.network(network).graph().setParam(node, "source", ParameterValue{std::string{key}});
        }};
}

Command relinkReadSourceCommand(std::string sourceKey, SourceReference expected, std::string path,
                                MediaProbeMetadata probe) {
    if (sourceKey.empty())
        throw std::invalid_argument("relink read source: source key must not be empty");
    if (path.empty())
        throw std::invalid_argument("relink read source: source '" + sourceKey + "' must reference a non-empty path");
    const bool wantsProbe = probeCommittable(probe);
    return Command{"relink read source '" + sourceKey + "'",
                   [sourceKey = std::move(sourceKey), expected = std::move(expected), path = std::move(path),
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
                           if (entry.sourceKey != sourceKey || !entry.metadata.committedProbe)
                               continue;
                           MediaMetadata metadata = entry.metadata;
                           metadata.committedProbe.reset();
                           document.mediaCatalog().setMetadata(entry.id, std::move(metadata));
                       }
                       if (wantsProbe)
                           commitProbe(document, sourceKey, probe);
                   }};
}

Command setReadSourceTimingCommand(std::string sourceKey, SourceReference expected, ReadSourceTiming timing) {
    if (sourceKey.empty())
        throw std::invalid_argument("set read source timing: source key must not be empty");
    requireValidTiming(timing, "source '" + sourceKey + "'");
    return Command{"set read source timing '" + sourceKey + "'",
                   [sourceKey = std::move(sourceKey), expected = std::move(expected),
                    timing = std::move(timing)](Document& document) {
                       const SourceReference& current = requireReference(document, sourceKey, "cannot set timing");
                       requireExpected(current, expected, sourceKey, "cannot set timing");
                       if (current.revision == std::numeric_limits<std::uint64_t>::max())
                           reject(GraphError::InvalidMediaQuery, "source '" + sourceKey + "' revision exhausted");
                       SourceReference updated = current;
                       updated.frameOffset = timing.frameOffset;
                       updated.frameStep = timing.frameStep;
                       updated.firstFrame = timing.firstFrame;
                       updated.lastFrame = timing.lastFrame;
                       updated.interpretation = timing.interpretation;
                       updated.revision = current.revision + 1;
                       document.setSourceReference(sourceKey, std::move(updated));
                   }};
}

}  // namespace nemo
