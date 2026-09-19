// ViewerModelTests (issue #11): persistent sources in the Document, the
// source command API, source/time mapping validation, scale-aware request
// identity, declared sampling reductions, and the viewer resolution policy.
// Behavioral tests only: what a consumer of the Document/evaluation API
// observes, never implementation internals.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/core/evaluation/ViewerResolution.hpp"

namespace nemo {
namespace {
Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

// ---------------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------------

SourceReference plateSource() {
    SourceReference reference;
    reference.path = "media/plate.exr";
    reference.frameOffset = 1000;
    reference.frameStep = 1;
    return reference;
}

// Graph: source (A foreground) over constcolor (B background) into the output.
Document overGraph(const std::string& sourceKey = "plate") {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    document.name = "viewer-model";
    const NodeId color = rootGraph(document).addNode("constcolor", "bg");
    rootGraph(document).setParam(color, "color", ColorValue{{0.1F, 0.2F, 0.3F, 1.0F}});
    const NodeId source = rootGraph(document).addNode("source", "plateNode");
    rootGraph(document).setParam(source, "source", sourceKey);
    const NodeId merge = rootGraph(document).addNode("merge", "over");
    const NodeId output = rootGraph(document).addNode("output", "view");
    static_cast<void>(rootGraph(document).connect(PortRef{color, 0}, PortRef{merge, 1}));
    static_cast<void>(rootGraph(document).connect(PortRef{source, 0}, PortRef{merge, 0}));
    static_cast<void>(rootGraph(document).connect(PortRef{merge, 0}, PortRef{output, 0}));
    return document;
}

Document patternGraph() {
    Document pattern;
    rootGraph(pattern).removeNode(rootGraph(pattern).nodeByName("Output")->id);
    const NodeId node = rootGraph(pattern).addNode("testpattern", "pattern");
    const NodeId output = rootGraph(pattern).addNode("output", "view");
    static_cast<void>(rootGraph(pattern).connect(PortRef{node, 0}, PortRef{output, 0}));
    return pattern;
}

// Deterministic decode stand-in: the frame value is a function of the
// source path and mapped frame, so a test can prove which media/frame was
// actually served. Also records what it was asked for.
class FakeSourceProvider final : public SourceProvider {
public:
    [[nodiscard]] ImageDescription describe(const Document&, const EffectiveSourceRequest&) override {
        return {.format = {0, 0, 16, 16}, .dataBounds = {0, 0, 16, 16}};
    }
    [[nodiscard]] CpuImage frame(const Document& /*document*/, const EffectiveSourceRequest& source,
                                 const EvaluationRequest& request) override {
        ++calls;
        lastPath = source.path;
        lastFrame = source.readFrame;
        lastScale = request.samplingScale;
        CpuImage image(scaledDimension(request.region.width, request.samplingScale),
                       scaledDimension(request.region.height, request.samplingScale));
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                // Scene-linear stand-in encoding of (path frame, full-res x/y).
                const double r = (static_cast<double>(lastFrame) * 7.0 + static_cast<double>(x)) / 1024.0;
                const double g = (static_cast<double>(lastFrame) * 11.0 + static_cast<double>(y)) / 1024.0;
                image.setPixel(x, y, {static_cast<float>(r), static_cast<float>(g), 0.0F, 1.0F});
            }
        }
        return image;
    }

    int calls{0};
    std::string lastPath;
    std::int64_t lastFrame{0};
    int lastScale{0};
};

EvaluationRequest fullRequest(const Document& document, NodeId output, std::int64_t localTime = 0) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = output;
    request.localTime = localTime;
    request.region = {0, 0, 16, 16};
    return request;
}

// ---------------------------------------------------------------------------
// Source command API: validation, undo/redo, content identity.
// ---------------------------------------------------------------------------

TEST(SourceCommand, RejectsInvalidReferencesWithoutTouchingDocument) {
    Document document;
    document.sources["plate"] = plateSource();
    const std::uint64_t before = document.stateRevision();

    SourceReference emptyKey;
    emptyKey.path = "media/x.exr";
    EXPECT_THROW(setSourceCommand("", emptyKey), std::runtime_error);

    SourceReference emptyPath = plateSource();
    emptyPath.path.clear();
    EXPECT_THROW(setSourceCommand("shot", emptyPath), std::runtime_error);

    SourceReference zeroStep = plateSource();
    zeroStep.frameStep = 0;
    EXPECT_THROW(setSourceCommand("shot", zeroStep), std::runtime_error);

    // Nothing was added and the freshness token did not move.
    EXPECT_EQ(document.sources.size(), 1U);
    EXPECT_EQ(document.stateRevision(), before);
}

