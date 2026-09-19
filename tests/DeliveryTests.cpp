// Explicit delivery jobs (issue #94, stories 74-86) — consumer-visible behavior
// of the shared submit/status/progress/results/cancel seam over the NATIVE
// execution path.
//
// These tests defend what a consumer observes: the refusal a preflight reports,
// the pixels/channels/precision/compression an INDEPENDENT media readback finds
// at the pattern-expanded paths, the frozen snapshot an accepted job keeps, the
// distinction between finalized and unwritten frames, the atomic publication
// that leaves no temporary behind, and the movie contract (one container,
// published only after the whole range encoded, nothing at all when cancelled).
//
// Execution is the real native path — `evaluateGpu` over a retained
// `SourceSession`, the GPU export staging transfer charged to the shared
// allocator, and the media encoders — because that IS the delivery seam: a test
// that stubbed any of it would assert nothing a user can observe. Nothing here
// asserts wiring, source text or an incidental default.

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/eval/DeliveryJob.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/ImageIO.hpp"

using namespace nemo;
using namespace nemo::eval;
using namespace nemo::media;

namespace {

namespace fs = std::filesystem;

// A per-test output directory, removed when the test ends: a delivery run never
// leaves files behind in the temp area, so a failing assertion cannot hide state
// in the next run.
class TestOutputDir {
public:
    explicit TestOutputDir(const std::string& name) : path_(fs::temp_directory_path() / "nemo-delivery-tests" / name) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TestOutputDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TestOutputDir(const TestOutputDir&) = delete;
    TestOutputDir& operator=(const TestOutputDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

    // A sequence file name formatted here, independently of the module's own
    // pattern expansion, so a delivered path is verified rather than echoed.
    [[nodiscard]] std::string sequence(const std::string& stem, const std::int64_t frame) const {
        std::ostringstream name;
        name << (path_ / stem).string() << std::setw(4) << std::setfill('0') << frame << ".exr";
        return name.str();
    }

    // Every hidden delivery temporary the queue would have created, so a test
    // can prove none survived.
    [[nodiscard]] std::vector<std::string> temporaries() const {
        std::vector<std::string> found;
        for (const auto& entry : fs::directory_iterator(path_)) {
            if (entry.path().filename().string().find("nemo-delivery") != std::string::npos) {
                found.push_back(entry.path().string());
            }
        }
        return found;
    }

private:
    fs::path path_;
};

// The smallest delivery graph that still exercises the real seams: a generator
// at a known small canvas feeding a Write node whose settings are authored on
// the document. The generator's image IS its owning network's authored canvas,
// so the delivered raster is deterministic.
struct DeliveryGraph {
    Document document;
    NetworkId network{kInvalidNetwork};
    NodeId color{kInvalidNode};
    NodeId write{kInvalidNode};
};

DeliveryGraph deliveryGraph(const std::array<float, 4>& color, const int width = 4, const int height = 3) {
    DeliveryGraph graph;
    graph.network = graph.document.rootNetworkId();
    graph.document.network(graph.network).setFormat(ImageFormat{.width = width, .height = height});
    Graph& scoped = graph.document.network(graph.network).graph();
    graph.color = scoped.addNode("constcolor", "color");
    scoped.setParam(graph.color, "color", ParameterValue{ColorValue{color}});
    graph.write = scoped.addNode("write", "deliver");
    static_cast<void>(scoped.connect(PortRef{graph.color, 0}, PortRef{graph.write, 0}));
    return graph;
}

// Authors the delivery request on the Write node, exactly as the inspector does:
// the node states the request and the seam validates it.
void authorDelivery(Document& document, const NodeId write, const std::string& pattern, const std::int64_t first,
                    const std::int64_t last, const std::string& compression = "zip",
                    const std::string& precision = "half", const bool overwrite = false,
                    const bool createDirectories = true, const std::int64_t offset = 0,
                    const std::string& fileType = "exr") {
    Graph& scoped = document.network(document.rootNetworkId()).graph();
    scoped.setParam(write, "file", ParameterValue{pattern});
    scoped.setParam(write, "fileType", ParameterValue{ChoiceValue{fileType}});
    scoped.setParam(write, "createDirectories", ParameterValue{createDirectories});
    scoped.setParam(write, "overwrite", ParameterValue{overwrite});
    scoped.setParam(write, "frameFirst", ParameterValue{first});
    scoped.setParam(write, "frameLast", ParameterValue{last});
    scoped.setParam(write, "frameOffset", ParameterValue{offset});
    scoped.setParam(write, "precision", ParameterValue{ChoiceValue{precision}});
    scoped.setParam(write, "compression", ParameterValue{ChoiceValue{compression}});
}

void setColor(Document& document, const NodeId color, const std::array<float, 4>& value) {
    document.network(document.rootNetworkId()).graph().setParam(color, "color", ParameterValue{ColorValue{value}});
}

// The compression the file itself declares, read by OpenImageIO directly: the
// delivery setting must reach the encoder, not merely the report.
std::string storedCompression(const std::string& path) {
    auto input = OIIO::ImageInput::open(path);
    EXPECT_NE(input, nullptr) << path;
    if (!input) {
        return {};
    }
    const std::string compression = input->spec().get_string_attribute("compression");
    static_cast<void>(input->close());
    return compression;
}

// One delivered frame, asserted through the module's own reader: the pixel
// values (chosen exactly representable in half), the channel names, the native
// precision on disk and the data window the file declares. `tolerance` is 0 for
// a lossless compression, which is asserted as exact equality; a lossy storage
// name bounds the difference instead of pretending to be exact.
void expectDelivered(const std::string& path, const std::array<float, 4>& color, const int width, const int height,
                     const std::string& precision, const float tolerance = 0.0F) {
    const ImageReadResult read = readImage(path);
    EXPECT_EQ(read.image.width(), width) << path;
    EXPECT_EQ(read.image.height(), height) << path;
    EXPECT_EQ(read.header.channelNames, (std::vector<std::string>{"R", "G", "B", "A"})) << path;
    EXPECT_EQ(read.header.nativePrecision, precision) << path;
    EXPECT_EQ(read.header.windows.data, (PixelWindow{0, 0, width - 1, height - 1})) << path;
    EXPECT_EQ(read.header.windows.display, (PixelWindow{0, 0, width - 1, height - 1})) << path;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, 4> actual = read.image.pixel(x, y);
            for (std::size_t channel = 0; channel < color.size(); ++channel) {
                if (tolerance == 0.0F)
                    EXPECT_EQ(actual[channel], color[channel]) << path << " at (" << x << ", " << y << ") c" << channel;
                else
                    EXPECT_NEAR(actual[channel], color[channel], tolerance)
                        << path << " at (" << x << ", " << y << ") c" << channel;
            }
        }
    }
}

