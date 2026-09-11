// Native GPU effect execution tests (issue #8, spec sections 10.4/11 shader
// contract gate).
//
// The seam is nemo::eval::evaluateGpu over the shared evaluation plan, with
// two front ends meeting one declared effect contract: build-time Slang
// SPIR-V (the native path) and runtime-compiled GLSL (the
// reference-equivalence path). Declared, operation-specific tolerances —
// not bitwise equality — and wrong bindings/alpha/coordinates must FAIL
// the comparison (spec section 10.4).
//
// Tests guard themselves: without a usable Vulkan device they skip. Slang
// SPIR-V comes from the nemo_shaders build target (NEMO_SLANG_SPV_DIR);
// when that directory is absent (CPU-only configure) the Slang-path tests
// skip with the enabling instructions — a skipped GPU run is not gate
// evidence, and this is reported rather than silently passed.
//
// Readback policy: every test performs at most ONE declared diagnostic
// readback (the output); the executor path itself never reads back. Zero
// validation-layer warnings is the bar (as in GpuTests/ColorTests).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/eval/EffectShaders.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"

using namespace nemo;

namespace {
Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

enum class BootstrapOutcome { Created, NoDevice, Failed };
struct Bootstrap {
    std::unique_ptr<gpu::Instance> instance;
    std::unique_ptr<gpu::Device> device;
    std::unique_ptr<gpu::Allocator> allocator;
    BootstrapOutcome outcome = BootstrapOutcome::Created;
    std::string message;
};

[[nodiscard]] Bootstrap createBootstrap() {
    Bootstrap boot;
    try {
        boot.instance = gpu::Instance::create({.validation = true});
        boot.device = gpu::Device::create(*boot.instance);
        boot.allocator = gpu::Allocator::create(*boot.instance, *boot.device, {.max_device_bytes = 256u << 20});
    } catch (const gpu::GpuException& error) {
        boot.outcome =
            error.errorCode() == gpu::GpuError::NoDevice ? BootstrapOutcome::NoDevice : BootstrapOutcome::Failed;
        boot.message = error.what();
    }
    return boot;
}

#define NEMO_SKIP_OR_FAIL(boot)                                                                                        \
    do {                                                                                                               \
        if ((boot).outcome == BootstrapOutcome::NoDevice)                                                              \
            GTEST_SKIP() << (boot).message;                                                                            \
        if ((boot).outcome == BootstrapOutcome::Failed) {                                                              \
            ADD_FAILURE() << "device creation failed: " << (boot).message;                                             \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

void expectValidationClean(gpu::Instance& instance) {
    if (!instance.validation_enabled()) {
        return;
    }
    std::string collected;
    bool hasWarnings = false;
    for (const auto& message : instance.take_debug_messages()) {
        collected += message.text + "\n";
        hasWarnings = hasWarnings || message.severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    EXPECT_FALSE(hasWarnings) << "validation-layer messages:\n" << collected;
}

// The nemo_shaders output directory; empty when slangc was unavailable at
// configure time (a CPU-only configuration, reported by the skip).
#if defined(NEMO_SLANG_SPV_DIR)
[[nodiscard]] std::filesystem::path slangSpvDir() {
    return NEMO_SLANG_SPV_DIR;
}
[[nodiscard]] std::filesystem::path slangSrcDir() {
    return NEMO_SLANG_SRC_DIR;
}
#else
[[nodiscard]] std::filesystem::path slangSpvDir() {
    return {};
}
[[nodiscard]] std::filesystem::path slangSrcDir() {
    return {};
}
#endif

// Requires a device AND compiled Slang kernels; skip otherwise.
#define NEMO_SKIP_UNLESS_SLANG(boot)                                                                                   \
    do {                                                                                                               \
        NEMO_SKIP_OR_FAIL(boot);                                                                                       \
        if (slangSpvDir().empty()) {                                                                                   \
            GTEST_SKIP() << "no compiled Slang kernels (CPU-only configuration): configure with "                      \
                            "-D NEMO_DOWNLOAD_SLANGC=ON or -D NEMO_SLANGC=<path>";                                     \
        }                                                                                                              \
    } while (false)

// The multi-node acceptance composition (issue #8 example 1): spatially
// varying testpattern as the over base, a const tint with values outside
// the display range ([0, 1]) and partial alpha as the over source, merged
// and emitted through Output. Out-of-range scene-linear values must
// survive both executors unclamped.
struct Composition {
    Document doc;
    NodeId output = kInvalidNode;
    NodeId tint = kInvalidNode;
};

[[nodiscard]] Composition makeComposition() {
    Composition composition;
    Document& doc = composition.doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = "gpu-effect-composition";
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    composition.tint = rootGraph(doc).addNode("constcolor", "tint");
    rootGraph(doc).setParam(composition.tint, "color", "0.5 8 -1 0.25");
    const NodeId over = rootGraph(doc).addNode("merge", "over");
    composition.output = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({plate, 0}, {over, 0});
    (void)rootGraph(doc).connect({composition.tint, 0}, {over, 1});
    (void)rootGraph(doc).connect({over, 0}, {composition.output, 0});
    return composition;
}

[[nodiscard]] CpuImage evaluateCpuImage(const Document& doc, const EvaluationRequest& request) {
    return evaluateCpu(doc, request).image;
}
[[nodiscard]] EvaluationRequest requestFor(const Document& doc, Region region, std::int64_t frame) {
    EvaluationRequest request;
    request.network = doc.rootNetworkId();
    request.output = resolveOutput(doc, request.network);
    request.localTime = frame;
    request.region = region;
    request.fullWidth = region.x + region.width;
    request.fullHeight = region.y + region.height;
    return request;
}

// Declared, operation-specific tolerances (spec section 10.4: operation-
// specific, not a universal bitwise promise):
//   constcolor: 0      — one constant broadcast, exact on both executors.
//   merge:      2e-7   — same float32 expression; difference budget covers
//                        potential FMA fusion in the GPU compiler.
//   testpattern: 1e-6  — CPU computes gradients in double then rounds once;
//                        the shaders compute in float (<= 1 ulp difference).
// The composition comparisons below bound by the max of the ops involved
// (kTolerance), keeping one named constant for the shared helpers.
constexpr float kMergeTolerance = 2e-7F;
constexpr float kTestpatternTolerance = 1e-6F;
constexpr float kTolerance = kTestpatternTolerance;

void expectImagesClose(const CpuImage& expected, const CpuImage& actual, float tolerance, const char* what) {
    ASSERT_EQ(expected.width(), actual.width()) << what;
    ASSERT_EQ(expected.height(), actual.height()) << what;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const auto e = expected.pixel(x, y);
            const auto a = actual.pixel(x, y);
            for (std::size_t c = 0; c < CpuImage::channelCount(); ++c) {
                EXPECT_NEAR(a[c], e[c], tolerance) << what << ": pixel (" << x << "," << y << ") channel " << c;
            }
        }
    }
}

// The constcolor operation's declared tolerance is 0: one broadcast
// constant must be bit-exact on every executor (this is what "operation-
// specific tolerances" buys — exact where the op is exact).
TEST(Effect, ConstcolorIsBitExact) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Document doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = "exact-const";
    const NodeId color = rootGraph(doc).addNode("constcolor", "color");
    rootGraph(doc).setParam(color, "color", "4 -2 2.5 0.125");
    const NodeId out = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({color, 0}, {out, 0});
    const EvaluationRequest request = requestFor(doc, {0, 0, 9, 7}, 0);

    const CpuImage cpuImage = evaluateCpuImage(doc, request);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    eval::GpuEvaluation slangEval = evaluateGpu(doc, request, slang, *boot.device, *boot.allocator);
    const CpuImage slangImage = slangEval.readBack(request.output, *boot.device, *boot.allocator);
    eval::GpuEvaluation glslEval = evaluateGpu(doc, request, glsl, *boot.device, *boot.allocator);
    const CpuImage glslImage = glslEval.readBack(request.output, *boot.device, *boot.allocator);
    for (int y = 0; y < request.region.height; ++y) {
        for (int x = 0; x < request.region.width; ++x) {
            EXPECT_EQ(slangImage.pixel(x, y), cpuImage.pixel(x, y)) << "pixel (" << x << "," << y << ")";
            EXPECT_EQ(glslImage.pixel(x, y), cpuImage.pixel(x, y)) << "pixel (" << x << "," << y << ")";
        }
    }
    expectValidationClean(*boot.instance);
}

// Max channel difference over the shared extent, for threshold-style checks
// where a WRONG interpretation must exceed the declared tolerance.
[[nodiscard]] float maxChannelDiff(const CpuImage& a, const CpuImage& b) {
    float diff = 0.0F;
    for (int y = 0; y < std::min(a.height(), b.height()); ++y) {
        for (int x = 0; x < std::min(a.width(), b.width()); ++x) {
            const auto pa = a.pixel(x, y);
            const auto pb = b.pixel(x, y);
            for (std::size_t c = 0; c < CpuImage::channelCount(); ++c) {
                diff = std::max(diff, std::fabs(pa[c] - pb[c]));
            }
        }
    }
    return diff;
}

}  // namespace

