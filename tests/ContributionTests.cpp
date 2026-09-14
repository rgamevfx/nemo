// Contribution extension proof (issue #83).
//
// The primary seam is the REAL public project workflow: a test-only affine RGB
// contribution is registered through the same production NodeContributions /
// EffectLibrary builders the built-ins use, discovered and edited through
// ProjectSession commands inside a Composition Network, persisted and reopened
// through ProjectFile with the same catalog, and rendered by the public CPU and
// GPU evaluators against independently hand-derived pixels.
//
// The narrow second seam is immutable assembly: invalid declarations are
// rejected atomically before any snapshot is published, while a legitimately
// unavailable backend (a kernel this build never compiled) reports the
// offending node instead of substituting output and never blocks unrelated
// rendering.
//
// The expected pixels are hand-derived constants, not another executor's
// output: the CPU and Slang algorithms are authored independently and agree
// only as evidence.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "contributions/Affine.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"

using namespace nemo;

namespace {

namespace fs = std::filesystem;

// --- independently hand-derived expectation --------------------------------
// input (straight alpha) : (-2, 8, 0.5, 0.25)
// scale / offset          : (2, 3, 0.5, 9) / (0.5, -1, 4, 7)
// out.rgb = scale.rgb * in.rgb + offset.rgb, out.a = in.a exactly. The alpha
// components of scale/offset are deliberately nonzero to prove they are
// ignored; a clamping or premultiplied result cannot equal these constants.
constexpr std::array<float, 4> kInputColor{-2.0F, 8.0F, 0.5F, 0.25F};
constexpr std::array<float, 4> kScale{2.0F, 3.0F, 0.5F, 9.0F};
constexpr std::array<float, 4> kOffset{0.5F, -1.0F, 4.0F, 7.0F};
constexpr std::array<float, 4> kExpected{-3.5F, 23.0F, 4.25F, 0.25F};

const std::string kAffineType{test::affine::kNodeType};

// --- shared assembly helpers ------------------------------------------------

std::vector<eval::GpuNodeContribution> affineGpuContributions() {
    auto contributions = eval::builtinGpuContributions();
    contributions.push_back(test::affine::affineGpuContribution());
    return contributions;
}

std::vector<NodeContribution> affineContributions() {
    auto declarations = affineGpuContributions();
    std::vector<NodeContribution> contributions;
    contributions.reserve(declarations.size());
    for (auto& declaration : declarations)
        contributions.push_back(std::move(declaration.node));
    return contributions;
}

std::shared_ptr<const NodeContributions> affineRegistry() {
    return std::make_shared<const NodeContributions>(affineContributions());
}

// --- document/request helpers (public graph + evaluation seams) -------------

Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

EvaluationRequest requestFor(const Document& document, NodeId output) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = output;
    request.region = {0, 0, 1, 1};
    return request;
}

// constcolor(known negative/HDR value) -> affine -> Output.
struct AffineChain {
    Document document;
    NodeId affine{kInvalidNode};
    NodeId output{kInvalidNode};
};

[[nodiscard]] AffineChain makeAffineChain(const std::shared_ptr<const NodeContributions>& registry) {
    AffineChain chain{Document(registry->catalog()), kInvalidNode, kInvalidNode};
    Graph& graph = rootGraph(chain.document);
    chain.output = graph.nodeByName("Output")->id;
    const NodeId plate = graph.addNode("constcolor", "plate");
    graph.setParam(plate, "color", ColorValue{kInputColor});
    chain.affine = graph.addNode(kAffineType, "Affine");
    graph.setParam(chain.affine, "scale", ColorValue{kScale});
    graph.setParam(chain.affine, "offset", ColorValue{kOffset});
    static_cast<void>(graph.connect({plate, 0}, {chain.affine, 0}));
    static_cast<void>(graph.connect({chain.affine, 0}, {chain.output, 0}));
    return chain;
}