TEST(SourceCommand, AddUndoRedoRoundtrip) {
    Document document;
    CommandStack stack(document);

    const std::uint64_t initialRevision = document.stateRevision();
    stack.push(setSourceCommand("plate", plateSource()));
    ASSERT_EQ(document.sources.count("plate"), 1U);
    EXPECT_EQ(document.sources.at("plate").path, "media/plate.exr");
    const std::uint64_t withSource = document.stateRevision();
    EXPECT_NE(withSource, initialRevision);

    EXPECT_TRUE(stack.undo());
    EXPECT_TRUE(document.sources.empty());
    const std::uint64_t afterUndo = document.stateRevision();
    EXPECT_NE(afterUndo, withSource);

    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(document.sources.at("plate"), plateSource());
    const std::uint64_t afterRedo = document.stateRevision();
    EXPECT_NE(afterRedo, afterUndo);
    EXPECT_NE(afterRedo, withSource);
}

TEST(SourceCommand, ReplaceAndUndoRestoresPreviousReference) {
    Document document;
    document.sources["plate"] = plateSource();
    CommandStack stack(document);

    SourceReference changed = plateSource();
    changed.path = "media/plate_v2.exr";
    changed.frameStep = 2;
    changed.interpretation["transfer"] = "lin";
    stack.push(setSourceCommand("plate", changed));
    EXPECT_EQ(document.sources.at("plate"), changed);

    EXPECT_TRUE(stack.undo());
    EXPECT_EQ(document.sources.at("plate"), plateSource());

    EXPECT_TRUE(stack.redo());
    EXPECT_EQ(document.sources.at("plate"), changed);
}

// ---------------------------------------------------------------------------
// Serialization roundtrip.
// ---------------------------------------------------------------------------

TEST(SourceSerialization, SourcesRoundtripThroughSaveLoad) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();
    SourceReference keyed = plateSource();
    keyed.path = "media/overlay.mov";
    keyed.frameOffset = 12;
    keyed.frameStep = 2;
    keyed.interpretation["transfer"] = "log";
    keyed.interpretation["primaries"] = "rec709";
    document.sources["overlay"] = keyed;

    const nlohmann::json saved = saveDocument(document);
    const LoadResult loaded = loadDocument(saved);
    EXPECT_TRUE(loaded.warnings.empty());
    EXPECT_EQ(loaded.document.sources, document.sources);

    // Identical persistent state serializes identically.
    EXPECT_EQ(saveDocument(loaded.document), saved);
}

TEST(SourceSerialization, DocumentWithoutSourcesBlockLoads) {
    const Document document = overGraph();
    nlohmann::json stripped = saveDocument(document);
    stripped.erase("sources");
    const LoadResult loaded = loadDocument(stripped);
    EXPECT_TRUE(loaded.document.sources.empty());
    EXPECT_TRUE(loaded.warnings.empty());
}

TEST(SourceSerialization, MalformedSourceEntriesAreStructuralErrors) {
    const nlohmann::json emptyPath = {{"schema", Document::kSchemaVersion}, {"sources", {{"plate", {{"path", ""}}}}}};
    EXPECT_THROW(loadDocument(emptyPath), DeserializeError);

    const nlohmann::json zeroStep = {{"schema", Document::kSchemaVersion},
                                     {"sources", {{"plate", {{"path", "m.exr"}, {"frameStep", 0}}}}}};
    EXPECT_THROW(loadDocument(zeroStep), DeserializeError);

    const nlohmann::json badOffset = {{"schema", Document::kSchemaVersion},
                                      {"sources", {{"plate", {{"path", "m.exr"}, {"frameOffset", "ten"}}}}}};
    EXPECT_THROW(loadDocument(badOffset), DeserializeError);

    const nlohmann::json missingPath = {{"schema", Document::kSchemaVersion},
                                        {"sources", {{"plate", {{"frameOffset", 3}}}}}};
    EXPECT_THROW(loadDocument(missingPath), DeserializeError);
}

// ---------------------------------------------------------------------------
// Source node typing and time mapping.
// ---------------------------------------------------------------------------

TEST(SourceNode, DeclaresTypedPorts) {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    const NodeId source = rootGraph(document).addNode("source", "plateNode");
    const NodeId output = rootGraph(document).addNode("output", "view");
    const auto edge = rootGraph(document).connect(PortRef{source, 0}, PortRef{output, 0});
    EXPECT_EQ(rootGraph(document).edgesInto(output).front().id, edge);
    EXPECT_THROW(rootGraph(document).connect(PortRef{output, 0}, PortRef{source, 0}), GraphException);
    EXPECT_EQ(rootGraph(document).edgesInto(output).front().id, edge);
}

