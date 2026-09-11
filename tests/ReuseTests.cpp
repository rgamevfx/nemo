// Evaluator graph reuse and invalidation tests (issue #9, spec sections
// 8/10.3, ADR-0004).
//
// The seam is the content-derived ResultKey + ResultCache over the shared
// evaluators: reuse is keyed on effective dependency state (implementation
// version, node type, parameters, effective input results, mapped local
// time, region, channels, quality, working space) — never on document edit
// history — and publication freshness is guarded by an evaluation ticket.
// Counts defend observable avoided work, not private storage layout.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Reuse.hpp"

using namespace nemo;

namespace {
Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

Document makeDocument(const std::vector<std::pair<std::string, std::string>>& typeAndName) {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    for (const auto& [type, name] : typeAndName) {
        rootGraph(document).addNode(type, name);
    }
    return document;
}

void connect(Graph& graph, const std::string& from, const std::string& to, std::uint32_t fromPort = 0,
             std::uint32_t toPort = 0) {
    const NodeInstance* fromNode = graph.nodeByName(from);
    const NodeInstance* toNode = graph.nodeByName(to);
    ASSERT_NE(fromNode, nullptr);
    ASSERT_NE(toNode, nullptr);
    static_cast<void>(graph.connect(PortRef{fromNode->id, fromPort}, PortRef{toNode->id, toPort}));
}

EvaluationRequest requestFor(const Document& document, const std::string& outputName, std::int64_t frame,
                             Region region = {0, 0, 8, 4}) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = rootGraph(document).nodeByName(outputName)->id;
    request.localTime = frame;
    request.region = region;
    request.fullWidth = std::max(8, region.x + region.width);
    request.fullHeight = std::max(4, region.y + region.height);
    return request;
}

// Shared VFX composition with independent downstream grades (acceptance
// example 1): plate and tint feed two merges carrying different authored
// grades, each through its own Output.
struct SharedVfx {
    Document doc;
    NodeId tint{kInvalidNode};
    NodeId gradeA{kInvalidNode};
    NodeId gradeB{kInvalidNode};
};

[[nodiscard]] SharedVfx makeSharedVfx(const char* tint, const char* gradeA, const char* gradeB) {
    SharedVfx fixture;
    Document& doc = fixture.doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = "shared-vfx";
    (void)rootGraph(doc).addNode("testpattern", "plate");
    fixture.tint = rootGraph(doc).addNode("constcolor", "tint");
    rootGraph(doc).setParam(fixture.tint, "color", tint);
    fixture.gradeA = rootGraph(doc).addNode("merge", "gradeA");
    rootGraph(doc).setParam(fixture.gradeA, "grade", gradeA);
    fixture.gradeB = rootGraph(doc).addNode("merge", "gradeB");
    rootGraph(doc).setParam(fixture.gradeB, "grade", gradeB);
    (void)rootGraph(doc).addNode("output", "outA");
    (void)rootGraph(doc).addNode("output", "outB");
    connect(rootGraph(doc), "plate", "gradeA", 0, 0);
    connect(rootGraph(doc), "tint", "gradeA", 0, 1);
    connect(rootGraph(doc), "gradeA", "outA");
    connect(rootGraph(doc), "plate", "gradeB", 0, 0);
    connect(rootGraph(doc), "tint", "gradeB", 0, 1);
    connect(rootGraph(doc), "gradeB", "outB");
    return fixture;
}

