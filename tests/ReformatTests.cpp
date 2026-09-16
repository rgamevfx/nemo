// Reformat backend acceptance tests (issue #92, stories 50 and 52-57).
//
// The primary seam is the REAL public workflow: nodes are created, edited and
// connected through ProjectSession commands inside a Composition Network,
// persisted and reopened through ProjectFile with the same catalog, and rendered
// by the public CPU and native (Slang/GLSL) evaluators over the shared plan.
//
// The samples are hand-authored, not another executor's output: every canvas is
// authored per test, and every expected value below is derived by hand from the
// declared filters, the declared placement policy and the declared half-pixel
// coordinate convention. The reference pattern generator's channels are
// `R = x / (width - 1)`, `G = y / (height - 1)`, `B = 1 over its leading bar` and
// `A = 1`, all in FULL-RESOLUTION coordinates, so the exact resampled values of a
// given tap window are computable. Where a case is a property rather than a
// constant — a constant image survives every filter, `clamp` bounds by the
// contributing samples — the test states that property directly.
//
// Native coverage compares the CPU reference against the compiled Slang kernels
// and the runtime-compiled GLSL reference within the declared tolerance
// (absolute 2e-5 plus relative 2e-5) and asserts zero validation-layer warnings.
// A CPU-only configuration skips the native half and says so.

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "nemo/core/commands/NetworkCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/ImageSource.hpp"

using namespace nemo;

namespace {

namespace fs = std::filesystem;

// --- public document/session helpers ---------------------------------------

EditOptions options(const ProjectSession& session) {
    return EditOptions{session.revision(), {}};
}

NodeId createNode(ProjectSession& session, const std::string& type, const std::string& name) {
    auto id = std::make_shared<NodeId>();
    EXPECT_TRUE(
        session.submit(addNodeCommand(session.document().rootNetworkId(), type, name, id), options(session)).committed);
    return *id;
}

void setParam(ProjectSession& session, NodeId node, const std::string& key, ParameterValue value) {
    EXPECT_TRUE(
        session
            .submit(setParamCommand(session.document().rootNetworkId(), node, key, std::move(value)), options(session))
            .committed);
}

// Connects a FREE destination port (`connectCommand`), or rewires an occupied one
// (`replaceInputCommand`), through the same public command path the application
// uses.
void connectInput(ProjectSession& session, NodeId from, NodeId to, std::uint32_t fromPort = 0,
                  std::uint32_t toPort = 0) {
    EXPECT_TRUE(session
                    .submit(connectCommand(session.document().rootNetworkId(), {from, fromPort}, {to, toPort}),
                            options(session))
                    .committed);
}

void replaceInput(ProjectSession& session, NodeId from, NodeId to, std::uint32_t fromPort = 0,
                  std::uint32_t toPort = 0) {
    EXPECT_TRUE(session
                    .submit(replaceInputCommand(session.document().rootNetworkId(), {from, fromPort}, {to, toPort}),
                            options(session))
                    .committed);
}

EvaluationRequest fullFrameRequest(const Document& document, NodeId output, int width, int height) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = output;
    request.region = {0, 0, width, height};
    request.fullWidth = width;
    request.fullHeight = height;
    return request;
}

const PlanStep* stepFor(const EvaluationPlan& plan, const std::string& name) {
    for (const PlanStep& step : plan.steps) {
        if (step.name == name) {
            return &step;
        }
    }
    return nullptr;
}

// The declared CPU/native agreement tolerance: absolute 2e-5 plus relative 2e-5.
constexpr float kAbsoluteTolerance = 2.0e-5F;
constexpr float kRelativeTolerance = 2.0e-5F;

void expectPixelClose(const CpuImage& image, int x, int y, const std::array<float, 4>& expected, const char* what) {
    ASSERT_LT(x, image.width()) << what;
    ASSERT_LT(y, image.height()) << what;
    const std::array<float, 4> actual = image.pixel(x, y);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        const float tolerance = kAbsoluteTolerance + kRelativeTolerance * std::fabs(expected[channel]);
        EXPECT_NEAR(actual[channel], expected[channel], tolerance)
            << what << ": pixel (" << x << "," << y << ") channel " << channel;
    }
}

void expectImagesClose(const CpuImage& expected, const CpuImage& actual, const char* what) {
    ASSERT_EQ(actual.width(), expected.width()) << what;
    ASSERT_EQ(actual.height(), expected.height()) << what;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            expectPixelClose(actual, x, y, expected.pixel(x, y), what);
        }
    }
}

void expectPixelExact(const CpuImage& image, int x, int y, const std::array<float, 4>& expected, const char* what) {
    ASSERT_LT(x, image.width()) << what;
    ASSERT_LT(y, image.height()) << what;
    const std::array<float, 4> actual = image.pixel(x, y);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_FLOAT_EQ(actual[channel], expected[channel])
            << what << ": pixel (" << x << "," << y << ") channel " << channel;
    }
}

// A hand-derived value expectation for a filtered sample (the values are exact
// rationals of the authored pattern, so a tight absolute tolerance is enough and
// does not depend on the accumulation order).
void expectPixelNear(const CpuImage& image, int x, int y, const std::array<float, 4>& expected, const char* what) {
    ASSERT_LT(x, image.width()) << what;
    ASSERT_LT(y, image.height()) << what;
    const std::array<float, 4> actual = image.pixel(x, y);
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        EXPECT_NEAR(actual[channel], expected[channel], 1.0e-6F)
            << what << ": pixel (" << x << "," << y << ") channel " << channel;
    }
}

// --- the reference pattern's authored values --------------------------------

[[nodiscard]] float patternRed(int x, int width) {
    return width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0F;
}

[[nodiscard]] float patternGreen(int y, int height) {
    return height > 1 ? static_cast<float>(y) / static_cast<float>(height - 1) : 0.0F;
}

// The blue bar sits at the frame's leading edge at local time 0 and is two
// pixels wide (the pattern widens it for very small canvases).
[[nodiscard]] bool patternBar(int x, int width) {
    return x >= 0 && x < std::max(2, width / 16);
}

[[nodiscard]] std::array<float, 4> patternPixel(int x, int y, int width, int height) {
    return {patternRed(x, width), patternGreen(y, height), patternBar(x, width) ? 1.0F : 0.0F, 1.0F};
}

constexpr std::array<float, 4> kTransparent{0.0F, 0.0F, 0.0F, 0.0F};

// An authored RGB-only plate (no alpha channel), written independently of
// Nemo's evaluation and sampling code: R is the horizontal ramp, G the vertical
// one and B the leading bar of the reference pattern's authored formula.
void writeRgbPlate(const fs::path& path, int width, int height) {
    OIIO::ImageSpec spec(width, height, 3, OIIO::TypeDesc::FLOAT);
    spec.channelnames = {"R", "G", "B"};
    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto i = static_cast<std::size_t>((y * width + x) * 3);
            pixels[i] = patternRed(x, width);
            pixels[i + 1] = patternGreen(y, height);
            pixels[i + 2] = patternBar(x, width) ? 1.0F : 0.0F;
        }
    }
    auto writer = OIIO::ImageOutput::create(path.string());
    ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->open(path.string(), spec)) << writer->geterror();
    ASSERT_TRUE(writer->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << writer->geterror();
    ASSERT_TRUE(writer->close()) << writer->geterror();
}

// --- native bootstrap (the same pattern the sibling suites use) -------------

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

class ReformatTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::error_code error;
        dir_ = fs::temp_directory_path(error) /
               ("nemo-reformat-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(counter_++));
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

int ReformatTest::counter_ = 0;

// A chain built through the public session: an authored canvas, the reference
// pattern generator, one Reformat and the network Output.
class ReformatChain {
public:
    ReformatChain(int canvasWidth, int canvasHeight, float canvasAspect = 1.0F)
        : session(Document(builtinNodeCatalogPtr())) {
        network = session.document().rootNetworkId();
        EXPECT_TRUE(session
                        .submit(setNetworkFormatCommand(network, ImageFormat{canvasWidth, canvasHeight, canvasAspect}),
                                options(session))
                        .committed);
        output = session.document().network(network).defaultOutput();
        pattern = createNode(session, "testpattern", "Pattern");
        reformat = createNode(session, "reformat", "Reformat");
        connectInput(session, pattern, reformat);
        connectInput(session, reformat, output);
    }

    ProjectSession session;
    NetworkId network{kInvalidNetwork};
    NodeId pattern{kInvalidNode};
    NodeId reformat{kInvalidNode};
    NodeId output{kInvalidNode};
};

// The custom canvas every test drives the node's own format from.
void setCustomCanvas(ProjectSession& session, NodeId node, int width, int height, float pixelAspect = 1.0F) {
    setParam(session, node, "formatSource", ParameterValue{ChoiceValue{"custom"}});
    setParam(session, node, "width", ParameterValue{static_cast<std::int64_t>(width)});
    setParam(session, node, "height", ParameterValue{static_cast<std::int64_t>(height)});
    setParam(session, node, "pixelAspect", ParameterValue{static_cast<double>(pixelAspect)});
}

}  // namespace