constexpr std::array<float, 4> kAuthoredColor{0.25F, 0.5F, 0.75F, 1.0F};

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

#if defined(NEMO_SLANG_SPV_DIR)
[[nodiscard]] fs::path slangSpvDir() {
    return NEMO_SLANG_SPV_DIR;
}
#else
[[nodiscard]] fs::path slangSpvDir() {
    return {};
}
#endif

// A delivery runs on the native path, so it needs a device AND the compiled
// kernels the viewer renders with; a configuration without either is reported
// instead of silently exercising nothing.
#define NEMO_SKIP_OR_FAIL(boot)                                                                                        \
    do {                                                                                                               \
        if ((boot).outcome == BootstrapOutcome::NoDevice)                                                              \
            GTEST_SKIP() << (boot).message;                                                                            \
        if ((boot).outcome == BootstrapOutcome::Failed) {                                                              \
            ADD_FAILURE() << "device creation failed: " << (boot).message;                                             \
            return;                                                                                                    \
        }                                                                                                              \
        if (slangSpvDir().empty())                                                                                     \
            GTEST_SKIP() << "no compiled Slang shader directory in this configuration";                                \
    } while (false)

class DeliveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        boot_ = createBootstrap();
        dir_ = std::make_unique<TestOutputDir>(testName());
    }

    // One delivery queue per test, borrowing the test's own native owners: the
    // same shape the desktop runtime uses (never a device or allocator of its
    // own).
    [[nodiscard]] std::unique_ptr<DeliveryQueue> queue() const {
        return std::make_unique<DeliveryQueue>(*boot_.instance, *boot_.device, *boot_.allocator, slangSpvDir());
    }

    // The gtest name, sanitized for a directory name (distinct per test so a
    // failing run leaves readable state).
    [[nodiscard]] std::string testName() const {
        std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        for (char& character : name)
            if (character == '/')
                character = '-';
        return name;
    }

    Bootstrap boot_;
    std::unique_ptr<TestOutputDir> dir_;
};

}  // namespace

