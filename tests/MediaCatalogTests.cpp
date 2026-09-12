#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectSession.hpp"

using namespace nemo;

namespace {

struct CatalogFixture {
    Document document;
    CommandStack history{document};

    CatalogFixture() {
        document.sources.emplace("shot", SourceReference{.path = "/media/shot.mov", .revision = 3});
        document.sources.emplace("still", SourceReference{.path = "/media/still.exr", .revision = 7});
    }

    MediaSourceId import(std::string source, MediaBinId parent, MediaMetadata metadata = {}) {
        auto created = std::make_shared<MediaSourceId>();
        history.push(importMediaReferenceCommand(std::move(source), parent, std::move(metadata), created));
        return *created;
    }

    MediaBinId bin(std::string name, MediaBinId parent = kInvalidMediaBin) {
        auto created = std::make_shared<MediaBinId>();
        history.push(createBinCommand(std::move(name), parent, created));
        return *created;
    }
};

MediaMetadata metadata(std::string name, MediaKind kind, bool offline = false) {
    MediaMetadata value;
    value.userName = std::move(name);
    value.kind = kind;
    value.offline = offline;
    return value;
}

MediaProbeMetadata readyProbe(std::string provenance = "fixture-probe") {
    MediaProbeMetadata probe;
    probe.width = 1920;
    probe.height = 1080;
    probe.duration = 240;
    probe.codec = "h264";
    probe.provenance = std::move(provenance);
    probe.status = MediaProbeStatus::Ready;
    return probe;
}

void addSourceNode(Document& document, std::string key) {
    auto& graph = document.network(document.rootNetworkId()).graph();
    const NodeId source = graph.addNode("source", "source-node");
    graph.setParam(source, "source", ParameterValue{std::move(key)});
}

}  // namespace

TEST(MediaCatalogTest, DuplicateEntriesHaveIndependentIdentityAndMetadataButShareSourceReference) {
    CatalogFixture fixture;
    const auto first = fixture.import("shot", kInvalidMediaBin, metadata("Original", MediaKind::Video));
    const auto second = fixture.import("shot", kInvalidMediaBin, metadata("Copy", MediaKind::Video));
    ASSERT_NE(first, second);
    ASSERT_EQ(fixture.document.mediaCatalog.entry(first)->sourceKey, "shot");
    ASSERT_EQ(fixture.document.mediaCatalog.entry(second)->sourceKey, "shot");
    const auto destination = fixture.bin("Destination");

    fixture.history.push(renameMediaCommand(second, "Renamed copy"));
    auto tagged = metadata("Renamed copy", MediaKind::Video);
    tagged.tags = {"select", "hero", "select"};
    fixture.history.push(setMediaMetadataCommand(second, tagged));
    fixture.history.push(moveMediaCommand(second, destination));
    fixture.history.push(setMediaMarksCommand(second, {{10, 25}}));

    EXPECT_EQ(fixture.document.mediaCatalog.entry(first)->metadata.userName, "Original");
    EXPECT_TRUE(fixture.document.mediaCatalog.entry(first)->metadata.tags.empty());
    EXPECT_TRUE(fixture.document.mediaCatalog.entry(first)->marks.empty());
    EXPECT_EQ(fixture.document.sources.at("shot").revision, 3u);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(first)->parent, kInvalidMediaBin);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(second)->parent, destination);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(second)->metadata.tags, (std::vector<std::string>{"hero", "select"}));
}

TEST(MediaCatalogTest, DuplicateCommandAllocatesNewIdentityAndRetainsSourceKey) {
    CatalogFixture fixture;
    const auto original = fixture.import("shot", kInvalidMediaBin, metadata("Original", MediaKind::Video));
    const auto destination = fixture.bin("Copies");
    auto duplicate = std::make_shared<MediaSourceId>();
    fixture.history.push(duplicateCatalogEntryCommand(original, destination, duplicate));

    ASSERT_NE(*duplicate, original);
    ASSERT_EQ(fixture.document.mediaCatalog.entry(*duplicate)->sourceKey, "shot");
    EXPECT_EQ(fixture.document.mediaCatalog.entry(*duplicate)->metadata,
              fixture.document.mediaCatalog.entry(original)->metadata);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(*duplicate)->parent, destination);
}