TEST(SourceTimeMapping, OffsetPlusLocalTimesStep) {
    SourceReference reference = plateSource();
    reference.frameOffset = 1000;
    reference.frameStep = 2;
    EXPECT_EQ(reference.frameAt(0), 1000);
    EXPECT_EQ(reference.frameAt(3), 1006);
    EXPECT_EQ(reference.frameAt(-4), 992);  // negative local time is fine while frames stay >= 0
}

TEST(SourceTimeMapping, RejectsNegativeMappedFrames) {
    SourceReference reference = plateSource();
    reference.frameOffset = 10;
    reference.frameStep = 1;
    EXPECT_THROW(reference.frameAt(-11), std::runtime_error);
    EXPECT_EQ(reference.frameAt(-10), 0);  // exactly zero is still valid
}

TEST(SourceTimeMapping, RejectsOverflowRatherThanWrapping) {
    SourceReference reference = plateSource();
    reference.frameOffset = std::numeric_limits<std::int64_t>::max() - 2;
    reference.frameStep = 1;
    EXPECT_THROW(reference.frameAt(3), std::runtime_error);

    SourceReference runaway = plateSource();
    runaway.frameOffset = 5;
    runaway.frameStep = std::numeric_limits<std::int64_t>::max() / 2;
    EXPECT_THROW(runaway.frameAt(3), std::runtime_error);  // local*step overflows
    EXPECT_NO_THROW(runaway.frameAt(0));
}

// ---------------------------------------------------------------------------
// Source evaluation: explicit rejection, provider path, effective state.
// ---------------------------------------------------------------------------

TEST(SourceEvaluation, MissingSourceParameterIsANodeIdentifyingError) {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    const NodeId source = rootGraph(document).addNode("source", "plateNode");
    const NodeId output = rootGraph(document).addNode("output", "view");
    rootGraph(document).connect(PortRef{source, 0}, PortRef{output, 0});

    FakeSourceProvider provider;
    try {
        evaluateCpu(document, fullRequest(document, output), nullptr, &provider);
        FAIL() << "expected evaluation to reject the unkeyed source node";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("'source' parameter"), std::string::npos);
        EXPECT_TRUE(error.hasNode());
        EXPECT_EQ(error.nodeName, "plateNode");
    }
}

TEST(SourceEvaluation, UnknownSourceKeyIsAnUnresolvedSourceError) {
    const Document document = overGraph("missing");

    FakeSourceProvider provider;
    try {
        evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId())), nullptr,
                    &provider);
        FAIL() << "expected evaluation to reject the unresolved source";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("unresolved source 'missing'"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("never"), std::string::npos);  // synthetic substitution
        EXPECT_EQ(provider.calls, 0);  // the provider was never asked for pixels
    }
}

TEST(SourceEvaluation, WithoutProviderRealSourceIsRejectedExplicitly) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();

    try {
        evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId())));
        FAIL() << "expected evaluation to reject real media without a provider";
    } catch (const EvaluationException& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("media/plate.exr"), std::string::npos);
        EXPECT_EQ(error.nodeName, "plateNode");
    }
}

TEST(SourceEvaluation, ProviderServesMappedFramesAndPlanRecordsEffectiveState) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();
    // Mapping is node-scoped (issue #75): one Read owns its offset/step, and the
    // shared reference keeps the value that non-Read consumers still read.
    const NodeId sourceNode = rootGraph(document).nodeByName("plateNode")->id;
    rootGraph(document).setParam(sourceNode, "frameOffset", std::int64_t{100});
    rootGraph(document).setParam(sourceNode, "frameStep", std::int64_t{2});

    FakeSourceProvider provider;
    const CpuEvaluation evaluation = evaluateCpu(
        document, fullRequest(document, resolveOutput(document, document.rootNetworkId()), 3), nullptr, &provider);

    EXPECT_EQ(provider.lastPath, "media/plate.exr");
    EXPECT_EQ(provider.lastFrame, 106);  // 100 + 3*2
    EXPECT_EQ(provider.lastScale, 1);

    // The plan records the resolved source state, not authored guesses.
    const PlanStep* sourceStep = nullptr;
    for (const PlanStep& step : evaluation.plan.steps) {
        if (step.type == "source") {
            sourceStep = &step;
        }
    }
    ASSERT_NE(sourceStep, nullptr);
    EXPECT_EQ(std::get<std::string>(sourceStep->effectiveParams.at("source")), "plate");
    EXPECT_EQ(std::get<std::string>(sourceStep->effectiveParams.at("sourcePath")), "media/plate.exr");
    EXPECT_EQ(std::get<std::int64_t>(sourceStep->effectiveParams.at("frame")), 106);

    // The decoded pixels actually reached the composition: the merge over
    // the fully opaque constcolor base carries the provider's values.
    const std::array<float, 4> decoded = evaluation.image.pixel(2, 2);
    EXPECT_FLOAT_EQ(decoded[0], static_cast<float>((106.0 * 7.0 + 2.0) / 1024.0));
    EXPECT_FLOAT_EQ(decoded[1], static_cast<float>((106.0 * 11.0 + 2.0) / 1024.0));
    EXPECT_FLOAT_EQ(decoded[2], 0.0F);
    EXPECT_FLOAT_EQ(decoded[3], 1.0F);
}

