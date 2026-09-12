#pragma once

#include "nemo/core/document/Ids.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nemo {

struct Document;

enum class MediaKind { Unknown, Image, Video, Audio, Sequence, Other };
enum class MediaProbeStatus { Unknown, Pending, Ready, Failed };

struct MediaMarkRange {
    std::optional<std::int64_t> inFrame;
    std::optional<std::int64_t> outFrame;
    // Authored fields of the persisted mark this build does not model, retained
    // verbatim for lossless save.
    nlohmann::json extension{};
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool operator==(const MediaMarkRange&) const = default;
};

// Plain persistent/proposed probe data. It intentionally contains no decoder,
// GPU, or framework handles. A proposal is persistent only after a commit command.
struct MediaProbeMetadata {
    std::int64_t width{};
    std::int64_t height{};
    std::int64_t duration{};
    std::string codec;
    std::string colorPrimaries;
    std::string colorTransfer;
    std::string colorMatrix;
    std::string provenance;
    MediaProbeStatus status{MediaProbeStatus::Unknown};
    // Authored fields of the persisted probe this build does not model,
    // retained verbatim for lossless save.
    nlohmann::json extension{};
    [[nodiscard]] bool operator==(const MediaProbeMetadata&) const = default;
};
using MediaProbeResult = MediaProbeMetadata;
using MediaProbeProposal = MediaProbeMetadata;

struct MediaMetadata {
    std::string userName;
    std::string description;
    std::vector<std::string> tags;
    std::string label;
    bool offline{false};
    MediaKind kind{MediaKind::Unknown};
    std::optional<MediaProbeMetadata> committedProbe;
    // Authored fields of the persisted metadata this build does not model,
    // retained verbatim for lossless save.
    nlohmann::json extension{};
    [[nodiscard]] bool operator==(const MediaMetadata&) const = default;
};

struct MediaCatalogEntry {
    MediaSourceId id{kInvalidMediaSource};
    std::string sourceKey;
    MediaBinId parent{kInvalidMediaBin};
    MediaMetadata metadata;
    std::vector<MediaMarkRange> marks;
    // Authored fields of the persisted entry this build does not model, retained
    // verbatim for lossless save.
    nlohmann::json extension{};
    [[nodiscard]] bool operator==(const MediaCatalogEntry&) const = default;
};

struct MediaQueryDescriptor {
    std::string text;
    std::optional<MediaKind> kind;
    std::optional<bool> offline;
    std::optional<bool> unused;
    // Authored fields of the persisted smart-bin query this build does not
    // model, retained verbatim for lossless save.
    nlohmann::json extension{};
    [[nodiscard]] bool operator==(const MediaQueryDescriptor&) const = default;
};

struct MediaBin {
    MediaBinId id{kInvalidMediaBin};
    std::string name;
    MediaBinId parent{kInvalidMediaBin};
    std::optional<MediaQueryDescriptor> query;
    // Authored fields of the persisted bin this build does not model, retained
    // verbatim for lossless save.
    nlohmann::json extension{};
    [[nodiscard]] bool operator==(const MediaBin&) const = default;
};

class MediaCatalog final {
public:
    [[nodiscard]] const MediaCatalogEntry* findEntry(MediaSourceId id) const noexcept { return entry(id); }
    [[nodiscard]] const MediaBin* findBin(MediaBinId id) const noexcept { return bin(id); }
    [[nodiscard]] std::vector<MediaSourceId> depthFirst(MediaBinId parent = kInvalidMediaBin) const {
        return depthFirstEntries(parent);
    }
    MediaCatalog() = default;

    [[nodiscard]] const std::vector<MediaCatalogEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::vector<MediaBin>& bins() const noexcept { return bins_; }
    [[nodiscard]] const MediaCatalogEntry* entry(MediaSourceId id) const noexcept;
    [[nodiscard]] MediaCatalogEntry* entry(MediaSourceId id) noexcept;
    [[nodiscard]] const MediaBin* bin(MediaBinId id) const noexcept;
    [[nodiscard]] MediaBin* bin(MediaBinId id) noexcept;

