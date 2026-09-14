#include "nemo/core/document/MediaCatalog.hpp"

#include "nemo/core/document/Document.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace nemo {
namespace {

void reject(GraphError code, const std::string& message) {
    throw GraphException(code, "media catalog: " + message);
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

std::uint64_t mixText(std::uint64_t hash, std::string_view value) noexcept {
    hash = mix(hash, static_cast<std::uint64_t>(value.size()));
    for (const unsigned char ch : value)
        hash = mix(hash, ch);
    return hash;
}

bool containsText(std::string_view value, std::string_view needle) {
    if (needle.empty())
        return true;
    return std::search(value.begin(), value.end(), needle.begin(), needle.end(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
           }) != value.end();
}

// Entry and bin metadata share one canonical tag form: sorted, deduplicated.
void normalizeTags(std::vector<std::string>& tags) {
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
}

}  // namespace

const char* coverageQualityName(CoverageQuality quality) noexcept {
    switch (quality) {
    case CoverageQuality::Unknown:
        return "unknown";
    case CoverageQuality::Estimated:
        return "estimated";
    case CoverageQuality::Validated:
        return "validated";
    }
    return "unknown";
}

bool MediaMarkRange::valid() const noexcept {
    return !inFrame || !outFrame || *inFrame <= *outFrame;
}

std::optional<std::string> probeFactProblem(const MediaProbeMetadata& probe) {
    if (probe.width < 0 || probe.height < 0 || probe.duration < 0)
        return "probe dimensions and duration must be nonnegative";
    if (probe.firstFrame && probe.lastFrame && *probe.firstFrame > *probe.lastFrame)
        return "probe firstFrame must not exceed lastFrame";
    if (probe.availableFrameCount && *probe.availableFrameCount < 0)
        return "probe availableFrameCount must be nonnegative";
    if (probe.missingFrameCount && *probe.missingFrameCount < 0)
        return "probe missingFrameCount must be nonnegative";
    if (probe.availableFrameCount && probe.missingFrameCount && *probe.missingFrameCount > *probe.availableFrameCount)
        return "probe missingFrameCount must not exceed availableFrameCount";
    if (!probe.missingRanges.empty() && !(probe.firstFrame && probe.lastFrame))
        return "probe missing ranges require the discovered firstFrame/lastFrame interval they describe";
    std::optional<std::int64_t> previousLast;
    for (const MediaFrameRange& hole : probe.missingRanges) {
        if (!hole.valid())
            return "probe missing range " + std::to_string(hole.first) + ".." + std::to_string(hole.last) +
                   " must not have first greater than last";
        if (previousLast && hole.first <= *previousLast)
            return "probe missing ranges must be sorted and non-overlapping (range starting at " +
                   std::to_string(hole.first) + " follows " + std::to_string(*previousLast) + ")";
        if (probe.firstFrame && (hole.first < *probe.firstFrame || hole.last > *probe.lastFrame))
            return "probe missing range " + std::to_string(hole.first) + ".." + std::to_string(hole.last) +
                   " lies outside the discovered interval";
        previousLast = hole.last;
    }
    if (probe.pixelAspect && (!std::isfinite(*probe.pixelAspect) || *probe.pixelAspect <= 0.0))
        return "probe pixelAspect must be finite and positive";
    if (probe.rateNumerator.has_value() != probe.rateDenominator.has_value())
        return "probe frame rate requires both rateNumerator and rateDenominator";
    if (probe.rateNumerator && (*probe.rateNumerator == 0 || *probe.rateDenominator == 0))
        return "probe frame rate numerator and denominator must be positive";
    return std::nullopt;
}

const MediaCatalogEntry* MediaCatalog::entry(MediaSourceId id) const noexcept {
    const std::size_t index = entries_.indexOf([id](const MediaCatalogEntry& value) { return value.id == id; });
    return index == entries_.size() ? nullptr : &entries_[index];
}
const MediaBin* MediaCatalog::bin(MediaBinId id) const noexcept {
    if (id == kInvalidMediaBin)
        return nullptr;
    const std::size_t index = bins_.indexOf([id](const MediaBin& value) { return value.id == id; });
    return index == bins_.size() ? nullptr : &bins_[index];
}
MediaCatalogEntry* MediaCatalog::mutableEntry(MediaSourceId id) noexcept {
    const std::size_t index = entries_.indexOf([id](const MediaCatalogEntry& value) { return value.id == id; });
    return index == entries_.size() ? nullptr : &entries_.mutableAt(index);
}
MediaBin* MediaCatalog::mutableBin(MediaBinId id) noexcept {
    if (id == kInvalidMediaBin)
        return nullptr;
    const std::size_t index = bins_.indexOf([id](const MediaBin& value) { return value.id == id; });
    return index == bins_.size() ? nullptr : &bins_.mutableAt(index);
}
void MediaCatalog::recordEntry(MediaSourceId id) noexcept {
    if (recorder_)
        recorder_->mediaEntry(id);
}
void MediaCatalog::recordBin(MediaBinId id) noexcept {
    if (recorder_)
        recorder_->mediaBin(id);
}

bool MediaCatalog::hasName(MediaBinId parent, std::string_view name, MediaSourceId exceptEntry,
                           MediaBinId exceptBin) const {
    if (name.empty())
        return true;
    for (const auto& value : entries_)
        if (value.parent == parent && value.id != exceptEntry && displayName(value) == name)
            return true;
    for (const auto& value : bins_)
        if (value.parent == parent && value.id != exceptBin && value.name == name)
            return true;
    return false;
}

bool MediaCatalog::isDescendant(MediaBinId candidate, MediaBinId ancestor) const noexcept {
    if (candidate == kInvalidMediaBin)
        return false;
    std::set<MediaBinId> seen;
    for (MediaBinId current = candidate; current != kInvalidMediaBin;) {
        if (!seen.insert(current).second)
            return true;
        if (current == ancestor)
            return true;
        const auto* value = bin(current);
        if (!value)
            return false;
        current = value->parent;
    }
    return false;
}

std::string MediaCatalog::displayName(const MediaCatalogEntry& value) const {
    return value.metadata.userName.empty() ? value.sourceKey : value.metadata.userName;
}
std::string MediaCatalog::nextAvailableName(MediaBinId parent, std::string_view base) const {
    std::string candidate(base);
    if (candidate.empty())
        candidate = "Media";
    if (!hasName(parent, candidate))
        return candidate;
    for (std::size_t suffix = 2;; ++suffix) {
        candidate = std::string(base) + " " + std::to_string(suffix);
        if (!hasName(parent, candidate))
            return candidate;
    }
}

std::vector<MediaBinId> MediaCatalog::childBins(MediaBinId parent) const {
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "parent bin " + std::to_string(parent) + " does not exist");
    std::vector<MediaBinId> result;
    for (const auto& value : bins_)
        if (value.parent == parent)
            result.push_back(value.id);
    std::sort(result.begin(), result.end(), [&](MediaBinId lhs, MediaBinId rhs) {
        const auto* a = bin(lhs);
        const auto* b = bin(rhs);
        return a->name == b->name ? lhs < rhs : a->name < b->name;
    });
    return result;
}

