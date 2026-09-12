// Media Library adapter tests (issue #43).
//
// These cover the seams the QML media panel consumes: prototype-shaped catalog
// gestures over the persistent catalog/commands, explicit open/reveal routing,
// ordered per-occurrence marked insertion intent, the runtime overlay that lets
// Offline smart/filter membership reflect a probe result without authoring
// metadata, and the request identity (source reference + viewing transforms)
// that rejects a stale probe/thumbnail after relink, project reset, or a
// viewing-transform change. Decode/format/color validation and the import
// service itself are covered by MediaImportTests and MediaTests; this file
// asserts only what the adapter adds on top.

#include "MediaLibraryModel.hpp"
#include "PanelContextRouter.hpp"
#include "WorkspaceController.hpp"

#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/MediaImportService.hpp"

#include <QDir>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using nemo::MediaCatalogEntry;
using nemo::MediaKind;
using nemo::MediaSourceId;
using nemo::ProjectSession;

// Minimal OCIO v2 config with two matrix-only display views, so a viewing
// transform change is a real, resolvable change. The transform math is covered
// by ColorTests; here it only has to resolve.
std::filesystem::path writeColorConfig(const std::filesystem::path& directory) {
    const auto path = directory / "media-library-color.ocio";
    std::string config;
    config += "ocio_profile_version: 2\n";
    config += "search_path: \"\"\n";
    config += "roles:\n  default: linear\n  scene_linear: linear\n";
    config += "colorspaces:\n";
    config += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
    config += "  - !<ColorSpace>\n    name: display_view\n";
    config +=
        "    from_reference: !<MatrixTransform> {matrix: [1.1, 0.0, 0.0, 0.0, 0.0, 0.95, 0.0, 0.0, 0.0, 0.0, 1.05, "
        "0.0, 0.0, 0.0, 0.0, 1.0]}\n";
    config += "  - !<ColorSpace>\n    name: display_view_alt\n";
    config += "    from_reference: !<MatrixTransform> {matrix: [0.9, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.1, "
              "0.0, 0.0, 0.0, 0.0, 1.0]}\n";
    config += "displays:\n  sRGB:\n";
    config += "    - !<View> {name: rec709, colorspace: display_view}\n";
    config += "    - !<View> {name: alt, colorspace: display_view_alt}\n";
    std::ofstream file(path);
    file << config;
    return path;
}

// A real, decodable EXR still of the requested size: the import service reads
// it through the shared image adapter, so no format is simulated here.
std::filesystem::path writeExrFixture(const std::filesystem::path& directory, const std::string& name, int width,
                                      int height) {
    nemo::CpuImage image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, {0.25F, 0.5F, 0.75F, 1.0F});
        }
    }
    const auto path = directory / (name + ".exr");
    nemo::media::writeImage(path.string(), image, nemo::media::OutputPrecision::Half);
    return path;
}

QVariantMap operation(const QString& type) {
    QVariantMap map;
    map.insert(QStringLiteral("type"), type);
    return map;
}

QString mediaIdentity(MediaSourceId id) {
    return QStringLiteral("media-") + QString::number(id);
}

MediaSourceId entryIdOf(const QString& identity) {
    return static_cast<MediaSourceId>(identity.mid(6).toULongLong());
}

QStringList recordIds(const QVariantList& records) {
    QStringList ids;
    for (const QVariant& value : records) {
        ids.append(value.toMap().value(QStringLiteral("id")).toString());
    }
    return ids;
}

QStringList recordNames(const QVariantList& records) {
    QStringList names;
    for (const QVariant& value : records) {
        names.append(value.toMap().value(QStringLiteral("name")).toString());
    }
    return names;
}

QString firstMediaId(const nemo::ui::MediaLibraryModel& model, const QString& parentId = QStringLiteral("root")) {
    for (const QVariant& value : model.children(parentId)) {
        const QVariantMap record = value.toMap();
        if (record.value(QStringLiteral("kind")).toString() != QStringLiteral("bin")) {
            return record.value(QStringLiteral("id")).toString();
        }
    }
    return {};
}

QString binIdNamed(const nemo::ui::MediaLibraryModel& model, const QString& parentId, const QString& name) {
    for (const QVariant& value : model.children(parentId)) {
        const QVariantMap record = value.toMap();
        if (record.value(QStringLiteral("name")).toString() == name) {
            return record.value(QStringLiteral("id")).toString();
        }
    }
    return {};
}

// Pumps the GUI event loop (the adapter polls the import service on a QTimer)
// until the predicate holds or the deadline passes.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        QTest::qWait(20);
    }
    return predicate();
}

QString firstLeafId(const QVariantMap& node) {
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("split")) {
        for (const QVariant& child : node.value(QStringLiteral("children")).toList()) {
            const QString id = firstLeafId(child.toMap());
            if (!id.isEmpty()) {
                return id;
            }
        }
        return {};
    }
    return node.value(QStringLiteral("id")).toString();
}

QVariantMap leafById(const QVariantMap& node, const QString& leafId) {
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("split")) {
        for (const QVariant& child : node.value(QStringLiteral("children")).toList()) {
            const QVariantMap found = leafById(child.toMap(), leafId);
            if (!found.isEmpty()) {
                return found;
            }
        }
        return {};
    }
    return node.value(QStringLiteral("id")).toString() == leafId ? node : QVariantMap();
}

QVariantList panelsOfType(const QVariantMap& node, const QString& type) {
    QVariantList panels;
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("split")) {
        for (const QVariant& child : node.value(QStringLiteral("children")).toList()) {
            panels.append(panelsOfType(child.toMap(), type));
        }
        return panels;
    }
    for (const QVariant& value : node.value(QStringLiteral("panels")).toList()) {
        const QVariantMap panel = value.toMap();
        if (panel.value(QStringLiteral("type")).toString() == type) {
            panels.append(panel);
        }
    }
    return panels;
}

class MediaLibraryFixture {
public:
    MediaLibraryFixture() : model_(session_, importer_) {
        session_.setColorConfigPath(writeColorConfig(temporary_.path().toStdString()).string());
    }

