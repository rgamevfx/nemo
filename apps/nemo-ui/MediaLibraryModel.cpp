#include "MediaLibraryModel.hpp"

#include "NativeFileChooser.hpp"
#include "WorkspaceController.hpp"
#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/evaluation/Image.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace nemo::ui {
namespace {

using nemo::Command;
using nemo::Document;
using nemo::GraphException;
using nemo::MediaBin;
using nemo::MediaBinId;
using nemo::MediaCatalogEntry;
using nemo::MediaKind;
using nemo::MediaMetadata;
using nemo::MediaProbeMetadata;
using nemo::MediaProbeStatus;
using nemo::MediaQueryDescriptor;
using nemo::MediaSourceId;
using nemo::SourceReference;

using nemo::kInvalidMediaBin;
using nemo::kInvalidMediaSource;

constexpr int kPollIntervalMs = 40;
constexpr std::size_t kStoredProbeLimit = 64;
constexpr std::size_t kQueuedRequestLimit = 256;
constexpr int kThumbnailCacheCapacity = 128;
constexpr std::int64_t kMaxCatalogNameLength = 128;

// --- Identity: stable strings distinct for bins and media ------------------

QString binIdentity(MediaBinId id) {
    return id == kInvalidMediaBin ? QStringLiteral("root") : QStringLiteral("bin-") + QString::number(id);
}

QString entryIdentity(MediaSourceId id) {
    return QStringLiteral("media-") + QString::number(id);
}

bool parseBinIdentity(const QString& value, MediaBinId& out) {
    const QString id = value.trimmed();
    if (id.isEmpty() || id == QLatin1String("root")) {
        out = kInvalidMediaBin;
        return true;
    }
    if (!id.startsWith(QLatin1String("bin-"))) {
        return false;
    }
    bool ok = false;
    const qulonglong parsed = id.mid(4).toULongLong(&ok);
    if (!ok || parsed == 0) {
        return false;
    }
    out = static_cast<MediaBinId>(parsed);
    return true;
}

bool parseEntryIdentity(const QString& value, MediaSourceId& out) {
    const QString id = value.trimmed();
    if (!id.startsWith(QLatin1String("media-"))) {
        return false;
    }
    bool ok = false;
    const qulonglong parsed = id.mid(6).toULongLong(&ok);
    if (!ok || parsed == 0) {
        return false;
    }
    out = static_cast<MediaSourceId>(parsed);
    return true;
}

struct RecordRef {
    bool valid{false};
    bool bin{false};
    MediaBinId binId{kInvalidMediaBin};
    MediaSourceId entryId{kInvalidMediaSource};
    MediaBinId parent{kInvalidMediaBin};
};

RecordRef lookupRecord(const Document& document, const QString& id) {
    RecordRef ref;
    const QString value = id.trimmed();
    if (value.isEmpty() || value == QLatin1String("root")) {
        ref.valid = true;
        ref.bin = true;
        return ref;
    }
    if (value.startsWith(QLatin1String("bin-"))) {
        bool ok = false;
        const qulonglong parsed = value.mid(4).toULongLong(&ok);
        if (!ok || parsed == 0) {
            return ref;
        }
        const MediaBin* bin = document.mediaCatalog.bin(static_cast<MediaBinId>(parsed));
        if (!bin) {
            return ref;
        }
        ref.valid = true;
        ref.bin = true;
        ref.binId = bin->id;
        ref.parent = bin->parent;
        return ref;
    }
    if (value.startsWith(QLatin1String("media-"))) {
        bool ok = false;
        const qulonglong parsed = value.mid(6).toULongLong(&ok);
        if (!ok || parsed == 0) {
            return ref;
        }
        const MediaCatalogEntry* entry = document.mediaCatalog.entry(static_cast<MediaSourceId>(parsed));
        if (!entry) {
            return ref;
        }
        ref.valid = true;
        ref.entryId = entry->id;
        ref.parent = entry->parent;
        return ref;
    }
    return ref;
}

bool ancestorOf(const Document& document, const QString& ancestorId, const QString& recordId) {
    if (ancestorId == recordId) {
        return false;
    }
    QString current = recordId;
    for (int guard = 0; guard < 1024; ++guard) {
        const RecordRef ref = lookupRecord(document, current);
        if (!ref.valid) {
            return false;
        }
        const QString parentId = binIdentity(ref.parent);
        if (parentId == ancestorId) {
            return true;
        }
        if (ref.parent == kInvalidMediaBin) {
            return false;
        }
        current = parentId;
    }
    return false;
}

QVariantList normalizeIds(const Document& document, const QVariantList& ids) {
    QStringList unique;
    for (const QVariant& value : ids) {
        const QString id = value.toString().trimmed();
        if (id.isEmpty() || id == QLatin1String("root")) {
            continue;
        }
        if (!lookupRecord(document, id).valid || unique.contains(id)) {
            continue;
        }
        unique.append(id);
    }
    QStringList selected;
    for (const QString& id : unique) {
        bool hasSelectedAncestor = false;
        for (const QString& other : unique) {
            if (other != id && ancestorOf(document, other, id)) {
                hasSelectedAncestor = true;
                break;
            }
        }
        if (!hasSelectedAncestor) {
            selected.append(id);
        }
    }
    QVariantList out;
    out.reserve(selected.size());
    for (const QString& id : selected) {
        out.push_back(id);
    }
    return out;
}

// --- Record fields ---------------------------------------------------------

QString prototypeKind(MediaKind kind) {
    switch (kind) {
    case MediaKind::Image:
    case MediaKind::Sequence:
        return QStringLiteral("still");
    case MediaKind::Audio:
        return QStringLiteral("audio");
    case MediaKind::Video:
        return QStringLiteral("video");
    case MediaKind::Unknown:
        // Never substituted with a media kind: an unprobed item is honestly
        // unknown, and the panel falls back to its neutral glyph.
        return QStringLiteral("unknown");
    case MediaKind::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("unknown");
}

QString mediaKindName(MediaKind kind) {
    switch (kind) {
    case MediaKind::Image:
        return QStringLiteral("image");
    case MediaKind::Video:
        return QStringLiteral("video");
    case MediaKind::Audio:
        return QStringLiteral("audio");
    case MediaKind::Sequence:
        return QStringLiteral("sequence");
    case MediaKind::Other:
        return QStringLiteral("other");
    case MediaKind::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QString probeStatusName(MediaProbeStatus status) {
    switch (status) {
    case MediaProbeStatus::Pending:
        return QStringLiteral("pending");
    case MediaProbeStatus::Ready:
        return QStringLiteral("ready");
    case MediaProbeStatus::Failed:
        return QStringLiteral("failed");
    case MediaProbeStatus::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QString displayName(const MediaCatalogEntry& entry) {
    return QString::fromStdString(entry.metadata.userName.empty() ? entry.sourceKey : entry.metadata.userName);
}

bool containsLower(const std::string& haystack, const QString& needleLower) {
    if (needleLower.isEmpty()) {
        return true;
    }
    return QString::fromStdString(haystack).toLower().contains(needleLower);
}

// True when any entry in the bin's subtree addresses a source used by a source
// node (the prototype's bin-level used semantics).
bool binHasUsedDescendant(const Document& document, MediaBinId bin) {
    for (const MediaSourceId id : document.mediaCatalog.depthFirstEntries(bin)) {
        const MediaCatalogEntry* entry = document.mediaCatalog.entry(id);
        if (entry && document.mediaCatalog.sourceUsed(document, entry->sourceKey)) {
            return true;
        }
    }
    return false;
}

bool subtreeHasUsed(const Document& document, const RecordRef& ref) {
    if (!ref.bin) {
        const MediaCatalogEntry* entry = document.mediaCatalog.entry(ref.entryId);
        return entry && document.mediaCatalog.sourceUsed(document, entry->sourceKey);
    }
    return binHasUsedDescendant(document, ref.binId);
}

// Prototype `_matchesText` for a bin: name, description and tags (a bin has no
// source identity).
bool binMatchesText(const MediaBin& bin, const QString& needleLower) {
    if (containsLower(bin.name, needleLower) || containsLower(bin.metadata.description, needleLower)) {
        return true;
    }
    for (const std::string& tag : bin.metadata.tags) {
        if (containsLower(tag, needleLower)) {
            return true;
        }
    }
    return false;
}

bool validCatalogName(const QString& name) {
    if (name.isEmpty() || name.size() > kMaxCatalogNameLength || name.trimmed().isEmpty()) {
        return false;
    }
    for (const QChar character : name) {
        if (character.unicode() < 32 || character.unicode() == 127) {
            return false;
        }
    }
    return true;
}

bool validHexColor(const QString& value) {
    if (value.isEmpty()) {
        return true;
    }
    if (value.size() != 7 && value.size() != 9) {
        return false;
    }
    if (value.at(0) != QLatin1Char('#')) {
        return false;
    }
    for (int i = 1; i < value.size(); ++i) {
        const QChar character = value.at(i);
        if (!(character.isDigit() || (character >= QLatin1Char('a') && character <= QLatin1Char('f')) ||
              (character >= QLatin1Char('A') && character <= QLatin1Char('F')))) {
            return false;
        }
    }
    return true;
}

// A descriptor carrying Image also covers sequences for the "Still" family
// (the UI's kindFilter "still"); a descriptor without a kind is every media
// kind, including unknown/other.
QString kindFilterFromDescriptor(const MediaQueryDescriptor& query) {
    if (query.binsOnly) {
        return QStringLiteral("bin");
    }
    if (!query.kind) {
        return QStringLiteral("all");
    }
    switch (*query.kind) {
    case MediaKind::Video:
        return QStringLiteral("video");
    case MediaKind::Audio:
        return QStringLiteral("audio");
    case MediaKind::Image:
    case MediaKind::Sequence:
        return QStringLiteral("still");
    case MediaKind::Other:
        return QStringLiteral("other");
    case MediaKind::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("all");
}

QString objectString(const QVariantMap& object, const QString& key) {
    return object.value(key).toString();
}

// --- JSON <-> QVariant bridging for the prototype query record --------------

QVariant jsonToVariant(const nlohmann::json& value) {
    switch (value.type()) {
    case nlohmann::json::value_t::boolean:
        return QVariant(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return QVariant(static_cast<qlonglong>(value.get<std::int64_t>()));
    case nlohmann::json::value_t::number_unsigned:
        return QVariant(static_cast<qulonglong>(value.get<std::uint64_t>()));
    case nlohmann::json::value_t::number_float:
        return QVariant(value.get<double>());
    case nlohmann::json::value_t::string:
        return QVariant(QString::fromStdString(value.get<std::string>()));
    case nlohmann::json::value_t::array: {
        QVariantList list;
        for (const auto& item : value) {
            list.push_back(jsonToVariant(item));
        }
        return list;
    }
    case nlohmann::json::value_t::object: {
        QVariantMap map;
        for (auto it = value.begin(); it != value.end(); ++it) {
            map.insert(QString::fromStdString(it.key()), jsonToVariant(it.value()));
        }
        return map;
    }
    default:
        break;
    }
    return {};
}

QVariantMap jsonToVariantMap(const nlohmann::json& value) {
    if (!value.is_object()) {
        return {};
    }
    QVariantMap map;
    for (auto it = value.begin(); it != value.end(); ++it) {
        map.insert(QString::fromStdString(it.key()), jsonToVariant(it.value()));
    }
    return map;
}

nlohmann::json runtimeExtension(const nemo::media::MediaImportResult& result) {
    nlohmann::json runtime = nlohmann::json::object();
    runtime["frameRate"] = result.frameRate;
    runtime["pixelAspect"] = result.pixelAspect;
    runtime["pixelFormat"] = result.pixelFormat;
    runtime["bitDepth"] = result.bitDepth;
    runtime["colorRange"] = result.colorRange;
    runtime["chromaLocation"] = result.chromaLocation;
    runtime["hardware"] = result.hardware;
    runtime["fallbackReason"] = result.fallbackReason;
    runtime["streamIndex"] = result.streamIndex;
    runtime["profile"] = result.profile;
    runtime["planeCount"] = result.planeCount;
    return runtime;
}

QVariantMap runtimeFromResult(const nemo::media::MediaImportResult& result) {
    QVariantMap runtime;
    runtime.insert(QStringLiteral("frameRate"), result.frameRate);
    runtime.insert(QStringLiteral("pixelAspect"), result.pixelAspect);
    runtime.insert(QStringLiteral("pixelFormat"), QString::fromStdString(result.pixelFormat));
    runtime.insert(QStringLiteral("bitDepth"), result.bitDepth);
    runtime.insert(QStringLiteral("colorRange"), QString::fromStdString(result.colorRange));
    runtime.insert(QStringLiteral("chromaLocation"), QString::fromStdString(result.chromaLocation));
    runtime.insert(QStringLiteral("hardware"), result.hardware);
    runtime.insert(QStringLiteral("fallbackReason"), QString::fromStdString(result.fallbackReason));
    runtime.insert(QStringLiteral("streamIndex"), result.streamIndex);
    runtime.insert(QStringLiteral("profile"), QString::fromStdString(result.profile));
    runtime.insert(QStringLiteral("planeCount"), result.planeCount);
    return runtime;
}

QVariantMap probeRecord(const MediaProbeMetadata& probe) {
    QVariantMap map;
    map.insert(QStringLiteral("width"), static_cast<qlonglong>(probe.width));
    map.insert(QStringLiteral("height"), static_cast<qlonglong>(probe.height));
    map.insert(QStringLiteral("duration"), static_cast<qlonglong>(probe.duration));
    map.insert(QStringLiteral("codec"), QString::fromStdString(probe.codec));
    map.insert(QStringLiteral("colorPrimaries"), QString::fromStdString(probe.colorPrimaries));
    map.insert(QStringLiteral("colorTransfer"), QString::fromStdString(probe.colorTransfer));
    map.insert(QStringLiteral("colorMatrix"), QString::fromStdString(probe.colorMatrix));
    map.insert(QStringLiteral("provenance"), QString::fromStdString(probe.provenance));
    map.insert(QStringLiteral("status"), probeStatusName(probe.status));
    if (probe.extension.is_object() && probe.extension.contains("runtime")) {
        map.insert(QStringLiteral("runtime"), jsonToVariant(probe.extension.at("runtime")));
    }
    return map;
}

QVariantMap runtimeRecord(const MediaMetadata& metadata) {
    QVariantMap runtime;
    runtime.insert(QStringLiteral("frameRate"), 0.0);
    runtime.insert(QStringLiteral("pixelAspect"), 1.0);
    runtime.insert(QStringLiteral("pixelFormat"), QString());
    runtime.insert(QStringLiteral("bitDepth"), 0);
    runtime.insert(QStringLiteral("colorRange"), QString());
    runtime.insert(QStringLiteral("chromaLocation"), QString());
    runtime.insert(QStringLiteral("hardware"), false);
    runtime.insert(QStringLiteral("fallbackReason"), QString());
    runtime.insert(QStringLiteral("streamIndex"), 0);
    runtime.insert(QStringLiteral("profile"), QString());
    runtime.insert(QStringLiteral("planeCount"), 0);
    if (metadata.committedProbe && metadata.committedProbe->extension.is_object() &&
        metadata.committedProbe->extension.contains("runtime")) {
        const QVariantMap stored = jsonToVariantMap(metadata.committedProbe->extension.at("runtime"));
        for (auto it = stored.begin(); it != stored.end(); ++it) {
            runtime.insert(it.key(), it.value());
        }
    }
    return runtime;
}

// The prototype's panel-local query record, synthesized from the typed
// descriptor (the typed descriptor is the only persisted form). The panel's
// projectScope/parentId pair is exactly the typed scope: nullopt is project,
// an engaged scope is a bin (engaged kInvalidMediaBin is the root bin).
QVariantMap prototypeQuery(const MediaQueryDescriptor& query) {
    QVariantMap map;
    if (query.scope.has_value()) {
        map.insert(QStringLiteral("parentId"), binIdentity(*query.scope));
        map.insert(QStringLiteral("projectScope"), false);
    } else {
        map.insert(QStringLiteral("parentId"), QStringLiteral("project"));
        map.insert(QStringLiteral("projectScope"), true);
    }
    map.insert(QStringLiteral("searchText"), QString::fromStdString(query.text));
    map.insert(QStringLiteral("kindFilter"), kindFilterFromDescriptor(query));
    map.insert(QStringLiteral("mediaOnly"), !query.binsOnly);
    map.insert(QStringLiteral("unusedOnly"), query.unused.value_or(false));
    map.insert(QStringLiteral("offlineOnly"), query.offline.value_or(false));
    map.insert(QStringLiteral("recursive"), query.recursive);
    return map;
}

// Media formats the chooser offers. The suffix is a chooser convenience only;
// format validation stays with the import adapters.
[[nodiscard]] const std::vector<NativeFileChooser::Filter>& mediaChooserFilters() {
    static const std::vector<NativeFileChooser::Filter> filters{
        NativeFileChooser::Filter{QStringLiteral("Media files"),
                                  {QStringLiteral("*.exr"), QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                   QStringLiteral("*.jpeg"), QStringLiteral("*.tif"), QStringLiteral("*.tiff"),
                                   QStringLiteral("*.dpx"), QStringLiteral("*.mov"), QStringLiteral("*.mp4"),
                                   QStringLiteral("*.mkv"), QStringLiteral("*.wav")}},
        NativeFileChooser::Filter{QStringLiteral("All files"), {QStringLiteral("*")}}};
    return filters;
}

// The chooser reports local files only. A non-local URL or an empty selection
// is a failure the panel must see, never a path this model may import; `failure`
// is set, and the returned list left empty, when that happens.
QStringList chooserLocalPaths(const QList<QUrl>& urls, QString& failure) {
    QStringList paths;
    paths.reserve(urls.size());
    for (const QUrl& url : urls) {
        const QString path = url.toLocalFile();
        if (path.isEmpty()) {
            failure = QStringLiteral("The file dialog returned a non-local path.");
            return {};
        }
        paths.push_back(path);
    }
    if (paths.isEmpty()) {
        failure = QStringLiteral("The file dialog returned no file path.");
    }
    return paths;
}

QVariantList markList(const std::vector<nemo::MediaMarkRange>& ranges) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(ranges.size()));
    for (const nemo::MediaMarkRange& range : ranges) {
        QVariantMap mark;
        mark.insert(QStringLiteral("inFrame"),
                    range.inFrame ? QVariant(static_cast<qlonglong>(*range.inFrame)) : QVariant());
        mark.insert(QStringLiteral("outFrame"),
                    range.outFrame ? QVariant(static_cast<qlonglong>(*range.outFrame)) : QVariant());
        out.push_back(mark);
    }
    return out;
}

// Translates the panel's prototype query record into the typed descriptor that
// is the persisted form. A named scope that cannot be parsed is rejected; a
// scope bin that no longer exists is validated by the caller against the
// document (unavailable, never a root fallback).
std::optional<MediaQueryDescriptor> descriptorFromQuery(const QVariantMap& query, QString& error) {
    MediaQueryDescriptor descriptor;
    descriptor.text = objectString(query, QStringLiteral("searchText")).toStdString();

    const QString scopeValue = objectString(query, QStringLiteral("parentId")).trimmed();
    const bool projectScope = query.value(QStringLiteral("projectScope")).toBool() ||
                              scopeValue == QLatin1String("project") || scopeValue == QLatin1String("all") ||
                              scopeValue == QLatin1String("*") || scopeValue.isEmpty();
    if (projectScope) {
        descriptor.scope = std::nullopt;
        descriptor.recursive = true;
    } else {
        MediaBinId scope = kInvalidMediaBin;
        if (!parseBinIdentity(scopeValue, scope)) {
            error = QStringLiteral("Saved search destination must be a bin or the project");
            return std::nullopt;
        }
        descriptor.scope = scope;
        descriptor.recursive = query.value(QStringLiteral("recursive")).toBool();
    }

    const QString kindFilter = objectString(query, QStringLiteral("kindFilter"));
    if (kindFilter == QLatin1String("video")) {
        descriptor.kind = MediaKind::Video;
    } else if (kindFilter == QLatin1String("audio")) {
        descriptor.kind = MediaKind::Audio;
    } else if (kindFilter == QLatin1String("still")) {
        descriptor.kind = MediaKind::Image;
        descriptor.includeImageSequences = true;
    } else if (kindFilter == QLatin1String("unknown")) {
        descriptor.kind = MediaKind::Unknown;
    } else if (kindFilter == QLatin1String("other")) {
        descriptor.kind = MediaKind::Other;
    } else if (kindFilter == QLatin1String("bin")) {
        descriptor.binsOnly = true;
    }
    // Only engaged optionals filter; false would mean "online only"/"used only",
    // so a disabled Only flag leaves the field unset.
    if (query.value(QStringLiteral("offlineOnly")).toBool()) {
        descriptor.offline = true;
    }
    if (query.value(QStringLiteral("unusedOnly")).toBool()) {
        descriptor.unused = true;
    }
    // An all-default descriptor is a valid saved all-media query; only an
    // explicit nullopt query is a plain bin.
    return descriptor;
}

// --- Operation planning ----------------------------------------------------

Command noOpCommand(std::string label) {
    return Command{std::move(label), [](Document&) {}};
}

// Duplicate the media entry. The prototype names a top-level copy "<name> Copy".
Command copyEntryCommand(const Document& document, MediaSourceId entry, MediaBinId parent, bool topLevel) {
    auto created = std::make_shared<MediaSourceId>(kInvalidMediaSource);
    std::vector<Command> parts;
    parts.push_back(nemo::duplicateCatalogEntryCommand(entry, parent, created));
    if (topLevel) {
        const MediaCatalogEntry* source = document.mediaCatalog.entry(entry);
        if (source) {
            const std::string base = source->metadata.userName.empty() ? source->sourceKey : source->metadata.userName;
            const std::string name = document.mediaCatalog.nextAvailableName(parent, base + " Copy");
            parts.push_back(Command{"name duplicated media", [created, name](Document& candidate) {
                                        nemo::renameMediaCommand(*created, name).apply(candidate);
                                    }});
        }
    }
    return nemo::transactionCommand("duplicate media entry", std::move(parts));
}

// Children in stored creation order (the order the panel's tree renders), with
// smart query bins excluded exactly as children() does.
void storedChildIds(const Document& document, MediaBinId parent, std::vector<MediaBinId>& bins,
                    std::vector<MediaSourceId>& entries) {
    for (const MediaBin& bin : document.mediaCatalog.bins()) {
        if (bin.parent == parent && !bin.query) {
            bins.push_back(bin.id);
        }
    }
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        if (entry.parent == parent) {
            entries.push_back(entry.id);
        }
    }
}

// Duplicate a bin subtree. Children keep their names; only a top-level copy is
// renamed, matching the archived prototype `_copyTree`.
Command copyBinCommand(const Document& document, MediaBinId source, MediaBinId parent, bool topLevel) {
    const MediaBin* bin = document.mediaCatalog.bin(source);
    if (!bin) {
        return noOpCommand("duplicate unknown media bin");
    }
    auto created = std::make_shared<MediaBinId>(kInvalidMediaBin);
    const std::string name =
        topLevel ? document.mediaCatalog.nextAvailableName(parent, bin->name + " Copy") : bin->name;
    std::vector<MediaBinId> bins;
    std::vector<MediaSourceId> entries;
    storedChildIds(document, source, bins, entries);

    std::vector<Command> parts;
    parts.push_back(nemo::createBinCommand(name, parent, created));
    // The prototype clones the whole item record, so a duplicate keeps the
    // source bin's authored metadata (tags/description/color).
    parts.push_back(Command{"duplicate bin metadata", [source, created](Document& candidate) {
                                const MediaBin* original = candidate.mediaCatalog.bin(source);
                                if (original) {
                                    nemo::setMediaBinMetadataCommand(*created, original->metadata).apply(candidate);
                                }
                            }});
    parts.push_back(Command{"duplicate bin contents", [entries, bins, created](Document& candidate) {
                                for (const MediaBinId id : bins) {
                                    copyBinCommand(candidate, id, *created, false).apply(candidate);
                                }
                                for (const MediaSourceId id : entries) {
                                    nemo::duplicateCatalogEntryCommand(id, *created).apply(candidate);
                                }
                            }});
    return nemo::transactionCommand("duplicate media bin", std::move(parts));
}

// Faithful port of the archived prototype's keep-contents deletion: it is a
// true flatten, so every descendant of the removed bin is promoted to the
// removed bin's parent, not merely its direct children. Composed from the
// existing move/remove commands as one UI transaction; the core
// removeBinCommand(keepContents=true) direct-child semantics stay untouched.
Command flattenBinCommand(const Document& document, MediaBinId bin) {
    const MediaBin* value = document.mediaCatalog.bin(bin);
    if (!value) {
        return noOpCommand("remove unknown media bin");
    }
    const MediaBinId parent = value->parent;
    std::vector<Command> parts;
    for (const MediaBinId child : document.mediaCatalog.depthFirstBins(bin)) {
        parts.push_back(nemo::moveBinCommand(child, parent));
    }
    for (const MediaSourceId child : document.mediaCatalog.depthFirstEntries(bin)) {
        parts.push_back(nemo::moveMediaCommand(child, parent));
    }
    parts.push_back(nemo::removeBinCommand(bin, false));
    return nemo::transactionCommand("remove media bin keeping contents", std::move(parts));
}

// Remove a nested bin deleting its contents: the catalog has no single command
// for a non-empty bin, so descendants are removed deepest first and the bin
// itself last, inside one transaction.
void collectRemovalCommands(const Document& document, MediaBinId bin, std::vector<Command>& out) {
    for (const MediaBinId child : document.mediaCatalog.childBins(bin)) {
        collectRemovalCommands(document, child, out);
    }
    for (const MediaSourceId child : document.mediaCatalog.childEntries(bin)) {
        out.push_back(nemo::removeMediaEntryCommand(child));
    }
    out.push_back(nemo::removeBinCommand(bin, false));
}

class CatalogPlanner final {
public:
    explicit CatalogPlanner(const Document& document) : document_(document) {}

    bool plan(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QString type = operation.value(QStringLiteral("type")).toString();
        if (type.isEmpty()) {
            error = QStringLiteral("Operation type is required");
            return false;
        }
        if (type == QLatin1String("createBin")) {
            return planCreateBin(operation, commands, error);
        }
        if (type == QLatin1String("rename")) {
            return planRename(operation, commands, error);
        }
        if (type == QLatin1String("move")) {
            return planMove(operation, commands, error);
        }
        if (type == QLatin1String("duplicate")) {
            return planDuplicate(operation, commands, error);
        }
        if (type == QLatin1String("remove")) {
            return planRemove(operation, commands, error);
        }
        if (type == QLatin1String("metadata")) {
            return planMetadata(operation, commands, error);
        }
        if (type == QLatin1String("collect")) {
            return planCollect(operation, commands, error);
        }
        if (type == QLatin1String("createSmartBin")) {
            return planCreateSmartBin(operation, commands, error);
        }
        if (type == QLatin1String("setSmartBinQuery")) {
            return planSetSmartBinQuery(operation, commands, error);
        }
        if (type == QLatin1String("renameSmartBin")) {
            return planRenameSmartBin(operation, commands, error);
        }
        if (type == QLatin1String("deleteSmartBin")) {
            return planDeleteSmartBin(operation, commands, error);
        }
        error = QStringLiteral("Unknown operation ") + type;
        return false;
    }

private:
    bool resolveParent(const QVariant& value, MediaBinId& parent, QString& error) const {
        const QString id = value.toString().trimmed();
        if (id.isEmpty() || id == QLatin1String("root")) {
            parent = kInvalidMediaBin;
            return true;
        }
        MediaBinId parsed = kInvalidMediaBin;
        if (!parseBinIdentity(id, parsed) || !document_.mediaCatalog.bin(parsed)) {
            error = QStringLiteral("Destination must be an existing bin");
            return false;
        }
        parent = parsed;
        return true;
    }

    bool resolveBin(const QVariant& value, MediaBinId& bin, QString& error) const {
        MediaBinId parsed = kInvalidMediaBin;
        if (!parseBinIdentity(value.toString(), parsed) || parsed == kInvalidMediaBin) {
            error = QStringLiteral("Unknown media bin ") + value.toString();
            return false;
        }
        if (!document_.mediaCatalog.bin(parsed)) {
            error = QStringLiteral("Unknown media bin ") + value.toString();
            return false;
        }
        bin = parsed;
        return true;
    }

    bool planCreateBin(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        MediaBinId parent = kInvalidMediaBin;
        if (!resolveParent(operation.value(QStringLiteral("parentId")), parent, error)) {
            return false;
        }
        const QString name = objectString(operation, QStringLiteral("name")).trimmed();
        if (!validCatalogName(name)) {
            error = QStringLiteral("Bin name must be 1-128 printable characters");
            return false;
        }
        commands.push_back(nemo::createBinCommand(name.toStdString(), parent));
        return true;
    }

    bool planRename(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const RecordRef ref = lookupRecord(document_, objectString(operation, QStringLiteral("id")));
        if (!ref.valid) {
            error = QStringLiteral("Unknown catalog item ") + objectString(operation, QStringLiteral("id"));
            return false;
        }
        const QString name = objectString(operation, QStringLiteral("name")).trimmed();
        if (!validCatalogName(name)) {
            error = QStringLiteral("Name must be 1-128 printable characters");
            return false;
        }
        if (ref.bin) {
            commands.push_back(nemo::renameBinCommand(ref.binId, name.toStdString()));
        } else {
            commands.push_back(nemo::renameMediaCommand(ref.entryId, name.toStdString()));
        }
        return true;
    }

    bool planMove(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QVariantList ids = normalizeIds(document_, operation.value(QStringLiteral("ids")).toList());
        if (ids.isEmpty()) {
            error = QStringLiteral("No catalog items selected");
            return false;
        }
        MediaBinId parent = kInvalidMediaBin;
        if (!resolveParent(operation.value(QStringLiteral("parentId")), parent, error)) {
            return false;
        }
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid || ref.bin) {
                continue;
            }
            commands.push_back(nemo::moveMediaCommand(ref.entryId, parent));
        }
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid || !ref.bin) {
                continue;
            }
            commands.push_back(nemo::moveBinCommand(ref.binId, parent));
        }
        return true;
    }

    bool planDuplicate(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QVariantList ids = normalizeIds(document_, operation.value(QStringLiteral("ids")).toList());
        if (ids.isEmpty()) {
            error = QStringLiteral("No catalog items selected");
            return false;
        }
        MediaBinId parent = kInvalidMediaBin;
        if (!resolveParent(operation.value(QStringLiteral("parentId")), parent, error)) {
            return false;
        }
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid) {
                continue;
            }
            commands.push_back(ref.bin ? copyBinCommand(document_, ref.binId, parent, true)
                                       : copyEntryCommand(document_, ref.entryId, parent, true));
        }
        return true;
    }

    bool planRemove(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QVariantList ids = normalizeIds(document_, operation.value(QStringLiteral("ids")).toList());
        if (ids.isEmpty()) {
            error = QStringLiteral("No catalog items selected");
            return false;
        }
        const bool keepContents = operation.value(QStringLiteral("keepContents")).toBool();
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid) {
                continue;
            }
            if (!keepContents) {
                if (subtreeHasUsed(document_, ref)) {
                    error = QStringLiteral("Used media cannot be removed from the catalog");
                    return false;
                }
            } else if (!ref.bin) {
                const MediaCatalogEntry* entry = document_.mediaCatalog.entry(ref.entryId);
                if (entry && document_.mediaCatalog.sourceUsed(document_, entry->sourceKey)) {
                    error = QStringLiteral("Used media cannot be removed from the catalog");
                    return false;
                }
            }
        }
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid) {
                continue;
            }
            if (!ref.bin) {
                commands.push_back(nemo::removeMediaEntryCommand(ref.entryId));
            } else if (keepContents) {
                commands.push_back(flattenBinCommand(document_, ref.binId));
            } else {
                collectRemovalCommands(document_, ref.binId, commands);
            }
        }
        return true;
    }

    bool planMetadata(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QVariantList ids = normalizeIds(document_, operation.value(QStringLiteral("ids")).toList());
        if (ids.isEmpty()) {
            error = QStringLiteral("No catalog items selected");
            return false;
        }
        const QVariantMap changes = operation.value(QStringLiteral("changes")).toMap();
        if (changes.isEmpty()) {
            error = QStringLiteral("Metadata changes are required");
            return false;
        }
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            if (it.key() != QLatin1String("tags") && it.key() != QLatin1String("description") &&
                it.key() != QLatin1String("color")) {
                error = QStringLiteral("Unsupported metadata field ") + it.key();
                return false;
            }
        }
        std::vector<std::string> tags;
        if (changes.contains(QStringLiteral("tags"))) {
            const QVariantList values = changes.value(QStringLiteral("tags")).toList();
            if (values.size() > 64) {
                error = QStringLiteral("Tags must be non-empty printable strings");
                return false;
            }
            for (const QVariant& value : values) {
                const QString tag = value.toString().trimmed();
                if (tag.isEmpty() || tag.size() > 64) {
                    error = QStringLiteral("Tags must be non-empty printable strings");
                    return false;
                }
                for (const QChar character : tag) {
                    if (character.unicode() < 32 || character.unicode() == 127) {
                        error = QStringLiteral("Tags must be non-empty printable strings");
                        return false;
                    }
                }
                tags.push_back(tag.toStdString());
            }
        }
        if (changes.contains(QStringLiteral("description"))) {
            const QString description = changes.value(QStringLiteral("description")).toString();
            if (description.size() > 4096) {
                error = QStringLiteral("Description must be at most 4096 characters");
                return false;
            }
        }
        if (changes.contains(QStringLiteral("color"))) {
            const QString color = changes.value(QStringLiteral("color")).toString();
            if (!validHexColor(color)) {
                error = QStringLiteral("Color must be a hexadecimal color");
                return false;
            }
        }
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid) {
                continue;
            }
            if (ref.bin) {
                // The prototype allows tags/description/color on any catalog
                // item, so bins use the same authored fields through the bin
                // metadata command.
                const MediaBin* bin = document_.mediaCatalog.bin(ref.binId);
                if (!bin) {
                    continue;
                }
                nemo::MediaBinMetadata metadata = bin->metadata;
                if (changes.contains(QStringLiteral("tags"))) {
                    metadata.tags = tags;
                }
                if (changes.contains(QStringLiteral("description"))) {
                    metadata.description = changes.value(QStringLiteral("description")).toString().toStdString();
                }
                if (changes.contains(QStringLiteral("color"))) {
                    metadata.label = changes.value(QStringLiteral("color")).toString().toStdString();
                }
                commands.push_back(nemo::setMediaBinMetadataCommand(ref.binId, std::move(metadata)));
                continue;
            }
            const MediaCatalogEntry* entry = document_.mediaCatalog.entry(ref.entryId);
            if (!entry) {
                continue;
            }
            MediaMetadata metadata = entry->metadata;
            metadata.committedProbe.reset();
            if (changes.contains(QStringLiteral("tags"))) {
                metadata.tags = tags;
            }
            if (changes.contains(QStringLiteral("description"))) {
                metadata.description = changes.value(QStringLiteral("description")).toString().toStdString();
            }
            if (changes.contains(QStringLiteral("color"))) {
                metadata.label = changes.value(QStringLiteral("color")).toString().toStdString();
            }
            commands.push_back(nemo::setMediaMetadataCommand(ref.entryId, std::move(metadata)));
        }
        return true;
    }

    bool planCollect(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QVariantList ids = normalizeIds(document_, operation.value(QStringLiteral("ids")).toList());
        if (ids.isEmpty()) {
            error = QStringLiteral("No catalog items selected");
            return false;
        }
        const QString name = objectString(operation, QStringLiteral("name")).trimmed();
        if (!validCatalogName(name)) {
            error = QStringLiteral("Bin name must be 1-128 printable characters");
            return false;
        }
        MediaBinId parent = kInvalidMediaBin;
        if (!resolveParent(operation.value(QStringLiteral("parentId")), parent, error)) {
            return false;
        }
        std::vector<MediaSourceId> entries;
        std::vector<MediaBinId> bins;
        const QString parentIdentity = binIdentity(parent);
        for (const QVariant& value : ids) {
            const RecordRef ref = lookupRecord(document_, value.toString());
            if (!ref.valid) {
                continue;
            }
            if (ref.bin) {
                if (ref.binId == parent || ancestorOf(document_, binIdentity(ref.binId), parentIdentity)) {
                    error = QStringLiteral("Selection cannot contain its destination");
                    return false;
                }
                bins.push_back(ref.binId);
            } else {
                entries.push_back(ref.entryId);
            }
        }
        auto created = std::make_shared<MediaBinId>(kInvalidMediaBin);
        std::vector<Command> parts;
        parts.push_back(nemo::createBinCommand(name.toStdString(), parent, created));
        parts.push_back(Command{"collect selection", [entries, bins, created](Document& candidate) {
                                    for (const MediaSourceId id : entries) {
                                        nemo::moveMediaCommand(id, *created).apply(candidate);
                                    }
                                    for (const MediaBinId id : bins) {
                                        nemo::moveBinCommand(id, *created).apply(candidate);
                                    }
                                }});
        commands.push_back(nemo::transactionCommand("collect media into bin", std::move(parts)));
        return true;
    }

    bool planCreateSmartBin(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        const QString name = objectString(operation, QStringLiteral("name")).trimmed();
        if (!validCatalogName(name)) {
            error = QStringLiteral("Bin name must be 1-128 printable characters");
            return false;
        }
        MediaBinId parent = kInvalidMediaBin;
        if (!resolveParent(operation.value(QStringLiteral("parentId")), parent, error)) {
            return false;
        }
        auto descriptor = descriptorFromQuery(operation.value(QStringLiteral("query")).toMap(), error);
        if (!descriptor) {
            return false;
        }
        if (descriptor->scope.has_value() && *descriptor->scope != kInvalidMediaBin &&
            !document_.mediaCatalog.bin(*descriptor->scope)) {
            error = QStringLiteral("Saved search destination must be an existing bin");
            return false;
        }
        auto created = std::make_shared<MediaBinId>(kInvalidMediaBin);
        std::vector<Command> parts;
        parts.push_back(nemo::createBinCommand(name.toStdString(), parent, created));
        parts.push_back(Command{"set smart query", [created, descriptor = *descriptor](Document& candidate) {
                                    nemo::setMediaQueryCommand(*created, descriptor).apply(candidate);
                                }});
        commands.push_back(nemo::transactionCommand("create smart media bin", std::move(parts)));
        return true;
    }

    bool planSetSmartBinQuery(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        MediaBinId bin = kInvalidMediaBin;
        if (!resolveBin(operation.value(QStringLiteral("id")), bin, error)) {
            return false;
        }
        auto descriptor = descriptorFromQuery(operation.value(QStringLiteral("query")).toMap(), error);
        if (!descriptor) {
            return false;
        }
        if (descriptor->scope.has_value() && *descriptor->scope != kInvalidMediaBin &&
            !document_.mediaCatalog.bin(*descriptor->scope)) {
            error = QStringLiteral("Saved search destination must be an existing bin");
            return false;
        }
        commands.push_back(nemo::setMediaQueryCommand(bin, *descriptor));
        return true;
    }

    bool planRenameSmartBin(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        MediaBinId bin = kInvalidMediaBin;
        if (!resolveBin(operation.value(QStringLiteral("id")), bin, error)) {
            return false;
        }
        const QString name = objectString(operation, QStringLiteral("name")).trimmed();
        if (!validCatalogName(name)) {
            error = QStringLiteral("Bin name must be 1-128 printable characters");
            return false;
        }
        commands.push_back(nemo::renameBinCommand(bin, name.toStdString()));
        return true;
    }

    bool planDeleteSmartBin(const QVariantMap& operation, std::vector<Command>& commands, QString& error) const {
        MediaBinId bin = kInvalidMediaBin;
        if (!resolveBin(operation.value(QStringLiteral("id")), bin, error)) {
            return false;
        }
        commands.push_back(nemo::removeBinCommand(bin, true));
        return true;
    }

    const Document& document_;
};