std::vector<MediaSourceId> MediaCatalog::childEntries(MediaBinId parent) const {
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "parent bin " + std::to_string(parent) + " does not exist");
    std::vector<MediaSourceId> result;
    for (const auto& value : entries_)
        if (value.parent == parent)
            result.push_back(value.id);
    std::sort(result.begin(), result.end(), [&](MediaSourceId lhs, MediaSourceId rhs) {
        const auto* a = entry(lhs);
        const auto* b = entry(rhs);
        const auto aName = displayName(*a);
        const auto bName = displayName(*b);
        return aName == bName ? lhs < rhs : aName < bName;
    });
    return result;
}

std::vector<MediaBinId> MediaCatalog::depthFirstBins(MediaBinId parent) const {
    std::vector<MediaBinId> result;
    std::set<MediaBinId> seen;
    const auto walk = [&](auto&& self, MediaBinId current) -> void {
        for (const auto id : childBins(current)) {
            if (!seen.insert(id).second)
                continue;
            result.push_back(id);
            self(self, id);
        }
    };
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "scope bin " + std::to_string(parent) + " does not exist");
    walk(walk, parent);
    return result;
}

std::vector<MediaSourceId> MediaCatalog::depthFirstEntries(MediaBinId parent) const {
    std::vector<MediaSourceId> result;
    std::set<MediaBinId> seen;
    const auto walk = [&](auto&& self, MediaBinId current) -> void {
        if (current != kInvalidMediaBin && !seen.insert(current).second)
            return;
        for (const auto id : childEntries(current))
            result.push_back(id);
        for (const auto id : childBins(current))
            self(self, id);
    };
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "scope bin " + std::to_string(parent) + " does not exist");
    walk(walk, parent);
    return result;
}