TEST(MediaCatalogTest, InvalidMarksAreRejectedWithoutMutationOrHistory) {
    CatalogFixture fixture;
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    const auto beforeHash = fixture.document.mediaCatalog.stateHash();
    const auto beforeDepth = fixture.history.depth();
    try {
        fixture.history.push(setMediaMarksCommand(entry, {{20, 10}}));
        FAIL() << "inverted mark unexpectedly succeeded";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::InvalidMediaMark);
    }
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), beforeHash);
    EXPECT_TRUE(fixture.document.mediaCatalog.entry(entry)->marks.empty());
    EXPECT_EQ(fixture.history.depth(), beforeDepth);
}

TEST(MediaCatalogTest, BinRenamePreservesStableIdentityAcrossUndoAndRedo) {
    CatalogFixture fixture;
    const auto bin = fixture.bin("Original");
    fixture.history.push(renameBinCommand(bin, "Renamed"));
    ASSERT_NE(fixture.document.mediaCatalog.bin(bin), nullptr);
    EXPECT_EQ(fixture.document.mediaCatalog.bin(bin)->name, "Renamed");
    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(fixture.document.mediaCatalog.bin(bin)->name, "Original");
    ASSERT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.document.mediaCatalog.bin(bin)->name, "Renamed");
}

TEST(MediaCatalogTest, NestedTraversalSearchAndMetadataFiltersAreDeterministic) {
    CatalogFixture fixture;
    const auto z = fixture.bin("Z bin");
    const auto a = fixture.bin("A bin");
    const auto nested = fixture.bin("Nested", a);
    const auto zEntry = fixture.import("shot", z, metadata("Zed", MediaKind::Video));
    const auto nestedEntry = fixture.import("still", nested, metadata("Alpha hero", MediaKind::Image));
    const auto offline = fixture.import("shot", a, metadata("Offline", MediaKind::Video, true));
    addSourceNode(fixture.document, "shot");

    EXPECT_EQ(fixture.document.mediaCatalog.childBins(), (std::vector<MediaBinId>{a, z}));
    EXPECT_EQ(fixture.document.mediaCatalog.depthFirstBins(), (std::vector<MediaBinId>{a, nested, z}));

    EXPECT_EQ(fixture.document.mediaCatalog.depthFirstEntries(),
              (std::vector<MediaSourceId>{offline, nestedEntry, zEntry}));
    EXPECT_EQ(fixture.document.mediaCatalog.path(nested), "/A bin/Nested");
    EXPECT_EQ(fixture.document.mediaCatalog.search(fixture.document, "hero"),
              (std::vector<MediaSourceId>{nestedEntry}));
    EXPECT_EQ(fixture.document.mediaCatalog.search(fixture.document, {}, MediaKind::Image),
              (std::vector<MediaSourceId>{nestedEntry}));
    EXPECT_EQ(fixture.document.mediaCatalog.search(fixture.document, {}, {}, true),
              (std::vector<MediaSourceId>{offline}));
    EXPECT_EQ(fixture.document.mediaCatalog.search(fixture.document, {}, {}, {}, true),
              (std::vector<MediaSourceId>{nestedEntry}));
}
TEST(MediaCatalogTest, ProjectSessionForwardsCatalogSearchAndBinQueries) {
    CatalogFixture fixture;
    const auto root = fixture.import("shot", kInvalidMediaBin, metadata("Hero shot", MediaKind::Video));
    const auto bin = fixture.bin("Shots");
    const auto nested = fixture.import("still", bin, metadata("Plate", MediaKind::Image));
    ProjectSession session(fixture.document);

    const auto matches = session.queryMedia("hero", MediaKind::Video);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().id, root);
    EXPECT_EQ(matches.front().sourceKey, "shot");
    const auto bins = session.queryMediaBins();
    ASSERT_EQ(bins.size(), 1u);
    EXPECT_EQ(bins.front().id, bin);
    EXPECT_EQ(session.queryMedia({}, MediaKind::Image).front().id, nested);
}

TEST(MediaCatalogTest, ProjectSessionReportsCatalogIdentityChanges) {
    CatalogFixture fixture;
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    ProjectSession session(fixture.document);
    const auto before = session.revision();
    const auto result = session.submit(renameMediaCommand(entry, "Renamed"),
                                       EditOptions{.expectedRevision = before, .requestId = "rename-media"});
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(result.changedMediaEntryIds, (std::vector<MediaSourceId>{entry}));
    const auto history = session.changesSince(before);
    ASSERT_EQ(history.events.size(), 1u);
    EXPECT_EQ(history.events.front().changedMediaEntryIds, (std::vector<MediaSourceId>{entry}));
}