// A Read's own mapping replaces the shared reference's mapping and is never
// added to it (issue #75). The source's kind comes from the media catalog entry,
// so an interval is meaningful for a sequence (a still's availability is
// unbounded).
TEST(SourceEvaluation, ReadMappingReplacesTheSharedReferenceMapping) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();  // offset 1000, step 4: no authored interval
    SourceReference& shared = document.sources.at("plate");
    shared.frameStep = 4;
    MediaMetadata metadata;
    metadata.userName = "Plate";
    metadata.kind = MediaKind::Sequence;
    static_cast<void>(document.mediaCatalog().addEntry("plate", kInvalidMediaBin, metadata));
    const NodeId sourceNode = rootGraph(document).nodeByName("plateNode")->id;
    rootGraph(document).setParam(sourceNode, "frameOffset", std::int64_t{10});
    rootGraph(document).setParam(sourceNode, "frameStep", std::int64_t{1});

    FakeSourceProvider provider;
    static_cast<void>(evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId()), 2),
                                  nullptr, &provider));
    EXPECT_EQ(provider.lastFrame, 12);  // 10 + 2*1, never 10 + 2 + 1008

    const EffectiveSourceRequest read = resolveSourceRequest(document, *rootGraph(document).nodeByName("plateNode"), 2);
    EXPECT_EQ(read.sourceFrame, 12);
    EXPECT_EQ(read.kind, MediaKind::Sequence);
    EXPECT_EQ(read.status, SourceRequestStatus::Ok);
    EXPECT_FALSE(read.policyError);
    // With no discovered facts, no authored node range and no authored shared
    // interval, the mapping is intentionally unbounded and never enforces a
    // boundary - a still-like availability, never a guessed one.
    EXPECT_EQ(read.mapping.origin, SourceBoundsOrigin::Unbounded);
    EXPECT_FALSE(read.mapping.boundsEnforced);
    EXPECT_FALSE(read.mapping.bounded());

    // The source-scoped request non-Read consumers use keeps the shared mapping.
    const EffectiveSourceRequest scoped = resolveSourceRequest("plate", document.sources.at("plate"), nullptr, 2);
    EXPECT_EQ(scoped.sourceFrame, 1008);
    EXPECT_EQ(scoped.mapping.origin, SourceBoundsOrigin::Unbounded);
    EXPECT_FALSE(scoped.mapping.boundsEnforced);
    EXPECT_EQ(scoped.status, SourceRequestStatus::Ok);
}