// ---------------------------------------------------------------------------
// Example 1: multi-node GPU composition vs CPU reference within declared
// tolerance; a contributing input change moves the output.
// ---------------------------------------------------------------------------

TEST(Effect, MultiNodeGpuCompositionMatchesCpuReference) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 24, 17}, 3);

    const CpuEvaluation cpu = evaluateCpu(composition.doc, request);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation gpuEval = evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator);
    const CpuImage gpuImage = gpuEval.readBack(request.output, *boot.device, *boot.allocator);

    EXPECT_EQ(gpuEval.plan.result.residency, Residency::GpuDevice);
    EXPECT_EQ(gpuEval.plan.steps.size(), cpu.plan.steps.size());
    expectImagesClose(cpu.image, gpuImage, kTolerance, "slang vs cpu");

    // Values outside the display range survive: the tint's G channel (8.0)
    // over the testpattern contributes out-of-range scene-linear values —
    // the result is stored, not clamped to [0, 1].
    bool sawOutOfRange = false;
    for (int y = 0; y < gpuImage.height() && !sawOutOfRange; ++y) {
        for (int x = 0; x < gpuImage.width(); ++x) {
            if (gpuImage.pixel(x, y)[1] > 1.0F) {
                sawOutOfRange = true;
                break;
            }
        }
    }
    EXPECT_TRUE(sawOutOfRange) << "out-of-range scene-linear value was clamped or lost";

    // A contributing input change must change the GPU output.
    Document changed = composition.doc;
    rootGraph(changed).setParam(composition.tint, "color", "0.5 8 -1 1.0");
    eval::GpuEvaluation changedEval = evaluateGpu(changed, request, slang, *boot.device, *boot.allocator);
    const CpuImage changedImage = changedEval.readBack(request.output, *boot.device, *boot.allocator);
    EXPECT_GT(gpuEval.plan.result.contentHash, 0u);
    EXPECT_NE(gpuEval.plan.result.contentHash, changedEval.plan.result.contentHash);
    EXPECT_GT(maxChannelDiff(gpuImage, changedImage), kTolerance);
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Example 2: equivalent Slang and GLSL effects agree within their declared
// tolerance.
// ---------------------------------------------------------------------------

