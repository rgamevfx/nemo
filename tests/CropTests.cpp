// Crop's regression and acceptance coverage (issue #92, stories 43, 45-49).
//
// The seam is the REAL public workflow: nodes are authored as ordinary graph
// state or through ProjectSession commands, saved and reopened through
// ProjectFile, and rendered by the public CPU evaluator against hand-derived
// pixels. The expected samples are analytic literals derived from the
// documented test pattern (R = x/(W-1), G = y/(H-1), B = 1 inside the bar) and
// from the approved softness policy, never another executor's output: the CPU
// adapter and the two kernels are authored independently and agree only as
// evidence.
//
// The first test is the regression this change exists to make pass: before the
// crop contribution existed, the node type had no registered implementation and
// no geometry at all.

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/ImageSource.hpp"

using namespace nemo;

namespace {

namespace fs = std::filesystem;

// --- public graph helpers ---------------------------------------------------

Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

// A fresh root document whose authored canvas is `width` x `height`. That saved
// canvas is the frame Crop's bottom-left box is stated in (issue #96), and it is
// also what a generator falls back to, so a chain built on it has the plate's
// format, its data window and the box frame as one rectangle.
struct Canvas {
    Document document;
    NodeId output{kInvalidNode};
};

[[nodiscard]] Canvas makeCanvas(int width, int height) {
    Canvas canvas;
    CommandStack stack(canvas.document);
    stack.push(setNetworkFormatCommand(canvas.document.rootNetworkId(), ImageFormat{width, height, 1.0F}));
    canvas.output = resolveOutput(canvas.document, canvas.document.rootNetworkId());
    return canvas;
}

// A plate -> crop -> Output chain on a `width` x `height` canvas.
struct CropChain {
    Document document;
    NodeId plate{kInvalidNode};
    NodeId crop{kInvalidNode};
    NodeId output{kInvalidNode};
};

[[nodiscard]] CropChain makeCropChain(int width, int height) {
    Canvas canvas = makeCanvas(width, height);
    CropChain chain;
    chain.document = std::move(canvas.document);
    chain.output = canvas.output;
    Graph& graph = rootGraph(chain.document);
    chain.plate = graph.addNode("testpattern", "plate");
    chain.crop = graph.addNode("crop", "Crop");
    static_cast<void>(graph.connect({chain.plate, 0}, {chain.crop, 0}));
    static_cast<void>(graph.connect({chain.crop, 0}, {chain.output, 0}));
    return chain;
}

// The authored box, in the reference's bottom-left convention: `x`/`y` are the
// bottom-left corner's distance from the canvas's left/bottom edges and
// `right`/`top` the top-right corner's.
void setBox(Graph& graph, NodeId crop, double x, double y, double right, double top) {
    graph.setParam(crop, "x", ParameterValue{x});
    graph.setParam(crop, "y", ParameterValue{y});
    graph.setParam(crop, "right", ParameterValue{right});
    graph.setParam(crop, "top", ParameterValue{top});
}

[[nodiscard]] EvaluationRequest frameRequest(const Document& document, Region region, int samplingScale = 1) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = resolveOutput(document, request.network);
    request.region = region;
    request.samplingScale = samplingScale;
    return request;
}

// The documented test pattern at one full-resolution sample: R = x/(W-1),
// G = y/(H-1), B = 1 inside the bar (the first max(2, W/16) columns at local
// time 0, which is the frame every request here evaluates), A = 1.
[[nodiscard]] std::array<float, 4> patternSample(int x, int y, int width, int height) {
    const int barWidth = std::max(2, width / 16);
    const float r = width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0F;
    const float g = height > 1 ? static_cast<float>(y) / static_cast<float>(height - 1) : 0.0F;
    return {r, g, x < barWidth ? 1.0F : 0.0F, 1.0F};
}

void expectSample(const CpuImage& image, int x, int y, const std::array<float, 4>& expected, const char* what) {
    ASSERT_GE(x, 0) << what;
    ASSERT_GE(y, 0) << what;
    ASSERT_LT(x, image.width()) << what;
    ASSERT_LT(y, image.height()) << what;
    const std::array<float, 4> actual = image.pixel(x, y);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_NEAR(actual[channel], expected[channel], 1e-6F)
            << what << ": raster (" << x << "," << y << ") channel " << channel;
    }
}

// A three-channel float EXR authored here: no Nemo or image-decoder code
// produces these values, and the file stores NO alpha, which is the case Crop's
// black-outside rule has to add one for.
void writeRgbPlate(const fs::path& path, int width, int height) {
    OIIO::ImageSpec spec(width, height, 3, OIIO::TypeDesc::FLOAT);
    spec.channelnames = {"R", "G", "B"};
    std::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>((y * width + x) * 3);
            pixels[offset] = static_cast<float>(x + 1) / 16.0F;
            pixels[offset + 1] = static_cast<float>(y + 1) / 16.0F;
            pixels[offset + 2] = 0.25F;
        }
    }
    auto writer = OIIO::ImageOutput::create(path.string());
    ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->open(path.string(), spec)) << writer->geterror();
    ASSERT_TRUE(writer->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << writer->geterror();
    ASSERT_TRUE(writer->close()) << writer->geterror();
}

// A four-channel float EXR authored here, storing RGB ALREADY multiplied by its
// alpha — EXR's own declared association, which a raw decode leaves alone — with
// the alpha rising by row so a premultiplied fade cannot look like a straight
// one. Every stored value is a hand-derivable rational: R = (x+1)(y+1)/32,
// G = (y+1)/4, B = (y+1)/8, A = (y+1)/4.
void writeRgbaPlate(const fs::path& path, int width, int height) {
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);
    spec.channelnames = {"R", "G", "B", "A"};
    std::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = static_cast<std::size_t>((y * width + x) * 4);
            const float alpha = static_cast<float>(y + 1) / static_cast<float>(height);
            pixels[offset] = static_cast<float>(x + 1) / 8.0F * alpha;
            pixels[offset + 1] = alpha;
            pixels[offset + 2] = 0.5F * alpha;
            pixels[offset + 3] = alpha;
        }
    }
    auto writer = OIIO::ImageOutput::create(path.string());
    ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->open(path.string(), spec)) << writer->geterror();
    ASSERT_TRUE(writer->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << writer->geterror();
    ASSERT_TRUE(writer->close()) << writer->geterror();
}

// --- the session fixture seam ----------------------------------------------