// An interval the shared reference authored remains Auto's last fallback and
// bounds the Read request; an authored node range replaces that interval
// entirely. The boundary outcome is read from the structured request, never from
// error wording.
TEST(SourceEvaluation, SharedAuthoredIntervalBoundsAndNodeRangeReplacesIt) {
    Document document = overGraph();
    SourceReference shared;
    shared.path = "media/plate.####.exr";
    shared.frameOffset = 1000;
    shared.frameStep = 4;
    shared.firstFrame = 900;
    shared.lastFrame = 1200;
    document.sources["plate"] = shared;
    MediaMetadata metadata;
    metadata.userName = "Plate";
    metadata.kind = MediaKind::Sequence;
    static_cast<void>(document.mediaCatalog().addEntry("plate", kInvalidMediaBin, metadata));
    const NodeId sourceNode = rootGraph(document).nodeByName("plateNode")->id;
    rootGraph(document).setParam(sourceNode, "frameOffset", std::int64_t{10});
    rootGraph(document).setParam(sourceNode, "frameStep", std::int64_t{1});

    // The source-scoped request this shared policy addresses is inside the
    // authored interval (1000 + 2*4).
    const EffectiveSourceRequest scoped = resolveSourceRequest("plate", document.sources.at("plate"), nullptr, 2);
    EXPECT_EQ(scoped.sourceFrame, 1008);
    EXPECT_EQ(scoped.status, SourceRequestStatus::Ok);

    // The Read's own offset/step replaced the shared mapping, so its mapped
    // frame (12) falls outside the shared authored interval and the default
    // Error policy applies before any frame is opened.
    const EffectiveSourceRequest read = resolveSourceRequest(document, *rootGraph(document).nodeByName("plateNode"), 2);
    EXPECT_EQ(read.sourceFrame, 12);
    EXPECT_EQ(read.mapping.origin, SourceBoundsOrigin::SharedReference);
    EXPECT_TRUE(read.mapping.boundsEnforced);
    EXPECT_EQ(read.mapping.firstFrame, std::optional<std::int64_t>{900});
    EXPECT_EQ(read.mapping.lastFrame, std::optional<std::int64_t>{1200});
    EXPECT_EQ(read.status, SourceRequestStatus::BeforeRange);
    EXPECT_TRUE(read.policyError);

    FakeSourceProvider provider;
    try {
        evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId()), 2), nullptr,
                    &provider);
        FAIL() << "expected the out-of-range request to be rejected before decoding";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, sourceNode);
        EXPECT_EQ(provider.calls, 0);  // the out-of-range request never opened a frame
    }

    // An authored node range replaces the shared interval entirely - the same
    // local frame is now inside it and reaches the provider.
    rootGraph(document).setParam(sourceNode, "rangeMode", ParameterValue{ChoiceValue{std::string{"custom"}}});
    rootGraph(document).setParam(sourceNode, "rangeFirst", std::int64_t{0});
    rootGraph(document).setParam(sourceNode, "rangeLast", std::int64_t{100});
    const EffectiveSourceRequest custom =
        resolveSourceRequest(document, *rootGraph(document).nodeByName("plateNode"), 2);
    EXPECT_EQ(custom.mapping.origin, SourceBoundsOrigin::NodeCustom);
    EXPECT_EQ(custom.mapping.firstFrame, std::optional<std::int64_t>{0});
    EXPECT_EQ(custom.mapping.lastFrame, std::optional<std::int64_t>{100});
    EXPECT_EQ(custom.sourceFrame, 12);
    EXPECT_EQ(custom.status, SourceRequestStatus::Ok);
    static_cast<void>(evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId()), 2),
                                  nullptr, &provider));
    EXPECT_EQ(provider.lastFrame, 12);
    EXPECT_EQ(provider.calls, 1);
}

TEST(SourceEvaluation, ProviderRasterMustCoverTheRequestedRaster) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();

    class WrongSizeProvider final : public SourceProvider {
        [[nodiscard]] ImageDescription describe(const Document&, const EffectiveSourceRequest&) override {
            return {.format = {0, 0, 16, 16}, .dataBounds = {0, 0, 16, 16}};
        }
        [[nodiscard]] CpuImage frame(const Document&, const EffectiveSourceRequest&,
                                     const EvaluationRequest&) override {
            return CpuImage(4, 4);  // anything but the requested raster
        }
    };
    WrongSizeProvider provider;
    try {
        evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId())), nullptr,
                    &provider);
        FAIL() << "expected the undersized decode to be rejected";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("does not cover the requested raster"), std::string::npos);
    }
}

TEST(SourceEvaluation, ProviderFailureIdentifiesNodeAndReason) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();
    // The Read owns its mapping: author the offset on the node so the request
    // this executor asks for is the frame under test.
    const NodeId sourceNode = rootGraph(document).nodeByName("plateNode")->id;
    rootGraph(document).setParam(sourceNode, "frameOffset", std::int64_t{1000});

    class FailingProvider final : public SourceProvider {
    public:
        [[nodiscard]] ImageDescription describe(const Document&, const EffectiveSourceRequest&) override {
            return {.format = {0, 0, 16, 16}, .dataBounds = {0, 0, 16, 16}};
        }
        [[nodiscard]] CpuImage frame(const Document&, const EffectiveSourceRequest& source,
                                     const EvaluationRequest&) override {
            // The provider reports why it refused, naming the frame it was
            // asked for; the executor must not replace that reason.
            throw std::runtime_error("frame " + std::to_string(source.readFrame) + " beyond media range");
        }
    };
    FailingProvider provider;
    try {
        evaluateCpu(document, fullRequest(document, resolveOutput(document, document.rootNetworkId()), 2), nullptr,
                    &provider);
        FAIL() << "expected the failing decode to be rejected";
    } catch (const EvaluationException& error) {
        const std::string message = error.what();
        // The offending node, the source it addresses and the provider's own
        // reason are all identifiable from the failure.
        EXPECT_NE(message.find("plateNode"), std::string::npos) << message;
        EXPECT_EQ(error.node, sourceNode);
        EXPECT_NE(message.find("source provider failed for 'plate'"), std::string::npos) << message;
        EXPECT_NE(message.find("frame 1002 beyond media range"), std::string::npos) << message;
    }
}

// ---------------------------------------------------------------------------
// Source identity: source map changes invalidate only dependent content.
// ---------------------------------------------------------------------------