TEST(Effect, SlangAndGlslEffectsAgree) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 24, 17}, 3);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();

    eval::GpuEvaluation slangEval = evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator);
    const CpuImage slangImage = slangEval.readBack(request.output, *boot.device, *boot.allocator);
    eval::GpuEvaluation glslEval = evaluateGpu(composition.doc, request, glsl, *boot.device, *boot.allocator);
    const CpuImage glslImage = glslEval.readBack(request.output, *boot.device, *boot.allocator);

    expectImagesClose(slangImage, glslImage, kTolerance, "glsl vs slang");
    expectValidationClean(*boot.instance);
}

// A WRONG interpretation of the declared contract must fail the comparison:
// (a) swapped merge ports (base/source exchanged), (b) premultiplied-alpha
// over instead of the declared straight-alpha over.
namespace {

[[nodiscard]] eval::EffectLibrary glslLibraryWithMerge(const char* mergeBody) {
    eval::EffectLibrary library = eval::glslEffectLibrary();
    library["merge"] = eval::EffectProgram{{}, std::string(eval::kGlslPreamble) + mergeBody, "test-only merge GLSL"};
    return library;
}

constexpr const char* kSwappedPortsMerge = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_a;
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_b;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    vec4 bg = imageLoad(in_b, ivec2(p));  // WRONG: ports swapped
    vec4 fg = imageLoad(in_a, ivec2(p));
    vec4 result;
    result.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    result.w = fg.a + (1.0 - fg.a) * bg.a;
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

constexpr const char* kPremultipliedMerge = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_a;
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_b;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    vec4 bg = imageLoad(in_a, ivec2(p));
    vec4 fg = imageLoad(in_b, ivec2(p));  // WRONG: premultiplied over
    vec4 result;
    result.xyz = fg.xyz + (1.0 - fg.a) * bg.xyz;
    result.w = fg.a + (1.0 - fg.a) * bg.a;
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

}  // namespace

