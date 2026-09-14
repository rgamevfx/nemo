// ReadSourceTests (issues #61/#75): the Read media commands and the effective
// source request they resolve to. These are command/API-level proofs: what a
// caller and a downstream consumer observe, never internal storage shape.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nemo/core/commands/ReadSourceCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"

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

// A discovered sequence 1001..1100 with 5 missing members, validated.
MediaProbeMetadata sequenceProbe() {
    MediaProbeMetadata probe = readyProbe("sequence-probe");
    probe.firstFrame = 1001;
    probe.lastFrame = 1100;
    probe.coverageQuality = CoverageQuality::Validated;
    probe.availableFrameCount = 95;
    probe.missingFrameCount = 5;
    probe.missingRanges.push_back(MediaFrameRange{1050, 1054});
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

// A new sequence selection: the caller aligns the discovered first frame to
// local zero (offset = first frame) and keeps the mapping at step 1.
ReadNodeOverrides sequenceOverrides() {
    ReadNodeOverrides overrides;
    overrides.frameOffset = 1001;
    overrides.frameStep = 1;
    return overrides;
}

// A Read's authored choices are ordinary node parameters on the node, so
// authoring them is the shared parameter batch: one gesture, one history entry,
// through the one core-owned translation.
std::vector<ParameterEdit> readValueEdits(NetworkId network, NodeId node, const ReadNodeOverrides& overrides) {
    std::vector<ParameterEdit> edits;
    for (auto& [key, value] : readOverrideParameters(overrides))
        edits.push_back(ParameterEdit{ParameterAddress{network, node, key}, std::move(value)});
    return edits;
}

const MediaCatalogEntry* entryForKey(const Document& document, const std::string& key) {
    for (const auto& entry : document.mediaCatalog().entries())
        if (entry.sourceKey == key)
            return &entry;
    return nullptr;
}

}  // namespace

// Choosing a path is one command: it authors the shared reference (media
// identity only), registers the Media Bin entry with the validated probe,
// writes the Read's own choices, and points the node at the reference. Undo
// removes all of it together.
TEST(ReadSourceTest, RegisterCreatesReferenceEntryAndNodeBindingAsOneHistoryEntry) {
    ReadFixture fixture;
    const std::size_t baseline = fixture.history.depth();
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, sequenceProbe(), key));

    ASSERT_FALSE(key->empty());
    EXPECT_EQ(fixture.history.depth(), baseline + 1);
    EXPECT_EQ(fixture.nodeKey(), *key);
    const auto reference = fixture.document.sources.find(*key);
    ASSERT_NE(reference, fixture.document.sources.end());
    EXPECT_EQ(reference->second.path, "/media/shot.####.exr");
    // The shared reference no longer carries a Read's timing: the Read owns it.
    EXPECT_EQ(reference->second.frameOffset, 0);
    EXPECT_EQ(reference->second.frameStep, 1);
    EXPECT_FALSE(reference->second.firstFrame.has_value());
    EXPECT_TRUE(reference->second.interpretation.empty());
    const ReadNodeOverrides authored = readAuthoredOverrides(fixture.document, fixture.instance());
    EXPECT_EQ(authored.frameOffset, 1001);
    EXPECT_EQ(authored.frameStep, 1);
    EXPECT_EQ(authored.rangeMode, "auto");

    const MediaCatalogEntry* entry = entryForKey(fixture.document, *key);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->provenance, "sequence-probe");
    EXPECT_EQ(entry->metadata.committedProbe->firstFrame, std::optional<std::int64_t>{1001});

    ASSERT_TRUE(fixture.history.undo());
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.document.sources.find(*key), fixture.document.sources.end());
    EXPECT_EQ(entryForKey(fixture.document, *key), nullptr);
    EXPECT_TRUE(fixture.history.redo());
    EXPECT_EQ(fixture.nodeKey(), *key);
}

// One media reference per normalized path, whatever interpretation each Read
// chooses: interpretation is a Read's own choice now, so two Reads of one file
// never duplicate the media identity or the Media Bin entry.
TEST(ReadSourceTest, SamePathSharesReferenceAndEntryRegardlessOfReadChoices) {
    ReadFixture fixture;
    const NodeId second = fixture.addNode("source", "Read2");
    auto firstKey = std::make_shared<std::string>();
    auto secondKey = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/plate.exr", sequenceOverrides(), nemo::MediaKind::Unknown,
                                                   readyProbe(), firstKey));
    ReadNodeOverrides other = sequenceOverrides();
    other.frameOffset = 0;
    other.sourceTransfer = "srgb";
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, second, "source"}, 0.0,
                                                   "/media/./plate.exr", other, nemo::MediaKind::Unknown, {},
                                                   secondKey));

    EXPECT_EQ(*firstKey, *secondKey);
    EXPECT_EQ(fixture.document.sources.size(), 1u);
    int entries = 0;
    for (const auto& entry : fixture.document.mediaCatalog().entries())
        if (entry.sourceKey == *firstKey)
            ++entries;
    EXPECT_EQ(entries, 1);
    const auto otherNode = fixture.document.network(fixture.network).graph().node(second);
    ASSERT_NE(otherNode, nullptr);
    EXPECT_EQ(std::get<std::string>(otherNode->params.at("source")), *firstKey);
    // The two Reads keep independent choices while sharing one media identity.
    EXPECT_EQ(readAuthoredOverrides(fixture.document, *otherNode).frameOffset, 0);
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()).frameOffset, 1001);
}

// Relinking preserves the shared reference's identity and every Read's authored
// choices, so every node sharing the reference recovers together, and the stale
// probe is replaced.
TEST(ReadSourceTest, RelinkUpdatesPathInPlaceAndReplacesProbe) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, readyProbe("before"), key));
    const ReadNodeOverrides before = readAuthoredOverrides(fixture.document, fixture.instance());
    const SourceReference expected = fixture.document.sources.at(*key);
    fixture.history.push(
        relinkReadSourceCommand(*key, expected, "/moved/shot.####.exr", nemo::MediaKind::Unknown, readyProbe("after")));

    const SourceReference& relinked = fixture.document.sources.at(*key);
    EXPECT_EQ(relinked.path, "/moved/shot.####.exr");
    EXPECT_EQ(relinked.revision, expected.revision + 1);
    EXPECT_TRUE(relinked.interpretation.empty());
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()), before);
    EXPECT_EQ(entryForKey(fixture.document, *key)->metadata.committedProbe->provenance, "after");
}