// The Crop parameters a fixture authors, as authored bottom-left box values.
struct CropFixtureSettings {
    double x{0.0};
    double y{0.0};
    double right{0.0};
    double top{0.0};
    double softness{0.0};
    bool blackOutside{true};
};

// A Read -> Crop -> Output chain authored through the public session commands,
// reading `platePath` with a RAW decode so the file's stored samples — and, when
// it stores alpha, its declared association — reach the Crop exactly as written.
//
// Creation and configuration are TWO transactions, and the split is the point: a
// command captures its arguments when it is CONSTRUCTED, so a command that
// addresses a created node — a parameter edit or an edge — has a valid id only
// once the command that creates that node has RUN. The first transaction is
// atomic node creation; the second is the source declaration, the Read's decode
// choice, the Crop's authored parameters and both edges as ONE atomic
// configuration.
void makeSessionCropChain(ProjectSession& session, const fs::path& platePath, const std::string& alphaMode,
                          const CropFixtureSettings& crop) {
    const NetworkId network = session.document().rootNetworkId();
    const NodeId output = session.document().network(network).defaultOutput();
    auto createdPlate = std::make_shared<NodeId>();
    auto createdCrop = std::make_shared<NodeId>();
    const Command plateNode = addNodeCommand(network, "source", "Plate", createdPlate);
    const Command cropNode = addNodeCommand(network, "crop", "Crop", createdCrop);
    EXPECT_TRUE(
        session.submit(transactionCommand("crop nodes", {plateNode, cropNode}), EditOptions{session.revision(), {}})
            .committed);
    const NodeId plate = *createdPlate;
    const NodeId node = *createdCrop;
    SourceReference reference;
    reference.path = platePath.string();
    std::vector<Command> configuration;
    configuration.push_back(setSourceCommand("plate", reference));
    configuration.push_back(setParamCommand(network, plate, "source", std::string{"plate"}));
    configuration.push_back(setParamCommand(network, plate, "inputTransform", ChoiceValue{"raw"}));
    configuration.push_back(setParamCommand(network, plate, "alphaMode", ChoiceValue{alphaMode}));
    configuration.push_back(setParamCommand(network, node, "x", ParameterValue{crop.x}));
    configuration.push_back(setParamCommand(network, node, "y", ParameterValue{crop.y}));
    configuration.push_back(setParamCommand(network, node, "right", ParameterValue{crop.right}));
    configuration.push_back(setParamCommand(network, node, "top", ParameterValue{crop.top}));
    configuration.push_back(setParamCommand(network, node, "softness", ParameterValue{crop.softness}));
    configuration.push_back(setParamCommand(network, node, "blackOutside", ParameterValue{crop.blackOutside}));
    configuration.push_back(connectCommand(network, {plate, 0}, {node, 0}));
    configuration.push_back(connectCommand(network, {node, 0}, {output, 0}));
    EXPECT_TRUE(session
                    .submit(transactionCommand("configure crop chain", std::move(configuration)),
                            EditOptions{session.revision(), {}})
                    .committed);
}

// --- native bootstrap (the same pattern the sibling suites use) -------------
//
// The native path is where the compiled Slang kernel and the runtime GLSL
// reference are actually exercised; both must reproduce the same analytic
// samples as the CPU adapter, which is evidence because the three are written
// independently.

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

// The declared pixel tolerance (issue #92): absolute plus relative, never an
// exactness claim across three independent implementations.
void expectImagesClose(const CpuImage& expected, const CpuImage& actual, const char* what) {
    ASSERT_EQ(actual.width(), expected.width()) << what;
    ASSERT_EQ(actual.height(), expected.height()) << what;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const std::array<float, 4> want = expected.pixel(x, y);
            const std::array<float, 4> got = actual.pixel(x, y);
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                const float tolerance = 2e-5F + 2e-5F * std::abs(want[channel]);
                EXPECT_NEAR(got[channel], want[channel], tolerance)
                    << what << ": pixel (" << x << "," << y << ") channel " << channel;
            }
        }
    }
}

class CropTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code error;
        dir_ = fs::temp_directory_path(error) /
               ("nemo-crop-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(counter_++));
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

int CropTest::counter_ = 0;

}  // namespace

// The box is the retained domain: the enclosure of its four fractional corners,
// and nothing outside it carries data. The canvas is 8x4, the box's bottom-left
// corner is (2, 1) and its top-right (6, 3), so the retained rows in the stored
// raster's y-down convention are 1..3 and the retained columns 2..6.
TEST_F(CropTest, CropKeepsOnlyTheBoxEnclosureAndBlacksOutside) {
    CropChain chain = makeCropChain(8, 4);
    setBox(rootGraph(chain.document), chain.crop, 2.0, 1.0, 6.0, 3.0);

    const CpuEvaluation evaluation = evaluateCpu(chain.document, frameRequest(chain.document, {0, 0, 8, 4}));

    // The description states the retained enclosure as data bounds and keeps the
    // incoming canvas as the format (story 45: bounds without a format change).
    EXPECT_EQ(evaluation.plan.description.format, (Region{0, 0, 8, 4}));
    EXPECT_EQ(evaluation.plan.description.dataBounds, (Region{2, 1, 4, 2}));
    EXPECT_FALSE(evaluation.plan.description.edgeExtension);
    // The plate already carries an alpha channel, so black outside adds none.
    EXPECT_EQ(evaluation.plan.description.channels, (std::vector<std::string>{"R", "G", "B", "A"}));

    ASSERT_EQ(evaluation.image.width(), 8);
    ASSERT_EQ(evaluation.image.height(), 4);
    expectSample(evaluation.image, 2, 1, patternSample(2, 1, 8, 4), "inside the box's top-left sample");
    expectSample(evaluation.image, 5, 2, patternSample(5, 2, 8, 4), "inside the box");
    // Every sample outside the box is transparent black: left of it, right of
    // it, and below it.
    expectSample(evaluation.image, 1, 1, {0.0F, 0.0F, 0.0F, 0.0F}, "left of the box");
    expectSample(evaluation.image, 6, 1, {0.0F, 0.0F, 0.0F, 0.0F}, "right of the box");
    expectSample(evaluation.image, 3, 3, {0.0F, 0.0F, 0.0F, 0.0F}, "below the box");
}

