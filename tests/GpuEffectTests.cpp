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
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "contributions/Affine.hpp"
#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/nodes/GpuCommon.hpp"

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
    rootGraph(doc).setParam(composition.tint, "color", ColorValue{{0.5F, 8.0F, -1.0F, 0.25F}});
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
//   merge ops:  1e-6   — issue #75's extended operations chain more mixed
//                        operations, so their FMA budget is wider than the
//                        single Over expression (still far below the
//                        wrong-binding differences the negative controls
//                        must exceed).
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
            for (std::size_t c = 0; c < e.size(); ++c) {
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
    rootGraph(doc).setParam(color, "color", ColorValue{{4.0F, -2.0F, 2.5F, 0.125F}});
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

TEST(Effect, PremultAndUnpremultMatchIndependentArithmeticOnBothNativeFrontends) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();

    const auto runCase = [&](const char* type, const char* selectorKey, const std::array<float, 4>& input,
                             const std::optional<ChoiceValue>& selector, const std::optional<ChoiceValue>& by,
                             const std::array<float, 4>& expected, const char* what) {
        Document doc;
        rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
        const NodeId color = rootGraph(doc).addNode("constcolor", "input");
        rootGraph(doc).setParam(color, "color", ColorValue{input});
        const NodeId operation = rootGraph(doc).addNode(type, "operation");
        if (selector)
            rootGraph(doc).setParam(operation, selectorKey, *selector);
        if (by)
            rootGraph(doc).setParam(operation, "by", *by);
        const NodeId output = rootGraph(doc).addNode("output", "result");
        (void)rootGraph(doc).connect({color, 0}, {operation, 0});
        (void)rootGraph(doc).connect({operation, 0}, {output, 0});
        const EvaluationRequest request = requestFor(doc, {0, 0, 3, 2}, 0);

        const CpuImage cpu = evaluateCpuImage(doc, request);
        const CpuImage slangImage = evaluateGpu(doc, request, slang, *boot.device, *boot.allocator)
                                        .readBack(output, *boot.device, *boot.allocator);
        const CpuImage glslImage = evaluateGpu(doc, request, glsl, *boot.device, *boot.allocator)
                                       .readBack(output, *boot.device, *boot.allocator);
        for (int y = 0; y < request.region.height; ++y) {
            for (int x = 0; x < request.region.width; ++x) {
                for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                    EXPECT_FLOAT_EQ(cpu.pixel(x, y)[channel], expected[channel]) << what << " CPU channel " << channel;
                    EXPECT_FLOAT_EQ(slangImage.pixel(x, y)[channel], expected[channel])
                        << what << " Slang channel " << channel;
                    EXPECT_FLOAT_EQ(glslImage.pixel(x, y)[channel], expected[channel])
                        << what << " GLSL channel " << channel;
                }
            }
        }
    };

    runCase("premult", "multiply", {-2.0F, 8.0F, 0.5F, -0.25F}, std::nullopt, std::nullopt,
            {0.5F, -2.0F, -0.125F, -0.25F}, "default premult by negative alpha");
    runCase("premult", "multiply", {-2.0F, 8.0F, 0.5F, -0.25F}, ChoiceValue{"R"}, ChoiceValue{"G"},
            {-16.0F, 8.0F, 0.5F, -0.25F}, "partial premult by green");
    runCase("unpremult", "divide", {-2.0F, 8.0F, 0.5F, -0.25F}, std::nullopt, std::nullopt,
            {8.0F, -32.0F, -2.0F, -0.25F}, "default unpremult by negative alpha");
    runCase("unpremult", "divide", {-2.0F, 8.0F, 0.5F, 0.0F}, std::nullopt, std::nullopt, {0.0F, 0.0F, 0.0F, 0.0F},
            "exact zero divisor");
    runCase("unpremult", "divide", {0.000003814697265625F, -0.0000019073486328125F, 0.5F, 0.00000095367431640625F},
            std::nullopt, std::nullopt, {4.0F, -2.0F, 524288.0F, 0.00000095367431640625F},
            "near-zero divisor without epsilon or clamp");

    Document chain;
    rootGraph(chain).removeNode(rootGraph(chain).nodeByName("Output")->id);
    const NodeId color = rootGraph(chain).addNode("constcolor", "premultiplied");
    rootGraph(chain).setParam(color, "color", ColorValue{{0.25F, 0.5F, 1.0F, 0.25F}});
    const NodeId unpremult = rootGraph(chain).addNode("unpremult", "Unpremult");
    const NodeId grade = rootGraph(chain).addNode("grade", "Grade");
    rootGraph(chain).setParam(grade, "multiply", ColorValue{{2.0F, 2.0F, 2.0F, 1.0F}});
    const NodeId premult = rootGraph(chain).addNode("premult", "Premult");
    const NodeId output = rootGraph(chain).addNode("output", "result");
    (void)rootGraph(chain).connect({color, 0}, {unpremult, 0});
    (void)rootGraph(chain).connect({unpremult, 0}, {grade, 0});
    (void)rootGraph(chain).connect({grade, 0}, {premult, 0});
    (void)rootGraph(chain).connect({premult, 0}, {output, 0});
    const EvaluationRequest request = requestFor(chain, {0, 0, 2, 2}, 0);
    constexpr std::array<float, 4> kAuthoredResult{0.5F, 1.0F, 2.0F, 0.25F};
    const CpuImage cpu = evaluateCpuImage(chain, request);
    const CpuImage slangImage = evaluateGpu(chain, request, slang, *boot.device, *boot.allocator)
                                    .readBack(output, *boot.device, *boot.allocator);
    const CpuImage glslImage = evaluateGpu(chain, request, glsl, *boot.device, *boot.allocator)
                                   .readBack(output, *boot.device, *boot.allocator);
    for (std::size_t channel = 0; channel < kAuthoredResult.size(); ++channel) {
        EXPECT_FLOAT_EQ(cpu.pixel(0, 0)[channel], kAuthoredResult[channel]);
        EXPECT_FLOAT_EQ(slangImage.pixel(0, 0)[channel], kAuthoredResult[channel]);
        EXPECT_FLOAT_EQ(glslImage.pixel(0, 0)[channel], kAuthoredResult[channel]);
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
            for (std::size_t c = 0; c < pa.size(); ++c) {
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
    rootGraph(changed).setParam(composition.tint, "color", ColorValue{{0.5F, 8.0F, -1.0F, 1.0F}});
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
    auto contributions = eval::builtinGpuContributions();
    const auto merge = std::find_if(contributions.begin(), contributions.end(),
                                    [](const auto& entry) { return entry.node.descriptor.type == "merge"; });
    // Independent deliberately wrong pixels still use the public declaration
    // seam, before the immutable snapshot is assembled.
    merge->gpu->passes[0].glsl = nemo::nodes::gpuGlsl(R"GLSL(
layout(std140, set = 0, binding = 1) uniform MergePayload {
    vec4 mask;
    vec4 op;
};
)GLSL",
                                                      mergeBody);
    return eval::EffectLibrary(std::move(contributions), eval::EffectBackend::Glsl);
}

constexpr const char* kSwappedPortsMerge = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_a;
layout(set = 1, binding = 1) restrict readonly uniform image2D in_b;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    vec4 bg = gpuLoadRgba(in_b, ivec2(p), inputGeometry[1].rgba, int(inputGeometry[1].extent.y),
                          inputGeometry[1].channels.y);  // WRONG: ports swapped
    vec4 fg = gpuLoadRgba(in_a, ivec2(p), inputGeometry[0].rgba, int(inputGeometry[0].extent.y),
                          inputGeometry[0].channels.y);
    vec4 result;
    result.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    result.w = fg.a + (1.0 - fg.a) * bg.a;
    gpuStoreRgba(out_color, ivec2(p), int(meta2.y), channels.y, rgba, result);
}
)GLSL";

constexpr const char* kPremultipliedMerge = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_a;
layout(set = 1, binding = 1) restrict readonly uniform image2D in_b;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    vec4 bg = gpuLoadRgba(in_a, ivec2(p), inputGeometry[0].rgba, int(inputGeometry[0].extent.y),
                          inputGeometry[0].channels.y);
    vec4 fg = gpuLoadRgba(in_b, ivec2(p), inputGeometry[1].rgba, int(inputGeometry[1].extent.y),
                          inputGeometry[1].channels.y);  // WRONG: premultiplied over
    vec4 result;
    result.xyz = fg.xyz + (1.0 - fg.a) * bg.xyz;
    result.w = fg.a + (1.0 - fg.a) * bg.a;
    gpuStoreRgba(out_color, ivec2(p), int(meta2.y), channels.y, rgba, result);
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
// Issue #75: Merge operations, the optional mask, mix and A/B roles on the
// native GPU path. The oracle fixture uses distinct per-channel RGB AND
// distinct background/foreground alpha, so a swapped binding changes every
// operation.
// ---------------------------------------------------------------------------