// Preflight is a report: the frames and paths it would write, the collisions it
// finds, and a refusal that names the offending value — all without creating,
// writing or removing anything.
TEST_F(DeliveryTest, PreflightReportsRefusalsFramesAndCollisions) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    const std::string still = dir_->file("still.exr");

    // A still pattern cannot name a multi-frame range; the refusal names it.
    authorDelivery(graph.document, graph.write, still, 1, 3);
    auto queue = this->queue();
    const DeliveryPlan refused = queue->plan(graph.document, graph.network, graph.write, 0);
    EXPECT_FALSE(refused.ok());
    EXPECT_NE(refused.problem.find(still), std::string::npos) << refused.problem;
    EXPECT_FALSE(fs::exists(still));

    // A range with a sequence pattern reports the exact frames, file numbering
    // and pattern-expanded paths (offset included).
    const std::string pattern = dir_->file("shot.####.exr");
    authorDelivery(graph.document, graph.write, pattern, 5, 7, "zip", "half", false, true, 10);
    const DeliveryPlan planned = queue->plan(graph.document, graph.network, graph.write, 0);
    EXPECT_TRUE(planned.problem.empty()) << planned.problem;
    EXPECT_TRUE(planned.collisions.empty());
    ASSERT_EQ(planned.frames.size(), 3U);
    EXPECT_EQ(planned.frames[0].documentFrame, 5);
    EXPECT_EQ(planned.frames[0].fileFrame, 15);
    EXPECT_EQ(planned.frames[0].path, dir_->sequence("shot.", 15));
    EXPECT_EQ(planned.frames[2].documentFrame, 7);
    EXPECT_EQ(planned.frames[2].fileFrame, 17);
    EXPECT_EQ(planned.frames[2].path, dir_->sequence("shot.", 17));
    // The raster the accepted job would deliver is the target's described image,
    // resolved through the SAME native source descriptions execution uses.
    EXPECT_EQ(planned.width, 4);
    EXPECT_EQ(planned.height, 3);
    EXPECT_EQ(planned.channels, (std::vector<std::string>{"R", "G", "B", "A"}));
    EXPECT_FALSE(planned.movie);

    // An existing destination is reported as a collision, in frame order, and
    // refuses the plan until overwriting is authorized.
    const CpuImage existing(2, 2);
    writeImage(dir_->sequence("shot.", 15), existing, OutputPrecision::Half);
    writeImage(dir_->sequence("shot.", 17), existing, OutputPrecision::Half);
    const DeliveryPlan collided = queue->plan(graph.document, graph.network, graph.write, 0);
    EXPECT_EQ(collided.collisions,
              (std::vector<std::string>{dir_->sequence("shot.", 15), dir_->sequence("shot.", 17)}));
    EXPECT_FALSE(collided.ok());
    DeliverySettings authorized = collided.settings;
    authorized.overwrite = true;
    const DeliveryPlan replacement = queue->plan(graph.document, graph.network, graph.write, authorized, 0);
    EXPECT_TRUE(replacement.ok()) << replacement.problem;
    EXPECT_EQ(replacement.collisions, collided.collisions) << "authorization must not hide the affected files";

    // An unsupported compression is refused by name, before any job exists. The
    // command path cannot author one — the schema's declared choices are that
    // path's authoritative validator — so this is the seam's own guard, reached
    // by an explicit settings request exactly like the CLI's `--compression`.
    DeliverySettings unsupported;
    unsupported.file = dir_->file("other.####.exr");
    unsupported.frameFirst = 1;
    unsupported.frameLast = 1;
    unsupported.output.compression = "lzw";
    const DeliveryPlan unsupportedPlan = queue->plan(graph.document, graph.network, graph.write, unsupported, 0);
    EXPECT_FALSE(unsupportedPlan.ok());
    EXPECT_NE(unsupportedPlan.problem.find("lzw"), std::string::npos) << unsupportedPlan.problem;
    EXPECT_FALSE(fs::exists(dir_->file("other.1.exr")));

    // A node that is not a Write node is refused, naming the node.
    EXPECT_THROW(static_cast<void>(deliverySettings(graph.document, graph.network, graph.color, 0)), DeliveryException);
}