TEST(MediaCatalogTest, InvalidMovesAndDuplicateNamesAreAtomicAndDoNotConsumeHistory) {
    CatalogFixture fixture;
    const auto parent = fixture.bin("Parent");
    const auto child = fixture.bin("Child", parent);
    const auto entry = fixture.import("shot", child, metadata("Clip", MediaKind::Video));
    const auto beforeHash = fixture.document.mediaCatalog.stateHash();
    const auto beforeDepth = fixture.history.depth();
    try {
        fixture.history.push(moveBinCommand(parent, child));
        FAIL() << "ancestor move unexpectedly succeeded";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::MediaCycle);
    }
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), beforeHash);
    EXPECT_EQ(fixture.history.depth(), beforeDepth);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->parent, child);

    try {
        fixture.history.push(createBinCommand("Child", parent));
        FAIL() << "duplicate bin name unexpectedly succeeded";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::MediaDuplicateName);
    }
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), beforeHash);
    EXPECT_EQ(fixture.history.depth(), beforeDepth);
}
TEST(MediaCatalogTest, UsedSourceCannotBeRemovedAndFailureLeavesGraphAndCatalogUntouched) {
    CatalogFixture fixture;
    addSourceNode(fixture.document, "shot");
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Used", MediaKind::Video));
    const auto beforeHash = fixture.document.mediaCatalog.stateHash();
    const auto beforeGraphRevision = fixture.document.network(fixture.document.rootNetworkId()).revision();
    const auto beforeDepth = fixture.history.depth();
    EXPECT_TRUE(fixture.document.mediaCatalog.sourceUsed(fixture.document, "shot"));
    try {
        fixture.history.push(removeMediaEntryCommand(entry));
        FAIL() << "used source removal unexpectedly succeeded";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::MediaSourceInUse);
    }
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), beforeHash);
    EXPECT_EQ(fixture.document.network(fixture.document.rootNetworkId()).revision(), beforeGraphRevision);
    EXPECT_EQ(fixture.history.depth(), beforeDepth);
}

TEST(MediaCatalogTest, RemovingBinWithKeepContentsReparentsEntriesAndUndoRestoresHierarchy) {
    CatalogFixture fixture;
    const auto outer = fixture.bin("Outer");
    const auto inner = fixture.bin("Inner", outer);
    const auto entry = fixture.import("shot", inner, metadata("Clip", MediaKind::Video));

    fixture.history.push(removeBinCommand(outer, true));
    ASSERT_EQ(fixture.document.mediaCatalog.bin(inner)->parent, kInvalidMediaBin);
    ASSERT_EQ(fixture.document.mediaCatalog.entry(entry)->parent, inner);
    EXPECT_EQ(fixture.history.depth(), 4u);
    ASSERT_TRUE(fixture.history.undo());
    ASSERT_EQ(fixture.document.mediaCatalog.bin(outer)->parent, kInvalidMediaBin);
    ASSERT_EQ(fixture.document.mediaCatalog.bin(inner)->parent, outer);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->parent, inner);
    ASSERT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.document.mediaCatalog.bin(outer), nullptr);
    EXPECT_EQ(fixture.document.mediaCatalog.bin(inner)->parent, kInvalidMediaBin);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->parent, inner);
}

TEST(MediaCatalogTest, CatalogCommandsHaveExactUndoRedoAndClearRedoBranch) {
    CatalogFixture fixture;
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    const auto beforeHash = fixture.document.mediaCatalog.stateHash();
    fixture.history.push(renameMediaCommand(entry, "Renamed"));
    const auto renamedHash = fixture.document.mediaCatalog.stateHash();
    EXPECT_EQ(fixture.history.depth(), 2u);
    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), beforeHash);
    ASSERT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), renamedHash);
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->metadata.userName, "Renamed");
    ASSERT_TRUE(fixture.history.undo());
    fixture.history.push(setMediaMarksCommand(entry, {{3, 8}}));
    EXPECT_FALSE(fixture.history.canRedo());
    EXPECT_EQ(fixture.history.depth(), 2u);
}