// ---------------------------------------------------------------------------
// Story 50/51/57: the composition target is the OWNING network's authored
// canvas, resolved through the document — never the input's own size, never the
// selected viewer.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, CompositionTargetFollowsTheOwningNetworkCanvas) {
    ReformatChain chain(4, 2);

    const CpuEvaluation first =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 4, 2));
    const PlanStep* described = stepFor(first.plan, "Reformat");
    ASSERT_NE(described, nullptr);
    EXPECT_EQ(described->description.format, (Region{0, 0, 4, 2}));
    EXPECT_FLOAT_EQ(described->description.pixelAspect, 1.0F);
    EXPECT_FALSE(described->description.edgeExtension);
    EXPECT_EQ(described->description.dataBounds, (Region{0, 0, 4, 2}));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectPixelExact(first.image, x, y, patternPixel(x, y, 4, 2), "composition identity");
        }
    }

    // Re-authoring the owning network's canvas moves the node's output with it,
    // with no edit to the node's own parameters.
    ASSERT_TRUE(
        chain.session.submit(setNetworkFormatCommand(chain.network, ImageFormat{6, 3, 2.0F}), options(chain.session))
            .committed);
    const EvaluationRequest widened = fullFrameRequest(chain.session.document(), chain.output, 6, 3);
    const CpuEvaluation second = evaluateCpu(chain.session.document(), widened);
    const PlanStep* moved = stepFor(second.plan, "Reformat");
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->description.format, (Region{0, 0, 6, 3}));
    EXPECT_FLOAT_EQ(moved->description.pixelAspect, 2.0F);
    EXPECT_EQ(second.image.width(), 6);
    EXPECT_EQ(second.image.height(), 3);
    EXPECT_FLOAT_EQ(second.image.layout().pixelAspect, 2.0F);
}

// ---------------------------------------------------------------------------
// Story 50/51: `custom` is the node's own authored canvas — independent of the
// composition target — and a document-owned format preset is a VALUE: applying
// it to the network never rewrites the node's authored width/height/aspect.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, CustomFormatIsIndependentOfTheCompositionAndOfPresets) {
    ReformatChain chain(4, 2);
    setCustomCanvas(chain.session, chain.reformat, 3, 5, 2.0F);

    const CpuEvaluation custom =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 3, 5));
    const PlanStep* described = stepFor(custom.plan, "Reformat");
    ASSERT_NE(described, nullptr);
    EXPECT_EQ(described->description.format, (Region{0, 0, 3, 5}));
    EXPECT_FLOAT_EQ(described->description.pixelAspect, 2.0F);

    // A document-owned named format, applied to the owning network, is a copy:
    // the network canvas changes and the node's authored values do not.
    ASSERT_TRUE(chain.session.submit(setNamedFormatCommand("delivery", ImageFormat{9, 7, 1.0F}), options(chain.session))
                    .committed);
    ASSERT_TRUE(
        chain.session.submit(applyNamedFormatCommand(chain.network, "delivery"), options(chain.session)).committed);
    const Document& document = chain.session.document();
    EXPECT_EQ(document.network(chain.network).format(), (ImageFormat{9, 7, 1.0F}));
    const NodeInstance* node = document.network(chain.network).graph().node(chain.reformat);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->params.at("width"), ParameterValue{std::int64_t{3}});
    EXPECT_EQ(node->params.at("height"), ParameterValue{std::int64_t{5}});
    EXPECT_EQ(node->params.at("pixelAspect"), ParameterValue{2.0});
    EXPECT_EQ(node->params.at("formatSource"), ParameterValue{ChoiceValue{"custom"}});
}

TEST_F(ReformatTest, UnforcedBoxFollowsTheChosenResizePolicy) {
    ReformatChain chain(4, 2);
    setParam(chain.session, chain.reformat, "type", ParameterValue{ChoiceValue{"box"}});
    setParam(chain.session, chain.reformat, "boxWidth", ParameterValue{std::int64_t{3}});
    setParam(chain.session, chain.reformat, "boxHeight", ParameterValue{std::int64_t{6}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    const auto check = [&](const char* resize, int width, int height) {
        setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{resize}});
        const auto evaluated = evaluateCpu(chain.session.document(),
                                           fullFrameRequest(chain.session.document(), chain.output, width, height));
        const auto* step = stepFor(evaluated.plan, "Reformat");
        ASSERT_NE(step, nullptr);
        EXPECT_EQ(step->description.format, (Region{0, 0, width, height})) << resize;
        expectPixelExact(evaluated.image, 0, 0, patternPixel(0, 0, 4, 2), resize);
    };
    check("height", 12, 6);
    check("width", 3, 2);
    check("fit", 3, 2);
    check("fill", 12, 6);
    check("distort", 3, 6);
    check("none", 4, 2);
}

// ---------------------------------------------------------------------------
// Story 53/54: the resize policy places the image exactly as declared —
// `none` one source pixel per output pixel, `width` matching the output's
// physical width, `fill` covering it — and the produced data window says which
// samples carry data.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, ResizePoliciesPlaceTheImageAsDeclared) {
    ReformatChain chain(4, 2);
    setCustomCanvas(chain.session, chain.reformat, 4, 4);
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    setParam(chain.session, chain.reformat, "center", ParameterValue{false});

    // `none`, not centered: one source pixel per output pixel with the image's
    // lower-left corner at the output's.
    const CpuEvaluation bottomLeft =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 4, 4));
    const PlanStep* none = stepFor(bottomLeft.plan, "Reformat");
    ASSERT_NE(none, nullptr);
    EXPECT_EQ(none->description.dataBounds, (Region{0, 2, 4, 2}));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectPixelExact(bottomLeft.image, x, y, kTransparent, "none above the image");
            expectPixelExact(bottomLeft.image, x, y + 2, patternPixel(x, y, 4, 2), "none");
        }
    }

    // `center` moves the same 4x2 image to the vertical middle.
    setParam(chain.session, chain.reformat, "center", ParameterValue{true});
    const CpuEvaluation centered =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 4, 4));
    const PlanStep* middle = stepFor(centered.plan, "Reformat");
    ASSERT_NE(middle, nullptr);
    EXPECT_EQ(middle->description.dataBounds, (Region{0, 1, 4, 2}));
    for (int x = 0; x < 4; ++x) {
        expectPixelExact(centered.image, x, 0, kTransparent, "centered top");
        expectPixelExact(centered.image, x, 3, kTransparent, "centered bottom");
    }
    for (int y = 1; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectPixelExact(centered.image, x, y, patternPixel(x, y - 1, 4, 2), "centered");
        }
    }

    // `fill` covers the 8x8 output: the 4x2 source is magnified four times, so
    // the output shows the image's middle columns and both of its rows.
    setCustomCanvas(chain.session, chain.reformat, 8, 8);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"fill"}});
    const CpuEvaluation filled =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 8, 8));
    const PlanStep* fill = stepFor(filled.plan, "Reformat");
    ASSERT_NE(fill, nullptr);
    EXPECT_EQ(fill->description.dataBounds, (Region{0, 0, 8, 8}));
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            // `fill` covers: the 4x2 source is magnified 4x, so the output sees
            // the image's middle columns (source column 1 + X/4) and both rows.
            expectPixelExact(filled.image, x, y, patternPixel(1 + x / 4, y / 4, 4, 2), "fill");
        }
    }

    // `width` matches the output's physical width and keeps the input's aspect
    // ratio: the same 8-wide image is 4 rows tall, centered.
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"width"}});
    const CpuEvaluation widthPolicy =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 8, 8));
    const PlanStep* widthStep = stepFor(widthPolicy.plan, "Reformat");
    ASSERT_NE(widthStep, nullptr);
    EXPECT_EQ(widthStep->description.dataBounds, (Region{0, 2, 8, 4}));
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool inside = y >= 2 && y < 6;
            expectPixelExact(widthPolicy.image, x, y, inside ? patternPixel(x / 2, (y - 2) / 2, 4, 2) : kTransparent,
                             "width policy");
        }
    }
}

// ---------------------------------------------------------------------------
// Story 52/53: an anamorphic image keeps its SHAPE. A 4x2 image at pixel aspect
// 2 is physically 8x2, so `fit` into a square-pixel 8x2 output is exactly 1:1 —
// an implementation that ignored the pixel aspect would leave black columns.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, AnamorphicAspectIsPhysical) {
    ReformatChain chain(4, 2);
    // The source plate is anamorphic: 4x2 at pixel aspect 2.
    setCustomCanvas(chain.session, chain.reformat, 4, 2, 2.0F);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"none"}});

    const NodeId second = createNode(chain.session, "reformat", "Square");
    setCustomCanvas(chain.session, second, 8, 2, 1.0F);
    setParam(chain.session, second, "resize", ParameterValue{ChoiceValue{"fit"}});
    setParam(chain.session, second, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    replaceInput(chain.session, second, chain.output);
    connectInput(chain.session, chain.reformat, second);

    const CpuEvaluation evaluation =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 8, 2));
    const PlanStep* described = stepFor(evaluation.plan, "Square");
    ASSERT_NE(described, nullptr);
    EXPECT_EQ(described->description.format, (Region{0, 0, 8, 2}));
    EXPECT_EQ(described->description.dataBounds, (Region{0, 0, 8, 2}))
        << "a physically 1:1 fit must cover the whole output";
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            expectPixelExact(evaluation.image, x, y, patternPixel(x / 2, y, 4, 2), "anamorphic fit");
        }
    }
}