// Reformat publishes the retained enclosure as the output format, translated to
// the origin (story 46). The authored box is signed — its bottom-left corner is
// (-2, -1) on an 8x4 canvas, so its y-down enclosure is {-2, 2, 5, 3} — and the
// output must claim exactly 5x3 at (0, 0), reading the incoming image 2 columns
// right and 2 rows up.
TEST_F(CropTest, CropReformatMovesASignedEnclosureToTheOutputOrigin) {
    CropChain chain = makeCropChain(8, 4);
    Graph& graph = rootGraph(chain.document);
    setBox(graph, chain.crop, -2.0, -1.0, 3.0, 2.0);
    graph.setParam(chain.crop, "reformat", ParameterValue{true});

    const CpuEvaluation evaluation = evaluateCpu(chain.document, frameRequest(chain.document, {0, 0, 5, 3}));

    EXPECT_EQ(evaluation.plan.description.format, (Region{0, 0, 5, 3}));
    EXPECT_EQ(evaluation.plan.description.dataBounds, (Region{0, 0, 5, 3}));
    ASSERT_EQ(evaluation.image.width(), 5);
    ASSERT_EQ(evaluation.image.height(), 3);

    // Output (2, 0) is incoming (0, 2): inside the plate, inside its bar.
    expectSample(evaluation.image, 2, 0, patternSample(0, 2, 8, 4), "reformatted sample of the incoming origin");
    // Output (4, 1) is incoming (2, 3): the plate's last row.
    expectSample(evaluation.image, 4, 1, patternSample(2, 3, 8, 4), "reformatted sample of the plate's edge");
    // Output (0, 0) is incoming (-2, 2), left of the plate: the box retains the
    // pixel but the incoming image has no data there.
    expectSample(evaluation.image, 0, 0, {0.0F, 0.0F, 0.0F, 0.0F}, "retained but beyond the plate");
    // Output (4, 2) is incoming (2, 4), below the plate's last row.
    expectSample(evaluation.image, 4, 2, {0.0F, 0.0F, 0.0F, 0.0F}, "retained but below the plate");
}

// Stories 47/49 without reformat: `intersect` limits the retained domain to the
// incoming data window, and a box that misses it entirely leaves an EMPTY
// domain — a valid, fully transparent image whose format is still the inherited
// canvas, never an error.
TEST_F(CropTest, CropIntersectCanEmptyTheDomainWithoutReformat) {
    Canvas canvas = makeCanvas(8, 4);
    Graph& graph = rootGraph(canvas.document);
    const NodeId plate = graph.addNode("testpattern", "plate");
    // A first crop retains the left half; a second one, intersecting, asks for
    // the right half. Their intersection is empty.
    const NodeId left = graph.addNode("crop", "left");
    const NodeId right = graph.addNode("crop", "right");
    setBox(graph, left, 0.0, 0.0, 4.0, 4.0);
    setBox(graph, right, 6.0, 0.0, 8.0, 4.0);
    graph.setParam(right, "intersect", ParameterValue{true});
    static_cast<void>(graph.connect({plate, 0}, {left, 0}));
    static_cast<void>(graph.connect({left, 0}, {right, 0}));
    static_cast<void>(graph.connect({right, 0}, {canvas.output, 0}));

    const CpuEvaluation evaluation = evaluateCpu(canvas.document, frameRequest(canvas.document, {0, 0, 8, 4}));
    EXPECT_EQ(evaluation.plan.description.format, (Region{0, 0, 8, 4}));
    EXPECT_LE(evaluation.plan.description.dataBounds.width, 0);
    EXPECT_LE(evaluation.plan.description.dataBounds.height, 0);
    ASSERT_EQ(evaluation.image.width(), 8);
    for (int y = 0; y < evaluation.image.height(); ++y) {
        for (int x = 0; x < evaluation.image.width(); ++x) {
            expectSample(evaluation.image, x, y, {0.0F, 0.0F, 0.0F, 0.0F}, "an empty intersection is transparent");
        }
    }
}

// Story 46 with story 47: reformat publishes the BOX's own floor/ceil enclosure
// as the output format and translates by its top-left corner, so `intersect`
// clips only the DATA window inside that format. A non-empty box lying entirely
// outside the incoming image therefore still publishes a valid, positive,
// box-sized format with an empty data window — transparent on both executors,
// never a zero-size format error — and moving the box back into the image
// recovers the pixels. This is the exact distinction between the box's enclosure
// and the intersected retained domain.
TEST_F(CropTest, CropIntersectClipsDataBoundsButNeverTheBoxFormat) {
    Canvas canvas = makeCanvas(8, 4);
    Graph& graph = rootGraph(canvas.document);
    const NodeId plate = graph.addNode("testpattern", "plate");
    const NodeId left = graph.addNode("crop", "left");
    const NodeId crop = graph.addNode("crop", "Crop");
    setBox(graph, left, 0.0, 0.0, 4.0, 4.0);
    setBox(graph, crop, 6.0, 0.0, 8.0, 4.0);
    graph.setParam(crop, "intersect", ParameterValue{true});
    graph.setParam(crop, "reformat", ParameterValue{true});
    static_cast<void>(graph.connect({plate, 0}, {left, 0}));
    static_cast<void>(graph.connect({left, 0}, {crop, 0}));
    static_cast<void>(graph.connect({crop, 0}, {canvas.output, 0}));

    // The box is 2x4, entirely to the right of the retained half: the format is
    // the box's own size and the data window is empty.
    const CpuEvaluation evaluation = evaluateCpu(canvas.document, frameRequest(canvas.document, {0, 0, 2, 4}));
    EXPECT_EQ(evaluation.plan.description.format, (Region{0, 0, 2, 4}));
    EXPECT_LE(evaluation.plan.description.dataBounds.width, 0);
    EXPECT_LE(evaluation.plan.description.dataBounds.height, 0);
    ASSERT_EQ(evaluation.image.width(), 2);
    ASSERT_EQ(evaluation.image.height(), 4);
    for (int y = 0; y < evaluation.image.height(); ++y) {
        for (int x = 0; x < evaluation.image.width(); ++x) {
            expectSample(evaluation.image, x, y, {0.0F, 0.0F, 0.0F, 0.0F}, "a disjoint box is transparent");
        }
    }

    // Moving the box inside the incoming data recovers real pixels: the same
    // 2x2 box now overlaps the retained half's lower rows.
    setBox(graph, crop, 0.0, 0.0, 2.0, 2.0);
    const CpuEvaluation recovered = evaluateCpu(canvas.document, frameRequest(canvas.document, {0, 0, 2, 2}));
    EXPECT_EQ(recovered.plan.description.format, (Region{0, 0, 2, 2}));
    EXPECT_EQ(recovered.plan.description.dataBounds, (Region{0, 0, 2, 2}));
    expectSample(recovered.image, 0, 0, patternSample(0, 2, 8, 4), "recovered sample");

    if (slangSpvDir().empty()) {
        GTEST_SKIP() << "no compiled Slang kernels (CPU-only configuration)";
    }
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    // Both native front ends must agree that the empty data window is
    // transparent, not a shifted or black-clamped box of data.
    setBox(graph, crop, 6.0, 0.0, 8.0, 4.0);
    const EvaluationRequest request = frameRequest(canvas.document, {0, 0, 2, 4});
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation slangEvaluation = evaluateGpu(canvas.document, request, slang, *boot.device, *boot.allocator);
    const CpuImage slangPixels = slangEvaluation.readBack(canvas.output, *boot.device, *boot.allocator);
    expectImagesClose(evaluation.image, slangPixels, "slang empty data window");
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    eval::GpuEvaluation glslEvaluation = evaluateGpu(canvas.document, request, glsl, *boot.device, *boot.allocator);
    const CpuImage glslPixels = glslEvaluation.readBack(canvas.output, *boot.device, *boot.allocator);
    expectImagesClose(evaluation.image, glslPixels, "glsl empty data window");
    // The native samples are also checked directly: an empty data window is
    // transparent, not a clamped or black-filled box of pixels.
    expectSample(slangPixels, 1, 2, {0.0F, 0.0F, 0.0F, 0.0F}, "slang disjoint box sample");
    expectSample(glslPixels, 1, 2, {0.0F, 0.0F, 0.0F, 0.0F}, "glsl disjoint box sample");
    expectValidationClean(*boot.instance);
}