TEST(MediaCatalogTest, SmartMembershipUpdatesWithMetadataAndUndo) {
    CatalogFixture fixture;
    const auto smart = fixture.bin("Video", kInvalidMediaBin);
    fixture.history.push(setMediaQueryCommand(smart, MediaQueryDescriptor{.text = "hero", .kind = MediaKind::Video}));
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    EXPECT_TRUE(fixture.document.mediaCatalog.smartMembers(fixture.document, smart).empty());

    auto changed = metadata("hero clip", MediaKind::Video);
    fixture.history.push(setMediaMetadataCommand(entry, changed));
    EXPECT_EQ(fixture.document.mediaCatalog.smartMembers(fixture.document, smart), (std::vector<MediaSourceId>{entry}));
    ASSERT_TRUE(fixture.history.undo());
    EXPECT_TRUE(fixture.document.mediaCatalog.smartMembers(fixture.document, smart).empty());
}

TEST(MediaCatalogTest, SetMediaBinMetadataCommandNormalizesTagsAndUndoesAtomically) {
    CatalogFixture fixture;
    const auto bin = fixture.bin("Shots");
    ASSERT_FALSE(fixture.document.mediaCatalog.bin(bin)->query.has_value());
    const auto before = fixture.document.mediaCatalog.stateHash();

    MediaBinMetadata metadata;
    metadata.description = "archived selects";
    metadata.tags = {"hero", "b-roll", "hero"};
    metadata.label = "#3366ff";
    fixture.history.push(setMediaBinMetadataCommand(bin, metadata));

    const auto* stored = fixture.document.mediaCatalog.bin(bin);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->metadata.tags, (std::vector<std::string>{"b-roll", "hero"}));
    EXPECT_EQ(stored->metadata.description, "archived selects");
    EXPECT_EQ(stored->metadata.label, "#3366ff");
    // A metadata edit never turns a container bin into a smart bin.
    EXPECT_FALSE(stored->query.has_value());
    EXPECT_NE(fixture.document.mediaCatalog.stateHash(), before);

    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(fixture.document.mediaCatalog.bin(bin)->metadata, MediaBinMetadata{});
    EXPECT_EQ(fixture.document.mediaCatalog.stateHash(), before);
    ASSERT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.document.mediaCatalog.bin(bin)->metadata.tags, (std::vector<std::string>{"b-roll", "hero"}));
    EXPECT_EQ(fixture.document.mediaCatalog.bin(bin)->metadata.label, "#3366ff");
}

TEST(MediaCatalogTest, EmptyQueryDescriptorIsSavedAllMediaQueryAndNulloptResetsPlainBin) {
    CatalogFixture fixture;
    const auto smart = fixture.bin("All media");
    const auto shot = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    const auto still = fixture.import("still", kInvalidMediaBin, metadata("Plate", MediaKind::Image));

    // An engaged all-default descriptor is a saved all-media query.
    fixture.history.push(setMediaQueryCommand(smart, MediaQueryDescriptor{}));
    ASSERT_TRUE(fixture.document.mediaCatalog.bin(smart)->query.has_value());
    auto members = fixture.document.mediaCatalog.smartMembers(fixture.document, smart);
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, (std::vector<MediaSourceId>{shot, still}));

    // Membership is live: newly imported media joins without editing the query.
    const auto added = fixture.import("shot", kInvalidMediaBin, metadata("Later", MediaKind::Video));
    members = fixture.document.mediaCatalog.smartMembers(fixture.document, smart);
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, (std::vector<MediaSourceId>{shot, still, added}));

    // Explicit nullopt alone resets the bin to an ordinary container.
    fixture.history.push(setMediaQueryCommand(smart, std::nullopt));
    EXPECT_FALSE(fixture.document.mediaCatalog.bin(smart)->query.has_value());
    EXPECT_TRUE(fixture.document.mediaCatalog.smartMembers(fixture.document, smart).empty());

    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(fixture.document.mediaCatalog.smartMembers(fixture.document, smart).size(), 3u);
}