TEST(SourceIdentity, SourceEditChangesSourceAndDependentKeysOnly) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();

    const NodeInstance* sourceNode = rootGraph(document).nodeByName("plateNode");
    const NodeInstance* colorNode = rootGraph(document).nodeByName("bg");
    const std::vector<std::uint64_t> noInputs;
    const EvaluationRequest request = fullRequest(document, resolveOutput(document, document.rootNetworkId()));
    const ResultKey sourceBefore = nodeResultKey(document, *sourceNode, noInputs, request);
    const ResultKey colorBefore = nodeResultKey(document, *colorNode, noInputs, request);

    SourceReference changed = plateSource();
    changed.path = "media/plate_v2.exr";
    setSourceCommand("plate", changed).apply(document);

    EXPECT_NE(nodeResultKey(document, *sourceNode, noInputs, request), sourceBefore);
    EXPECT_EQ(nodeResultKey(document, *colorNode, noInputs, request), colorBefore);
}

TEST(SourceIdentity, SourceEditInvalidatesOnlyDependentCacheEntries) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();
    ResultCache<CpuImage> cache;
    FakeSourceProvider provider;
    const NodeId output = resolveOutput(document, document.rootNetworkId());

    (void)evaluateCpu(document, fullRequest(document, output), &cache, &provider);

    // Source content changes: the constcolor branch keeps its entry,
    // everything downstream of the source recomputes.
    SourceReference changed = plateSource();
    changed.path = "media/plate_v2.exr";
    CommandStack stack(document);
    stack.push(setSourceCommand("plate", changed));
    const CpuEvaluation second = evaluateCpu(document, fullRequest(document, output), &cache, &provider);
    int reusedConstcolor = 0;
    int recomputedSource = 0;
    for (const PlanStep& step : second.plan.steps) {
        if (step.type == "constcolor" && step.cacheReused) {
            ++reusedConstcolor;
        }
        if (step.type == "source" && !step.cacheReused) {
            ++recomputedSource;
        }
    }
    EXPECT_EQ(reusedConstcolor, 1);
    EXPECT_EQ(recomputedSource, 1);

    // Restoring the previous source state re-serves the original source
    // result from the cache: identity is content, not history.
    EXPECT_TRUE(stack.undo());
    const CpuEvaluation third = evaluateCpu(document, fullRequest(document, output), &cache, &provider);
    for (const PlanStep& step : third.plan.steps) {
        if (step.type == "source") {
            EXPECT_TRUE(step.cacheReused);
        }
    }
}

// ---------------------------------------------------------------------------
// Sampling support declarations and scale-aware requests.
// ---------------------------------------------------------------------------

TEST(RequestValidation, RejectsUndeclaredSamplingScales) {
    Document document = overGraph();
    document.sources["plate"] = plateSource();
    const EvaluationRequest request = fullRequest(document, resolveOutput(document, document.rootNetworkId()));

    EvaluationRequest scale3 = request;
    scale3.samplingScale = 3;
    EXPECT_THROW(validateRequest(document, scale3), EvaluationException);

    EvaluationRequest scale0 = request;
    scale0.samplingScale = 0;
    EXPECT_THROW(validateRequest(document, scale0), EvaluationException);
}

TEST(RequestValidation, UnsupportedReductionIsExplicitPerNode) {
    NodeDescriptor descriptor = *builtinNodeCatalog().find("constcolor");
    descriptor.type = "custom";
    descriptor.capabilities.samplingScales = {1};
    Document document(
        std::make_shared<const NodeCatalog>(extendedBuiltinSchema(std::vector<NodeDescriptor>{descriptor})));
    const NodeId custom = rootGraph(document).addNode("custom", "plugin");
    const NodeId output = document.network(document.rootNetworkId()).defaultOutput();
    rootGraph(document).connect(PortRef{custom, 0}, PortRef{output, 0});

    EvaluationRequest request = fullRequest(document, output);
    request.samplingScale = 2;
    try {
        validateRequest(document, request);
        FAIL() << "expected the custom node to reject the reduction";
    } catch (const EvaluationException& error) {
        EXPECT_TRUE(error.hasNode());
        EXPECT_EQ(error.nodeName, "plugin");
    }
}

TEST(ScaleAwareRaster, DimensionsCeilAndStayFullResAnchored) {
    EXPECT_EQ(scaledDimension(64, 2), 32);
    EXPECT_EQ(scaledDimension(33, 2), 17);  // ceil keeps every region edge covered
    EXPECT_EQ(scaledDimension(7, 4), 2);
}