// A stale expected reference is rejected and the document is unchanged.
TEST(ReadSourceTest, StaleRelinkIsRejectedWithoutMutation) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.exr", {}, nemo::MediaKind::Unknown, readyProbe(), key));
    const std::size_t baseline = fixture.history.depth();
    SourceReference stale = fixture.document.sources.at(*key);
    stale.revision += 5;
    const std::string before = fixture.document.sources.at(*key).path;
    EXPECT_THROW(
        fixture.history.push(relinkReadSourceCommand(*key, stale, "/elsewhere/shot.exr", nemo::MediaKind::Unknown, {})),
        GraphException);
    EXPECT_EQ(fixture.document.sources.at(*key).path, before);
    EXPECT_EQ(fixture.history.depth(), baseline);
}

// Two Reads of one file keep independent timing: editing one leaves the other's
// effective mapping exactly as it was.
TEST(ReadSourceTest, TimingEditIsNodeScopedAndLeavesTheOtherReadUnchanged) {
    ReadFixture fixture;
    const NodeId second = fixture.addNode("source", "Read2");
    auto firstKey = std::make_shared<std::string>();
    auto secondKey = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, sequenceProbe(), firstKey));
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, second, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, sequenceProbe(), secondKey));
    ASSERT_EQ(*firstKey, *secondKey);

    const NodeInstance& first = fixture.instance();
    const NodeInstance& other = *fixture.document.network(fixture.network).graph().node(second);
    const EffectiveSourceRequest before = resolveSourceRequest(fixture.document, other, 7);
    EXPECT_EQ(before.mapping.origin, SourceBoundsOrigin::DiscoveredFacts);

    ReadNodeOverrides edited = readAuthoredOverrides(fixture.document, first);
    edited.frameOffset = 500;
    edited.frameStep = 2;
    edited.rangeMode = "custom";
    edited.rangeFirst = 500;
    edited.rangeLast = 600;
    const std::size_t baseline = fixture.history.depth();
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, edited)));
    EXPECT_EQ(fixture.history.depth(), baseline + 1);

    const EffectiveSourceRequest changed = resolveSourceRequest(fixture.document, fixture.instance(), 7);
    EXPECT_EQ(changed.sourceFrame, 514);  // 500 + 7*2, applied exactly once
    EXPECT_EQ(changed.mapping.origin, SourceBoundsOrigin::NodeCustom);
    EXPECT_EQ(resolveSourceRequest(fixture.document, other, 7).sourceFrame, before.sourceFrame);
    EXPECT_EQ(resolveSourceRequest(fixture.document, other, 7).mapping.origin, SourceBoundsOrigin::DiscoveredFacts);

    // One undo restores the whole set.
    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()), sequenceOverrides());
    EXPECT_EQ(resolveSourceRequest(fixture.document, fixture.instance(), 7).sourceFrame, 1008);
}

// Read value edits validate through the shared parameter seam. The catalog
// validator owns everything a single key can judge — a zero step, an unknown
// choice — so those edits never reach history. A cross-field set no single key
// can judge stays authored exactly as written and is reported by the resolver,
// which owns the Read's semantic contract, instead of being silently clamped.
TEST(ReadSourceTest, ReadValueEditsValidateThroughTheSharedParameterSeam) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", {}, nemo::MediaKind::Unknown, readyProbe(),
                                                   key));
    const ReadNodeOverrides current = readAuthoredOverrides(fixture.document, fixture.instance());
    const std::size_t baseline = fixture.history.depth();

    ReadNodeOverrides zeroStep = current;
    zeroStep.frameStep = 0;
    EXPECT_THROW(fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, zeroStep))),
                 GraphException);

    ReadNodeOverrides unknownPolicy = current;
    unknownPolicy.beforePolicy = "clamp";
    EXPECT_THROW(
        fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, unknownPolicy))),
        GraphException);

    ReadNodeOverrides unknownMode = current;
    unknownMode.rangeMode = "sometimes";
    EXPECT_THROW(fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, unknownMode))),
                 GraphException);

    // A rejected edit is atomic: no history entry and no changed value.
    EXPECT_EQ(fixture.history.depth(), baseline);
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()), current);

    // An inverted custom range is authored as written — the endpoints are two
    // independently valid keys — and the resolver refuses to resolve it.
    ReadNodeOverrides inverted = current;
    inverted.rangeMode = "custom";
    inverted.rangeFirst = 10;
    inverted.rangeLast = 4;
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, inverted)));
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()), inverted);
    EXPECT_THROW(static_cast<void>(resolveSourceRequest(fixture.document, fixture.instance(), 0)), EvaluationException);
    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()), current);

    // `explicit` names no color space, so the pair cannot resolve either; the
    // authoring seam keeps the value the artist typed and the resolver reports
    // the requirement.
    ReadFixture namelessCase;
    auto namelessKey = std::make_shared<std::string>();
    namelessCase.history.push(
        registerReadSourceCommand(ParameterAddress{namelessCase.network, namelessCase.node, "source"}, 0.0,
                                  "/media/shot.exr", {}, nemo::MediaKind::Unknown, readyProbe(), namelessKey));
    ReadNodeOverrides nameless = readAuthoredOverrides(namelessCase.document, namelessCase.instance());
    nameless.inputTransform = "explicit";
    namelessCase.history.push(setParametersCommand(readValueEdits(namelessCase.network, namelessCase.node, nameless)));
    EXPECT_EQ(readAuthoredOverrides(namelessCase.document, namelessCase.instance()), nameless);
    EXPECT_THROW(static_cast<void>(resolveSourceRequest(namelessCase.document, namelessCase.instance(), 0)),
                 EvaluationException);
}