void expectPixel(const CpuImage& image, const std::array<float, 4>& expected, const char* what) {
    ASSERT_EQ(image.width(), 1) << what;
    ASSERT_EQ(image.height(), 1) << what;
    const std::array<float, 4> pixel = image.pixel(0, 0);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_FLOAT_EQ(pixel[channel], expected[channel]) << what << " channel " << channel;
    }
}

// --- GPU bootstrap (same pattern as GpuEffectTests) -------------------------

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

// The shared compiled-kernel directory (empty for a CPU-only configuration).
#if defined(NEMO_SLANG_SPV_DIR)
[[nodiscard]] fs::path slangSpvDir() {
    return NEMO_SLANG_SPV_DIR;
}
[[nodiscard]] fs::path slangSrcDir() {
    return NEMO_SLANG_SRC_DIR;
}
#else
[[nodiscard]] fs::path slangSpvDir() {
    return {};
}
[[nodiscard]] fs::path slangSrcDir() {
    return {};
}
#endif

class ContributionTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code error;
        dir_ = fs::temp_directory_path(error) / ("nemo-contribution-" + std::to_string(static_cast<long>(::getpid())) +
                                                 "-" + std::to_string(counter_++));
        fs::remove_all(dir_, error);
        ASSERT_TRUE(fs::create_directories(dir_, error) || !error);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(dir_, error);
    }

    fs::path dir_;

private:
    static int counter_;
};

int ContributionTest::counter_ = 0;

}  // namespace