// A submitted job delivers EXR frames that an independent readback finds at the
// pattern-expanded paths, with the authored pixels, channel names and precision,
// through the native executor and the shared export staging.
//
// `dwaa` is the one LOSSY name in the inventory (documented on the schema choice
// and in `ImageWriteOptions`), so it is asserted with the tolerance DWA's own
// quantization implies — DWA stores RGB at a reduced precision — and with
// exactly the same geometry, channels, precision and path evidence as the
// lossless names. Claiming an exact round-trip for it would be a false claim.
TEST_F(DeliveryTest, DeliveredSequenceCarriesPixelsChannelsPrecisionAndCompression) {
    NEMO_SKIP_OR_FAIL(boot_);
    struct Case {
        std::string compression;
        std::string precision;
        float tolerance;
    };
    const std::array<Case, 3> cases{{{.compression = "zip", .precision = "half", .tolerance = 0.0F},
                                     {.compression = "none", .precision = "float", .tolerance = 0.0F},
                                     {.compression = "dwaa", .precision = "half", .tolerance = 1.0F / 255.0F}}};
    for (const Case& test : cases) {
        SCOPED_TRACE(test.compression);
        DeliveryGraph graph = deliveryGraph(kAuthoredColor);
        const std::string pattern = dir_->file("shot." + test.compression + ".####.exr");
        authorDelivery(graph.document, graph.write, pattern, 1, 2, test.compression, test.precision);

        auto queue = this->queue();
        const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
        EXPECT_NE(id, 0U);
        queue->waitForIdle();

        const DeliveryJobInfo info = queue->status(id);
        EXPECT_EQ(info.state, DeliveryState::Completed) << info.error;
        EXPECT_TRUE(info.fullQuality) << "a delivery states full quality";
        EXPECT_TRUE(info.nativeStaging) << "the frame reached the host through the GPU export staging owner";
        EXPECT_GT(info.stagingBytes, 0U) << "the staging transfer is charged through the shared allocator";
        EXPECT_EQ(info.totalFrames, 2U);
        EXPECT_EQ(info.writtenFrames, 2U);
        EXPECT_EQ(info.failedFrames, 0U);
        EXPECT_TRUE(info.error.empty());
        EXPECT_DOUBLE_EQ(info.progress(), 1.0);
        EXPECT_EQ(info.width, 4);
        EXPECT_EQ(info.height, 3);
        EXPECT_EQ(info.channels, (std::vector<std::string>{"R", "G", "B", "A"}));
        ASSERT_EQ(info.files.size(), 2U);
        EXPECT_TRUE(info.files[0].written);
        EXPECT_EQ(info.files[0].documentFrame, 1);
        EXPECT_EQ(info.files[0].path, dir_->sequence("shot." + test.compression + ".", 1));
        EXPECT_TRUE(info.files[1].written);
        EXPECT_EQ(info.files[1].path, dir_->sequence("shot." + test.compression + ".", 2));

        expectDelivered(dir_->sequence("shot." + test.compression + ".", 1), kAuthoredColor, 4, 3, test.precision,
                        test.tolerance);
        expectDelivered(dir_->sequence("shot." + test.compression + ".", 2), kAuthoredColor, 4, 3, test.precision,
                        test.tolerance);
        EXPECT_EQ(storedCompression(dir_->sequence("shot." + test.compression + ".", 1)), test.compression);
        EXPECT_TRUE(dir_->temporaries().empty());
    }
}

// A collision is refused by the worker BEFORE any write — the UI's submit never
// blocks on the filesystem — and only an explicit overwrite authorizes the
// write.
TEST_F(DeliveryTest, CollisionsRefuseTheWorkerBeforeAnyWrite) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    const std::string path = dir_->file("still.exr");
    const CpuImage existing(2, 2);
    writeImage(path, existing, OutputPrecision::Half);

    authorDelivery(graph.document, graph.write, path, 1, 1);
    auto queue = this->queue();
    const std::uint64_t refused = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    const DeliveryJobInfo refusal = queue->status(refused);
    EXPECT_EQ(refusal.state, DeliveryState::Failed);
    EXPECT_NE(refusal.error.find(path), std::string::npos) << refusal.error;
    EXPECT_EQ(refusal.writtenFrames, 0U);
    EXPECT_TRUE(refusal.files.empty());
    EXPECT_EQ(readImage(path).image.width(), 2) << "the existing file is untouched by a refusal";
    EXPECT_TRUE(dir_->temporaries().empty());

    // The same job with explicit authorization replaces the file with delivered
    // pixels.
    authorDelivery(graph.document, graph.write, path, 1, 1, "zip", "half", /*overwrite=*/true);
    const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    EXPECT_EQ(queue->status(id).state, DeliveryState::Completed) << queue->status(id).error;
    expectDelivered(path, kAuthoredColor, 4, 3, "half");

    // The caller then edits the graph and re-submits without authorization: the
    // collision is refused and the delivered file is left exactly as it was.
    setColor(graph.document, graph.color, {1.0F, 0.0F, 0.0F, 1.0F});
    authorDelivery(graph.document, graph.write, path, 1, 1, "zip", "half", /*overwrite=*/false);
    const std::uint64_t second = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    EXPECT_EQ(queue->status(second).state, DeliveryState::Failed);
    EXPECT_NE(queue->status(second).error.find(path), std::string::npos);
    expectDelivered(path, kAuthoredColor, 4, 3, "half");
}