QString operationLabel(const QVariantMap& operation) {
    const QString type = operation.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("createBin")) {
        return QStringLiteral("create media bin");
    }
    if (type == QLatin1String("rename")) {
        return QStringLiteral("rename media item");
    }
    if (type == QLatin1String("move")) {
        return QStringLiteral("move media items");
    }
    if (type == QLatin1String("duplicate")) {
        return QStringLiteral("duplicate media items");
    }
    if (type == QLatin1String("remove")) {
        return QStringLiteral("remove media items");
    }
    if (type == QLatin1String("metadata")) {
        return QStringLiteral("edit media metadata");
    }
    if (type == QLatin1String("collect")) {
        return QStringLiteral("collect media into bin");
    }
    return QStringLiteral("edit media catalog");
}

QImage thumbnailToImage(const nemo::CpuImage& image) {
    if (image.width() <= 0 || image.height() <= 0) {
        return {};
    }
    QImage out(image.width(), image.height(), QImage::Format_RGBA8888);
    const float* source = image.data();
    for (int y = 0; y < image.height(); ++y) {
        uchar* row = out.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const float* pixel = source + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) +
                                           static_cast<std::size_t>(x)) *
                                              nemo::kImageChannels;
            for (std::size_t channel = 0; channel < nemo::kImageChannels; ++channel) {
                const float value = std::clamp(pixel[channel], 0.0F, 1.0F);
                row[x * 4 + static_cast<int>(channel)] = static_cast<uchar>(std::lround(value * 255.0F));
            }
        }
    }
    return out;
}