namespace {

struct MergeOracle {
    Document doc;
    NodeId background{kInvalidNode};
    NodeId foreground{kInvalidNode};
    NodeId mask{kInvalidNode};
    NodeId merge{kInvalidNode};
    NodeId output{kInvalidNode};
};

[[nodiscard]] MergeOracle makeMergeOracle() {
    MergeOracle oracle;
    Document& doc = oracle.doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = "merge-oracle";
    oracle.background = rootGraph(doc).addNode("constcolor", "background");
    rootGraph(doc).setParam(oracle.background, "color", ColorValue{{0.2F, 0.4F, 0.6F, 0.4F}});
    oracle.foreground = rootGraph(doc).addNode("constcolor", "foreground");
    rootGraph(doc).setParam(oracle.foreground, "color", ColorValue{{0.8F, 0.5F, 0.25F, 0.5F}});
    oracle.mask = rootGraph(doc).addNode("constcolor", "mask");
    rootGraph(doc).setParam(oracle.mask, "color", ColorValue{{0.1F, 0.2F, 0.3F, 0.3F}});
    oracle.merge = rootGraph(doc).addNode("merge", "comp");
    oracle.output = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({oracle.background, 0}, {oracle.merge, 0});
    (void)rootGraph(doc).connect({oracle.foreground, 0}, {oracle.merge, 1});
    (void)rootGraph(doc).connect({oracle.merge, 0}, {oracle.output, 0});
    return oracle;
}

// Test-only merge implementation with the A/B roles exchanged. With the
// oracle's distinct channels and alphas it must exceed the declared merge
// tolerance for EVERY operation, so role preservation is enforced by
// comparison rather than assumed.
constexpr const char* kSwappedOperationsMerge = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_a;
layout(set = 1, binding = 1) restrict readonly uniform image2D in_b;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    vec4 bg = gpuLoadRgba(in_b, ivec2(p), inputGeometry[1].rgba, int(inputGeometry[1].extent.y),
                          inputGeometry[1].channels.y);  // WRONG: A/B roles exchanged
    vec4 fg = gpuLoadRgba(in_a, ivec2(p), inputGeometry[0].rgba, int(inputGeometry[0].extent.y),
                          inputGeometry[0].channels.y);
    int operation = int(op.x);
    vec4 composite;
    if (operation == 0) {
        composite.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    } else {
        vec3 target;
        if (operation == 1) { target = bg.xyz + fg.xyz; }
        else if (operation == 2) { target = bg.xyz * fg.xyz; }
        else if (operation == 3) { target = vec3(1.0) - (vec3(1.0) - bg.xyz) * (vec3(1.0) - fg.xyz); }
        else { target = abs(bg.xyz - fg.xyz); }
        composite.xyz = bg.xyz + fg.a * (target - bg.xyz);
    }
    composite.w = fg.a + (1.0 - fg.a) * bg.a;
    gpuStoreRgba(out_color, ivec2(p), int(meta2.y), channels.y, rgba, composite);
}
)GLSL";

}  // namespace

TEST(Effect, MergeOperationsMatchCpuReferenceOnBothFrontEnds) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    // The fixture values are the issue's independent oracle, so each front
    // end is checked against the hand-computed pixel as well as the CPU.
    struct Case {
        const char* operation;
        std::array<float, 4> expected;
    };
    const Case cases[] = {
        {"over", {0.5F, 0.45F, 0.425F, 0.7F}},       {"plus", {0.6F, 0.65F, 0.725F, 0.7F}},
        {"multiply", {0.18F, 0.3F, 0.375F, 0.7F}},   {"screen", {0.52F, 0.55F, 0.65F, 0.7F}},
        {"difference", {0.4F, 0.25F, 0.475F, 0.7F}},
    };
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    for (const Case& expected : cases) {
        MergeOracle oracle = makeMergeOracle();
        rootGraph(oracle.doc).setParam(oracle.merge, "operation", ParameterValue{ChoiceValue{expected.operation}});
        const EvaluationRequest request = requestFor(oracle.doc, {0, 0, 8, 4}, 0);

        // Over keeps the declared 2e-7 bar (it is the same expression as
        // before); the extended operations use the wider FMA budget above.
        const float tolerance = std::string(expected.operation) == "over" ? kMergeTolerance : kTolerance;
        const CpuImage cpuImage = evaluateCpuImage(oracle.doc, request);
        eval::GpuEvaluation slangEval = evaluateGpu(oracle.doc, request, slang, *boot.device, *boot.allocator);
        const CpuImage slangImage = slangEval.readBack(request.output, *boot.device, *boot.allocator);
        expectImagesClose(cpuImage, slangImage, tolerance, expected.operation);
        eval::GpuEvaluation glslEval = evaluateGpu(oracle.doc, request, glsl, *boot.device, *boot.allocator);
        const CpuImage glslImage = glslEval.readBack(request.output, *boot.device, *boot.allocator);
        expectImagesClose(cpuImage, glslImage, tolerance, expected.operation);
        expectImagesClose(slangImage, glslImage, tolerance, expected.operation);

        const std::array<float, 4> pixel = slangImage.pixel(0, 0);
        for (std::size_t channel = 0; channel < 4; ++channel) {
            EXPECT_NEAR(pixel[channel], expected.expected[channel], tolerance)
                << expected.operation << " channel " << channel;
        }
    }
    expectValidationClean(*boot.instance);
}

TEST(Effect, MergeHdrAndNegativeValuesSurviveBothFrontEnds) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    MergeOracle oracle = makeMergeOracle();
    rootGraph(oracle.doc).setParam(oracle.background, "color", ColorValue{{-0.5F, 2.0F, -2.0F, 0.5F}});
    rootGraph(oracle.doc).setParam(oracle.foreground, "color", ColorValue{{2.5F, -1.0F, 0.5F, 0.25F}});
    rootGraph(oracle.doc).setParam(oracle.merge, "operation", ParameterValue{ChoiceValue{"plus"}});
    const EvaluationRequest request = requestFor(oracle.doc, {0, 0, 8, 4}, 0);

    const CpuImage cpuImage = evaluateCpuImage(oracle.doc, request);
    eval::GpuEvaluation slangEval = evaluateGpu(oracle.doc, request, slang, *boot.device, *boot.allocator);
    const CpuImage slangImage = slangEval.readBack(request.output, *boot.device, *boot.allocator);
    expectImagesClose(cpuImage, slangImage, kTolerance, "hdr plus slang vs cpu");
    eval::GpuEvaluation glslEval = evaluateGpu(oracle.doc, request, glsl, *boot.device, *boot.allocator);
    const CpuImage glslImage = glslEval.readBack(request.output, *boot.device, *boot.allocator);
    expectImagesClose(cpuImage, glslImage, kTolerance, "hdr plus glsl vs cpu");

    // Plus interpolates background -> bg+fg by foreground alpha: the HDR
    // green (1.75) and the negative blue (-1.875) are stored, not clamped.
    const std::array<float, 4> pixel = slangImage.pixel(0, 0);
    EXPECT_NEAR(pixel[0], 0.125F, kTolerance);
    EXPECT_GT(pixel[1], 1.0F) << "HDR scene-linear value was clamped";
    EXPECT_LT(pixel[2], 0.0F) << "negative scene-linear value was clamped";
    EXPECT_NEAR(pixel[3], 0.625F, kTolerance);
    expectValidationClean(*boot.instance);
}

TEST(Effect, MergeMaskInvertAndMixMatchCpuReference) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    MergeOracle oracle = makeMergeOracle();
    (void)rootGraph(oracle.doc).connect({oracle.mask, 0}, {oracle.merge, 2});
    rootGraph(oracle.doc).setParam(oracle.merge, "mix", ParameterValue{0.5});
    const EvaluationRequest request = requestFor(oracle.doc, {0, 0, 8, 4}, 0);

    const auto expectParity = [&](const char* what) {
        const CpuImage cpuImage = evaluateCpuImage(oracle.doc, request);
        eval::GpuEvaluation slangEval = evaluateGpu(oracle.doc, request, slang, *boot.device, *boot.allocator);
        const CpuImage slangImage = slangEval.readBack(request.output, *boot.device, *boot.allocator);
        expectImagesClose(cpuImage, slangImage, kTolerance, what);
        eval::GpuEvaluation glslEval = evaluateGpu(oracle.doc, request, glsl, *boot.device, *boot.allocator);
        const CpuImage glslImage = glslEval.readBack(request.output, *boot.device, *boot.allocator);
        expectImagesClose(cpuImage, glslImage, kTolerance, what);
    };

    // Fractional coverage: mask A = 0.3 with Mix 0.5, the hand-computed
    // masked Over (0.245, 0.4075, 0.57375, 0.445).
    expectParity("masked mix 0.5");
    const std::array<float, 4> masked = evaluateCpuImage(oracle.doc, request).pixel(0, 0);
    EXPECT_NEAR(masked[0], 0.245F, 1e-6F);
    EXPECT_NEAR(masked[3], 0.445F, 1e-6F);

    rootGraph(oracle.doc).setParam(oracle.merge, "invertMask", ParameterValue{true});
    expectParity("inverted mask");
    rootGraph(oracle.doc).setParam(oracle.merge, "invertMask", ParameterValue{false});
    rootGraph(oracle.doc).setParam(oracle.merge, "maskChannel", ParameterValue{ChoiceValue{"none"}});
    expectParity("mask channel none");

    // Zero Mix returns the background exactly on both executors.
    rootGraph(oracle.doc).setParam(oracle.merge, "mix", ParameterValue{0.0});
    expectParity("mix zero");
    const CpuImage zero = evaluateCpuImage(oracle.doc, request);
    EXPECT_EQ(zero.pixel(0, 0), (std::array<float, 4>{0.2F, 0.4F, 0.6F, 0.4F}));
    expectValidationClean(*boot.instance);
}