[[nodiscard]] bool samePixels(const CpuImage& a, const CpuImage& b) {
    if (a.width() != b.width() || a.height() != b.height()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] const PlanStep* stepFor(const EvaluationPlan& plan, const std::string& name) {
    for (const auto& step : plan.steps) {
        if (step.name == name) {
            return &step;
        }
    }
    return nullptr;
}

}  // namespace
// Acceptance example 5: revisiting a valid matching cached request does not
// rerender — the second identical request reuses every step and returns an
// identical image.
TEST(ReuseTest, SecondIdenticalRequestReusesWithoutRerender) {
    Document doc = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(doc), "plate", "out");
    const EvaluationRequest request = requestFor(doc, "out", 12);

    ResultCache<CpuImage> cache;
    const CpuEvaluation first = evaluateCpu(doc, request, &cache);
    const CacheCounts afterFirst = cache.counts();
    EXPECT_EQ(afterFirst.hits, 0u);
    EXPECT_GT(afterFirst.misses, 0u);

    const CpuEvaluation second = evaluateCpu(doc, request, &cache);
    const CacheCounts afterSecond = cache.counts();
    EXPECT_GT(afterSecond.hits, afterFirst.hits);
    EXPECT_EQ(afterSecond.misses, afterFirst.misses);
    EXPECT_TRUE(samePixels(first.image, second.image));
    EXPECT_EQ(first.plan.result.contentHash, second.plan.result.contentHash);
    // Plan evidence: every step on the second pass came from the cache.
    for (const PlanStep& step : second.plan.steps) {
        EXPECT_TRUE(step.cacheReused) << "step " << step.name;
    }
}

// Acceptance example 1: shared VFX with independent grades. Evaluating both
// outputs reuses the shared upstream results; changing one grade invalidates
// only its branch; changing the shared VFX invalidates both.
TEST(ReuseTest, SharedVfxWithIndependentGradesReuseAndInvalidateByDependency) {
    SharedVfx fixture = makeSharedVfx("0 0 1 0.5", "1", "2");

    ResultCache<CpuImage> cache;
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outA", 0), &cache));
    const CacheCounts afterA = cache.counts();
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outB", 0), &cache));
    const CacheCounts afterB = cache.counts();
    // The shared VFX and plate results are valid for the second output.
    EXPECT_EQ(afterB.hits - afterA.hits, 2u);
    // The two grades are different requests: gradeB and outB were computed.
    EXPECT_EQ(afterB.misses - afterA.misses, 2u);

    // Change one grade: only gradeA's branch re-renders; shared results and
    // gradeB stay valid.
    CommandStack stack(fixture.doc);
    stack.push(setParamCommand(fixture.doc.rootNetworkId(), fixture.gradeA, "grade", "3"));
    const CacheCounts before = cache.counts();
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outA", 0), &cache));
    const CacheCounts afterGradeEdit = cache.counts();
    EXPECT_EQ(afterGradeEdit.hits - before.hits, 2u);      // plate + tint reused
    EXPECT_EQ(afterGradeEdit.misses - before.misses, 2u);  // gradeA + outA recomputed

    // Change the shared VFX: both affected downstream branches invalidate.
    stack.push(setParamCommand(fixture.doc.rootNetworkId(), fixture.tint, "color", "1 0 0 1"));
    const CacheCounts beforeShared = cache.counts();
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outA", 0), &cache));
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outB", 0), &cache));
    const CacheCounts afterSharedEdit = cache.counts();
    // tint recomputed once then reused for outB; both grades and both
    // outputs recomputed (their effective input identities changed).
    EXPECT_EQ(afterSharedEdit.misses - beforeShared.misses, 5u);
    EXPECT_EQ(afterSharedEdit.hits - beforeShared.hits, 3u);  // plate, tint, plate
}