// Story 83: an accepted job keeps its own Document snapshot, so an edit made
// after submit neither changes the frames it delivers nor cancels it — while a
// NEW job sees the edit.
TEST_F(DeliveryTest, AcceptedJobFreezesTheDocumentSnapshot) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    auto queue = this->queue();
    const std::string pattern = dir_->file("shot.####.exr");
    authorDelivery(graph.document, graph.write, pattern, 1, 2);

    const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
    // The caller keeps editing immediately: the accepted job must not notice.
    setColor(graph.document, graph.color, {1.0F, 1.0F, 0.0F, 1.0F});
    queue->waitForIdle();
    EXPECT_EQ(queue->status(id).state, DeliveryState::Completed) << queue->status(id).error;
    expectDelivered(dir_->sequence("shot.", 1), kAuthoredColor, 4, 3, "half");
    expectDelivered(dir_->sequence("shot.", 2), kAuthoredColor, 4, 3, "half");

    // A job submitted after the edit delivers the edited image: the freeze
    // belongs to the accepted job, not to the queue.
    const std::string edited = dir_->file("edited.####.exr");
    authorDelivery(graph.document, graph.write, edited, 1, 1);
    const std::uint64_t second = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    EXPECT_EQ(queue->status(second).state, DeliveryState::Completed) << queue->status(second).error;
    expectDelivered(dir_->sequence("edited.", 1), {1.0F, 1.0F, 0.0F, 1.0F}, 4, 3, "half");
}

// Cancellation stops at a frame boundary: the job settles as Cancelled, the
// frames it finalized stay readable, the count it reports is honest, no hidden
// temporary survives, and the frames it did not reach are not claimed.
TEST_F(DeliveryTest, CancelledJobKeepsFinalizedFramesAndLeavesNoTemporaries) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    // The range must outlast the cancel call by a wide margin: a running job can
    // only observe cancellation between frames.
    constexpr std::int64_t kFrames = 64;
    const std::string pattern = dir_->file("many.####.exr");
    authorDelivery(graph.document, graph.write, pattern, 1, kFrames);

    auto queue = this->queue();
    const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
    ASSERT_TRUE(queue->cancel(id));
    queue->waitForIdle();

    const DeliveryJobInfo info = queue->status(id);
    EXPECT_EQ(info.state, DeliveryState::Cancelled) << info.error;
    EXPECT_EQ(info.totalFrames, static_cast<std::size_t>(kFrames));
    EXPECT_LE(info.writtenFrames, info.totalFrames);
    EXPECT_EQ(info.failedFrames, 0U);
    EXPECT_EQ(info.files.size(), info.writtenFrames);
    EXPECT_NE(info.error.find(std::to_string(info.writtenFrames)), std::string::npos) << info.error;
    EXPECT_NE(info.error.find(std::to_string(kFrames)), std::string::npos) << info.error;
    EXPECT_TRUE(dir_->temporaries().empty());
    // Finalized frames are real, readable files; the frames the job did not
    // reach are neither reported nor on disk.
    for (const DeliveryFileResult& file : info.files) {
        EXPECT_TRUE(file.written) << file.path << ": " << file.error;
        EXPECT_TRUE(fs::exists(file.path)) << file.path;
    }
    for (std::int64_t frame = static_cast<std::int64_t>(info.writtenFrames) + 1; frame <= kFrames; ++frame) {
        EXPECT_FALSE(fs::exists(dir_->sequence("many.", frame))) << frame;
    }
    // An unknown or already settled job cannot be cancelled again.
    EXPECT_FALSE(queue->cancel(id));
    EXPECT_FALSE(queue->cancel(4242));
}