// --- Workspace tree helpers (plain QVariantMap, no shell type switches) -----

bool findWorkspacePanel(const QVariantMap& node, const QString& type, const QString& group, QString& leafId,
                        QString& panelId) {
    if (node.value(QStringLiteral("kind")).toString() == QLatin1String("split")) {
        for (const QVariant& child : node.value(QStringLiteral("children")).toList()) {
            if (child.canConvert<QVariantMap>() && findWorkspacePanel(child.toMap(), type, group, leafId, panelId)) {
                return true;
            }
        }
        return false;
    }
    const QString id = node.value(QStringLiteral("id")).toString();
    for (const QVariant& panelValue : node.value(QStringLiteral("panels")).toList()) {
        const QVariantMap panel = panelValue.toMap();
        if (panel.value(QStringLiteral("type")).toString() == type &&
            panel.value(QStringLiteral("group")).toString() == group &&
            !panel.value(QStringLiteral("id")).toString().isEmpty()) {
            leafId = id;
            panelId = panel.value(QStringLiteral("id")).toString();
            return true;
        }
    }
    return false;
}

bool findWorkspaceLeafWithPanel(const QVariantMap& node, const QString& type, const QString& group, QString& leafId) {
    if (node.value(QStringLiteral("kind")).toString() == QLatin1String("split")) {
        for (const QVariant& child : node.value(QStringLiteral("children")).toList()) {
            if (child.canConvert<QVariantMap>() && findWorkspaceLeafWithPanel(child.toMap(), type, group, leafId)) {
                return true;
            }
        }
        return false;
    }
    for (const QVariant& panelValue : node.value(QStringLiteral("panels")).toList()) {
        const QVariantMap panel = panelValue.toMap();
        if (panel.value(QStringLiteral("type")).toString() == type &&
            panel.value(QStringLiteral("group")).toString() == group) {
            leafId = node.value(QStringLiteral("id")).toString();
            return true;
        }
    }
    return false;
}