TEST(MediaCatalogTest, BinMetadataAndAllMediaQueryRoundTripThroughSavedDocument) {
    Document document;
    auto& catalog = document.mediaCatalog;
    const auto bin = catalog.addBin("All media", kInvalidMediaBin, MediaQueryDescriptor{});
    MediaBinMetadata binMetadata;
    binMetadata.description = "everything";
    binMetadata.tags = {"hero", "hero"};
    binMetadata.label = "#3366ff";
    binMetadata.extension = {{"futureBinFlag", true}};
    catalog.setBinMetadata(bin, binMetadata);
    MediaMetadata entryMetadata = metadata("Clip", MediaKind::Video);
    const auto entry = catalog.addEntry("shot", bin, entryMetadata);

    const auto encoded = saveDocument(document);
    const auto loaded = loadDocument(encoded);
    const MediaBin* loadedBin = loaded.document.mediaCatalog.bin(bin);
    ASSERT_NE(loadedBin, nullptr);
    EXPECT_TRUE(loadedBin->query.has_value());
    EXPECT_EQ(loadedBin->metadata.tags, (std::vector<std::string>{"hero"}));
    EXPECT_EQ(loadedBin->metadata.description, "everything");
    EXPECT_EQ(loadedBin->metadata.label, "#3366ff");
    ASSERT_TRUE(loadedBin->metadata.extension.is_object());
    EXPECT_EQ(loadedBin->metadata.extension.at("futureBinFlag"), true);
    // The saved all-media query still resolves against the loaded document.
    EXPECT_EQ(loaded.document.mediaCatalog.smartMembers(loaded.document, bin), (std::vector<MediaSourceId>{entry}));
    EXPECT_EQ(saveDocument(loaded.document), encoded);

    // A file authored before bin metadata existed loads defaults and regains them
    // on save without touching the smart-bin marker.
    auto legacy = encoded;
    legacy["mediaCatalog"]["bins"][0].erase("metadata");
    const auto migrated = loadDocument(legacy).document;
    ASSERT_NE(migrated.mediaCatalog.bin(bin), nullptr);
    EXPECT_EQ(migrated.mediaCatalog.bin(bin)->metadata, MediaBinMetadata{});
    EXPECT_TRUE(migrated.mediaCatalog.bin(bin)->query.has_value());
    EXPECT_TRUE(saveDocument(migrated)["mediaCatalog"]["bins"][0].contains("metadata"));
}

TEST(MediaCatalogTest, SmartQueryTypedScopeRecursionAndKindFamilyDriveMembership) {
    Document document;
    auto& catalog = document.mediaCatalog;
    const auto outer = catalog.addBin("Outer");
    const auto nested = catalog.addBin("Nested", outer);
    const auto topLevel = catalog.addEntry("shot", kInvalidMediaBin, metadata("Top", MediaKind::Video));
    const auto inOuter = catalog.addEntry("shot", outer, metadata("Outer clip", MediaKind::Video));
    const auto inNested = catalog.addEntry("still", nested, metadata("Nested plate", MediaKind::Image));
    const auto sequence = catalog.addEntry("sequence", outer, metadata("Sequence", MediaKind::Sequence));

    // Engaged scope + recursive=false: only the scope bin's direct entries.
    const auto direct =
        catalog.addBin("Direct", kInvalidMediaBin, MediaQueryDescriptor{.scope = outer, .recursive = false});
    auto members = catalog.smartMembers(document, direct);
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, (std::vector<MediaSourceId>{inOuter, sequence}));

    // Engaged scope + default recursive=true: the whole scope subtree.
    const auto subtree = catalog.addBin("Subtree", kInvalidMediaBin, MediaQueryDescriptor{.scope = outer});
    members = catalog.smartMembers(document, subtree);
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, (std::vector<MediaSourceId>{inOuter, inNested, sequence}));

    // Absent scope: whole project regardless of depth.
    const auto project = catalog.addBin("Project", kInvalidMediaBin, MediaQueryDescriptor{});
    members = catalog.smartMembers(document, project);
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, (std::vector<MediaSourceId>{topLevel, inOuter, inNested, sequence}));

    // Engaged root id (0) with recursive=false: direct children of the root.
    const auto rootDirect = catalog.addBin("Root direct", kInvalidMediaBin,
                                           MediaQueryDescriptor{.scope = kInvalidMediaBin, .recursive = false});
    members = catalog.smartMembers(document, rootDirect);
    EXPECT_EQ(members, (std::vector<MediaSourceId>{topLevel}));

    // Kind family: Image alone excludes sequences; the flag admits them.
    const auto stills = catalog.addBin("Stills", kInvalidMediaBin, MediaQueryDescriptor{.kind = MediaKind::Image});
    members = catalog.smartMembers(document, stills);
    EXPECT_EQ(members, (std::vector<MediaSourceId>{inNested}));
    const auto stillsAndSequences =
        catalog.addBin("Stills and sequences", kInvalidMediaBin,
                       MediaQueryDescriptor{.kind = MediaKind::Image, .includeImageSequences = true});
    members = catalog.smartMembers(document, stillsAndSequences);
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, (std::vector<MediaSourceId>{inNested, sequence}));

    // A bins-only query contributes no media membership.
    const auto binsOnly = catalog.addBin("Bins only", kInvalidMediaBin, MediaQueryDescriptor{.binsOnly = true});
    EXPECT_TRUE(catalog.smartMembers(document, binsOnly).empty());

    // The typed fields persist, and loaded queries keep their membership.
    const auto encoded = saveDocument(document);
    const auto loaded = loadDocument(encoded).document;
    EXPECT_EQ(loaded.mediaCatalog.bins(), catalog.bins());
    EXPECT_EQ(saveDocument(loaded), encoded);
    auto loadedDirect = loaded.mediaCatalog.smartMembers(loaded, direct);
    std::sort(loadedDirect.begin(), loadedDirect.end());
    EXPECT_EQ(loadedDirect, (std::vector<MediaSourceId>{inOuter, sequence}));
    auto loadedStills = loaded.mediaCatalog.smartMembers(loaded, stillsAndSequences);
    std::sort(loadedStills.begin(), loadedStills.end());
    EXPECT_EQ(loadedStills, (std::vector<MediaSourceId>{inNested, sequence}));
}

