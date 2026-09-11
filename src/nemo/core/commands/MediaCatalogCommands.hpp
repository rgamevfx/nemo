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
[[nodiscard]] Command setMediaMarksCommand(MediaSourceId entry, std::vector<MediaMarkRange> marks);
[[nodiscard]] Command removeMediaEntryCommand(MediaSourceId entry);
[[nodiscard]] Command removeBinCommand(MediaBinId bin, bool keepContents = true);
[[nodiscard]] Command duplicateCatalogEntryCommand(MediaSourceId entry, MediaBinId parent = kInvalidMediaBin,
                                                   std::shared_ptr<MediaSourceId> createdId = {});
[[nodiscard]] Command duplicateCatalogEntryCommand(MediaSourceId entry, std::shared_ptr<MediaSourceId> createdId);
[[nodiscard]] Command setMediaQueryCommand(MediaBinId bin, std::optional<MediaQueryDescriptor> query);
[[nodiscard]] Command commitMediaProbeCommand(MediaSourceId entry, MediaProbeMetadata probe);

}  // namespace nemo