TEST(Effect, SwappedMergePortsFailEveryOperation) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const eval::EffectLibrary wrong = glslLibraryWithMerge(kSwappedOperationsMerge);
    for (const char* operation : {"over", "plus", "multiply", "screen", "difference"}) {
        MergeOracle oracle = makeMergeOracle();
        rootGraph(oracle.doc).setParam(oracle.merge, "operation", ParameterValue{ChoiceValue{operation}});
        const EvaluationRequest request = requestFor(oracle.doc, {0, 0, 8, 4}, 0);

        const CpuImage cpuImage = evaluateCpuImage(oracle.doc, request);
        eval::GpuEvaluation wrongEval = evaluateGpu(oracle.doc, request, wrong, *boot.device, *boot.allocator);
        const CpuImage wrongImage = wrongEval.readBack(request.output, *boot.device, *boot.allocator);
        EXPECT_GT(maxChannelDiff(cpuImage, wrongImage), kMergeTolerance)
            << "swapped A/B binding passed for " << operation;
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
    rootGraph(doc).setParam(tint1, "color", ColorValue{{1.0F, 0.5F, 0.25F, 0.5F}});
    const NodeId tint2 = rootGraph(doc).addNode("constcolor", "tint2");
    rootGraph(doc).setParam(tint2, "color", ColorValue{{0.25F, 0.5F, 2.0F, 0.75F}});
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
        EXPECT_EQ(error.node, rootGraph(composition.doc).nodeByName("over")->id);
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
    const auto missingDir = slangSpvDir() / "uninstalled-node-kernels";
    const auto unavailable = eval::loadSlangEffectLibrary(missingDir);
    const Composition composition = makeComposition();
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 16, 16}, 0);
    try {
        static_cast<void>(eval::queryViewerResultKey(composition.doc, request, unavailable));
        FAIL() << "a missing kernel must not admit cached output";
    } catch (const EvaluationException& error) {
        EXPECT_TRUE(error.hasNode());
        EXPECT_NE(std::string(error.what()).find(missingDir.string()), std::string::npos);
    }

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    auto contributions = eval::builtinGpuContributions();
    const auto merge = std::find_if(contributions.begin(), contributions.end(),
                                    [](const auto& entry) { return entry.node.descriptor.type == "merge"; });
    merge->gpu.reset();
    merge->gpuUnavailableReason = "test device has no Merge backend";
    const eval::EffectLibrary partial(std::move(contributions), eval::EffectBackend::Glsl);
    try {
        static_cast<void>(evaluateGpu(composition.doc, request, partial, *boot.device, *boot.allocator));
        FAIL() << "unavailable Merge must not produce substituted output";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, rootGraph(composition.doc).nodeByName("over")->id);
    }
    expectValidationClean(*boot.instance);
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

TEST(Effect, AnimatedParametersReachNativeGpuExecution) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Composition composition = makeComposition();
    const ParameterAddress address{composition.doc.rootNetworkId(), composition.tint, "color", kInvalidNetworkInstance};
    CommandStack stack(composition.doc);
    stack.push(setKeyframesCommand({
        KeyframeEdit{address, Keyframe{0, 0.0, ColorValue{{1.0F, 0.0F, 0.0F, 1.0F}}}},
        KeyframeEdit{address, Keyframe{0, 2.0, ColorValue{{0.0F, 0.0F, 1.0F, 1.0F}}}},
    }));

    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 16, 16}, 1);
    const CpuEvaluation cpu = evaluateCpu(composition.doc, request);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation gpu = evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator);
    const CpuImage gpuImage = gpu.readBack(request.output, *boot.device, *boot.allocator);
    expectImagesClose(cpu.image, gpuImage, kTolerance, "animated CPU vs native GPU");
    expectValidationClean(*boot.instance);
}

TEST(Effect, DroppedAsyncGraphRetainsResourcesUntilCompletion) {
    auto boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    auto composition = makeComposition();
    // A nonzero-size Blur after the merge (before Output) adds the retained
    // blur scratch and weight buffers to the submitted batch this test drops.
    const NodeId blur = rootGraph(composition.doc).addNode("blur", "blur");
    rootGraph(composition.doc).setParam(blur, "size", 2.0);
    const auto intoOutput = rootGraph(composition.doc).edgesInto(composition.output);
    ASSERT_EQ(intoOutput.size(), 1u);
    const NodeId merge = intoOutput.front().from.node;
    rootGraph(composition.doc).disconnect(intoOutput.front().id);
    (void)rootGraph(composition.doc).connect({merge, 0}, {blur, 0});
    (void)rootGraph(composition.doc).connect({blur, 0}, {composition.output, 0});
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

// ---------------------------------------------------------------------------
// Issue #34: Grade, Blur, and Transform native kernels across both front
// ends over one tiny shared fixture per effect. The connected mask on Grade
// also exercises the optional mask slot binding (absent masks bind no
// allocated fallback image).
// ---------------------------------------------------------------------------

namespace {

// Declared, operation-specific tolerances for the issue #34 inventory:
//   grade:     1e-5 — a handful of float multiply/add/signed-pow ops; the
//                      budget covers GPU pow/FMA rounding only.
//   blur:      2e-4 — separable Gaussian with float exp weights; GPU exp and
//                      tap accumulation differ from the CPU kernel.
//   transform: 2e-4 — linear/Catmull-Rom interpolation with negative lobes.
constexpr float kGradeTolerance34 = 1e-5F;
constexpr float kBlurTolerance34 = 2e-4F;
constexpr float kTransformTolerance34 = 2e-4F;

}  // namespace

TEST(Effect, NativeGradeBlurTransformMatchCpuReference) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const auto build = [](const char* type, ParameterValues params, bool withMask) {
        Document doc;
        rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
        doc.name = std::string{"native-"} + type;
        const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
        const NodeId effect = rootGraph(doc).addNode(type, "effect");
        const NodeId output = rootGraph(doc).addNode("output", "result");
        for (const auto& [key, value] : params)
            rootGraph(doc).setParam(effect, key, value);
        (void)rootGraph(doc).connect({plate, 0}, {effect, 0});
        if (withMask) {
            const NodeId mask = rootGraph(doc).addNode("constcolor", "mask");
            rootGraph(doc).setParam(mask, "color", ColorValue{{0.25F, 0.5F, 0.75F, 0.6F}});
            (void)rootGraph(doc).connect({mask, 0}, {effect, 1});
        }
        (void)rootGraph(doc).connect({effect, 0}, {output, 0});
        return doc;
    };

    struct Case {
        Document doc;
        const char* label;
        float tolerance;
    };
    const Case cases[] = {
        {build("grade",
               ParameterValues{{"gain", ColorValue{{1.5F, 0.75F, 1.25F, 1.0F}}},
                               {"offset", ColorValue{{-0.1F, 0.05F, 0.0F, 0.0F}}},
                               {"gamma", ColorValue{{1.2F, 1.0F, 2.0F, 1.0F}}},
                               {"clampBlack", false},
                               {"maskChannel", ChoiceValue{"G"}},
                               {"mix", 0.5}},
               true),
         "grade", kGradeTolerance34},
        {build("blur", ParameterValues{{"size", 4.0}, {"channels", ChoiceValue{"RGBA"}}}, false), "blur",
         kBlurTolerance34},
        {build("transform",
               ParameterValues{{"translateX", 1.5F},
                               {"translateY", -0.75F},
                               {"rotate", 12.0F},
                               {"scale", 1.25F},
                               {"filter", ChoiceValue{"Cubic"}}},
               false),
         "transform", kTransformTolerance34},
    };

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    for (const Case& testCase : cases) {
        const EvaluationRequest request = requestFor(testCase.doc, {0, 0, 24, 17}, 2);
        const CpuImage cpuImage = evaluateCpuImage(testCase.doc, request);
        eval::GpuEvaluation slangEval = evaluateGpu(testCase.doc, request, slang, *boot.device, *boot.allocator);
        expectImagesClose(cpuImage, slangEval.readBack(request.output, *boot.device, *boot.allocator),
                          testCase.tolerance, testCase.label);
        eval::GpuEvaluation glslEval = evaluateGpu(testCase.doc, request, glsl, *boot.device, *boot.allocator);
        expectImagesClose(cpuImage, glslEval.readBack(request.output, *boot.device, *boot.allocator),
                          testCase.tolerance, testCase.label);
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Issue #85: region evaluation. A region-limited request must produce exactly
// the full-frame result's rectangle, from both front ends, while the executor
// reports the coverage it really rendered (planned demand, halo-expanded
// inputs, resident backing rectangles) in plan.steps and returns the consumer's
// normalized rectangle.
// ---------------------------------------------------------------------------

namespace {

// Region request over an explicit full-image domain, so an ROI and the whole
// frame describe the same image.
[[nodiscard]] EvaluationRequest roiRequestFor(const Document& document, Region region, int fullWidth, int fullHeight,
                                              std::int64_t frame) {
    EvaluationRequest request = requestFor(document, region, frame);
    request.fullWidth = fullWidth;
    request.fullHeight = fullHeight;
    return request;
}

void expectRegionContains(Region outer, Region inner, const char* what) {
    EXPECT_LE(outer.x, inner.x) << what;
    EXPECT_LE(outer.y, inner.y) << what;
    EXPECT_GE(outer.x + outer.width, inner.x + inner.width) << what;
    EXPECT_GE(outer.y + outer.height, inner.y + inner.height) << what;
}

// Locates the step of one node type in a plan.
[[nodiscard]] const PlanStep* stepOfType(const EvaluationPlan& plan, std::string_view type) {
    for (const PlanStep& step : plan.steps)
        if (step.type == type)
            return &step;
    return nullptr;
}

// Every returned pixel, including the ROI boundary, must match the whole-frame
// render. Filter support is the planner's responsibility, not a tolerated seam.
void expectRegionMatchesFullFrame(const CpuImage& regionImage, const CpuImage& fullImage, Region region,
                                  float tolerance, const char* what) {
    ASSERT_EQ(regionImage.width(), region.width) << what;
    ASSERT_EQ(regionImage.height(), region.height) << what;
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            const auto expected = fullImage.pixel(region.x + x, region.y + y);
            const auto actual = regionImage.pixel(x, y);
            for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                EXPECT_NEAR(actual[channel], expected[channel], tolerance)
                    << what << ": region pixel (" << x << "," << y << ") channel " << channel;
            }
        }
    }
}

