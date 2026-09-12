#include "nemo/core/commands/MediaCatalogCommands.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace nemo {
namespace {

void reject(GraphError code, const std::string& message) {
    throw GraphException(code, "media command: " + message);
}

void requireSource(const Document& document, const std::string& key) {
    if (key.empty())
        reject(GraphError::InvalidName, "source key must not be empty");
    if (!document.sources.contains(key))
        reject(GraphError::MissingMediaSource, "catalog entry source '" + key + "' is not in Document::sources");
}

void requireProbe(const MediaProbeMetadata& probe) {
    if (probe.width < 0 || probe.height < 0 || probe.duration < 0)
        reject(GraphError::InvalidMediaQuery, "probe dimensions and duration must be nonnegative");
    if (probe.provenance.empty())
        reject(GraphError::InvalidMediaQuery, "committed probe metadata must identify its provenance");
}

const MediaCatalogEntry& requireEntry(const Document& document, MediaSourceId id, std::string_view action) {
    const auto* entry = document.mediaCatalog.entry(id);
    if (!entry)
        reject(GraphError::UnknownMediaEntry, std::string(action) + " unknown media entry " + std::to_string(id));
    return *entry;
}

// Guards a revision-aware publication against a source reference that changed
// after the caller snapshotted it.
const SourceReference& requireExpectedSource(const Document& document, const MediaCatalogEntry& entry,
                                             const SourceReference& expected, std::string_view action) {
    const auto source = document.sources.find(entry.sourceKey);
    if (source == document.sources.end())
        reject(GraphError::MissingMediaSource, std::string(action) + ": media entry " + std::to_string(entry.id) +
                                                   " source '" + entry.sourceKey + "' is not in Document::sources");
    if (source->second != expected)
        reject(GraphError::StaleMediaSource, std::string(action) + ": media entry " + std::to_string(entry.id) +
                                                 " source '" + entry.sourceKey +
                                                 "' no longer matches the expected reference");
    return source->second;
}

}  // namespace

Command importMediaReferenceCommand(std::string sourceKey, MediaBinId parent, MediaMetadata metadata,
                                    std::shared_ptr<MediaSourceId> createdId) {
    return Command{
        "import media reference '" + sourceKey + "'",
        [sourceKey = std::move(sourceKey), parent, metadata = std::move(metadata), createdId](Document& document) {
            requireSource(document, sourceKey);
            if (metadata.committedProbe)
                reject(GraphError::InvalidMediaQuery, "probe metadata requires explicit commitMediaProbeCommand");
            const auto id = document.mediaCatalog.addEntry(sourceKey, parent, metadata);
            if (createdId)
                *createdId = id;
        }};
}
Command importMediaReferenceCommand(std::string sourceKey, MediaMetadata metadata,
                                    std::shared_ptr<MediaSourceId> createdId) {
    return importMediaReferenceCommand(std::move(sourceKey), kInvalidMediaBin, std::move(metadata),
                                       std::move(createdId));
}

Command importMediaReferenceCommand(std::string sourceKey, MediaBinId parent,
                                    std::shared_ptr<MediaSourceId> createdId) {
    return importMediaReferenceCommand(std::move(sourceKey), parent, MediaMetadata{}, std::move(createdId));
}

Command importMediaReferenceCommand(std::string sourceKey, MediaMetadata metadata, MediaBinId parent,
                                    std::shared_ptr<MediaSourceId> createdId) {
    return importMediaReferenceCommand(std::move(sourceKey), parent, std::move(metadata), std::move(createdId));
}

Command createBinCommand(std::string name, MediaBinId parent, std::shared_ptr<MediaBinId> createdId) {
    return Command{"create media bin '" + name + "'", [name = std::move(name), parent, createdId](Document& document) {
                       const auto id = document.mediaCatalog.addBin(name, parent);
                       if (createdId)
                           *createdId = id;
                   }};
}

Command createBinCommand(std::string name, std::shared_ptr<MediaBinId> createdId) {
    return createBinCommand(std::move(name), kInvalidMediaBin, std::move(createdId));
}
Command renameMediaCommand(MediaSourceId id, std::string name) {
    return Command{"rename media entry " + std::to_string(id),
                   [id, name = std::move(name)](Document& document) { document.mediaCatalog.renameEntry(id, name); }};
}

Command renameBinCommand(MediaBinId id, std::string name) {
    return Command{"rename media bin " + std::to_string(id),
                   [id, name = std::move(name)](Document& document) { document.mediaCatalog.renameBin(id, name); }};
}

Command moveMediaCommand(MediaSourceId id, MediaBinId parent) {
    return Command{"move media entry " + std::to_string(id),
                   [id, parent](Document& document) { document.mediaCatalog.moveEntry(id, parent); }};
}

Command moveBinCommand(MediaBinId id, MediaBinId parent) {
    return Command{"move media bin " + std::to_string(id),
                   [id, parent](Document& document) { document.mediaCatalog.moveBin(id, parent); }};
}

Command setMediaMetadataCommand(MediaSourceId id, MediaMetadata metadata) {
    if (metadata.committedProbe)
        throw std::invalid_argument("set media metadata cannot commit probe data; use commitMediaProbeCommand");
    return Command{"set metadata on media entry " + std::to_string(id),
                   [id, metadata = std::move(metadata)](Document& document) mutable {
                       const auto* entry = document.mediaCatalog.entry(id);
                       if (!entry)
                           reject(GraphError::UnknownMediaEntry,
                                  "cannot edit unknown media entry " + std::to_string(id));
                       metadata.committedProbe = entry->metadata.committedProbe;
                       document.mediaCatalog.setMetadata(id, metadata);
                   }};
}