// ---------------------------------------------------------------------------
// Story 54: orientation is flop, then flip, then a 90-degree COUNTER-CLOCKWISE
// turn, applied before the resize and the alignment. The reference pattern's red
// channel is a horizontal ramp, so a counter-clockwise turn makes it increase
// upwards in the stored (y down) raster.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, OrientationIsFlopFlipThenCounterClockwiseTurn) {
    ReformatChain chain(4, 2);
    setCustomCanvas(chain.session, chain.reformat, 2, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"none"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    setParam(chain.session, chain.reformat, "turn", ParameterValue{true});

    const CpuEvaluation turned =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 2, 4));
    const PlanStep* described = stepFor(turned.plan, "Reformat");
    ASSERT_NE(described, nullptr);
    EXPECT_EQ(described->description.dataBounds, (Region{0, 0, 2, 4}));
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 2; ++x) {
            // Output row y reads source column 3 - y: counter-clockwise, not
            // clockwise (which would read column y).
            expectPixelExact(turned.image, x, y, patternPixel(3 - y, x, 4, 2), "turn counter-clockwise");
        }
    }

    // Flop happens BEFORE the turn, so with both enabled the source column the
    // output reads is the output ROW (and not the mirrored output column): the
    // documented order is observable only this way.
    setParam(chain.session, chain.reformat, "flop", ParameterValue{true});
    const CpuImage turnedFlopped =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 2, 4)).image;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 2; ++x) {
            expectPixelExact(turnedFlopped, x, y, patternPixel(y, x, 4, 2), "flop then turn");
        }
    }

    // Flop alone mirrors the source horizontally; flip alone vertically.
    ReformatChain flop(4, 2);
    setCustomCanvas(flop.session, flop.reformat, 4, 2);
    setParam(flop.session, flop.reformat, "resize", ParameterValue{ChoiceValue{"none"}});
    setParam(flop.session, flop.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    setParam(flop.session, flop.reformat, "flop", ParameterValue{true});
    const CpuImage flopped =
        evaluateCpu(flop.session.document(), fullFrameRequest(flop.session.document(), flop.output, 4, 2)).image;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectPixelExact(flopped, x, y, patternPixel(3 - x, y, 4, 2), "flop");
        }
    }

    setParam(flop.session, flop.reformat, "flop", ParameterValue{false});
    setParam(flop.session, flop.reformat, "flip", ParameterValue{true});
    const CpuImage flipped =
        evaluateCpu(flop.session.document(), fullFrameRequest(flop.session.document(), flop.output, 4, 2)).image;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectPixelExact(flipped, x, y, patternPixel(x, 1 - y, 4, 2), "flip");
        }
    }
}

// ---------------------------------------------------------------------------
// Story 48/55: black outside off extends the outermost samples and the produced
// description says so (`edgeExtension`), so a consumer reads the extended image
// instead of treating the finite data bounds as transparent.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, BlackOutsideOffExtendsTheEdgeAndDeclaresIt) {
    ReformatChain chain(4, 2);
    setCustomCanvas(chain.session, chain.reformat, 6, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"none"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});

    // Black outside on (the default): the placed 4x2 image is centered in the
    // 6x4 output and everything outside it is transparent black.
    const CpuEvaluation black =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 6, 4));
    const PlanStep* blackStep = stepFor(black.plan, "Reformat");
    ASSERT_NE(blackStep, nullptr);
    EXPECT_FALSE(blackStep->description.edgeExtension);
    EXPECT_EQ(blackStep->description.dataBounds, (Region{1, 1, 4, 2}));
    expectPixelExact(black.image, 0, 0, kTransparent, "black outside");
    expectPixelExact(black.image, 1, 1, patternPixel(0, 0, 4, 2), "black inside");
    expectPixelExact(black.image, 5, 3, kTransparent, "black outside right");

    // Black outside off: every requested coordinate answers with the outermost
    // sample, and the description claims the extension.
    setParam(chain.session, chain.reformat, "blackOutside", ParameterValue{false});
    const CpuEvaluation extended =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 6, 4));
    const PlanStep* extendedStep = stepFor(extended.plan, "Reformat");
    ASSERT_NE(extendedStep, nullptr);
    EXPECT_TRUE(extendedStep->description.edgeExtension);
    EXPECT_TRUE(hasEdgeExtension(extendedStep->description));
    EXPECT_EQ(extendedStep->description.dataBounds, (Region{0, 0, 6, 4}));
    for (int y = 0; y < 4; ++y) {
        const int sourceY = std::clamp(y - 1, 0, 1);
        for (int x = 0; x < 6; ++x) {
            const int sourceX = std::clamp(x - 1, 0, 3);
            expectPixelExact(extended.image, x, y, patternPixel(sourceX, sourceY, 4, 2), "edge extension");
        }
    }
}

TEST_F(ReformatTest, BlackOutsideFiltersTheRetainedBoundaryNotAnUpstreamExtension) {
    ReformatChain chain(12, 5);
    const NodeId crop = createNode(chain.session, "crop", "Crop");
    setParam(chain.session, crop, "x", 2.0);
    setParam(chain.session, crop, "right", 10.0);
    setParam(chain.session, crop, "y", 0.0);
    setParam(chain.session, crop, "top", 5.0);
    connectInput(chain.session, chain.pattern, crop);
    replaceInput(chain.session, crop, chain.reformat);
    setCustomCanvas(chain.session, chain.reformat, 24, 10);
    setParam(chain.session, chain.reformat, "resize", ChoiceValue{"distort"});
    setParam(chain.session, chain.reformat, "filter", ChoiceValue{"Cubic"});

    // At output (3,4), source x=1.75 lies outside the retained x>=2.
    // BC(0,0) gives the sole retained column weight 5/32. Straight RGB
    // stays that column's color; Y blends rows 1/2 with weights 5/32,27/32.
    auto expected = patternPixel(2, 2, 12, 5);
    const auto previous = patternPixel(2, 1, 12, 5);
    for (std::size_t channel = 0; channel < 3; ++channel)
        expected[channel] = previous[channel] * (5.0F / 32.0F) + expected[channel] * (27.0F / 32.0F);
    expected[3] = 5.0F / 32.0F;
    const auto request = fullFrameRequest(chain.session.document(), chain.output, 24, 10);
    for (const bool upstreamBlackOutside : {false, true}) {
        setParam(chain.session, crop, "blackOutside", upstreamBlackOutside);
        expectPixelClose(evaluateCpu(chain.session.document(), request).image, 3, 4, expected,
                         "Reformat blacks outside its retained input boundary");
    }
    if (slangSpvDir().empty())
        return;
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const auto glsl = eval::glslEffectLibrary();
    for (const bool upstreamBlackOutside : {false, true}) {
        setParam(chain.session, crop, "blackOutside", upstreamBlackOutside);
        for (const auto* library : {&slang, &glsl}) {
            auto native = evaluateGpu(chain.session.document(), request, *library, *boot.device, *boot.allocator);
            expectPixelClose(native.readBack(request.output, *boot.device, *boot.allocator), 3, 4, expected,
                             "native retained boundary");
        }
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 48/55: black outside on adds the documented solid alpha over the input
// image area when the input carries no alpha, and no alpha at all when black
// outside is off.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, BlackOutsideAddsASolidAlphaToAnAlphaLessInput) {
    const fs::path plate = dir_ / "rgb-plate.exr";
    writeRgbPlate(plate, 4, 2);

    ReformatChain chain(4, 2);
    const NodeId source = createNode(chain.session, "source", "Plate");
    SourceReference reference;
    reference.path = plate.string();
    ASSERT_TRUE(chain.session
                    .submit(transactionCommand("rgb plate",
                                               {setSourceCommand("rgb", reference),
                                                setParamCommand(chain.network, source, "source", std::string{"rgb"}),
                                                setParamCommand(chain.network, source, "inputTransform",
                                                                ParameterValue{ChoiceValue{"raw"}}),
                                                replaceInputCommand(chain.network, {source, 0}, {chain.reformat, 0})}),
                            options(chain.session))
                    .committed);
    setCustomCanvas(chain.session, chain.reformat, 4, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"none"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    setParam(chain.session, chain.reformat, "center", ParameterValue{false});

    media::ImageSourceProvider provider;
    const CpuEvaluation solid = evaluateCpu(
        chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 4, 4), nullptr, &provider);
    const PlanStep* described = stepFor(solid.plan, "Reformat");
    ASSERT_NE(described, nullptr);
    ASSERT_EQ(described->description.channels, (std::vector<std::string>{"R", "G", "B", "A"}));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectPixelExact(solid.image, x, y, kTransparent, "solid alpha above the image");
            expectPixelExact(solid.image, x, y + 2, patternPixel(x, y, 4, 2), "solid alpha inside");
        }
    }

    // Black outside off adds no alpha: the composed channel set stays the
    // input's, and the image is not manufactured opaque.
    setParam(chain.session, chain.reformat, "blackOutside", ParameterValue{false});
    const CpuEvaluation open = evaluateCpu(
        chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 4, 4), nullptr, &provider);
    const PlanStep* openStep = stepFor(open.plan, "Reformat");
    ASSERT_NE(openStep, nullptr);
    EXPECT_EQ(openStep->description.channels, (std::vector<std::string>{"R", "G", "B"}));
}