bool findFirstWorkspaceLeaf(const QVariantMap& node, QString& leafId) {
    if (node.value(QStringLiteral("kind")).toString() == QLatin1String("split")) {
        for (const QVariant& child : node.value(QStringLiteral("children")).toList()) {
            if (child.canConvert<QVariantMap>() && findFirstWorkspaceLeaf(child.toMap(), leafId)) {
                return true;
            }
        }
        return false;
    }
    leafId = node.value(QStringLiteral("id")).toString();
    return !leafId.isEmpty();
}

}  // namespace

// --- Thumbnail cache / provider -------------------------------------------

MediaThumbnailCache::MediaThumbnailCache(int capacity) : capacity_(std::max(1, capacity)) {}

void MediaThumbnailCache::insert(const QString& key, QImage image) {
    QMutexLocker locker(&mutex_);
    if (!images_.contains(key)) {
        order_.append(key);
    }
    images_.insert(key, std::move(image));
    while (order_.size() > capacity_) {
        images_.remove(order_.takeFirst());
    }
}

QImage MediaThumbnailCache::find(const QString& key) const {
    QMutexLocker locker(&mutex_);
    return images_.value(key);
}

void MediaThumbnailCache::clear() {
    QMutexLocker locker(&mutex_);
    images_.clear();
    order_.clear();
}

MediaThumbnailProvider::MediaThumbnailProvider(std::shared_ptr<MediaThumbnailCache> cache)
    : QQuickImageProvider(QQuickImageProvider::Image), cache_(std::move(cache)) {}

QImage MediaThumbnailProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    Q_UNUSED(requestedSize);
    const QImage image = cache_ ? cache_->find(id) : QImage();
    if (size) {
        *size = image.size();
    }
    return image;
}

// --- Model ----------------------------------------------------------------

MediaLibraryModel::MediaLibraryModel(nemo::ProjectSession& session, nemo::media::MediaImportService& importer,
                                     PanelContextRouter* contextRouter, nemo::workspace::WorkspaceController* workspace,
                                     QObject* parent)
    : QObject(parent), session_(session), importer_(importer), contextRouter_(contextRouter), workspace_(workspace),
      thumbnails_(std::make_shared<MediaThumbnailCache>(kThumbnailCacheCapacity)), pollTimer_(new QTimer(this)) {
    pollTimer_->setInterval(kPollIntervalMs);
    connect(pollTimer_, &QTimer::timeout, this, &MediaLibraryModel::pollRuntime);
    projectGeneration_ = session_.projectGeneration();
    lastDocumentStamp_ = documentStamp();
    lastHistoryStamp_ = historyStamp();
    lastColorPolicy_ = session_.document().color;
    lastColorConfig_ = session_.colorConfigPath();
    subscription_ = session_.subscribe(this, &MediaLibraryModel::sessionChanged);
}

MediaLibraryModel::~MediaLibraryModel() {
    pollTimer_->stop();
    std::vector<std::string> keys;
    keys.reserve(inFlight_.size());
    for (const auto& [key, probe] : inFlight_) {
        static_cast<void>(probe);
        keys.push_back(key);
    }
    inFlight_.clear();
    for (const std::string& key : keys) {
        importer_.cancel(key);
    }
}

void MediaLibraryModel::setContextRouter(PanelContextRouter* contextRouter) {
    contextRouter_ = contextRouter;
}

void MediaLibraryModel::setWorkspaceController(nemo::workspace::WorkspaceController* workspace) {
    workspace_ = workspace;
}

QQuickImageProvider* MediaLibraryModel::createThumbnailProvider() {
    return new MediaThumbnailProvider(thumbnails_);
}

bool MediaLibraryModel::canUndo() const {
    return session_.canUndo();
}

bool MediaLibraryModel::canRedo() const {
    return session_.canRedo();
}

std::uint64_t MediaLibraryModel::documentStamp() const {
    const std::uint64_t catalog = session_.document().mediaCatalog.stateHash();
    return (session_.revision() * 1099511628211ULL) ^ catalog;
}

std::uint64_t MediaLibraryModel::historyStamp() const {
    return (session_.canUndo() ? 2ULL : 0ULL) | (session_.canRedo() ? 1ULL : 0ULL);
}

void MediaLibraryModel::sessionChanged(void* context) noexcept {
    auto* model = static_cast<MediaLibraryModel*>(context);
    if (!model) {
        return;
    }
    try {
        model->onSessionChanged();
    } catch (...) {
        // A notification callback never propagates; the published document is
        // authoritative and the next change re-synchronizes.
    }
}

void MediaLibraryModel::onSessionChanged() {
    const std::uint64_t generation = session_.projectGeneration();
    const std::uint64_t documentStampValue = documentStamp();
    const std::uint64_t historyStampValue = historyStamp();
    const ColorPolicy colorPolicy = session_.document().color;
    const std::string colorConfig = session_.colorConfigPath();
    const bool colorChanged = !(colorPolicy == lastColorPolicy_) || colorConfig != lastColorConfig_;
    lastColorPolicy_ = colorPolicy;
    lastColorConfig_ = colorConfig;
    if (generation != projectGeneration_) {
        // A new project was opened or recovered: integer identities are only
        // comparable within one project, so every queued/stored result is
        // dropped. Thumbnails repopulate lazily through probeState as the panel
        // requests visible items; nothing about the old project survives.
        projectGeneration_ = generation;
        inFlight_.clear();
        stored_.clear();
        latestRequest_.clear();
        queued_.clear();
        thumbnails_->clear();
        revision_ += 1;
        lastDocumentStamp_ = documentStampValue;
        lastHistoryStamp_ = historyStampValue;
        emit revisionChanged();
        emit catalogChanged();
        emit smartBinsChanged();
        emit historyChanged();
        return;
    }
    if (documentStampValue != lastDocumentStamp_) {
        lastDocumentStamp_ = documentStampValue;
        revision_ += 1;
        emit revisionChanged();
        emit catalogChanged();
        emit smartBinsChanged();
    }
    if (historyStampValue != lastHistoryStamp_) {
        lastHistoryStamp_ = historyStampValue;
        emit historyChanged();
    }
    if (colorChanged) {
        // A viewing-transform change makes every produced thumbnail stale,
        // while an unrelated metadata edit leaves them valid. Refresh the
        // thumbnails the adapter has actually produced; the visible-request
        // hook covers everything else.
        refreshThumbnailsForColorChange();
    }
}