TEST(MediaCatalogTest, DeletedQueryScopeIsUnavailableAndNeverFallsBackToProject) {
    Document document;
    auto& catalog = document.mediaCatalog;
    const auto scope = catalog.addBin("Scope");
    const auto rootClip = catalog.addEntry("shot", kInvalidMediaBin, metadata("Root clip", MediaKind::Video));
    const auto inside = catalog.addEntry("still", scope, metadata("Inside", MediaKind::Image));
    const auto smart = catalog.addBin("Scoped", kInvalidMediaBin, MediaQueryDescriptor{.scope = scope});
    EXPECT_EQ(catalog.smartMembers(document, smart), (std::vector<MediaSourceId>{inside}));

    // Authoring a query against a missing bin is rejected without consuming history.
    CommandStack history{document};
    auto created = std::make_shared<MediaBinId>();
    history.push(createBinCommand("Fresh", created));
    const auto depth = history.depth();
    MediaQueryDescriptor bogus;
    bogus.scope = static_cast<MediaBinId>(999);
    try {
        history.push(setMediaQueryCommand(*created, bogus));
        FAIL() << "a query scoped to a missing bin unexpectedly succeeded";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::UnknownMediaBin);
    }
    EXPECT_EQ(history.depth(), depth);
    EXPECT_FALSE(catalog.bin(*created)->query.has_value());

    // Removing the scope bin leaves the query unavailable, not project-wide.
    catalog.removeBin(scope, true);
    EXPECT_FALSE(catalog.queryScopeAvailable(*catalog.bin(smart)->query));
    const auto members = catalog.smartMembers(document, smart);
    EXPECT_TRUE(members.empty());
    EXPECT_EQ(std::find(members.begin(), members.end(), rootClip), members.end());
}