// plate -> effect -> output, the one-effect fixture the ROI cases share.
[[nodiscard]] Document makeEffectDocument(const char* type, ParameterValues params, bool withMask) {
    Document doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = std::string{"region-"} + type;
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    const NodeId effect = rootGraph(doc).addNode(type, "effect");
    const NodeId output = rootGraph(doc).addNode("output", "result");
    for (const auto& [key, value] : params)
        rootGraph(doc).setParam(effect, key, value);
    (void)rootGraph(doc).connect({plate, 0}, {effect, 0});
    if (withMask) {
        const NodeId mask = rootGraph(doc).addNode("constcolor", "mask");
        rootGraph(doc).setParam(mask, "color", ColorValue{{0.25F, 0.5F, 0.75F, 0.6F}});
        (void)rootGraph(doc).connect({mask, 0}, {effect, 1});
    }
    (void)rootGraph(doc).connect({effect, 0}, {output, 0});
    return doc;
}

}  // namespace

TEST(Effect, RegionRequestReturnsNormalizedRectangleAndMatchesFullFrame) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const Region domain{0, 0, 256, 192};
    const Region region{80, 64, 48, 32};
    const EvaluationRequest fullRequest = requestFor(composition.doc, domain, 2);
    const EvaluationRequest regionRequest = roiRequestFor(composition.doc, region, domain.width, domain.height, 2);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    const CpuImage fullImage = evaluateGpu(composition.doc, fullRequest, slang, *boot.device, *boot.allocator)
                                   .readBack(fullRequest.output, *boot.device, *boot.allocator);
    eval::GpuEvaluation regionEval = evaluateGpu(composition.doc, regionRequest, slang, *boot.device, *boot.allocator);
    const CpuImage regionImage = regionEval.readBack(regionRequest.output, *boot.device, *boot.allocator);
    const CpuImage glslImage = evaluateGpu(composition.doc, regionRequest, glsl, *boot.device, *boot.allocator)
                                   .readBack(regionRequest.output, *boot.device, *boot.allocator);

    // The caller receives the normalized request's rectangle, not the padded
    // backing rectangle the executor actually rendered.
    ASSERT_EQ(regionImage.width(), region.width);
    ASSERT_EQ(regionImage.height(), region.height);
    EXPECT_TRUE(regionEval.plan.request.region == region);
    expectRegionMatchesFullFrame(regionImage, fullImage, region, kTolerance, "slang region vs full frame");
    expectImagesClose(regionImage, glslImage, kTolerance, "region glsl vs slang");
    expectImagesClose(evaluateCpuImage(composition.doc, regionRequest), regionImage, kTolerance, "region slang vs cpu");

    // Every step reports coverage that contains the consumer's demand, and the
    // output step reports the normalized rectangle it delivered.
    for (const PlanStep& step : regionEval.plan.steps)
        expectRegionContains(step.region, region, "step coverage contains the consumer demand");
    const PlanStep* outputStep = stepOfType(regionEval.plan, "output");
    ASSERT_NE(outputStep, nullptr);
    EXPECT_TRUE(outputStep->region == region);
    expectValidationClean(*boot.instance);
}

TEST(Effect, BlurRegionRequestCarriesItsHaloAndMatchesFullFrame) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Document doc = makeEffectDocument("blur", ParameterValues{{"size", 4.0}, {"channels", ChoiceValue{"RGBA"}}}, false);
    const Region domain{0, 0, 256, 192};
    const Region region{80, 64, 48, 32};
    const EvaluationRequest fullRequest = requestFor(doc, domain, 1);
    const EvaluationRequest regionRequest = roiRequestFor(doc, region, domain.width, domain.height, 1);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    const CpuImage fullImage = evaluateGpu(doc, fullRequest, slang, *boot.device, *boot.allocator)
                                   .readBack(fullRequest.output, *boot.device, *boot.allocator);
    eval::GpuEvaluation regionEval = evaluateGpu(doc, regionRequest, slang, *boot.device, *boot.allocator);
    const CpuImage regionImage = regionEval.readBack(regionRequest.output, *boot.device, *boot.allocator);
    const CpuImage glslImage = evaluateGpu(doc, regionRequest, glsl, *boot.device, *boot.allocator)
                                   .readBack(regionRequest.output, *boot.device, *boot.allocator);

    // Clamp-to-edge filtering is exact: the region render reproduces the whole
    // frame's pixels including the border, because the taps a region pixel
    // needs were requested with it.
    expectRegionMatchesFullFrame(regionImage, fullImage, region, kBlurTolerance34, "blur region vs full frame");
    expectImagesClose(regionImage, glslImage, kBlurTolerance34, "blur region glsl vs slang");
    expectImagesClose(evaluateCpuImage(doc, regionRequest), regionImage, kBlurTolerance34, "blur region vs cpu");

    // The halo is real: the blur node's input coverage reaches ceil(size/scale)
    // pixels beyond the demand on every side, and it is anchored before the
    // node's own (block-padded) raster — the displacement the bound per-input
    // geometry exists for, in both blur passes.
    const PlanStep* plateStep = stepOfType(regionEval.plan, "testpattern");
    const PlanStep* blurStep = stepOfType(regionEval.plan, "blur");
    ASSERT_NE(plateStep, nullptr);
    ASSERT_NE(blurStep, nullptr);
    expectRegionContains(plateStep->region, Region{region.x - 4, region.y - 4, region.width + 8, region.height + 8},
                         "blur input carries the filter halo");
    EXPECT_LT(plateStep->region.x, blurStep->region.x)
        << "blur input is anchored before the blur raster (halo demanded beyond the node coverage)";
    EXPECT_LT(plateStep->region.y, blurStep->region.y) << "blur input is anchored above the blur raster";
    EXPECT_GT(plateStep->region.width, blurStep->region.width);
    expectValidationClean(*boot.instance);
}

TEST(Effect, TransformRegionRequestMatchesFullFrameAndCpu) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Document doc = makeEffectDocument("transform",
                                      ParameterValues{{"translateX", 1.5F},
                                                      {"translateY", -0.75F},
                                                      {"rotate", 12.0F},
                                                      {"scale", 1.25F},
                                                      {"filter", ChoiceValue{"Cubic"}},
                                                      {"maskChannel", ChoiceValue{"G"}},
                                                      {"mix", 0.5}},
                                      true);
    const Region domain{0, 0, 256, 192};
    const Region region{80, 64, 48, 32};
    const EvaluationRequest fullRequest = requestFor(doc, domain, 0);
    const EvaluationRequest regionRequest = roiRequestFor(doc, region, domain.width, domain.height, 0);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    const CpuImage fullImage = evaluateGpu(doc, fullRequest, slang, *boot.device, *boot.allocator)
                                   .readBack(fullRequest.output, *boot.device, *boot.allocator);
    eval::GpuEvaluation regionEval = evaluateGpu(doc, regionRequest, slang, *boot.device, *boot.allocator);
    const CpuImage regionImage = regionEval.readBack(regionRequest.output, *boot.device, *boot.allocator);
    const CpuImage glslImage = evaluateGpu(doc, regionRequest, glsl, *boot.device, *boot.allocator)
                                   .readBack(regionRequest.output, *boot.device, *boot.allocator);

    expectRegionMatchesFullFrame(regionImage, fullImage, region, kTransformTolerance34,
                                 "transform region vs full frame");
    expectImagesClose(regionImage, glslImage, kTransformTolerance34, "transform region glsl vs slang");
    expectImagesClose(evaluateCpuImage(doc, regionRequest), regionImage, kTransformTolerance34,
                      "transform region vs cpu");
    expectValidationClean(*boot.instance);
}