void MediaLibraryModel::setError(QString message) {
    if (error_ == message) {
        return;
    }
    error_ = std::move(message);
    emit errorChanged();
}

void MediaLibraryModel::clearError() {
    setError(QString());
}

nemo::EditOptions MediaLibraryModel::editOptions(const nemo::ProjectSession& session) {
    return nemo::EditOptions{.expectedRevision = session.revision()};
}

bool MediaLibraryModel::finishEdit(const nemo::EditResult& result, const QString& fallback) {
    if (result.committed) {
        clearError();
        return true;
    }
    setError(result.error ? QString::fromStdString(result.error->message) : fallback);
    return false;
}

bool MediaLibraryModel::submitCommands(const std::string& label, std::vector<Command> commands) {
    if (commands.empty()) {
        clearError();
        return true;
    }
    if (commands.size() == 1) {
        return finishEdit(session_.submit(std::move(commands.front()), editOptions(session_)),
                          QStringLiteral("Media catalog edit rejected"));
    }
    return finishEdit(session_.submit(nemo::transactionCommand(label, std::move(commands)), editOptions(session_)),
                      QStringLiteral("Media catalog edit rejected"));
}

QVariantMap MediaLibraryModel::rootRecord() {
    QVariantMap record;
    record.insert(QStringLiteral("id"), QStringLiteral("root"));
    record.insert(QStringLiteral("parentId"), QString());
    record.insert(QStringLiteral("kind"), QStringLiteral("bin"));
    record.insert(QStringLiteral("name"), QStringLiteral("Media"));
    record.insert(QStringLiteral("sourceId"), QString());
    record.insert(QStringLiteral("duration"), 0);
    record.insert(QStringLiteral("color"), QString());
    record.insert(QStringLiteral("tags"), QVariantList());
    record.insert(QStringLiteral("description"), QString());
    record.insert(QStringLiteral("offline"), false);
    return record;
}

QVariantMap MediaLibraryModel::binRecord(const MediaBin& bin) const {
    QVariantMap record;
    record.insert(QStringLiteral("id"), binIdentity(bin.id));
    record.insert(QStringLiteral("parentId"), binIdentity(bin.parent));
    record.insert(QStringLiteral("kind"), QStringLiteral("bin"));
    record.insert(QStringLiteral("name"), QString::fromStdString(bin.name));
    record.insert(QStringLiteral("sourceId"), QString());
    record.insert(QStringLiteral("duration"), 0);
    // Bins carry the same user-visible metadata fields as media records; the
    // prototype allows tags/description/color on any catalog item.
    record.insert(QStringLiteral("color"), QString::fromStdString(bin.metadata.label));
    QVariantList tags;
    tags.reserve(static_cast<qsizetype>(bin.metadata.tags.size()));
    for (const std::string& tag : bin.metadata.tags) {
        tags.push_back(QString::fromStdString(tag));
    }
    record.insert(QStringLiteral("tags"), tags);
    record.insert(QStringLiteral("description"), QString::fromStdString(bin.metadata.description));
    record.insert(QStringLiteral("offline"), false);
    record.insert(QStringLiteral("smart"), bin.query.has_value());
    return record;
}

const MediaLibraryModel::StoredProbe* MediaLibraryModel::validStoredProbe(const std::string& sourceKey) const {
    const auto stored = stored_.find(sourceKey);
    if (stored == stored_.end()) {
        return nullptr;
    }
    const Document& document = session_.document();
    const auto source = document.sources.find(sourceKey);
    if (source == document.sources.end() || !(stored->second.expected == source->second)) {
        return nullptr;
    }
    if (!(stored->second.colorPolicy == document.color) || stored->second.colorConfig != session_.colorConfigPath()) {
        return nullptr;
    }
    return &stored->second;
}

MediaKind MediaLibraryModel::displayedMediaKind(const MediaCatalogEntry& entry) const {
    const StoredProbe* probe = validStoredProbe(entry.sourceKey);
    if (probe && probe->result.kind != MediaKind::Unknown) {
        return probe->result.kind;
    }
    return entry.metadata.kind;
}

bool MediaLibraryModel::effectiveOffline(const MediaCatalogEntry& entry) const {
    // Mirrors the core query overlay exactly: a live runtime fact for the
    // unchanged source reference replaces the authored availability, and with
    // no live fact the authored flag applies. The authored flag itself is
    // never rewritten (reported separately as authoredOffline).
    const StoredProbe* probe = validStoredProbe(entry.sourceKey);
    return probe ? probe->result.offline : entry.metadata.offline;
}

QVariantMap MediaLibraryModel::entryRecord(const MediaCatalogEntry& entry) const {
    const MediaMetadata& metadata = entry.metadata;
    // Read-only display overlay: a live runtime result shows its validated
    // kind/probe/runtime/duration without requiring an explicit apply, while
    // every authored field (name/tags/marks/label/offline) is preserved and
    // the authored values stay visible separately.
    const StoredProbe* live = validStoredProbe(entry.sourceKey);
    const MediaKind displayedKind = displayedMediaKind(entry);
    const MediaProbeMetadata* displayedProbe = nullptr;
    if (live) {
        displayedProbe = &live->result.probe;
    } else if (metadata.committedProbe) {
        displayedProbe = &*metadata.committedProbe;
    }

    QVariantMap record;
    record.insert(QStringLiteral("id"), entryIdentity(entry.id));
    record.insert(QStringLiteral("parentId"), binIdentity(entry.parent));
    record.insert(QStringLiteral("kind"), prototypeKind(displayedKind));
    record.insert(QStringLiteral("authoredKind"), prototypeKind(metadata.kind));
    record.insert(QStringLiteral("mediaKind"), mediaKindName(displayedKind));
    record.insert(QStringLiteral("name"), displayName(entry));
    record.insert(QStringLiteral("sourceId"), QString::fromStdString(entry.sourceKey));
    const auto source = session_.document().sources.find(entry.sourceKey);
    // Authored sequence start frame; read-only for the relink dialog, which
    // never rewrites it.
    record.insert(QStringLiteral("frameOffset"),
                  static_cast<qint64>(source == session_.document().sources.end() ? 0 : source->second.frameOffset));
    record.insert(QStringLiteral("duration"),
                  static_cast<qlonglong>(displayedProbe ? std::max<std::int64_t>(0, displayedProbe->duration) : 0));
    record.insert(QStringLiteral("color"), QString::fromStdString(metadata.label));
    QVariantList tags;
    tags.reserve(static_cast<qsizetype>(metadata.tags.size()));
    for (const std::string& tag : metadata.tags) {
        tags.push_back(QString::fromStdString(tag));
    }
    record.insert(QStringLiteral("tags"), tags);
    record.insert(QStringLiteral("description"), QString::fromStdString(metadata.description));
    record.insert(QStringLiteral("offline"), effectiveOffline(entry));
    // The authored flag is preserved and reported separately; runtime
    // availability never rewrites user metadata.
    record.insert(QStringLiteral("authoredOffline"), metadata.offline);
    // Per-catalog-entry marks. Duplicate occurrences of one source key keep
    // distinct mark lists, so QML must pass the mark of the occurrence it is
    // inserting rather than looking one up by source key.
    record.insert(QStringLiteral("marks"), markList(entry.marks));
    record.insert(QStringLiteral("probeStatus"),
                  displayedProbe ? probeStatusName(displayedProbe->status) : QStringLiteral("unknown"));
    record.insert(QStringLiteral("probe"), displayedProbe ? QVariant(probeRecord(*displayedProbe)) : QVariant());
    // The persisted proposal separately, so the panel can distinguish what is
    // displayed from what an explicit apply would commit.
    record.insert(QStringLiteral("committedProbe"),
                  metadata.committedProbe ? QVariant(probeRecord(*metadata.committedProbe)) : QVariant());
    record.insert(QStringLiteral("runtime"), live ? runtimeFromResult(live->result) : runtimeRecord(metadata));
    record.insert(QStringLiteral("thumbnailUrl"), thumbnailUrl(entryIdentity(entry.id)));
    return record;
}

QVariantList MediaLibraryModel::itemsSnapshot(const Document& document) const {
    QVariantList out;
    out.push_back(rootRecord());
    for (const MediaBin& bin : document.mediaCatalog.bins()) {
        if (!bin.query) {
            out.push_back(binRecord(bin));
        }
    }
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        out.push_back(entryRecord(entry));
    }
    return out;
}

QVariantList MediaLibraryModel::smartBins() const {
    QVariantList out;
    const Document& document = session_.document();
    for (const MediaBin& bin : document.mediaCatalog.bins()) {
        if (!bin.query) {
            continue;
        }
        QVariantMap record;
        record.insert(QStringLiteral("id"), binIdentity(bin.id));
        record.insert(QStringLiteral("name"), QString::fromStdString(bin.name));
        record.insert(QStringLiteral("builtIn"), false);
        const QVariantMap query = prototypeQuery(*bin.query);
        record.insert(QStringLiteral("parentId"), query.value(QStringLiteral("parentId"), QStringLiteral("project")));
        record.insert(QStringLiteral("query"), query);
        out.push_back(record);
    }
    return out;
}

QVariant MediaLibraryModel::item(const QString& id) const {
    const Document& document = session_.document();
    const RecordRef ref = lookupRecord(document, id);
    if (!ref.valid) {
        return {};
    }
    if (ref.bin) {
        if (ref.binId == kInvalidMediaBin) {
            return rootRecord();
        }
        const MediaBin* bin = document.mediaCatalog.bin(ref.binId);
        return bin ? QVariant(binRecord(*bin)) : QVariant();
    }
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(ref.entryId);
    return entry ? QVariant(entryRecord(*entry)) : QVariant();
}

QVariantList MediaLibraryModel::children(const QString& parentId) const {
    MediaBinId parent = kInvalidMediaBin;
    if (!parseBinIdentity(parentId, parent)) {
        return {};
    }
    const Document& document = session_.document();
    if (parent != kInvalidMediaBin && !document.mediaCatalog.bin(parent)) {
        return {};
    }
    QVariantList out;
    // Stored creation order, matching the prototype's single items array. The
    // core childBins/childEntries queries stay name-sorted and are used by
    // query()/traversal elsewhere; the panel sorts its own visible list.
    for (const MediaBin& bin : document.mediaCatalog.bins()) {
        if (bin.parent == parent && !bin.query) {
            out.push_back(binRecord(bin));
        }
    }
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        if (entry.parent == parent) {
            out.push_back(entryRecord(entry));
        }
    }
    return out;
}

QVariantList MediaLibraryModel::descendants(const QString& id) const {
    const Document& document = session_.document();
    const RecordRef ref = lookupRecord(document, id);
    if (!ref.valid || !ref.bin) {
        return {};
    }
    // One pass over the stored vectors (creation order), then a breadth-first
    // walk in the prototype's order: a level's bins then its media, recursing.
    std::map<MediaBinId, std::vector<MediaBinId>> childBins;
    std::map<MediaBinId, std::vector<MediaSourceId>> childEntries;
    for (const MediaBin& bin : document.mediaCatalog.bins()) {
        if (!bin.query) {
            childBins[bin.parent].push_back(bin.id);
        }
    }
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        childEntries[entry.parent].push_back(entry.id);
    }

    QVariantList out;
    std::vector<MediaBinId> queue{ref.binId};
    std::set<MediaBinId> seen;
    if (ref.binId != kInvalidMediaBin) {
        seen.insert(ref.binId);
    }
    for (std::size_t index = 0; index < queue.size(); ++index) {
        const MediaBinId current = queue[index];
        const auto bins = childBins.find(current);
        if (bins != childBins.end()) {
            for (const MediaBinId child : bins->second) {
                if (!seen.insert(child).second) {
                    continue;
                }
                const MediaBin* bin = document.mediaCatalog.bin(child);
                if (bin) {
                    out.push_back(binRecord(*bin));
                    queue.push_back(child);
                }
            }
        }
        const auto entries = childEntries.find(current);
        if (entries != childEntries.end()) {
            for (const MediaSourceId child : entries->second) {
                const MediaCatalogEntry* entry = document.mediaCatalog.entry(child);
                if (entry) {
                    out.push_back(entryRecord(*entry));
                }
            }
        }
    }
    return out;
}

