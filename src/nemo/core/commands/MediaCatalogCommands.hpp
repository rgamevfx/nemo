#pragma once

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/MediaCatalog.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nemo {

[[nodiscard]] Command importMediaReferenceCommand(std::string sourceKey, MediaBinId parent = kInvalidMediaBin,
                                                  MediaMetadata metadata = {},
                                                  std::shared_ptr<MediaSourceId> createdId = {});
[[nodiscard]] Command importMediaReferenceCommand(std::string sourceKey, MediaMetadata metadata,
                                                  std::shared_ptr<MediaSourceId> createdId);
[[nodiscard]] Command importMediaReferenceCommand(std::string sourceKey, MediaBinId parent,
                                                  std::shared_ptr<MediaSourceId> createdId);
[[nodiscard]] Command importMediaReferenceCommand(std::string sourceKey, MediaMetadata metadata, MediaBinId parent,
                                                  std::shared_ptr<MediaSourceId> createdId = {});
[[nodiscard]] Command createBinCommand(std::string name, MediaBinId parent = kInvalidMediaBin,
                                       std::shared_ptr<MediaBinId> createdId = {});
[[nodiscard]] Command createBinCommand(std::string name, std::shared_ptr<MediaBinId> createdId);
[[nodiscard]] Command renameMediaCommand(MediaSourceId entry, std::string name);
[[nodiscard]] Command renameBinCommand(MediaBinId bin, std::string name);
[[nodiscard]] Command moveMediaCommand(MediaSourceId entry, MediaBinId parent);
[[nodiscard]] Command moveBinCommand(MediaBinId bin, MediaBinId parent);
[[nodiscard]] Command setMediaMetadataCommand(MediaSourceId entry, MediaMetadata metadata);
[[nodiscard]] Command setMediaBinMetadataCommand(MediaBinId bin, MediaBinMetadata metadata);
[[nodiscard]] Command setMediaMarksCommand(MediaSourceId entry, std::vector<MediaMarkRange> marks);
[[nodiscard]] Command removeMediaEntryCommand(MediaSourceId entry);
[[nodiscard]] Command removeBinCommand(MediaBinId bin, bool keepContents = true);
[[nodiscard]] Command duplicateCatalogEntryCommand(MediaSourceId entry, MediaBinId parent = kInvalidMediaBin,
                                                   std::shared_ptr<MediaSourceId> createdId = {});
[[nodiscard]] Command duplicateCatalogEntryCommand(MediaSourceId entry, std::shared_ptr<MediaSourceId> createdId);
[[nodiscard]] Command setMediaQueryCommand(MediaBinId bin, std::optional<MediaQueryDescriptor> query);
// Publishes decoded probe metadata onto a catalog entry. `expectedSource` must
// equal the entry's current Document::sources reference or the command rejects
// with GraphError::StaleMediaSource, so a probe result cannot overwrite a
// reference that was relinked or reinterpreted after the probe was requested.
// Only the entry's committedProbe changes; user metadata is preserved.
[[nodiscard]] Command commitMediaProbeCommand(MediaSourceId entry, SourceReference expectedSource,
                                              MediaProbeMetadata probe);
// Points the entry's existing source key at `path` while preserving the source
// identity, frame offset/step mapping and interpretation. The source revision
// advances by one, and the now-obsolete committed probe is cleared on every
// catalog entry that shares the source key. `expectedSource` must still match
// the current reference or the command rejects with GraphError::StaleMediaSource.
[[nodiscard]] Command relinkMediaSourceCommand(MediaSourceId entry, SourceReference expectedSource, std::string path);

}  // namespace nemo