// Story 48, the extension half: with black outside disabled the retained bounds
// are an edge domain the node ANSWERS outside of, so a consumer's bounded region
// keeps real data — including through an ordinary downstream effect and outside
// the node's own format — instead of transparent black. The box is hard-edged
// here, so an extended sample is exactly the clamped retained sample.
TEST_F(CropTest, CropExtensionAnswersOutsideTheBoxThroughDownstreamEffects) {
    Canvas canvas = makeCanvas(8, 4);
    Graph& graph = rootGraph(canvas.document);
    const NodeId plate = graph.addNode("testpattern", "plate");
    const NodeId crop = graph.addNode("crop", "Crop");
    const NodeId grade = graph.addNode("grade", "Grade");
    setBox(graph, crop, 2.0, 1.0, 6.0, 3.0);
    graph.setParam(crop, "blackOutside", ParameterValue{false});
    static_cast<void>(graph.connect({plate, 0}, {crop, 0}));
    static_cast<void>(graph.connect({crop, 0}, {grade, 0}));
    static_cast<void>(graph.connect({grade, 0}, {canvas.output, 0}));

    // The region reaches outside the canvas as well as outside the box on every
    // side, so the extension is exercised at each of the box's edges.
    const CpuEvaluation evaluation = evaluateCpu(canvas.document, frameRequest(canvas.document, {-2, -1, 10, 5}));

    EXPECT_EQ(evaluation.plan.description.dataBounds, (Region{2, 1, 4, 2}));
    EXPECT_TRUE(evaluation.plan.description.edgeExtension);
    ASSERT_EQ(evaluation.image.width(), 10);
    ASSERT_EQ(evaluation.image.height(), 5);
    // Raster (0, 0) is image (-2, -1): both axes clamp to the box's top-left
    // retained sample (2, 1).
    expectSample(evaluation.image, 0, 0, patternSample(2, 1, 8, 4), "extended beyond the box's top-left");
    // Raster (0, 3) is image (-2, 2): only the column clamps.
    expectSample(evaluation.image, 0, 3, patternSample(2, 2, 8, 4), "extended left of the box");
    // Raster (9, 3) is image (7, 2): only the column clamps, on the other side.
    expectSample(evaluation.image, 9, 3, patternSample(5, 2, 8, 4), "extended right of the box");
    // Raster (5, 4) is image (3, 3): below the box, so only the row clamps.
    expectSample(evaluation.image, 5, 4, patternSample(3, 2, 8, 4), "extended below the box");
    // Raster (6, 3) is image (4, 2), inside the box: the plate's own sample,
    // unchanged by the downstream effect.
    expectSample(evaluation.image, 6, 3, patternSample(4, 2, 8, 4), "inside the box, after the downstream effect");
}

// Story 48/49, the absent-alpha half. The plate stores R, G and B only, so black
// outside (the default) adds the reference's solid alpha over the incoming image
// area, and the approved straight-alpha fade goes THROUGH that alpha: primary
// RGB is kept and the alpha carries the ramp. The ramp is measured inward from
// the fractional box edges (softness 2) and its corners are the product of both
// ramps.
TEST_F(CropTest, CropBlackOutsideAddsAlphaAndFadesStraightRgbThroughIt) {
    const fs::path platePath = dir_ / "rgb-plate.exr";
    writeRgbPlate(platePath, 8, 4);

    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    ASSERT_TRUE(
        session.submit(setNetworkFormatCommand(network, ImageFormat{8, 4, 1.0F}), EditOptions{session.revision(), {}})
            .committed);
    makeSessionCropChain(session, platePath, "straight",
                         CropFixtureSettings{.x = 2.0, .y = 1.0, .right = 6.0, .top = 3.0, .softness = 2.0});

    media::ImageSourceProvider provider;
    const CpuEvaluation evaluation =
        evaluateCpu(session.document(), frameRequest(session.document(), {0, 0, 8, 4}), nullptr, &provider);

    // Black outside adds exactly one alpha channel for an image that stores none.
    EXPECT_EQ(evaluation.plan.description.channels, (std::vector<std::string>{"R", "G", "B", "A"}));
    ASSERT_EQ(evaluation.image.layout().channels, (std::vector<std::string>{"R", "G", "B", "A"}));

    // (2, 1) is the box's top-left sample: the horizontal ramp is zero there, so
    // the added alpha is zero while the straight RGB is untouched.
    expectSample(evaluation.image, 2, 1, {3.0F / 16.0F, 2.0F / 16.0F, 0.25F, 0.0F}, "top-left box corner");
    // (4, 2): horizontal ramp 1 (two pixels inside a softness of 2), vertical
    // ramps 0.5 from the top edge and 0.5 towards the bottom edge -> alpha 0.25.
    expectSample(evaluation.image, 4, 2, {5.0F / 16.0F, 3.0F / 16.0F, 0.25F, 0.25F}, "faded through alpha");
    // (5, 2): the horizontal ramps multiply to 0.5, so alpha is 0.125.
    expectSample(evaluation.image, 5, 2, {6.0F / 16.0F, 3.0F / 16.0F, 0.25F, 0.125F}, "product of both ramps");
    // Outside the box the added alpha makes the sample transparent black.
    expectSample(evaluation.image, 1, 2, {0.0F, 0.0F, 0.0F, 0.0F}, "outside the box");
}