QVariantList MediaLibraryModel::path(const QString& id) const {
    const Document& document = session_.document();
    if (!lookupRecord(document, id).valid) {
        return {};
    }
    std::vector<QVariantMap> chain;
    QString current = id.trimmed().isEmpty() ? QStringLiteral("root") : id.trimmed();
    for (int guard = 0; guard < 1024; ++guard) {
        const RecordRef ref = lookupRecord(document, current);
        if (!ref.valid) {
            break;
        }
        if (ref.bin) {
            if (ref.binId == kInvalidMediaBin) {
                chain.push_back(rootRecord());
                break;
            }
            const MediaBin* bin = document.mediaCatalog.bin(ref.binId);
            if (!bin) {
                break;
            }
            chain.push_back(binRecord(*bin));
        } else {
            const MediaCatalogEntry* entry = document.mediaCatalog.entry(ref.entryId);
            if (!entry) {
                break;
            }
            chain.push_back(entryRecord(*entry));
        }
        current = binIdentity(ref.parent);
    }
    QVariantList out;
    out.reserve(static_cast<qsizetype>(chain.size()));
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        out.push_back(*it);
    }
    return out;
}

QVariantList MediaLibraryModel::query(const QString& parentId, const QString& text, bool recursive,
                                      const QVariantMap& filter) const {
    const Document& document = session_.document();
    const QString scopeValue = parentId.trimmed();
    const QString filterScope = filter.value(QStringLiteral("scope")).toString().trimmed();
    const bool projectScope = scopeValue == QLatin1String("project") || scopeValue == QLatin1String("all") ||
                              scopeValue == QLatin1String("*") || filterScope == QLatin1String("project") ||
                              filterScope == QLatin1String("all") || filterScope == QLatin1String("*");
    const bool recurse = recursive || filter.value(QStringLiteral("recursive")).toBool() || projectScope;

    // Scope resolution. A named bin that no longer exists is unavailable: this
    // never falls back to the root.
    std::optional<MediaBinId> scope;
    MediaBinId scopeBin = kInvalidMediaBin;
    if (!projectScope) {
        if (!parseBinIdentity(scopeValue, scopeBin)) {
            return {};
        }
        if (scopeBin != kInvalidMediaBin && !document.mediaCatalog.bin(scopeBin)) {
            return {};
        }
        scope = scopeBin;
    }

    const bool mediaOnly = filter.value(QStringLiteral("mediaOnly")).toBool();
    bool kindFilterSet = false;
    QStringList kindFilter;
    if (filter.contains(QStringLiteral("kind"))) {
        const QVariant value = filter.value(QStringLiteral("kind"));
        if (value.typeId() == QMetaType::QVariantList || value.typeId() == QMetaType::QStringList) {
            for (const QVariant& kind : value.toList()) {
                kindFilter.append(kind.toString());
            }
        } else {
            kindFilter.append(value.toString());
        }
        kindFilterSet = !kindFilter.isEmpty();
    }
    bool offlineSet = false;
    bool offlineValue = false;
    if (filter.contains(QStringLiteral("offline"))) {
        offlineSet = true;
        offlineValue = filter.value(QStringLiteral("offline")).toBool();
    }
    bool unusedSet = false;
    bool unusedValue = false;
    if (filter.contains(QStringLiteral("unused"))) {
        unusedSet = true;
        unusedValue = filter.value(QStringLiteral("unused")).toBool();
    }
    const QString singleKind = kindFilterSet && kindFilter.size() == 1 ? kindFilter.constFirst() : QString();
    // The UI can ask for bins only; any other kind filter is a media-kind
    // filter and therefore excludes presentation bins.
    const bool binsOnlyFilter = singleKind == QLatin1String("bin");

    const QString needle = text.toLower();
    QVariantList out;
    if (!mediaOnly) {
        // Presentation bins (the panel's tree rows). The core query owner
        // returns media members only, so bin text/kind/availability matching
        // stays adapter-owned presentation.
        std::vector<MediaBinId> bins;
        try {
            if (projectScope) {
                bins = document.mediaCatalog.depthFirstBins(kInvalidMediaBin);
            } else if (recurse) {
                bins = document.mediaCatalog.depthFirstBins(scopeBin);
            } else {
                bins = document.mediaCatalog.childBins(scopeBin);
            }
        } catch (const GraphException&) {
            return {};
        }
        for (const MediaBinId id : bins) {
            const MediaBin* bin = document.mediaCatalog.bin(id);
            if (!bin || bin->query) {
                continue;
            }
            if (kindFilterSet && !binsOnlyFilter) {
                continue;  // prototype: a media-kind filter excludes bins
            }
            if (offlineSet && offlineValue) {
                continue;  // prototype: offline=true excludes bins
            }
            if (unusedSet) {
                // Faithful port of the archived MediaCatalog.qml bin semantics:
                // `unused` is true when a descendant entry's source is used.
                if (binHasUsedDescendant(document, id) != unusedValue) {
                    continue;
                }
            }
            if (!binMatchesText(*bin, needle)) {
                continue;
            }
            out.push_back(binRecord(*bin));
        }
    }

    // Media membership comes from the core query owner (text/kind/unused/scope)
    // so the catalog's search semantics are not reimplemented here.
    MediaQueryDescriptor descriptor;
    descriptor.text = text.toStdString();
    descriptor.scope = scope;
    descriptor.recursive = recurse;
    if (!binsOnlyFilter) {
        if (singleKind == QLatin1String("video")) {
            descriptor.kind = MediaKind::Video;
        } else if (singleKind == QLatin1String("audio")) {
            descriptor.kind = MediaKind::Audio;
        } else if (singleKind == QLatin1String("still")) {
            descriptor.kind = MediaKind::Image;
            descriptor.includeImageSequences = true;
        } else if (singleKind == QLatin1String("unknown")) {
            descriptor.kind = MediaKind::Unknown;
        } else if (singleKind == QLatin1String("other")) {
            descriptor.kind = MediaKind::Other;
        }
    }
    if (unusedSet) {
        descriptor.unused = unusedValue;
    }
    descriptor.binsOnly = binsOnlyFilter;
    if (offlineSet) {
        descriptor.offline = offlineValue;
    }
    // The validated runtime facts for entries whose live result still matches
    // the document: the core query owner applies them to kind/offline so the
    // adapter does not duplicate the predicate.
    std::vector<nemo::MediaQuerySourceState> runtime;
    runtime.reserve(stored_.size());
    for (const auto& [key, stored] : stored_) {
        static_cast<void>(stored);
        const StoredProbe* probe = validStoredProbe(key);
        if (probe) {
            runtime.push_back(nemo::MediaQuerySourceState{key, probe->result.kind, probe->result.offline});
        }
    }
    std::vector<MediaSourceId> members;
    try {
        members = document.mediaCatalog.search(document, descriptor, std::nullopt, runtime);
    } catch (const GraphException&) {
        return {};
    }
    for (const MediaSourceId id : members) {
        const MediaCatalogEntry* entry = document.mediaCatalog.entry(id);
        if (entry) {
            out.push_back(entryRecord(*entry));
        }
    }
    return out;
}

QVariant MediaLibraryModel::sourceItem(const QString& sourceId) const {
    const QString key = sourceId.trimmed();
    if (key.isEmpty()) {
        return {};
    }
    const Document& document = session_.document();
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        if (entry.sourceKey == key.toStdString()) {
            return entryRecord(entry);
        }
    }
    return {};
}

QVariantList MediaLibraryModel::itemsForSource(const QString& sourceId) const {
    const QString key = sourceId.trimmed();
    if (key.isEmpty()) {
        return {};
    }
    const Document& document = session_.document();
    QVariantList out;
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        if (entry.sourceKey == key.toStdString()) {
            out.push_back(entryRecord(entry));
        }
    }
    return out;
}

bool MediaLibraryModel::isUsed(const QString& sourceId) const {
    const QString key = sourceId.trimmed();
    return !key.isEmpty() && session_.document().mediaCatalog.sourceUsed(session_.document(), key.toStdString());
}

QVariantList MediaLibraryModel::normalizeSelection(const QVariantList& ids) const {
    return normalizeIds(session_.document(), ids);
}

QVariantMap MediaLibraryModel::preview(const QVariantMap& operation) const {
    Document candidate = session_.snapshot();
    std::vector<Command> commands;
    QString error;
    QVariantMap result;
    const CatalogPlanner planner(candidate);
    if (!planner.plan(operation, commands, error)) {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("error"), error);
        result.insert(QStringLiteral("items"), itemsSnapshot(session_.document()));
        return result;
    }
    try {
        for (const Command& command : commands) {
            command.apply(candidate);
        }
    } catch (const GraphException& exception) {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("error"), QString::fromStdString(exception.what()));
        result.insert(QStringLiteral("items"), itemsSnapshot(session_.document()));
        return result;
    } catch (const std::exception& exception) {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("error"), QString::fromUtf8(exception.what()));
        result.insert(QStringLiteral("items"), itemsSnapshot(session_.document()));
        return result;
    }
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), QString());
    result.insert(QStringLiteral("items"), itemsSnapshot(candidate));
    return result;
}

bool MediaLibraryModel::apply(const QVariantMap& operation) {
    std::vector<Command> commands;
    QString error;
    const CatalogPlanner planner(session_.document());
    if (!planner.plan(operation, commands, error)) {
        setError(error);
        return false;
    }
    return submitCommands(operationLabel(operation).toStdString(), std::move(commands));
}

bool MediaLibraryModel::undo() {
    if (!session_.canUndo()) {
        setError(QStringLiteral("Nothing to undo"));
        return false;
    }
    return finishEdit(session_.undo(editOptions(session_)), QStringLiteral("Nothing to undo"));
}

bool MediaLibraryModel::redo() {
    if (!session_.canRedo()) {
        setError(QStringLiteral("Nothing to redo"));
        return false;
    }
    return finishEdit(session_.redo(editOptions(session_)), QStringLiteral("Nothing to redo"));
}

bool MediaLibraryModel::newBinFromSelection(const QVariantList& ids, const QString& name, const QString& parentId) {
    QVariantMap operation;
    operation.insert(QStringLiteral("type"), QStringLiteral("collect"));
    operation.insert(QStringLiteral("ids"), ids);
    operation.insert(QStringLiteral("name"), name);
    operation.insert(QStringLiteral("parentId"), parentId.trimmed().isEmpty() ? QStringLiteral("root") : parentId);
    return apply(operation);
}

bool MediaLibraryModel::importPaths(const QStringList& paths, const QString& parentId, qint64 frameOffset) {
    MediaBinId parent = kInvalidMediaBin;
    if (!parseBinIdentity(parentId, parent)) {
        setError(QStringLiteral("Import destination must be an existing bin"));
        return false;
    }
    const Document& document = session_.document();
    if (parent != kInvalidMediaBin && !document.mediaCatalog.bin(parent)) {
        setError(QStringLiteral("Import destination bin does not exist"));
        return false;
    }

    std::set<std::string> usedKeys;
    for (const auto& [key, reference] : document.sources) {
        static_cast<void>(reference);
        usedKeys.insert(key);
    }
    QStringList usedNames;
    for (const MediaSourceId id : document.mediaCatalog.childEntries(parent)) {
        const MediaCatalogEntry* entry = document.mediaCatalog.entry(id);
        if (entry) {
            usedNames.append(displayName(*entry));
        }
    }
    for (const MediaBinId id : document.mediaCatalog.childBins(parent)) {
        const MediaBin* bin = document.mediaCatalog.bin(id);
        if (bin) {
            usedNames.append(QString::fromStdString(bin->name));
        }
    }

    std::vector<Command> commands;
    std::vector<std::shared_ptr<MediaSourceId>> created;
    for (const QString& raw : paths) {
        const QString path = raw.trimmed();
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        QString stem = info.completeBaseName().trimmed();
        if (stem.isEmpty()) {
            stem = info.fileName().trimmed();
        }
        if (stem.isEmpty()) {
            stem = QStringLiteral("media");
        }
        QString sanitized;
        for (const QChar character : stem) {
            if (character.isLetterOrNumber() || character == QLatin1Char('.') || character == QLatin1Char('_') ||
                character == QLatin1Char('-')) {
                sanitized.append(character);
            } else {
                sanitized.append(QLatin1Char('_'));
            }
        }
        if (sanitized.isEmpty()) {
            sanitized = QStringLiteral("media");
        }
        std::string key = sanitized.toStdString();
        for (int suffix = 2; usedKeys.contains(key); ++suffix) {
            key = sanitized.toStdString() + "-" + std::to_string(suffix);
        }
        usedKeys.insert(key);

        QString name = stem;
        for (int suffix = 2; usedNames.contains(name); ++suffix) {
            name = stem + QLatin1Char(' ') + QString::number(suffix);
        }
        usedNames.append(name);

        SourceReference reference;
        reference.path = path.toStdString();
        // Authored sequence start frame supplied by the import dialog; never
        // guessed from the file.
        reference.frameOffset = static_cast<std::int64_t>(frameOffset);
        reference.revision = 1;
        MediaMetadata metadata;
        metadata.userName = name.toStdString();
        auto createdId = std::make_shared<MediaSourceId>(kInvalidMediaSource);
        commands.push_back(nemo::setSourceCommand(key, reference));
        commands.push_back(nemo::importMediaReferenceCommand(key, parent, metadata, createdId));
        created.push_back(std::move(createdId));
    }
    if (commands.empty()) {
        setError(QStringLiteral("No media paths to import"));
        return false;
    }
    if (!submitCommands("import media sources", std::move(commands))) {
        return false;
    }
    for (const auto& createdId : created) {
        if (*createdId != kInvalidMediaSource) {
            enqueueRuntime(*createdId);
        }
    }
    return true;
}