// Acceptance example 2: an unrelated document edit does not invalidate
// branch reuse — identity comes from effective state, not edit history.
TEST(ReuseTest, UnrelatedEditPreservesBranchReuse) {
    SharedVfx fixture = makeSharedVfx("0 0 1 0.5", "1", "2");
    ResultCache<CpuImage> cache;
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outA", 0), &cache));

    // Unrelated edit: a new disconnected node bumps the document revision.
    const std::uint64_t revisionBefore = fixture.doc.stateRevision();
    (void)rootGraph(fixture.doc).addNode("constcolor", "unused");
    const CpuEvaluation reevaluated = evaluateCpu(fixture.doc, requestFor(fixture.doc, "outA", 0), &cache);
    const CacheCounts counts = cache.counts();
    EXPECT_GT(counts.hits, 0u);
    EXPECT_EQ(counts.misses, 4u);
    EXPECT_NE(fixture.doc.stateRevision(), revisionBefore);
    for (const PlanStep& step : reevaluated.plan.steps) {
        EXPECT_TRUE(step.cacheReused) << "step " << step.name;
    }

    // An edit to the *other* output's grade is unrelated to outA.
    CommandStack stack(fixture.doc);
    stack.push(setParamCommand(fixture.doc.rootNetworkId(), fixture.gradeB, "grade", "9"));
    const CacheCounts before = cache.counts();
    static_cast<void>(evaluateCpu(fixture.doc, requestFor(fixture.doc, "outA", 0), &cache));
    const CacheCounts after = cache.counts();
    EXPECT_EQ(after.hits - before.hits, 4u);  // plate, tint, gradeA, outA reused
    EXPECT_EQ(after.misses - before.misses, 0u);
}

// Acceptance example 2: different mapped times are different requests, and
// their representations coexist — a new request never erases valid siblings.
TEST(ReuseTest, TimeVariantsCoexistWithoutBlanketDeletion) {
    Document doc = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(doc), "plate", "out");
    ResultCache<CpuImage> cache;
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 1), &cache));
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 2), &cache));
    const CacheCounts afterTwoFrames = cache.counts();
    EXPECT_EQ(afterTwoFrames.misses, 4u);  // plate+out per frame

    // Revisiting frame 1 hits; nothing was erased by the frame-2 request.
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 1), &cache));
    const CacheCounts afterRevisit = cache.counts();
    EXPECT_EQ(afterRevisit.hits - afterTwoFrames.hits, 2u);
    EXPECT_EQ(afterRevisit.misses, afterTwoFrames.misses);
}

// Acceptance example 3 (representation part): region variants are distinct
// requests and coexist; revisiting either reuses its own representation.
TEST(ReuseTest, RegionVariantsCoexist) {
    Document doc = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(doc), "plate", "out");
    ResultCache<CpuImage> cache;
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 0, {0, 0, 8, 4}), &cache));
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 0, {2, 1, 4, 2}), &cache));
    const CacheCounts afterBoth = cache.counts();
    EXPECT_EQ(afterBoth.misses, 4u);

    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 0, {0, 0, 8, 4}), &cache));
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 0, {2, 1, 4, 2}), &cache));
    const CacheCounts afterRevisit = cache.counts();
    EXPECT_EQ(afterRevisit.hits - afterBoth.hits, 4u);
    EXPECT_EQ(afterRevisit.misses, afterBoth.misses);
}

// Acceptance example 3 (viewer part): a viewer-transform edit invalidates
// viewer representations — their baked viewing state changed — without
// touching upstream scene-linear reuse. Scene-linear keys exclude viewing
// state; viewerResultKey bakes it in.
TEST(ReuseTest, ViewerTransformEditInvalidatesViewerIdentityButNotSceneLinearReuse) {
    Document doc = makeDocument({{"constcolor", "tint"}, {"output", "out"}});
    rootGraph(doc).setParam(rootGraph(doc).nodeByName("tint")->id, "color", "0.25 0.5 1 1");
    connect(rootGraph(doc), "tint", "out");
    const EvaluationRequest request = requestFor(doc, "out", 0);

    ResultCache<CpuImage> cache;
    static_cast<void>(evaluateCpu(doc, request, &cache));

    const ResultKey sceneLinearBefore = nodeResultKey(doc, *rootGraph(doc).nodeByName("tint"), {}, request);
    const ResultKey viewerBefore = viewerResultKey(sceneLinearBefore, doc.color);

    // Viewer-transform edit through the sanctioned color-policy command.
    CommandStack stack(doc);
    const ColorPolicy changed{doc.color.workingSpace, "Rec709/Rec.1886", doc.color.deliveryTransform};
    stack.push(setColorPolicyCommand(changed));

    const ResultKey sceneLinearAfter = nodeResultKey(doc, *rootGraph(doc).nodeByName("tint"), {}, request);
    const ResultKey viewerAfter = viewerResultKey(sceneLinearAfter, doc.color);
    EXPECT_EQ(sceneLinearBefore, sceneLinearAfter);
    EXPECT_NE(viewerBefore, viewerAfter);

    // The scene-linear evaluation itself fully reuses.
    const CacheCounts before = cache.counts();
    const CpuEvaluation evaluation = evaluateCpu(doc, request, &cache);
    const CacheCounts after = cache.counts();
    EXPECT_EQ(after.hits - before.hits, 2u);
    EXPECT_EQ(after.misses - before.misses, 0u);
    for (const PlanStep& step : evaluation.plan.steps) {
        EXPECT_TRUE(step.cacheReused) << "step " << step.name;
    }
}