TEST(Effect, ReducedScaleBlurTransformRegionsPreserveSamplingAndBorders) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    Document doc = makeEffectDocument("blur", ParameterValues{{"size", 5.0}, {"mix", 0.7}}, true);
    auto& graph = rootGraph(doc);
    const NodeId blur = graph.nodeByName("effect")->id;
    const NodeId output = graph.nodeByName("result")->id;
    graph.disconnect(graph.edgesInto(output).front().id);
    const NodeId transform = graph.addNode("transform", "move");
    graph.setParam(transform, "rotate", 17.0);
    graph.setParam(transform, "translateX", 3.5);
    graph.setParam(transform, "translateY", -2.25);
    graph.setParam(transform, "mix", 0.6);
    graph.setParam(transform, "maskChannel", ChoiceValue{"G"});
    (void)graph.connect({blur, 0}, {transform, 0});
    (void)graph.connect({graph.nodeByName("mask")->id, 0}, {transform, 1});
    (void)graph.connect({transform, 0}, {output, 0});
    const auto slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const auto glsl = eval::glslEffectLibrary();
    for (const int scale : {2, 4}) {
        SCOPED_TRACE(scale);
        auto full = requestFor(doc, {0, 0, 1024, 640}, 0);
        full.samplingScale = scale;
        auto region = roiRequestFor(doc, {385, 257, 113, 73}, 1024, 640, 0);
        region.samplingScale = scale;
        const auto normalized = canonicalizeRequest(region);
        const CpuImage fullPixels = evaluateGpu(doc, full, slang, *boot.device, *boot.allocator)
                                        .readBack(output, *boot.device, *boot.allocator);
        const CpuImage regionPixels = evaluateGpu(doc, region, slang, *boot.device, *boot.allocator)
                                          .readBack(output, *boot.device, *boot.allocator);
        const Region raster{normalized.region.x / scale, normalized.region.y / scale,
                            scaledDimension(normalized.region.width, scale),
                            scaledDimension(normalized.region.height, scale)};
        expectRegionMatchesFullFrame(regionPixels, fullPixels, raster, kTransformTolerance34,
                                     "reduced-scale chain vs full frame");
        const CpuImage glslPixels = evaluateGpu(doc, region, glsl, *boot.device, *boot.allocator)
                                        .readBack(output, *boot.device, *boot.allocator);
        expectImagesClose(regionPixels, glslPixels, kTransformTolerance34, "reduced-scale chain frontends");
        expectImagesClose(evaluateCpuImage(doc, region), regionPixels, kTransformTolerance34,
                          "reduced-scale chain vs CPU");
    }
    expectValidationClean(*boot.instance);
}

TEST(Effect, WholeFrameOnlyContributionEscalatesRegionRequest) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    auto contributions = eval::builtinGpuContributions();
    contributions.push_back(test::affine::affineGpuContribution());
    contributions.push_back(test::affine::affineWholeFrameGpuContribution());
    std::vector<NodeContribution> declarations;
    declarations.reserve(contributions.size());
    for (auto& declaration : contributions)
        declarations.push_back(declaration.node);
    auto registry = std::make_shared<const NodeContributions>(declarations);

    const eval::EffectLibrary slang(std::move(contributions), eval::EffectBackend::Slang, slangSpvDir(), slangSrcDir());

    Document doc(registry->catalog());
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = "whole-frame-region";
    const NodeId plate = rootGraph(doc).addNode("constcolor", "plate");
    rootGraph(doc).setParam(plate, "color", ColorValue{{-1.5F, 4.0F, 0.5F, 1.0F}});
    const NodeId affine = rootGraph(doc).addNode(std::string(test::affine::kWholeFrameNodeType), "affine");
    rootGraph(doc).setParam(affine, "scale", ColorValue{{2.0F, -1.0F, 0.5F, 1.0F}});
    rootGraph(doc).setParam(affine, "offset", ColorValue{{0.25F, 0.5F, -0.5F, 0.0F}});
    const NodeId output = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({plate, 0}, {affine, 0});
    (void)rootGraph(doc).connect({affine, 0}, {output, 0});

    const Region domain{0, 0, 256, 192};
    CommandStack commands(doc);
    commands.push(setNetworkFormatCommand(doc.rootNetworkId(), ImageFormat{domain.width, domain.height, 1.0F}));
    const Region region{80, 64, 48, 32};
    const EvaluationRequest fullRequest = requestFor(doc, domain, 0);
    const EvaluationRequest regionRequest = roiRequestFor(doc, region, domain.width, domain.height, 0);

    eval::GpuEvaluation regionEval =
        evaluateGpu(doc, regionRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, nullptr);
    const CpuImage regionImage = regionEval.readBack(regionRequest.output, *boot.device, *boot.allocator);
    const CpuImage fullImage = evaluateGpu(doc, fullRequest, slang, *boot.device, *boot.allocator)
                                   .readBack(fullRequest.output, *boot.device, *boot.allocator);

    // The whole-frame-only node is escalated to the whole image domain — a
    // declared capability, not a node-name branch — and the consumer still
    // receives its own rectangle, cropped from that whole-domain result.
    const PlanStep* affineStep = stepOfType(regionEval.plan, "nemo.test.affineWholeFrame");
    ASSERT_NE(affineStep, nullptr);
    EXPECT_TRUE(affineStep->region == domain);
    ASSERT_EQ(regionImage.width(), region.width);
    ASSERT_EQ(regionImage.height(), region.height);
    expectRegionMatchesFullFrame(regionImage, fullImage, region, 1e-6F, "whole-frame region vs full frame");
    expectImagesClose(evaluateCpu(doc, regionRequest, nullptr, nullptr, registry).image, regionImage, 1e-6F,
                      "whole-frame region vs cpu");
    expectValidationClean(*boot.instance);
}

TEST(Effect, OverlappingRegionRequestsReuseTheResidentBackingAndKeepTheirKeys) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const Region domain{0, 0, 128, 96};
    const Region first{0, 0, 16, 16};
    const Region second{16, 0, 16, 16};
    const EvaluationRequest firstRequest = roiRequestFor(composition.doc, first, domain.width, domain.height, 0);
    const EvaluationRequest secondRequest = roiRequestFor(composition.doc, second, domain.width, domain.height, 0);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());

    // Cold: the second rectangle computed on its own, with no resident backing.
    ResultCache<eval::GpuNodeImage> cold;
    eval::GpuEvaluation coldEval =
        evaluateGpu(composition.doc, secondRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &cold);
    const CpuImage coldImage = coldEval.readBack(secondRequest.output, *boot.device, *boot.allocator);
    const std::string coldKey = coldEval.keys.at(secondRequest.output).canonical;

    // Warm: a padded backing rendered for the first rectangle covers the
    // second, so it is served in place — the dispatches are avoided.
    ResultCache<eval::GpuNodeImage> warm;
    eval::GpuEvaluation firstEval =
        evaluateGpu(composition.doc, firstRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &warm);
    const CacheCounts afterFirst = warm.counts();
    eval::GpuEvaluation secondEval =
        evaluateGpu(composition.doc, secondRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &warm);
    const CacheCounts afterSecond = warm.counts();
    const CpuImage secondImage = secondEval.readBack(secondRequest.output, *boot.device, *boot.allocator);

    EXPECT_GT(afterSecond.hits, afterFirst.hits);
    EXPECT_EQ(afterSecond.misses, afterFirst.misses);  // no recomputation
    for (const PlanStep& step : secondEval.plan.steps)
        EXPECT_TRUE(step.cacheReused) << "step " << step.name;
    // The served result is the same image, and its caller key addresses the
    // normalized request rather than the wider rectangle that backed it.
    expectImagesClose(coldImage, secondImage, kTolerance, "overlapping region reuse");
    EXPECT_EQ(secondEval.keys.at(secondRequest.output).canonical, coldKey);
    static_cast<void>(firstEval);
    expectValidationClean(*boot.instance);
}