TEST(Effect, WrongBindingsAndAlphaFailComparison) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 24, 17}, 3);
    const CpuImage cpuImage = evaluateCpuImage(composition.doc, request);

    for (const char* mergeBody : {kSwappedPortsMerge, kPremultipliedMerge}) {
        const eval::EffectLibrary wrong = glslLibraryWithMerge(mergeBody);
        eval::GpuEvaluation wrongEval = evaluateGpu(composition.doc, request, wrong, *boot.device, *boot.allocator);
        const CpuImage wrongImage = wrongEval.readBack(request.output, *boot.device, *boot.allocator);
        // The wrong interpretation must exceed the merge operation's own
        // declared tolerance (kMergeTolerance), not just the composition
        // bound — this is the contract-enforcement bar for this op.
        EXPECT_GT(maxChannelDiff(cpuImage, wrongImage), kMergeTolerance)
            << "wrong interpretation passed the declared tolerance";
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Example 3: a region-limited request preserves full-resolution coordinate
// semantics on both executors.
// ---------------------------------------------------------------------------

TEST(Effect, RegionLimitedRequestPreservesCoordinates) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const Region region{8, 4, 33, 21};
    const EvaluationRequest request = requestFor(composition.doc, region, 5);

    const CpuEvaluation cpu = evaluateCpu(composition.doc, request);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation gpuEval = evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator);
    const CpuImage gpuImage = gpuEval.readBack(request.output, *boot.device, *boot.allocator);

    ASSERT_EQ(gpuImage.width(), region.width);
    ASSERT_EQ(gpuImage.height(), region.height);
    // Both executors anchor the pattern to the same full-image frame
    // implied by the request, so the region-limited results agree — the
    // crop did not restart the gradient at the region origin.
    expectImagesClose(cpu.image, gpuImage, kTolerance, "region-limited slang vs cpu");
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Example 4: correct lifetimes and synchronization through dependent
// effects, with the only readback at the output.
// ---------------------------------------------------------------------------

TEST(Effect, DependentChainSynchronizesWithoutIntermediateReadback) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    // plate ── over1(A) ── over2(A) ── out
    // tint1 ──↗          tint2 ──↗
    Document doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = "dependent-chain";
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    const NodeId tint1 = rootGraph(doc).addNode("constcolor", "tint1");
    rootGraph(doc).setParam(tint1, "color", "1 0.5 0.25 0.5");
    const NodeId tint2 = rootGraph(doc).addNode("constcolor", "tint2");
    rootGraph(doc).setParam(tint2, "color", "0.25 0.5 2 0.75");
    const NodeId over1 = rootGraph(doc).addNode("merge", "over1");
    const NodeId over2 = rootGraph(doc).addNode("merge", "over2");
    const NodeId out = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({plate, 0}, {over1, 0});
    (void)rootGraph(doc).connect({tint1, 0}, {over1, 1});
    (void)rootGraph(doc).connect({over1, 0}, {over2, 0});
    (void)rootGraph(doc).connect({tint2, 0}, {over2, 1});
    (void)rootGraph(doc).connect({over2, 0}, {out, 0});

    const EvaluationRequest request = requestFor(doc, {0, 0, 40, 25}, 7);
    const CpuEvaluation cpu = evaluateCpu(doc, request);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation gpuEval = evaluateGpu(doc, request, slang, *boot.device, *boot.allocator);
    ASSERT_EQ(gpuEval.plan.steps.size(), 6u);

    // Only the output is read back — the intermediate chain stayed
    // GPU-resident; reading the output proves every write→read barrier in
    // the chain produced the correct dependent values.
    const CpuImage gpuImage = gpuEval.readBack(request.output, *boot.device, *boot.allocator);
    expectImagesClose(cpu.image, gpuImage, kTolerance, "chain slang vs cpu");
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Example 5: invalid shader source produces a diagnostic naming the node
// and the source location; it does not silently substitute another effect.
// ---------------------------------------------------------------------------