// Reload advances the shared content revision exactly once, replaces the
// discovered facts, and leaves every Read's authored choices alone: a custom
// trim is not rewritten by a refresh.
TEST(ReadSourceTest, ReloadAdvancesRevisionOnceAndPreservesCustomTrim) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, sequenceProbe(), key));
    ReadNodeOverrides custom = readAuthoredOverrides(fixture.document, fixture.instance());
    custom.rangeMode = "custom";
    custom.rangeFirst = 1010;
    custom.rangeLast = 1020;
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, custom)));

    const SourceReference expected = fixture.document.sources.at(*key);
    MediaProbeMetadata refreshed = sequenceProbe();
    refreshed.firstFrame = 1001;
    refreshed.lastFrame = 1120;
    refreshed.availableFrameCount = 120;
    refreshed.missingFrameCount = 0;
    refreshed.missingRanges.clear();
    const std::size_t baseline = fixture.history.depth();
    fixture.history.push(reloadReadSourceCommand(*key, expected, nemo::MediaKind::Unknown, refreshed));

    EXPECT_EQ(fixture.history.depth(), baseline + 1);
    EXPECT_EQ(fixture.document.sources.at(*key).revision, expected.revision + 1);
    EXPECT_EQ(entryForKey(fixture.document, *key)->metadata.committedProbe->lastFrame,
              std::optional<std::int64_t>{1120});
    EXPECT_EQ(readAuthoredOverrides(fixture.document, fixture.instance()), custom);

    // Auto range follows the refreshed facts; the custom trim did not.
    const EffectiveSourceRequest request = resolveSourceRequest(fixture.document, fixture.instance(), 0);
    EXPECT_EQ(request.originalLastFrame, std::optional<std::int64_t>{1120});
    EXPECT_EQ(request.mapping.origin, SourceBoundsOrigin::NodeCustom);
    EXPECT_EQ(request.mapping.lastFrame, std::optional<std::int64_t>{1020});

    // A stale reload is rejected without touching the reference.
    SourceReference stale = expected;
    stale.revision += 3;
    const std::size_t after = fixture.history.depth();
    EXPECT_THROW(fixture.history.push(reloadReadSourceCommand(*key, stale, nemo::MediaKind::Unknown, refreshed)),
                 GraphException);
    EXPECT_EQ(fixture.history.depth(), after);
    EXPECT_EQ(fixture.document.sources.at(*key).revision, expected.revision + 1);
}

// A non-Read node cannot be given a media reference.
TEST(ReadSourceTest, RegisterRejectsNonReadNode) {
    ReadFixture fixture;
    const NodeId merge = fixture.addNode("merge", "Merge1");
    const std::size_t baseline = fixture.history.depth();
    EXPECT_THROW(
        fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, merge, "source"}, 0.0,
                                                       "/media/shot.exr", {}, nemo::MediaKind::Unknown, {}, {})),
        GraphException);
    EXPECT_EQ(fixture.history.depth(), baseline);
}

// Registering rejects an inadmissible probe and inadmissible choices before any
// command exists: a probe's facts are media-domain data and reject with the
// established media-query error, while an inadmissible authored choice is an
// argument error.
TEST(ReadSourceTest, RegisterRejectsInadmissibleProbeAndChoices) {
    ReadFixture fixture;
    const std::size_t baseline = fixture.history.depth();
    MediaProbeMetadata inverted = sequenceProbe();
    inverted.firstFrame = 1200;
    inverted.lastFrame = 1100;
    EXPECT_THROW(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                           "/media/shot.exr", {}, nemo::MediaKind::Unknown, inverted, {}),
                 GraphException);
    ReadNodeOverrides zeroStep;
    zeroStep.frameStep = 0;
    EXPECT_THROW(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                           "/media/shot.exr", zeroStep, nemo::MediaKind::Unknown, {}, {}),
                 std::invalid_argument);
    ReadNodeOverrides unsupportedHint;
    unsupportedHint.sourceTransfer = "gamma24";
    EXPECT_THROW(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                           "/media/shot.exr", unsupportedHint, nemo::MediaKind::Unknown, {}, {}),
                 std::invalid_argument);
    EXPECT_THROW(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0, "", {},
                                           nemo::MediaKind::Unknown, {}, {}),
                 std::invalid_argument);
    EXPECT_EQ(fixture.history.depth(), baseline);
    EXPECT_TRUE(fixture.document.sources.empty());
}

// A Read's authored choices and the shared reference are both persisted; a load
// restores them verbatim and a second save is byte-identical.
TEST(ReadSourceTest, AuthoredChoicesAndReferenceRoundTripThroughSaveLoad) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    ReadNodeOverrides overrides = sequenceOverrides();
    overrides.sourceTransfer = "linear";
    overrides.sourcePrimaries = "bt709";
    overrides.alphaMode = "premultiplied";
    overrides.beforePolicy = "hold";
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", overrides, nemo::MediaKind::Unknown,
                                                   sequenceProbe(), key));

    const nlohmann::json saved = saveDocument(fixture.document);
    const LoadResult loaded = loadDocument(saved);
    EXPECT_EQ(saveDocument(loaded.document), saved);
    const auto restoredNode = loaded.document.network(loaded.document.rootNetworkId()).graph().node(fixture.node);
    ASSERT_NE(restoredNode, nullptr);
    EXPECT_EQ(readAuthoredOverrides(loaded.document, *restoredNode), overrides);
    EXPECT_EQ(loaded.document.sources.at(*key).path, "/media/shot.####.exr");
    EXPECT_TRUE(loaded.document.sources.at(*key).interpretation.empty());
}