TEST(Effect, ViewerResultKeyAgreesWithExecutedKeysForRegionAndFullRequests) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const Composition composition = makeComposition();
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    for (const Region region : {Region{0, 0, 32, 24}, Region{8, 4, 16, 12}}) {
        const EvaluationRequest request = roiRequestFor(composition.doc, region, 40, 28, 3);
        ResultCache<eval::GpuNodeImage> cache;
        eval::GpuEvaluation gpu =
            evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &cache);
        const ResultKey sceneLinear = gpu.keys.at(request.output);
        const ResultKey viewer = viewerResultKey(sceneLinear, composition.doc.color);
        const ResultKey queried = eval::queryViewerResultKey(composition.doc, request, slang);
        EXPECT_EQ(queried.canonical, viewer.canonical) << "region " << region.x << "," << region.y;
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Issue #85: a real decoded source under a region request. The source fill
// keeps FULL-resolution coordinate semantics, so the region render must equal
// the still's own pixels at the region's coordinates — an independent oracle,
// not a comparison with the other executor.
// ---------------------------------------------------------------------------

namespace {

// Temp directory for the still fixture, removed on scope exit.
struct StillScratchDir {
    std::filesystem::path path;
    StillScratchDir()
        : path(std::filesystem::temp_directory_path() /
               ("nemo-region-still-" + std::to_string(::getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name())) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~StillScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    [[nodiscard]] std::string file(const std::string& name) const { return (path / name).string(); }
};

// R separates x, G separates y, B separates the diagonal; every coefficient is
// a power of two, so the values are exact in float32 and equality is meaningful.
[[nodiscard]] std::array<float, 4> stillPatternPixel(int x, int y) {
    constexpr float kUnit = 0.0625F;
    return {(static_cast<float>(x) + 1.0F) * kUnit, (static_cast<float>(y) + 1.0F) * 2.0F * kUnit,
            (static_cast<float>(x + y) + 1.0F) * 0.5F * kUnit, 1.0F};
}

[[nodiscard]] CpuImage stillPatternFrame(int width, int height) {
    CpuImage image(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, stillPatternPixel(x, y));
    return image;
}

}  // namespace

TEST(Effect, SourceRegionRequestFillsFullResolutionCoordinates) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const StillScratchDir scratch;
    const std::string path = scratch.file("still.exr");
    constexpr int kWidth = 32;
    constexpr int kHeight = 24;
    media::writeImage(path, stillPatternFrame(kWidth, kHeight), media::OutputPrecision::Float32);

    Document doc;
    doc.name = "region-source";
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    CommandStack stack(doc);
    stack.push(setSourceCommand("plate", SourceReference{path}));
    const NodeId plate = rootGraph(doc).addNode("source", "plate");
    rootGraph(doc).setParam(plate, "source", std::string("plate"));
    const NodeId output = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({plate, 0}, {output, 0});

    const Region region{12, 8, 16, 12};
    const EvaluationRequest regionRequest = roiRequestFor(doc, region, kWidth, kHeight, 0);
    const EvaluationRequest fullRequest = requestFor(doc, Region{0, 0, kWidth, kHeight}, 0);

    // The still's written pattern is the oracle: at scale 1 over a same-size
    // frame the source fill is the identity map, so region pixel (x, y) is the
    // full-resolution sample (region.x + x, region.y + y).
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation regionEval =
        evaluateGpu(doc, regionRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
    const CpuImage regionImage = regionEval.readBack(regionRequest.output, *boot.device, *boot.allocator);
    const CpuImage fullImage =
        evaluateGpu(doc, fullRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources)
            .readBack(fullRequest.output, *boot.device, *boot.allocator);

    ASSERT_EQ(regionImage.width(), region.width);
    ASSERT_EQ(regionImage.height(), region.height);
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            const std::array<float, 4> expected = stillPatternPixel(region.x + x, region.y + y);
            for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                EXPECT_FLOAT_EQ(regionImage.pixel(x, y)[channel], expected[channel])
                    << "region pixel (" << x << "," << y << ") channel " << channel;
            }
        }
    }
    expectRegionMatchesFullFrame(regionImage, fullImage, region, 1e-6F, "source region vs full frame");

    // The CPU reference resolves the same source request through its own
    // provider, so both executors agree on the region raster.
    media::ImageSourceProvider provider;
    const CpuEvaluation cpu = evaluateCpu(doc, regionRequest, nullptr, &provider);
    ASSERT_EQ(cpu.image.width(), region.width);
    expectImagesClose(cpu.image, regionImage, 1e-6F, "source region cpu vs slang");
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Issue #90: named auxiliary planes through the native effects. The
// handcrafted multilayer fixture (docs/evidence/assets/issue90-channels) is the
// independent oracle: authored root RGBA 0.25/0.5/0.75/1, beauty.R/G/B 4/-5/6,
// depth.Z = 10*x+y, matte.coverage 0.125, motion.u/v -2/3, over an 8x8
// whole-frame data window. Both fixtures are real media served through the same
// source/session seam as the region test above, and every named value is
// asserted against those authored formulas through exact name lookup (never
// OIIO's channel ordering) rather than against another implementation.
//
// Merge takes its meaning and its preserved auxiliary planes from its main
// input — the composite's background, the primary "B" pipe of the referenced
// vocabulary. This fixture is wired there and the data-only fixture
// (A, depth.Z, matte.coverage; data window (2,2)-(5,5), so 100.25/0.625 inside
// it and transparent black outside) is wired to the foreground. Its smaller
// inventory, its missing primary RGB, and its colliding depth.Z/matte.coverage
// values are the negative controls: an implementation that took the
// foreground's values, its names, or an invented projection would fail every
// assertion below.
//
// Blur then re-uses the same composite as the separable filter's main input, so
// one region-limited request exercises the scratch pass, the final pass and the
// executor's shared preservation at once. The scratch covers exactly the
// request's columns and the input's rows, so a wrong scratch origin moves every
// auxiliary value; the depth ramp makes that a hard failure rather than a
// tolerance question.
// ---------------------------------------------------------------------------

namespace {

constexpr int kChannelFixtureWidth = 8;
constexpr int kChannelFixtureHeight = 8;

// The multilayer fixture's exact inventory (name lookup, never storage order).
const std::vector<std::string> kMultilayerChannels{
    "A", "B", "G", "R", "beauty.B", "beauty.G", "beauty.R", "depth.Z", "matte.coverage", "motion.u", "motion.v"};

// Stored plane index of one exactly named channel, resolved once outside any
// pixel loop (issue #90). A missing name fails here instead of reading a value
// the fixture never authored.
[[nodiscard]] int namedChannel(const CpuImage& image, std::string_view name) {
    const int index = channelIndex(image.layout().channels, name);
    EXPECT_GE(index, 0) << "image does not carry channel '" << name << "'";
    return index;
}

// The produced image names exactly the primary input's channels: none dropped,
// renamed, or manufactured from the other input's names.
void expectMultilayerInventory(const CpuImage& image, const char* what) {
    std::vector<std::string> names = image.layout().channels;
    std::vector<std::string> expected = kMultilayerChannels;
    std::sort(names.begin(), names.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(names, expected) << what << ": no dropped, renamed or manufactured named channels";
}

// The authored auxiliary formulas at ABSOLUTE image coordinates (the fixture's
// data window is the whole frame, so every sample is authored data and an exact
// pass-through is exact). Reaching this through a blurred, shifted or
// foreground-sourced value fails far outside float equality.
void expectMultilayerAuxiliaryPlanes(const CpuImage& image, Region region, const char* what) {
    const int beautyR = namedChannel(image, "beauty.R");
    const int beautyG = namedChannel(image, "beauty.G");
    const int beautyB = namedChannel(image, "beauty.B");
    const int depthZ = namedChannel(image, "depth.Z");
    const int matteCoverage = namedChannel(image, "matte.coverage");
    const int motionU = namedChannel(image, "motion.u");
    const int motionV = namedChannel(image, "motion.v");
    ASSERT_EQ(image.width(), region.width) << what;
    ASSERT_EQ(image.height(), region.height) << what;
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            const int imageX = region.x + x;
            const int imageY = region.y + y;
            const std::string at = std::string{what} + " at " + std::to_string(imageX) + "," + std::to_string(imageY);
            EXPECT_FLOAT_EQ(image.channel(x, y, beautyR), 4.0F) << "beauty.R " << at;
            EXPECT_FLOAT_EQ(image.channel(x, y, beautyG), -5.0F) << "beauty.G " << at;
            EXPECT_FLOAT_EQ(image.channel(x, y, beautyB), 6.0F) << "beauty.B " << at;
            EXPECT_FLOAT_EQ(image.channel(x, y, motionU), -2.0F) << "motion.u " << at;
            EXPECT_FLOAT_EQ(image.channel(x, y, motionV), 3.0F) << "motion.v " << at;
            EXPECT_FLOAT_EQ(image.channel(x, y, matteCoverage), 0.125F) << "matte.coverage " << at;
            EXPECT_FLOAT_EQ(image.channel(x, y, depthZ), static_cast<float>(10 * imageX + imageY)) << "depth.Z " << at;
        }
    }
}

