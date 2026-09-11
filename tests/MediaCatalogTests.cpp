#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Document.hpp"
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

    fixture.history.push(commitMediaProbeCommand(entry, proposal));
    ASSERT_TRUE(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe.has_value());
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe->provenance, "fixture-probe");
    EXPECT_EQ(fixture.document.mediaCatalog.entry(entry)->metadata.committedProbe->width, 1920);
}