bool MediaCatalog::sourceUsed(const Document& document, std::string_view sourceKey) const {
    for (const auto& network : document.networks())
        for (const auto& node : network.graph().nodes()) {
            if (node.type != "source")
                continue;
            const auto parameter = node.params.find("source");
            if (parameter != node.params.end()) {
                const auto* value = std::get_if<std::string>(&parameter->second);
                if (value && *value == sourceKey)
                    return true;
            }
        }
    return false;
}

std::vector<MediaSourceId> MediaCatalog::searchCandidates(const Document& document,
                                                          std::vector<MediaSourceId> candidates, std::string_view text,
                                                          std::optional<MediaKind> kind, std::optional<bool> offline,
                                                          std::optional<bool> unused, bool includeImageSequences,
                                                          std::span<const MediaQuerySourceState> runtime) const {
    std::vector<MediaSourceId> result;
    result.reserve(candidates.size());
    for (const auto id : candidates) {
        const auto* value = entry(id);
        const auto& metadata = value->metadata;
        // Runtime facts overlay kind/offline only; authored metadata, text,
        // scope and unused stay canonical.
        MediaKind effectiveKind = metadata.kind;
        bool effectiveOffline = metadata.offline;
        for (const auto& state : runtime) {
            if (state.sourceKey == value->sourceKey) {
                effectiveKind = state.kind;
                effectiveOffline = state.offline;
                break;
            }
        }
        if (kind) {
            const bool matchesKind = effectiveKind == *kind;
            const bool matchesStillFamily =
                includeImageSequences && *kind == MediaKind::Image && effectiveKind == MediaKind::Sequence;
            if (!matchesKind && !matchesStillFamily)
                continue;
        }
        if (offline && effectiveOffline != *offline)
            continue;
        if (unused && !sourceUsed(document, value->sourceKey) != *unused)
            continue;
        const auto source = document.sources.find(value->sourceKey);
        const bool sourceText = source != document.sources.end() && containsText(source->second.path, text);
        if (!containsText(value->sourceKey, text) && !sourceText && !containsText(metadata.userName, text) &&
            !containsText(metadata.description, text) && !containsText(metadata.label, text) &&
            std::none_of(metadata.tags.begin(), metadata.tags.end(),
                         [text](const auto& tag) { return containsText(tag, text); }))
            continue;
        result.push_back(id);
    }
    return result;
}

std::vector<MediaSourceId> MediaCatalog::search(const Document& document, std::string_view text,
                                                std::optional<MediaKind> kind, std::optional<bool> offline,
                                                std::optional<bool> unused, MediaBinId scope) const {
    return searchCandidates(document, depthFirstEntries(scope), text, kind, offline, unused, false, {});
}

bool MediaCatalog::queryScopeAvailable(const MediaQueryDescriptor& query) const noexcept {
    // The root scope is always available even though it has no stored bin.
    return !query.scope || *query.scope == kInvalidMediaBin || bin(*query.scope) != nullptr;
}

std::vector<MediaSourceId> MediaCatalog::search(const Document& document, const MediaQueryDescriptor& query,
                                                std::optional<MediaBinId> scopeOverride,
                                                std::span<const MediaQuerySourceState> runtime) const {
    if (query.binsOnly)
        return {};
    const std::optional<MediaBinId> scope = scopeOverride ? scopeOverride : query.scope;
    std::vector<MediaSourceId> candidates;
    if (!scope) {
        // Whole project: traversal is always recursive, `recursive` only shapes
        // an engaged scope.
        candidates = depthFirstEntries(kInvalidMediaBin);
    } else if (*scope == kInvalidMediaBin) {
        candidates = query.recursive ? depthFirstEntries(kInvalidMediaBin) : childEntries(kInvalidMediaBin);
    } else if (bin(*scope) != nullptr) {
        candidates = query.recursive ? depthFirstEntries(*scope) : childEntries(*scope);
    } else {
        // The scoped bin was removed after the query was authored: the query is
        // unavailable and must not silently widen back to the whole project.
        return {};
    }
    return searchCandidates(document, std::move(candidates), query.text, query.kind, query.offline, query.unused,
                            query.includeImageSequences, runtime);
}