    // Seeds a Document source and imports a catalog reference without decoding.
    MediaSourceId importReference(const std::string& key, const QString& name,
                                  const QString& parentId = QStringLiteral("root"), std::int64_t frameOffset = 0) {
        nemo::MediaBinId parent = nemo::kInvalidMediaBin;
        if (!parentId.isEmpty() && parentId != QLatin1String("root")) {
            parent = static_cast<nemo::MediaBinId>(parentId.mid(4).toULongLong());
        }
        nemo::MediaMetadata metadata;
        metadata.userName = name.toStdString();
        auto created = std::make_shared<MediaSourceId>(nemo::kInvalidMediaSource);
        nemo::SourceReference reference;
        reference.path = "/media/" + key;
        reference.frameOffset = frameOffset;
        std::vector<nemo::Command> commands;
        commands.push_back(nemo::setSourceCommand(key, reference));
        commands.push_back(nemo::importMediaReferenceCommand(key, parent, metadata, created));
        const auto result = session_.submit(nemo::transactionCommand("seed media", std::move(commands)),
                                            {.expectedRevision = session_.revision()});
        EXPECT_TRUE(result.committed);
        return *created;
    }

    QTemporaryDir temporary_;
    ProjectSession session_;
    nemo::media::MediaImportService importer_;
    nemo::ui::MediaLibraryModel model_;
};

}  // namespace