// A schema 4 document keeps its timing and interpretation: migration moves them
// onto the Read, the shared reference stays intact for other consumers, and the
// effective request resolves to exactly the same source frame as before.
TEST(ReadSourceTest, Schema4MigrationMaterializesSharedTimingOnTheRead) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", {}, nemo::MediaKind::Unknown, {}, key));
    // Author the legacy shape by hand: schema 4 had none of the Read's own
    // choice parameters, and stored timing and interpretation on the shared
    // reference.
    const std::string readParamKeys[] = {"rangeMode",       "rangeFirst",      "rangeLast",   "frameOffset",
                                         "frameStep",       "beforePolicy",    "afterPolicy", "missingPolicy",
                                         "inputTransform",  "inputColorSpace", "alphaMode",   "sourceTransfer",
                                         "sourcePrimaries", "sourceMatrix",    "sourceRange", "sourceChromaLocation"};
    for (const std::string& name : readParamKeys)
        fixture.document.network(fixture.network).graph().eraseParam(fixture.node, name);

    SourceReference legacy = fixture.document.sources.at(*key);
    legacy.frameOffset = 1001;
    legacy.frameStep = 1;
    legacy.firstFrame = 1001;
    legacy.lastFrame = 1100;
    legacy.interpretation = {{"transfer", "srgb"}, {"primaries", "bt709"}, {"matrix", "bt601"}};
    fixture.document.setSourceReference(*key, legacy);

    nlohmann::json json = saveDocument(fixture.document);
    json["schema"] = 4;
    const LoadResult loaded = loadDocument(json);
    ASSERT_FALSE(loaded.warnings.empty());

    // The Read now owns the legacy values...
    const auto migrated = loaded.document.network(loaded.document.rootNetworkId()).graph().node(fixture.node);
    ASSERT_NE(migrated, nullptr);
    const ReadNodeOverrides authored = readAuthoredOverrides(loaded.document, *migrated);
    EXPECT_EQ(authored.frameOffset, 1001);
    EXPECT_EQ(authored.frameStep, 1);
    EXPECT_EQ(authored.rangeMode, "custom");
    EXPECT_EQ(authored.rangeFirst, 1001);
    EXPECT_EQ(authored.rangeLast, 1100);
    EXPECT_EQ(authored.sourceTransfer, "srgb");
    EXPECT_EQ(authored.sourcePrimaries, "bt709");
    EXPECT_EQ(authored.sourceMatrix, "bt601");

    // ...and the shared reference is untouched for every other consumer.
    const SourceReference& shared = loaded.document.sources.at(*key);
    EXPECT_EQ(shared.frameOffset, 1001);
    EXPECT_EQ(shared.interpretation.size(), 3u);
    EXPECT_EQ(resolveSourceRequest(*key, shared, nullptr, 3).sourceFrame, 1004);

    // The migrated Read resolves the identical source frame and the identical
    // effective hints, with the node now naming their origin.
    const EffectiveSourceRequest request = resolveSourceRequest(loaded.document, *migrated, 3);
    EXPECT_EQ(request.sourceFrame, 1004);
    EXPECT_EQ(request.readFrame, 1004);
    EXPECT_EQ(request.mapping.origin, SourceBoundsOrigin::NodeCustom);
    EXPECT_EQ(request.interpretation.at("transfer"), "srgb");
    EXPECT_EQ(request.nodeInterpretationKeys & kSourceHintTransferBit, kSourceHintTransferBit);
    EXPECT_EQ(request.nodeInterpretationKeys & kSourceHintMatrixBit, kSourceHintMatrixBit);

    // Migration is applied once: the saved schema-5 document reloads unchanged.
    const nlohmann::json resaved = saveDocument(loaded.document);
    EXPECT_EQ(resaved.at("schema").get<int>(), 5);
    const LoadResult reopened = loadDocument(resaved);
    EXPECT_TRUE(reopened.warnings.empty());
    EXPECT_EQ(
        readAuthoredOverrides(reopened.document,
                              *reopened.document.network(reopened.document.rootNetworkId()).graph().node(fixture.node)),
        authored);
}

// The migrated node hint is a fill-only hint: the merged map carries the node's
// value with its origin, so a consumer can still let reliably tagged media win.
TEST(ReadSourceTest, NodeHintOverridesTheSharedPolicyButKeepsItsVocabulary) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.exr", {}, nemo::MediaKind::Unknown, readyProbe(), key));
    SourceReference shared = fixture.document.sources.at(*key);
    shared.interpretation = {{"transfer", "linear"}, {"range", "full"}};
    fixture.document.setSourceReference(*key, shared);

    ReadNodeOverrides overrides;
    overrides.sourceTransfer = "srgb";
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, overrides)));

    const EffectiveSourceRequest request = resolveSourceRequest(fixture.document, fixture.instance(), 0);
    EXPECT_EQ(request.interpretation.at("transfer"), "srgb");
    EXPECT_EQ(request.interpretation.at("range"), "full");  // untouched shared hint carried through
    EXPECT_EQ(request.nodeInterpretationKeys & kSourceHintTransferBit, kSourceHintTransferBit);
    EXPECT_EQ(request.nodeInterpretationKeys & kSourceHintRangeBit, 0);

    // An unknown shared field keeps reporting exactly as it did.
    SourceReference unknown = fixture.document.sources.at(*key);
    unknown.interpretation["mystery"] = "1";
    fixture.document.setSourceReference(*key, unknown);
    const EffectiveSourceRequest carried = resolveSourceRequest(fixture.document, fixture.instance(), 0);
    EXPECT_EQ(carried.interpretation.at("mystery"), "1");
}

