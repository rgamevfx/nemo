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

#include <OpenImageIO/imageio.h>

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
#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include "nemo/media/ImageSource.hpp"

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

// Independent EXR fixture: a nonzero display origin and an overscan data
// window. Stored rows increase downwards; RG are authored coordinate labels,
// not an image generated by Nemo's evaluation or sampling helpers.
void writeWindowedPlate(const fs::path& path, float pixelAspect = 1.5F) {
    OIIO::ImageSpec spec(12, 12, 4, OIIO::TypeDesc::FLOAT);
    spec.channelnames = {"R", "G", "B", "A"};
    spec.x = -6;
    spec.y = -7;
    spec.full_x = -2;
    spec.full_y = -3;
    spec.full_width = 8;
    spec.full_height = 8;
    spec.attribute("pixelaspectratio", pixelAspect);
    std::vector<float> pixels(12 * 12 * 4);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 12; ++x) {
            const auto i = static_cast<std::size_t>((y * 12 + x) * 4);
            pixels[i] = static_cast<float>(x + 1) / 16.0F;
            pixels[i + 1] = static_cast<float>(y + 1) / 16.0F;
            pixels[i + 2] = 0.25F;
            pixels[i + 3] = 1.0F;
        }
    }
    auto writer = OIIO::ImageOutput::create(path.string());
    ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->open(path.string(), spec)) << writer->geterror();
    ASSERT_TRUE(writer->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << writer->geterror();
    ASSERT_TRUE(writer->close()) << writer->geterror();
}

NodeId createSessionNode(ProjectSession& session, const std::string& type, const std::string& name) {
    auto id = std::make_shared<NodeId>();
    const auto result = session.submit(addNodeCommand(session.document().rootNetworkId(), type, name, id),
                                       EditOptions{session.revision(), {}});
    if (!result.committed)
        throw std::runtime_error("fixture node creation failed: " + name);
    return *id;
}

void expectRgba(const CpuImage& image, int x, int y, std::array<float, 4> expected) {
    ASSERT_GE(x, 0);
    ASSERT_GE(y, 0);
    ASSERT_LT(x, image.width());
    ASSERT_LT(y, image.height());
    const auto actual = image.pixel(x, y);
    for (std::size_t channel = 0; channel < 4; ++channel)
        EXPECT_NEAR(actual[channel], expected[channel], 1e-6F)
            << "pixel (" << x << "," << y << "), channel " << channel;
}

void expectStoredChannel(const CpuImage& image, int x, int y, const std::string& name, float expected) {
    const auto& names = image.layout().channels;
    const auto found = std::find(names.begin(), names.end(), name);
    ASSERT_NE(found, names.end()) << name;
    EXPECT_FLOAT_EQ(image.channel(x, y, static_cast<int>(found - names.begin())), expected)
        << name << " at raster " << x << "," << y;
}

// --- retained-edge-domain fixture (issue #92) -------------------------------
// A generator with a fixed 4x4 format whose RETAINED data window is either the
// whole format (the ordinary finite producer) or a strict 2x2 sub-rectangle it
// claims to answer outside of (or nothing at all, with the claim still set).
// Every sample it produces is the same constant, so "the effect answered this
// coordinate" and "the executor cleared it" are distinguishable by pixels alone,
// and the ordinary test-only Affine downstream grades whatever survived with an
// independently hand-derived value. The fixture uses only public production
// interfaces and adds no shared behavior of its own: the claim is one
// description field that the shared planner, guards and keys must honor.
//
// `frameWidth`/`frameHeight` exist for the other half of the shared contract:
// they declare the generic creation-time initial value rule, so a node of this
// type created through the ordinary command must capture the owning network's
// saved canvas dimension rather than anything about the node it will read. They
// never take part in the pixel proof above.
constexpr std::string_view kEdgeFixtureType{"nemo.test.edgeFixture"};
constexpr Region kEdgeFormat{0, 0, 4, 4};
constexpr Region kEdgeRetained{1, 1, 2, 2};
constexpr std::array<float, 4> kEdgeSample{0.25F, 0.5F, 0.75F, 1.0F};
// Affine is out.rgb = scale.rgb*in.rgb + offset.rgb with out.a = in.a exactly.
constexpr std::array<float, 4> kEdgeGradedSample{kScale[0] * kEdgeSample[0] + kOffset[0],
                                                 kScale[1] * kEdgeSample[1] + kOffset[1],
                                                 kScale[2] * kEdgeSample[2] + kOffset[2], kEdgeSample[3]};

[[nodiscard]] bool fixtureFlag(const NodeInstance& node, const char* name) {
    const auto found = node.params.find(name);
    if (found == node.params.end()) {
        return false;
    }
    const auto* value = std::get_if<bool>(&found->second);
    return value != nullptr && *value;
}

NodeDescriptor edgeFixtureDescriptor() {
    return NodeDescriptor{.type = std::string{kEdgeFixtureType},
                          .displayName = "Retained Edge Fixture",
                          .group = "Tests",
                          .implementationVersion = 1,
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = {{.name = "extend", .type = ParameterType::Boolean, .defaultValue = false},
                                         {.name = "empty", .type = ParameterType::Boolean, .defaultValue = false},
                                         {.name = "frameWidth",
                                          .type = ParameterType::Float,
                                          .defaultValue = 0.0,
                                          .initialValue = ParameterInitialValue::OwningNetworkWidth},
                                         {.name = "frameHeight",
                                          .type = ParameterType::Integer,
                                          .defaultValue = std::int64_t{0},
                                          .initialValue = ParameterInitialValue::OwningNetworkHeight}},
                          .capabilities = NodeCapabilities{.samplingScales = {1},
                                                           .qualityModes = {Quality::Full},
                                                           .channels = {std::string{kAnyChannelCapability}}}};
}

CpuImage executeEdgeFixture(const CpuNodeContext& context) {
    CpuImage image(effectRasterLayout(context));
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixel(x, y, kEdgeSample);
        }
    }
    return image;
}

NodeContribution edgeFixtureContribution() {
    NodeContribution contribution;
    contribution.descriptor = edgeFixtureDescriptor();
    contribution.role = NodeRole::Image;
    // The claim travels through the shared contracts; a native kernel would add
    // nothing to this CPU-reference proof, so no backend is promised.
    contribution.nativeGpu = false;
    contribution.cpu = CpuImplementation{1, executeEdgeFixture};
    contribution.describe = [](const NodeDescriptionContext& context) {
        const bool extend = fixtureFlag(context.node, "extend");
        ImageDescription described;
        described.format = kEdgeFormat;
        described.dataBounds = extend ? kEdgeRetained : kEdgeFormat;
        if (fixtureFlag(context.node, "empty")) {
            // An empty image has no edge to extend: the claim is still stated,
            // and the shared predicate must refuse to honor it.
            described.dataBounds = Region{};
        }
        described.edgeExtension = extend;
        return described;
    };
    return contribution;
}

std::shared_ptr<const NodeContributions> edgeRegistry() {
    auto declarations = affineContributions();
    declarations.push_back(edgeFixtureContribution());
    return std::make_shared<const NodeContributions>(std::move(declarations));
}

// edge fixture -> affine -> Output.
struct EdgeChain {
    Document document;
    NodeId fixture{kInvalidNode};
    NodeId affine{kInvalidNode};
    NodeId output{kInvalidNode};
};