TEST(MediaLibraryModel, CatalogGesturesAreAtomicAndKeepStableSourceIdentity) {
    MediaLibraryFixture fixture;

    auto createBin = operation(QStringLiteral("createBin"));
    createBin.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    createBin.insert(QStringLiteral("name"), QStringLiteral("Footage"));
    ASSERT_TRUE(fixture.model_.apply(createBin));
    const QString binId = binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("Footage"));
    ASSERT_FALSE(binId.isEmpty());

    const MediaSourceId entry = fixture.importReference("shot", QStringLiteral("Shot"));
    const QString mediaId = mediaIdentity(entry);
    QVariantMap item = fixture.model_.item(mediaId).toMap();
    EXPECT_EQ(item.value(QStringLiteral("parentId")).toString(), QStringLiteral("root"));
    EXPECT_EQ(item.value(QStringLiteral("sourceId")).toString(), QStringLiteral("shot"));

    auto move = operation(QStringLiteral("move"));
    move.insert(QStringLiteral("ids"), QVariantList{mediaId});
    move.insert(QStringLiteral("parentId"), binId);
    ASSERT_TRUE(fixture.model_.apply(move));
    EXPECT_EQ(fixture.model_.item(mediaId).toMap().value(QStringLiteral("parentId")).toString(), binId);

    auto renameMedia = operation(QStringLiteral("rename"));
    renameMedia.insert(QStringLiteral("id"), mediaId);
    renameMedia.insert(QStringLiteral("name"), QStringLiteral("Take 1"));
    ASSERT_TRUE(fixture.model_.apply(renameMedia));
    auto renameBin = operation(QStringLiteral("rename"));
    renameBin.insert(QStringLiteral("id"), binId);
    renameBin.insert(QStringLiteral("name"), QStringLiteral("Reel 1"));
    ASSERT_TRUE(fixture.model_.apply(renameBin));

    item = fixture.model_.item(mediaId).toMap();
    EXPECT_EQ(item.value(QStringLiteral("name")).toString(), QStringLiteral("Take 1"));
    EXPECT_EQ(item.value(QStringLiteral("sourceId")).toString(), QStringLiteral("shot"));
    EXPECT_EQ(fixture.model_.item(binId).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Reel 1"));

    ASSERT_TRUE(fixture.model_.undo());
    EXPECT_EQ(fixture.model_.item(binId).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Footage"));
    ASSERT_TRUE(fixture.model_.redo());
    EXPECT_EQ(fixture.model_.item(binId).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Reel 1"));

    // A cycle/self move rejects atomically and leaves the catalog untouched.
    auto badMove = operation(QStringLiteral("move"));
    badMove.insert(QStringLiteral("ids"), QVariantList{binId});
    badMove.insert(QStringLiteral("parentId"), binId);
    EXPECT_FALSE(fixture.model_.apply(badMove));
    EXPECT_FALSE(fixture.model_.error().isEmpty());
    EXPECT_EQ(fixture.model_.item(mediaId).toMap().value(QStringLiteral("parentId")).toString(), binId);
}

TEST(MediaLibraryModel, DuplicateBuildsAnIndependentSubtreeAndUsedMediaIsProtected) {
    MediaLibraryFixture fixture;

    auto createOuter = operation(QStringLiteral("createBin"));
    createOuter.insert(QStringLiteral("name"), QStringLiteral("Outer"));
    ASSERT_TRUE(fixture.model_.apply(createOuter));
    const QString outer = binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("Outer"));
    ASSERT_FALSE(outer.isEmpty());
    auto createInner = operation(QStringLiteral("createBin"));
    createInner.insert(QStringLiteral("parentId"), outer);
    createInner.insert(QStringLiteral("name"), QStringLiteral("Inner"));
    ASSERT_TRUE(fixture.model_.apply(createInner));
    const QString inner = binIdNamed(fixture.model_, outer, QStringLiteral("Inner"));
    ASSERT_FALSE(inner.isEmpty());

    const MediaSourceId entry = fixture.importReference("shot", QStringLiteral("Shot"));
    auto move = operation(QStringLiteral("move"));
    move.insert(QStringLiteral("ids"), QVariantList{mediaIdentity(entry)});
    move.insert(QStringLiteral("parentId"), inner);
    ASSERT_TRUE(fixture.model_.apply(move));

    auto duplicate = operation(QStringLiteral("duplicate"));
    duplicate.insert(QStringLiteral("ids"), QVariantList{outer});
    duplicate.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    ASSERT_TRUE(fixture.model_.apply(duplicate));
    const QString copiedOuter = binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("Outer Copy"));
    ASSERT_FALSE(copiedOuter.isEmpty());
    EXPECT_EQ(recordNames(fixture.model_.children(copiedOuter)), QStringList{QStringLiteral("Inner")});
    const QString copiedInner = binIdNamed(fixture.model_, copiedOuter, QStringLiteral("Inner"));
    ASSERT_FALSE(copiedInner.isEmpty());
    const QVariantList copiedChildren = fixture.model_.children(copiedInner);
    ASSERT_EQ(copiedChildren.size(), 1);
    const QVariantMap duplicateRecord = copiedChildren.first().toMap();
    EXPECT_NE(duplicateRecord.value(QStringLiteral("id")).toString(), mediaIdentity(entry));
    EXPECT_EQ(duplicateRecord.value(QStringLiteral("sourceId")).toString(), QStringLiteral("shot"));

    auto renameDuplicate = operation(QStringLiteral("rename"));
    renameDuplicate.insert(QStringLiteral("id"), duplicateRecord.value(QStringLiteral("id")).toString());
    renameDuplicate.insert(QStringLiteral("name"), QStringLiteral("Duplicated"));
    ASSERT_TRUE(fixture.model_.apply(renameDuplicate));
    EXPECT_EQ(fixture.model_.item(mediaIdentity(entry)).toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Shot"));

    // A source addressed by a source node cannot be removed.
    const nemo::NetworkId network = fixture.session_.document().rootNetworkId();
    auto nodeId = std::make_shared<nemo::NodeId>(nemo::kInvalidNode);
    ASSERT_TRUE(fixture.session_
                    .submit(nemo::addNodeCommand(network, "source", "Source", nodeId),
                            {.expectedRevision = fixture.session_.revision()})
                    .committed);
    ASSERT_TRUE(
        fixture.session_
            .submit(nemo::setParamCommand(network, *nodeId, "source", nemo::ParameterValue{std::string("shot")}),
                    {.expectedRevision = fixture.session_.revision()})
            .committed);

    auto removeUsed = operation(QStringLiteral("remove"));
    removeUsed.insert(QStringLiteral("ids"), QVariantList{mediaIdentity(entry)});
    removeUsed.insert(QStringLiteral("keepContents"), false);
    EXPECT_FALSE(fixture.model_.apply(removeUsed));
    EXPECT_FALSE(fixture.model_.error().isEmpty());
    EXPECT_TRUE(fixture.model_.item(mediaIdentity(entry)).isValid());

    // The duplicate shares the source key, so it is protected too; removing the
    // empty shell bin with keepContents still reparents nothing and undo restores.
    auto removeShell = operation(QStringLiteral("remove"));
    removeShell.insert(QStringLiteral("ids"), QVariantList{copiedOuter});
    removeShell.insert(QStringLiteral("keepContents"), true);
    ASSERT_TRUE(fixture.model_.apply(removeShell));
    EXPECT_FALSE(fixture.model_.item(copiedOuter).isValid());
    EXPECT_TRUE(fixture.model_.item(copiedInner).isValid());
    ASSERT_TRUE(fixture.model_.undo());
    EXPECT_TRUE(fixture.model_.item(copiedOuter).isValid());
}

TEST(MediaLibraryModel, MetadataGesturePublishesOnlyRequestedFields) {
    MediaLibraryFixture fixture;
    const MediaSourceId entry = fixture.importReference("shot", QStringLiteral("Shot"));
    const QString mediaId = mediaIdentity(entry);

    auto metadata = operation(QStringLiteral("metadata"));
    metadata.insert(QStringLiteral("ids"), QVariantList{mediaId});
    metadata.insert(QStringLiteral("changes"),
                    QVariantMap{{QStringLiteral("tags"), QStringList{QStringLiteral("hero")}},
                                {QStringLiteral("description"), QStringLiteral("take 1")},
                                {QStringLiteral("color"), QStringLiteral("#d9855d")}});
    ASSERT_TRUE(fixture.model_.apply(metadata));

    const QVariantMap item = fixture.model_.item(mediaId).toMap();
    EXPECT_EQ(item.value(QStringLiteral("name")).toString(), QStringLiteral("Shot"));
    EXPECT_EQ(item.value(QStringLiteral("description")).toString(), QStringLiteral("take 1"));
    EXPECT_EQ(item.value(QStringLiteral("color")).toString(), QStringLiteral("#d9855d"));
    EXPECT_EQ(item.value(QStringLiteral("tags")).toStringList(), QStringList{QStringLiteral("hero")});

    auto unsupported = operation(QStringLiteral("metadata"));
    unsupported.insert(QStringLiteral("ids"), QVariantList{mediaId});
    unsupported.insert(QStringLiteral("changes"), QVariantMap{{QStringLiteral("kind"), QStringLiteral("video")}});
    EXPECT_FALSE(fixture.model_.apply(unsupported));
    EXPECT_TRUE(fixture.model_.error().contains(QStringLiteral("Unsupported metadata field")));

    // Bins carry the same authored metadata fields; the prototype edits any
    // catalog item and clones the record on duplicate.
    auto createBin = operation(QStringLiteral("createBin"));
    createBin.insert(QStringLiteral("name"), QStringLiteral("Selects"));
    ASSERT_TRUE(fixture.model_.apply(createBin));
    const QString binId = binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("Selects"));
    ASSERT_FALSE(binId.isEmpty());
    auto binMetadata = operation(QStringLiteral("metadata"));
    binMetadata.insert(QStringLiteral("ids"), QVariantList{binId});
    binMetadata.insert(QStringLiteral("changes"),
                       QVariantMap{{QStringLiteral("tags"), QStringList{QStringLiteral("selects")}},
                                   {QStringLiteral("description"), QStringLiteral("keeper")},
                                   {QStringLiteral("color"), QStringLiteral("#86aa72")}});
    ASSERT_TRUE(fixture.model_.apply(binMetadata));
    const QVariantMap bin = fixture.model_.item(binId).toMap();
    EXPECT_EQ(bin.value(QStringLiteral("description")).toString(), QStringLiteral("keeper"));
    EXPECT_EQ(bin.value(QStringLiteral("color")).toString(), QStringLiteral("#86aa72"));
    EXPECT_EQ(bin.value(QStringLiteral("tags")).toStringList(), QStringList{QStringLiteral("selects")});

    auto duplicateBin = operation(QStringLiteral("duplicate"));
    duplicateBin.insert(QStringLiteral("ids"), QVariantList{binId});
    duplicateBin.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    ASSERT_TRUE(fixture.model_.apply(duplicateBin));
    const QString copiedBin = binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("Selects Copy"));
    ASSERT_FALSE(copiedBin.isEmpty());
    EXPECT_EQ(fixture.model_.item(copiedBin).toMap().value(QStringLiteral("description")).toString(),
              QStringLiteral("keeper"));

    // Search reaches bin metadata, as the prototype's text match did.
    EXPECT_TRUE(recordIds(fixture.model_.query(QStringLiteral("root"), QStringLiteral("keeper"), false, QVariantMap{}))
                    .contains(binId));
}