    [[nodiscard]] std::vector<MediaBinId> childBins(MediaBinId parent = kInvalidMediaBin) const;
    [[nodiscard]] std::vector<MediaSourceId> childEntries(MediaBinId parent = kInvalidMediaBin) const;
    [[nodiscard]] std::vector<MediaBinId> depthFirstBins(MediaBinId parent = kInvalidMediaBin) const;
    [[nodiscard]] std::vector<MediaSourceId> depthFirstEntries(MediaBinId parent = kInvalidMediaBin) const;
    [[nodiscard]] std::vector<MediaSourceId> search(const Document& document, std::string_view text = {},
                                                    std::optional<MediaKind> kind = {},
                                                    std::optional<bool> offline = {}, std::optional<bool> unused = {},
                                                    MediaBinId scope = kInvalidMediaBin) const;
    [[nodiscard]] std::vector<MediaSourceId> smartMembers(const Document& document, MediaBinId bin) const;
    [[nodiscard]] std::string path(MediaBinId bin) const;
    [[nodiscard]] bool sourceUsed(const Document& document, std::string_view sourceKey) const;
    [[nodiscard]] std::uint64_t stateHash() const noexcept;
    [[nodiscard]] MediaSourceId nextEntryId() const noexcept { return nextEntryId_; }
    [[nodiscard]] MediaBinId nextBinId() const noexcept { return nextBinId_; }
    void restoreIdentityHighWatermarks(MediaSourceId nextEntryId, MediaBinId nextBinId);
    [[nodiscard]] std::vector<MediaSourceId> search(const Document& document, const MediaQueryDescriptor& query,
                                                    MediaBinId scope = kInvalidMediaBin) const;
    [[nodiscard]] bool isSourceUsed(const Document& document, std::string_view sourceKey) const {
        return sourceUsed(document, sourceKey);
    }
    [[nodiscard]] std::string nextAvailableName(MediaBinId parent, std::string_view base) const;
    void preserveIdentityHighWatermarksFrom(const MediaCatalog& source);

    // Mutation primitives are used by validated command implementations.
    MediaSourceId addEntry(std::string sourceKey, MediaBinId parent, MediaMetadata metadata,
                           std::vector<MediaMarkRange> marks = {}, MediaSourceId id = kInvalidMediaSource);
    MediaBinId addBin(std::string name, MediaBinId parent = kInvalidMediaBin,
                      std::optional<MediaQueryDescriptor> query = {}, MediaBinId id = kInvalidMediaBin);
    void removeEntry(MediaSourceId id);
    void removeBin(MediaBinId id, bool keepContents);
    void renameEntry(MediaSourceId id, std::string name);
    void renameBin(MediaBinId id, std::string name);
    void moveEntry(MediaSourceId id, MediaBinId parent);
    void moveBin(MediaBinId id, MediaBinId parent);
    void setMetadata(MediaSourceId id, MediaMetadata metadata);
    void setMarks(MediaSourceId id, std::vector<MediaMarkRange> marks);
    void setQuery(MediaBinId id, std::optional<MediaQueryDescriptor> query);

private:
    [[nodiscard]] bool hasName(MediaBinId parent, std::string_view name,
                               MediaSourceId exceptEntry = kInvalidMediaSource,
                               MediaBinId exceptBin = kInvalidMediaBin) const;
    [[nodiscard]] bool isDescendant(MediaBinId candidate, MediaBinId ancestor) const noexcept;
    [[nodiscard]] std::string displayName(const MediaCatalogEntry& entry) const;
    std::vector<MediaCatalogEntry> entries_;
    std::vector<MediaBin> bins_;
    MediaSourceId nextEntryId_{1};
    MediaBinId nextBinId_{1};
};

}  // namespace nemo