// ---------------------------------------------------------------------------
// Primary seam: real ProjectSession create/connect/edit/undo/redo, ProjectFile
// save/reopen with the same catalog, and CPU rendering of hand-derived pixels.
// ---------------------------------------------------------------------------
TEST_F(ContributionTest, AffineContributionFlowsThroughProjectSessionAndPersistence) {
    const auto registry = affineRegistry();

    // The test-only identity is never part of the shipped inventory.
    EXPECT_EQ(builtinNodeCatalogPtr()->find(kAffineType), nullptr);

    ProjectSession session(Document(registry->catalog()));
    const NetworkId network = session.document().rootNetworkId();
    const NodeId output = session.document().network(network).graph().nodeByName("Output")->id;

    auto createdAffine = std::make_shared<NodeId>();
    ASSERT_TRUE(
        session
            .submit(addNodeCommand(network, kAffineType, "Affine", createdAffine), EditOptions{session.revision(), {}})
            .committed);
    const NodeId affine = *createdAffine;
    auto createdPlate = std::make_shared<NodeId>();
    ASSERT_TRUE(
        session
            .submit(addNodeCommand(network, "constcolor", "plate", createdPlate), EditOptions{session.revision(), {}})
            .committed);
    const NodeId plate = *createdPlate;
    ASSERT_TRUE(session
                    .submit(setParamCommand(network, plate, "color", ParameterValue{ColorValue{kInputColor}}),
                            EditOptions{session.revision(), {}})
                    .committed);
    ASSERT_TRUE(session.submit(connectCommand(network, {plate, 0}, {affine, 0}), EditOptions{session.revision(), {}})
                    .committed);
    ASSERT_TRUE(session.submit(connectCommand(network, {affine, 0}, {output, 0}), EditOptions{session.revision(), {}})
                    .committed);

    // Typed ProjectSession discovery sees the contributed type.
    bool discovered = false;
    for (const NodeQueryResult& node : session.queryNodes(network, "affine")) {
        discovered = discovered || (node.id == affine && node.type == kAffineType);
    }
    EXPECT_TRUE(discovered);

    // Defaults (scale 1, offset 0) are an exact identity mapping.
    expectPixel(
        evaluateCpu(session.document(), requestFor(session.document(), output), nullptr, nullptr, registry).image,
        kInputColor, "defaults");

    // Parameter edit through commands...
    ASSERT_TRUE(session
                    .submit(setParamCommand(network, affine, "scale", ParameterValue{ColorValue{kScale}}),
                            EditOptions{session.revision(), {}})
                    .committed);
    ASSERT_TRUE(session
                    .submit(setParamCommand(network, affine, "offset", ParameterValue{ColorValue{kOffset}}),
                            EditOptions{session.revision(), {}})
                    .committed);
    // ...discoverable as typed values...
    const std::vector<ValueQueryResult> values = session.queryValues(network, affine, "scale");
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values.front().value, ParameterValue{ColorValue{kScale}});
    // ...and observable in the rendered image.
    expectPixel(
        evaluateCpu(session.document(), requestFor(session.document(), output), nullptr, nullptr, registry).image,
        kExpected, "edited scale/offset");

    // Undo restores the authored defaults (both edits), redo restores them.
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    expectPixel(
        evaluateCpu(session.document(), requestFor(session.document(), output), nullptr, nullptr, registry).image,
        kInputColor, "undone edits");
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    expectPixel(
        evaluateCpu(session.document(), requestFor(session.document(), output), nullptr, nullptr, registry).image,
        kExpected, "redone edits");

    // Save through the public file layer...
    const fs::path target = dir_ / "affine.nemo";
    const ProjectWriteRequest job = session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(job, written).committed);

    // ...and reopen with the same catalog: identity, type and values survive.
    ProjectReadResult read = ProjectFile::read(target, registry->catalog());
    ASSERT_TRUE(read.ok) << read.error.message;
    const NodeInstance* loaded = read.document.network(network).graph().nodeByName("Affine");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->type, kAffineType);
    EXPECT_EQ(loaded->id, affine);
    ASSERT_TRUE(loaded->params.contains("scale"));
    EXPECT_EQ(loaded->params.at("scale"), ParameterValue{ColorValue{kScale}});
    EXPECT_EQ(loaded->params.at("offset"), ParameterValue{ColorValue{kOffset}});

    ProjectSession reopened(Document(registry->catalog()));
    ASSERT_TRUE(reopened.open(std::move(read)).replaced);
    const NodeId reopenedAffine = reopened.document().network(network).graph().nodeByName("Affine")->id;
    const NodeId reopenedOutput = reopened.document().network(network).defaultOutput();
    ASSERT_NE(reopenedAffine, kInvalidNode);
    const auto reopenedRequest = requestFor(reopened.document(), reopenedOutput);
    expectPixel(evaluateCpu(reopened.document(), reopenedRequest, nullptr, nullptr, registry).image, kExpected,
                "reopened project");

    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang compilation unavailable; CPU workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary library(affineGpuContributions(), eval::EffectBackend::Slang, slangSpvDir(),
                                      slangSrcDir());
    auto rendered = eval::evaluateGpu(reopened.document(), reopenedRequest, library, *boot.device, *boot.allocator);
    expectPixel(rendered.readBack(reopenedOutput, *boot.device, *boot.allocator), kExpected,
                "native rendering after reopen");
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Narrow seam: invalid contributions are rejected before publication, and the
// previously published immutable snapshot is untouched.
// ---------------------------------------------------------------------------
TEST_F(ContributionTest, ImmutableContributionAssemblyRejectsInvalidDeclarationsAtomically) {
    const auto expectRejected = [](std::vector<NodeContribution> contributions, const char* relationship) {
        std::shared_ptr<const NodeContributions> candidate;
        try {
            candidate = std::make_shared<const NodeContributions>(std::move(contributions));
            ADD_FAILURE() << "invalid contribution assembly was published";
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(kAffineType), std::string::npos);
            EXPECT_NE(std::string(error.what()).find(relationship), std::string::npos);
        }
        EXPECT_EQ(candidate, nullptr);
    };

    {
        // Duplicate node identity.
        auto contributions = affineContributions();
        contributions.push_back(test::affine::affineContribution());
        expectRejected(std::move(contributions), "duplicate");
    }
    {
        // A promised CPU path with no implementation callback and no reason.
        auto contributions = builtinContributions();
        NodeContribution promised = test::affine::affineContribution();
        promised.cpu.reset();
        promised.cpuUnavailableReason.clear();
        contributions.push_back(std::move(promised));
        expectRejected(std::move(contributions), "CPU");
    }
}