std::vector<MediaSourceId> MediaCatalog::smartMembers(const Document& document, MediaBinId id,
                                                      std::span<const MediaQuerySourceState> runtime) const {
    const auto* value = bin(id);
    if (!value)
        reject(GraphError::UnknownMediaBin, "cannot query unknown bin " + std::to_string(id));
    if (!value->query)
        return {};
    return search(document, *value->query, std::nullopt, runtime);
}

std::string MediaCatalog::path(MediaBinId id) const {
    if (id == kInvalidMediaBin)
        return "/";
    if (!bin(id))
        reject(GraphError::UnknownMediaBin, "cannot find path for unknown bin " + std::to_string(id));
    std::vector<std::string> names;
    std::set<MediaBinId> seen;
    for (MediaBinId current = id; current != kInvalidMediaBin;) {
        if (!seen.insert(current).second)
            reject(GraphError::MediaCycle, "bin path contains a cycle at bin " + std::to_string(current));
        const auto* value = bin(current);
        if (!value)
            reject(GraphError::UnknownMediaBin, "bin path references unknown parent");
        names.push_back(value->name);
        current = value->parent;
    }
    std::string result;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
        result += "/" + *it;
    return result.empty() ? "/" : result;
}

MediaSourceId MediaCatalog::addEntry(std::string sourceKey, MediaBinId parent, MediaMetadata metadata,
                                     std::vector<MediaMarkRange> marks, MediaSourceId id) {
    if (sourceKey.empty())
        reject(GraphError::InvalidName, "entry source key must not be empty");
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "entry parent bin " + std::to_string(parent) + " does not exist");
    if (id == kInvalidMediaSource) {
        if (nextEntryId_ == std::numeric_limits<MediaSourceId>::max())
            reject(GraphError::InvalidId, "media entry identity space is exhausted");
        id = nextEntryId_;
    } else if (id == std::numeric_limits<MediaSourceId>::max()) {
        reject(GraphError::InvalidId, "media entry id must be below the identity limit");
    }
    if (entry(id))
        reject(GraphError::DuplicateId, "media entry id " + std::to_string(id) + " already exists");
    normalizeTags(metadata.tags);
    for (const auto& mark : marks)
        if (!mark.valid())
            reject(GraphError::InvalidMediaMark, "entry '" + sourceKey + "' has an inverted mark range");
    if (hasName(parent, metadata.userName.empty() ? sourceKey : metadata.userName))
        reject(GraphError::MediaDuplicateName, "entry name already exists in its parent bin");
    entries_.push_back(MediaCatalogEntry{id, std::move(sourceKey), parent, std::move(metadata), std::move(marks)});
    nextEntryId_ = std::max(nextEntryId_, static_cast<MediaSourceId>(id + 1));
    recordEntry(id);
    return id;
}

MediaBinId MediaCatalog::addBin(std::string name, MediaBinId parent, std::optional<MediaQueryDescriptor> query,
                                MediaBinId id) {
    if (name.empty())
        reject(GraphError::InvalidName, "bin name must not be empty");
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "bin parent " + std::to_string(parent) + " does not exist");
    if (id == kInvalidMediaBin) {
        if (nextBinId_ == std::numeric_limits<MediaBinId>::max())
            reject(GraphError::InvalidId, "media bin identity space is exhausted");
        id = nextBinId_;
    } else if (id == std::numeric_limits<MediaBinId>::max()) {
        reject(GraphError::InvalidId, "media bin id must be below the identity limit");
    }
    if (bin(id))
        reject(GraphError::DuplicateId, "media bin id " + std::to_string(id) + " already exists");
    if (hasName(parent, name))
        reject(GraphError::MediaDuplicateName, "bin name '" + name + "' already exists in its parent");
    bins_.push_back(MediaBin{id, std::move(name), parent, std::move(query)});
    nextBinId_ = std::max(nextBinId_, static_cast<MediaBinId>(id + 1));
    recordBin(id);
    return id;
}

void MediaCatalog::removeEntry(MediaSourceId id) {
    const std::size_t index = entries_.indexOf([id](const MediaCatalogEntry& value) { return value.id == id; });
    if (index == entries_.size())
        reject(GraphError::UnknownMediaEntry, "cannot remove unknown media entry " + std::to_string(id));
    entries_.erase(index);
    recordEntry(id);
}