// Acceptance example 5: a stale publication cannot replace cache state — a
// ticket issued before a document edit (or superseded by a newer request)
// is rejected and counted.
TEST(ReuseTest, StalePublicationTicketIsRejected) {
    Document doc = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(doc), "plate", "out");
    ResultCache<CpuImage> cache;

    const EvaluationTicket stale = cache.beginTicket(doc);
    (void)rootGraph(doc).addNode("constcolor", "late-edit");

    ResultCache<CpuImage>::Entry entry;
    entry.key = ResultKey{1, "stale"};
    entry.image = std::make_shared<const CpuImage>(8, 4);
    entry.identity = ImageIdentity{};

    EXPECT_FALSE(cache.publish(doc, stale, entry.key, std::move(entry.image), entry.identity));
    const CacheCounts counts = cache.counts();
    EXPECT_EQ(counts.staleRejected, 1u);
    EXPECT_EQ(counts.published, 0u);

    // A superseded (older-generation) ticket is equally stale even without
    // a document edit.
    const EvaluationTicket old = cache.beginTicket(doc);
    const EvaluationTicket fresh = cache.beginTicket(doc);
    ResultCache<CpuImage>::Entry freshEntry;
    freshEntry.key = ResultKey{2, "fresh"};
    freshEntry.image = std::make_shared<const CpuImage>(8, 4);
    EXPECT_TRUE(cache.publish(doc, fresh, freshEntry.key, std::move(freshEntry.image), freshEntry.identity));
    ResultCache<CpuImage>::Entry oldEntry;
    oldEntry.key = ResultKey{3, "old"};
    oldEntry.image = std::make_shared<const CpuImage>(8, 4);
    EXPECT_FALSE(cache.publish(doc, old, oldEntry.key, std::move(oldEntry.image), oldEntry.identity));
    EXPECT_EQ(cache.counts().staleRejected, 2u);
}

// Acceptance example 6: undo/redo restores effective state; evaluation is
// correct and may reuse the still-valid matching representation without
// accepting stale publication.
TEST(ReuseTest, UndoRedoRestoresEffectiveStateAndReuse) {
    Document doc = makeDocument({{"constcolor", "tint"}, {"output", "out"}});
    rootGraph(doc).setParam(rootGraph(doc).nodeByName("tint")->id, "color", "0 0 1 1");
    connect(rootGraph(doc), "tint", "out");
    const EvaluationRequest request = requestFor(doc, "out", 0);

    ResultCache<CpuImage> cache;
    const CpuEvaluation original = evaluateCpu(doc, request, &cache);

    CommandStack stack(doc);
    stack.push(setParamCommand(doc.rootNetworkId(), rootGraph(doc).nodeByName("tint")->id, "color", "1 0 0 1"));
    const CpuEvaluation edited = evaluateCpu(doc, request, &cache);
    EXPECT_FALSE(samePixels(original.image, edited.image));

    EXPECT_TRUE(stack.undo());
    const CpuEvaluation undone = evaluateCpu(doc, request, &cache);
    EXPECT_TRUE(samePixels(original.image, undone.image));
    const PlanStep* tintStep = stepFor(undone.plan, "tint");
    ASSERT_NE(tintStep, nullptr);
    EXPECT_TRUE(tintStep->cacheReused);  // still-valid representation reused

    EXPECT_TRUE(stack.redo());
    const CpuEvaluation redone = evaluateCpu(doc, request, &cache);
    EXPECT_TRUE(samePixels(edited.image, redone.image));
    tintStep = stepFor(redone.plan, "tint");
    ASSERT_NE(tintStep, nullptr);
    EXPECT_TRUE(tintStep->cacheReused);
}