// ---------------------------------------------------------------------------
// Story 55/56: the declared kernels are the ones that run, at the declared
// half-pixel coordinate convention. Impulse carries the nearest sample; Notch is
// a unit-width box that widens by the minification factor; Cubic is the BC(0,0)
// member of the family, whose two-tap weights at a quarter-sample offset are
// 27/32 and 5/32 (a Catmull-Rom kernel would answer differently).
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, DeclaredKernelsResampleExactly) {
    // A 6x1 source (blue bar over its first two columns) resampled to 2x1: three
    // source samples per output sample.
    ReformatChain chain(6, 1);
    setCustomCanvas(chain.session, chain.reformat, 2, 1);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"distort"}});
    const Document& document = chain.session.document();

    // Impulse: the sample nearest the mapped position — source x = 1 and x = 4.
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    const CpuImage impulse = evaluateCpu(document, fullFrameRequest(document, chain.output, 2, 1)).image;
    expectPixelExact(impulse, 0, 0, patternPixel(1, 0, 6, 1), "impulse left");
    expectPixelExact(impulse, 1, 0, patternPixel(4, 0, 6, 1), "impulse right");

    // Notch widens to three samples and averages them equally: the bar covers two
    // of the three source samples, so the blue channel is 2/3 and the ramp's
    // exact rational is reproduced.
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Notch"}});
    const CpuImage notch = evaluateCpu(document, fullFrameRequest(document, chain.output, 2, 1)).image;
    expectPixelNear(notch, 0, 0, {(0.0F + 1.0F / 5.0F + 2.0F / 5.0F) / 3.0F, 0.0F, 2.0F / 3.0F, 1.0F}, "notch left");
    expectPixelNear(notch, 1, 0, {(3.0F / 5.0F + 4.0F / 5.0F + 1.0F) / 3.0F, 0.0F, 0.0F, 1.0F}, "notch right");

    // Cubic, BC(0,0), at a half-sized source: output samples land a quarter of a
    // sample either side of a source center, where the two-tap smoothstep has
    // weights 27/32 and 5/32 (and zero beyond one sample).
    ReformatChain magnified(2, 1);
    setCustomCanvas(magnified.session, magnified.reformat, 4, 1);
    setParam(magnified.session, magnified.reformat, "resize", ParameterValue{ChoiceValue{"distort"}});
    const Document& magnifiedDocument = magnified.session.document();
    const CpuImage cubic =
        evaluateCpu(magnifiedDocument, fullFrameRequest(magnifiedDocument, magnified.output, 4, 1)).image;
    // Source: R = (0, 1), B = both bar-covered, and the pattern is STRAIGHT alpha
    // at full coverage. The primary RGBA is resampled association-aware: the kernel
    // premultiplies for the tap window and divides the result by the filtered
    // coverage afterwards. An edge sample whose missing tap leaves coverage at
    // 27/32 therefore keeps the SOURCE COLOUR of the sample it spans — the straight
    // RGB is not faded by the tap weight, only the alpha is — while an interior
    // sample whose window is fully covered keeps colour and alpha at 1.
    expectPixelNear(cubic, 0, 0, {0.0F, 0.0F, 1.0F, 27.0F / 32.0F}, "cubic edge left");
    expectPixelNear(cubic, 1, 0, {5.0F / 32.0F, 0.0F, 1.0F, 1.0F}, "cubic inside left");
    expectPixelNear(cubic, 2, 0, {27.0F / 32.0F, 0.0F, 1.0F, 1.0F}, "cubic inside right");
    expectPixelNear(cubic, 3, 0, {1.0F, 0.0F, 1.0F, 27.0F / 32.0F}, "cubic edge right");
}