// Coverage policy: a request outside a validated discovered interval resolves to
// the authored policy, a hole inside the interval is a missing-frame condition,
// and an estimated interval never fabricates a boundary failure.
TEST(ReadSourceTest, CoveragePolicyAppliesToBoundsAndHolesOnlyWhenAuthoritative) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", {}, nemo::MediaKind::Unknown,
                                                   sequenceProbe(), key));

    // Inside the validated interval: no policy applies.
    const EffectiveSourceRequest inside = resolveSourceRequest(fixture.document, fixture.instance(), 1001);
    EXPECT_EQ(inside.sourceFrame, 1001);
    EXPECT_EQ(inside.status, SourceRequestStatus::Ok);
    EXPECT_FALSE(inside.policyError);

    // Before the interval with the default Error policy.
    const EffectiveSourceRequest before = resolveSourceRequest(fixture.document, fixture.instance(), 1);
    EXPECT_EQ(before.sourceFrame, 1);
    EXPECT_EQ(before.status, SourceRequestStatus::BeforeRange);
    EXPECT_TRUE(before.policyError);
    EXPECT_FALSE(before.transparentBlack);

    // Hold reads the boundary frame; Black produces transparent black.
    ReadNodeOverrides holding = readAuthoredOverrides(fixture.document, fixture.instance());
    holding.beforePolicy = "hold";
    holding.afterPolicy = "black";
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, holding)));
    const EffectiveSourceRequest held = resolveSourceRequest(fixture.document, fixture.instance(), 1);
    EXPECT_EQ(held.status, SourceRequestStatus::BeforeRange);
    EXPECT_FALSE(held.policyError);
    EXPECT_EQ(held.readFrame, 1001);
    EXPECT_TRUE(held.holdApplied);
    const EffectiveSourceRequest black = resolveSourceRequest(fixture.document, fixture.instance(), 2000);
    EXPECT_EQ(black.status, SourceRequestStatus::AfterRange);
    EXPECT_TRUE(black.transparentBlack);
    EXPECT_FALSE(black.policyError);

    // A hole inside the interval is a missing-frame condition, not a boundary.
    const EffectiveSourceRequest hole = resolveSourceRequest(fixture.document, fixture.instance(), 1052);
    EXPECT_EQ(hole.status, SourceRequestStatus::MissingFrame);
    EXPECT_TRUE(hole.policyError);
    EXPECT_TRUE(hole.missingAt(1052));
    EXPECT_FALSE(hole.missingAt(1060));

    ReadNodeOverrides blackHoles = holding;
    blackHoles.missingPolicy = "black";
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, blackHoles)));
    EXPECT_TRUE(resolveSourceRequest(fixture.document, fixture.instance(), 1052).transparentBlack);

    // Estimated coverage reports its interval but never enforces it.
    ReadFixture estimated;
    auto estimatedKey = std::make_shared<std::string>();
    MediaProbeMetadata estimate = sequenceProbe();
    estimate.coverageQuality = CoverageQuality::Estimated;
    estimated.history.push(registerReadSourceCommand(ParameterAddress{estimated.network, estimated.node, "source"}, 0.0,
                                                     "/media/est.mov", {}, nemo::MediaKind::Unknown, estimate,
                                                     estimatedKey));
    const EffectiveSourceRequest loose = resolveSourceRequest(estimated.document, estimated.instance(), 900);
    EXPECT_EQ(loose.coverage, CoverageQuality::Estimated);
    EXPECT_EQ(loose.mapping.origin, SourceBoundsOrigin::DiscoveredFacts);
    EXPECT_FALSE(loose.mapping.boundsEnforced);
    EXPECT_EQ(loose.status, SourceRequestStatus::Ok);
    EXPECT_FALSE(loose.policyError);
}

// A still is time-independent: with no interval and no facts, no frame is
// outside coverage and no policy fires.
TEST(ReadSourceTest, StillIsTimeIndependent) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/still.exr", {}, nemo::MediaKind::Unknown, readyProbe(),
                                                   key));

    const EffectiveSourceRequest early = resolveSourceRequest(fixture.document, fixture.instance(), -500);
    const EffectiveSourceRequest late = resolveSourceRequest(fixture.document, fixture.instance(), 5000);
    for (const EffectiveSourceRequest* request : {&early, &late}) {
        EXPECT_EQ(request->status, SourceRequestStatus::Ok);
        EXPECT_FALSE(request->policyError);
        EXPECT_FALSE(request->transparentBlack);
        EXPECT_FALSE(request->mapping.bounded());
        EXPECT_EQ(request->mapping.origin, SourceBoundsOrigin::Unbounded);
    }
}

// Offset/Start At/reverse mapping arithmetic: one mapping, exact integers, and
// overflow is an error rather than a wrapped frame.
TEST(ReadSourceTest, MappingArithmeticCoversStartAtReverseAndOverflowBoundaries) {
    // Forward mapping shows the selected first frame at Start At.
    EXPECT_EQ(startAtOffset(0, 1001, 1100, 1), 1001);
    EXPECT_EQ(mapSourceFrame(startAtOffset(0, 1001, 1100, 1), 1, 50), 1051);
    EXPECT_EQ(startAtOffset(50, 1001, 1100, 1), 951);
    EXPECT_EQ(mapSourceFrame(951, 1, 50), 1001);
    // Reverse mapping shows the selected last frame at Start At.
    EXPECT_EQ(startAtOffset(0, 1001, 1100, -1), 1100);
    EXPECT_EQ(mapSourceFrame(1100, -1, 0), 1100);
    EXPECT_EQ(mapSourceFrame(1100, -1, 50), 1050);
    EXPECT_EQ(startAtOffset(50, 1001, 1100, -1), 1150);
    EXPECT_EQ(mapSourceFrame(1150, -1, 50), 1100);
    // A step's magnitude is part of the alignment, not just its sign: Start At
    // names the local frame that shows the selected endpoint (first for a
    // forward step, last for a reverse one), so the round trip lands there.
    EXPECT_EQ(startAtOffset(7, 1001, 1100, 2), 987);  // 1001 - 7*2
    EXPECT_EQ(mapSourceFrame(startAtOffset(7, 1001, 1100, 2), 2, 7), 1001);
    EXPECT_EQ(startAtOffset(7, 1001, 1100, -3), 1121);  // 1100 - 7*-3
    EXPECT_EQ(mapSourceFrame(startAtOffset(7, 1001, 1100, -3), -3, 7), 1100);
    EffectiveSourceMapping mapping{.frameOffset = -4, .frameStep = 2, .firstFrame = 2, .lastFrame = 7};
    EXPECT_EQ(mapping.startAt(), 3);
    mapping.frameOffset = 11;
    mapping.frameStep = -2;
    EXPECT_EQ(mapping.startAt(), 2);
    mapping.frameOffset = 10;
    EXPECT_FALSE(mapping.startAt().has_value());  // Fractional alignment.
    mapping.firstFrame = std::numeric_limits<std::int64_t>::min();
    mapping.lastFrame = mapping.firstFrame;
    mapping.frameStep = -1;
    mapping.frameOffset = 0;
    EXPECT_FALSE(mapping.startAt().has_value());  // INT64_MIN / -1.
    mapping.frameOffset = 1;
    EXPECT_FALSE(mapping.startAt().has_value());  // Anchor-minus-offset underflow.

    EXPECT_THROW(static_cast<void>(mapSourceFrame(0, 0, 1)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(startAtOffset(0, 5, 4, 1)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mapSourceFrame(std::numeric_limits<std::int64_t>::max() - 1, 1, 3)),
                 std::overflow_error);
    EXPECT_THROW(static_cast<void>(mapSourceFrame(0, std::numeric_limits<std::int64_t>::max(), 4)),
                 std::overflow_error);
    EXPECT_THROW(static_cast<void>(startAtOffset(std::numeric_limits<std::int64_t>::max(), 1001, 1100, -1)),
                 std::overflow_error);
    EXPECT_THROW(static_cast<void>(startAtOffset(std::numeric_limits<std::int64_t>::min(), 1001, 1100, 1)),
                 std::overflow_error);
    // A negative mapped frame is representable: the policy decides, not the
    // arithmetic.
    EXPECT_EQ(mapSourceFrame(0, 1, -5), -5);

    // The resolver reports an unrepresentable mapping as a node error and never
    // clamps it into a policy decision.
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    ReadNodeOverrides runaway;
    runaway.frameOffset = std::numeric_limits<std::int64_t>::max() - 1;
    runaway.rangeMode = "custom";
    runaway.rangeFirst = 0;
    runaway.rangeLast = 10;
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.exr", runaway, nemo::MediaKind::Unknown, {}, key));
    EXPECT_THROW(static_cast<void>(resolveSourceRequest(fixture.document, fixture.instance(), 4)), EvaluationException);
}

// The effective identity separates differing requests and matches equivalent
// ones, independent of the node that reaches them.
TEST(ReadSourceTest, EffectiveIdentityIsContentDerivedAcrossNodes) {
    ReadFixture fixture;
    const NodeId second = fixture.addNode("source", "Read2");
    auto firstKey = std::make_shared<std::string>();
    auto secondKey = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, sequenceProbe(), firstKey));
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, second, "source"}, 0.0,
                                                   "/media/shot.####.exr", sequenceOverrides(),
                                                   nemo::MediaKind::Unknown, sequenceProbe(), secondKey));

    const EffectiveSourceRequest first = resolveSourceRequest(fixture.document, fixture.instance(), 5);
    const EffectiveSourceRequest other =
        resolveSourceRequest(fixture.document, *fixture.document.network(fixture.network).graph().node(second), 5);
    std::string firstIdentity;
    std::string otherIdentity;
    appendEffectiveSourceIdentity(firstIdentity, first, "linear", "");
    appendEffectiveSourceIdentity(otherIdentity, other, "linear", "");
    EXPECT_EQ(firstIdentity, otherIdentity);

    ReadNodeOverrides edited =
        readAuthoredOverrides(fixture.document, *fixture.document.network(fixture.network).graph().node(second));
    edited.inputTransform = "explicit";
    edited.inputColorSpace = "ACEScg";
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, second, edited)));
    std::string changed;
    appendEffectiveSourceIdentity(
        changed,
        resolveSourceRequest(fixture.document, *fixture.document.network(fixture.network).graph().node(second), 5),
        "linear", "");
    EXPECT_NE(changed, firstIdentity);

    // A changed working target or a changed config content are different
    // identities even at one path and one frame: a cached result produced under
    // the previous color context can never be served.
    std::string otherWorkingSpace;
    appendEffectiveSourceIdentity(otherWorkingSpace, first, "aces2065", "");
    EXPECT_NE(otherWorkingSpace, firstIdentity);
    std::string configured;
    appendEffectiveSourceIdentity(configured, first, "linear", "ocio-config-v2");
    EXPECT_NE(configured, firstIdentity);
}