// Story 48/49, the premultiplied half. The plate stores its own alpha and EXR
// declares that association premultiplied, which the raw decode leaves alone, so
// the approved fade scales the stored primary RGB AND alpha together instead of
// routing it through alpha. The alpha rises by row precisely so the two
// dispositions cannot produce the same numbers. The request reaches outside the
// plate's own 8x4 format on every side, so the clamped extension is exercised
// too, and the box covers the plate's full height.
TEST_F(CropTest, CropPremultipliedFadeScalesStoredRgbAndAlphaTogether) {
    const fs::path platePath = dir_ / "rgba-plate.exr";
    writeRgbaPlate(platePath, 8, 4);

    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    ASSERT_TRUE(
        session.submit(setNetworkFormatCommand(network, ImageFormat{8, 4, 1.0F}), EditOptions{session.revision(), {}})
            .committed);
    makeSessionCropChain(
        session, platePath, "premultiplied",
        CropFixtureSettings{.x = 2.0, .right = 8.0, .top = 4.0, .softness = 2.0, .blackOutside = false});

    media::ImageSourceProvider provider;
    const CpuEvaluation evaluation =
        evaluateCpu(session.document(), frameRequest(session.document(), {-1, -1, 10, 6}), nullptr, &provider);

    // The image already stores alpha, so none is added and the association is
    // inherited untouched: the fade scales RGB and alpha alike.
    EXPECT_EQ(evaluation.plan.description.channels, (std::vector<std::string>{"R", "G", "B", "A"}));
    EXPECT_EQ(evaluation.plan.description.association, ImageAssociation::Premultiplied);
    EXPECT_EQ(evaluation.image.layout().channels, (std::vector<std::string>{"R", "G", "B", "A"}));

    // Raster (5, 3) is image (4, 2): inside the box, where both ramps are 1, so
    // every stored value is the file's own. The stored RGB are already
    // multiplied by the stored alpha of 3/4.
    expectSample(evaluation.image, 5, 3, {15.0F / 32.0F, 0.75F, 0.375F, 0.75F}, "premultiplied, ramp at 1");
    // Raster (5, 5) is image (4, 4), BELOW the plate's own format: the row clamps
    // back to the box's bottom retained sample (4, 3), where the vertical ramp is
    // 0.5, so RGB and alpha all scale by 0.5 together.
    expectSample(evaluation.image, 5, 5, {5.0F / 16.0F, 0.5F, 0.25F, 0.5F}, "premultiplied off-format extension");
    // Raster (9, 5) is image (8, 4): both axes clamp to (7, 3), where the
    // horizontal and vertical ramps are 0.5 each, so the weight is 0.25.
    expectSample(evaluation.image, 9, 5, {0.25F, 0.25F, 0.125F, 0.25F}, "premultiplied clamped corner");
}

// Story 48/49, the absent-alpha half with black outside DISABLED: an image that
// stores no alpha has nothing to fade through, so the approved disposition
// multiplies every stored value numerically and does NOT invent an alpha
// channel. The plate stores R, G and B only; the box covers the plate's full
// height and the request reaches outside the plate's own 8x4 format, so the
// clamped extension is exercised as well.
TEST_F(CropTest, CropAbsentAlphaFadesEveryStoredValueWithoutInventingAlpha) {
    const fs::path platePath = dir_ / "rgb-plate.exr";
    writeRgbPlate(platePath, 8, 4);

    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    ASSERT_TRUE(
        session.submit(setNetworkFormatCommand(network, ImageFormat{8, 4, 1.0F}), EditOptions{session.revision(), {}})
            .committed);
    makeSessionCropChain(
        session, platePath, "straight",
        CropFixtureSettings{.x = 2.0, .right = 8.0, .top = 4.0, .softness = 2.0, .blackOutside = false});

    media::ImageSourceProvider provider;
    const CpuEvaluation evaluation =
        evaluateCpu(session.document(), frameRequest(session.document(), {-1, -1, 10, 6}), nullptr, &provider);

    // No alpha channel is manufactured: black outside is off and the image
    // stores none, so the output is exactly the incoming R, G and B. (The CPU
    // raster's fourth projected value below is its documented missing-alpha
    // reading for a colour image, not a stored channel.)
    EXPECT_EQ(evaluation.plan.description.channels, (std::vector<std::string>{"R", "G", "B"}));
    EXPECT_EQ(evaluation.plan.description.association, ImageAssociation::Straight);
    EXPECT_EQ(evaluation.image.channelCount(), std::size_t{3});

    // Raster (5, 3) is image (4, 2): inside the box, where both ramps are 1, so
    // the stored sample passes through unchanged.
    expectSample(evaluation.image, 5, 3, {5.0F / 16.0F, 3.0F / 16.0F, 0.25F, 1.0F}, "stored values, ramp at 1");
    // Raster (4, 3) is image (3, 2): the horizontal ramp is 0.5, so all three
    // stored values scale by it.
    expectSample(evaluation.image, 4, 3, {0.125F, 3.0F / 32.0F, 0.125F, 1.0F}, "every stored value scales numerically");
    // Raster (5, 5) is image (4, 4), BELOW the plate's own format: the row clamps
    // back to the box's bottom retained sample (4, 3), where the vertical ramp is
    // 0.5.
    expectSample(evaluation.image, 5, 5, {5.0F / 32.0F, 0.125F, 0.125F, 1.0F}, "numeric fade of an extended sample");
    // Raster (9, 5) is image (8, 4): both axes clamp to (7, 3), where the ramps
    // are 0.5 each, so the weight is 0.25.
    expectSample(evaluation.image, 9, 5, {0.125F, 0.0625F, 0.0625F, 1.0F}, "numeric fade of a clamped corner");
}