TEST_F(ContributionTest, EffectAssemblyRejectsInvalidAffineGpuDeclarationsAtomically) {
    const auto expectRejected = [](std::vector<eval::GpuNodeContribution> contributions, const char* relationship) {
        std::optional<eval::EffectLibrary> candidate;
        try {
            candidate.emplace(std::move(contributions), eval::EffectBackend::Slang);
            ADD_FAILURE() << "invalid GPU assembly was published";
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(kAffineType), std::string::npos);
            EXPECT_NE(std::string(error.what()).find(relationship), std::string::npos);
        }
        EXPECT_FALSE(candidate.has_value());
    };

    {
        // Implementation version conflicts with the schema version.
        auto contributions = affineGpuContributions();
        contributions.back().gpu->version = test::affine::kImplementationVersion + 1;
        expectRejected(std::move(contributions), "version");
    }
    {
        // Payload size is not a 16-byte multiple (incompatible with set 0/1).
        auto contributions = affineGpuContributions();
        contributions.back().gpu->payloadSize = sizeof(float) * 2;
        expectRejected(std::move(contributions), "payload");
    }
    {
        // A local pass reads a descriptor input port that does not exist.
        auto contributions = affineGpuContributions();
        contributions.back().gpu->passes[0].inputs = {eval::EffectImageRef{eval::EffectImageKind::Input, 7}};
        expectRejected(std::move(contributions), "input port");
    }
    {
        // A local pass reads scratch no earlier pass produced.
        auto contributions = affineGpuContributions();
        contributions.back().gpu->passes[0].inputs = {eval::EffectImageRef{eval::EffectImageKind::Scratch, 0}};
        expectRejected(std::move(contributions), "scratch");
    }
    {
        // A promised native backend carries neither implementation nor reason.
        auto contributions = affineGpuContributions();
        contributions.back().gpu.reset();
        contributions.back().gpuUnavailableReason.clear();
        expectRejected(std::move(contributions), "implementation");
    }
}