[[nodiscard]] EdgeChain makeEdgeChain(const std::shared_ptr<const NodeContributions>& registry, bool extend,
                                      bool empty = false) {
    EdgeChain chain{Document(registry->catalog())};
    Graph& graph = rootGraph(chain.document);
    chain.output = graph.nodeByName("Output")->id;
    chain.fixture = graph.addNode(std::string{kEdgeFixtureType}, "Edge");
    graph.setParam(chain.fixture, "extend", extend);
    graph.setParam(chain.fixture, "empty", empty);
    chain.affine = graph.addNode(kAffineType, "Affine");
    graph.setParam(chain.affine, "scale", ColorValue{kScale});
    graph.setParam(chain.affine, "offset", ColorValue{kOffset});
    static_cast<void>(graph.connect({chain.fixture, 0}, {chain.affine, 0}));
    static_cast<void>(graph.connect({chain.affine, 0}, {chain.output, 0}));
    return chain;
}

[[nodiscard]] const PlanStep* stepOf(const CpuEvaluation& evaluation, NodeId node) {
    for (const PlanStep& step : evaluation.plan.steps) {
        if (step.node == node) {
            return &step;
        }
    }
    return nullptr;
}

[[nodiscard]] EvaluationRequest windowRequest(const Document& document, NodeId output, Region region) {
    EvaluationRequest request = requestFor(document, output);
    request.region = region;
    return request;
}

void expectUniform(const CpuImage& image, std::array<float, 4> expected, const char* what) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const std::array<float, 4> actual = image.pixel(x, y);
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                ASSERT_FLOAT_EQ(actual[channel], expected[channel])
                    << what << " raster (" << x << "," << y << ") channel " << channel;
            }
        }
    }
}

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