// Acceptance example 2: Full, Half and Quarter requests agree at the anchors
// they share. The box is 8x8 on a 16x16 canvas (its edges lie on the quarter
// lattice) and the extension is on, so both the fade ramp and the clamped edge
// are exercised at every density. Outside the box each density answers with the
// nearest retained RASTER sample its own raster holds, so only genuinely shared
// anchors are compared across densities and the clamped edges are asserted on
// their own.
TEST_F(CropTest, CropFullHalfAndQuarterAgreeAtSharedAnchors) {
    CropChain chain = makeCropChain(16, 16);
    Graph& graph = rootGraph(chain.document);
    setBox(graph, chain.crop, 4.0, 4.0, 12.0, 12.0);
    graph.setParam(chain.crop, "softness", ParameterValue{4.0});
    graph.setParam(chain.crop, "blackOutside", ParameterValue{false});

    const CpuEvaluation full = evaluateCpu(chain.document, frameRequest(chain.document, {0, 0, 16, 16}, 1));
    const CpuEvaluation half = evaluateCpu(chain.document, frameRequest(chain.document, {0, 0, 16, 16}, 2));
    const CpuEvaluation quarter = evaluateCpu(chain.document, frameRequest(chain.document, {0, 0, 16, 16}, 4));
    ASSERT_EQ(full.image.width(), 16);
    ASSERT_EQ(half.image.width(), 8);
    ASSERT_EQ(quarter.image.width(), 4);

    // Inside the retained box no sample is clamped at any density, so Half and
    // Quarter read exactly the source pixel their shared full-resolution anchor
    // names and all three densities must agree there. The clamped extension
    // outside the box is deliberately NOT compared across densities: a proxy
    // raster can only answer with the nearest retained RASTER sample it actually
    // holds — full-resolution anchor 10 at Half and 8 at Quarter, against Full's
    // 11 — so a reduced request cannot recover the unsampled full-resolution edge
    // pixel. That policy is asserted on its own, with independently derived
    // values, below.
    const auto sharedAnchor = [](int anchor) { return anchor >= 4 && anchor < 12; };
    for (int y = 0; y < half.image.height(); ++y) {
        for (int x = 0; x < half.image.width(); ++x) {
            if (!sharedAnchor(2 * x) || !sharedAnchor(2 * y)) {
                continue;
            }
            const std::array<float, 4> expected = full.image.pixel(2 * x, 2 * y);
            const std::array<float, 4> actual = half.image.pixel(x, y);
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                EXPECT_NEAR(actual[channel], expected[channel], 1e-6F)
                    << "Half sample (" << x << "," << y << ") channel " << channel;
            }
        }
    }
    for (int y = 0; y < quarter.image.height(); ++y) {
        for (int x = 0; x < quarter.image.width(); ++x) {
            if (!sharedAnchor(4 * x) || !sharedAnchor(4 * y)) {
                continue;
            }
            const std::array<float, 4> expected = full.image.pixel(4 * x, 4 * y);
            const std::array<float, 4> actual = quarter.image.pixel(x, y);
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                EXPECT_NEAR(actual[channel], expected[channel], 1e-6F)
                    << "Quarter sample (" << x << "," << y << ") channel " << channel;
            }
        }
    }

    // The shared anchors also carry the analytic values: (8, 8) is inside the
    // ramp's plateau, so it is the plate's own sample; (8, 4) is ON the top
    // edge, where the vertical ramp is zero and only the straight alpha fades.
    expectSample(full.image, 8, 8, patternSample(8, 8, 16, 16), "plateau sample");
    expectSample(half.image, 4, 4, patternSample(8, 8, 16, 16), "plateau sample at Half");
    expectSample(quarter.image, 2, 2, patternSample(8, 8, 16, 16), "plateau sample at Quarter");
    const std::array<float, 4> edge = patternSample(8, 4, 16, 16);
    expectSample(full.image, 8, 4, {edge[0], edge[1], edge[2], 0.0F}, "top edge sample");
    expectSample(half.image, 4, 2, {edge[0], edge[1], edge[2], 0.0F}, "top edge sample at Half");
    expectSample(quarter.image, 2, 1, {edge[0], edge[1], edge[2], 0.0F}, "top edge sample at Quarter");
    // (0, 0) is outside the box at every density: it clamps to the box's
    // top-left retained sample, whose own alpha the ramp has already faded to
    // zero, so all three densities agree there as well.
    const std::array<float, 4> corner = patternSample(4, 4, 16, 16);
    expectSample(full.image, 0, 0, {corner[0], corner[1], corner[2], 0.0F}, "extended corner");
    expectSample(half.image, 0, 0, {corner[0], corner[1], corner[2], 0.0F}, "extended corner at Half");
    expectSample(quarter.image, 0, 0, {corner[0], corner[1], corner[2], 0.0F}, "extended corner at Quarter");

    // The clamped edge each reduced raster actually HOLDS, as independent
    // literals rather than another executor's output. The ramp is measured in
    // full-resolution pixels on the anchor the sample really read: Half's
    // right/bottom retained sample is anchor 10, where each ramp is
    // (12 - 10) / 4 = 0.5, and Quarter's is the plateau point anchor 8, where
    // each ramp is 1. The straight association keeps the primary RGB and lets the
    // alpha carry the product.
    //
    // Half (6, 0): both axes clamp -> anchor (10, 4), weight 0.5 * 0 = 0.
    expectSample(half.image, 6, 0, {10.0F / 15.0F, 4.0F / 15.0F, 0.0F, 0.0F}, "Half clamps to its own right edge");
    // Half (7, 5): the right edge clamps to anchor 10 and the row is retained at
    // anchor 10 -> weight 0.5 * 0.5 = 0.25.
    expectSample(half.image, 7, 5, {10.0F / 15.0F, 10.0F / 15.0F, 0.0F, 0.25F}, "Half clamps both edges to anchor 10");
    // Half (3, 6): the column is retained (anchor 6) and only the row clamps ->
    // anchor (6, 10), weight 0.5 * 0.5 = 0.25.
    expectSample(half.image, 3, 6, {6.0F / 15.0F, 10.0F / 15.0F, 0.0F, 0.25F}, "Half clamps only the bottom edge");
    // Quarter (3, 3): both axes clamp to anchor 8, where the ramp is 1, so the
    // sample is the plate's own at (8, 8) with its alpha intact.
    expectSample(quarter.image, 3, 3, {8.0F / 15.0F, 8.0F / 15.0F, 0.0F, 1.0F},
                 "Quarter clamps both edges to anchor 8");
    // Quarter (3, 2): only the column clamps, so the same anchor (8, 8).
    expectSample(quarter.image, 3, 2, {8.0F / 15.0F, 8.0F / 15.0F, 0.0F, 1.0F}, "Quarter clamps only the right edge");
    // Full, for contrast: the nearest retained full-resolution anchor on both
    // clamped axes is 11, where each ramp is 0.25 -> weight 0.0625.
    expectSample(full.image, 12, 12, {11.0F / 15.0F, 11.0F / 15.0F, 0.0F, 0.0625F}, "Full clamps to anchor 11");
}