void MediaCatalog::removeBin(MediaBinId id, bool keepContents) {
    const std::size_t binIndex = bins_.indexOf([id](const MediaBin& value) { return value.id == id; });
    if (binIndex == bins_.size())
        reject(GraphError::UnknownMediaBin, "cannot remove unknown media bin " + std::to_string(id));
    const MediaBinId parent = bins_[binIndex].parent;
    const auto directBins = childBins(id);
    const auto directEntries = childEntries(id);
    if (!keepContents && (!directBins.empty() || !directEntries.empty()))
        reject(GraphError::MediaDuplicateName,
               "cannot remove non-empty bin " + std::to_string(id) + " without keepContents");
    if (keepContents) {
        std::set<std::string> names;
        for (const auto& value : entries_)
            if (value.parent == parent && value.id != kInvalidMediaSource)
                names.insert(displayName(value));
        for (const auto& value : bins_)
            if (value.parent == parent && value.id != id)
                names.insert(value.name);
        for (const auto child : directEntries) {
            const auto* value = entry(child);
            if (!names.insert(displayName(*value)).second)
                reject(GraphError::MediaDuplicateName, "keeping contents would collide at parent bin");
        }
        for (const auto child : directBins) {
            const auto* value = bin(child);
            if (!names.insert(value->name).second)
                reject(GraphError::MediaDuplicateName, "keeping contents would collide at parent bin");
        }
    }
    if (keepContents) {
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (entries_[index].parent != id)
                continue;
            entries_.mutableAt(index).parent = parent;
            recordEntry(entries_[index].id);
        }
        for (std::size_t index = 0; index < bins_.size(); ++index) {
            if (bins_[index].parent != id)
                continue;
            const MediaBinId child = bins_[index].id;
            bins_.mutableAt(index).parent = parent;
            recordBin(child);
        }
    }
    bins_.erase(bins_.indexOf([id](const MediaBin& value) { return value.id == id; }));
    recordBin(id);
}

void MediaCatalog::renameEntry(MediaSourceId id, std::string name) {
    auto* value = mutableEntry(id);
    if (!value)
        reject(GraphError::UnknownMediaEntry, "cannot rename unknown media entry " + std::to_string(id));
    if (name.empty())
        reject(GraphError::InvalidName, "entry name must not be empty");
    if (hasName(value->parent, name, id))
        reject(GraphError::MediaDuplicateName, "entry name '" + name + "' already exists in its parent");
    value->metadata.userName = std::move(name);
    recordEntry(id);
}

void MediaCatalog::renameBin(MediaBinId id, std::string name) {
    auto* value = mutableBin(id);
    if (!value)
        reject(GraphError::UnknownMediaBin, "cannot rename unknown media bin " + std::to_string(id));
    if (name.empty())
        reject(GraphError::InvalidName, "bin name must not be empty");
    if (hasName(value->parent, name, kInvalidMediaSource, id))
        reject(GraphError::MediaDuplicateName, "bin name '" + name + "' already exists in its parent");
    value->name = std::move(name);
    recordBin(id);
}

void MediaCatalog::moveEntry(MediaSourceId id, MediaBinId parent) {
    auto* value = mutableEntry(id);
    if (!value)
        reject(GraphError::UnknownMediaEntry, "cannot move unknown media entry " + std::to_string(id));
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "destination bin " + std::to_string(parent) + " does not exist");
    if (hasName(parent, displayName(*value), id))
        reject(GraphError::MediaDuplicateName, "moving entry would collide at destination bin");
    value->parent = parent;
    recordEntry(id);
}

void MediaCatalog::moveBin(MediaBinId id, MediaBinId parent) {
    auto* value = mutableBin(id);
    if (!value)
        reject(GraphError::UnknownMediaBin, "cannot move unknown media bin " + std::to_string(id));
    if (parent == id || isDescendant(parent, id))
        reject(GraphError::MediaCycle,
               "moving bin " + std::to_string(id) + " under its descendant would create a cycle");
    if (parent != kInvalidMediaBin && !bin(parent))
        reject(GraphError::UnknownMediaBin, "destination bin " + std::to_string(parent) + " does not exist");
    if (hasName(parent, value->name, kInvalidMediaSource, id))
        reject(GraphError::MediaDuplicateName, "moving bin would collide at destination bin");
    value->parent = parent;
    recordBin(id);
}

