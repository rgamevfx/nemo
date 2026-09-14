#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "nemo/core/commands/ReadSourceCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"

using namespace nemo;

namespace {

MediaProbeMetadata readyProbe(std::string provenance = "fixture-probe") {
    MediaProbeMetadata probe;
    probe.width = 1920;
    probe.height = 1080;
    probe.provenance = std::move(provenance);
    probe.status = MediaProbeStatus::Ready;
    return probe;
}

struct ReadFixture {
    Document document;
    CommandStack history{document};
    NetworkId network = document.rootNetworkId();
    NodeId node{kInvalidNode};

    ReadFixture() {
        auto created = std::make_shared<NodeId>();
        history.push(addNodeCommand(network, "source", "Read1", created));
        node = *created;
    }

    [[nodiscard]] const NodeInstance& instance() const { return *document.network(network).graph().node(node); }

    [[nodiscard]] std::string nodeKey() const {
        const auto found = instance().params.find("source");
        if (found == instance().params.end())
            return {};
        const auto* text = std::get_if<std::string>(&found->second);
        return text == nullptr ? std::string{} : *text;
    }

    NodeId addNode(std::string type, std::string name) {
        auto created = std::make_shared<NodeId>();
        history.push(addNodeCommand(network, std::move(type), std::move(name), created));
        return *created;
    }
};

ReadSourceTiming sequenceTiming() {
    ReadSourceTiming timing;
    timing.frameOffset = 1001;
    timing.frameStep = 1;
    timing.firstFrame = 1001;
    timing.lastFrame = 1100;
    return timing;
}

const MediaCatalogEntry* entryForKey(const Document& document, const std::string& key) {
    for (const auto& entry : document.mediaCatalog().entries())
        if (entry.sourceKey == key)
            return &entry;
    return nullptr;
}

}  // namespace

// Choosing a path is one command: it authors the reference, registers the
// Media Bin entry with the validated probe, and points the node at the
// reference. Undo removes all three together.
TEST(ReadSourceTest, RegisterCreatesReferenceEntryAndNodeBindingAsOneHistoryEntry) {
    ReadFixture fixture;
    const std::size_t baseline = fixture.history.depth();
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.network, fixture.node, "/media/shot.####.exr",
                                                   sequenceTiming(), readyProbe(), key));

    ASSERT_FALSE(key->empty());
    EXPECT_EQ(fixture.history.depth(), baseline + 1);
    EXPECT_EQ(fixture.nodeKey(), *key);
    const auto reference = fixture.document.sources.find(*key);
    ASSERT_NE(reference, fixture.document.sources.end());
    EXPECT_EQ(reference->second.path, "/media/shot.####.exr");
    EXPECT_EQ(reference->second.frameOffset, 1001);
    EXPECT_EQ(reference->second.firstFrame, std::optional<std::int64_t>{1001});
    EXPECT_EQ(reference->second.lastFrame, std::optional<std::int64_t>{1100});
    const MediaCatalogEntry* entry = entryForKey(fixture.document, *key);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->provenance, "fixture-probe");

    ASSERT_TRUE(fixture.history.undo());
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.document.sources.find(*key), fixture.document.sources.end());
    EXPECT_EQ(entryForKey(fixture.document, *key), nullptr);
    EXPECT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.nodeKey(), *key);
}

// Two Read nodes naming the same path and interpretation share one media
// reference and one Media Bin entry.
TEST(ReadSourceTest, SamePathAndInterpretationShareReferenceAndEntry) {
    ReadFixture fixture;
    const NodeId second = fixture.addNode("source", "Read2");
    auto firstKey = std::make_shared<std::string>();
    auto secondKey = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.network, fixture.node, "/media/plate.exr", sequenceTiming(),
                                                   readyProbe(), firstKey));
    fixture.history.push(
        registerReadSourceCommand(fixture.network, second, "/media/plate.exr", sequenceTiming(), {}, secondKey));

    EXPECT_EQ(*firstKey, *secondKey);
    EXPECT_EQ(fixture.document.sources.size(), 1u);
    int entries = 0;
    for (const auto& entry : fixture.document.mediaCatalog().entries())
        if (entry.sourceKey == *firstKey)
            ++entries;
    EXPECT_EQ(entries, 1);
    const auto other = fixture.document.network(fixture.network).graph().node(second);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(std::get<std::string>(other->params.at("source")), *firstKey);
}

// Normalized paths match (`/media/./plate.exr` == `/media/plate.exr`), while a
// different interpretation is a different media reference.
TEST(ReadSourceTest, NormalizationAndInterpretationGovernReuse) {
    ReadFixture fixture;
    const NodeId second = fixture.addNode("source", "Read2");
    const NodeId third = fixture.addNode("source", "Read3");
    auto firstKey = std::make_shared<std::string>();
    auto secondKey = std::make_shared<std::string>();
    auto thirdKey = std::make_shared<std::string>();
    fixture.history.push(
        registerReadSourceCommand(fixture.network, fixture.node, "/media/plate.exr", {}, readyProbe(), firstKey));
    fixture.history.push(registerReadSourceCommand(fixture.network, second, "/media/./plate.exr", {}, {}, secondKey));
    ReadSourceTiming interpreted;
    interpreted.interpretation = {{"transfer", "srgb"}};
    fixture.history.push(
        registerReadSourceCommand(fixture.network, third, "/media/plate.exr", interpreted, {}, thirdKey));

    EXPECT_EQ(*firstKey, *secondKey);
    EXPECT_NE(*firstKey, *thirdKey);
    EXPECT_EQ(fixture.document.sources.size(), 2u);
}