TEST(MediaLibraryModel, PersistentSmartQueryBinsTrackMetadataAndUndo) {
    MediaLibraryFixture fixture;
    const MediaSourceId first = fixture.importReference("asset-a", QStringLiteral("Shot A"));
    const MediaSourceId second = fixture.importReference("asset-b", QStringLiteral("B roll"));

    auto create = operation(QStringLiteral("createSmartBin"));
    create.insert(QStringLiteral("name"), QStringLiteral("Shots"));
    create.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    create.insert(QStringLiteral("query"), QVariantMap{{QStringLiteral("parentId"), QStringLiteral("project")},
                                                       {QStringLiteral("projectScope"), true},
                                                       {QStringLiteral("searchText"), QStringLiteral("shot")},
                                                       {QStringLiteral("kindFilter"), QStringLiteral("all")},
                                                       {QStringLiteral("mediaOnly"), false},
                                                       {QStringLiteral("unusedOnly"), false},
                                                       {QStringLiteral("offlineOnly"), false}});
    ASSERT_TRUE(fixture.model_.apply(create));

    const QVariantList bins = fixture.model_.smartBins();
    ASSERT_EQ(bins.size(), 1);
    const QVariantMap bin = bins.first().toMap();
    EXPECT_EQ(bin.value(QStringLiteral("name")).toString(), QStringLiteral("Shots"));
    EXPECT_FALSE(bin.value(QStringLiteral("builtIn")).toBool());
    EXPECT_EQ(bin.value(QStringLiteral("query")).toMap().value(QStringLiteral("searchText")).toString(),
              QStringLiteral("shot"));
    const QString binId = bin.value(QStringLiteral("id")).toString();

    const auto matches = [&] {
        return fixture.model_.query(QStringLiteral("project"), QStringLiteral("shot"), true, QVariantMap{});
    };
    EXPECT_EQ(recordIds(matches()), QStringList{mediaIdentity(first)});

    // The saved query follows authored metadata: renaming the second item to
    // match adds it without touching the bin.
    auto rename = operation(QStringLiteral("rename"));
    rename.insert(QStringLiteral("id"), mediaIdentity(second));
    rename.insert(QStringLiteral("name"), QStringLiteral("shot B"));
    ASSERT_TRUE(fixture.model_.apply(rename));
    EXPECT_EQ(recordIds(matches()), (QStringList{mediaIdentity(first), mediaIdentity(second)}));

    // Undo restores the metadata, then removing the smart bin clears it.
    ASSERT_TRUE(fixture.model_.undo());
    EXPECT_EQ(recordIds(matches()), QStringList{mediaIdentity(first)});
    auto remove = operation(QStringLiteral("deleteSmartBin"));
    remove.insert(QStringLiteral("id"), binId);
    ASSERT_TRUE(fixture.model_.apply(remove));
    EXPECT_TRUE(fixture.model_.smartBins().isEmpty());

    // An all-default saved search is a valid smart-all query; only an explicit
    // nullopt means a plain bin.
    auto allQuery = operation(QStringLiteral("createSmartBin"));
    allQuery.insert(QStringLiteral("name"), QStringLiteral("Everything"));
    allQuery.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    allQuery.insert(QStringLiteral("query"), QVariantMap{{QStringLiteral("parentId"), QStringLiteral("project")},
                                                         {QStringLiteral("projectScope"), true},
                                                         {QStringLiteral("searchText"), QString()},
                                                         {QStringLiteral("kindFilter"), QStringLiteral("all")},
                                                         {QStringLiteral("mediaOnly"), true},
                                                         {QStringLiteral("unusedOnly"), false},
                                                         {QStringLiteral("offlineOnly"), false}});
    ASSERT_TRUE(fixture.model_.apply(allQuery));
    ASSERT_EQ(fixture.model_.smartBins().size(), 1);
    QString everythingId;
    for (const QVariant& value : fixture.model_.smartBins()) {
        const QVariantMap smart = value.toMap();
        if (smart.value(QStringLiteral("name")).toString() == QStringLiteral("Everything")) {
            everythingId = smart.value(QStringLiteral("id")).toString();
        }
    }
    ASSERT_FALSE(everythingId.isEmpty());
    const nemo::Document& document = fixture.session_.document();
    const std::vector<MediaSourceId> members =
        document.mediaCatalog.smartMembers(document, static_cast<nemo::MediaBinId>(everythingId.mid(4).toULongLong()));
    EXPECT_EQ(members.size(), 2U);
}

TEST(MediaLibraryModel, QueryHonorsCoreScopeAndMissingScopeIsUnavailable) {
    MediaLibraryFixture fixture;
    auto createBin = operation(QStringLiteral("createBin"));
    createBin.insert(QStringLiteral("name"), QStringLiteral("Selects"));
    ASSERT_TRUE(fixture.model_.apply(createBin));
    const QString binId = binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("Selects"));
    ASSERT_FALSE(binId.isEmpty());

    const MediaSourceId rootEntry = fixture.importReference("shot-a", QStringLiteral("Shot A"));
    const MediaSourceId binEntry = fixture.importReference("shot-b", QStringLiteral("Shot B"), binId);

    QVariantMap mediaOnly;
    mediaOnly.insert(QStringLiteral("mediaOnly"), true);
    // Project scope, the root's direct children, and an explicit bin scope are
    // three distinct memberships, resolved by the core query owner.
    EXPECT_EQ(recordIds(fixture.model_.query(QStringLiteral("project"), QString(), true, mediaOnly)),
              (QStringList{mediaIdentity(rootEntry), mediaIdentity(binEntry)}));
    EXPECT_EQ(recordIds(fixture.model_.query(QStringLiteral("root"), QString(), false, mediaOnly)),
              QStringList{mediaIdentity(rootEntry)});
    EXPECT_EQ(recordIds(fixture.model_.query(binId, QString(), false, mediaOnly)),
              QStringList{mediaIdentity(binEntry)});

    // A saved search scoped to a bin persists the typed scope and evaluates
    // against it.
    auto scoped = operation(QStringLiteral("createSmartBin"));
    scoped.insert(QStringLiteral("name"), QStringLiteral("In Selects"));
    scoped.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    scoped.insert(QStringLiteral("query"), QVariantMap{{QStringLiteral("parentId"), binId},
                                                       {QStringLiteral("projectScope"), false},
                                                       {QStringLiteral("searchText"), QString()},
                                                       {QStringLiteral("kindFilter"), QStringLiteral("all")},
                                                       {QStringLiteral("mediaOnly"), true},
                                                       {QStringLiteral("unusedOnly"), false},
                                                       {QStringLiteral("offlineOnly"), false}});
    ASSERT_TRUE(fixture.model_.apply(scoped));
    QString scopedId;
    for (const QVariant& value : fixture.model_.smartBins()) {
        const QVariantMap smart = value.toMap();
        if (smart.value(QStringLiteral("name")).toString() == QStringLiteral("In Selects")) {
            scopedId = smart.value(QStringLiteral("id")).toString();
            EXPECT_EQ(smart.value(QStringLiteral("query")).toMap().value(QStringLiteral("parentId")).toString(), binId);
        }
    }
    ASSERT_FALSE(scopedId.isEmpty());
    const nemo::Document& document = fixture.session_.document();
    const std::vector<MediaSourceId> members =
        document.mediaCatalog.smartMembers(document, static_cast<nemo::MediaBinId>(scopedId.mid(4).toULongLong()));
    ASSERT_EQ(members.size(), 1U);
    EXPECT_EQ(members.front(), binEntry);

    // A deleted scope is unavailable, never a root fallback.
    auto remove = operation(QStringLiteral("remove"));
    remove.insert(QStringLiteral("ids"), QVariantList{binId});
    remove.insert(QStringLiteral("keepContents"), true);
    ASSERT_TRUE(fixture.model_.apply(remove));
    EXPECT_TRUE(fixture.model_.query(binId, QString(), false, mediaOnly).isEmpty());
}