TEST(MediaCatalogTest, RuntimeSourceFactsOverlayKindAndOfflineWithoutMutatingDocument) {
    Document document;
    auto& catalog = document.mediaCatalog;
    const auto entry = catalog.addEntry("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Unknown));
    const auto kindBin = catalog.addBin("Video", kInvalidMediaBin, MediaQueryDescriptor{.kind = MediaKind::Video});
    const auto offlineBin = catalog.addBin("Offline", kInvalidMediaBin, MediaQueryDescriptor{.offline = true});

    // Without runtime facts the authored classification stands.
    EXPECT_TRUE(catalog.smartMembers(document, kindBin).empty());
    EXPECT_TRUE(catalog.smartMembers(document, offlineBin).empty());

    const MediaQuerySourceState runtime[] = {{"shot", MediaKind::Video, true}};
    EXPECT_EQ(catalog.smartMembers(document, kindBin, runtime), (std::vector<MediaSourceId>{entry}));
    EXPECT_EQ(catalog.smartMembers(document, offlineBin, runtime), (std::vector<MediaSourceId>{entry}));

    // The overlay is read-only: authored metadata and catalog state are untouched.
    EXPECT_EQ(catalog.entry(entry)->metadata.kind, MediaKind::Unknown);
    EXPECT_FALSE(catalog.entry(entry)->metadata.offline);
    const auto authoredHash = catalog.stateHash();

    // A fact for another source leaves the entry to its authored values.
    const MediaQuerySourceState other[] = {{"other", MediaKind::Video, true}};
    EXPECT_TRUE(catalog.smartMembers(document, kindBin, other).empty());
    EXPECT_TRUE(catalog.smartMembers(document, offlineBin, other).empty());
    EXPECT_EQ(catalog.stateHash(), authoredHash);
}

TEST(MediaCatalogTest, CatalogOnlyEditsLeaveSourceReferencesAndGraphOccurrencesUntouched) {
    CatalogFixture fixture;
    addSourceNode(fixture.document, "shot");
    const auto sourceBefore = fixture.document.sources;
    const auto& graph = fixture.document.network(fixture.document.rootNetworkId()).graph();
    const auto graphRevisionBefore = graph.revision();
    const auto graphNodeCountBefore = graph.nodes().size();
    const auto sourceParamsBefore = graph.nodes().front().params;
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    fixture.history.push(renameMediaCommand(entry, "Edited"));
    fixture.history.push(setMediaMarksCommand(entry, {{4, 12}}));
    fixture.history.push(duplicateCatalogEntryCommand(entry, kInvalidMediaBin));
    const auto& graphAfter = fixture.document.network(fixture.document.rootNetworkId()).graph();
    EXPECT_EQ(fixture.document.sources, sourceBefore);
    EXPECT_EQ(graphAfter.revision(), graphRevisionBefore);
    EXPECT_EQ(graphAfter.nodes().size(), graphNodeCountBefore);
    EXPECT_EQ(graphAfter.nodes().front().params, sourceParamsBefore);
}

TEST(MediaCatalogTest, ProbeProposalIsNotCommittedUntilExplicitProvenanceBearingCommit) {
    CatalogFixture fixture;
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    MediaProbeProposal proposal;
    proposal.width = 1920;
    proposal.height = 1080;
    proposal.duration = 240;
    proposal.codec = "h264";
    proposal.provenance = "fixture-probe";
    proposal.status = MediaProbeStatus::Ready;
    EXPECT_FALSE(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe.has_value());

    fixture.history.push(commitMediaProbeCommand(entry, fixture.document.sources.at("shot"), proposal));
    ASSERT_TRUE(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe.has_value());
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe->provenance, "fixture-probe");
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe->width, 1920);
}

TEST(MediaCatalogTest, StaleProbeApplyAfterRelinkRejectsWithoutMutationOrHistory) {
    CatalogFixture fixture;
    const auto entry = fixture.import("shot", kInvalidMediaBin, metadata("Clip", MediaKind::Video));
    const SourceReference expected = fixture.document.sources.at("shot");
    fixture.history.push(relinkMediaSourceCommand(entry, expected, "/media/relinked/shot.mov"));
    const auto depthAfterRelink = fixture.history.depth();

    try {
        fixture.history.push(commitMediaProbeCommand(entry, expected, readyProbe()));
        FAIL() << "probe from before the relink unexpectedly applied";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::StaleMediaSource);
    }
    EXPECT_FALSE(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe.has_value());
    EXPECT_EQ(fixture.history.depth(), depthAfterRelink);
    EXPECT_EQ(fixture.document.sources.at("shot").path, "/media/relinked/shot.mov");
    EXPECT_EQ(fixture.document.sources.at("shot").revision, expected.revision + 1);

    fixture.history.push(commitMediaProbeCommand(entry, fixture.document.sources.at("shot"), readyProbe()));
    EXPECT_TRUE(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe.has_value());
}