TEST(ScaleAwareRaster, ReducedPatternSubsamplesTheFullResolutionPattern) {
    const Document pattern = patternGraph();
    const NodeId output = resolveOutput(pattern, pattern.rootNetworkId());

    EvaluationRequest full = fullRequest(pattern, output);
    full.region = {0, 0, 64, 64};
    const CpuEvaluation scale1 = evaluateCpu(pattern, full);

    EvaluationRequest half = full;
    half.samplingScale = 2;
    const CpuEvaluation scale2 = evaluateCpu(pattern, half);
    ASSERT_EQ(scale2.image.width(), 32);
    ASSERT_EQ(scale2.image.height(), 32);

    // Every scale-2 pixel equals the full-resolution pixel it covers: the
    // reduction changed pixel density, not the pattern/coordinate space.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            EXPECT_EQ(scale2.image.pixel(x, y), scale1.image.pixel(2 * x, 2 * y))
                << "scale-2 pixel (" << x << ", " << y << ")";
        }
    }
}

TEST(ScaleAwareRaster, RegionLimitedRequestsPreserveFullResolutionCoordinates) {
    const Document pattern = patternGraph();
    const NodeId output = resolveOutput(pattern, pattern.rootNetworkId());

    EvaluationRequest full = fullRequest(pattern, output);
    full.region = {0, 0, 64, 64};
    full.fullWidth = 64;
    full.fullHeight = 64;
    const CpuEvaluation whole = evaluateCpu(pattern, full);

    EvaluationRequest roi = full;
    roi.region = {16, 8, 32, 24};  // ROI in full-resolution coordinates
    const CpuEvaluation crop = evaluateCpu(pattern, roi);
    ASSERT_EQ(crop.image.width(), 32);
    ASSERT_EQ(crop.image.height(), 24);
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 32; ++x) {
            EXPECT_EQ(crop.image.pixel(x, y), whole.image.pixel(16 + x, 8 + y))
                << "roi pixel (" << x << ", " << y << ")";
        }
    }

    // ROI at a reduced scale still samples the same full-resolution space.
    roi.samplingScale = 4;
    const CpuEvaluation roiQuarter = evaluateCpu(pattern, roi);
    ASSERT_EQ(roiQuarter.image.width(), 8);
    ASSERT_EQ(roiQuarter.image.height(), 6);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) {
            EXPECT_EQ(roiQuarter.image.pixel(x, y), whole.image.pixel(16 + 4 * x, 8 + 4 * y));
        }
    }
}

TEST(RequestIdentity, ScaleAndRegionAreDistinctIdentityComponents) {
    const Document document = overGraph();
    const NodeInstance* sourceNode = rootGraph(document).nodeByName("plateNode");
    const std::vector<std::uint64_t> noInputs;

    EvaluationRequest base = fullRequest(document, resolveOutput(document, document.rootNetworkId()));
    const ResultKey fullScale = nodeResultKey(document, *sourceNode, noInputs, base);

    // Same region, different scale: distinct identities (a scale-2 result
    // must never satisfy a scale-1 request, or vice versa).
    EvaluationRequest reduced = base;
    reduced.samplingScale = 2;
    EXPECT_NE(nodeResultKey(document, *sourceNode, noInputs, reduced), fullScale);

    // Same scale, different region (ROI): distinct from both.
    EvaluationRequest roi = base;
    roi.region = {8, 0, 8, 16};
    EXPECT_NE(nodeResultKey(document, *sourceNode, noInputs, roi), fullScale);
    EXPECT_NE(nodeResultKey(document, *sourceNode, noInputs, roi),
              nodeResultKey(document, *sourceNode, noInputs, reduced));

    // Identity is stable across identical requests.
    EXPECT_EQ(nodeResultKey(document, *sourceNode, noInputs, base), fullScale);
}

// ---------------------------------------------------------------------------
// Viewer resolution policy.
// ---------------------------------------------------------------------------

TEST(ViewerResolution, ExplicitModesOverrideAuto) {
    const ViewerResolutionPolicy policy;
    EXPECT_EQ(policy.resolve(ViewerResolution::Full, 1920, 1080, 1.0, 800, 600, 1.0), 1);
    EXPECT_EQ(policy.resolve(ViewerResolution::Half, 1920, 1080, 1.0, 800, 600, 1.0), 2);
    EXPECT_EQ(policy.resolve(ViewerResolution::Quarter, 1920, 1080, 1.0, 800, 600, 1.0), 4);
}

TEST(ViewerResolution, AutoKeepsFullWhenImageFitsOrIsZoomedIn) {
    ViewerResolutionPolicy policy;
    // 800x600 image in a 1000x800 panel fits: Full.
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 800, 600, 1.0, 1000, 800), 1);
    // Zooming in shrinks the shown image region faster than the panel:
    // Full at any magnification.
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 4000, 2000, 1.0, 800, 600, 8.0), 1);
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 4000, 2000, 1.0, 800, 600, 32.0), 1);
}