// The decode-owner identity is frame-independent, while the result identity is
// frame-exact: one decoder serves every frame of a clip, and no two frames share
// a reusable result.
TEST(ReadSourceTest, DecodeIdentityIsFrameIndependentWhileResultIdentityIsNot) {
    ReadFixture fixture;
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(ParameterAddress{fixture.network, fixture.node, "source"}, 0.0,
                                                   "/media/shot.exr", {}, nemo::MediaKind::Unknown, readyProbe(), key));

    const NodeInstance& read = fixture.instance();
    const EffectiveSourceRequest frameA = resolveSourceRequest(fixture.document, read, 0);
    const EffectiveSourceRequest frameB = resolveSourceRequest(fixture.document, read, 7);
    ASSERT_NE(frameA.sourceFrame, frameB.sourceFrame);

    std::string decodeA;
    std::string decodeB;
    appendSourceDecodeIdentity(decodeA, frameA, "linear", "config-1");
    appendSourceDecodeIdentity(decodeB, frameB, "linear", "config-1");
    EXPECT_EQ(decodeA, decodeB);

    std::string resultA;
    std::string resultB;
    appendEffectiveSourceIdentity(resultA, frameA, "linear", "config-1");
    appendEffectiveSourceIdentity(resultB, frameB, "linear", "config-1");
    EXPECT_NE(resultA, resultB);

    // A changed interpretation or color context is a different decode owner.
    std::string otherContext;
    appendSourceDecodeIdentity(otherContext, frameA, "aces2065", "config-1");
    EXPECT_NE(otherContext, decodeA);
    std::string otherConfig;
    appendSourceDecodeIdentity(otherConfig, frameA, "linear", "config-2");
    EXPECT_NE(otherConfig, decodeA);
    // Raw/Data bypasses conversion, so the working space and config content do
    // not describe what it decodes.
    ReadNodeOverrides data = readAuthoredOverrides(fixture.document, read);
    data.inputTransform = "raw";
    fixture.history.push(setParametersCommand(readValueEdits(fixture.network, fixture.node, data)));
    const EffectiveSourceRequest raw = resolveSourceRequest(fixture.document, fixture.instance(), 0);
    std::string rawA;
    std::string rawB;
    appendSourceDecodeIdentity(rawA, raw, "linear", "config-1");
    appendSourceDecodeIdentity(rawB, raw, "aces2065", "config-2");
    EXPECT_EQ(rawA, rawB);
}

// ---------------------------------------------------------------------------
// Binding through an address (issue #76 / story 22): a Read's file can be
// bound on one occurrence without rewriting the definition or any other
// occurrence, and an address with animation receives a current-frame key.
// ---------------------------------------------------------------------------

namespace {
struct OccurrenceFixture {
    Document document;
    CommandStack history{document};
    NetworkId definition{kInvalidNetwork};
    NodeId read{kInvalidNode};
    NetworkInstanceId first{kInvalidNetworkInstance};
    NetworkInstanceId second{kInvalidNetworkInstance};

    OccurrenceFixture() {
        definition = document.addNetwork("ReadNet");
        read = document.network(definition).graph().addNode("source", "Read1");
        first = document.addInstance(document.rootNetworkId(), definition, "A");
        second = document.addInstance(document.rootNetworkId(), definition, "B");
    }