// Story 49's explicit failures: an authored value that cannot be a box is
// reported against the node and the parameter, at authoring time for the typed
// parameters and where the box becomes geometry for a corner that cannot be
// represented at all.
TEST_F(CropTest, CropNamesTheParameterForNonFiniteOrUnrepresentableGeometry) {
    const std::shared_ptr<const NodeContributions> contributions = builtinNodeContributions();
    const NodeCatalog& catalog = *contributions->catalog();
    const auto validate = [&](const char* key, ParameterValue value) -> std::optional<std::string> {
        NodeInstance node;
        node.type = "crop";
        node.name = "Crop";
        node.params[key] = std::move(value);
        return contributions->validateParameters(catalog, node, node.params);
    };

    const std::optional<std::string> nonFinite =
        validate("right", ParameterValue{std::numeric_limits<double>::infinity()});
    ASSERT_TRUE(nonFinite.has_value());
    EXPECT_NE(nonFinite->find("'right'"), std::string::npos) << *nonFinite;
    EXPECT_NE(nonFinite->find("Crop"), std::string::npos) << *nonFinite;

    const std::optional<std::string> negative = validate("softness", ParameterValue{-1.0});
    ASSERT_TRUE(negative.has_value());
    EXPECT_NE(negative->find("'softness'"), std::string::npos) << *negative;

    // A finite but unrepresentable corner is refused where the box becomes
    // geometry, naming the node and the parameters that place it.
    CropChain chain = makeCropChain(8, 4);
    setBox(rootGraph(chain.document), chain.crop, 1.0e30, 0.0, 6.0, 3.0);
    bool named = false;
    try {
        static_cast<void>(evaluateCpu(chain.document, frameRequest(chain.document, {0, 0, 8, 4})));
    } catch (const EvaluationException& error) {
        const std::string message = error.what();
        named = message.find("Crop") != std::string::npos && message.find("'right'") != std::string::npos;
    }
    EXPECT_TRUE(named) << "an unrepresentable box must name the node and the offending parameters";

    // The one reformat that really has no format to publish is a degenerate box:
    // its floor/ceil enclosure is empty, so the output would be zero-sized.
    CropChain degenerate = makeCropChain(8, 4);
    Graph& graph = rootGraph(degenerate.document);
    setBox(graph, degenerate.crop, 3.0, 0.0, 3.0, 4.0);
    graph.setParam(degenerate.crop, "reformat", ParameterValue{true});
    bool zeroFormat = false;
    try {
        static_cast<void>(evaluateCpu(degenerate.document, frameRequest(degenerate.document, {0, 0, 8, 4})));
    } catch (const EvaluationException& error) {
        const std::string message = error.what();
        zeroFormat = message.find("'reformat'") != std::string::npos && message.find("Crop") != std::string::npos;
    }
    EXPECT_TRUE(zeroFormat) << "a degenerate box with reformat must name the node and the parameter";
    // Without reformat the same degenerate box is simply a fully transparent
    // image, which is story 49's empty data bounds rather than an error.
    graph.setParam(degenerate.crop, "reformat", ParameterValue{false});
    const CpuEvaluation empty = evaluateCpu(degenerate.document, frameRequest(degenerate.document, {0, 0, 8, 4}));
    EXPECT_LE(empty.plan.description.dataBounds.width, 0);
    expectSample(empty.image, 0, 0, {0.0F, 0.0F, 0.0F, 0.0F}, "a degenerate box without reformat");
}

// The authored box is measured in the INCOMING IMAGE, not in the saved
// composition: the canvas here is 13x11 while the Read is 9x7, so a box that
// covers the incoming image exactly must convert through 7 and enclose 9x7. A
// canvas-based conversion would place the enclosure at y = -4 with height 11,
// which is what this asserts against.
TEST_F(CropTest, CropBoxConvertsThroughTheIncomingFormatHeight) {
    const fs::path platePath = dir_ / "nine-by-seven.exr";
    writeRgbPlate(platePath, 9, 7);

    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    ASSERT_TRUE(
        session.submit(setNetworkFormatCommand(network, ImageFormat{13, 11, 1.0F}), EditOptions{session.revision(), {}})
            .committed);
    EXPECT_EQ(session.document().network(network).format(), (ImageFormat{13, 11, 1.0F}));
    makeSessionCropChain(session, platePath, "straight", CropFixtureSettings{.right = 9.0, .top = 7.0});

    media::ImageSourceProvider provider;
    const CpuEvaluation evaluation =
        evaluateCpu(session.document(), frameRequest(session.document(), {0, 0, 9, 7}), nullptr, &provider);
    // Converted through the incoming 9x7 format: the box covers exactly it.
    EXPECT_EQ(evaluation.plan.description.dataBounds, (Region{0, 0, 9, 7}));
    EXPECT_EQ(evaluation.plan.description.format, (Region{0, 0, 9, 7}));
    ASSERT_EQ(evaluation.image.width(), 9);
    ASSERT_EQ(evaluation.image.height(), 7);
    // The plate's own pixels, with black outside adding a solid alpha.
    expectSample(evaluation.image, 0, 0, {1.0F / 16.0F, 1.0F / 16.0F, 0.25F, 1.0F}, "incoming origin");
    expectSample(evaluation.image, 8, 6, {9.0F / 16.0F, 7.0F / 16.0F, 0.25F, 1.0F}, "incoming far corner");
}