TEST(MediaLibraryModel, BrowsingNeverOpensSourcesButExplicitOpenRoutesPerGroup) {
    MediaLibraryFixture fixture;
    fixture.importReference("shot", QStringLiteral("Shot"));

    nemo::ui::PanelContextRouter router(fixture.session_);
    ASSERT_TRUE(router.registerPanel(QStringLiteral("viewer-a"), QStringLiteral("A")));
    ASSERT_TRUE(router.registerPanel(QStringLiteral("viewer-b"), QStringLiteral("B")));
    fixture.model_.setContextRouter(&router);

    // Browsing is presentation-only: no routing target changes.
    static_cast<void>(fixture.model_.children(QStringLiteral("root")));
    static_cast<void>(fixture.model_.item(mediaIdentity(MediaSourceId{1})));
    static_cast<void>(fixture.model_.query(QStringLiteral("project"), QString(), true, QVariantMap{}));
    EXPECT_TRUE(
        router.contextFor(QStringLiteral("viewer-a")).value(QStringLiteral("sourceTarget")).toString().isEmpty());
    EXPECT_TRUE(
        router.contextFor(QStringLiteral("viewer-b")).value(QStringLiteral("sourceTarget")).toString().isEmpty());

    EXPECT_TRUE(fixture.model_.openMediaSource(QStringLiteral("A"), QStringLiteral("shot")));
    EXPECT_EQ(router.contextFor(QStringLiteral("viewer-a")).value(QStringLiteral("sourceTarget")).toString(),
              QStringLiteral("shot"));
    EXPECT_TRUE(
        router.contextFor(QStringLiteral("viewer-b")).value(QStringLiteral("sourceTarget")).toString().isEmpty());

    EXPECT_FALSE(fixture.model_.openMediaSource(QStringLiteral("A"), QStringLiteral("missing")));
    EXPECT_FALSE(fixture.model_.error().isEmpty());
}

TEST(MediaLibraryModel, RevealActivatesExistingPanelPerGroupAndCreatesOnlyWhenAbsent) {
    MediaLibraryFixture fixture;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    nemo::workspace::WorkspaceController workspace(directory.filePath(QStringLiteral("workspace.json")));
    workspace.registerPanelType(QStringLiteral("media"), QStringLiteral("Media Bin"),
                                QStringLiteral("MediaBinPanel.qml"), QString());
    workspace.registerPanelType(QStringLiteral("timeline"), QStringLiteral("Timeline"),
                                QStringLiteral("TimelinePanel.qml"), QString());
    fixture.model_.setWorkspaceController(&workspace);

    const QString leaf = firstLeafId(workspace.root());
    ASSERT_FALSE(leaf.isEmpty());
    const QString panelA = workspace.createPanel(leaf, QStringLiteral("media"), QStringLiteral("A"));
    const QString panelB = workspace.createPanel(leaf, QStringLiteral("media"), QStringLiteral("B"));
    ASSERT_FALSE(panelA.isEmpty());
    ASSERT_FALSE(panelB.isEmpty());

    // Revealing an existing group activates its panel; no second panel appears.
    EXPECT_TRUE(fixture.model_.revealMediaPanel(QStringLiteral("A")));
    EXPECT_EQ(leafById(workspace.root(), leaf).value(QStringLiteral("active")).toString(), panelA);
    EXPECT_TRUE(fixture.model_.revealMediaPanel(QStringLiteral("B")));
    EXPECT_EQ(leafById(workspace.root(), leaf).value(QStringLiteral("active")).toString(), panelB);
    EXPECT_EQ(panelsOfType(workspace.root(), QStringLiteral("media")).size(), 2);

    // A group without a media panel gets one created through the same registry.
    EXPECT_TRUE(fixture.model_.revealMediaPanel(QStringLiteral("C")));
    const QVariantList mediaPanels = panelsOfType(workspace.root(), QStringLiteral("media"));
    ASSERT_EQ(mediaPanels.size(), 3);
    QStringList groups;
    for (const QVariant& value : mediaPanels) {
        groups.append(value.toMap().value(QStringLiteral("group")).toString());
    }
    EXPECT_TRUE(groups.contains(QStringLiteral("C")));
}