    [[nodiscard]] ParameterAddress definitionAddress() const { return ParameterAddress{definition, read, "source"}; }

    [[nodiscard]] ParameterAddress occurrenceAddress(NetworkInstanceId instance) const {
        return ParameterAddress{definition, read, "source", instance};
    }

    [[nodiscard]] const std::string* occurrenceKey(NetworkInstanceId instance) const {
        const NetworkInstance* occurrence = document.instance(instance);
        if (occurrence == nullptr)
            return nullptr;
        const auto node = occurrence->params.find(read);
        if (node == occurrence->params.end())
            return nullptr;
        const auto value = node->second.find("source");
        return value == node->second.end() ? nullptr : std::get_if<std::string>(&value->second);
    }
};
}  // namespace

TEST(ReadSourceTest, OccurrenceBindingLeavesTheDefinitionAndOtherOccurrencesUntouched) {
    OccurrenceFixture fixture;
    auto firstKey = std::make_shared<std::string>();
    const std::size_t baseline = fixture.history.depth();
    fixture.history.push(registerReadSourceCommand(fixture.occurrenceAddress(fixture.first), 0.0, "/media/a.exr",
                                                   std::nullopt, nemo::MediaKind::Unknown, readyProbe("a"), firstKey));

    ASSERT_FALSE(firstKey->empty());
    EXPECT_EQ(fixture.history.depth(), baseline + 1);
    // The definition keeps its own (unbound) value...
    const NodeInstance& definitionNode = *fixture.document.network(fixture.definition).graph().node(fixture.read);
    EXPECT_EQ(definitionNode.params.find("source"), definitionNode.params.end());
    // ...the addressed occurrence is bound...
    ASSERT_NE(fixture.occurrenceKey(fixture.first), nullptr);
    EXPECT_EQ(*fixture.occurrenceKey(fixture.first), *firstKey);
    // ...and the other occurrence is untouched.
    EXPECT_EQ(fixture.occurrenceKey(fixture.second), nullptr);

    // One undo removes the binding and leaves the definition and sibling alone.
    ASSERT_TRUE(fixture.history.undo());
    EXPECT_EQ(fixture.occurrenceKey(fixture.first), nullptr);
    EXPECT_EQ(fixture.occurrenceKey(fixture.second), nullptr);
    EXPECT_TRUE(fixture.history.redo());
    EXPECT_EQ(*fixture.occurrenceKey(fixture.first), *firstKey);
}

TEST(ReadSourceTest, ReadBindingInitializesDefinitionOnceAndPreservesLaterChoices) {
    OccurrenceFixture fixture;
    auto firstKey = std::make_shared<std::string>();
    auto secondKey = std::make_shared<std::string>();
    ReadNodeOverrides overrides;
    overrides.frameOffset = 1001;
    overrides.beforePolicy = "hold";
    fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 0.0, "/media/a.exr", overrides,
                                                   nemo::MediaKind::Unknown, readyProbe("a"), firstKey));
    EXPECT_EQ(fixture.occurrenceKey(fixture.first), nullptr);  // definition binding, not an occurrence override

    // Replacing the file must not re-initialize or lose the authored choices.
    fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 0.0, "/media/b.exr", std::nullopt,
                                                   nemo::MediaKind::Unknown, readyProbe("b"), secondKey));
    EXPECT_EQ(fixture.occurrenceKey(fixture.first), nullptr);
    const NodeInstance& node = *fixture.document.network(fixture.definition).graph().node(fixture.read);
    const ReadNodeOverrides authored = readAuthoredOverrides(fixture.document, node);
    EXPECT_EQ(authored.frameOffset, 1001);
    EXPECT_EQ(authored.beforePolicy, "hold");
    EXPECT_EQ(std::get<std::string>(node.params.at("source")), *secondKey);

    // Re-initializing an already bound Read, or initializing through an
    // occurrence, is refused without touching anything.
    const std::size_t depth = fixture.history.depth();
    EXPECT_THROW(fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 0.0, "/media/c.exr",
                                                                overrides, nemo::MediaKind::Unknown, {}, {})),
                 GraphException);
    EXPECT_THROW(
        fixture.history.push(registerReadSourceCommand(fixture.occurrenceAddress(fixture.first), 0.0, "/media/c.exr",
                                                       overrides, nemo::MediaKind::Unknown, {}, {})),
        GraphException);
    EXPECT_EQ(fixture.history.depth(), depth);
    EXPECT_EQ(std::get<std::string>(node.params.at("source")), *secondKey);
    EXPECT_EQ(fixture.occurrenceKey(fixture.first), nullptr);

    // Key syntax is checked at construction; document scope is checked on apply.
    const auto sourcesBeforeInvalidScope = fixture.document.sources;
    EXPECT_THROW(registerReadSourceCommand(ParameterAddress{fixture.definition, fixture.read, "rangeMode"}, 0.0,
                                           "/media/c.exr", std::nullopt, nemo::MediaKind::Unknown, {}, {}),
                 std::invalid_argument);
    EXPECT_THROW(fixture.history.push(registerReadSourceCommand(fixture.occurrenceAddress(9999), 0.0, "/media/c.exr",
                                                                std::nullopt, nemo::MediaKind::Unknown, {}, {})),
                 GraphException);
    const NetworkId other = fixture.document.addNetwork("OtherNet");
    static_cast<void>(fixture.document.network(other).graph().addNode("source", "Elsewhere"));
    EXPECT_THROW(fixture.history.push(
                     registerReadSourceCommand(ParameterAddress{other, fixture.read, "source", fixture.first}, 0.0,
                                               "/media/c.exr", std::nullopt, nemo::MediaKind::Unknown, {}, {})),
                 GraphException);
    EXPECT_EQ(fixture.document.sources, sourcesBeforeInvalidScope);
    EXPECT_EQ(fixture.history.depth(), depth);
}