TEST(MediaCatalogTest, RelinkPreservesSharedSourceIdentityAndClearsProbesWithUndo) {
    CatalogFixture fixture;
    auto firstMetadata = metadata("First", MediaKind::Video);
    firstMetadata.tags = {"hero"};
    const auto first = fixture.import("shot", kInvalidMediaBin, firstMetadata);
    const auto second = fixture.import("shot", kInvalidMediaBin, metadata("Second", MediaKind::Video));
    const SourceReference expected = fixture.document.sources.at("shot");
    fixture.history.push(commitMediaProbeCommand(first, expected, readyProbe("first")));
    fixture.history.push(commitMediaProbeCommand(second, expected, readyProbe("second")));
    const auto destination = fixture.bin("Destination");
    fixture.history.push(moveMediaCommand(second, destination));

    fixture.history.push(relinkMediaSourceCommand(first, expected, "/media/relinked/shot.mov"));

    const auto* firstEntry = fixture.document.mediaCatalog.entry(first);
    const auto* secondEntry = fixture.document.mediaCatalog.entry(second);
    ASSERT_NE(firstEntry, nullptr);
    ASSERT_NE(secondEntry, nullptr);
    EXPECT_EQ(firstEntry->id, first);
    EXPECT_EQ(firstEntry->sourceKey, "shot");
    EXPECT_EQ(secondEntry->sourceKey, "shot");
    EXPECT_EQ(firstEntry->metadata.userName, "First");
    EXPECT_EQ(firstEntry->metadata.tags, (std::vector<std::string>{"hero"}));
    EXPECT_EQ(firstEntry->parent, kInvalidMediaBin);
    EXPECT_EQ(secondEntry->parent, destination);
    EXPECT_FALSE(firstEntry->metadata.committedProbe.has_value());
    EXPECT_FALSE(secondEntry->metadata.committedProbe.has_value());
    EXPECT_EQ(fixture.document.sources.at("shot").path, "/media/relinked/shot.mov");
    EXPECT_EQ(fixture.document.sources.at("shot").revision, expected.revision + 1);
    EXPECT_EQ(fixture.document.sources.at("shot").frameOffset, expected.frameOffset);
    EXPECT_EQ(fixture.document.sources.at("shot").frameStep, expected.frameStep);
    EXPECT_EQ(fixture.document.sources.at("still").path, "/media/still.exr");
    EXPECT_EQ(fixture.document.sources.at("still").revision, 7u);

    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(fixture.document.sources.at("shot").path, "/media/shot.mov");
    EXPECT_EQ(fixture.document.sources.at("shot").revision, expected.revision);
    EXPECT_TRUE(fixture.document.mediaCatalog.entry(first)->metadata.committedProbe.has_value());
    EXPECT_TRUE(fixture.document.mediaCatalog.entry(second)->metadata.committedProbe.has_value());
    EXPECT_EQ(fixture.document.mediaCatalog.entry(first)->metadata.tags, (std::vector<std::string>{"hero"}));
    ASSERT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.document.sources.at("shot").path, "/media/relinked/shot.mov");
    EXPECT_FALSE(fixture.document.mediaCatalog.entry(first)->metadata.committedProbe.has_value());
}

TEST(MediaCatalogTest, ExpectedSourceGuardRejectsInterpretationAndMappingMismatchButAllowsMetadataEdits) {
    CatalogFixture fixture;
    auto tagged = metadata("Clip", MediaKind::Video);
    tagged.tags = {"hero", "select"};
    const auto entry = fixture.import("shot", kInvalidMediaBin, tagged);
    const SourceReference expected = fixture.document.sources.at("shot");

    // Unrelated catalog metadata edits do not invalidate the source snapshot.
    fixture.history.push(setMediaMetadataCommand(entry, tagged));

    auto reinterpreted = expected;
    reinterpreted.interpretation["transfer"] = "sRGB";
    fixture.history.push(setSourceCommand("shot", reinterpreted));
    const auto depthAfterReinterpret = fixture.history.depth();
    try {
        fixture.history.push(commitMediaProbeCommand(entry, expected, readyProbe()));
        FAIL() << "probe applied against a reinterpreted source";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::StaleMediaSource);
    }
    EXPECT_EQ(fixture.history.depth(), depthAfterReinterpret);
    EXPECT_FALSE(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe.has_value());
    ASSERT_TRUE(fixture.history.undo());

    auto remapped = expected;
    remapped.frameStep = 2;
    fixture.history.push(setSourceCommand("shot", remapped));
    try {
        fixture.history.push(commitMediaProbeCommand(entry, expected, readyProbe()));
        FAIL() << "probe applied against a remapped source";
    } catch (const GraphException& error) {
        EXPECT_EQ(error.errorCode(), GraphError::StaleMediaSource);
    }
    ASSERT_TRUE(fixture.history.undo());

    fixture.history.push(commitMediaProbeCommand(entry, expected, readyProbe()));
    const auto* committed = fixture.document.mediaCatalog.entry(entry);
    ASSERT_NE(committed, nullptr);
    ASSERT_TRUE(committed->metadata.committedProbe.has_value());
    EXPECT_EQ(committed->metadata.committedProbe->provenance, "fixture-probe");
    EXPECT_EQ(committed->metadata.tags, (std::vector<std::string>{"hero", "select"}));
}