Command setMediaBinMetadataCommand(MediaBinId id, MediaBinMetadata metadata) {
    return Command{"set metadata on media bin " + std::to_string(id),
                   [id, metadata = std::move(metadata)](Document& document) mutable {
                       document.mediaCatalog.setBinMetadata(id, std::move(metadata));
                   }};
}

Command setMediaMarksCommand(MediaSourceId id, std::vector<MediaMarkRange> marks) {
    return Command{"set marks on media entry " + std::to_string(id),
                   [id, marks = std::move(marks)](Document& document) { document.mediaCatalog.setMarks(id, marks); }};
}

Command removeMediaEntryCommand(MediaSourceId id) {
    return Command{
        "remove media entry " + std::to_string(id), [id](Document& document) {
            const auto* entry = document.mediaCatalog.entry(id);
            if (!entry)
                reject(GraphError::UnknownMediaEntry, "cannot remove unknown media entry " + std::to_string(id));
            if (document.mediaCatalog.sourceUsed(document, entry->sourceKey))
                reject(GraphError::MediaSourceInUse, "cannot remove entry " + std::to_string(id) + ": source '" +
                                                         entry->sourceKey + "' is addressed by a source node");
            document.mediaCatalog.removeEntry(id);
        }};
}

Command removeBinCommand(MediaBinId id, bool keepContents) {
    return Command{"remove media bin " + std::to_string(id),
                   [id, keepContents](Document& document) { document.mediaCatalog.removeBin(id, keepContents); }};
}

Command duplicateCatalogEntryCommand(MediaSourceId id, MediaBinId parent, std::shared_ptr<MediaSourceId> createdId) {
    return Command{
        "duplicate media entry " + std::to_string(id), [id, parent, createdId](Document& document) {
            const auto* original = document.mediaCatalog.entry(id);
            if (!original)
                reject(GraphError::UnknownMediaEntry, "cannot duplicate unknown media entry " + std::to_string(id));
            const MediaBinId destination = parent == kInvalidMediaBin ? original->parent : parent;
            MediaMetadata metadata = original->metadata;
            const std::string base = metadata.userName.empty() ? original->sourceKey : metadata.userName;
            metadata.userName = document.mediaCatalog.nextAvailableName(destination, base);
            const auto newId =
                document.mediaCatalog.addEntry(original->sourceKey, destination, std::move(metadata), original->marks);
            if (createdId)
                *createdId = newId;
        }};
}

Command duplicateCatalogEntryCommand(MediaSourceId id, std::shared_ptr<MediaSourceId> createdId) {
    return duplicateCatalogEntryCommand(id, kInvalidMediaBin, std::move(createdId));
}
Command setMediaQueryCommand(MediaBinId id, std::optional<MediaQueryDescriptor> query) {
    return Command{"set query on media bin " + std::to_string(id),
                   [id, query = std::move(query)](Document& document) mutable {
                       // A query authored against a missing bin is rejected up
                       // front; a scope removed later leaves the query
                       // unavailable rather than silently project-wide.
                       if (query && !document.mediaCatalog.queryScopeAvailable(*query))
                           reject(GraphError::UnknownMediaBin,
                                  "smart query scope bin " + std::to_string(*query->scope) + " does not exist");
                       document.mediaCatalog.setQuery(id, std::move(query));
                   }};
}

Command commitMediaProbeCommand(MediaSourceId id, SourceReference expectedSource, MediaProbeMetadata probe) {
    requireProbe(probe);
    return Command{"commit probe metadata on media entry " + std::to_string(id),
                   [id, expectedSource = std::move(expectedSource), probe = std::move(probe)](Document& document) {
                       requireProbe(probe);
                       const MediaCatalogEntry& entry = requireEntry(document, id, "cannot commit probe on");
                       requireExpectedSource(document, entry, expectedSource, "cannot commit probe on");
                       MediaMetadata metadata = entry.metadata;
                       metadata.committedProbe = probe;
                       document.mediaCatalog.setMetadata(id, std::move(metadata));
                   }};
}

Command relinkMediaSourceCommand(MediaSourceId id, SourceReference expectedSource, std::string path) {
    if (path.empty())
        throw std::invalid_argument("relink media source: media entry " + std::to_string(id) +
                                    " must reference a non-empty path");
    return Command{
        "relink media source of entry " + std::to_string(id),
        [id, expectedSource = std::move(expectedSource), path = std::move(path)](Document& document) {
            const MediaCatalogEntry& entry = requireEntry(document, id, "cannot relink");
            const SourceReference& current = requireExpectedSource(document, entry, expectedSource, "cannot relink");
            SourceReference relinked = current;
            relinked.path = path;
            if (relinked.frameStep == 0)
                throw std::invalid_argument("relink media source: source '" + entry.sourceKey +
                                            "' frameStep must not be zero");
            if (relinked.revision == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("relink media source: source '" + entry.sourceKey + "' revision exhausted");
            ++relinked.revision;
            document.sources[entry.sourceKey] = std::move(relinked);

            // The committed probe described the previous file; it is
            // obsolete for every entry that shares this source key.
            std::vector<MediaSourceId> obsoleteProbes;
            for (const auto& candidate : document.mediaCatalog.entries())
                if (candidate.sourceKey == entry.sourceKey && candidate.metadata.committedProbe)
                    obsoleteProbes.push_back(candidate.id);
            for (const MediaSourceId candidate : obsoleteProbes) {
                MediaMetadata metadata = document.mediaCatalog.entry(candidate)->metadata;
                metadata.committedProbe.reset();
                document.mediaCatalog.setMetadata(candidate, std::move(metadata));
            }
        }};
}

}  // namespace nemo