// ---------------------------------------------------------------------------
// Legitimate unavailability: a kernel this build never compiled reports the
// offending node and does not prevent unrelated rendering.
// ---------------------------------------------------------------------------
TEST_F(ContributionTest, UnavailableAffineKernelDoesNotBlockUnrelatedRendering) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    if (slangSpvDir().empty() || !fs::exists(slangSpvDir() / "constcolor.spv")) {
        GTEST_SKIP() << "no compiled built-in Slang kernels to render the unrelated chain";
    }

    const std::string missingKernel = "AffineKernelThatWasNeverCompiled";
    auto contributions = affineGpuContributions();
    contributions.back().gpu->passes[0].shader = missingKernel;
    const eval::EffectLibrary library(std::move(contributions), eval::EffectBackend::Slang, slangSpvDir(),
                                      slangSrcDir());

    const eval::RegisteredGpuEffect* registered = library.find(kAffineType);
    ASSERT_NE(registered, nullptr);
    EXPECT_FALSE(registered->unavailableReason.empty());
    EXPECT_NE(registered->unavailableReason.find(missingKernel), std::string::npos);

    // An unrelated chain still renders through the same library.
    const auto registry = library.contributions();
    Document unrelated(registry->catalog());
    Graph& graph = rootGraph(unrelated);
    const NodeId output = graph.nodeByName("Output")->id;
    const NodeId plate = graph.addNode("constcolor", "plate");
    graph.setParam(plate, "color", ColorValue{kInputColor});
    static_cast<void>(graph.connect({plate, 0}, {output, 0}));
    const EvaluationRequest request = requestFor(unrelated, output);
    eval::GpuEvaluation rendered = evaluateGpu(unrelated, request, library, *boot.device, *boot.allocator);
    const CpuImage image = rendered.readBack(output, *boot.device, *boot.allocator);
    ASSERT_EQ(image.width(), 1);
    ASSERT_EQ(image.height(), 1);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_NEAR(image.pixel(0, 0)[channel], kInputColor[channel], 1e-6F) << "unrelated channel " << channel;
    }

    // The unavailable node reports itself; no substituted output is produced.
    const AffineChain chain = makeAffineChain(registry);
    bool threw = false;
    try {
        static_cast<void>(evaluateGpu(chain.document, requestFor(chain.document, chain.output), library, *boot.device,
                                      *boot.allocator));
    } catch (const EvaluationException& error) {
        threw = true;
        EXPECT_EQ(error.node, chain.affine);
        EXPECT_NE(std::string(error.what()).find(missingKernel), std::string::npos);
    }
    EXPECT_TRUE(threw);
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Reuse edge: an identical evaluation reuses the cached affine result, and a
// changed implementation version is a different identity that must NOT serve
// the previous version's cached image.
// ---------------------------------------------------------------------------
TEST_F(ContributionTest, CpuReuseIsScopedToTheImplementationVersion) {
    const auto registry = affineRegistry();
    const AffineChain chain = makeAffineChain(registry);
    const EvaluationRequest request = requestFor(chain.document, chain.output);

    const auto stepFor = [](const CpuEvaluation& evaluation, NodeId node) -> const PlanStep* {
        for (const PlanStep& step : evaluation.plan.steps) {
            if (step.node == node) {
                return &step;
            }
        }
        return nullptr;
    };

    ResultCache<CpuImage> cache;
    const CpuEvaluation first = evaluateCpu(chain.document, request, &cache, nullptr, registry);
    const PlanStep* firstAffine = stepFor(first, chain.affine);
    ASSERT_NE(firstAffine, nullptr);
    EXPECT_FALSE(firstAffine->cacheReused);

    const CpuEvaluation reused = evaluateCpu(chain.document, request, &cache, nullptr, registry);
    const PlanStep* reusedAffine = stepFor(reused, chain.affine);
    ASSERT_NE(reusedAffine, nullptr);
    EXPECT_TRUE(reusedAffine->cacheReused);
    EXPECT_EQ(reusedAffine->produced.contentHash, firstAffine->produced.contentHash);
    expectPixel(reused.image, kExpected, "reused result");

    auto reordered = affineContributions();
    reordered.back().descriptor.displayName = "Renamed for discovery";
    std::reverse(reordered.begin(), reordered.end());
    const auto reorderedRegistry = std::make_shared<const NodeContributions>(std::move(reordered));
    const auto same = evaluateCpu(chain.document, request, &cache, nullptr, reorderedRegistry);
    ASSERT_NE(stepFor(same, chain.affine), nullptr);
    EXPECT_TRUE(stepFor(same, chain.affine)->cacheReused);
    expectPixel(same.image, kExpected, "labels and registration order are not image identity");

    // The same request under a new implementation version computes afresh: the
    // version is part of the result identity, so stale pixels cannot be served.
    auto upgradedContributions = builtinContributions();
    NodeContribution upgraded = test::affine::affineContribution();
    upgraded.descriptor.implementationVersion = test::affine::kImplementationVersion + 1;
    upgraded.cpu->version = test::affine::kImplementationVersion + 1;
    upgraded.cpu->execute = [execute = upgraded.cpu->execute](const CpuNodeContext& context) {
        auto image = execute(context);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                auto pixel = image.pixel(x, y);
                pixel[0] += 10.0F;
                image.setPixel(x, y, pixel);
            }
        }
        return image;
    };
    upgradedContributions.push_back(std::move(upgraded));
    const auto upgradedRegistry = std::make_shared<const NodeContributions>(std::move(upgradedContributions));
    try {
        static_cast<void>(evaluateCpu(chain.document, request, &cache, nullptr, upgradedRegistry));
        FAIL() << "a cached image must not bypass schema/implementation compatibility";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, chain.affine);
    }

    const AffineChain upgradedChain = makeAffineChain(upgradedRegistry);
    const CpuEvaluation upgradedEvaluation =
        evaluateCpu(upgradedChain.document, requestFor(upgradedChain.document, upgradedChain.output), &cache, nullptr,
                    upgradedRegistry);
    const PlanStep* upgradedAffine = stepFor(upgradedEvaluation, upgradedChain.affine);
    ASSERT_NE(upgradedAffine, nullptr);
    EXPECT_FALSE(upgradedAffine->cacheReused);
    expectPixel(upgradedEvaluation.image, {6.5F, 23.0F, 4.25F, 0.25F}, "changed implementation");
    const auto* plate = rootGraph(upgradedChain.document).nodeByName("plate");
    ASSERT_NE(stepFor(upgradedEvaluation, plate->id), nullptr);
    EXPECT_TRUE(stepFor(upgradedEvaluation, plate->id)->cacheReused);
}