// The workflow seam: a Crop created through ProjectSession takes its box from
// the network's SAVED canvas rather than from any transient selection, its edits
// are ordinary undoable commands, and the authored box survives save/reopen.
TEST_F(CropTest, CropCreationSeedsTheBoxFromTheSavedCanvasAndSurvivesSaveReopen) {
    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    ASSERT_TRUE(
        session.submit(setNetworkFormatCommand(network, ImageFormat{64, 48, 1.0F}), EditOptions{session.revision(), {}})
            .committed);
    const NodeId output = session.document().network(network).defaultOutput();
    auto createdPlate = std::make_shared<NodeId>();
    auto createdCrop = std::make_shared<NodeId>();
    // Creation then wiring, as two atomic transactions: `connectCommand`
    // captures its NodeId arguments when it is constructed, so the edges can only
    // be authored after the commands that create the nodes have RUN.
    ASSERT_TRUE(
        session
            .submit(transactionCommand("crop chain", {addNodeCommand(network, "testpattern", "plate", createdPlate),
                                                      addNodeCommand(network, "crop", "Crop", createdCrop)}),
                    EditOptions{session.revision(), {}})
            .committed);
    const NodeId plate = *createdPlate;
    const NodeId crop = *createdCrop;
    ASSERT_TRUE(session
                    .submit(transactionCommand("wire crop chain", {connectCommand(network, {plate, 0}, {crop, 0}),
                                                                   connectCommand(network, {crop, 0}, {output, 0})}),
                            EditOptions{session.revision(), {}})
                    .committed);

    // The saved canvas is 64x48: the new box encloses it, bottom-left to
    // top-right, without any transient selection being consulted.
    const auto created = evaluateCpu(session.document(), frameRequest(session.document(), {0, 0, 64, 48}));
    EXPECT_EQ(created.plan.description.dataBounds, (Region{0, 0, 64, 48}));

    // An ordinary parameter edit, undone and redone through the shared history.
    ASSERT_TRUE(
        session.submit(setParamCommand(network, crop, "x", ParameterValue{8.0}), EditOptions{session.revision(), {}})
            .committed);
    ASSERT_TRUE(session.undo(EditOptions{session.revision(), {}}).committed);
    const auto undone = evaluateCpu(session.document(), frameRequest(session.document(), {0, 0, 64, 48}));
    EXPECT_EQ(undone.plan.description.dataBounds, (Region{0, 0, 64, 48}));
    ASSERT_TRUE(session.redo(EditOptions{session.revision(), {}}).committed);
    const auto redone = evaluateCpu(session.document(), frameRequest(session.document(), {0, 0, 64, 48}));
    EXPECT_EQ(redone.plan.description.dataBounds, (Region{8, 0, 56, 48}));

    const fs::path target = dir_ / "crop.nemo";
    const ProjectWriteRequest job = session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(session.commitSave(job, written).committed);

    ProjectReadResult read = ProjectFile::read(target, builtinNodeCatalogPtr());
    ASSERT_TRUE(read.ok) << read.error.message;
    ProjectSession reopened;
    ASSERT_TRUE(reopened.open(std::move(read)).replaced);

    // The reopened document describes the same retained box: the left edge moved
    // to 8 and the box still spans the canvas's full height.
    const CpuEvaluation evaluation =
        evaluateCpu(reopened.document(), frameRequest(reopened.document(), {0, 0, 64, 48}));
    EXPECT_EQ(evaluation.plan.description.dataBounds, (Region{8, 0, 56, 48}));
}

// Both native front ends — the compiled Slang kernel and the runtime GLSL
// reference — reproduce the CPU adapter's samples within the declared tolerance,
// on a request that covers the box's interior, its faded edge and the clamped
// extension beyond it, including coordinates outside the plate's own format.
TEST_F(CropTest, CropNativeBackendsReproduceTheHandDerivedSamples) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    if (slangSpvDir().empty()) {
        GTEST_SKIP() << "no compiled Slang kernels (CPU-only configuration)";
    }

    Canvas canvas = makeCanvas(8, 4);
    Graph& graph = rootGraph(canvas.document);
    const NodeId plate = graph.addNode("testpattern", "plate");
    const NodeId crop = graph.addNode("crop", "Crop");
    setBox(graph, crop, 2.0, 1.0, 6.0, 3.0);
    graph.setParam(crop, "softness", ParameterValue{2.0});
    graph.setParam(crop, "blackOutside", ParameterValue{false});
    static_cast<void>(graph.connect({plate, 0}, {crop, 0}));
    static_cast<void>(graph.connect({crop, 0}, {canvas.output, 0}));

    const EvaluationRequest request = frameRequest(canvas.document, {-2, -1, 10, 5});
    const CpuImage reference = evaluateCpu(canvas.document, request).image;

    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation slangEvaluation = evaluateGpu(canvas.document, request, slang, *boot.device, *boot.allocator);
    const CpuImage slangPixels = slangEvaluation.readBack(canvas.output, *boot.device, *boot.allocator);
    expectImagesClose(reference, slangPixels, "slang vs cpu");

    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    eval::GpuEvaluation glslEvaluation = evaluateGpu(canvas.document, request, glsl, *boot.device, *boot.allocator);
    const CpuImage glslPixels = glslEvaluation.readBack(canvas.output, *boot.device, *boot.allocator);
    expectImagesClose(reference, glslPixels, "glsl vs cpu");

    // The native samples are also checked against the analytic values directly,
    // so agreement between the three implementations is not the only evidence.
    // Raster (0, 0) is image (-2, -1): both axes clamp to the box's top-left
    // retained sample (2, 1), whose own alpha the ramp has taken to zero.
    // Raster (6, 3) is image (4, 2), inside the box past the horizontal ramp's
    // plateau, so its alpha is the vertical ramp's 0.25 and its RGB is the
    // plate's own sample.
    const std::array<float, 4> corner = patternSample(2, 1, 8, 4);
    const std::array<float, 4> faded = patternSample(4, 2, 8, 4);
    expectSample(slangPixels, 0, 0, {corner[0], corner[1], corner[2], 0.0F}, "slang clamped corner");
    expectSample(slangPixels, 6, 3, {faded[0], faded[1], faded[2], 0.25F}, "slang faded sample");
    expectSample(glslPixels, 0, 0, {corner[0], corner[1], corner[2], 0.0F}, "glsl clamped corner");
    expectSample(glslPixels, 6, 3, {faded[0], faded[1], faded[2], 0.25F}, "glsl faded sample");
    expectValidationClean(*boot.instance);
}