// An animated binding address receives a current-frame key (with the animation
// owner's interpolation policy) instead of a static write, and an unanimated
// address stays a plain value: no unrequested timing or channel writes.
TEST(ReadSourceTest, ReadBindingFollowsTheExistingChannelRule) {
    OccurrenceFixture fixture;
    const Keyframe existingKey{0, 0.0, ParameterValue{std::string{"old"}}, KeyInterpolation::Hold};
    fixture.history.push(setKeyframesCommand({KeyframeEdit{fixture.definitionAddress(), existingKey}}));
    const AnimationChannel* channel = fixture.document.animationChannel(fixture.definitionAddress());
    ASSERT_NE(channel, nullptr);
    const KeyframeId originalKey = channel->keys.front().id;

    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 7.0, "/media/a.exr", std::nullopt,
                                                   nemo::MediaKind::Unknown, readyProbe("a"), key));

    const AnimationChannel* animated = fixture.document.animationChannel(fixture.definitionAddress());
    ASSERT_NE(animated, nullptr);
    ASSERT_EQ(animated->keys.size(), 2U);
    EXPECT_EQ(animated->keys.front().id, originalKey);
    EXPECT_EQ(animated->keys.front().value, ParameterValue{std::string{"old"}});
    EXPECT_EQ(animated->keys.back().time, 7.0);
    EXPECT_EQ(animated->keys.back().value, ParameterValue{std::string{*key}});
    EXPECT_EQ(animated->keys.back().interpolation, KeyInterpolation::Hold);
    // The animated address holds no static value, and no timing parameter was
    // written by the binding.
    const NodeInstance& node = *fixture.document.network(fixture.definition).graph().node(fixture.read);
    EXPECT_EQ(node.params.find("frameOffset"), node.params.end());
    EXPECT_EQ(node.params.find("rangeMode"), node.params.end());
}

// The first-binding admission edge is the Read's own state, not an empty string:
// a cleared Read authors an empty source and keeps its trim, an animated source
// address keeps its channel, and initialization fills only what the Read does not
// already hold.
TEST(ReadSourceTest, ClearedOrAnimatedReadIsNotAFreshBinding) {
    OccurrenceFixture fixture;
    ReadNodeOverrides trimmed;
    trimmed.frameOffset = 1001;
    trimmed.rangeMode = "custom";
    trimmed.rangeFirst = 1001;
    trimmed.rangeLast = 1100;
    auto firstKey = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 0.0, "/media/a.exr", trimmed,
                                                   nemo::MediaKind::Unknown, readyProbe("a"), firstKey));

    // Clear is an ordinary value edit: the authored (empty) source stays, and so
    // do the trim and the offset.
    const ParameterAddress sourceAddress = fixture.definitionAddress();
    const ParameterEdit clearSource{sourceAddress, ParameterValue{std::string{}}};
    fixture.history.push(setParametersCommand({clearSource}));

    // Re-binding with initial choices is refused: this Read is not fresh.
    const std::size_t depth = fixture.history.depth();
    ReadNodeOverrides other;
    other.frameOffset = 5;
    EXPECT_THROW(fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 0.0, "/media/b.exr", other,
                                                                nemo::MediaKind::Unknown, readyProbe("b"), {})),
                 GraphException);
    EXPECT_EQ(fixture.history.depth(), depth);

    // Re-binding without initial choices keeps every authored choice.
    auto secondKey = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 7.0, "/media/b.exr", std::nullopt,
                                                   nemo::MediaKind::Unknown, readyProbe("b"), secondKey));
    const NodeInstance& node = *fixture.document.network(fixture.definition).graph().node(fixture.read);
    EXPECT_EQ(readAuthoredOverrides(fixture.document, node), trimmed);
    EXPECT_EQ(std::get<std::string>(node.params.at("source")), *secondKey);

    // An animated source address is bound too: initialization is refused and the
    // channel survives a re-bind.
    OccurrenceFixture keyed;
    const ParameterAddress keyedAddress = keyed.definitionAddress();
    const Keyframe keyedKeyframe{0, 0.0, ParameterValue{std::string{}}, KeyInterpolation::Hold};
    keyed.history.push(setKeyframesCommand({KeyframeEdit{keyedAddress, keyedKeyframe}}));
    const KeyframeId keyedOriginal = keyed.document.animationChannel(keyed.definitionAddress())->keys.front().id;
    ReadNodeOverrides attempt;
    attempt.frameOffset = 9;
    EXPECT_THROW(keyed.history.push(registerReadSourceCommand(keyed.definitionAddress(), 0.0, "/media/c.exr", attempt,
                                                              nemo::MediaKind::Unknown, readyProbe("c"), {})),
                 GraphException);
    auto keyedKey = std::make_shared<std::string>();
    keyed.history.push(registerReadSourceCommand(keyed.definitionAddress(), 3.0, "/media/c.exr", std::nullopt,
                                                 nemo::MediaKind::Unknown, readyProbe("c"), keyedKey));
    const AnimationChannel* channel = keyed.document.animationChannel(keyed.definitionAddress());
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(channel->keys.size(), 2U);
    EXPECT_EQ(channel->keys.front().id, keyedOriginal);
    const NodeInstance& keyedNode = *keyed.document.network(keyed.definition).graph().node(keyed.read);
    EXPECT_EQ(keyedNode.params.find("frameOffset"), keyedNode.params.end());
}

// Initial choices fill only what an otherwise unbound Read does not already hold.
TEST(ReadSourceTest, InitialChoicesNeverOverwriteAnAuthoredField) {
    OccurrenceFixture fixture;
    // The Read authors one timing field before any media is bound.
    const ParameterAddress offsetAddress{fixture.definition, fixture.read, "frameOffset"};
    const ParameterEdit authoredOffset{offsetAddress, ParameterValue{std::int64_t{42}}};
    fixture.history.push(setParametersCommand({authoredOffset}));

    ReadNodeOverrides initial;
    initial.frameOffset = 1001;
    initial.beforePolicy = "hold";
    auto key = std::make_shared<std::string>();
    fixture.history.push(registerReadSourceCommand(fixture.definitionAddress(), 0.0, "/media/a.exr", initial,
                                                   nemo::MediaKind::Unknown, readyProbe("a"), key));

    const NodeInstance& node = *fixture.document.network(fixture.definition).graph().node(fixture.read);
    const ReadNodeOverrides authored = readAuthoredOverrides(fixture.document, node);
    EXPECT_EQ(authored.frameOffset, 42);       // preserved, never overwritten
    EXPECT_EQ(authored.beforePolicy, "hold");  // missing field initialized
}