// ---------------------------------------------------------------------------
// Story 55/56 (issue #92): the sampling window is GEOMETRY, resolved in host
// double precision, so the declared CLOSED unit-width box decides its own
// boundary samples — and a pixel aspect a hair either side of 1 lands on opposite
// sides of that decision. A 12x5 source fitted into a 6x4 canvas is placed
// 2.5 * pixelAspect output rows tall, so at pixel aspect 0.9999999 the first and
// last output row's box misses every source row (the nearest valid sample sits two
// ten-millionths of a sample OUTSIDE the closed support) while at 1.0000001 it
// holds one sample of two; only exactly 1 holds the sample a whole sample away
// from the mapped position. Two ten-millionths of a sample is far below what a
// float-wide window can resolve, and such a window answers with the half-covered
// COLORED first row this case was filed for.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, NotchSupportBoundaryFollowsTheAuthoredPixelAspect) {
    // Derivation, in source samples. `fit` takes contain = 6 * PAR / 12 = PAR / 2
    // over cover = 4 / 5, so gy = PAR / 2, gx = (PAR / 2) / PAR = 1 / 2 exactly, the
    // placed image is 2.5 * PAR rows tall and ay = (4 - 2.5 * PAR) / 2. Output row
    // y's sample sits at full-resolution y + 0.5 and maps to source
    // y = (2y - 3) / PAR + 2.5; the unit-width box (radius 1/2) widens by 1 / PAR to
    // a half width 1 / PAR, and the source rows its closed window [center - h,
    // center + h] holds, stated as sample indices, are
    //     first = ceil((2y - 4) / PAR + 2), last = floor((2y - 2) / PAR + 2):
    //     PAR = 1         -> {-2..0}, {0..2}, {2..4}, {4..6}
    //     PAR = 0.9999999 -> {-2..-1}, {0..2}, {2..4}, {5..6}
    //     PAR = 1.0000001 -> {-1..0}, {1..2}, {2..3}, {4..5}
    // Row 0's upper window edge is last(0) = floor(2 - 2 / PAR): exactly 0 at
    // PAR = 1 (so the CLOSED window holds source row 0), -0.0000002 at 0.9999999
    // (row 0 is out by two ten-millionths of a sample) and +0.0000002 at 1.0000001.
    // The input raster holds rows 0..4, and the box is normalized over its WHOLE support, so each row's alpha is
    // the fraction of the box the raster holds — and the primary RGBA is resampled
    // association-aware, so a row whose window holds one sample of three is not
    // faded, it is seen through 1/3 coverage. With G(y) = y / 4 the covered rows give
    //     PAR         row 0                row 1                 row 2                 row 3
    //     0.9999999   no source row (A=0)  (G0+G1+G2)/3 = 1/4   (G2+G3+G4)/3 = 3/4   no source row (A=0)
    //     1           {0}, A = 1/3         (G0+G1+G2)/3 = 1/4   (G2+G3+G4)/3 = 3/4   {4}, A = 1/3
    //     1.0000001   {0}, A = 1/2         (G1+G2)/2 = 3/8      (G2+G3)/2 = 5/8      {4}, A = 1/2
    // (the single-row windows above carry G(0) = 0 and G(4) = 1.) Every output column
    // maps to the whole position 2x + 1 (gx = 1/2, ax = 0), so its window is
    // {2x, 2x + 1} with weight 1/2 each: R = (R(2x) + R(2x + 1)) / 2 and B = 1 at
    // x = 0 only, where both taps lie under the pattern's leading bar.
    struct Row {
        float green;
        float alpha;  // the covered fraction; 0 when the window holds no source row
    };
    struct Case {
        double pixelAspect;
        const char* what;
        std::array<Row, 4> rows;
    };
    const std::array<Case, 3> cases{{
        {0.9999999,
         "notch just below pixel aspect 1",
         {{{0.0F, 0.0F},
           {(0.0F + 1.0F / 4.0F + 2.0F / 4.0F) / 3.0F, 1.0F},
           {(2.0F / 4.0F + 3.0F / 4.0F + 4.0F / 4.0F) / 3.0F, 1.0F},
           {0.0F, 0.0F}}}},
        {1.0,
         "notch at pixel aspect exactly 1",
         {{{0.0F, 1.0F / 3.0F},
           {(0.0F + 1.0F / 4.0F + 2.0F / 4.0F) / 3.0F, 1.0F},
           {(2.0F / 4.0F + 3.0F / 4.0F + 4.0F / 4.0F) / 3.0F, 1.0F},
           {4.0F / 4.0F, 1.0F / 3.0F}}}},
        {1.0000001,
         "notch just above pixel aspect 1",
         {{{0.0F, 1.0F / 2.0F},
           {(1.0F / 4.0F + 2.0F / 4.0F) / 2.0F, 1.0F},
           {(2.0F / 4.0F + 3.0F / 4.0F) / 2.0F, 1.0F},
           {4.0F / 4.0F, 1.0F / 2.0F}}}},
    }};
    // The hand-derived sample of one covered pixel: the box average of the covered
    // rows' horizontal ramp, the leading bar under the two taps, and the covered
    // fraction the box kept.
    const auto covered = [](int x, const Row& row) {
        return std::array<float, 4>{((2 * x) / 11.0F + (2 * x + 1) / 11.0F) / 2.0F, row.green, x == 0 ? 1.0F : 0.0F,
                                    row.alpha};
    };

    ReformatChain chain(12, 5);
    setCustomCanvas(chain.session, chain.reformat, 6, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"fit"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Notch"}});

    // The CPU reference first: the values above ARE the oracle, so a backend that
    // never agrees with them is caught even where the natives agree with each other.
    for (const Case& item : cases) {
        setParam(chain.session, chain.reformat, "pixelAspect", ParameterValue{item.pixelAspect});
        const CpuEvaluation evaluation =
            evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 6, 4));
        ASSERT_EQ(evaluation.image.width(), 6) << item.what;
        ASSERT_EQ(evaluation.image.height(), 4) << item.what;
        for (int y = 0; y < 4; ++y) {
            const Row& row = item.rows[static_cast<std::size_t>(y)];
            for (int x = 0; x < 6; ++x) {
                if (row.alpha == 0.0F) {
                    // A row the window misses entirely: no tap of this node's kernel
                    // reaches the raster, so the sample is transparent black.
                    expectPixelExact(evaluation.image, x, y, kTransparent, item.what);
                } else {
                    expectPixelNear(evaluation.image, x, y, covered(x, row), item.what);
                }
            }
        }
    }

    // Both native front ends must answer the same oracle — the tile this issue was
    // filed from was a half-covered COLORED first row, which no tolerance hides.
    if (slangSpvDir().empty()) {
        return;  // CPU-only configuration: the boundary oracle above is proven.
    }
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    const std::array<std::pair<const char*, const eval::EffectLibrary>, 2> backends{
        std::pair<const char*, const eval::EffectLibrary>{"slang", slang},
        std::pair<const char*, const eval::EffectLibrary>{"glsl", glsl}};
    for (const Case& item : cases) {
        setParam(chain.session, chain.reformat, "pixelAspect", ParameterValue{item.pixelAspect});
        const EvaluationRequest request = fullFrameRequest(chain.session.document(), chain.output, 6, 4);
        for (const auto& [backend, library] : backends) {
            eval::GpuEvaluation native =
                evaluateGpu(chain.session.document(), request, library, *boot.device, *boot.allocator);
            const CpuImage image = native.readBack(request.output, *boot.device, *boot.allocator);
            ASSERT_EQ(image.width(), 6) << backend;
            ASSERT_EQ(image.height(), 4) << backend;
            const std::string what = std::string{backend} + " vs the derived box: " + item.what;
            for (int y = 0; y < 4; ++y) {
                const Row& row = item.rows[static_cast<std::size_t>(y)];
                for (int x = 0; x < 6; ++x) {
                    expectPixelClose(image, x, y, row.alpha == 0.0F ? kTransparent : covered(x, row), what.c_str());
                }
            }
        }
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 55/56 (issue #92): a NEAREST read is decided by the same closed-boundary
// geometry. With the placement's lower-left corner aligned to the output's, the
// 12x5 -> 6x4 `fit` case above maps its output rows onto whole-sample positions at
// pixel aspect 1: output row 1 lands exactly on the image's top edge and must read
// source row 0 — the row whose own sample starts there — and the last output row
// must read the image's last row. A pixel aspect a hair BELOW 1 moves each of those
// positions a hair below its whole number, so the nearest sample becomes the
// previous row: the first two output rows then have no sample of the raster at all.
// A hair above 1 leaves every pick where it was.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, ImpulseNearestTapFollowsTheClosedPlacementEdge) {
    // Derivation, in source samples. gy = PAR / 2 and gx = 1 / 2 as in the Notch
    // case above; `center` off puts the placed image's bottom edge on the output's,
    // ay = 4 - 2.5 * PAR, so output row y maps to
    //     source y = (y + 0.5 - ay) / gy = (2y - 7) / PAR + 5:
    //     PAR = 1         -> -2, 0, 2, 4
    //     PAR = 0.9999999 -> -2.0000007, -5e-7, 1.9999997, 3.9999999
    //     PAR = 1.0000001 -> -1.9999993, 5e-7, 2.0000003, 4.0000001
    // Impulse reads the single raster sample i whose own row [i, i + 1) holds that
    // position (the nearest sample centre), so the picks are -2/0/2/4 exactly at 1
    // and above it, and -3/-1/1/3 below it: output rows 0 and 1 have NO sample of the
    // raster and stay transparent, while rows 2 and 3 land one row lower. Columns map
    // to the whole position 2x + 1 at every aspect, so every covered pixel carries
    // exactly the pattern's sample at (2x + 1, its row): R = (2x + 1) / 11, B = 1
    // only where that column lies under the leading bar (x = 0, column 1), and the
    // vertical ramp G = row / 4.
    struct Case {
        double pixelAspect;
        const char* what;
        std::array<int, 4> sourceRow;  // -1: the nearest sample is outside the raster
    };
    const std::array<Case, 3> cases{{
        {0.9999999, "impulse just below pixel aspect 1", {{-1, -1, 1, 3}}},
        {1.0, "impulse at pixel aspect exactly 1", {{-1, 0, 2, 4}}},
        {1.0000001, "impulse just above pixel aspect 1", {{-1, 0, 2, 4}}},
    }};

    ReformatChain chain(12, 5);
    setCustomCanvas(chain.session, chain.reformat, 6, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"fit"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    setParam(chain.session, chain.reformat, "center", ParameterValue{false});

    for (const Case& item : cases) {
        setParam(chain.session, chain.reformat, "pixelAspect", ParameterValue{item.pixelAspect});
        const CpuEvaluation evaluation =
            evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 6, 4));
        ASSERT_EQ(evaluation.image.width(), 6) << item.what;
        ASSERT_EQ(evaluation.image.height(), 4) << item.what;
        for (int y = 0; y < 4; ++y) {
            const int sourceRow = item.sourceRow[static_cast<std::size_t>(y)];
            for (int x = 0; x < 6; ++x) {
                if (sourceRow < 0) {
                    expectPixelExact(evaluation.image, x, y, kTransparent, item.what);
                } else {
                    expectPixelExact(evaluation.image, x, y, patternPixel(2 * x + 1, sourceRow, 12, 5), item.what);
                }
            }
        }
    }

    if (slangSpvDir().empty()) {
        return;  // CPU-only configuration: the nearest oracle above is proven.
    }
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    const std::array<std::pair<const char*, const eval::EffectLibrary>, 2> backends{
        std::pair<const char*, const eval::EffectLibrary>{"slang", slang},
        std::pair<const char*, const eval::EffectLibrary>{"glsl", glsl}};
    for (const Case& item : cases) {
        setParam(chain.session, chain.reformat, "pixelAspect", ParameterValue{item.pixelAspect});
        const EvaluationRequest request = fullFrameRequest(chain.session.document(), chain.output, 6, 4);
        for (const auto& [backend, library] : backends) {
            eval::GpuEvaluation native =
                evaluateGpu(chain.session.document(), request, library, *boot.device, *boot.allocator);
            const CpuImage image = native.readBack(request.output, *boot.device, *boot.allocator);
            ASSERT_EQ(image.width(), 6) << backend;
            ASSERT_EQ(image.height(), 4) << backend;
            const std::string what = std::string{backend} + " vs the derived nearest sample: " + item.what;
            for (int y = 0; y < 4; ++y) {
                const int sourceRow = item.sourceRow[static_cast<std::size_t>(y)];
                for (int x = 0; x < 6; ++x) {
                    expectPixelClose(image, x, y,
                                     sourceRow < 0 ? kTransparent : patternPixel(2 * x + 1, sourceRow, 12, 5),
                                     what.c_str());
                }
            }
        }
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 48/55/56 (issue #92): a BOUNDED regional demand whose inverse map lands
// further outside the input's data window than the filter's reach still addresses
// the same image. With black outside off the retained outermost samples answer
// there — a regional fetch may never shrink the documented edge extension to
// nothing, and the answer may not depend on how much of the image a fetch happened
// to cover — while with black outside on that very demand is transparent black.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, FarOutsideRegionalDemandAnswersWithTheRetainedEdge) {
    ReformatChain chain(4, 2);
    setCustomCanvas(chain.session, chain.reformat, 4, 20);
    setParam(chain.session, chain.reformat, "resize", ChoiceValue{"none"});
    setParam(chain.session, chain.reformat, "filter", ChoiceValue{"Notch"});
    setParam(chain.session, chain.reformat, "center", false);

    const auto verify = [&](auto&& render, const char* backend) {
        SCOPED_TRACE(backend);
        for (const int scale : {1, 2, 4}) {
            SCOPED_TRACE(scale);
            for (const bool blackOutside : {false, true}) {
                setParam(chain.session, chain.reformat, "blackOutside", blackOutside);
                auto frame = fullFrameRequest(chain.session.document(), chain.output, 4, 20);
                frame.samplingScale = scale;
                frame.region = {-100, -100, 204, 220};
                const CpuImage whole = render(frame);
                // These windows stay far outside even after shared regional
                // padding. A small in-format window can accidentally fetch the
                // entire tiny source and conceal a missing edge requirement.
                for (const int coordinate : {-100, 100}) {
                    SCOPED_TRACE(coordinate);
                    auto request = frame;
                    request.region = {coordinate, coordinate, 4, 4};
                    const CpuImage window = render(request);
                    ASSERT_EQ(window.width(), scaledDimension(4, scale));
                    ASSERT_EQ(window.height(), scaledDimension(4, scale));
                    const int sourceX = coordinate < 0 ? 0 : latticeFloor(3, scale);
                    const int sourceY = coordinate < 0 ? 0 : latticeFloor(1, scale);
                    const auto expected = blackOutside ? kTransparent : patternPixel(sourceX, sourceY, 4, 2);
                    const int offset = (coordinate + 100) / scale;
                    for (int y = 0; y < window.height(); ++y) {
                        for (int x = 0; x < window.width(); ++x) {
                            expectPixelClose(window, x, y, expected, "retained corner at this density");
                            expectPixelClose(window, x, y, whole.pixel(offset + x, offset + y),
                                             "bounded demand versus larger coverage");
                        }
                    }
                }
            }
        }
    };
    verify([&](const EvaluationRequest& request) { return evaluateCpu(chain.session.document(), request).image; },
           "CPU");
    if (slangSpvDir().empty())
        return;
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const auto glsl = eval::glslEffectLibrary();
    for (const auto* library : {&slang, &glsl}) {
        verify(
            [&](const EvaluationRequest& request) {
                auto native = evaluateGpu(chain.session.document(), request, *library, *boot.device, *boot.allocator);
                return native.readBack(request.output, *boot.device, *boot.allocator);
            },
            library == &slang ? "Slang" : "GLSL");
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 55/56: `clamp` bounds a filtered channel by the contributing samples'
// extrema — never by 0..1 — so sharpening halos are removed while legitimate HDR
// and negative values survive untouched.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, ClampBoundsByContributingSamplesAndPreservesHdr) {
    // A hard edge: the reference pattern's blue bar covers the first two columns
    // of a 4-wide canvas. Lanczos4 magnifies that step and rings.
    ReformatChain chain(4, 1);
    setCustomCanvas(chain.session, chain.reformat, 8, 1);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"distort"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Lanczos4"}});
    const Document& document = chain.session.document();

    const CpuImage ringing = evaluateCpu(document, fullFrameRequest(document, chain.output, 8, 1)).image;
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    for (int x = 0; x < 8; ++x) {
        minimum = std::min(minimum, ringing.pixel(x, 0)[2]);
        maximum = std::max(maximum, ringing.pixel(x, 0)[2]);
    }
    EXPECT_LT(minimum, 0.0F) << "Lanczos4 must show its documented haloing";
    EXPECT_GT(maximum, 1.0F) << "Lanczos4 must show its documented haloing";

    setParam(chain.session, chain.reformat, "clamp", ParameterValue{true});
    const CpuImage clamped = evaluateCpu(document, fullFrameRequest(document, chain.output, 8, 1)).image;
    for (int x = 0; x < 8; ++x) {
        EXPECT_GE(clamped.pixel(x, 0)[2], 0.0F) << "clamped sample " << x;
        EXPECT_LE(clamped.pixel(x, 0)[2], 1.0F) << "clamped sample " << x;
    }

    // The same control never clamps legitimate out-of-range data: a constant HDR
    // plate (with a negative channel) passes through untouched, edge extension
    // included.
    ReformatChain hdr(4, 1);
    const NodeId plate = createNode(hdr.session, "constcolor", "Plate");
    setParam(hdr.session, plate, "color", ParameterValue{ColorValue{{8.0F, -2.0F, 0.5F, 1.0F}}});
    replaceInput(hdr.session, plate, hdr.reformat);
    setCustomCanvas(hdr.session, hdr.reformat, 4, 1);
    setParam(hdr.session, hdr.reformat, "filter", ParameterValue{ChoiceValue{"Rifman"}});
    setParam(hdr.session, hdr.reformat, "clamp", ParameterValue{true});
    setParam(hdr.session, hdr.reformat, "blackOutside", ParameterValue{false});
    const CpuImage image =
        evaluateCpu(hdr.session.document(), fullFrameRequest(hdr.session.document(), hdr.output, 4, 1)).image;
    for (int x = 0; x < 4; ++x) {
        expectPixelExact(image, x, 0, {8.0F, -2.0F, 0.5F, 1.0F}, "clamped HDR");
    }
}

// ---------------------------------------------------------------------------
// Story 55: every declared filter resamples identically on the CPU reference and
// on both native front ends (compiled Slang and the runtime GLSL reference)
// within the declared tolerance.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, EveryFilterMatchesTheNativeBackends) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    if (slangSpvDir().empty()) {
        GTEST_SKIP() << "no compiled Slang kernels (CPU-only configuration)";
    }

    ReformatChain chain(9, 5);
    setCustomCanvas(chain.session, chain.reformat, 6, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"fit"}});

    const std::array<const char*, 11> filters{"Impulse", "Cubic", "Keys",     "Simon",    "Rifman", "Mitchell",
                                              "Parzen",  "Notch", "Lanczos4", "Lanczos6", "Sinc4"};
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    for (const char* name : filters) {
        setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{name}});
        const EvaluationRequest request = fullFrameRequest(chain.session.document(), chain.output, 6, 4);
        const CpuImage reference = evaluateCpu(chain.session.document(), request).image;
        eval::GpuEvaluation slangEval =
            evaluateGpu(chain.session.document(), request, slang, *boot.device, *boot.allocator);
        expectImagesClose(reference, slangEval.readBack(request.output, *boot.device, *boot.allocator),
                          ("slang vs cpu: " + std::string{name}).c_str());
        eval::GpuEvaluation glslEval =
            evaluateGpu(chain.session.document(), request, glsl, *boot.device, *boot.allocator);
        expectImagesClose(reference, glslEval.readBack(request.output, *boot.device, *boot.allocator),
                          ("glsl vs cpu: " + std::string{name}).c_str());
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 56/57: a regional request and a reduced-density request address the same
// absolute coordinates as the whole frame, so their samples agree. The geometry is
// deliberately fractional and odd-sized — an odd 9x5 source fitted into a 6x4
// canvas is a 2/3 placement with a 1/3-sample origin — and every supported density
// (Full, Half, Quarter) is exercised: an identity resize would hide both the
// fractional mapping and the sampling reduction behind a one-to-one copy.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, RegionalAndReducedDensityRequestsKeepTheSameLattice) {
    ReformatChain chain(9, 5);
    setCustomCanvas(chain.session, chain.reformat, 6, 4);
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"fit"}});
    setParam(chain.session, chain.reformat, "filter", ParameterValue{ChoiceValue{"Cubic"}});
    const Document& document = chain.session.document();

    const CpuImage whole = evaluateCpu(document, fullFrameRequest(document, chain.output, 6, 4)).image;
    ASSERT_EQ(whole.width(), 6);
    ASSERT_EQ(whole.height(), 4);

    // Full density: a sub-region is the same window of the same image at the same
    // absolute coordinates — an off-origin, odd-sized window included. The claim is
    // the declared pixel agreement, not bit identity: the two requests book-keep
    // their rasters differently while addressing the same coordinates.
    EvaluationRequest regional = fullFrameRequest(document, chain.output, 6, 4);
    regional.region = {1, 1, 3, 2};
    const CpuImage window = evaluateCpu(document, regional).image;
    ASSERT_EQ(window.width(), 3);
    ASSERT_EQ(window.height(), 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            expectPixelClose(window, x, y, whole.pixel(1 + x, 1 + y), "regional window");
        }
    }

    // Half and Quarter reduce the DENSITY, never the coordinate space (issue #88):
    // the raster is the same image on the coarser lattice the request names, so a
    // reduced sub-region is the same window of the reduced whole frame, anchored by
    // the coverage the planner delivered.
    for (const int scale : {2, 4}) {
        EvaluationRequest reducedWhole = fullFrameRequest(document, chain.output, 6, 4);
        reducedWhole.samplingScale = scale;
        const CpuEvaluation reduced = evaluateCpu(document, reducedWhole);
        ASSERT_EQ(reduced.image.width(), scaledDimension(6, scale)) << "scale " << scale;
        ASSERT_EQ(reduced.image.height(), scaledDimension(4, scale)) << "scale " << scale;

        EvaluationRequest reducedWindow = fullFrameRequest(document, chain.output, 6, 4);
        reducedWindow.samplingScale = scale;
        reducedWindow.region = {2, 1, 3, 2};
        const CpuEvaluation requestedWindow = evaluateCpu(document, reducedWindow);
        const Region delivered = requestedWindow.plan.request.region;
        ASSERT_EQ(delivered.x % scale, 0) << "scale " << scale;
        ASSERT_EQ(delivered.y % scale, 0) << "scale " << scale;
        ASSERT_EQ(delivered.width % scale, 0) << "scale " << scale;
        ASSERT_EQ(delivered.height % scale, 0) << "scale " << scale;
        EXPECT_LE(delivered.x, 2) << "scale " << scale;
        EXPECT_LE(delivered.y, 1) << "scale " << scale;
        EXPECT_GE(delivered.x + delivered.width, 5) << "scale " << scale;
        EXPECT_GE(delivered.y + delivered.height, 3) << "scale " << scale;
        ASSERT_EQ(requestedWindow.image.width(), delivered.width / scale);
        ASSERT_EQ(requestedWindow.image.height(), delivered.height / scale);
        // The window is inside the whole frame's reduced raster, so every sample it
        // reads is a sample of that raster (the ceil edge may make the coverage
        // reach past the format, and both requests cover it identically).
        ASSERT_LE(delivered.x / scale + requestedWindow.image.width(), reduced.image.width()) << "scale " << scale;
        ASSERT_LE(delivered.y / scale + requestedWindow.image.height(), reduced.image.height()) << "scale " << scale;
        for (int y = 0; y < requestedWindow.image.height(); ++y) {
            for (int x = 0; x < requestedWindow.image.width(); ++x) {
                expectPixelClose(requestedWindow.image, x, y,
                                 reduced.image.pixel(delivered.x / scale + x, delivered.y / scale + y),
                                 "reduced regional window");
            }
        }
    }

    // An identity resize at a reduced density copies its input raster: the reduced
    // sample anchors to its OWN lattice coordinate — the request origin plus a whole
    // number of samples (issue #88: the raster index of an absolute coordinate is
    // (absolute - origin) / scale), which is the coordinate the authored generator
    // sampled — not the full-resolution sample in the middle of the block it covers.
    ReformatChain identity(4, 2);
    setCustomCanvas(identity.session, identity.reformat, 4, 2);
    setParam(identity.session, identity.reformat, "resize", ParameterValue{ChoiceValue{"none"}});
    setParam(identity.session, identity.reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    const Document& identityDocument = identity.session.document();
    const CpuImage identityWhole =
        evaluateCpu(identityDocument, fullFrameRequest(identityDocument, identity.output, 4, 2)).image;
    for (const int scale : {2, 4}) {
        EvaluationRequest reducedIdentity = fullFrameRequest(identityDocument, identity.output, 4, 2);
        reducedIdentity.samplingScale = scale;
        const CpuImage reduced = evaluateCpu(identityDocument, reducedIdentity).image;
        ASSERT_EQ(reduced.width(), scaledDimension(4, scale)) << "scale " << scale;
        ASSERT_EQ(reduced.height(), scaledDimension(2, scale)) << "scale " << scale;
        for (int y = 0; y < reduced.height(); ++y) {
            for (int x = 0; x < reduced.width(); ++x) {
                expectPixelExact(reduced, x, y, identityWhole.pixel(x * scale, y * scale),
                                 "reduced sample anchors to its own lattice coordinate");
            }
        }
    }

    // The reduced, off-origin window on both native front ends: the compiled Slang
    // kernel and the runtime GLSL reference resample the reduced raster onto the
    // same absolute lattice as the CPU reference, within the declared tolerance.
    if (slangSpvDir().empty()) {
        return;  // the CPU lattice is proven; native filter parity has its own test.
    }
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    for (const int scale : {2, 4}) {
        EvaluationRequest reducedWindow = fullFrameRequest(document, chain.output, 6, 4);
        reducedWindow.samplingScale = scale;
        reducedWindow.region = {2, 1, 3, 2};
        const CpuImage reference = evaluateCpu(document, reducedWindow).image;
        eval::GpuEvaluation slangEval = evaluateGpu(document, reducedWindow, slang, *boot.device, *boot.allocator);
        expectImagesClose(reference, slangEval.readBack(reducedWindow.output, *boot.device, *boot.allocator),
                          ("slang vs cpu: reduced regional window at scale " + std::to_string(scale)).c_str());
        eval::GpuEvaluation glslEval = evaluateGpu(document, reducedWindow, glsl, *boot.device, *boot.allocator);
        expectImagesClose(reference, glslEval.readBack(reducedWindow.output, *boot.device, *boot.allocator),
                          ("glsl vs cpu: reduced regional window at scale " + std::to_string(scale)).c_str());
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 50/57: a chain of differently sized formats saves, reopens and renders
// identically — through the public file layer and on both executors.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, ChainedFormatsSurviveSaveAndReopen) {
    ReformatChain chain(5, 3);
    // to box (force shape), then to format (composition), then to scale.
    setParam(chain.session, chain.reformat, "type", ParameterValue{ChoiceValue{"box"}});
    setParam(chain.session, chain.reformat, "boxWidth", ParameterValue{std::int64_t{3}});
    setParam(chain.session, chain.reformat, "boxHeight", ParameterValue{std::int64_t{3}});
    setParam(chain.session, chain.reformat, "forceShape", ParameterValue{true});
    setParam(chain.session, chain.reformat, "resize", ParameterValue{ChoiceValue{"fill"}});

    const NodeId composed = createNode(chain.session, "reformat", "Composed");
    const NodeId scaled = createNode(chain.session, "reformat", "Scaled");
    setParam(chain.session, composed, "resize", ParameterValue{ChoiceValue{"distort"}});
    setParam(chain.session, scaled, "type", ParameterValue{ChoiceValue{"scale"}});
    setParam(chain.session, scaled, "scaleX", ParameterValue{1.5});
    setParam(chain.session, scaled, "scaleY", ParameterValue{0.5});
    setParam(chain.session, scaled, "resize", ParameterValue{ChoiceValue{"width"}});
    replaceInput(chain.session, scaled, chain.output);
    connectInput(chain.session, composed, scaled);
    connectInput(chain.session, chain.reformat, composed);

    // to box 3x3, then the composition canvas 5x3, then scale (1.5, 0.5) with the
    // width direction exact: 5 * 1.5 = 7.5 rounds to 8 (ties up) and the height
    // follows the aspect: 8 * 3 / 5 = 4.8 rounds to 5.
    const CpuEvaluation before =
        evaluateCpu(chain.session.document(), fullFrameRequest(chain.session.document(), chain.output, 8, 5));
    const PlanStep* box = stepFor(before.plan, "Reformat");
    const PlanStep* composition = stepFor(before.plan, "Composed");
    const PlanStep* scale = stepFor(before.plan, "Scaled");
    ASSERT_NE(box, nullptr);
    ASSERT_NE(composition, nullptr);
    ASSERT_NE(scale, nullptr);
    EXPECT_EQ(box->description.format, (Region{0, 0, 3, 3}));
    EXPECT_EQ(composition->description.format, (Region{0, 0, 5, 3}));
    EXPECT_EQ(scale->description.format, (Region{0, 0, 8, 5}));
    // The scaled step keeps the input's physical aspect from the exact width
    // direction (5 -> 7.5 rounds to 8), so its image covers the whole output.
    EXPECT_EQ(scale->description.dataBounds, (Region{0, 0, 8, 5}));

    const fs::path target = dir_ / "reformat-chain.nemo";
    const ProjectWriteRequest job = chain.session.prepareSave(target);
    const ProjectWriteResult written = ProjectFile::writeAtomic(job);
    ASSERT_TRUE(written.ok) << written.error.message;
    ASSERT_TRUE(chain.session.commitSave(job, written).committed);

    ProjectReadResult read = ProjectFile::read(target, builtinNodeCatalogPtr());
    ASSERT_TRUE(read.ok) << read.error.message;

    ProjectSession reopened{Document(builtinNodeCatalogPtr())};
    ASSERT_TRUE(reopened.open(std::move(read)).replaced);
    const NodeInstance* restored = reopened.document().network(chain.network).graph().nodeByName("Scaled");
    ASSERT_NE(restored, nullptr);
    // The reopened node's EFFECTIVE parameters, through the public query: the
    // authored inputs the file carries survive the round trip, while a control the
    // scale mode does not consume resolves to its descriptor default (the authored
    // map alone is not the node's state, and the file does not carry defaults).
    const auto effective = [&reopened, &chain](NodeId node, std::string_view key) {
        const std::vector<ValueQueryResult> values = reopened.queryValues(chain.network, node, key);
        EXPECT_FALSE(values.empty()) << "no effective value for '" << std::string{key} << "'";
        return values.empty() ? ParameterValue{} : values.front().value;
    };
    EXPECT_EQ(effective(restored->id, "type"), ParameterValue{ChoiceValue{"scale"}});
    EXPECT_EQ(effective(restored->id, "scaleX"), ParameterValue{1.5});
    EXPECT_EQ(effective(restored->id, "scaleY"), ParameterValue{0.5});
    EXPECT_EQ(effective(restored->id, "resize"), ParameterValue{ChoiceValue{"width"}});

    const NodeId reopenedOutput = reopened.document().network(chain.network).defaultOutput();
    const EvaluationRequest reopenedRequest = fullFrameRequest(reopened.document(), reopenedOutput, 8, 5);
    const CpuEvaluation after = evaluateCpu(reopened.document(), reopenedRequest);
    const PlanStep* reopenedScale = stepFor(after.plan, "Scaled");
    ASSERT_NE(reopenedScale, nullptr);
    EXPECT_EQ(reopenedScale->description.format, (Region{0, 0, 8, 5}));
    expectImagesClose(before.image, after.image, "reopened chain");

    if (slangSpvDir().empty()) {
        return;  // the CPU workflow is proven; filter parity is its own test.
    }
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary library = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    eval::GpuEvaluation native =
        evaluateGpu(reopened.document(), reopenedRequest, library, *boot.device, *boot.allocator);
    expectImagesClose(after.image, native.readBack(reopenedOutput, *boot.device, *boot.allocator),
                      "native reopened chain");

    // The same chain at Half and Quarter density: every step keeps the SAME
    // absolute lattice (issue #88), so the reduced description is the full one and
    // only the raster coarsens, and both native front ends agree with the CPU
    // reference on that reduced demand.
    const eval::EffectLibrary glslLibrary = eval::glslEffectLibrary();
    for (const int scale : {2, 4}) {
        EvaluationRequest reduced = fullFrameRequest(reopened.document(), reopenedOutput, 8, 5);
        reduced.samplingScale = scale;
        const CpuEvaluation reducedCpu = evaluateCpu(reopened.document(), reduced);
        const PlanStep* reducedScale = stepFor(reducedCpu.plan, "Scaled");
        ASSERT_NE(reducedScale, nullptr);
        EXPECT_EQ(reducedScale->description.format, (Region{0, 0, 8, 5})) << "scale " << scale;
        ASSERT_EQ(reducedCpu.image.width(), scaledDimension(8, scale));
        ASSERT_EQ(reducedCpu.image.height(), scaledDimension(5, scale));
        eval::GpuEvaluation reducedSlang =
            evaluateGpu(reopened.document(), reduced, library, *boot.device, *boot.allocator);
        expectImagesClose(reducedCpu.image, reducedSlang.readBack(reopenedOutput, *boot.device, *boot.allocator),
                          ("slang vs cpu: reopened chain at scale " + std::to_string(scale)).c_str());
        eval::GpuEvaluation reducedGlsl =
            evaluateGpu(reopened.document(), reduced, glslLibrary, *boot.device, *boot.allocator);
        expectImagesClose(reducedCpu.image, reducedGlsl.readBack(reopenedOutput, *boot.device, *boot.allocator),
                          ("glsl vs cpu: reopened chain at scale " + std::to_string(scale)).c_str());
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 55/57 (issue #90 integration): every named plane survives, the primary
// RGBA is resampled association-aware and each auxiliary plane is resampled
// NUMERICALLY and independently. The fixture is the hand-authored multilayer
// plate from the channel suite (values per
// docs/evidence/assets/issue90-channels/fixtures.json), so the expected samples
// are authored, not produced by another executor.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, AuxiliaryNamedPlanesAreResampledNumericallyAndPreserved) {
    const fs::path path = fs::path{NEMO_CHANNEL_FIXTURE_DIR} / "multilayer-b.exr";
    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    const NodeId output = session.document().network(network).defaultOutput();
    const NodeId source = createNode(session, "source", "Multilayer");
    const NodeId reformat = createNode(session, "reformat", "Reformat");
    SourceReference reference;
    reference.path = path.string();
    ASSERT_TRUE(session
                    .submit(transactionCommand(
                                "multilayer reformat",
                                {setSourceCommand("plate", reference),
                                 setParamCommand(network, source, "source", std::string{"plate"}),
                                 // The plate's authored samples are DATA: a raw input
                                 // bypass is what makes every stored plane's expected
                                 // value the file's own written value (issue #62).
                                 setParamCommand(network, source, "inputTransform", ParameterValue{ChoiceValue{"raw"}}),
                                 connectCommand(network, {source, 0}, {reformat, 0}),
                                 connectCommand(network, {reformat, 0}, {output, 0})}),
                            options(session))
                    .committed);

    // The plate is an authored 8x8 multilayer image; read it on its own first so
    // the one-to-one reformat can be authored against the plate's own canvas.
    media::ImageSourceProvider provider;
    const CpuEvaluation plate =
        evaluateCpu(session.document(), fullFrameRequest(session.document(), source, 8, 8), nullptr, &provider);
    const PlanStep* plateStep = stepFor(plate.plan, "Multilayer");
    ASSERT_NE(plateStep, nullptr);
    ASSERT_EQ(plateStep->description.format, (Region{0, 0, 8, 8}));

    setParam(session, reformat, "resize", ParameterValue{ChoiceValue{"none"}});
    setParam(session, reformat, "filter", ParameterValue{ChoiceValue{"Impulse"}});
    ASSERT_TRUE(session
                    .submit(setNetworkFormatCommand(network, ImageFormat{8, 8, plateStep->description.pixelAspect}),
                            options(session))
                    .committed);

    const EvaluationRequest planeRequest = fullFrameRequest(session.document(), output, 8, 8);
    const CpuEvaluation resampled = evaluateCpu(session.document(), planeRequest, nullptr, &provider);
    const PlanStep* step = stepFor(resampled.plan, "Reformat");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->description.channels, plateStep->description.channels) << "every named plane must survive";
    EXPECT_EQ(step->description.format, plateStep->description.format);

    const std::vector<std::string> storedPlanes = plateStep->description.channels;
    const auto planeOf = [](const CpuImage& image, const std::string& name) {
        return channelIndex(image.layout().channels, name);
    };
    // Every stored plane of the produced raster, in whatever backend produced it.
    // The fixture's authored values (docs/evidence/assets/issue90-channels/fixtures.json)
    // are the oracle: the primary roles pass through the association-aware path at
    // full coverage, and each named plane is THIS node's own numerical resample, so
    // no backend may drop, rename or perturb a plane. `exact` states that claim at
    // the CPU reference's own exactness; the native kernels answer it within the
    // declared tolerance, which absorbs only their float arithmetic.
    const auto expectPlanes = [&](const CpuImage& image, const char* backend, bool exact) {
        ASSERT_EQ(image.layout().channels, storedPlanes) << backend;
        ASSERT_EQ(image.width(), 8) << backend;
        ASSERT_EQ(image.height(), 8) << backend;
        const int red = planeOf(image, "R");
        const int green = planeOf(image, "G");
        const int blue = planeOf(image, "B");
        const int alpha = planeOf(image, "A");
        const int motionU = planeOf(image, "motion.u");
        const int motionV = planeOf(image, "motion.v");
        const int coverage = planeOf(image, "matte.coverage");
        const int beautyR = planeOf(image, "beauty.R");
        const int beautyG = planeOf(image, "beauty.G");
        const int beautyB = planeOf(image, "beauty.B");
        const int depthZ = planeOf(image, "depth.Z");
        for (const int plane :
             {red, green, blue, alpha, motionU, motionV, coverage, beautyR, beautyG, beautyB, depthZ}) {
            ASSERT_GE(plane, 0) << backend << ": a stored plane is missing";
        }
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const auto expect = [&](int plane, float authored, const char* name) {
                    const float tolerance =
                        exact ? 0.0F : kAbsoluteTolerance + kRelativeTolerance * std::fabs(authored);
                    EXPECT_NEAR(image.channel(x, y, plane), authored, tolerance)
                        << backend << ": " << name << " at (" << x << "," << y << ")";
                };
                expect(red, 0.25F, "R");
                expect(green, 0.5F, "G");
                expect(blue, 0.75F, "B");
                expect(alpha, 1.0F, "A");
                expect(beautyR, 4.0F, "beauty.R");
                expect(beautyG, -5.0F, "beauty.G");
                expect(beautyB, 6.0F, "beauty.B");
                expect(depthZ, 10.0F * static_cast<float>(x) + static_cast<float>(y), "depth.Z");
                expect(coverage, 0.125F, "matte.coverage");
                expect(motionU, -2.0F, "motion.u");
                expect(motionV, 3.0F, "motion.v");
            }
        }
    };
    // The CPU reference is exact for every one of them: a one-to-one Impulse
    // reformat carries each authored value with weight one.
    expectPlanes(resampled.image, "cpu", true);
    // The identity also means the plate's own described raster, sample for sample.
    for (std::size_t index = 0; index < storedPlanes.size(); ++index) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                EXPECT_FLOAT_EQ(resampled.image.channel(x, y, static_cast<int>(index)),
                                plate.image.channel(x, y, static_cast<int>(index)))
                    << "plane " << storedPlanes[index] << " at (" << x << "," << y << ")";
            }
        }
    }

    // The SAME stored planes through both native front ends: the kernels resample
    // every named plane themselves (this node owns its channel layout), so the
    // authored samples must survive there too instead of only agreeing with the CPU.
    if (slangSpvDir().empty()) {
        return;  // CPU-only configuration: the plane contract above is proven.
    }
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSrcDir());
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    eval::SourceSession nativeSources(*boot.instance, *boot.device, *boot.allocator,
                                      slangSpvDir() / "mediaConvert.spv");
    const std::array<std::pair<const char*, const eval::EffectLibrary>, 2> backends{
        std::pair<const char*, const eval::EffectLibrary>{"slang", slang},
        std::pair<const char*, const eval::EffectLibrary>{"glsl", glsl}};
    for (const auto& [backend, library] : backends) {
        eval::GpuEvaluation native = evaluateGpu(session.document(), planeRequest, library, *boot.device,
                                                 *boot.allocator, 10'000'000'000ULL, nullptr, &nativeSources);
        expectPlanes(native.readBack(output, *boot.device, *boot.allocator), backend, false);
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Story 49/50/56: inadmissible authored geometry is refused by the public
// command path — a zero canvas or a zero scale never becomes a silent default.
// ---------------------------------------------------------------------------
TEST_F(ReformatTest, InadmissibleCanvasAndScaleAreRefusedAtAuthoring) {
    ReformatChain chain(4, 2);
    setParam(chain.session, chain.reformat, "formatSource", ParameterValue{ChoiceValue{"custom"}});

    const EditResult zeroWidth =
        chain.session.submit(setParamCommand(chain.network, chain.reformat, "width", ParameterValue{std::int64_t{0}}),
                             options(chain.session));
    EXPECT_FALSE(zeroWidth.committed);
    ASSERT_TRUE(zeroWidth.error.has_value());
    EXPECT_FALSE(zeroWidth.error->message.empty());

    setParam(chain.session, chain.reformat, "type", ParameterValue{ChoiceValue{"scale"}});
    const EditResult zeroScale = chain.session.submit(
        setParamCommand(chain.network, chain.reformat, "scaleX", ParameterValue{0.0}), options(chain.session));
    EXPECT_FALSE(zeroScale.committed);
    ASSERT_TRUE(zeroScale.error.has_value());
    EXPECT_FALSE(zeroScale.error->message.empty());
}