// The directory policy is resolved by the worker before anything is written: a
// missing directory fails the job without authorization and is created with it.
TEST_F(DeliveryTest, MissingOutputDirectoryFollowsCreateDirectories) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    const std::string missing = (dir_->path() / "nested" / "deeper").string();
    const std::string pattern = missing + "/shot.####.exr";
    authorDelivery(graph.document, graph.write, pattern, 1, 1, "zip", "half", false, /*createDirectories=*/false);

    auto queue = this->queue();
    const DeliveryPlan refused = queue->plan(graph.document, graph.network, graph.write, 0);
    EXPECT_NE(refused.problem.find(missing), std::string::npos) << refused.problem;
    const std::uint64_t failed = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    EXPECT_EQ(queue->status(failed).state, DeliveryState::Failed);
    EXPECT_NE(queue->status(failed).error.find(missing), std::string::npos) << queue->status(failed).error;
    EXPECT_FALSE(fs::exists(missing));

    authorDelivery(graph.document, graph.write, pattern, 1, 1, "zip", "half", false, /*createDirectories=*/true);
    const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    EXPECT_EQ(queue->status(id).state, DeliveryState::Completed) << queue->status(id).error;
    expectDelivered(missing + "/shot.0001.exr", kAuthoredColor, 4, 3, "half");
}

// One frame's failure is reported with its own path and never masquerades as a
// delivered frame: the other frames of the job are still written.
TEST_F(DeliveryTest, FailedFrameNamesItsPathAndIsNotDelivered) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    const std::string pattern = dir_->file("shot.####.exr");
    // A directory occupies frame 2's destination, so the frame can be neither
    // written nor finalized there. Frame 1 is unaffected.
    const std::string blocked = dir_->sequence("shot.", 2);
    fs::create_directories(blocked);
    authorDelivery(graph.document, graph.write, pattern, 1, 2, "zip", "half", /*overwrite=*/true);

    auto queue = this->queue();
    const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();

    const DeliveryJobInfo info = queue->status(id);
    EXPECT_EQ(info.state, DeliveryState::Failed);
    EXPECT_EQ(info.writtenFrames, 1U);
    EXPECT_EQ(info.failedFrames, 1U);
    EXPECT_NE(info.error.find("1 of 2"), std::string::npos) << info.error;
    ASSERT_EQ(info.files.size(), 2U);
    EXPECT_TRUE(info.files[0].written);
    EXPECT_EQ(info.files[0].path, dir_->sequence("shot.", 1));
    EXPECT_FALSE(info.files[1].written);
    EXPECT_EQ(info.files[1].path, blocked);
    EXPECT_NE(info.files[1].error.find(blocked), std::string::npos) << info.files[1].error;

    expectDelivered(dir_->sequence("shot.", 1), kAuthoredColor, 4, 3, "half");
    EXPECT_TRUE(fs::is_directory(blocked));
    EXPECT_TRUE(dir_->temporaries().empty());
}

// The queue is bounded: the accepted-job limit is refused by name, and
// forgetting a settled job releases its place.
TEST_F(DeliveryTest, QueueRefusesBeyondItsAcceptedLimit) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    constexpr std::size_t kLimit = 3;
    auto queue =
        std::make_unique<DeliveryQueue>(*boot_.instance, *boot_.device, *boot_.allocator, slangSpvDir(), kLimit);
    std::vector<std::uint64_t> accepted;
    for (std::size_t index = 0; index < kLimit; ++index) {
        authorDelivery(graph.document, graph.write, dir_->file("take" + std::to_string(index) + ".exr"), 1, 1, "zip",
                       "half", /*overwrite=*/true);
        accepted.push_back(queue->submit(graph.document, graph.network, graph.write, 0));
    }
    queue->waitForIdle();
    for (const std::uint64_t id : accepted) {
        EXPECT_EQ(queue->status(id).state, DeliveryState::Completed) << queue->status(id).error;
    }

    authorDelivery(graph.document, graph.write, dir_->file("refused.exr"), 1, 1, "zip", "half", true);
    try {
        static_cast<void>(queue->submit(graph.document, graph.network, graph.write, 0));
        FAIL() << "a full queue must refuse a further job";
    } catch (const DeliveryException& error) {
        EXPECT_NE(std::string(error.what()).find(std::to_string(kLimit)), std::string::npos) << error.what();
    }

    EXPECT_FALSE(queue->forget(accepted.front() + 99));
    EXPECT_TRUE(queue->forget(accepted.front()));
    EXPECT_THROW(static_cast<void>(queue->status(accepted.front())), DeliveryException);
    const std::uint64_t released = queue->submit(graph.document, graph.network, graph.write, 0);
    EXPECT_NE(released, accepted.front());
    queue->waitForIdle();
    EXPECT_EQ(queue->status(released).state, DeliveryState::Completed) << queue->status(released).error;
}