void MediaCatalog::setMetadata(MediaSourceId id, MediaMetadata metadata) {
    auto* value = mutableEntry(id);
    if (!value)
        reject(GraphError::UnknownMediaEntry, "cannot edit unknown media entry " + std::to_string(id));
    normalizeTags(metadata.tags);
    const auto name = metadata.userName.empty() ? value->sourceKey : metadata.userName;
    if (hasName(value->parent, name, id))
        reject(GraphError::MediaDuplicateName, "metadata name would collide in parent bin");
    value->metadata = std::move(metadata);
    recordEntry(id);
}

void MediaCatalog::setMarks(MediaSourceId id, std::vector<MediaMarkRange> marks) {
    auto* value = mutableEntry(id);
    if (!value)
        reject(GraphError::UnknownMediaEntry, "cannot edit marks on unknown media entry " + std::to_string(id));
    for (const auto& mark : marks)
        if (!mark.valid())
            reject(GraphError::InvalidMediaMark, "entry has an inverted mark range");
    value->marks = std::move(marks);
    recordEntry(id);
}

void MediaCatalog::setQuery(MediaBinId id, std::optional<MediaQueryDescriptor> query) {
    auto* value = mutableBin(id);
    if (!value)
        reject(GraphError::UnknownMediaBin, "cannot set query on unknown media bin " + std::to_string(id));
    value->query = std::move(query);
    recordBin(id);
}

void MediaCatalog::setBinMetadata(MediaBinId id, MediaBinMetadata metadata) {
    auto* value = mutableBin(id);
    if (!value)
        reject(GraphError::UnknownMediaBin, "cannot edit metadata on unknown media bin " + std::to_string(id));
    normalizeTags(metadata.tags);
    value->metadata = std::move(metadata);
    recordBin(id);
}

void MediaCatalog::restoreEntryExtension(MediaSourceId id, nlohmann::json extension) {
    MediaCatalogEntry* value = mutableEntry(id);
    if (value == nullptr)
        reject(GraphError::UnknownMediaEntry,
               "cannot attach preserved data to unknown media entry " + std::to_string(id));
    value->extension = std::move(extension);
}

void MediaCatalog::restoreBinExtension(MediaBinId id, nlohmann::json extension) {
    MediaBin* value = mutableBin(id);
    if (value == nullptr)
        reject(GraphError::UnknownMediaBin, "cannot attach preserved data to unknown media bin " + std::to_string(id));
    value->extension = std::move(extension);
}

void MediaCatalog::restoreIdentityHighWatermarks(MediaSourceId nextEntryId, MediaBinId nextBinId) {
    if (nextEntryId == kInvalidMediaSource || nextBinId == kInvalidMediaBin)
        reject(GraphError::InvalidId, "media identity high watermarks must be nonzero");
    nextEntryId_ = std::max(nextEntryId_, nextEntryId);
    nextBinId_ = std::max(nextBinId_, nextBinId);
}

void MediaCatalog::preserveIdentityHighWatermarksFrom(const MediaCatalog& source) {
    restoreIdentityHighWatermarks(source.nextEntryId_, source.nextBinId_);
}