TEST_F(ContributionTest, ActiveCpuRequestRetainsItsRegistration) {
    auto declarations = affineContributions();
    auto entered = std::make_shared<std::promise<void>>();
    auto enteredFuture = entered->get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    auto& implementation = *declarations.back().cpu;
    implementation.execute = [execute = implementation.execute, entered, released](const CpuNodeContext& context) {
        entered->set_value();
        released.wait();
        return execute(context);
    };
    auto registry = std::make_shared<const NodeContributions>(std::move(declarations));
    std::weak_ptr<const NodeContributions> observed = registry;
    const auto chain = makeAffineChain(registry);
    auto requestRegistry = registry;
    auto work = std::async(std::launch::async, [requestRegistry = std::move(requestRegistry), &chain]() mutable {
        return evaluateCpu(chain.document, requestFor(chain.document, chain.output), nullptr, nullptr,
                           std::move(requestRegistry));
    });
    EXPECT_EQ(enteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    registry.reset();
    EXPECT_FALSE(observed.expired());
    release.set_value();
    expectPixel(work.get().image, kExpected, "request outlives the application registration");
    EXPECT_TRUE(observed.expired());
}

TEST_F(ContributionTest, NativeSubmissionRetainsRegistrationUntilCompletion) {
    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang compilation unavailable";
    auto boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    auto& queue = boot.device->submissions(boot.device->graphics_family());
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    VkSemaphore gate{};
    gpu::checkVulkan(vkCreateSemaphore(boot.device->handle(), &info, nullptr, &gate), "vkCreateSemaphore");
    const auto release = [&] {
        VkSemaphoreSignalInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signal.semaphore = gate;
        signal.value = 1;
        gpu::checkVulkan(vkSignalSemaphore(boot.device->handle(), &signal), "vkSignalSemaphore");
        queue.drain();
        vkDestroySemaphore(boot.device->handle(), gate, nullptr);
    };
    try {
        gpu::SubmissionQueue::TimelineSemaphores waits;
        waits.wait = {gate};
        waits.waitValues = {1};
        const auto blocked = queue.submit([](VkCommandBuffer) {}, {}, waits);
        if (!blocked)
            throw std::runtime_error("could not establish the native completion gate");

        std::optional<eval::GpuEvaluation> pending;
        std::weak_ptr<const void> observed;
        NodeId output{};
        {
            const eval::EffectLibrary library(affineGpuContributions(), eval::EffectBackend::Slang, slangSpvDir(),
                                              slangSrcDir());
            observed = library.retain();
            const auto chain = makeAffineChain(library.contributions());
            output = chain.output;
            pending = eval::submitGpu(chain.document, requestFor(chain.document, output), library, *boot.device,
                                      *boot.allocator);
        }
        EXPECT_TRUE(pending.has_value());
        EXPECT_FALSE(observed.expired());
        EXPECT_FALSE(queue.poll(pending.value().completion.value()));
        release();
        gate = VK_NULL_HANDLE;
        EXPECT_TRUE(observed.expired());
        expectPixel(pending->readBack(output, *boot.device, *boot.allocator), kExpected,
                    "submission survives application registration destruction");
    } catch (...) {
        if (gate != VK_NULL_HANDLE)
            release();
        throw;
    }
    expectValidationClean(*boot.instance);
}