bool MediaLibraryModel::enqueueRuntime(MediaSourceId entry) {
    const Document& document = session_.document();
    const MediaCatalogEntry* catalogEntry = document.mediaCatalog.entry(entry);
    if (!catalogEntry) {
        setError(QStringLiteral("Media item no longer exists"));
        return false;
    }
    const auto source = document.sources.find(catalogEntry->sourceKey);
    if (source == document.sources.end()) {
        setError(QStringLiteral("Media source path is unavailable: ") +
                 QString::fromStdString(catalogEntry->sourceKey));
        return false;
    }
    nemo::media::MediaImportRequest request;
    request.requestId = nextRequestId_++;
    request.sourceKey = catalogEntry->sourceKey;
    request.reference = source->second;
    request.colorPolicy = document.color;
    request.colorConfig = session_.colorConfigPath();
    request.thumbnailWidth = 160;
    request.thumbnailHeight = 90;
    request.frame = 0;
    InFlightProbe inflight;
    inflight.requestId = request.requestId;
    inflight.entry = entry;
    inflight.expected = source->second;
    inflight.colorPolicy = request.colorPolicy;
    inflight.colorConfig = request.colorConfig;
    inFlight_[catalogEntry->sourceKey] = inflight;
    if (!trySubmitRuntime(request)) {
        setError(QString());
        // The queue is bounded like every other adapter cache: the oldest
        // pending request is dropped with an explicit rejection (its in-flight
        // identity goes with it so the poll timer can still drain) when a huge
        // import exceeds the bound. The dropped item is not left pending; a
        // visible probeState/reprobe retries it once the queue drains.
        while (queued_.size() >= kQueuedRequestLimit) {
            const nemo::media::MediaImportRequest dropped = queued_.front();
            queued_.erase(queued_.begin());
            const auto stale = inFlight_.find(dropped.sourceKey);
            MediaSourceId droppedEntry = kInvalidMediaSource;
            if (stale != inFlight_.end() && stale->second.requestId == dropped.requestId) {
                droppedEntry = stale->second.entry;
                inFlight_.erase(stale);
            }
            const QString droppedItem = droppedEntry != kInvalidMediaSource ? entryIdentity(droppedEntry)
                                                                            : QString::fromStdString(dropped.sourceKey);
            const QString reason = QStringLiteral(
                "Media probe queue is full; this item was not queued. Show it again or reprobe to retry.");
            setError(reason);
            emit probeRejected(droppedItem, reason);
        }
        queued_.erase(std::remove_if(queued_.begin(), queued_.end(),
                                     [&](const nemo::media::MediaImportRequest& queued) {
                                         return queued.sourceKey == catalogEntry->sourceKey;
                                     }),
                      queued_.end());
        queued_.push_back(std::move(request));
    }
    startPollingIfNeeded();
    return true;
}

bool MediaLibraryModel::runtimePending(const std::string& sourceKey) const {
    if (inFlight_.find(sourceKey) != inFlight_.end()) {
        return true;
    }
    for (const nemo::media::MediaImportRequest& queued : queued_) {
        if (queued.sourceKey == sourceKey) {
            return true;
        }
    }
    return false;
}

bool MediaLibraryModel::ensureRuntime(MediaSourceId entry) {
    const Document& document = session_.document();
    const MediaCatalogEntry* catalogEntry = document.mediaCatalog.entry(entry);
    if (!catalogEntry) {
        return false;
    }
    const std::string& key = catalogEntry->sourceKey;
    const auto source = document.sources.find(key);
    if (source == document.sources.end()) {
        return false;
    }
    const ColorPolicy& policy = document.color;
    const std::string config = session_.colorConfigPath();

    const auto inflight = inFlight_.find(key);
    if (inflight != inFlight_.end() && inflight->second.expected == source->second &&
        inflight->second.colorPolicy == policy && inflight->second.colorConfig == config) {
        return true;
    }
    for (const nemo::media::MediaImportRequest& queued : queued_) {
        if (queued.sourceKey == key && queued.reference == source->second && queued.colorPolicy == policy &&
            queued.colorConfig == config) {
            return true;
        }
    }
    const auto stored = stored_.find(key);
    if (stored != stored_.end() && stored->second.expected == source->second && stored->second.colorPolicy == policy &&
        stored->second.colorConfig == config) {
        return true;  // a result for this identity already exists, even if it failed
    }
    const auto latest = latestRequest_.find(key);
    if (latest != latestRequest_.end() && latest->second.colorPolicy == policy &&
        latest->second.colorConfig == config &&
        !thumbnails_->find(QString::number(latest->second.requestId)).isNull()) {
        return true;
    }
    return enqueueRuntime(entry);
}

void MediaLibraryModel::refreshThumbnailsForColorChange() {
    if (latestRequest_.empty()) {
        return;
    }
    std::set<std::string> keys;
    for (const auto& [key, index] : latestRequest_) {
        static_cast<void>(index);
        keys.insert(key);
    }
    const Document& document = session_.document();
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        if (keys.contains(entry.sourceKey)) {
            enqueueRuntime(entry.id);
        }
    }
}

bool MediaLibraryModel::trySubmitRuntime(nemo::media::MediaImportRequest request) {
    return importer_.submit(std::move(request));
}

void MediaLibraryModel::flushQueuedRuntime() {
    while (!queued_.empty()) {
        nemo::media::MediaImportRequest request = queued_.front();
        if (!importer_.submit(std::move(request))) {
            break;
        }
        queued_.erase(queued_.begin());
    }
}

void MediaLibraryModel::startPollingIfNeeded() {
    if (!pollTimer_->isActive()) {
        pollTimer_->start();
    }
}

bool MediaLibraryModel::reprobe(const QString& id) {
    MediaSourceId entry = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entry) || !session_.document().mediaCatalog.entry(entry)) {
        setError(QStringLiteral("Unknown media item ") + id);
        return false;
    }
    return enqueueRuntime(entry);
}

bool MediaLibraryModel::applyProbe(const QString& id) {
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId)) {
        setError(QStringLiteral("Unknown media item ") + id);
        return false;
    }
    const Document& document = session_.document();
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(entryId);
    if (!entry) {
        setError(QStringLiteral("Unknown media item ") + id);
        return false;
    }
    const auto source = document.sources.find(entry->sourceKey);
    if (source == document.sources.end()) {
        setError(QStringLiteral("Media source path is unavailable: ") + QString::fromStdString(entry->sourceKey));
        return false;
    }
    const auto stored = stored_.find(entry->sourceKey);
    if (stored == stored_.end()) {
        setError(QStringLiteral("No probe result is available for ") + id +
                 QStringLiteral("; reprobe the media item first"));
        return false;
    }
    if (!(stored->second.expected == source->second)) {
        const QString reason = QStringLiteral("Media source changed; the probe result is stale");
        setError(reason);
        emit probeRejected(id, reason);
        return false;
    }
    if (!(stored->second.colorPolicy == document.color) || stored->second.colorConfig != session_.colorConfigPath()) {
        const QString reason = QStringLiteral("Viewing transform changed; the probe result is stale");
        setError(reason);
        emit probeRejected(id, reason);
        return false;
    }
    if (!stored->second.result.error.empty()) {
        const QString reason = QString::fromStdString(stored->second.result.error);
        setError(reason);
        emit probeRejected(id, reason);
        return false;
    }
    if (stored->second.result.probe.status != MediaProbeStatus::Ready) {
        setError(QStringLiteral("The probe did not produce ready metadata for ") + id);
        return false;
    }

    MediaMetadata metadata = entry->metadata;
    metadata.committedProbe.reset();
    if (stored->second.result.kind != MediaKind::Unknown) {
        metadata.kind = stored->second.result.kind;
    }
    MediaProbeMetadata probe = stored->second.result.probe;
    if (!probe.extension.is_object()) {
        probe.extension = nlohmann::json::object();
    }
    probe.extension["runtime"] = runtimeExtension(stored->second.result);

    std::vector<Command> commands;
    commands.push_back(nemo::commitMediaProbeCommand(entryId, stored->second.expected, probe));
    if (stored->second.result.kind != MediaKind::Unknown) {
        commands.push_back(nemo::setMediaMetadataCommand(entryId, std::move(metadata)));
    }
    return submitCommands("apply media probe", std::move(commands));
}

bool MediaLibraryModel::relink(const QString& id, const QString& path) {
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId)) {
        setError(QStringLiteral("Unknown media item ") + id);
        return false;
    }
    const QString target = path.trimmed();
    if (target.isEmpty()) {
        setError(QStringLiteral("Relink requires a file path"));
        return false;
    }
    const Document& document = session_.document();
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(entryId);
    if (!entry) {
        setError(QStringLiteral("Unknown media item ") + id);
        return false;
    }
    const auto source = document.sources.find(entry->sourceKey);
    if (source == document.sources.end()) {
        setError(QStringLiteral("Media source path is unavailable: ") + QString::fromStdString(entry->sourceKey));
        return false;
    }
    const std::string key = entry->sourceKey;
    std::vector<Command> commands;
    commands.push_back(nemo::relinkMediaSourceCommand(entryId, source->second, target.toStdString()));
    if (!submitCommands("relink media source", std::move(commands))) {
        return false;
    }
    inFlight_.erase(key);
    stored_.erase(key);
    latestRequest_.erase(key);
    queued_.erase(
        std::remove_if(queued_.begin(), queued_.end(),
                       [&](const nemo::media::MediaImportRequest& queued) { return queued.sourceKey == key; }),
        queued_.end());
    enqueueRuntime(entryId);
    return true;
}

void MediaLibraryModel::pollRuntime() {
    for (int processed = 0; processed < 8; ++processed) {
        auto result = importer_.takeResult();
        if (!result) {
            break;
        }
        handleRuntimeResult(std::move(*result));
    }
    if (inFlight_.empty() && queued_.empty()) {
        pollTimer_->stop();
    } else {
        flushQueuedRuntime();
    }
}