std::uint64_t MediaCatalog::stateHash() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash = mix(hash, nextEntryId_);
    hash = mix(hash, nextBinId_);
    for (const auto& value : bins_) {
        hash = mix(hash, value.id);
        hash = mixText(hash, value.name);
        hash = mix(hash, value.parent);
        if (value.query) {
            hash = mixText(hash, value.query->text);
            hash = mix(hash, value.query->kind ? static_cast<std::uint64_t>(*value.query->kind) + 1 : 0);
            hash = mix(hash, value.query->offline ? (*value.query->offline ? 2 : 1) : 0);
            hash = mix(hash, value.query->unused ? (*value.query->unused ? 2 : 1) : 0);
            hash = mix(hash, value.query->scope ? 1 : 0);
            if (value.query->scope)
                hash = mix(hash, *value.query->scope);
            hash = mix(hash, value.query->recursive ? 1 : 0);
            hash = mix(hash, value.query->includeImageSequences ? 1 : 0);
            hash = mix(hash, value.query->binsOnly ? 1 : 0);
        }
        hash = mix(hash, value.query ? 1 : 0);
        hash = mixText(hash, value.metadata.description);
        hash = mixText(hash, value.metadata.label);
        for (const auto& tag : value.metadata.tags)
            hash = mixText(hash, tag);
        hash = mix(hash, value.metadata.tags.size());
    }
    for (const auto& value : entries_) {
        hash = mix(hash, value.id);
        hash = mixText(hash, value.sourceKey);
        hash = mix(hash, value.parent);
        hash = mixText(hash, value.metadata.userName);
        hash = mixText(hash, value.metadata.description);
        hash = mixText(hash, value.metadata.label);
        hash = mix(hash, value.metadata.offline ? 1 : 0);
        hash = mix(hash, static_cast<std::uint64_t>(value.metadata.kind));
        for (const auto& tag : value.metadata.tags)
            hash = mixText(hash, tag);
        hash = mix(hash, value.metadata.tags.size());
        if (value.metadata.committedProbe) {
            const auto& probe = *value.metadata.committedProbe;
            hash = mix(hash, static_cast<std::uint64_t>(probe.width));
            hash = mix(hash, static_cast<std::uint64_t>(probe.height));
            hash = mix(hash, static_cast<std::uint64_t>(probe.duration));
            hash = mixText(hash, probe.codec);
            hash = mixText(hash, probe.colorPrimaries);
            hash = mixText(hash, probe.colorTransfer);
            hash = mixText(hash, probe.colorMatrix);
            hash = mixText(hash, probe.provenance);
            hash = mix(hash, static_cast<std::uint64_t>(probe.status));
            // Discovered source facts (issue #75) are part of publication
            // freshness: a reload that rediscovers coverage must advance the
            // document revision so stale publications are discarded and
            // affected results are invalidated.
            hash = mix(hash, probe.firstFrame ? static_cast<std::uint64_t>(*probe.firstFrame) + 1 : 0);
            hash = mix(hash, probe.lastFrame ? static_cast<std::uint64_t>(*probe.lastFrame) + 1 : 0);
            hash = mix(hash, probe.firstFrame ? 1 : 0);
            hash = mix(hash, probe.lastFrame ? 1 : 0);
            hash = mix(hash, static_cast<std::uint64_t>(probe.coverageQuality));
            hash =
                mix(hash, probe.availableFrameCount ? static_cast<std::uint64_t>(*probe.availableFrameCount) + 1 : 0);
            hash = mix(hash, probe.missingFrameCount ? static_cast<std::uint64_t>(*probe.missingFrameCount) + 1 : 0);
            hash = mix(hash, probe.availableFrameCount ? 1 : 0);
            hash = mix(hash, probe.missingFrameCount ? 1 : 0);
            for (const MediaFrameRange& hole : probe.missingRanges) {
                hash = mix(hash, static_cast<std::uint64_t>(hole.first));
                hash = mix(hash, static_cast<std::uint64_t>(hole.last));
            }
            hash = mix(hash, probe.missingRanges.size());
            hash = mix(hash, probe.pixelAspect ? static_cast<std::uint64_t>(*probe.pixelAspect) : 0);
            hash = mix(hash, probe.pixelAspect ? 1 : 0);
            hash = mix(hash, probe.rateNumerator ? *probe.rateNumerator : 0);
            hash = mix(hash, probe.rateDenominator ? *probe.rateDenominator : 0);
            hash = mix(hash, probe.rateNumerator ? 1 : 0);
            hash = mix(hash, probe.rateDenominator ? 1 : 0);
            hash = mixText(hash, probe.precision);
            hash = mixText(hash, probe.channels);
            hash = mixText(hash, probe.declaredInputColorSpace);
        }
        hash = mix(hash, value.metadata.committedProbe ? 1 : 0);
        for (const auto& mark : value.marks) {
            hash = mix(hash, mark.inFrame ? static_cast<std::uint64_t>(*mark.inFrame) : 0);
            hash = mix(hash, mark.outFrame ? static_cast<std::uint64_t>(*mark.outFrame) : 0);
            hash = mix(hash, mark.inFrame ? 1 : 0);
            hash = mix(hash, mark.outFrame ? 1 : 0);
        }
        hash = mix(hash, value.marks.size());
    }
    return hash;
}

}  // namespace nemo