TEST(MediaLibraryModel, InsertionIntentPreservesOrderRepeatsAndPerEntryMarks) {
    MediaLibraryFixture fixture;
    const MediaSourceId first = fixture.importReference("shot", QStringLiteral("Shot"), QStringLiteral("root"), 12);

    auto duplicate = operation(QStringLiteral("duplicate"));
    duplicate.insert(QStringLiteral("ids"), QVariantList{mediaIdentity(first)});
    duplicate.insert(QStringLiteral("parentId"), QStringLiteral("root"));
    ASSERT_TRUE(fixture.model_.apply(duplicate));

    QString secondId;
    for (const QVariant& value : fixture.model_.children(QStringLiteral("root"))) {
        const QVariantMap record = value.toMap();
        const QString id = record.value(QStringLiteral("id")).toString();
        if (id != mediaIdentity(first) &&
            record.value(QStringLiteral("sourceId")).toString() == QStringLiteral("shot")) {
            secondId = id;
        }
    }
    ASSERT_FALSE(secondId.isEmpty());

    const auto setMarks = [&](const QString& id, std::int64_t in, std::int64_t out) {
        std::vector<nemo::MediaMarkRange> ranges{nemo::MediaMarkRange{.inFrame = in, .outFrame = out}};
        const auto result = fixture.session_.submit(nemo::setMediaMarksCommand(entryIdOf(id), std::move(ranges)),
                                                    {.expectedRevision = fixture.session_.revision()});
        ASSERT_TRUE(result.committed);
    };
    setMarks(mediaIdentity(first), 2, 8);
    setMarks(secondId, 5, 5);

    // Each occurrence keeps its own authored range on the record.
    EXPECT_EQ(fixture.model_.item(mediaIdentity(first))
                  .toMap()
                  .value(QStringLiteral("marks"))
                  .toList()
                  .first()
                  .toMap()
                  .value(QStringLiteral("inFrame"))
                  .toInt(),
              2);
    EXPECT_EQ(fixture.model_.marks(secondId).toList().first().toMap().value(QStringLiteral("inFrame")).toInt(), 5);
    EXPECT_EQ(fixture.model_.frameOffset(mediaIdentity(first)), 12);

    QSignalSpy spy(&fixture.model_, &nemo::ui::MediaLibraryModel::timelineInsertRequested);
    const QVariantList marks{QVariantMap{{QStringLiteral("inFrame"), 2}, {QStringLiteral("outFrame"), 8}},
                             QVariantMap{{QStringLiteral("inFrame"), 5}, {QStringLiteral("outFrame"), 5}}};
    ASSERT_TRUE(fixture.model_.requestTimelineInsert(QStringLiteral("A"),
                                                     QStringList{QStringLiteral("shot"), QStringLiteral("shot")},
                                                     QStringLiteral("overwrite"), marks));
    ASSERT_EQ(spy.count(), 1);
    const QList<QVariant> arguments = spy.at(0);
    EXPECT_EQ(arguments.at(0).toString(), QStringLiteral("A"));
    const QStringList sourceIds = arguments.at(1).toStringList();
    EXPECT_EQ(sourceIds, (QStringList{QStringLiteral("shot"), QStringLiteral("shot")}));
    EXPECT_EQ(arguments.at(2).toString(), QStringLiteral("overwrite"));
    const QVariantList received = arguments.at(3).toList();
    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(received.at(0).toMap().value(QStringLiteral("inFrame")).toInt(), 2);
    EXPECT_EQ(received.at(1).toMap().value(QStringLiteral("inFrame")).toInt(), 5);

    // Without an explicit marks list the first authored range of the source is
    // used; the panel always passes per-occurrence marks instead.
    QSignalSpy defaultSpy(&fixture.model_, &nemo::ui::MediaLibraryModel::timelineInsertRequested);
    ASSERT_TRUE(fixture.model_.requestTimelineInsert(QStringLiteral("A"), QStringList{QStringLiteral("shot")},
                                                     QStringLiteral("insert"), {}));
    ASSERT_EQ(defaultSpy.count(), 1);
    EXPECT_EQ(defaultSpy.at(0).at(3).toList().first().toMap().value(QStringLiteral("inFrame")).toInt(), 2);
}

TEST(MediaLibraryModel, RuntimeResultIsRejectedAfterRelinkAndReprobedPerEntry) {
    MediaLibraryFixture fixture;
    const auto first = writeExrFixture(fixture.temporary_.path().toStdString(), "relink-a", 8, 4);
    const auto second = writeExrFixture(fixture.temporary_.path().toStdString(), "relink-b", 16, 6);

    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(first.string())}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());
    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));

    const auto committedProbe = [&]() -> const nemo::MediaProbeMetadata* {
        const MediaCatalogEntry* entry = fixture.session_.document().mediaCatalog.entry(entryIdOf(mediaId));
        return entry && entry->metadata.committedProbe ? &*entry->metadata.committedProbe : nullptr;
    };
    ASSERT_TRUE(fixture.model_.applyProbe(mediaId));
    ASSERT_NE(committedProbe(), nullptr);
    EXPECT_EQ(committedProbe()->width, 8);

    // Relinking swaps the source reference: the stored result for the old
    // reference is dropped, so nothing stale can be applied.
    ASSERT_TRUE(fixture.model_.relink(mediaId, QString::fromStdString(second.string())));
    EXPECT_FALSE(fixture.model_.applyProbe(mediaId));
    EXPECT_EQ(fixture.model_.frameOffset(mediaId), 0);

    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));
    ASSERT_TRUE(fixture.model_.applyProbe(mediaId));
    ASSERT_NE(committedProbe(), nullptr);
    EXPECT_EQ(committedProbe()->width, 16);

    // A fresh probe of the same entry is queued and collected again.
    ASSERT_TRUE(fixture.model_.reprobe(mediaId));
    EXPECT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));
}

TEST(MediaLibraryModel, ProjectResetDropsRuntimeStateAndRepopulatesLazily) {
    MediaLibraryFixture fixture;
    const auto source = writeExrFixture(fixture.temporary_.path().toStdString(), "reset", 10, 8);
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());
    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));

    const nemo::ProjectReplaceResult replaced = fixture.session_.replaceDocument(nemo::Document{});
    ASSERT_TRUE(replaced.replaced);
    EXPECT_FALSE(fixture.model_.item(mediaId).isValid());
    EXPECT_FALSE(fixture.model_.applyProbe(mediaId));

    // A new project can allocate the same decimal identity; the previous
    // project's stored result must not leak into it.
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 3));
    const QString newId = firstMediaId(fixture.model_);
    ASSERT_FALSE(newId.isEmpty());
    EXPECT_EQ(fixture.model_.frameOffset(newId), 3);
    EXPECT_FALSE(fixture.model_.applyProbe(newId));
    ASSERT_TRUE(waitFor([&] { return fixture.model_.probeState(newId).value(QStringLiteral("hasResult")).toBool(); }));
    ASSERT_TRUE(fixture.model_.applyProbe(newId));
    const MediaCatalogEntry* entry = fixture.session_.document().mediaCatalog.entry(entryIdOf(newId));
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->width, 10);
}