// Two ACTIVE deliveries must never write the same file, even with overwrite
// authorization: the second submission is refused by name while the first is
// still running, and the path becomes reusable once that job has settled.
TEST_F(DeliveryTest, OverlappingActiveDestinationsAreRefused) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor, 64, 48);
    const std::string path = dir_->file("overlap.mov");
    authorDelivery(graph.document, graph.write, path, 1, 3, "zip", "half", /*overwrite=*/true, true, 0, "mov");

    auto queue = this->queue();
    const std::uint64_t first = queue->submit(graph.document, graph.network, graph.write, 0);
    ASSERT_NE(first, 0U);
    // While that job is active, a second job for the same destination is refused
    // before it is accepted: its reservation names both the path and the job that
    // holds it.
    try {
        static_cast<void>(queue->submit(graph.document, graph.network, graph.write, 0));
        FAIL() << "an overlapping active destination must be refused";
    } catch (const DeliveryException& error) {
        EXPECT_NE(std::string(error.what()).find(path), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.what()).find(std::to_string(first)), std::string::npos) << error.what();
    }
    // A different SPELLING of the same destination is the same reservation: the
    // submitter's comparison is lexical, so no filesystem work happens on the
    // thread that submits, and `./` segments cannot smuggle a second active job
    // past it.
    authorDelivery(graph.document, graph.write, dir_->path().string() + "/./overlap.mov", 1, 3, "zip", "half", true,
                   true, 0, "mov");
    try {
        static_cast<void>(queue->submit(graph.document, graph.network, graph.write, 0));
        FAIL() << "an equivalent spelling of an active destination must be refused";
    } catch (const DeliveryException& error) {
        EXPECT_NE(std::string(error.what()).find("overlap.mov"), std::string::npos) << error.what();
    }
    queue->waitForIdle();
    EXPECT_EQ(queue->status(first).state, DeliveryState::Completed) << queue->status(first).error;

    // A further submission of the same (now settled) destination is accepted:
    // overlap is about ACTIVE jobs, and the collision policy is what refuses an
    // unauthorized replacement.
    authorDelivery(graph.document, graph.write, path, 1, 3, "zip", "half", /*overwrite=*/false, true, 0, "mov");
    const std::uint64_t again = queue->submit(graph.document, graph.network, graph.write, 0);
    queue->waitForIdle();
    EXPECT_EQ(queue->status(again).state, DeliveryState::Failed);
    EXPECT_NE(queue->status(again).error.find(path), std::string::npos) << queue->status(again).error;
}

// A frame range is resolved without overflowing an int64 endpoint: the extremal
// range is refused with the range named, and neither a submit nor a preflight
// hangs trying to enumerate it.
TEST_F(DeliveryTest, ExtremeFrameRangeIsRefusedWithoutOverflow) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    authorDelivery(graph.document, graph.write, dir_->file("extreme.####.exr"),
                   std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());

    auto queue = this->queue();
    const DeliveryPlan refused = queue->plan(graph.document, graph.network, graph.write, 0);
    EXPECT_FALSE(refused.ok());
    EXPECT_TRUE(refused.frames.empty()) << "the plan cannot enumerate an unbounded range";
    EXPECT_NE(refused.problem.find("frames one delivery job may request"), std::string::npos) << refused.problem;
    EXPECT_THROW(static_cast<void>(queue->submit(graph.document, graph.network, graph.write, 0)), DeliveryException);

    // The offset cannot overflow the FILE number either: a range that ends at the
    // top of the domain with a positive offset is refused by name.
    authorDelivery(graph.document, graph.write, dir_->file("offset.####.exr"), 1, 4, "zip", "half", false, true,
                   std::numeric_limits<std::int64_t>::max());
    const DeliveryPlan offset = queue->plan(graph.document, graph.network, graph.write, 0);
    EXPECT_FALSE(offset.ok());
    EXPECT_NE(offset.problem.find("overflows"), std::string::npos) << offset.problem;
    EXPECT_THROW(static_cast<void>(queue->submit(graph.document, graph.network, graph.write, 0)), DeliveryException);
}