// Acceptance example 4: identity is content-derived, so an occurrence whose
// effective composition-local inputs and time are unchanged keeps its
// result — even when node identities differ (fresh graph, same definition
// and state). A changed mapped time does not reuse.
TEST(ReuseTest, EquivalentOccurrenceWithUnchangedEffectiveStateReuses) {
    auto buildGraph = [] {
        Document doc =
            makeDocument({{"testpattern", "plate"}, {"constcolor", "tint"}, {"merge", "comp"}, {"output", "out"}});
        rootGraph(doc).setParam(rootGraph(doc).nodeByName("tint")->id, "color", "0 0 1 0.5");
        connect(rootGraph(doc), "plate", "comp", 0, 0);
        connect(rootGraph(doc), "tint", "comp", 0, 1);
        connect(rootGraph(doc), "comp", "out");
        return doc;
    };
    Document doc = buildGraph();
    const EvaluationRequest request = requestFor(doc, "out", 7);

    ResultCache<CpuImage> cache;
    const CpuEvaluation first = evaluateCpu(doc, request, &cache);
    EXPECT_EQ(cache.counts().misses, 4u);

    // The same occurrence rebuilt as fresh document state: new node ids,
    // identical definition and effective state.
    Document moved = buildGraph();
    const CpuEvaluation reused = evaluateCpu(moved, requestFor(moved, "out", 7), &cache);
    const CacheCounts counts = cache.counts();
    EXPECT_EQ(counts.misses, 4u);  // nothing new was computed
    EXPECT_EQ(counts.hits, 4u);
    EXPECT_TRUE(samePixels(first.image, reused.image));
    EXPECT_EQ(first.plan.result.contentHash, reused.plan.result.contentHash);
    EXPECT_NE(first.plan.result.contentHash, 0u);

    // A changed mapped local time does not reuse.
    static_cast<void>(evaluateCpu(moved, requestFor(moved, "out", 8), &cache));
    EXPECT_EQ(cache.counts().misses, 8u);
}

// Acceptance example 5: if a valid entry is evicted, the same identity may
// be computed again on demand.
TEST(ReuseTest, EvictedIdentityIsRecomputedOnDemand) {
    Document doc = makeDocument({{"constcolor", "tint"}, {"output", "out"}});
    rootGraph(doc).setParam(rootGraph(doc).nodeByName("tint")->id, "color", "0 0 1 1");
    connect(rootGraph(doc), "tint", "out");
    const EvaluationRequest request = requestFor(doc, "out", 0);

    ResultCache<CpuImage> cache;
    const CpuEvaluation first = evaluateCpu(doc, request, &cache);
    const std::uint64_t identity = first.plan.result.contentHash;
    const ResultKey tintKey = nodeResultKey(doc, *rootGraph(doc).nodeByName("tint"), {}, request);
    cache.evict(tintKey);
    cache.evict(nodeResultKey(doc, *rootGraph(doc).nodeByName("out"), {tintKey.hash}, request));

    const CpuEvaluation recomputed = evaluateCpu(doc, request, &cache);
    EXPECT_EQ(cache.counts().hits, 0u);  // both representations were evicted
    EXPECT_TRUE(samePixels(first.image, recomputed.image));
    EXPECT_EQ(recomputed.plan.result.contentHash, identity);
}

