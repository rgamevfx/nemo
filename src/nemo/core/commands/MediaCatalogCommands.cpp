#include "nemo/core/commands/MediaCatalogCommands.hpp"

#include <limits>
#include <stdexcept>
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
                   [id, query = std::move(query)](Document& document) { document.mediaCatalog.setQuery(id, query); }};
}

Command commitMediaProbeCommand(MediaSourceId id, MediaProbeMetadata probe) {
    requireProbe(probe);
    return Command{"commit probe metadata on media entry " + std::to_string(id),
                   [id, probe = std::move(probe)](Document& document) {
                       requireProbe(probe);
                       const auto* entry = document.mediaCatalog.entry(id);
                       if (!entry)
                           reject(GraphError::UnknownMediaEntry,
                                  "cannot commit probe on unknown media entry " + std::to_string(id));
                       MediaMetadata metadata = entry->metadata;
                       metadata.committedProbe = probe;
                       document.mediaCatalog.setMetadata(id, std::move(metadata));
                   }};
}

}  // namespace nemo