TEST_F(DeliveryTest, ActiveDestinationAliasesCannotReplaceAnOlderJob) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor);
    const std::string absolute = dir_->file("alias.exr");
    const std::string relative = fs::relative(absolute, fs::current_path()).string();
    const fs::path link = dir_->path() / "linked";
    fs::create_directory_symlink(dir_->path(), link);
    const std::string symlinked = (link / "alias.exr").string();
    auto queue = this->queue();

    // Hold actual GPU submission so the first job cannot settle before both
    // aliases are accepted. No timing assumption or mock delivery worker.
    std::unique_lock gpuGate(boot_.device->queueMutex(boot_.device->graphics_family()));
    authorDelivery(graph.document, graph.write, absolute, 1, 1, "zip", "float", true);
    const auto first = queue->submit(graph.document, graph.network, graph.write, 0);
    authorDelivery(graph.document, graph.write, relative, 1, 1, "zip", "float", true);
    const auto second = queue->submit(graph.document, graph.network, graph.write, 0);
    authorDelivery(graph.document, graph.write, symlinked, 1, 1, "zip", "float", true);
    const auto third = queue->submit(graph.document, graph.network, graph.write, 0);
    gpuGate.unlock();
    queue->waitForIdle();

    EXPECT_EQ(queue->status(first).state, DeliveryState::Completed) << queue->status(first).error;
    for (const auto id : {second, third}) {
        const auto refused = queue->status(id);
        EXPECT_EQ(refused.state, DeliveryState::Failed) << refused.error;
        EXPECT_EQ(refused.writtenFrames, 0U);
        EXPECT_NE(refused.error.find(std::to_string(first)), std::string::npos) << refused.error;
    }
    expectDelivered(absolute, kAuthoredColor, 4, 3, "float");
    EXPECT_TRUE(dir_->temporaries().empty());
}

// A movie is ONE container: it is published only after the whole range encoded,
// and a cancelled movie leaves nothing at its final path while the frames it
// encoded are reported as progress, not as delivered output.
TEST_F(DeliveryTest, MovieIsPublishedOnlyWhenTheWholeRangeEncodes) {
    NEMO_SKIP_OR_FAIL(boot_);
    DeliveryGraph graph = deliveryGraph(kAuthoredColor, 64, 48);
    auto queue = this->queue();

    // MOV (ProRes 4444 carries alpha) and MP4 (H.264) each deliver one file.
    const std::array<std::pair<std::string, std::string>, 2> formats{{{"mov", "4444"}, {"mp4", "422"}}};
    for (const auto& [fileType, profile] : formats) {
        SCOPED_TRACE(fileType);
        const std::string path = dir_->file("movie." + fileType);
        authorDelivery(graph.document, graph.write, path, 1, 2, "zip", "half", /*overwrite=*/true, true, 0, fileType);
        graph.document.network(graph.network)
            .graph()
            .setParam(graph.write, "profile", ParameterValue{ChoiceValue{profile}});
        const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
        queue->waitForIdle();
        const DeliveryJobInfo info = queue->status(id);
        EXPECT_EQ(info.state, DeliveryState::Completed) << info.error;
        EXPECT_TRUE(info.movie);
        EXPECT_EQ(info.totalFrames, 2U);
        EXPECT_EQ(info.writtenFrames, 2U);
        ASSERT_EQ(info.files.size(), 1U) << "a movie publishes exactly one file";
        EXPECT_TRUE(info.files[0].written);
        EXPECT_EQ(info.files[0].path, path);
        ASSERT_TRUE(fs::exists(path)) << path;
        EXPECT_GT(fs::file_size(path), 0U) << path;
        EXPECT_TRUE(dir_->temporaries().empty());
    }

    // A range that never encodes publishes nothing: the job is cancelled, the
    // container does not exist, and no frame is claimed as delivered.
    const std::string cancelled = dir_->file("cancelled.mov");
    authorDelivery(graph.document, graph.write, cancelled, 1, 64, "zip", "half", true, true, 0, "mov");
    const std::uint64_t id = queue->submit(graph.document, graph.network, graph.write, 0);
    ASSERT_TRUE(queue->cancel(id));
    queue->waitForIdle();
    const DeliveryJobInfo info = queue->status(id);
    EXPECT_EQ(info.state, DeliveryState::Cancelled) << info.error;
    EXPECT_FALSE(fs::exists(cancelled)) << "a cancelled movie leaves no file at its final path";
    EXPECT_TRUE(info.files.empty());
    EXPECT_EQ(info.writtenFrames, 0U) << "encoded frames are not delivered output";
    EXPECT_TRUE(dir_->temporaries().empty());
}