TEST(MediaLibraryModel, MetadataEditsKeepAProbeFreshButAViewingTransformChangeInvalidatesIt) {
    MediaLibraryFixture fixture;
    const auto source = writeExrFixture(fixture.temporary_.path().toStdString(), "color", 12, 6);
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());
    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));

    // Authored metadata is not part of the probe identity.
    auto metadata = operation(QStringLiteral("metadata"));
    metadata.insert(QStringLiteral("ids"), QVariantList{mediaId});
    metadata.insert(QStringLiteral("changes"),
                    QVariantMap{{QStringLiteral("tags"), QStringList{QStringLiteral("hero")}},
                                {QStringLiteral("description"), QStringLiteral("take 1")},
                                {QStringLiteral("color"), QStringLiteral("#d9855d")}});
    ASSERT_TRUE(fixture.model_.apply(metadata));
    ASSERT_TRUE(fixture.model_.applyProbe(mediaId));

    const nemo::ColorPolicy alternate{.workingSpace = "linear",
                                      .viewerTransform = "sRGB/alt",
                                      .deliveryTransform = "sRGB/rec709"};
    ASSERT_TRUE(fixture.session_
                    .submit(nemo::setColorPolicyCommand(alternate), {.expectedRevision = fixture.session_.revision()})
                    .committed);
    // The stored result was produced under the previous viewing transform.
    EXPECT_FALSE(fixture.model_.applyProbe(mediaId));

    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));
    ASSERT_TRUE(fixture.model_.applyProbe(mediaId));
    const MediaCatalogEntry* entry = fixture.session_.document().mediaCatalog.entry(entryIdOf(mediaId));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->metadata.userName, "color");
    EXPECT_EQ(entry->metadata.description, "take 1");
    EXPECT_EQ(entry->metadata.label, "#d9855d");
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
}

TEST(MediaLibraryModel, RuntimeProbeOverlaysDisplayedRecordWithoutAuthoredMutation) {
    MediaLibraryFixture fixture;
    const auto source = writeExrFixture(fixture.temporary_.path().toStdString(), "overlay", 10, 8);
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());

    // Before the runtime result is collected the record is honestly unknown.
    const QVariantMap initial = fixture.model_.item(mediaId).toMap();
    EXPECT_EQ(initial.value(QStringLiteral("kind")).toString(), QStringLiteral("unknown"));
    EXPECT_EQ(initial.value(QStringLiteral("authoredKind")).toString(), QStringLiteral("unknown"));
    EXPECT_TRUE(initial.value(QStringLiteral("probe")).isNull());
    EXPECT_TRUE(initial.value(QStringLiteral("committedProbe")).isNull());

    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));

    // The live validated result is displayed without an explicit apply.
    const QVariantMap displayed = fixture.model_.item(mediaId).toMap();
    EXPECT_EQ(displayed.value(QStringLiteral("kind")).toString(), QStringLiteral("still"));
    EXPECT_EQ(displayed.value(QStringLiteral("authoredKind")).toString(), QStringLiteral("unknown"));
    EXPECT_NE(displayed.value(QStringLiteral("mediaKind")).toString(), QStringLiteral("unknown"));
    EXPECT_FALSE(displayed.value(QStringLiteral("probe")).isNull());
    EXPECT_EQ(displayed.value(QStringLiteral("probe")).toMap().value(QStringLiteral("width")).toInt(), 10);
    EXPECT_EQ(displayed.value(QStringLiteral("probeStatus")).toString(), QStringLiteral("ready"));
    EXPECT_TRUE(displayed.value(QStringLiteral("committedProbe")).isNull());

    // Authored metadata is untouched, and the kind filter sees the validated
    // runtime kind through the core runtime-fact overlay.
    const nemo::MediaCatalogEntry* entry = fixture.session_.document().mediaCatalog.entry(entryIdOf(mediaId));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->metadata.kind, nemo::MediaKind::Unknown);
    EXPECT_FALSE(entry->metadata.committedProbe.has_value());
    QVariantMap stillFilter;
    stillFilter.insert(QStringLiteral("kind"), QStringLiteral("still"));
    EXPECT_TRUE(
        recordIds(fixture.model_.query(QStringLiteral("project"), QString(), true, stillFilter)).contains(mediaId));
    QVariantMap videoFilter;
    videoFilter.insert(QStringLiteral("kind"), QStringLiteral("video"));
    EXPECT_FALSE(
        recordIds(fixture.model_.query(QStringLiteral("project"), QString(), true, videoFilter)).contains(mediaId));

    // Explicit apply remains the only persistent mutation.
    ASSERT_TRUE(fixture.model_.applyProbe(mediaId));
    EXPECT_TRUE(
        fixture.session_.document().mediaCatalog.entry(entryIdOf(mediaId))->metadata.committedProbe.has_value());
    EXPECT_EQ(fixture.session_.document().mediaCatalog.entry(entryIdOf(mediaId))->metadata.kind,
              nemo::MediaKind::Image);
}