void MediaLibraryModel::handleRuntimeResult(nemo::media::MediaImportResult result) {
    const std::string key = result.request.sourceKey;
    const std::uint64_t requestId = result.request.requestId;
    const auto inflight = inFlight_.find(key);
    if (inflight == inFlight_.end() || inflight->second.requestId != requestId) {
        return;  // superseded by a newer request for the same source key
    }
    // Read the published document and capture only values before any signal:
    // a slot may mutate the catalog or replace the project during any emit
    // below, so no catalog pointer, iterator or map element reference is
    // retained past this point.
    const Document& document = session_.document();
    const MediaSourceId entryId = inflight->second.entry;
    const SourceReference expected = inflight->second.expected;
    const QString itemId = entryIdentity(entryId);
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(entryId);
    const MediaSourceId storedEntry = entry ? entry->id : kInvalidMediaSource;
    const bool entryMatchesKey = entry != nullptr && entry->sourceKey == key;
    inFlight_.erase(inflight);

    const auto source = document.sources.find(key);
    const bool referenceStale = !entryMatchesKey || source == document.sources.end() || !(source->second == expected);
    // The request identity carries the viewing transforms too: a result
    // produced under a replaced color policy/config is a stale preview.
    const bool colorStale =
        !(result.request.colorPolicy == document.color) || result.request.colorConfig != session_.colorConfigPath();
    if (referenceStale) {
        emit probeRejected(itemId,
                           QStringLiteral("Media source changed before the probe completed; the result was discarded"));
        return;
    }
    if (colorStale) {
        emit probeRejected(itemId,
                           QStringLiteral("Viewing transform changed before the probe completed; the result was "
                                          "discarded"));
        return;
    }

    // Commit every adapter-owned cache/index/result mutation BEFORE signalling,
    // so a reentrant slot can never observe or corrupt a half-published result.
    bool hasThumbnail = false;
    if (result.thumbnail && result.thumbnail->width() > 0 && result.thumbnail->height() > 0) {
        thumbnails_->insert(QString::number(requestId), thumbnailToImage(*result.thumbnail));
        ThumbnailIndexEntry index;
        index.requestId = requestId;
        index.colorPolicy = result.request.colorPolicy;
        index.colorConfig = result.request.colorConfig;
        latestRequest_[key] = index;
        pruneThumbnailIndex();
        hasThumbnail = true;
    }
    // The display-referred pixels now live only in the bounded thumbnail
    // cache; a stored result must never retain a CpuImage.
    result.thumbnail.reset();
    StoredProbe stored;
    stored.result = std::move(result);
    stored.entry = storedEntry;
    stored.expected = expected;
    stored.colorPolicy = stored.result.request.colorPolicy;
    stored.colorConfig = stored.result.request.colorConfig;
    const std::string error = stored.result.error;
    while (stored_.size() >= kStoredProbeLimit && !stored_.empty()) {
        stored_.erase(stored_.begin());
    }
    stored_[key] = std::move(stored);
    revision_ += 1;

    // Signals only now; each payload is a captured value or a freshly computed
    // string, never a reference into the catalog or an adapter map.
    if (hasThumbnail) {
        emit thumbnailChanged(QString::fromStdString(key));
    }
    emit revisionChanged();

    // A reentrant slot may have reset the project or relinked the source. The
    // stored result must still be this request's, for this project, before the
    // outcome is announced, so an old result can never leak as the new
    // project's success.
    const StoredProbe* current = validStoredProbe(key);
    if (current == nullptr || current->result.request.requestId != requestId) {
        return;
    }
    if (session_.document().mediaCatalog.entry(storedEntry) == nullptr) {
        return;
    }
    if (!error.empty()) {
        emit probeRejected(itemId, QString::fromStdString(error));
    } else {
        emit probeResultAvailable(itemId);
    }
}

void MediaLibraryModel::pruneThumbnailIndex() {
    if (latestRequest_.size() <= static_cast<std::size_t>(kThumbnailCacheCapacity)) {
        return;
    }
    // Mirror the bounded thumbnail cache: drop index entries whose pixels the
    // cache has already evicted, then cap.
    for (auto it = latestRequest_.begin(); it != latestRequest_.end();) {
        if (thumbnails_->find(QString::number(it->second.requestId)).isNull()) {
            it = latestRequest_.erase(it);
        } else {
            ++it;
        }
    }
    while (latestRequest_.size() > static_cast<std::size_t>(kThumbnailCacheCapacity) && !latestRequest_.empty()) {
        latestRequest_.erase(latestRequest_.begin());
    }
}

QString MediaLibraryModel::thumbnailUrl(const QString& id) const {
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId)) {
        return {};
    }
    const Document& document = session_.document();
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(entryId);
    if (!entry) {
        return {};
    }
    const auto latest = latestRequest_.find(entry->sourceKey);
    if (latest == latestRequest_.end()) {
        return {};
    }
    // A thumbnail produced under a replaced viewing transform is not shown; the
    // color-change refresh (or the visible-request hook) produces a new one.
    if (!(latest->second.colorPolicy == document.color) || latest->second.colorConfig != session_.colorConfigPath()) {
        return {};
    }
    const QString key = QString::number(latest->second.requestId);
    if (thumbnails_->find(key).isNull()) {
        return {};
    }
    return QStringLiteral("image://nemo-media/") + key;
}

QVariantMap MediaLibraryModel::probeState(const QString& id) {
    QVariantMap state;
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId)) {
        return state;
    }
    const Document& document = session_.document();
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(entryId);
    if (!entry) {
        return state;
    }
    // Visible-request hook: reopening a persisted project leaves the catalog
    // without thumbnails, so asking for an item's state queues a probe for the
    // current source reference and viewing transforms when none exists.
    ensureRuntime(entryId);
    entry = document.mediaCatalog.entry(entryId);
    if (!entry) {
        return state;
    }
    const std::string& key = entry->sourceKey;
    state.insert(QStringLiteral("id"), entryIdentity(entryId));
    state.insert(QStringLiteral("sourceId"), QString::fromStdString(key));
    state.insert(QStringLiteral("pending"), runtimePending(key));
    const StoredProbe* stored = validStoredProbe(key);
    state.insert(QStringLiteral("hasResult"), stored != nullptr);
    state.insert(QStringLiteral("applied"), entry->metadata.committedProbe.has_value());
    if (stored) {
        state.insert(QStringLiteral("kind"), mediaKindName(displayedMediaKind(*entry)));
        state.insert(QStringLiteral("offline"), stored->result.offline);
        state.insert(QStringLiteral("error"), QString::fromStdString(stored->result.error));
        state.insert(QStringLiteral("runtime"), runtimeFromResult(stored->result));
    } else {
        state.insert(QStringLiteral("kind"), mediaKindName(entry->metadata.kind));
        state.insert(QStringLiteral("offline"), effectiveOffline(*entry));
        state.insert(QStringLiteral("error"), QString());
        state.insert(QStringLiteral("runtime"), runtimeRecord(entry->metadata));
    }
    state.insert(QStringLiteral("thumbnailUrl"), thumbnailUrl(entryIdentity(entryId)));
    return state;
}

bool MediaLibraryModel::openMediaSource(const QString& group, const QString& sourceId) {
    if (!contextRouter_) {
        setError(QStringLiteral("Media viewing is unavailable: no panel context router"));
        return false;
    }
    const QString target = sourceId.trimmed();
    if (target.isEmpty()) {
        setError(QStringLiteral("Media source is unavailable: no source was selected"));
        return false;
    }
    if (!contextRouter_->openSource(group, target)) {
        setError(QStringLiteral("Media source '%1' cannot be opened in group %2").arg(target, group));
        return false;
    }
    clearError();
    return true;
}

QVariantMap MediaLibraryModel::defaultMarkForSource(const QString& sourceId) const {
    QVariantMap mark;
    mark.insert(QStringLiteral("sourceId"), sourceId);
    mark.insert(QStringLiteral("inFrame"), QVariant());
    mark.insert(QStringLiteral("outFrame"), QVariant());
    const Document& document = session_.document();
    for (const MediaCatalogEntry& entry : document.mediaCatalog.entries()) {
        if (entry.sourceKey != sourceId.toStdString() || entry.marks.empty()) {
            continue;
        }
        const nemo::MediaMarkRange& range = entry.marks.front();
        mark.insert(QStringLiteral("inFrame"),
                    range.inFrame ? QVariant(static_cast<qlonglong>(*range.inFrame)) : QVariant());
        mark.insert(QStringLiteral("outFrame"),
                    range.outFrame ? QVariant(static_cast<qlonglong>(*range.outFrame)) : QVariant());
        break;
    }
    return mark;
}

bool MediaLibraryModel::requestTimelineInsert(const QString& group, const QStringList& sourceIds, const QString& mode,
                                              const QVariantList& marks) {
    if (sourceIds.isEmpty()) {
        setError(QStringLiteral("No media sources are selected for insertion"));
        return false;
    }
    const QString insertMode = mode.trimmed().isEmpty() ? QStringLiteral("insert") : mode.trimmed();
    QVariantList ranges;
    ranges.reserve(sourceIds.size());
    for (int index = 0; index < sourceIds.size(); ++index) {
        // Repeated catalog occurrences are preserved in visible order.
        ranges.push_back(index < marks.size() ? marks.at(index) : QVariant(defaultMarkForSource(sourceIds.at(index))));
    }
    emit timelineInsertRequested(group, sourceIds, insertMode, ranges);
    return true;
}

bool MediaLibraryModel::revealMediaPanel(const QString& group) {
    if (!workspace_) {
        setError(QStringLiteral("Media reveal is unavailable: no workspace controller"));
        return false;
    }
    const QVariantMap root = workspace_->root();
    QString leafId;
    QString panelId;
    if (findWorkspacePanel(root, QStringLiteral("media"), group, leafId, panelId)) {
        workspace_->activate(leafId, panelId);
        clearError();
        return true;
    }
    QString targetLeaf;
    if (!findWorkspaceLeafWithPanel(root, QStringLiteral("timeline"), group, targetLeaf) &&
        !findFirstWorkspaceLeaf(root, targetLeaf)) {
        setError(QStringLiteral("No workspace leaf can host a media panel"));
        return false;
    }
    if (workspace_->createPanel(targetLeaf, QStringLiteral("media"), group).isEmpty()) {
        const QString workspaceError = workspace_->error();
        setError(workspaceError.isEmpty() ? QStringLiteral("The media panel could not be created") : workspaceError);
        return false;
    }
    clearError();
    return true;
}

QVariantList MediaLibraryModel::marks(const QString& id) const {
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId)) {
        return {};
    }
    const MediaCatalogEntry* entry = session_.document().mediaCatalog.entry(entryId);
    return entry ? markList(entry->marks) : QVariantList();
}

qint64 MediaLibraryModel::frameOffset(const QString& id) const {
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId)) {
        return 0;
    }
    const Document& document = session_.document();
    const MediaCatalogEntry* entry = document.mediaCatalog.entry(entryId);
    if (!entry) {
        return 0;
    }
    const auto source = document.sources.find(entry->sourceKey);
    return source == document.sources.end() ? 0 : static_cast<qint64>(source->second.frameOffset);
}

void MediaLibraryModel::failMediaChooser(QString message) {
    setError(message);
    emit mediaChooserFailed(message);
}

void MediaLibraryModel::setNativeFileChooser(NativeFileChooser* chooser) {
    chooser_ = chooser;
}

void MediaLibraryModel::chooseImportPaths(const QString& parentId) {
    if (!chooser_) {
        failMediaChooser(QStringLiteral("Media import chooser is unavailable"));
        return;
    }
    MediaBinId parent = kInvalidMediaBin;
    if (!parseBinIdentity(parentId, parent) ||
        (parent != kInvalidMediaBin && !session_.document().mediaCatalog.bin(parent))) {
        failMediaChooser(QStringLiteral("Import destination must be an existing bin"));
        return;
    }
    // The destination belongs to this request, not to the model: the outcome
    // handler carries it, so a refused request or another client's outcome can
    // never re-route the import. A refused request is not registered at all.
    const QString destination = binIdentity(parent);
    static_cast<void>(chooser_->openFiles(
        this,
        [this, destination](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Failed) {
                failMediaChooser(std::move(outcome.message));
                return;
            }
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled) {
                emit mediaChooserCancelled();
                return;
            }
            QString failure;
            const QStringList paths = chooserLocalPaths(outcome.urls, failure);
            if (!failure.isEmpty()) {
                failMediaChooser(std::move(failure));
                return;
            }
            emit importPathsChosen(paths, destination);
        },
        QStringLiteral("Import media"), true, mediaChooserFilters()));
}

void MediaLibraryModel::chooseRelinkPath(const QString& id) {
    if (!chooser_) {
        failMediaChooser(QStringLiteral("Media relink chooser is unavailable"));
        return;
    }
    MediaSourceId entryId = kInvalidMediaSource;
    if (!parseEntryIdentity(id, entryId) || !session_.document().mediaCatalog.entry(entryId)) {
        failMediaChooser(QStringLiteral("Unknown media item ") + id);
        return;
    }
    // As for import, the relink target is request-local.
    const QString target = entryIdentity(entryId);
    static_cast<void>(chooser_->openFiles(
        this,
        [this, target](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Failed) {
                failMediaChooser(std::move(outcome.message));
                return;
            }
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled) {
                emit mediaChooserCancelled();
                return;
            }
            QString failure;
            const QStringList paths = chooserLocalPaths(outcome.urls, failure);
            if (!failure.isEmpty()) {
                failMediaChooser(std::move(failure));
                return;
            }
            emit relinkPathChosen(target, paths.constFirst());
        },
        QStringLiteral("Relink media source"), false, mediaChooserFilters()));
}

}  // namespace nemo::ui