TEST_F(ContributionTest, AlphaOnlyReadPreservesItsNamedChannelThroughEffects) {
    const auto path = fs::path{NEMO_CHANNEL_FIXTURE_DIR} / "alpha-only.exr";

    ProjectSession session;
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto source = createSessionNode(session, "source", "Matte");
    const auto shuffle = createSessionNode(session, "shuffle", "Alpha Shuffle");
    const auto grade = createSessionNode(session, "grade", "Grade");
    SourceReference reference;
    reference.path = path.string();
    ASSERT_TRUE(session
                    .submit(transactionCommand("alpha-only source",
                                               {setSourceCommand("matte", reference),
                                                setParamCommand(network, source, "source", std::string{"matte"}),
                                                setParamCommand(network, shuffle, "outputChannel0", std::string{}),
                                                setParamCommand(network, shuffle, "outputChannel1", std::string{}),
                                                setParamCommand(network, shuffle, "outputChannel2", std::string{}),
                                                connectCommand(network, {source, 0}, {shuffle, 0}),
                                                connectCommand(network, {shuffle, 0}, {grade, 0}),
                                                connectCommand(network, {grade, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    auto request = requestFor(session.document(), output);
    request.region = {0, 0, 2, 1};
    media::ImageSourceProvider provider;
    const auto rendered = evaluateCpu(session.document(), request, nullptr, &provider);
    ASSERT_EQ(rendered.image.layout().channels.size(), 1U) << "Read must not manufacture RGB for a matte";
    EXPECT_EQ(rendered.image.layout().channels.front(), "A");
    EXPECT_EQ(rendered.plan.description.channels.size(), 1U);
    EXPECT_FLOAT_EQ(rendered.image.data()[0], 0.25F);
    EXPECT_FLOAT_EQ(rendered.image.data()[1], 0.75F);

    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU alpha-only workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    auto native = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                    10'000'000'000ULL, nullptr, &nativeSources);
    const auto pixels = native.readBack(output, *boot.device, *boot.allocator);
    ASSERT_EQ(pixels.layout().channels, (std::vector<std::string>{"A"}));
    EXPECT_FLOAT_EQ(pixels.data()[0], 0.25F);
    EXPECT_FLOAT_EQ(pixels.data()[1], 0.75F);
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, PremultUnpremultSettingsSurviveHistoryAndReopenWithoutTouchingAuxiliaryChannels) {
    const NodeDescriptor* premultDescriptor = builtinNodeCatalogPtr()->find("premult");
    const NodeDescriptor* unpremultDescriptor = builtinNodeCatalogPtr()->find("unpremult");
    ASSERT_NE(premultDescriptor, nullptr);
    ASSERT_NE(unpremultDescriptor, nullptr);
    ASSERT_EQ(premultDescriptor->parameters.size(), 2U);
    ASSERT_EQ(unpremultDescriptor->parameters.size(), 2U);
    EXPECT_EQ(premultDescriptor->parameters[0].name, "multiply");
    EXPECT_EQ(premultDescriptor->parameters[1].name, "by");
    EXPECT_EQ(unpremultDescriptor->parameters[0].name, "divide");
    EXPECT_EQ(unpremultDescriptor->parameters[1].name, "by");
    EXPECT_EQ(premultDescriptor->parameters[0].defaultValue, ParameterValue{ChoiceValue{"RGB"}});
    EXPECT_EQ(premultDescriptor->parameters[1].defaultValue, ParameterValue{ChoiceValue{"Alpha"}});
    EXPECT_EQ(unpremultDescriptor->parameters[0].defaultValue, ParameterValue{ChoiceValue{"RGB"}});
    EXPECT_EQ(unpremultDescriptor->parameters[1].defaultValue, ParameterValue{ChoiceValue{"Alpha"}});
    for (const NodeDescriptor* descriptor : {premultDescriptor, unpremultDescriptor}) {
        for (const ParameterSpec& parameter : descriptor->parameters) {
            EXPECT_EQ(parameter.type, ParameterType::Choice);
            EXPECT_TRUE(parameter.editor.empty())
                << "generic inspector must own " << descriptor->type << "." << parameter.name;
        }
    }

    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    const NodeId output = session.document().network(network).defaultOutput();
    const NodeId source = createSessionNode(session, "source", "Multilayer");
    const NodeId premult = createSessionNode(session, "premult", "Premult");
    const NodeId unpremult = createSessionNode(session, "unpremult", "Unpremult");
    SourceReference reference;
    reference.path = (fs::path{NEMO_CHANNEL_FIXTURE_DIR} / "multilayer-b.exr").string();
    ASSERT_TRUE(session
                    .submit(transactionCommand("alpha arithmetic graph",
                                               {setSourceCommand("plate", reference),
                                                setParamCommand(network, source, "source", std::string{"plate"}),
                                                setParamCommand(network, source, "inputTransform", ChoiceValue{"raw"}),
                                                connectCommand(network, {source, 0}, {premult, 0}),
                                                connectCommand(network, {premult, 0}, {unpremult, 0}),
                                                connectCommand(network, {unpremult, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    ASSERT_TRUE(session
                    .submit(setParamCommand(network, premult, "multiply", ChoiceValue{"R"}),
                            EditOptions{session.revision(), {}})
                    .committed);
    ASSERT_TRUE(
        session.submit(setParamCommand(network, premult, "by", ChoiceValue{"G"}), EditOptions{session.revision(), {}})
            .committed);
    ASSERT_TRUE(session
                    .submit(setParamCommand(network, unpremult, "divide", ChoiceValue{"R"}),
                            EditOptions{session.revision(), {}})
                    .committed);
    ASSERT_TRUE(
        session.submit(setParamCommand(network, unpremult, "by", ChoiceValue{"G"}), EditOptions{session.revision(), {}})
            .committed);

    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    EXPECT_EQ(session.document().network(network).graph().node(unpremult)->params.count("by"), 0U);
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    EXPECT_EQ(session.document().network(network).graph().node(unpremult)->params.at("by"),
              ParameterValue{ChoiceValue{"G"}});

    media::ImageSourceProvider provider;
    auto requestedR = requestFor(session.document(), output);
    requestedR.region = {2, 3, 3, 2};
    requestedR.channels = {"R"};
    const RegionPlan demand =
        planDependencyRegions(session.document(), requestedR, *builtinNodeContributions(), &provider);
    bool foundSource = false;
    for (const auto& [id, node] : demand.images.nodes) {
        if (node.node.id == source) {
            foundSource = true;
            EXPECT_EQ(demand.requests.at(id).channels, (std::vector<std::string>{"R", "G"}))
                << "the selected multiplier/divisor remains an explicit upstream demand";
        }
    }
    ASSERT_TRUE(foundSource);

    const fs::path target = dir_ / "premult-unpremult.nemo";
    const ProjectWriteRequest save = session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(save);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(save, written).committed);
    ProjectReadResult read = ProjectFile::read(target);
    ASSERT_TRUE(read.ok) << read.error.message;
    ProjectSession reopened;
    ASSERT_TRUE(reopened.open(std::move(read)).replaced);
    const Graph& graph = reopened.document().network(network).graph();
    EXPECT_EQ(graph.nodeByName("Premult")->params.at("multiply"), ParameterValue{ChoiceValue{"R"}});
    EXPECT_EQ(graph.nodeByName("Premult")->params.at("by"), ParameterValue{ChoiceValue{"G"}});
    EXPECT_EQ(graph.nodeByName("Unpremult")->params.at("divide"), ParameterValue{ChoiceValue{"R"}});
    EXPECT_EQ(graph.nodeByName("Unpremult")->params.at("by"), ParameterValue{ChoiceValue{"G"}});

    auto request = requestFor(reopened.document(), graph.nodeByName("Output")->id);
    request.region = {2, 3, 3, 2};
    const CpuImage cpu = evaluateCpu(reopened.document(), request, nullptr, &provider).image;
    expectStoredChannel(cpu, 0, 0, "R", 0.25F);
    expectStoredChannel(cpu, 0, 0, "G", 0.5F);
    expectStoredChannel(cpu, 0, 0, "A", 1.0F);
    expectStoredChannel(cpu, 0, 0, "beauty.G", -5.0F);
    expectStoredChannel(cpu, 0, 0, "matte.coverage", 0.125F);
    expectStoredChannel(cpu, 0, 0, "depth.Z", 23.0F);

    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU persistence workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    auto native = eval::evaluateGpu(reopened.document(), request, library, *boot.device, *boot.allocator,
                                    10'000'000'000ULL, nullptr, &nativeSources);
    const CpuImage pixels = native.readBack(graph.nodeByName("Output")->id, *boot.device, *boot.allocator);
    EXPECT_EQ(pixels.layout().channels, cpu.layout().channels);
    const std::size_t sampleCount =
        static_cast<std::size_t>(cpu.width()) * static_cast<std::size_t>(cpu.height()) * cpu.channelCount();
    for (std::size_t index = 0; index < sampleCount; ++index)
        EXPECT_FLOAT_EQ(pixels.data()[index], cpu.data()[index]) << "stored sample " << index;
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, MultilayerReadPreservesNamedDataThroughGradeAndRegionalReuse) {
    // Handwritten OpenEXR bytes, independent of Nemo and OIIO's writer.
    ProjectSession session;
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto source = createSessionNode(session, "source", "Multilayer");
    const auto grade = createSessionNode(session, "grade", "Grade");
    SourceReference reference;
    reference.path = (fs::path{NEMO_CHANNEL_FIXTURE_DIR} / "multilayer-b.exr").string();
    ASSERT_TRUE(session
                    .submit(transactionCommand("multilayer source",
                                               {setSourceCommand("plate", reference),
                                                setParamCommand(network, source, "source", std::string{"plate"}),
                                                setParamCommand(network, source, "inputTransform", ChoiceValue{"raw"}),
                                                setParamCommand(network, grade, "multiply", ColorValue{{2, 2, 2, 1}}),
                                                connectCommand(network, {source, 0}, {grade, 0}),
                                                connectCommand(network, {grade, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    const std::vector<std::string> names{
        "A", "B", "G", "R", "beauty.B", "beauty.G", "beauty.R", "depth.Z", "matte.coverage", "motion.u", "motion.v"};
    const auto expectSamples = [&names](const CpuImage& image, Region region) {
        ASSERT_EQ(image.width(), region.width);
        ASSERT_EQ(image.height(), region.height);
        auto actualNames = image.layout().channels;
        std::sort(actualNames.begin(), actualNames.end());
        ASSERT_EQ(actualNames, names) << "no renamed, dropped, or manufactured channels";
        std::array<std::size_t, 11> indices{};
        for (std::size_t i = 0; i < names.size(); ++i)
            indices[i] = std::find(image.layout().channels.begin(), image.layout().channels.end(), names[i]) -
                         image.layout().channels.begin();
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const std::array<float, 11> expected{
                    1, 1.5F, 1, 0.5F, 6, -5, 4, static_cast<float>(10 * (region.x + x) + region.y + y), 0.125F, -2, 3};
                const auto offset = (static_cast<std::size_t>(y) * image.width() + x) * names.size();
                for (std::size_t i = 0; i < names.size(); ++i)
                    ASSERT_FLOAT_EQ(image.data()[offset + indices[i]], expected[i])
                        << names[i] << " at " << region.x + x << "," << region.y + y;
            }
        }
    };
    auto request = requestFor(session.document(), output);
    request.region = {0, 0, 8, 8};
    media::ImageSourceProvider provider;
    ResultCache<CpuImage> cpuCache;
    expectSamples(evaluateCpu(session.document(), request, &cpuCache, &provider).image, request.region);
    request.region = {2, 3, 3, 2};
    expectSamples(evaluateCpu(session.document(), request, &cpuCache, &provider).image, request.region);

    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU multilayer workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    ResultCache<eval::GpuNodeImage> nativeCache;
    request.region = {0, 0, 8, 8};
    auto native = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                    10'000'000'000ULL, &nativeCache, &nativeSources);
    expectSamples(native.readBack(output, *boot.device, *boot.allocator), request.region);
    request.region = {2, 3, 3, 2};
    auto crop = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                  10'000'000'000ULL, &nativeCache, &nativeSources);
    expectSamples(crop.readBack(output, *boot.device, *boot.allocator), request.region);
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, ShuffleFanoutConstantsAndDataKeepTheirOwnCoordinatesThroughTransform) {
    ProjectSession session;
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto sourceB = createSessionNode(session, "source", "B");
    const auto sourceA = createSessionNode(session, "source", "A");
    const auto shuffle = createSessionNode(session, "shuffle", "Shuffle");
    const auto grade = createSessionNode(session, "grade", "Grade");
    const auto transform = createSessionNode(session, "transform", "Transform");
    SourceReference b;
    b.path = (fs::path{NEMO_CHANNEL_FIXTURE_DIR} / "multilayer-b.exr").string();
    SourceReference a;
    a.path = (fs::path{NEMO_CHANNEL_FIXTURE_DIR} / "overscan-a.exr").string();
    ASSERT_TRUE(session
                    .submit(transactionCommand(
                                "named mapping",
                                {setSourceCommand("B", b),
                                 setSourceCommand("A", a),
                                 setParamCommand(network, sourceB, "source", std::string{"B"}),
                                 setParamCommand(network, sourceA, "source", std::string{"A"}),
                                 setParamCommand(network, sourceB, "inputTransform", ChoiceValue{"raw"}),
                                 setParamCommand(network, shuffle, "sourceKind0", ChoiceValue{"one"}),
                                 setParamCommand(network, shuffle, "sourceKind2", ChoiceValue{"zero"}),
                                 setParamCommand(network, shuffle, "input2", ChoiceValue{"A"}),
                                 setParamCommand(network, shuffle, "sourceKind4", ChoiceValue{"input2"}),
                                 setParamCommand(network, shuffle, "sourceChannel4", std::string{"depth.Z"}),
                                 setParamCommand(network, shuffle, "outputChannel4", std::string{"made.depth"}),
                                 setParamCommand(network, shuffle, "sourceKind5", ChoiceValue{"input2"}),
                                 setParamCommand(network, shuffle, "sourceChannel5", std::string{"depth.Z"}),
                                 setParamCommand(network, shuffle, "outputChannel5", std::string{"made.copy"}),
                                 setParamCommand(network, shuffle, "sourceKind6", ChoiceValue{"one"}),
                                 setParamCommand(network, shuffle, "outputChannel6", std::string{"made.one"}),
                                 setParamCommand(network, shuffle, "sourceKind7", ChoiceValue{"input2"}),
                                 setParamCommand(network, shuffle, "sourceChannel7", std::string{"absent.channel"}),
                                 setParamCommand(network, shuffle, "outputChannel7", std::string{"made.missing"}),
                                 setParamCommand(network, grade, "multiply", ColorValue{{2, 2, 2, 1}}),
                                 setParamCommand(network, transform, "translateX", 1.0),
                                 setParamCommand(network, transform, "filter", ChoiceValue{"Nearest"}),
                                 connectCommand(network, {sourceB, 0}, {shuffle, 0}),
                                 connectCommand(network, {sourceA, 0}, {shuffle, 1}),
                                 connectCommand(network, {shuffle, 0}, {grade, 0}),
                                 connectCommand(network, {grade, 0}, {transform, 0}),
                                 connectCommand(network, {transform, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    const auto expectSamples = [](const CpuImage& image, Region region) {
        ASSERT_EQ(image.width(), region.width);
        ASSERT_EQ(image.height(), region.height);
        ASSERT_EQ(image.channelCount(), 15U);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const int fx = region.x + x;
                const int fy = region.y + y;
                const bool b = fx >= 0 && fx < 8 && fy >= 0 && fy < 8;
                const bool shifted = fx >= 1 && fx < 9 && fy >= 0 && fy < 8;
                const bool a = fx >= -2 && fx < 2 && fy >= 2 && fy < 6;
                expectStoredChannel(image, x, y, "R", shifted ? 2.0F : 0.0F);
                expectStoredChannel(image, x, y, "G", shifted ? 1.0F : 0.0F);
                expectStoredChannel(image, x, y, "B", 0.0F);
                expectStoredChannel(image, x, y, "A", shifted ? 1.0F : 0.0F);
                expectStoredChannel(image, x, y, "beauty.R", b ? 4.0F : 0.0F);
                expectStoredChannel(image, x, y, "beauty.G", b ? -5.0F : 0.0F);
                expectStoredChannel(image, x, y, "beauty.B", b ? 6.0F : 0.0F);
                expectStoredChannel(image, x, y, "depth.Z", b ? static_cast<float>(10 * fx + fy) : 0.0F);
                expectStoredChannel(image, x, y, "motion.u", b ? -2.0F : 0.0F);
                expectStoredChannel(image, x, y, "motion.v", b ? 3.0F : 0.0F);
                expectStoredChannel(image, x, y, "matte.coverage", b ? 0.125F : 0.0F);
                expectStoredChannel(image, x, y, "made.depth", a ? 100.25F : 0.0F);
                expectStoredChannel(image, x, y, "made.copy", a ? 100.25F : 0.0F);
                expectStoredChannel(image, x, y, "made.one", b ? 1.0F : 0.0F);
                expectStoredChannel(image, x, y, "made.missing", 0.0F);
            }
        }
    };
    auto request = requestFor(session.document(), output);
    request.region = {-2, 0, 11, 8};
    media::ImageSourceProvider provider;
    ResultCache<CpuImage> cpuCache;
    expectSamples(evaluateCpu(session.document(), request, &cpuCache, &provider).image, request.region);
    request.region = {6, 2, 3, 2};
    expectSamples(evaluateCpu(session.document(), request, &cpuCache, &provider).image, request.region);
    request.channels = {"made.depth"};
    const auto plan = planDependencyRegions(session.document(), request, *builtinNodeContributions(), &provider);
    bool foundA = false;
    for (const auto& [id, node] : plan.images.nodes) {
        if (node.node.id == sourceA) {
            foundA = true;
            EXPECT_EQ(plan.requests.at(id).channels, (std::vector<std::string>{"depth.Z"}))
                << "destination demands must be translated to available source channels";
        }
    }
    ASSERT_TRUE(foundA);

    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU Shuffle workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    ResultCache<eval::GpuNodeImage> nativeCache;
    request.channels.clear();
    request.region = {-2, 0, 11, 8};
    auto native = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                    10'000'000'000ULL, &nativeCache, &nativeSources);
    expectSamples(native.readBack(output, *boot.device, *boot.allocator), request.region);
    request.region = {6, 2, 3, 2};
    auto crop = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                  10'000'000'000ULL, &nativeCache, &nativeSources);
    expectSamples(crop.readBack(output, *boot.device, *boot.allocator), request.region);
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, WindowedReadDescriptionAndPixelsSurviveProjectFile) {
    const auto platePath = dir_ / "overscan.exr";
    writeWindowedPlate(platePath);
    ASSERT_FALSE(HasFatalFailure());
    ProjectSession session;
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto readNode = createSessionNode(session, "source", "WindowedRead");
    const auto grade = createSessionNode(session, "grade", "Gain");
    SourceReference reference;
    reference.path = platePath.string();
    ASSERT_TRUE(session
                    .submit(transactionCommand("windowed plate",
                                               {setSourceCommand("plate", reference),
                                                setParamCommand(network, readNode, "source", std::string{"plate"}),
                                                setParamCommand(network, grade, "multiply", ColorValue{{2, 2, 2, 1}}),
                                                connectCommand(network, {readNode, 0}, {grade, 0}),
                                                connectCommand(network, {grade, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    EvaluationRequest request = requestFor(session.document(), output);
    request.region = {-4, -4, 12, 12};
    media::ImageSourceProvider provider;
    const auto descriptions = describeDependencies(session.document(), request, *builtinNodeContributions(), &provider);
    const auto& described = descriptions.nodes.at(descriptions.order.back().id).description;
    EXPECT_EQ(described.format, (Region{0, 0, 8, 8}));
    EXPECT_EQ(described.dataBounds, (Region{-4, -4, 12, 12}));
    EXPECT_FLOAT_EQ(described.pixelAspect, 1.5F);
    EXPECT_EQ(described.association, ImageAssociation::Straight);

    const auto target = dir_ / "windows.nemo";
    const auto save = session.prepareSave(target);
    const auto written = ProjectFile::writeAtomic(save);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(save, written).committed);
    auto loaded = ProjectFile::read(target);
    ASSERT_TRUE(loaded.ok) << loaded.error.message;
    ProjectSession reopened;
    ASSERT_TRUE(reopened.open(std::move(loaded)).replaced);

    ResultCache<CpuImage> cpuCache;
    for (const int scale : {1, 2, 4}) {
        request.samplingScale = scale;
        const auto rendered = evaluateCpu(reopened.document(), request, &cpuCache, &provider);
        EXPECT_EQ(rendered.plan.description, described);
        EXPECT_EQ(rendered.image.width(), 12 / scale);
        expectRgba(rendered.image, 0, 0, {0.125F, 0.125F, 0.5F, 1.0F});
        // Absolute (0,0) is the fifth source sample, regardless of density.
        expectRgba(rendered.image, 4 / scale, 4 / scale, {0.625F, 0.625F, 0.5F, 1.0F});
        auto region = request;
        region.region = {0, 0, 4, 4};
        const auto counts = cpuCache.counts();
        const auto crop = evaluateCpu(reopened.document(), region, &cpuCache, &provider);
        EXPECT_EQ(cpuCache.counts().misses, counts.misses) << "covering source and effect results stay reusable";
        expectRgba(crop.image, 0, 0, {0.625F, 0.625F, 0.5F, 1.0F});
    }
    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU persistence workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    ResultCache<eval::GpuNodeImage> nativeCache;
    for (const int scale : {1, 2, 4}) {
        request.samplingScale = scale;
        auto rendered = eval::evaluateGpu(reopened.document(), request, library, *boot.device, *boot.allocator,
                                          10'000'000'000ULL, &nativeCache, &nativeSources);
        EXPECT_EQ(rendered.plan.description, described);
        const auto pixels = rendered.readBack(output, *boot.device, *boot.allocator);
        expectRgba(pixels, 0, 0, {0.125F, 0.125F, 0.5F, 1.0F});
        expectRgba(pixels, 4 / scale, 4 / scale, {0.625F, 0.625F, 0.5F, 1.0F});
    }
    // Header geometry is semantic identity, not raster coverage. A changed PAR
    // at the same source path cannot reuse either an old result or decoded frame.
    writeWindowedPlate(platePath, 2.0F);
    ASSERT_FALSE(HasFatalFailure());
    request.samplingScale = 1;
    const auto beforeHeaderChange = cpuCache.counts();
    const auto changed = evaluateCpu(reopened.document(), request, &cpuCache, &provider);
    EXPECT_EQ(changed.plan.description.pixelAspect, 2.0F);
    EXPECT_EQ(changed.image.layout().pixelAspect, 2.0F);
    EXPECT_GT(cpuCache.counts().misses, beforeHeaderChange.misses);
    expectRgba(changed.image, 4, 4, {0.625F, 0.625F, 0.5F, 1.0F});
    auto changedNative = eval::evaluateGpu(reopened.document(), request, library, *boot.device, *boot.allocator,
                                           10'000'000'000ULL, &nativeCache, &nativeSources);
    EXPECT_EQ(changedNative.plan.description, changed.plan.description);
    EXPECT_EQ(changedNative.readBack(output, *boot.device, *boot.allocator).layout().pixelAspect, 2.0F);
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, HeaderDescriptionDoesNotRequirePixelExecutionAndReportsOfflineSource) {
    const auto platePath = dir_ / "metadata.exr";
    writeWindowedPlate(platePath);
    ASSERT_FALSE(HasFatalFailure());
    // Keep the real EXR header but truncate its compressed pixel payload.
    // A description that secretly decodes pixels cannot pass this fixture.
    fs::resize_file(platePath, fs::file_size(platePath) - 1);
    auto declarations = builtinContributions();
    for (auto& declaration : declarations) {
        if (declaration.role == NodeRole::Source) {
            declaration.cpu.reset();
            declaration.cpuUnavailableReason = "pixel backend deliberately unavailable";
        }
    }
    const auto registry = std::make_shared<const NodeContributions>(std::move(declarations));
    ProjectSession session(Document(registry->catalog()));
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto readNode = createSessionNode(session, "source", "MetadataRead");
    ASSERT_TRUE(session
                    .submit(transactionCommand("header source",
                                               {setSourceCommand("header", SourceReference{platePath.string()}),
                                                setParamCommand(network, readNode, "source", std::string{"header"}),
                                                connectCommand(network, {readNode, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    const auto request = requestFor(session.document(), output);
    media::ImageSourceProvider provider;
    const auto described = describeDependencies(session.document(), request, *registry, &provider);
    EXPECT_EQ(described.nodes.at(described.order.back().id).description.dataBounds, (Region{-4, -4, 12, 12}));
    ASSERT_TRUE(described.nodes.at(described.order.front().id).source.has_value());
    EXPECT_THROW(provider.frame(session.document(), *described.nodes.at(described.order.front().id).source, request),
                 std::exception);
    try {
        static_cast<void>(evaluateCpu(session.document(), request, nullptr, &provider, registry));
        FAIL() << "metadata availability must not manufacture a pixel backend";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, readNode);
        EXPECT_NE(std::string(error.what()).find("pixel backend deliberately unavailable"), std::string::npos);
    }
    ASSERT_TRUE(fs::remove(platePath));
    try {
        static_cast<void>(describeDependencies(session.document(), request, *registry, &provider));
        FAIL() << "offline source must not inherit the composition canvas";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, readNode);
        EXPECT_NE(std::string(error.what()).find("MetadataRead"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("metadata.exr"), std::string::npos);
    }
}

TEST_F(ContributionTest, AffineShiftMovesDescriptionAndInputDemandThroughSessionHistory) {
    const auto path = dir_ / "shift.exr";
    writeWindowedPlate(path);
    ASSERT_FALSE(HasFatalFailure());
    const auto registry = affineRegistry();
    ProjectSession session(Document(registry->catalog()));
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto source = createSessionNode(session, "source", "Read");
    const auto affine = createSessionNode(session, kAffineType, "Shift");
    ASSERT_TRUE(
        session
            .submit(transactionCommand("shift graph", {setSourceCommand("plate", SourceReference{path.string()}),
                                                       setParamCommand(network, source, "source", std::string{"plate"}),
                                                       connectCommand(network, {source, 0}, {affine, 0}),
                                                       connectCommand(network, {affine, 0}, {output, 0})}),
                    EditOptions{session.revision(), {}})
            .committed);
    ASSERT_TRUE(
        session
            .submit(transactionCommand("shift image", {setParamCommand(network, affine, "shiftX", std::int64_t{-130}),
                                                       setParamCommand(network, affine, "shiftY", std::int64_t{2})}),
                    EditOptions{session.revision(), {}})
            .committed);
    auto request = requestFor(session.document(), output);
    request.region = {-134, -2, 4, 4};
    media::ImageSourceProvider provider;
    const auto shifted = evaluateCpu(session.document(), request, nullptr, &provider, registry);
    EXPECT_EQ(shifted.plan.description.format, (Region{0, 0, 8, 8}));
    EXPECT_EQ(shifted.plan.description.dataBounds, (Region{-134, -2, 12, 12}));
    expectRgba(shifted.image, 0, 0, {0.0625F, 0.0625F, 0.25F, 1.0F});
    expectRgba(shifted.image, 3, 2, {0.25F, 0.1875F, 0.25F, 1.0F});
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    expectRgba(evaluateCpu(session.document(), request, nullptr, &provider, registry).image, 0, 0, {0, 0, 0, 0});
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    const auto save = session.prepareSave(dir_ / "shift.nemo");
    const auto written = ProjectFile::writeAtomic(save);
    ASSERT_TRUE(written.ok) << written.error.message;
    auto loaded = ProjectFile::read(dir_ / "shift.nemo", registry->catalog());
    ASSERT_TRUE(loaded.ok) << loaded.error.message;
    EXPECT_EQ(evaluateCpu(loaded.document, request, nullptr, &provider, registry).plan.description,
              shifted.plan.description);
    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU shift workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary library(affineGpuContributions(), eval::EffectBackend::Slang, slangSpvDir(),
                                      slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    auto rendered = eval::evaluateGpu(loaded.document, request, library, *boot.device, *boot.allocator,
                                      10'000'000'000ULL, nullptr, &nativeSources);
    EXPECT_EQ(rendered.plan.description, shifted.plan.description);
    const auto pixels = rendered.readBack(output, *boot.device, *boot.allocator);
    expectRgba(pixels, 0, 0, {0.0625F, 0.0625F, 0.25F, 1.0F});
    expectRgba(pixels, 3, 2, {0.25F, 0.1875F, 0.25F, 1.0F});
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, KnownEmptyReadRemainsEmptyThroughTransformAndGradeOffset) {
    const auto path = dir_ / "empty.exr";
    writeWindowedPlate(path);
    ASSERT_FALSE(HasFatalFailure());
    ProjectSession session;
    const auto network = session.document().rootNetworkId();
    const auto output = session.document().network(network).defaultOutput();
    const auto source = createSessionNode(session, "source", "BlackRead");
    const auto move = createSessionNode(session, "transform", "Move");
    const auto grade = createSessionNode(session, "grade", "Offset");
    ASSERT_TRUE(session
                    .submit(transactionCommand("known empty image",
                                               {setSourceCommand("plate", SourceReference{path.string()}),
                                                setParamCommand(network, source, "source", std::string{"plate"}),
                                                setParamCommand(network, source, "rangeMode", ChoiceValue{"custom"}),
                                                setParamCommand(network, source, "rangeFirst", std::int64_t{1}),
                                                setParamCommand(network, source, "rangeLast", std::int64_t{1}),
                                                setParamCommand(network, source, "beforePolicy", ChoiceValue{"black"}),
                                                setParamCommand(network, move, "translateX", 10.0),
                                                setParamCommand(network, grade, "offset", ColorValue{{1, 1, 1, 1}}),
                                                connectCommand(network, {source, 0}, {move, 0}),
                                                connectCommand(network, {move, 0}, {grade, 0}),
                                                connectCommand(network, {grade, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    auto request = requestFor(session.document(), output);
    request.region = {-4, -4, 16, 16};
    request.localTime = 0;
    media::ImageSourceProvider provider;
    for (const int scale : {1, 2, 4}) {
        request.samplingScale = scale;
        const auto rendered = evaluateCpu(session.document(), request, nullptr, &provider);
        EXPECT_EQ(rendered.plan.description.format, (Region{0, 0, 8, 8}));
        EXPECT_EQ(rendered.plan.description.pixelAspect, 1.5F);
        EXPECT_EQ(rendered.plan.description.dataBounds, Region{});
        for (int y = 0; y < rendered.image.height(); ++y)
            for (int x = 0; x < rendered.image.width(); ++x)
                ASSERT_EQ(rendered.image.pixel(x, y), (std::array<float, 4>{0, 0, 0, 0}));
    }
    if (slangSpvDir().empty())
        GTEST_SKIP() << "native Slang unavailable; CPU empty-image workflow completed";
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    for (const int scale : {1, 2, 4}) {
        request.samplingScale = scale;
        auto rendered = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                          10'000'000'000ULL, nullptr, &nativeSources);
        EXPECT_EQ(rendered.plan.description.dataBounds, Region{});
        const auto pixels = rendered.readBack(output, *boot.device, *boot.allocator);
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                ASSERT_EQ(pixels.pixel(x, y), (std::array<float, 4>{0, 0, 0, 0}));
    }
    // Restore actual samples. Mix must retain the original OFF-FORMAT support,
    // not merely union the transformed window with the logical format.
    ASSERT_TRUE(session
                    .submit(transactionCommand("restore held image",
                                               {setParamCommand(network, source, "beforePolicy", ChoiceValue{"hold"}),
                                                setParamCommand(network, move, "translateX", 20.0),
                                                setParamCommand(network, move, "mix", 0.5),
                                                setParamCommand(network, grade, "offset", ColorValue{{0, 0, 0, 0}})}),
                            EditOptions{session.revision(), {}})
                    .committed);
    request.region = {-4, -4, 4, 4};
    request.samplingScale = 1;
    const auto mixed = evaluateCpu(session.document(), request, nullptr, &provider);
    EXPECT_TRUE(regionContains(mixed.plan.description.dataBounds, Region{-4, -4, 12, 12}));
    expectRgba(mixed.image, 0, 0, {0.03125F, 0.03125F, 0.125F, 0.5F});
    auto mixedNative = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator,
                                         10'000'000'000ULL, nullptr, &nativeSources);
    EXPECT_EQ(mixedNative.plan.description, mixed.plan.description);
    expectRgba(mixedNative.readBack(output, *boot.device, *boot.allocator), 0, 0, {0.03125F, 0.03125F, 0.125F, 0.5F});
    expectValidationClean(*boot.instance);
}

TEST_F(ContributionTest, InvalidDescriptionFailsBeforeAnyPixelBackendIsRequired) {
    auto declarations = affineContributions();
    for (auto& declaration : declarations) {
        if (declaration.descriptor.type == "constcolor") {
            declaration.cpu.reset();
            declaration.cpuUnavailableReason = "pixel execution unavailable";
        }
    }
    declarations.back().describe = [](const NodeDescriptionContext& context) {
        ImageDescription invalid = context.inherited;
        // Each member fits int32, but the right edge does not.
        invalid.dataBounds = {1'073'741'824, 0, 1'073'741'824, 1};
        return invalid;
    };
    const auto registry = std::make_shared<const NodeContributions>(std::move(declarations));
    const auto chain = makeAffineChain(registry);
    try {
        static_cast<void>(
            evaluateCpu(chain.document, requestFor(chain.document, chain.output), nullptr, nullptr, registry));
        FAIL() << "invalid description must not reach pixel execution";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, chain.affine);
        EXPECT_NE(std::string(error.what()).find("data bounds"), std::string::npos);
    }
}

TEST_F(ContributionTest, InputChannelRequirementsCannotInventProducerChannels) {
    auto declarations = affineContributions();
    declarations.back().inputRequirements = [](const NodeRegionContext& context) {
        return std::vector<InputRequirement>{{context.request.region, {"Z"}}};
    };
    const auto registry = std::make_shared<const NodeContributions>(std::move(declarations));
    const auto chain = makeAffineChain(registry);
    ResultCache<CpuImage> cache;
    try {
        static_cast<void>(
            evaluateCpu(chain.document, requestFor(chain.document, chain.output), &cache, nullptr, registry));
        FAIL() << "the RGBA producer cannot supply undeclared Z data";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, rootGraph(chain.document).nodeByName("plate")->id);
        EXPECT_NE(std::string(error.what()).find("Z"), std::string::npos);
    }
    EXPECT_EQ(cache.counts().misses, 0U) << "invalid demand must fail before any result lookup or dispatch";
}

TEST_F(ContributionTest, InputRequirementsCannotReferToUndeclaredPorts) {
    auto declarations = affineContributions();
    declarations.back().inputRequirements = [](const NodeRegionContext& context) {
        return std::vector<InputRequirement>{{context.request.region, {}}, {context.request.region, {}}};
    };
    const auto registry = std::make_shared<const NodeContributions>(std::move(declarations));
    const auto chain = makeAffineChain(registry);
    try {
        static_cast<void>(
            evaluateCpu(chain.document, requestFor(chain.document, chain.output), nullptr, nullptr, registry));
        FAIL() << "Affine has exactly one declared input";
    } catch (const EvaluationException& error) {
        EXPECT_EQ(error.node, chain.affine);
        EXPECT_NE(std::string(error.what()).find("port"), std::string::npos);
    }
}

TEST_F(ContributionTest, PreparedNativePlanCannotCrossRequestOrDocumentState) {
    const eval::EffectLibrary library(affineGpuContributions(), eval::EffectBackend::Slang, slangSpvDir(),
                                      slangSrcDir());
    auto chain = makeAffineChain(library.contributions());
    const auto request = requestFor(chain.document, chain.output);
    const auto described = describeDependencies(chain.document, request, *library.contributions());
    const auto plan = planResolvedRegions(chain.document, request, *library.contributions(), described);
    EXPECT_EQ(eval::queryViewerResultKey(chain.document, request, library, {}, nullptr, &plan),
              eval::queryViewerResultKey(chain.document, request, library));
    auto anotherFrame = request;
    ++anotherFrame.localTime;
    EXPECT_THROW(planResolvedRegions(chain.document, anotherFrame, *library.contributions(), described),
                 EvaluationException);
    auto anotherRegion = request;
    anotherRegion.region.x = 1;
    EXPECT_THROW(eval::queryViewerResultKey(chain.document, anotherRegion, library, {}, nullptr, &plan),
                 EvaluationException);
    CommandStack history(chain.document);
    history.push(setParamCommand(chain.document.rootNetworkId(), chain.affine, "shiftX", std::int64_t{1}));
    EXPECT_THROW(eval::queryViewerResultKey(chain.document, request, library, {}, nullptr, &plan), EvaluationException);
    EXPECT_THROW(planResolvedRegions(chain.document, request, *library.contributions(), described),
                 EvaluationException);
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

// ---------------------------------------------------------------------------
// Issue #92: an explicit retained-edge-domain claim is part of the shared image
// contract, not a node-local side effect. One independently written generator
// declares a 4x4 format whose retained data window is a smaller rectangle, and
// claims that its pixels answer beyond that window; the ordinary test-only
// Affine downstream grades whatever it reads. The delivered window reaches two
// samples past the retained rectangle on every side, so "the claim was honored"
// and "the support guard cleared everything outside the window" are
// distinguishable in the pixels of a node that knows nothing about the claim —
// and the plan's own coverage proves the demand was retained instead of being
// clipped to the retained rectangle.
// ---------------------------------------------------------------------------
TEST_F(ContributionTest, ExtendedDescriptionAnswersOutsideItsRetainedWindowThroughAnOrdinaryEffect) {
    const auto registry = edgeRegistry();
    const Region window{-2, -2, 8, 8};

    // 1) The ordinary finite producer: the retained window is graded exactly, and
    //    everything outside it is the transparent black every existing producer
    //    produces today.
    const EdgeChain finite = makeEdgeChain(registry, /*extend=*/false);
    const CpuEvaluation finiteEvaluation =
        evaluateCpu(finite.document, windowRequest(finite.document, finite.output, window), nullptr, nullptr, registry);
    EXPECT_FALSE(finiteEvaluation.plan.description.edgeExtension);
    ASSERT_EQ(finiteEvaluation.image.width(), window.width);
    ASSERT_EQ(finiteEvaluation.image.height(), window.height);
    expectRgba(finiteEvaluation.image, 0, 0, {0.0F, 0.0F, 0.0F, 0.0F});
    expectRgba(finiteEvaluation.image, 2, 2, kEdgeGradedSample);
    expectRgba(finiteEvaluation.image, 5, 5, kEdgeGradedSample);
    expectRgba(finiteEvaluation.image, 6, 6, {0.0F, 0.0F, 0.0F, 0.0F});

    // 2) The same pixels with a smaller retained window and the edge claimed: the
    //    planner plans exactly the demanded window (its padding blocks are limited
    //    to the retained domain, the demand itself is not), the claim survives the
    //    ordinary downstream effect, and every sample of the delivered raster is
    //    real graded data rather than transparent black.
    EdgeChain extended = makeEdgeChain(registry, /*extend=*/true);
    const CpuEvaluation extendedEvaluation = evaluateCpu(
        extended.document, windowRequest(extended.document, extended.output, window), nullptr, nullptr, registry);
    const PlanStep* extendedFixture = stepOf(extendedEvaluation, extended.fixture);
    const PlanStep* extendedAffine = stepOf(extendedEvaluation, extended.affine);
    ASSERT_NE(extendedFixture, nullptr);
    ASSERT_NE(extendedAffine, nullptr);
    EXPECT_TRUE(extendedFixture->description.edgeExtension);
    EXPECT_EQ(extendedFixture->description.dataBounds, kEdgeRetained);
    EXPECT_TRUE(extendedAffine->description.edgeExtension) << "an ordinary effect keeps the claim it inherits";
    EXPECT_EQ(extendedEvaluation.plan.description.edgeExtension, true);
    EXPECT_EQ(extendedFixture->region, window)
        << "the demanded window is kept, not clipped to the retained domain or escalated to the format";
    ASSERT_EQ(extendedEvaluation.image.width(), window.width);
    ASSERT_EQ(extendedEvaluation.image.height(), window.height);
    expectUniform(extendedEvaluation.image, kEdgeGradedSample, "extended samples survive the ordinary effect");

    // 3) An empty retained window stays fully transparent even with the claim
    //    set: there is no edge to extend, and the flag is not a license to keep
    //    whatever a node happened to write.
    const EdgeChain empty = makeEdgeChain(registry, /*extend=*/true, /*empty=*/true);
    const CpuEvaluation emptyEvaluation =
        evaluateCpu(empty.document, windowRequest(empty.document, empty.output, window), nullptr, nullptr, registry);
    const PlanStep* emptyFixture = stepOf(emptyEvaluation, empty.fixture);
    ASSERT_NE(emptyFixture, nullptr);
    EXPECT_TRUE(emptyFixture->description.edgeExtension) << "the declaration itself travels verbatim";
    EXPECT_EQ(emptyFixture->description.dataBounds, Region{});
    expectUniform(emptyEvaluation.image, {0.0F, 0.0F, 0.0F, 0.0F}, "an empty image stays transparent");

    // 4) Reuse identity follows the image's meaning: identical nodes, pixels and
    //    request are NOT the same image once the retained window and the claim
    //    change, while an unchanged request still reuses every node.
    ResultCache<CpuImage> cache;
    const std::size_t scheduled = extendedEvaluation.plan.steps.size();
    ASSERT_GE(scheduled, 3U) << "the fixture, the ordinary effect and the delivery are all scheduled";
    static_cast<void>(evaluateCpu(extended.document, windowRequest(extended.document, extended.output, window), &cache,
                                  nullptr, registry));
    const CacheCounts computed = cache.counts();
    EXPECT_EQ(computed.misses, scheduled) << "one lookup per scheduled node, all cold";
    EXPECT_EQ(computed.hits, 0U);
    static_cast<void>(evaluateCpu(extended.document, windowRequest(extended.document, extended.output, window), &cache,
                                  nullptr, registry));
    const CacheCounts reused = cache.counts();
    EXPECT_EQ(reused.hits - computed.hits, scheduled) << "an unchanged request still reuses every scheduled node";
    rootGraph(extended.document).setParam(extended.fixture, "extend", false);
    const CpuEvaluation finiteAgain = evaluateCpu(
        extended.document, windowRequest(extended.document, extended.output, window), &cache, nullptr, registry);
    const CacheCounts changed = cache.counts();
    EXPECT_EQ(changed.misses - reused.misses, scheduled) << "a changed retained window and claim is a different image";
    EXPECT_EQ(changed.hits, reused.hits) << "no cached result may be served for the changed meaning";
    expectRgba(finiteAgain.image, 0, 0, {0.0F, 0.0F, 0.0F, 0.0F});
    expectRgba(finiteAgain.image, 2, 2, kEdgeGradedSample);
}

TEST_F(ContributionTest, EmptyCropExtensionCannotReviveThroughFiniteCoverage) {
    struct Coverage {
        const char* type;
        const char* sourceKind;
        float red;
    };
    // Each path can grow an empty base into non-empty finite coverage: Merge B,
    // a Shuffle constant, and a Shuffle row sourced from its other input.
    for (const Coverage coverage :
         {Coverage{"merge", nullptr, 1.25F}, Coverage{"shuffle", "one", 2.0F}, Coverage{"shuffle", "input2", 1.25F}}) {
        SCOPED_TRACE(coverage.type);
        ProjectSession session;
        const auto network = session.document().rootNetworkId();
        const auto output = session.document().network(network).defaultOutput();
        const auto plate = createSessionNode(session, "constcolor", "Plate");
        const auto crop = createSessionNode(session, "crop", "EmptyExtendedCrop");
        const auto combine = createSessionNode(session, coverage.type, "FiniteCoverage");
        const auto grade = createSessionNode(session, "grade", "Offset");
        ASSERT_TRUE(session
                        .submit(transactionCommand(
                                    "empty extension through finite coverage",
                                    {setParamCommand(network, plate, "color", ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}),
                                     setParamCommand(network, crop, "right", 0.0),
                                     setParamCommand(network, crop, "blackOutside", false),
                                     setParamCommand(network, grade, "offset", ColorValue{{1, 1, 1, 1}}),
                                     connectCommand(network, {plate, 0}, {crop, 0}),
                                     connectCommand(network, {crop, 0}, {combine, 0}),
                                     connectCommand(network, {plate, 0}, {combine, 1}),
                                     connectCommand(network, {combine, 0}, {grade, 0}),
                                     connectCommand(network, {grade, 0}, {output, 0})}),
                                EditOptions{session.revision(), {}})
                        .committed);
        if (coverage.sourceKind != nullptr) {
            ASSERT_TRUE(session
                            .submit(transactionCommand("select finite Shuffle coverage",
                                                       {setParamCommand(network, combine, "sourceKind0",
                                                                        ChoiceValue{coverage.sourceKind}),
                                                        setParamCommand(network, combine, "input2", ChoiceValue{"A"})}),
                                    EditOptions{session.revision(), {}})
                            .committed);
        }
        auto request = requestFor(session.document(), output);
        request.region = {-4, -4, 8, 8};
        const auto check = [&](const CpuImage& image, int scale) {
            expectRgba(image, 0, 0, {0, 0, 0, 0});
            EXPECT_FLOAT_EQ(image.pixel(4 / scale, 4 / scale)[0], coverage.red);
        };
        for (const int scale : {1, 2, 4}) {
            request.samplingScale = scale;
            check(evaluateCpu(session.document(), request).image, scale);
        }
        if (slangSpvDir().empty())
            GTEST_SKIP() << "native Slang unavailable; CPU empty-extension workflow completed";
        const Bootstrap boot = createBootstrap();
        NEMO_SKIP_OR_FAIL(boot);
        for (const auto& library :
             {eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir()), eval::glslEffectLibrary()}) {
            for (const int scale : {1, 2, 4}) {
                request.samplingScale = scale;
                auto rendered = eval::evaluateGpu(session.document(), request, library, *boot.device, *boot.allocator);
                check(rendered.readBack(output, *boot.device, *boot.allocator), scale);
            }
        }
        expectValidationClean(*boot.instance);
    }
}

// ---------------------------------------------------------------------------
// Issue #92: the other half of the shared contract is the ONE generic
// creation-time initial value rule. A node whose authored values live in the
// owning network's own frame must be created FROM that network's saved canvas
// through the ordinary public creation command — never from the selected Read
// or the viewer — and what it captured is authored state from that moment on.
// ---------------------------------------------------------------------------
TEST_F(ContributionTest, CreationCapturesTheOwningNetworksSavedCanvas) {
    const auto registry = edgeRegistry();
    ProjectSession session(Document(registry->catalog()));
    const NetworkId network = session.document().rootNetworkId();
    ASSERT_TRUE(
        session
            .submit(setNetworkFormatCommand(network, ImageFormat{1234, 567, 1.0F}), EditOptions{session.revision(), {}})
            .committed);

    const NodeId fixture = createSessionNode(session, std::string{kEdgeFixtureType}, "Edge");
    {
        const NodeInstance* created = rootGraph(session.document()).node(fixture);
        ASSERT_NE(created, nullptr);
        const auto width = created->params.find("frameWidth");
        const auto height = created->params.find("frameHeight");
        ASSERT_NE(width, created->params.end()) << "the declared rule seeds the parameter at creation";
        ASSERT_NE(height, created->params.end());
        ASSERT_TRUE(std::holds_alternative<double>(width->second));
        ASSERT_TRUE(std::holds_alternative<std::int64_t>(height->second));
        EXPECT_DOUBLE_EQ(std::get<double>(width->second), 1234.0);
        EXPECT_EQ(std::get<std::int64_t>(height->second), 567);
    }

    // A later canvas edit never rewrites what the node captured, and a type that
    // declares no rule keeps its schema default instead of an accidental seed.
    ASSERT_TRUE(
        session
            .submit(setNetworkFormatCommand(network, ImageFormat{640, 480, 1.0F}), EditOptions{session.revision(), {}})
            .committed);
    const NodeInstance* afterEdit = rootGraph(session.document()).node(fixture);
    ASSERT_NE(afterEdit, nullptr);
    EXPECT_DOUBLE_EQ(std::get<double>(afterEdit->params.at("frameWidth")), 1234.0);
    EXPECT_EQ(std::get<std::int64_t>(afterEdit->params.at("frameHeight")), 567);

    const NodeId plain = createSessionNode(session, kAffineType, "Affine");
    const NodeInstance* plainNode = rootGraph(session.document()).node(plain);
    ASSERT_NE(plainNode, nullptr);
    EXPECT_EQ(plainNode->params.count("frameWidth"), 0U);
    EXPECT_EQ(plainNode->params.count("frameHeight"), 0U);
}