TEST(Effect, InvalidShaderSourceIdentifiesNodeAndLocation) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 16, 16}, 0);

    // Broken merge GLSL: references an undeclared image.
    constexpr const char* kBroken = R"GLSL(
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    vec4 v = imageLoad(this_does_not_exist, ivec2(p));
    imageStore(out_color, ivec2(p), v);
}
)GLSL";
    const eval::EffectLibrary broken = glslLibraryWithMerge(kBroken);

    bool threw = false;
    try {
        (void)evaluateGpu(composition.doc, request, broken, *boot.device, *boot.allocator);
        ADD_FAILURE() << "expected EvaluationException for broken shader source";
    } catch (const EvaluationException& error) {
        threw = true;
        const std::string message = error.what();
        // The offending node...
        EXPECT_NE(message.find("'over'"), std::string::npos) << message;
        EXPECT_TRUE(error.hasNode());
        // ...the effect...
        EXPECT_NE(message.find("effect 'merge'"), std::string::npos) << message;
        // ...and the glslang source location (ERROR: 0:<line> marker in the
        // log) naming the bad identifier.
        EXPECT_NE(message.find("ERROR"), std::string::npos) << message;
        EXPECT_NE(message.find("this_does_not_exist"), std::string::npos) << message;
    }
    EXPECT_TRUE(threw);
    // The failure propagated: evaluation did not fall back to another
    // implementation and produced no result.
    expectValidationClean(*boot.instance);
}

TEST(Effect, MissingSlangKernelFailsWithoutSubstitution) {
    // Library-level: a missing kernel names the effect and path.
    const std::filesystem::path emptyDir =
        std::filesystem::temp_directory_path() / ("nemo-empty-spv-" + std::to_string(::getpid()));
    std::filesystem::create_directories(emptyDir);
    try {
        (void)eval::loadSlangEffectLibrary(emptyDir);
        ADD_FAILURE() << "expected GpuException for missing kernels";
    } catch (const gpu::GpuException& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("effect 'testpattern'"), std::string::npos) << message;
        EXPECT_NE(message.find(emptyDir.string()), std::string::npos) << message;
    }

    // Executor-level: a library missing 'merge' names the node and does not
    // substitute another implementation.
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 16, 16}, 0);
    eval::EffectLibrary partial = eval::glslEffectLibrary();
    partial.erase("merge");
    try {
        (void)evaluateGpu(composition.doc, request, partial, *boot.device, *boot.allocator);
        ADD_FAILURE() << "expected EvaluationException for missing effect package";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("no effect package"), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.what()).find("no silent substitution"), std::string::npos) << error.what();
    }
}