// Independent oracle for the composite's RGBA projection. The background is the
// multilayer root RGBA (1.0 alpha) and the foreground is the data-only fixture:
// no primary RGB (its roles read 0, never an invented projection), stored alpha
// 0.625 inside its (2,2)-(5,5) data window and transparent black outside it
// (issue #88). Out.rgb = fg.a*fg.rgb + (1-fg.a)*bg.rgb, out.a = fg.a + (1-fg.a)*bg.a.
[[nodiscard]] std::array<float, 4> mergeOverOracle(int x, int y) {
    constexpr std::array<float, 4> kBackground{0.25F, 0.5F, 0.75F, 1.0F};
    const bool foregroundHoldsData = x >= 2 && x <= 5 && y >= 2 && y <= 5;
    const float foregroundAlpha = foregroundHoldsData ? 0.625F : 0.0F;
    return {(1.0F - foregroundAlpha) * kBackground[0], (1.0F - foregroundAlpha) * kBackground[1],
            (1.0F - foregroundAlpha) * kBackground[2], foregroundAlpha + (1.0F - foregroundAlpha) * kBackground[3]};
}

// The node identities of the two-source graph: the two real fixtures, their
// Merge and (optionally) the separable Blur over the composite.
struct NamedChannelGraph {
    NodeId merge = kInvalidNode;
    NodeId blur = kInvalidNode;
    NodeId output = kInvalidNode;
};

[[nodiscard]] NamedChannelGraph buildNamedChannelGraph(Document& doc, bool withBlur) {
    NamedChannelGraph graph;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = withBlur ? "named-channel-blur" : "named-channel-merge";
    CommandStack stack(doc);
    stack.push(setSourceCommand(
        "primary", SourceReference{(std::filesystem::path{NEMO_CHANNEL_FIXTURE_DIR} / "multilayer-b.exr").string()}));
    stack.push(setSourceCommand(
        "secondary", SourceReference{(std::filesystem::path{NEMO_CHANNEL_FIXTURE_DIR} / "data-a.exr").string()}));
    const NodeId primary = rootGraph(doc).addNode("source", "primary");
    rootGraph(doc).setParam(primary, "source", std::string{"primary"});
    rootGraph(doc).setParam(primary, "inputTransform", ChoiceValue{"raw"});
    const NodeId secondary = rootGraph(doc).addNode("source", "secondary");
    rootGraph(doc).setParam(secondary, "source", std::string{"secondary"});
    rootGraph(doc).setParam(secondary, "inputTransform", ChoiceValue{"raw"});
    graph.merge = rootGraph(doc).addNode("merge", "merge");
    graph.output = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({primary, 0}, {graph.merge, 0});
    (void)rootGraph(doc).connect({secondary, 0}, {graph.merge, 1});
    NodeId tail = graph.merge;
    if (withBlur) {
        graph.blur = rootGraph(doc).addNode("blur", "blur");
        rootGraph(doc).setParam(graph.blur, "size", 4.0);
        rootGraph(doc).setParam(graph.blur, "channels", ChoiceValue{"RGBA"});
        (void)rootGraph(doc).connect({graph.merge, 0}, {graph.blur, 0});
        tail = graph.blur;
    }
    (void)rootGraph(doc).connect({tail, 0}, {graph.output, 0});
    return graph;
}

}  // namespace

TEST(Effect, MergePreservesPrimaryNamedAuxiliaryChannels) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Document doc;
    const NamedChannelGraph graph = buildNamedChannelGraph(doc, /*withBlur=*/false);
    // The region spans both sides of the foreground's (2,2)-(5,5) data window, so
    // the composite, the preserved coordinates and the foreground's colliding
    // values share one read-back.
    const Region region{2, 3, 5, 4};
    const EvaluationRequest request = roiRequestFor(doc, region, kChannelFixtureWidth, kChannelFixtureHeight, 0);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    eval::GpuEvaluation native =
        evaluateGpu(doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
    const CpuImage image = native.readBack(graph.output, *boot.device, *boot.allocator);

    expectMultilayerInventory(image, "merge output");
    expectMultilayerAuxiliaryPlanes(image, region, "merge output");
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            const std::array<float, 4> expected = mergeOverOracle(region.x + x, region.y + y);
            for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                EXPECT_NEAR(image.pixel(x, y)[channel], expected[channel], kMergeTolerance)
                    << "merge pixel (" << region.x + x << "," << region.y + y << ") channel " << channel;
            }
        }
    }

    media::ImageSourceProvider provider;
    const CpuEvaluation cpu = evaluateCpu(doc, request, nullptr, &provider);
    expectImagesClose(cpu.image, image, kMergeTolerance, "merge composite cpu vs slang");
    // The CPU reference resolves the same authored formulas through its own
    // preservation rule, so neither executor's meaning is assumed from the other.
    expectMultilayerInventory(cpu.image, "merge cpu output");
    expectMultilayerAuxiliaryPlanes(cpu.image, region, "merge cpu output");
    expectValidationClean(*boot.instance);
}

TEST(Effect, SeparableBlurScratchPreservesNamedAuxiliaryChannelsWhileRgbaFilters) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Document doc;
    const NamedChannelGraph graph = buildNamedChannelGraph(doc, /*withBlur=*/true);
    // The same region as the merge case: a real ROI (the scratch pass covers
    // these columns and the input's rows, never the whole frame), inside the
    // filter's support of the image border so a blurred depth ramp cannot land
    // on the authored values.
    const Region region{2, 3, 5, 4};
    const EvaluationRequest request = roiRequestFor(doc, region, kChannelFixtureWidth, kChannelFixtureHeight, 0);

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    eval::GpuEvaluation native =
        evaluateGpu(doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
    const CpuImage image = native.readBack(graph.output, *boot.device, *boot.allocator);

    media::ImageSourceProvider provider;
    const CpuImage cpuImage = evaluateCpu(doc, request, nullptr, &provider).image;

    // The separable filter's scratch pass covers the request's columns and the
    // input's rows only, so a wrong scratch origin moves every auxiliary value:
    // the depth ramp turns that into a hard failure. The planes must not be
    // filtered either — clamp-to-edge would pull the ramp at this region's
    // borders — and the foreground's colliding values must not appear.
    expectMultilayerInventory(image, "blur output");
    expectMultilayerAuxiliaryPlanes(image, region, "blur output");

    // The composited RGBA still runs the declared effect math: the native filter
    // agrees with the CPU reference to the blur's declared tolerance.
    expectImagesClose(cpuImage, image, kBlurTolerance34, "blur cpu vs slang");
    // ... and that comparison is discriminating rather than a shared no-op: the
    // foreground's data window gives the composited RGB a step the filter must
    // smooth, so the filtered result leaves the unfiltered composite far beyond
    // that tolerance.
    float smoothed = 0.0F;
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            smoothed =
                std::max(smoothed, std::fabs(cpuImage.pixel(x, y)[0] - mergeOverOracle(region.x + x, region.y + y)[0]));
        }
    }
    EXPECT_GT(smoothed, 10.0F * kBlurTolerance34) << "the native blur must really filter the composited RGBA";
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Issue #98: packed four-channel storage. A native image whose description
// carries exactly four stored channels is ONE packed four-component image at
// the LOGICAL raster, whatever those channels are called and wherever they sit
// in storage; the SAME data with a fifth channel stays scalar planes. The
// authored fixture is the oracle: every value is asserted through exact
// channel-name lookup, never through a storage index, so an implementation that
// took component 0 for R from the count alone fails here — and the four- and
// five-channel runs must agree channel for channel, because the representation
// follows the stored channel COUNT and nothing else.
// ---------------------------------------------------------------------------