TEST(MediaLibraryModel, RuntimeCompletionSurvivesReentrantCatalogMutationDuringNotification) {
    MediaLibraryFixture fixture;
    const auto source = writeExrFixture(fixture.temporary_.path().toStdString(), "reentrant", 12, 6);
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());

    // Authored metadata the reentrant mutation must not disturb.
    auto metadata = operation(QStringLiteral("metadata"));
    metadata.insert(QStringLiteral("ids"), QVariantList{mediaId});
    metadata.insert(QStringLiteral("changes"),
                    QVariantMap{{QStringLiteral("tags"), QStringList{QStringLiteral("keep")}},
                                {QStringLiteral("description"), QStringLiteral("reentrant take")},
                                {QStringLiteral("color"), QStringLiteral("#86aa72")}});
    ASSERT_TRUE(fixture.model_.apply(metadata));

    QSignalSpy available(&fixture.model_, &nemo::ui::MediaLibraryModel::probeResultAvailable);
    QSignalSpy rejected(&fixture.model_, &nemo::ui::MediaLibraryModel::probeRejected);

    // Direct reentrant slots submit a real catalog command: the thumbnail slot
    // fires in the historical dangling-catalog-pointer window, the revision
    // slot fires while the completion is being announced. The nested
    // revisionChanged notifications produced by the thumbnail slot's own
    // command are skipped: ProjectSession forbids a nested submit, and the
    // mutation is meant to come from the outer completion notification.
    bool thumbnailMutated = false;
    bool inThumbnailMutation = false;
    bool revisionMutated = false;
    const QMetaObject::Connection thumbnailConnection =
        QObject::connect(&fixture.model_, &nemo::ui::MediaLibraryModel::thumbnailChanged, &fixture.model_, [&] {
            if (thumbnailMutated) {
                return;
            }
            thumbnailMutated = true;
            inThumbnailMutation = true;
            auto createBin = operation(QStringLiteral("createBin"));
            createBin.insert(QStringLiteral("name"), QStringLiteral("DuringThumbnail"));
            EXPECT_TRUE(fixture.model_.apply(createBin));
            inThumbnailMutation = false;
        });
    const QMetaObject::Connection revisionConnection =
        QObject::connect(&fixture.model_, &nemo::ui::MediaLibraryModel::revisionChanged, &fixture.model_, [&] {
            if (revisionMutated || inThumbnailMutation) {
                return;
            }
            if (fixture.model_.item(mediaId).toMap().value(QStringLiteral("probe")).isNull()) {
                return;  // not the result-completion notification
            }
            revisionMutated = true;
            auto createBin = operation(QStringLiteral("createBin"));
            createBin.insert(QStringLiteral("name"), QStringLiteral("DuringRevision"));
            EXPECT_TRUE(fixture.model_.apply(createBin));
        });

    ASSERT_TRUE(waitFor([&] { return available.count() + rejected.count() >= 1; }));
    EXPECT_EQ(rejected.count(), 0);
    EXPECT_EQ(available.count(), 1);
    EXPECT_TRUE(revisionMutated);
    EXPECT_FALSE(binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("DuringRevision")).isEmpty());

    // The published result is intact and the authored metadata survived the
    // reentrant catalog mutation.
    const QVariantMap item = fixture.model_.item(mediaId).toMap();
    EXPECT_EQ(item.value(QStringLiteral("probe")).toMap().value(QStringLiteral("width")).toInt(), 12);
    EXPECT_EQ(item.value(QStringLiteral("description")).toString(), QStringLiteral("reentrant take"));
    EXPECT_EQ(item.value(QStringLiteral("color")).toString(), QStringLiteral("#86aa72"));
    EXPECT_EQ(item.value(QStringLiteral("tags")).toStringList(), QStringList{QStringLiteral("keep")});
    if (thumbnailMutated) {
        EXPECT_FALSE(binIdNamed(fixture.model_, QStringLiteral("root"), QStringLiteral("DuringThumbnail")).isEmpty());
    }
    QObject::disconnect(thumbnailConnection);
    QObject::disconnect(revisionConnection);
}

TEST(MediaLibraryModel, ReentrantProjectResetCannotLeakAnOldRuntimeOutcome) {
    MediaLibraryFixture fixture;
    const auto source = writeExrFixture(fixture.temporary_.path().toStdString(), "reset-race", 14, 8);
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());

    QSignalSpy available(&fixture.model_, &nemo::ui::MediaLibraryModel::probeResultAvailable);
    QSignalSpy rejected(&fixture.model_, &nemo::ui::MediaLibraryModel::probeRejected);

    // The slot replaces the whole project during the completion notification:
    // the old result must not be announced as the new project's success.
    bool replaced = false;
    const QMetaObject::Connection connection =
        QObject::connect(&fixture.model_, &nemo::ui::MediaLibraryModel::revisionChanged, &fixture.model_, [&] {
            if (replaced) {
                return;
            }
            if (fixture.model_.item(mediaId).toMap().value(QStringLiteral("probe")).isNull()) {
                return;  // not the result-completion notification
            }
            replaced = true;
            EXPECT_TRUE(fixture.session_.replaceDocument(nemo::Document{}).replaced);
        });

    ASSERT_TRUE(waitFor([&] { return replaced; }));
    QTest::qWait(120);  // let any incorrect follow-up notification land
    EXPECT_EQ(available.count(), 0);
    EXPECT_EQ(rejected.count(), 0);
    QObject::disconnect(connection);

    // No leaked state, and the new project probes only its own sources.
    EXPECT_FALSE(fixture.model_.item(mediaId).isValid());
    EXPECT_FALSE(fixture.model_.applyProbe(mediaId));
    ASSERT_TRUE(fixture.model_.importPaths({QString::fromStdString(source.string())}, QStringLiteral("root"), 0));
    const QString newId = firstMediaId(fixture.model_);
    ASSERT_FALSE(newId.isEmpty());
    EXPECT_FALSE(fixture.model_.applyProbe(newId));
    ASSERT_TRUE(waitFor([&] { return fixture.model_.probeState(newId).value(QStringLiteral("hasResult")).toBool(); }));
    ASSERT_TRUE(fixture.model_.applyProbe(newId));
    const MediaCatalogEntry* entry = fixture.session_.document().mediaCatalog.entry(entryIdOf(newId));
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->width, 14);
}

TEST(MediaLibraryModel, RuntimeOfflineDrivesTheOfflineFilterWithoutAuthoringMetadata) {
    MediaLibraryFixture fixture;
    // A path that does not exist: the runtime reports offline, the catalog
    // keeps the authored flag untouched.
    ASSERT_TRUE(
        fixture.model_.importPaths({QStringLiteral("/media/definitely-missing-shot.exr")}, QStringLiteral("root"), 0));
    const QString mediaId = firstMediaId(fixture.model_);
    ASSERT_FALSE(mediaId.isEmpty());
    ASSERT_TRUE(
        waitFor([&] { return fixture.model_.probeState(mediaId).value(QStringLiteral("hasResult")).toBool(); }));

    const QVariantMap state = fixture.model_.probeState(mediaId);
    EXPECT_TRUE(state.value(QStringLiteral("offline")).toBool());
    EXPECT_FALSE(state.value(QStringLiteral("error")).toString().isEmpty());

    const QVariantMap item = fixture.model_.item(mediaId).toMap();
    EXPECT_TRUE(item.value(QStringLiteral("offline")).toBool());
    EXPECT_FALSE(item.value(QStringLiteral("authoredOffline")).toBool());
    const MediaCatalogEntry* entry = fixture.session_.document().mediaCatalog.entry(entryIdOf(mediaId));
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->metadata.offline);
    EXPECT_FALSE(entry->metadata.committedProbe.has_value());

    QVariantMap offlineFilter;
    offlineFilter.insert(QStringLiteral("offline"), true);
    EXPECT_TRUE(
        recordIds(fixture.model_.query(QStringLiteral("project"), QString(), true, offlineFilter)).contains(mediaId));
    QVariantMap onlineFilter;
    onlineFilter.insert(QStringLiteral("offline"), false);
    EXPECT_FALSE(
        recordIds(fixture.model_.query(QStringLiteral("project"), QString(), true, onlineFilter)).contains(mediaId));
}