// ---------------------------------------------------------------------------
// Issue #9: graph reuse over the native GPU path. Identical requests reuse
// device-resident results without re-dispatch; a changed dependency (a new
// mapped time) invalidates exactly the time-dependent keys. Declared
// diagnostic readbacks: two (before/after reuse) at the output.
// ---------------------------------------------------------------------------
TEST(Effect, GpuReuseAvoidsRecomputationAndPreservesResults) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 16, 16}, 0);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());

    ResultCache<eval::GpuNodeImage> cache;
    eval::GpuEvaluation first =
        evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &cache);
    const CacheCounts afterFirst = cache.counts();
    EXPECT_EQ(afterFirst.hits, 0u);
    EXPECT_GT(afterFirst.misses, 0u);
    const CpuImage firstImage = first.readBack(request.output, *boot.device, *boot.allocator);
    const std::uint64_t firstIdentity = first.plan.result.contentHash;

    eval::GpuEvaluation second =
        evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &cache);
    const CacheCounts afterSecond = cache.counts();
    EXPECT_GT(afterSecond.hits, afterFirst.hits);
    EXPECT_EQ(afterSecond.misses, afterFirst.misses);  // no recomputation
    for (const PlanStep& step : second.plan.steps) {
        EXPECT_TRUE(step.cacheReused) << "step " << step.name;
    }
    const CpuImage secondImage = second.readBack(request.output, *boot.device, *boot.allocator);
    EXPECT_EQ(secondImage.pixel(0, 0), firstImage.pixel(0, 0));
    EXPECT_GT(firstIdentity, 0u);

    // A different mapped time is a different request: the conservative
    // identity includes mapped local time for every node, so all keys move
    // and nothing is served for the new frame (no wrong reuse).
    eval::GpuEvaluation changedTime = evaluateGpu(composition.doc, requestFor(composition.doc, {0, 0, 16, 16}, 1),
                                                  slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &cache);
    const CacheCounts afterTimeChange = cache.counts();
    EXPECT_EQ(afterTimeChange.misses - afterSecond.misses, 4u);
    EXPECT_EQ(afterTimeChange.hits, afterSecond.hits);
    expectValidationClean(*boot.instance);
}

TEST(Effect, DroppedAsyncGraphRetainsResourcesUntilCompletion) {
    auto boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    const auto composition = makeComposition();
    const auto request = requestFor(composition.doc, {0, 0, 16, 16}, 0);
    const auto effects = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    auto& queue = boot.device->submissions(boot.device->graphics_family());
    struct Gate {
        VkDevice device;
        VkSemaphore semaphore{};
        ~Gate() {
            if (semaphore)
                vkDestroySemaphore(device, semaphore, nullptr);
        }
    };
    auto gate = std::make_shared<Gate>();
    gate->device = boot.device->handle();
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE,
                                   0};
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type, 0};
    ASSERT_EQ(vkCreateSemaphore(gate->device, &create, nullptr, &gate->semaphore), VK_SUCCESS);
    gpu::SubmissionQueue::TimelineSemaphores dependencies;
    dependencies.wait = {gate->semaphore};
    dependencies.waitValues = {1};
    const auto blocker = queue.submit([](VkCommandBuffer) {}, {gate}, dependencies);
    ASSERT_TRUE(blocker);
    // Always release the timeline gate before propagating a preparation
    // exception or using a fatal assertion on the graph result.
    std::optional<eval::GpuEvaluation> pending;
    std::exception_ptr failure;
    try {
        pending = eval::submitGpu(composition.doc, request, effects, *boot.device, *boot.allocator);
    } catch (...) {
        failure = std::current_exception();
    }
    if (pending) {
        EXPECT_TRUE(pending->completion);
        EXPECT_FALSE(queue.poll(*pending->completion));
    }
    const auto completion = pending ? pending->completion : std::nullopt;
    pending.reset();  // cancellation: no publication, but GPU ownership remains
    if (completion)
        EXPECT_GT(boot.allocator->charged_bytes(), 0u);
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, gate->semaphore, 1};
    EXPECT_EQ(vkSignalSemaphore(gate->device, &signal), VK_SUCCESS);
    EXPECT_TRUE(queue.wait(*blocker, 5'000'000'000ULL));
    if (completion)
        EXPECT_TRUE(queue.wait(*completion, 5'000'000'000ULL));
    if (failure)
        std::rethrow_exception(failure);
    ASSERT_TRUE(completion);
    EXPECT_EQ(boot.allocator->charged_bytes(), 0u);
    auto retry = eval::evaluateGpu(composition.doc, request, effects, *boot.device, *boot.allocator);
    expectImagesClose(evaluateCpuImage(composition.doc, request),
                      retry.readBack(request.output, *boot.device, *boot.allocator), kTolerance,
                      "retry after cancellation");
    expectValidationClean(*boot.instance);
}