namespace {

constexpr int kPackedWidth = 12;
constexpr int kPackedHeight = 9;

// The authored four-channel fixture, stored in a deliberately noncanonical
// order: the identified roles resolve to R=2, G=1, B=0, A=3. Every sample is an
// exact power of two, so the only inexactness a comparison can see is the
// effect's own arithmetic.
const std::vector<std::string> kPackedChannels{"B", "G", "R", "A"};
constexpr std::array<float, 4> kPackedValues{6.0F, 4.0F, 2.0F, 0.25F};
constexpr float kPackedAuxiliary = 7.0F;

[[nodiscard]] CpuImage packedStill(bool withAuxiliaryChannel) {
    ImageLayout layout;
    layout.width = kPackedWidth;
    layout.height = kPackedHeight;
    layout.channels = kPackedChannels;
    if (withAuxiliaryChannel) {
        layout.channels.push_back("depth.Z");
    }
    CpuImage image(layout);
    for (int y = 0; y < kPackedHeight; ++y) {
        for (int x = 0; x < kPackedWidth; ++x) {
            for (std::size_t channel = 0; channel < kPackedValues.size(); ++channel) {
                image.setChannel(x, y, static_cast<int>(channel), kPackedValues[channel]);
            }
            if (withAuxiliaryChannel) {
                image.setChannel(x, y, static_cast<int>(kPackedValues.size()), kPackedAuxiliary);
            }
        }
    }
    return image;
}

// The channel inventory as a set of names: nothing dropped, renamed or
// manufactured by either representation.
void expectChannelInventory(const CpuImage& image, std::vector<std::string> expected, const char* what) {
    std::vector<std::string> names = image.layout().channels;
    std::sort(names.begin(), names.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(names, expected) << what << ": no dropped, renamed or manufactured channels";
}

[[nodiscard]] float namedValue(const CpuImage& image, std::string_view name, int x, int y) {
    const int index = channelIndex(image.layout().channels, name);
    EXPECT_GE(index, 0) << "image does not carry channel '" << name << "'";
    return index >= 0 ? image.channel(x, y, index) : 0.0F;
}

// source -> effect -> output over one authored still, through the same source
// session seam every real-media graph uses.
[[nodiscard]] Document packedEffectGraph(const std::string& stillPath, const char* type,
                                         const ParameterValues& params) {
    Document doc;
    rootGraph(doc).removeNode(rootGraph(doc).nodeByName("Output")->id);
    doc.name = std::string{"packed-"} + type;
    CommandStack stack(doc);
    stack.push(setSourceCommand("plate", SourceReference{stillPath}));
    const NodeId plate = rootGraph(doc).addNode("source", "plate");
    rootGraph(doc).setParam(plate, "source", std::string{"plate"});
    rootGraph(doc).setParam(plate, "inputTransform", ChoiceValue{"raw"});
    const NodeId effect = rootGraph(doc).addNode(type, "effect");
    for (const auto& [key, value] : params) {
        rootGraph(doc).setParam(effect, key, value);
    }
    const NodeId output = rootGraph(doc).addNode("output", "result");
    (void)rootGraph(doc).connect({plate, 0}, {effect, 0});
    (void)rootGraph(doc).connect({effect, 0}, {output, 0});
    return doc;
}

}  // namespace

TEST(Effect, PackedFourChannelStorageFollowsTheStoredChannelCount) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const StillScratchDir scratch;
    const std::string packedPath = scratch.file("packed-four.exr");
    const std::string planarPath = scratch.file("planar-five.exr");
    media::writeImage(packedPath, packedStill(false), media::OutputPrecision::Float32);
    media::writeImage(planarPath, packedStill(true), media::OutputPrecision::Float32);

    // An RGB-only grade at an exact weight: the identified RGB roles must be
    // read and written through their resolved channels, while alpha — and the
    // fifth channel of the other fixture — keep their authored samples.
    const ParameterValues params{{"multiply", ColorValue{{2.0F, 2.0F, 2.0F, 1.0F}}},
                                 {"channels", ChoiceValue{"RGB"}},
                                 {"mix", 0.5}};
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");

    struct Run {
        CpuImage native;
        CpuImage reference;
    };
    const auto evaluate = [&](const std::string& stillPath) {
        Document doc = packedEffectGraph(stillPath, "grade", params);
        const EvaluationRequest request = requestFor(doc, Region{0, 0, kPackedWidth, kPackedHeight}, 0);
        eval::GpuEvaluation gpu =
            evaluateGpu(doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
        media::ImageSourceProvider provider;
        return Run{gpu.readBack(request.output, *boot.device, *boot.allocator),
                   evaluateCpu(doc, request, nullptr, &provider).image};
    };
    const Run packed = evaluate(packedPath);
    const Run planar = evaluate(planarPath);

    // Four stored channels stay four, named and ordered as the media authored
    // them; a fifth channel stores the same four as scalar planes and keeps the
    // auxiliary one.
    expectChannelInventory(packed.native, kPackedChannels, "packed four-channel result");
    std::vector<std::string> planarExpected = kPackedChannels;
    planarExpected.push_back("depth.Z");
    expectChannelInventory(planar.native, planarExpected, "five-channel result");

    // The graded roles resolve through the channel names, not through storage
    // position: R=1.5*2, G=1.5*4, B=1.5*6, alpha untouched, depth.Z untouched.
    const std::vector<std::pair<std::string, float>> authored{{"B", kPackedValues[0]},
                                                              {"G", kPackedValues[1]},
                                                              {"R", kPackedValues[2]},
                                                              {"A", kPackedValues[3]}};
    for (int y = 0; y < kPackedHeight; ++y) {
        for (int x = 0; x < kPackedWidth; ++x) {
            const std::string at = " at " + std::to_string(x) + "," + std::to_string(y);
            for (const auto& [name, value] : authored) {
                const bool graded = name != "A";
                const float wanted = graded ? 1.5F * value : value;
                const float tolerance = graded ? kGradeTolerance34 : 0.0F;
                EXPECT_NEAR(namedValue(packed.native, name, x, y), wanted, tolerance) << "packed " << name << at;
                EXPECT_NEAR(namedValue(planar.native, name, x, y), wanted, tolerance)
                    << "scalar-plane " << name << at << " disagrees with the packed run";
            }
            EXPECT_FLOAT_EQ(namedValue(planar.native, "depth.Z", x, y), kPackedAuxiliary)
                << "the unselected auxiliary channel" << at;
        }
    }
    // Both front ends and the CPU reference agree on either storage.
    expectImagesClose(packed.reference, packed.native, kGradeTolerance34, "packed four-channel cpu vs slang");
    expectImagesClose(planar.reference, planar.native, kGradeTolerance34, "five-channel cpu vs slang");
    expectValidationClean(*boot.instance);
}

TEST(Effect, PackedFourChannelBlurRegionMatchesFullFrameAndCpuReference) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const StillScratchDir scratch;
    const std::string packedPath = scratch.file("packed-four.exr");
    media::writeImage(packedPath, packedStill(false), media::OutputPrecision::Float32);

    // The separable filter runs its scratch pass over the node's own four
    // channels, so the intermediate is packed as well, and the executor's crop
    // copies one packed region.
    const ParameterValues params{{"size", 3.0}, {"channels", ChoiceValue{"RGBA"}}};
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    Document doc = packedEffectGraph(packedPath, "blur", params);

    const Region region{3, 2, 6, 5};
    // The ROI and the whole frame describe the same image: the same explicit
    // full-resolution domain, so the comparison below is about the rectangle only.
    const EvaluationRequest request = roiRequestFor(doc, region, kPackedWidth, kPackedHeight, 0);
    const EvaluationRequest fullRequest =
        roiRequestFor(doc, Region{0, 0, kPackedWidth, kPackedHeight}, kPackedWidth, kPackedHeight, 0);
    eval::GpuEvaluation regionEval =
        evaluateGpu(doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
    const CpuImage regionImage = regionEval.readBack(request.output, *boot.device, *boot.allocator);
    const CpuImage fullImage =
        evaluateGpu(doc, fullRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources)
            .readBack(fullRequest.output, *boot.device, *boot.allocator);

    expectChannelInventory(regionImage, kPackedChannels, "packed blur region");
    expectRegionMatchesFullFrame(regionImage, fullImage, region, kBlurTolerance34, "packed blur region vs full frame");
    // The fixture is constant, so the filtered result is that same constant: the
    // authored values are an oracle the scratch pass, the tap loop and the crop
    // must all preserve.
    const std::vector<std::pair<std::string, float>> authored{{"B", kPackedValues[0]},
                                                              {"G", kPackedValues[1]},
                                                              {"R", kPackedValues[2]},
                                                              {"A", kPackedValues[3]}};
    for (int y = 0; y < region.height; ++y) {
        for (int x = 0; x < region.width; ++x) {
            for (const auto& [name, value] : authored) {
                EXPECT_NEAR(namedValue(regionImage, name, x, y), value, kBlurTolerance34)
                    << "packed blur " << name << " at " << x << "," << y;
            }
        }
    }
    media::ImageSourceProvider provider;
    expectImagesClose(evaluateCpu(doc, request, nullptr, &provider).image, regionImage, kBlurTolerance34,
                      "packed blur region cpu vs slang");

    // Reuse: the padded backing rendered for one rectangle serves an overlapping
    // one in place, with the packed crop still delivering the caller's own
    // rectangle.
    ResultCache<eval::GpuNodeImage> warm;
    const Region second{4, 3, 4, 4};
    const EvaluationRequest secondRequest = roiRequestFor(doc, second, kPackedWidth, kPackedHeight, 0);
    eval::GpuEvaluation firstEval =
        evaluateGpu(doc, secondRequest, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &warm, &sources);
    static_cast<void>(firstEval);
    const CacheCounts afterFirst = warm.counts();
    eval::GpuEvaluation reusedEval =
        evaluateGpu(doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, &warm, &sources);
    const CacheCounts afterReused = warm.counts();
    const CpuImage reusedImage = reusedEval.readBack(request.output, *boot.device, *boot.allocator);
    EXPECT_GT(afterReused.hits, afterFirst.hits);
    EXPECT_EQ(afterReused.misses, afterFirst.misses) << "an overlapping packed rectangle must not recompute";
    expectImagesClose(regionImage, reusedImage, 1e-6F, "packed blur reused region");
    expectValidationClean(*boot.instance);
}