// Relinking preserves identity, authored timing and interpretation, so every
// node sharing the reference recovers, and the stale probe is replaced.
TEST(ReadSourceTest, RelinkUpdatesPathInPlaceAndReplacesProbe) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.network, fixture.node, "/media/shot.####.exr",
                                                   sequenceTiming(), readyProbe("before"), key));
    const SourceReference expected = fixture.document.sources.at(*key);
    fixture.history.push(relinkReadSourceCommand(*key, expected, "/moved/shot.####.exr", readyProbe("after")));

    const SourceReference& relinked = fixture.document.sources.at(*key);
    EXPECT_EQ(relinked.path, "/moved/shot.####.exr");
    EXPECT_EQ(relinked.frameOffset, 1001);
    EXPECT_EQ(relinked.firstFrame, std::optional<std::int64_t>{1001});
    EXPECT_EQ(relinked.lastFrame, std::optional<std::int64_t>{1100});
    EXPECT_EQ(relinked.interpretation, expected.interpretation);
    EXPECT_EQ(relinked.revision, expected.revision + 1);
    EXPECT_EQ(entryForKey(fixture.document, *key)->metadata.committedProbe->provenance, "after");
}

// A stale expected reference is rejected and the document is unchanged.
TEST(ReadSourceTest, StaleRelinkIsRejectedWithoutMutation) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(
        registerReadSourceCommand(fixture.network, fixture.node, "/media/shot.exr", {}, readyProbe(), key));
    const std::size_t baseline = fixture.history.depth();
    SourceReference stale = fixture.document.sources.at(*key);
    stale.revision += 5;
    const std::string before = fixture.document.sources.at(*key).path;
    EXPECT_THROW(fixture.history.push(relinkReadSourceCommand(*key, stale, "/elsewhere/shot.exr", {})), GraphException);
    EXPECT_EQ(fixture.document.sources.at(*key).path, before);
    EXPECT_EQ(fixture.history.depth(), baseline);
}

// Timing edits validate the range up front and are one undoable command.
TEST(ReadSourceTest, TimingEditValidatesRangeAndIsOneEntry) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(
        registerReadSourceCommand(fixture.network, fixture.node, "/media/shot.####.exr", {}, readyProbe(), key));
    ReadSourceTiming inverted;
    inverted.firstFrame = 10;
    inverted.lastFrame = 4;
    EXPECT_THROW(setReadSourceTimingCommand(*key, fixture.document.sources.at(*key), inverted), std::invalid_argument);
    ReadSourceTiming zeroStep;
    zeroStep.frameStep = 0;
    EXPECT_THROW(setReadSourceTimingCommand(*key, fixture.document.sources.at(*key), zeroStep), std::invalid_argument);

    const std::size_t baseline = fixture.history.depth();
    ReadSourceTiming timing;
    timing.frameOffset = 7;
    timing.firstFrame = 7;
    timing.lastFrame = 9;
    fixture.history.push(setReadSourceTimingCommand(*key, fixture.document.sources.at(*key), timing));
    EXPECT_EQ(fixture.history.depth(), baseline + 1);
    EXPECT_EQ(fixture.document.sources.at(*key).frameOffset, 7);
    EXPECT_EQ(fixture.document.sources.at(*key).lastFrame, std::optional<std::int64_t>{9});
}

// A non-Read node cannot be given a media reference.
TEST(ReadSourceTest, RegisterRejectsNonReadNode) {
    ReadFixture fixture;
    const NodeId merge = fixture.addNode("merge", "Merge1");
    const std::size_t baseline = fixture.history.depth();
    EXPECT_THROW(fixture.history.push(registerReadSourceCommand(fixture.network, merge, "/media/shot.exr", {}, {}, {})),
                 GraphException);
    EXPECT_EQ(fixture.history.depth(), baseline);
}

// The authored range is additive persisted state: written only when engaged,
// and restored verbatim.
TEST(ReadSourceTest, SequenceRangeSerializesOnlyWhenAuthored) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.network, fixture.node, "/media/shot.####.exr",
                                                   sequenceTiming(), readyProbe(), key));

    const nlohmann::json saved = saveDocument(fixture.document);
    const nlohmann::json& authored = saved.at("sources").at(*key);
    EXPECT_EQ(authored.at("firstFrame").get<std::int64_t>(), 1001);
    EXPECT_EQ(authored.at("lastFrame").get<std::int64_t>(), 1100);

    SourceReference plain;
    plain.path = "/media/still.exr";
    fixture.document.sources.emplace("still", plain);
    const nlohmann::json plainSaved = saveDocument(fixture.document);
    EXPECT_FALSE(plainSaved.at("sources").at("still").contains("firstFrame"));
    EXPECT_FALSE(plainSaved.at("sources").at("still").contains("lastFrame"));

    const LoadResult loaded = loadDocument(saved);
    const auto restored = loaded.document.sources.find(*key);
    ASSERT_NE(restored, loaded.document.sources.end());
    EXPECT_EQ(restored->second.firstFrame, std::optional<std::int64_t>{1001});
    EXPECT_EQ(restored->second.lastFrame, std::optional<std::int64_t>{1100});
    EXPECT_TRUE(loaded.warnings.empty());
}

// Deserializing an inverted range is a structural error, not a silent clamp.
TEST(ReadSourceTest, InvertedAuthoredRangeIsRejectedByTheCodec) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(
        registerReadSourceCommand(fixture.network, fixture.node, "/media/shot.####.exr", sequenceTiming(), {}, key));
    nlohmann::json saved = saveDocument(fixture.document);
    saved["sources"][*key]["firstFrame"] = 1100;
    saved["sources"][*key]["lastFrame"] = 1001;
    EXPECT_THROW(static_cast<void>(loadDocument(saved)), DeserializeError);
}