TEST(ViewerResolution, AutoReducesLargeImagesByPhysicalArea) {
    ViewerResolutionPolicy policy;
    // Each image dimension is 10x the panel's: r = 100, maximum reduction.
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 8000, 4000, 1.0, 800, 400), 4);
    // Moderate reduction (r slightly above 4).
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 1800, 1800, 1.0, 850, 850), 2);
}

TEST(ViewerResolution, AutoHonorsPixelAspectInFit) {
    // The pixel aspect changes the DISPLAY area the image covers, which is
    // what the level is chosen from.
    const ViewerFit squarePa = aspectFit(800, 600, 1.0, 800, 600);
    const ViewerFit widePa = aspectFit(800, 600, 2.0, 800, 600);
    EXPECT_DOUBLE_EQ(squarePa.width, 800.0);
    EXPECT_DOUBLE_EQ(squarePa.height, 600.0);
    EXPECT_DOUBLE_EQ(widePa.width, 800.0);
    EXPECT_DOUBLE_EQ(widePa.height, 300.0);  // width-limited by the aspect-corrected width

    // A narrow panel: the square image sits exactly on the r = 4 Full/Half
    // boundary (Full), while the aspect-2 image covers four times the
    // image pixels per panel pixel (Half).
    ViewerResolutionPolicy policy;
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 800, 600, 1.0, 400, 800), 1);
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 800, 600, 2.0, 400, 800), 2);
}

TEST(ViewerResolution, HysteresisHoldsAcrossEquivalentConfigurations) {
    ViewerResolutionPolicy policy;
    // Establish Half at r = 16 (4000x2000 image, 1000x500 panel, fitted
    // exactly): the base rule picks Half on the boundary.
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 4000, 2000, 1.0, 1000, 500), 2);
    EXPECT_EQ(policy.lastAutoScale(), 2);

    // One-pixel panel resizes around the boundary keep r inside the
    // switching band [12.8, 19.2): the level must hold, not oscillate.
    for (int panelHeight = 470; panelHeight <= 520; ++panelHeight) {
        EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 4000, 2000, 1.0, 1000, panelHeight), 2)
            << "panel height " << panelHeight;
    }
    // Crossing far past the band switches, then one pixel back holds.
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 4000, 2000, 1.0, 1000, 400), 4);
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 4000, 2000, 1.0, 1000, 401), 4);
}

TEST(ViewerResolution, ZoomChangesTheRequestedSamplingDensity) {
    ViewerResolutionPolicy policy;
    // Image fitting the panel at 1:1 (fitted = panel): zoom out to 0.2
    // shows the whole image in 1/25 of the panel area => r = 6.25, Half.
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 800, 600, 1.0, 1600, 1200, 0.2), 2);
    // Zooming in needs at most Full density over the visible ROI.
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 800, 600, 1.0, 1600, 1200, 2.0), 1);
    // Zoomed out past r = 16 uses Quarter (whole image, small display).
    policy.reset();
    EXPECT_EQ(policy.resolve(ViewerResolution::Auto, 800, 600, 1.0, 1600, 1200, 0.1), 4);
}

TEST(SourceTimeMapping, ReverseAndNegativeLocalTimeRemainSigned) {
    SourceReference source;
    source.path = "plate.mkv";
    source.frameOffset = 10;
    source.frameStep = -2;
    EXPECT_EQ(source.frameAt(3), 4);
    EXPECT_EQ(source.frameAt(-3), 16);
    source.frameStep = 2;
    EXPECT_EQ(source.frameAt(-3), 4);
}

TEST(SourceCommand, GraphHistoryPreservesConnectionsAcrossRepeatedRedo) {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    CommandStack commands(document);
    auto source = std::make_shared<NodeId>();
    auto output = std::make_shared<NodeId>();
    commands.push(addNodeCommand(document.rootNetworkId(), "constcolor", "color", source));
    commands.push(addNodeCommand(document.rootNetworkId(), "output", "view", output));
    commands.push(connectCommand(document.rootNetworkId(), {*source, 0}, {*output, 0}));
    commands.push(setParamCommand(document.rootNetworkId(), *source, "color", ColorValue{{0.1F, 0.2F, 0.3F, 1.0F}}));
    const auto saved = saveDocument(document);
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(commands.undo());
        ASSERT_TRUE(commands.undo());
        ASSERT_TRUE(commands.undo());
        ASSERT_TRUE(commands.undo());
        ASSERT_TRUE(commands.redo());
        ASSERT_TRUE(commands.redo());
        ASSERT_TRUE(commands.redo());
        ASSERT_TRUE(commands.redo());
        EXPECT_EQ(saveDocument(document), saved);
        const auto image = evaluateCpu(document, fullRequest(document, *output)).image;
        EXPECT_EQ(image.pixel(0, 0), (std::array<float, 4>{0.1F, 0.2F, 0.3F, 1}));
    }
}

}  // namespace
}  // namespace nemo