// Reuse is bounded: the entry cap drops the oldest entries (bounded
// residency under GPU ownership rules; the LRU/disk policy of issue #14
// builds elsewhere on this bound).
TEST(ReuseTest, EntryCapDropsOldestEntries) {
    Document doc = makeDocument({{"constcolor", "tint"}, {"output", "out"}});
    connect(rootGraph(doc), "tint", "out");
    ResultCache<CpuImage> cache(2);
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 1), &cache));
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 2), &cache));
    EXPECT_EQ(cache.counts().evicted, 2u);

    // The evicted frame-1 representations are computed again on demand.
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 1), &cache));
    EXPECT_EQ(cache.counts().misses, 6u);
}

TEST(ReuseTest, EditingCachedNodeInvalidatesOnlyItsResults) {
    Document doc = makeDocument({{"testpattern", "plate"}, {"output", "out"}});
    connect(rootGraph(doc), "plate", "out");
    ResultCache<CpuImage> cache;
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 0), &cache));

    CommandStack stack(doc);
    stack.push(setParamCommand(doc.rootNetworkId(), rootGraph(doc).nodeByName("plate")->id, "gain", "2"));
    const CacheCounts before = cache.counts();
    static_cast<void>(evaluateCpu(doc, requestFor(doc, "out", 0), &cache));
    const CacheCounts after = cache.counts();
    EXPECT_EQ(after.hits - before.hits, 0u);
    EXPECT_EQ(after.misses - before.misses, 2u);  // plate + out re-render
}

// The canonical key form is injective: param sets that would alias under
// naive string concatenation ('=' or separator bytes inside names/values)
// must produce distinct keys — otherwise the cache could serve wrong
// results (ADR-0007: canonical equality is the collision protection).
TEST(ReuseTest, AliasedParamSetsGetDistinctKeys) {
    EvaluationRequest request;

    const auto keyFor = [&](const std::map<std::string, std::string>& params) {
        Document value = makeDocument({{"constcolor", "tint"}});
        request.network = value.rootNetworkId();
        const NodeId id = rootGraph(value).nodeByName("tint")->id;
        for (const auto& [key, parameter] : params)
            rootGraph(value).setParam(id, key, parameter);
        return nodeResultKey(value, *rootGraph(value).node(id), {}, request);
    };

    const ResultKey valueWithEquals = keyFor({{"a", "b=c"}});
    const ResultKey keyWithEquals = keyFor({{"a=b", "c"}});
    const ResultKey valueWithSeparator = keyFor({{"a", "v\x1F"
                                                       "b=c"}});
    const ResultKey twoParams = keyFor({{"a", "v"}, {"b", "c"}});

    EXPECT_NE(valueWithEquals, keyWithEquals);
    EXPECT_NE(valueWithSeparator, twoParams);
    EXPECT_NE(valueWithEquals, twoParams);
    EXPECT_NE(valueWithSeparator, keyWithEquals);
}

// Acceptance example 2 (implementation-identity arm): a different
// implementation identity — a different version of the operation or a
// different effect library — must not hit the same cache entry.
TEST(ReuseTest, ImplementationIdentityChangesTheKey) {
    Document doc = makeDocument({{"constcolor", "tint"}});
    const NodeInstance* tint = rootGraph(doc).nodeByName("tint");
    EvaluationRequest request;
    request.network = doc.rootNetworkId();

    const ResultKey cpuReference = nodeResultKey(doc, *tint, {}, request);
    const ResultKey otherImplementation = nodeResultKey(doc, *tint, {}, request, KeyContext{1});
    EXPECT_NE(cpuReference, otherImplementation);
}
