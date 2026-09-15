// Color policy + OpenColorIO viewing transform tests (issue #6).
//
// Two axes:
//   1. Core: ColorPolicy serialization with documented defaults when absent.
//   2. Media/GPU: the OCIO adapter's declared error handling, and the
//      acceptance bar — OCIO's generated GPU viewing transform applied to a
//      synthetic 2x2 buffer agrees with the OCIO CPU reference within the
//      declared tolerance (operation-specific, not bitwise; LUT textures
//      are float32 so the GPU path is LUT-data exact, but OCIO's CPU
//      processor vectorizes differently).
//
// GPU tests guard themselves like GpuTests: without a usable Vulkan device
// they skip, so CI and device-less machines still pass.

#include "ScopedEnvironment.hpp"
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>
#include <nlohmann/json.hpp>

#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/InputColor.hpp"
#include "nemo/media/ViewingTransform.hpp"

using namespace nemo;

namespace {

Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

const Graph& rootGraph(const Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

// ---------------------------------------------------------------------------
// OCIO config fixture
// ---------------------------------------------------------------------------

// A minimal OCIO v2 config exercising every op shape the GPU path must
// support: inline matrix, 3D LUT (tetrahedral, from a .cube file), a
// moncurve display curve (ExponentWithLinear), and a clamp (Range). The
// "matrix" view is a pure-matrix control path.
[[nodiscard]] std::filesystem::path writeColorConfig() {
    const auto dir =
        std::filesystem::temp_directory_path() / ("nemo-color-test-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::create_directories(dir);

    std::string config;
    config += "ocio_profile_version: 2\n";
    config += "search_path: \"\"\n";
    config += "roles:\n  default: linear\n  scene_linear: working_rec709\n";
    config += "colorspaces:\n";
    config += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
    // The config-backed working space these tests use: identity with the config
    // reference, i.e. scene-linear. It is NOT named "linear" so it exercises the
    // config-backed policy path (the legacy sentinel keeps its own meaning).
    config += "  - !<ColorSpace>\n    name: working_rec709\n    allocation: linear\n";
    // An encoded input space with a real nonlinear transfer (gamma 2.2), so the
    // input-to-working conversion and the encoded-domain alpha contract are
    // exercised by an actual op chain rather than a matrix.
    config += "  - !<ColorSpace>\n    name: rec709_texture\n";
    config += "    to_reference: !<ExponentTransform> {value: [2.2, 2.2, 2.2, 1.0]}\n";
    // A name that lies: linear-looking name, encoded transfer. The working-space
    // validation must reject it by measurement, never by its name or a role.
    config += "  - !<ColorSpace>\n    name: Linear Rec.709 (sRGB)\n";
    config += "    to_reference: !<ExponentTransform> {value: [2.2, 2.2, 2.2, 1.0]}\n";
    config += "  - !<ColorSpace>\n    name: display_matrix\n";
    config +=
        "    from_reference: !<MatrixTransform> {matrix: [1.2, 0.0, 0.0, 0.0, 0.0, 1.1, 0.0, 0.0, 0.0, 0.0, 0.9, 0.0, "
        "0.0, 0.0, 0.0, 1.0]}\n";
    config += "  - !<ColorSpace>\n    name: display_view\n";
    config += "    from_reference: !<GroupTransform>\n      children:\n";
    config +=
        "        - !<MatrixTransform> {matrix: [1.0, 0.05, 0.0, 0.0, 0.02, 0.95, 0.03, 0.0, 0.0, 0.02, 1.05, 0.0, 0.0, "
        "0.0, 0.0, 1.0]}\n";
    config += "        - !<FileTransform> {src: view_lut3d.cube, interpolation: tetrahedral}\n";
    config += "        - !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}\n";
    config +=
        "        - !<RangeTransform> {min_in_value: 0.0, min_out_value: 0.0, max_in_value: 1.0, max_out_value: 1.0}\n";
    // A file rule for TIFF (and a Default fallback), so the config-file-rule
    // step of Auto resolution is a real configured rule rather than an
    // assumed built-in behavior.
    config += "file_rules:\n";
    config += "  - !<Rule> {name: NemoTIFF, pattern: '*', extension: tif, colorspace: rec709_texture}\n";
    config += "  - !<Rule> {name: Default, colorspace: linear}\n";
    config += "displays:\n  sRGB:\n    - !<View> {name: rec709, colorspace: display_view}\n";
    config += "    - !<View> {name: matrix, colorspace: display_matrix}\n";

    std::ofstream config_file(dir / "color.ocio");
    config_file << config;

    // Per-axis curved diagonal 3D LUT, 4^3, R varies fastest (Iridas .cube
    // convention).
    std::ofstream cube(dir / "view_lut3d.cube");
    cube << "TITLE \"nemo test viewing lut\"\nLUT_3D_SIZE 4\n";
    for (int b = 0; b < 4; ++b) {
        for (int g = 0; g < 4; ++g) {
            for (int r = 0; r < 4; ++r) {
                const float x = r / 3.0F;
                const float y = g / 3.0F;
                const float z = b / 3.0F;
                cube << x * (0.9F + 0.1F * y) << " " << y * (0.85F + 0.15F * z) << " " << (z * 0.95F + 0.05F * x)
                     << "\n";
            }
        }
    }
    config_file.close();
    cube.close();
    return dir / "color.ocio";
}

[[nodiscard]] CpuImage sample2x2() {
    CpuImage image(2, 2);
    image.setPixel(0, 0, {0.0F, 0.0F, 0.0F, 1.0F});
    image.setPixel(1, 0, {0.25F, 0.5F, 0.75F, 1.0F});
    image.setPixel(0, 1, {1.0F, 0.75F, 0.5F, 1.0F});
    image.setPixel(1, 1, {0.5F, 0.5F, 0.5F, 1.0F});
    return image;
}

[[nodiscard]] std::vector<float> sample2x2Flat() {
    const CpuImage image = sample2x2();
    return {image.data(), image.data() + 2 * 2 * 4};
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

// Zero validation warnings/errors matches the GpuTests bar; benign info
// chatter (e.g. physical-device sorting notes) does not fail the check.

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

// Runs the adapter's GPU program over `pixels` (RGBA float32, count =
// pixels.size()/4) through the device-resident path (issue #22): the
// pixels upload into a device image, GpuViewingTransform submits
// asynchronously on the device's shared queue (no host transfer inside the
// graph), and the displayed image is read back through the declared
// diagnostic download. Returns the transformed pixels.
[[nodiscard]] std::vector<float> runGpuProgram(const Bootstrap& boot, const media::OcioGpuProgram& program,
                                               const std::vector<float>& pixels,
                                               uint64_t timeout_ns = 5'000'000'000ULL) {
    const std::size_t pixelCount = pixels.size() / 4;
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(pixels.size() * sizeof(float));
    const uint32_t width = 2;
    const uint32_t height = static_cast<uint32_t>(pixelCount / width);
    EXPECT_EQ(static_cast<std::size_t>(width) * height, pixelCount);

    gpu::SubmissionQueue& queue = boot.device->submissions(boot.device->graphics_family());

    // Source image: RGBA32F 2D, uploaded then transitioned to GENERAL —
    // the layout convention GpuViewingTransform's submit() documents.
    gpu::Image source = boot.allocator->create_image(width, height, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                         VK_IMAGE_USAGE_SAMPLED_BIT);
    gpu::uploadImage(queue, *boot.allocator, source, pixels.data(), byteSize, timeout_ns);
    gpu::imageBarrier(queue, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, timeout_ns);

    gpu::GpuViewingTransform transform(*boot.device, *boot.allocator, program);
    std::optional<gpu::GpuViewedImage> viewed = transform.submit(source, ColorInterpretation::SceneLinear);
    EXPECT_TRUE(viewed.has_value());
    if (!viewed.has_value()) {
        return {};
    }
    EXPECT_TRUE(queue.wait(viewed->completion, timeout_ns));

    std::vector<float> result(pixels.size());
    gpu::downloadImage(queue, *boot.allocator, viewed->image, result.data(), byteSize, timeout_ns);
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Policy serialization (acceptance example 1)
// ---------------------------------------------------------------------------

TEST(Color, PolicyRoundTrips) {
    Document doc;
    doc.name = "graded";
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    EXPECT_NE(plate, kInvalidNode);
    doc.color.workingSpace = "ACEScg";
    doc.color.viewerTransform = "sRGB/Display P3";
    doc.color.deliveryTransform = "Rec709/Rec.1886";

    const LoadResult loaded = loadDocument(saveDocument(doc));
    EXPECT_TRUE(loaded.warnings.empty());
    EXPECT_EQ(loaded.document.color, doc.color);
}

TEST(Color, DefaultPolicyWhenAbsent) {
    // A document saved without the policy block (e.g. by an older build)
    // loads with the documented defaults and no warnings.
    Document doc;
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    EXPECT_NE(plate, kInvalidNode);
    nlohmann::json saved = saveDocument(doc);
    ASSERT_TRUE(saved.contains("color"));
    saved.erase("color");
    const LoadResult loaded = loadDocument(saved);
    EXPECT_TRUE(loaded.warnings.empty());
    EXPECT_EQ(loaded.document.color, ColorPolicy{});
    EXPECT_EQ(loaded.document.color.workingSpace, "linear");
    EXPECT_EQ(loaded.document.color.viewerTransform, "sRGB/rec709");
    EXPECT_EQ(loaded.document.color.deliveryTransform, "sRGB/rec709");
}

TEST(Color, PartialPolicyFallsBackFieldWise) {
    Document doc;
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    EXPECT_NE(plate, kInvalidNode);
    nlohmann::json saved = saveDocument(doc);
    saved["color"] = {{"workingSpace", "ACEScg"}};
    const LoadResult loaded = loadDocument(saved);
    EXPECT_EQ(loaded.document.color.workingSpace, "ACEScg");
    EXPECT_EQ(loaded.document.color.viewerTransform, "sRGB/rec709");
    EXPECT_EQ(loaded.document.color.deliveryTransform, "sRGB/rec709");
}

TEST(Color, NonObjectPolicyWarnsAndUsesDefaults) {
    Document doc;
    const NodeId plate = rootGraph(doc).addNode("testpattern", "plate");
    EXPECT_NE(plate, kInvalidNode);
    nlohmann::json saved = saveDocument(doc);
    saved["color"] = 42;
    const LoadResult loaded = loadDocument(saved);
    ASSERT_EQ(loaded.warnings.size(), 1u);
    EXPECT_NE(loaded.warnings.front().find("color"), std::string::npos);
    EXPECT_EQ(loaded.document.color, ColorPolicy{});
}

// ---------------------------------------------------------------------------
// OCIO adapter: declared error handling (config/transform names missing)
// ---------------------------------------------------------------------------

TEST(Color, MissingConfigNamesPath) {
    try {
        (void)media::resolveConfigPath("/nonexistent/color.ocio");
        ADD_FAILURE() << "expected OcioException";
    } catch (const media::OcioException& error) {
        EXPECT_NE(std::string(error.what()).find("/nonexistent/color.ocio"), std::string::npos);
    }
    try {
        // Empty request and no OCIO environment variable.
        const test::ScopedEnvironment ocio("OCIO", std::nullopt);
        (void)media::resolveConfigPath({});
        ADD_FAILURE() << "expected OcioException";
    } catch (const media::OcioException& error) {
        EXPECT_NE(std::string(error.what()).find("OCIO"), std::string::npos);
    }
}

TEST(Color, MissingNamesAreIdentified) {
    const auto configPath = writeColorConfig();
    SCOPED_TRACE("config " + configPath.string());

    // Unknown working space.
    try {
        CpuImage scratch = sample2x2();
        media::applyViewingTransformCpu(scratch, configPath.string(), "not-a-space", "sRGB/rec709");
        ADD_FAILURE() << "expected OcioException";
    } catch (const media::OcioException& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("not-a-space"), std::string::npos);
        EXPECT_NE(message.find(configPath.string()), std::string::npos);
    }

    // Unknown view on a known display.
    try {
        CpuImage scratch = sample2x2();
        media::applyViewingTransformCpu(scratch, configPath.string(), "linear", "sRGB/nosuchview");
        ADD_FAILURE() << "expected OcioException";
    } catch (const media::OcioException& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("nosuchview"), std::string::npos);
        EXPECT_NE(message.find("sRGB"), std::string::npos);
    }

    // Unknown display.
    try {
        CpuImage scratch = sample2x2();
        media::applyViewingTransformCpu(scratch, configPath.string(), "linear", "HDR/rec709");
        ADD_FAILURE() << "expected OcioException";
    } catch (const media::OcioException& error) {
        EXPECT_NE(std::string(error.what()).find("HDR"), std::string::npos);
    }
}

TEST(Color, CpuReferenceAppliesViewingTransform) {
    const auto configPath = writeColorConfig();
    CpuImage image = sample2x2();
    media::applyViewingTransformCpu(image, configPath.string(), ColorPolicy{});
    EXPECT_EQ(image.layout().color, ColorInterpretation::DisplayReferred);

    // The composite transform is non-identity: black stays black (0.0
    // through matrix/LUT/curve with the range clamp as floor) but mid-grays
    // move measurably.
    const auto black = image.pixel(0, 0);
    EXPECT_NEAR(black[0], 0.0F, 1e-6F);
    const auto gray = image.pixel(1, 1);
    // 0.5 scene-linear gray through matrix + LUT + sRGB-like curve lands in
    // display-light territory; the point of the check is that the transform
    // measurably changed the pixel rather than pinning OCIO's exact value.
    EXPECT_GT(gray[0], 0.6F);
    EXPECT_LT(gray[0], 0.85F);
}

// ---------------------------------------------------------------------------
// GPU-native path vs CPU reference (acceptance example 2)
// ---------------------------------------------------------------------------

TEST(Color, GpuViewingTransformMatchesCpuReferenceWithinTolerance) {
    const auto configPath = writeColorConfig();
    const media::OcioGpuProgram program = media::buildViewingTransformGpu(configPath.string(), "linear", "sRGB/rec709");

    // CPU reference and GPU run the same scene-linear input.
    CpuImage cpuImage = sample2x2();
    media::applyViewingTransformCpu(cpuImage, configPath.string(), ColorPolicy{});

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const std::vector<float> gpuPixels = runGpuProgram(boot, program, sample2x2Flat());

    // Declared tolerance for this operation: OCIO's CPU processor and its
    // generated GLSL evaluate the same op sequence (inline matrix, shader
    // tetrahedral LUT3D over float32 textures, moncurve gamma, range clamp);
    // differences come from CPU vectorization order. 2e-5 per channel is the
    // operation-specific bar for this scene-linear→display transform.
    constexpr float kTolerance = 2e-5F;
    for (int y = 0; y < cpuImage.height(); ++y) {
        for (int x = 0; x < cpuImage.width(); ++x) {
            const auto expected = cpuImage.pixel(x, y);
            const auto* actual = &gpuPixels[(static_cast<std::size_t>(y) * 2 + x) * 4];
            EXPECT_NEAR(actual[0], expected[0], kTolerance) << "pixel (" << x << "," << y << ") R";
            EXPECT_NEAR(actual[1], expected[1], kTolerance) << "pixel (" << x << "," << y << ") G";
            EXPECT_NEAR(actual[2], expected[2], kTolerance) << "pixel (" << x << "," << y << ") B";
            EXPECT_NEAR(actual[3], expected[3], 0.0F) << "pixel (" << x << "," << y << ") A";
        }
    }
    expectValidationClean(*boot.instance);
}

TEST(Color, GpuAces2ViewMatchesCpuReference) {
    const std::string configPath = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
    const std::string workingSpace = "Linear Rec.709 (sRGB)";
    const std::string view = "sRGB - Display/ACES 2.0 - SDR 100 nits (Rec.709)";
    // The ACES2 gamut cusp table uses tightly packed RGB texels, unlike
    // the RED-channel table and the 3D LUT exercised by the other views.
    const auto program = media::buildViewingTransformGpu(configPath, workingSpace, view);
    const std::vector<float> pixels{0.0F, 0.0F, 0.0F,   1.0F, 1.0F, 0.0F,  0.0F,  1.0F,  0.0F, 1.0F, 0.0F,
                                    0.5F, 0.0F, 0.0F,   1.0F, 0.0F, 0.18F, 0.18F, 0.18F, 1.0F, 4.0F, 4.0F,
                                    4.0F, 1.0F, -0.01F, 0.2F, 1.5F, 0.75F, 2.0F,  0.03F, 0.2F, 1.0F};
    CpuImage cpuImage(2, 4);
    std::memcpy(cpuImage.data(), pixels.data(), pixels.size() * sizeof(float));
    media::applyViewingTransformCpu(cpuImage, configPath, workingSpace, view);

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const auto gpuPixels = runGpuProgram(boot, program, pixels);
    ASSERT_EQ(gpuPixels.size(), pixels.size());
    // ACES2's nonlinear float32 shader may differ from OCIO's vectorized
    // CPU processor; this bound is below 0.06 of an 8-bit display code.
    constexpr float kTolerance = 2e-4F;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if (i % 4 == 3) {
            EXPECT_FLOAT_EQ(gpuPixels[i], pixels[i]) << "pixel " << i / 4 << " alpha";
        } else {
            EXPECT_NEAR(gpuPixels[i], cpuImage.data()[i], kTolerance) << "sample " << i;
        }
    }
    expectValidationClean(*boot.instance);
}

TEST(Color, GpuMatrixViewMatchesCpuReferenceWithinTolerance) {
    // Pure-matrix control path: no LUTs, no textures, no uniforms — both
    // sides evaluate the identical formula in float32.
    const auto configPath = writeColorConfig();
    const media::OcioGpuProgram program = media::buildViewingTransformGpu(configPath.string(), "linear", "sRGB/matrix");

    CpuImage cpuImage = sample2x2();
    media::applyViewingTransformCpu(cpuImage, configPath.string(), "linear", "sRGB/matrix");

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const std::vector<float> gpuPixels = runGpuProgram(boot, program, sample2x2Flat());

    constexpr float kTolerance = 1e-6F;
    for (int y = 0; y < cpuImage.height(); ++y) {
        for (int x = 0; x < cpuImage.width(); ++x) {
            const auto expected = cpuImage.pixel(x, y);
            const auto* actual = &gpuPixels[(static_cast<std::size_t>(y) * 2 + x) * 4];
            EXPECT_NEAR(actual[0], expected[0], kTolerance) << "pixel (" << x << "," << y << ") R";
            EXPECT_NEAR(actual[1], expected[1], kTolerance) << "pixel (" << x << "," << y << ") G";
            EXPECT_NEAR(actual[2], expected[2], kTolerance) << "pixel (" << x << "," << y << ") B";
            EXPECT_NEAR(actual[3], expected[3], 0.0F) << "pixel (" << x << "," << y << ") A";
        }
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Compute pipeline cache + retained execution (issue #22)
// ---------------------------------------------------------------------------

namespace {

// Minimal scale kernel: out[i] = in[i] * scale over vec4s. Same descriptor
// layout shape for every pass in the tests below, so the device-owned
// pipeline cache must serve them all from one VkPipeline.
constexpr const char* kScaleKernel = R"GLSL(#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer InData { vec4 inData[]; };
layout(std430, set = 0, binding = 1) writeonly buffer OutData { vec4 outData[]; };
layout(std140, set = 1, binding = 0) uniform Scale { vec4 scale; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= inData.length()) return;
    outData[i] = inData[i] * scale;
}
)GLSL";

constexpr uint64_t kGpuTimeoutNs = 5'000'000'000ULL;

}  // namespace

// Repeated pipeline reuse with independently expected pixels: two passes
// over the same SPIR-V + layout shape but different input buffers and
// uniform params run through one cached pipeline, interleaved. Immutable
// descriptor contents must not leak between passes or recycled bundles.
TEST(Color, ComputePipelineCacheReusesPipelineAcrossPasses) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const std::vector<std::uint32_t> spirv = gpu::compileGlslToSpirv(kScaleKernel);

    // Pass A: input ×2.0.
    gpu::Buffer inA = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    gpu::MemoryPreference::HostMapped);
    gpu::Buffer outA = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     gpu::MemoryPreference::HostMapped);
    gpu::Buffer uniA = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                     gpu::MemoryPreference::HostMapped);
    const float inputA[4] = {0.5F, 1.0F, 1.5F, 2.0F};
    const float scaleA[4] = {2.0F, 2.0F, 2.0F, 2.0F};
    std::memcpy(inA.mapped(), inputA, sizeof(inputA));
    std::memcpy(uniA.mapped(), scaleA, sizeof(scaleA));
    const float expectedA[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    std::vector<gpu::ComputeBinding> bindingsA = {
        {0, 0, gpu::DescriptorKind::StorageBuffer, &inA, nullptr, false},
        {0, 1, gpu::DescriptorKind::StorageBuffer, &outA, nullptr, false},
        {1, 0, gpu::DescriptorKind::UniformBuffer, &uniA, nullptr, false},
    };
    auto passA = gpu::ComputePass::create(*boot.device, spirv, bindingsA);

    passA->dispatch(1, 1, 1, kGpuTimeoutNs);
    const auto* gotA = static_cast<const float*>(outA.mapped());
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(gotA[i], expectedA[i]) << "pass A element " << i;
    }

    // Pass B: same shader and layout shape, different input and params.
    // The device cache must serve it without building another pipeline.
    const uint64_t pipelinesBefore = boot.device->submissionStats().pipelineCreations;
    gpu::Buffer inB = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    gpu::MemoryPreference::HostMapped);
    gpu::Buffer outB = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     gpu::MemoryPreference::HostMapped);
    gpu::Buffer uniB = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                     gpu::MemoryPreference::HostMapped);
    const float inputB[4] = {2.0F, 1.5F, 1.0F, 0.5F};
    const float scaleB[4] = {0.5F, 0.5F, 0.5F, 0.5F};
    std::memcpy(inB.mapped(), inputB, sizeof(inputB));
    std::memcpy(uniB.mapped(), scaleB, sizeof(scaleB));
    const float expectedB[4] = {1.0F, 0.75F, 0.5F, 0.25F};
    std::vector<gpu::ComputeBinding> bindingsB = {
        {0, 0, gpu::DescriptorKind::StorageBuffer, &inB, nullptr, false},
        {0, 1, gpu::DescriptorKind::StorageBuffer, &outB, nullptr, false},
        {1, 0, gpu::DescriptorKind::UniformBuffer, &uniB, nullptr, false},
    };
    auto passB = gpu::ComputePass::create(*boot.device, spirv, bindingsB);
    EXPECT_EQ(boot.device->submissionStats().pipelineCreations, pipelinesBefore)
        << "compatible layout/shader must reuse the cached pipeline";

    passB->dispatch(1, 1, 1, kGpuTimeoutNs);
    const auto* gotB = static_cast<const float*>(outB.mapped());
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(gotB[i], expectedB[i]) << "pass B element " << i;
    }

    // Returning to A must not pick up B's descriptor contents.
    passA->dispatch(1, 1, 1, kGpuTimeoutNs);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(gotA[i], expectedA[i]) << "pass A re-dispatch element " << i;
    }
    EXPECT_EQ(boot.device->submissionStats().pipelineCreations, pipelinesBefore);
    // Same layout, different shader: a layout-only cache key is incorrect.
    std::string addKernel = kScaleKernel;
    const std::string expression = "inData[i] * scale";
    addKernel.replace(addKernel.find(expression), expression.size(), "inData[i] + scale");
    auto addition = gpu::ComputePass::create(*boot.device, gpu::compileGlslToSpirv(addKernel), bindingsA);
    addition->dispatch(1, 1, 1, kGpuTimeoutNs);
    const float expectedSum[4] = {2.5F, 3.0F, 3.5F, 4.0F};
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(gotA[i], expectedSum[i]);
    expectValidationClean(*boot.instance);
}

// Retained execution: the pass may be destroyed while its recorded work is
// in flight — the retain() token keeps the descriptor bundle, samplers, and
// bound resources alive until the fence signals.
TEST(Color, ComputeRetainedExecutionSurvivesPassDestruction) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);

    const std::vector<std::uint32_t> spirv = gpu::compileGlslToSpirv(kScaleKernel);
    gpu::Buffer inC = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    gpu::MemoryPreference::HostMapped);
    gpu::Buffer outC = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                     gpu::MemoryPreference::HostMapped);
    gpu::Buffer uniC = boot.allocator->create_buffer(4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                     gpu::MemoryPreference::HostMapped);
    const float inputC[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    const float scaleC[4] = {3.0F, 3.0F, 3.0F, 3.0F};
    std::memcpy(inC.mapped(), inputC, sizeof(inputC));
    std::memcpy(uniC.mapped(), scaleC, sizeof(scaleC));
    std::vector<gpu::ComputeBinding> bindingsC = {
        {0, 0, gpu::DescriptorKind::StorageBuffer, &inC, nullptr, false},
        {0, 1, gpu::DescriptorKind::StorageBuffer, &outC, nullptr, false},
        {1, 0, gpu::DescriptorKind::UniformBuffer, &uniC, nullptr, false},
    };
    auto pass = gpu::ComputePass::create(*boot.device, spirv, bindingsC);

    inC = gpu::Buffer{};
    uniC = gpu::Buffer{};
    auto token = pass->retain();
    auto completion = boot.device->submissions(boot.device->graphics_family())
                          .submit([&](VkCommandBuffer cmd) { pass->record(cmd, 1, 1, 1); }, {std::move(token)});
    ASSERT_TRUE(completion.has_value());
    pass.reset();  // the submission stays in flight without the pass
    EXPECT_TRUE(boot.device->submissions(boot.device->graphics_family()).wait(*completion, kGpuTimeoutNs));

    const auto* gotC = static_cast<const float*>(outC.mapped());
    const float expectedC[4] = {3.0F, 6.0F, 9.0F, 12.0F};
    for (int i = 0; i < 4; ++i)
        EXPECT_FLOAT_EQ(gotC[i], expectedC[i]) << "retained dispatch element " << i;
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// Input color: OCIO input-to-working conversion, Raw/Data, alpha (issue #81)
//
// The acceptance bar is an independently derived pixel, not the adapter
// agreeing with itself: every expected value below is computed in the test
// from a published definition (the sRGB EOTF, a gamma power, the Rec.709
// working space being linear) and the resolved interpretation/identity is
// asserted from the public resolution seam.
// ---------------------------------------------------------------------------

namespace {

using nemo::media::EncodedColorFacts;
using nemo::media::ImageFrame;
using nemo::media::ImageFrameInfo;
using nemo::media::ImagePrimaries;
using nemo::media::ImageTransfer;
using nemo::media::InputColorCache;
using nemo::media::InputColorChoice;
using nemo::media::InputTransformKind;
using nemo::media::InputTransformOrigin;
using nemo::media::ResolvedAlpha;
using nemo::media::ResolvedInputColor;
using nemo::media::SourceColorPolicy;

// The declared RGB accuracy bar for every independent analytical comparison in
// this section, one owner: the pinned OCIO 2.5.2 CPU/GPU processors fast-math
// their op chains, and the measured divergence for these input transforms is
// <=4.5e-6 per channel (2.408e-5 for the pinned Studio config's sRGB space).
// 1e-4 carries ~20x headroom over the measured divergence — the same bar the
// CPU/GPU parity checks use — while a wrong or missing conversion differs by
// 1e-1 or more, so the check still has full discriminating power. Alpha is
// exact and is asserted with EXPECT_FLOAT_EQ, never through this bar.
constexpr float kRgbAccuracyTolerance = 1e-4F;

// The published sRGB inverse EOTF, computed here rather than taken from the
// adapter.
[[nodiscard]] double srgbToLinear(const double value) {
    return value < 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] CpuImage knownPixel(const std::array<float, 4>& rgba) {
    CpuImage image(1, 1);
    image.setPixel(0, 0, rgba);
    return image;
}

// One EXR pixel with an explicit declared color space and an alpha channel:
// OIIO reports EXR alpha as premultiplied by convention, so this fixture
// exercises Auto association AND a declared transfer at once.
[[nodiscard]] std::filesystem::path writeDeclaredExr(const std::string& name, const std::array<float, 4>& pixel,
                                                     const char* colorSpace) {
    const auto dir =
        std::filesystem::temp_directory_path() / ("nemo-input-color-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    auto output = OIIO::ImageOutput::create(path.string());
    if (!output) {
        throw std::runtime_error(OIIO::geterror());
    }
    OIIO::ImageSpec spec(1, 1, 4, OIIO::TypeDesc::FLOAT);
    spec.channelnames = {"R", "G", "B", "A"};
    if (colorSpace != nullptr) {
        spec.attribute("oiio:ColorSpace", colorSpace);
    }
    if (!output->open(path.string(), spec) || !output->write_image(OIIO::TypeDesc::FLOAT, pixel.data()) ||
        !output->close()) {
        throw std::runtime_error(OIIO::geterror());
    }
    return path;
}

// A TIFF that declares nothing, written through the shared still writer.
[[nodiscard]] std::filesystem::path writeUndeclaredTiff(const std::string& name, const std::array<float, 4>& pixel) {
    const auto dir =
        std::filesystem::temp_directory_path() / ("nemo-input-color-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    nemo::media::writeImage(path.string(), knownPixel(pixel), nemo::media::OutputPrecision::Float32);
    return path;
}

[[nodiscard]] EvaluationRequest rasterRequest(int width, int height) {
    EvaluationRequest request;
    request.region = {0, 0, width, height};
    return request;
}

// A hand-built resolved request: this test drives the provider seam directly,
// so no catalog/node authoring is involved.
[[nodiscard]] EffectiveSourceRequest sourceRequest(const std::string& key, const std::string& path) {
    EffectiveSourceRequest source;
    source.sourceKey = key;
    source.path = path;
    source.sourceFrame = 0;
    source.readFrame = 0;
    return source;
}

// Rewrites the fixture config at the SAME path with a different gamma and an
// extra comment line, so both the content and the file size change (the
// freshness stamp is then guaranteed to move on any filesystem).
void rewriteConfigGamma(const std::filesystem::path& configPath, const std::string& gamma) {
    std::ifstream in(configPath);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::string replaced;
    const std::string needle = "2.2, 2.2, 2.2";
    for (std::size_t pos = 0; pos < text.size();) {
        const auto hit = text.find(needle, pos);
        if (hit == std::string::npos) {
            replaced.append(text, pos, std::string::npos);
            break;
        }
        replaced.append(text, pos, hit - pos);
        replaced += gamma + ", " + gamma + ", " + gamma;
        pos = hit + needle.size();
    }
    replaced += "\n# rewritten by the input-color test\n";
    std::ofstream out(configPath, std::ios::trunc);
    out << replaced;
}

}  // namespace

// The owner-approved new-project default: the pinned built-in ACES Studio
// config, its real scene-linear Rec.709 working space and the ACES 2.0 SDR
// view, with an explicit $OCIO override preserved.
TEST(InputColor, NewProjectDefaultIsThePinnedAcesStudioConfig) {
    {
        // No environment override: the project authors the registered reference.
        const test::ScopedEnvironment ocio("OCIO", std::nullopt);
        const nemo::media::NewProjectColorDefault defaulted = nemo::media::newProjectColorDefault();
        EXPECT_EQ(defaulted.configUri, std::string{nemo::kBuiltinColorConfigUri});
        EXPECT_TRUE(nemo::isRegisteredColorConfigReference(defaulted.configUri));
        EXPECT_EQ(defaulted.policy.workingSpace, "Linear Rec.709 (sRGB)");
        EXPECT_EQ(defaulted.policy.viewerTransform, "sRGB - Display/ACES 2.0 - SDR 100 nits (Rec.709)");
        EXPECT_EQ(defaulted.policy.deliveryTransform, defaulted.policy.viewerTransform);

        // The named working space really is scene-linear Rec.709 in that config,
        // and the searchable Input Transform list comes from the config itself.
        nemo::media::requireSceneLinearRec709(defaulted.configUri, defaulted.policy.workingSpace, "test");
        const std::vector<std::string> spaces = nemo::media::configInputColorSpaces(defaulted.configUri);
        EXPECT_GT(spaces.size(), 40u);
        EXPECT_NE(std::find(spaces.begin(), spaces.end(), defaulted.policy.workingSpace), spaces.end());
        // Canonical published entries, not incidental alias inventory: the
        // active list is what the Input Transform control offers.
        for (const char* canonical : {"sRGB Encoded Rec.709 (sRGB)", "ACES2065-1", "ACEScg"}) {
            EXPECT_NE(std::find(spaces.begin(), spaces.end(), std::string{canonical}), spaces.end()) << canonical;
        }
        EXPECT_TRUE(std::is_sorted(spaces.begin(), spaces.end()));
        EXPECT_EQ(std::adjacent_find(spaces.begin(), spaces.end()), spaces.end());
        EXPECT_FALSE(nemo::media::colorConfigIdentity(defaulted.configUri).empty());
    }
    {
        // An explicit OCIO environment is an owner override: the new project
        // authors no reference and resolves through the environment instead.
        const test::ScopedEnvironment ocio("OCIO", "/tmp/nemo-explicit-env.ocio");
        const nemo::media::NewProjectColorDefault overridden = nemo::media::newProjectColorDefault();
        EXPECT_TRUE(overridden.configUri.empty());
        EXPECT_EQ(overridden.policy.workingSpace, "Linear Rec.709 (sRGB)");
    }
}

// The working target is validated by MEANING: a name that claims Rec.709 but
// carries an encoded transfer, the ACES scene_linear role (ACEScg), and a
// display space are all rejected; the pinned config's real Rec.709 space and
// the config's own linear space are accepted.
TEST(InputColor, WorkingTargetIsVerifiedByMeaningNotNameOrRole) {
    const auto configPath = writeColorConfig();
    SCOPED_TRACE("config " + configPath.string());
    nemo::media::requireSceneLinearRec709(configPath.string(), "working_rec709", "test");
    nemo::media::requireSceneLinearRec709(configPath.string(), "linear", "test");

    for (const char* encoded : {"rec709_texture", "Linear Rec.709 (sRGB)", "display_view"}) {
        try {
            nemo::media::requireSceneLinearRec709(configPath.string(), encoded, "test");
            ADD_FAILURE() << "expected '" << encoded << "' to be rejected as a working space";
        } catch (const nemo::media::OcioException& error) {
            EXPECT_NE(std::string(error.what()).find(encoded), std::string::npos) << error.what();
        }
    }
    try {
        nemo::media::requireSceneLinearRec709(configPath.string(), "not-a-space", "test");
        ADD_FAILURE() << "expected an unknown working space to be rejected";
    } catch (const nemo::media::OcioException& error) {
        EXPECT_NE(std::string(error.what()).find("not-a-space"), std::string::npos) << error.what();
    }

    // The pinned default: accepted, while the role-named scene_linear space
    // (ACEScg) is a different gamut and is refused.
    const std::string pinned{nemo::kBuiltinColorConfigUri};
    nemo::media::requireSceneLinearRec709(pinned, "Linear Rec.709 (sRGB)", "test");
    for (const char* other : {"ACEScg", "sRGB Encoded Rec.709 (sRGB)", "sRGB - Display"}) {
        try {
            nemo::media::requireSceneLinearRec709(pinned, other, "test");
            ADD_FAILURE() << "expected '" << other << "' to be rejected as a working space";
        } catch (const nemo::media::OcioException& error) {
            EXPECT_NE(std::string(error.what()).find(other), std::string::npos) << error.what();
        }
    }
}

// The real config-backed conversion: a named input space converts encoded
// samples into the working space, checked against the published sRGB EOTF
// computed in the test, with alpha passing through untouched.
TEST(InputColor, NamedInputSpaceConvertsToWorkingAgainstPublishedReference) {
    const std::string pinned{nemo::kBuiltinColorConfigUri};
    const nemo::media::OcioInputTransform transform(pinned, "Linear Rec.709 (sRGB)", "sRGB Encoded Rec.709 (sRGB)");
    EXPECT_EQ(transform.inputColorSpace(), "sRGB Encoded Rec.709 (sRGB)");
    EXPECT_NE(transform.identity().find("sRGB Encoded Rec.709 (sRGB)"), std::string::npos);

    CpuImage image(2, 1);
    image.setPixel(0, 0, {0.5F, 0.25F, 0.75F, 1.0F});
    image.setPixel(1, 0, {0.0F, 1.0F, 0.04045F, 0.25F});
    transform.apply(image);
    EXPECT_EQ(image.layout().color, ColorInterpretation::SceneLinear);

    const auto pixel = image.pixel(0, 0);
    EXPECT_NEAR(pixel[0], srgbToLinear(0.5), 1e-4F);
    EXPECT_NEAR(pixel[1], srgbToLinear(0.25), 1e-4F);
    EXPECT_NEAR(pixel[2], srgbToLinear(0.75), 1e-4F);
    EXPECT_FLOAT_EQ(pixel[3], 1.0F);
    const auto alphaPixel = image.pixel(1, 0);
    EXPECT_NEAR(alphaPixel[0], 0.0F, kRgbAccuracyTolerance);
    EXPECT_NEAR(alphaPixel[1], 1.0F, kRgbAccuracyTolerance);
    // The sRGB linear-segment knee: 0.04045 encodes 0.0031308.
    EXPECT_NEAR(alphaPixel[2], srgbToLinear(0.04045), kRgbAccuracyTolerance);
    EXPECT_FLOAT_EQ(alphaPixel[3], 0.25F);

    // A missing input space is an error naming the space and the config.
    try {
        const nemo::media::OcioInputTransform missing(pinned, "Linear Rec.709 (sRGB)", "no-such-space");
        ADD_FAILURE() << "expected the missing input space to be rejected";
    } catch (const nemo::media::OcioException& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("no-such-space"), std::string::npos) << what;
        EXPECT_NE(what.find("config"), std::string::npos) << what;
    }
}

// Precedence and origin: an explicit override wins; Auto lets tagged media
// win over fill-only hints; an undeclared file falls through to the config's
// own file rule (reported as a rule, never as file metadata); Raw bypasses;
// and a partial hint is an error rather than a silent completion.
TEST(InputColor, ResolutionPrecedenceAndOrigin) {
    const auto configPath = writeColorConfig();
    const SourceColorPolicy policy{configPath.string(), "working_rec709"};
    const std::string tiff = (configPath.parent_path() / "undeclared.tif").string();

    // 1. Explicit named input space wins over a conflicting hint.
    {
        InputColorChoice choice;
        choice.mode = nemo::InputTransformMode::Explicit;
        choice.inputColorSpace = "rec709_texture";
        choice.hints["transfer"] = "linear";
        EncodedColorFacts facts;
        facts.formatName = "openexr";
        const ResolvedInputColor resolved = nemo::media::resolveInputColor(policy, choice, facts, tiff, "test");
        EXPECT_EQ(resolved.kind, InputTransformKind::OcioColorspace);
        EXPECT_EQ(resolved.colorSpace, "rec709_texture");
        EXPECT_EQ(resolved.origin, InputTransformOrigin::NodeOcioOverride);
    }

    // 2. Auto with tagged media: the file declaration wins, the hint is inert.
    {
        InputColorChoice choice;
        choice.hints["transfer"] = "gamma28";
        EncodedColorFacts facts;
        facts.formatName = "png";
        facts.declaredColorSpace = "srgb_rec709_scene";
        const ResolvedInputColor resolved = nemo::media::resolveInputColor(policy, choice, facts, tiff, "test");
        EXPECT_EQ(resolved.kind, InputTransformKind::MetadataTransfer);
        EXPECT_EQ(resolved.transfer, ImageTransfer::Srgb);
        EXPECT_EQ(resolved.origin, InputTransformOrigin::FileMetadata);
    }

    // 3. Auto with nothing declared: the config's file rule for the path.
    {
        InputColorChoice choice;
        EncodedColorFacts facts;
        facts.formatName = "tiff";
        const ResolvedInputColor tiffRule = nemo::media::resolveInputColor(policy, choice, facts, tiff, "test");
        EXPECT_EQ(tiffRule.kind, InputTransformKind::OcioColorspace);
        EXPECT_EQ(tiffRule.colorSpace, "rec709_texture");
        EXPECT_EQ(tiffRule.origin, InputTransformOrigin::ConfigFileRule);
        // A path the file rules do not match resolves through the DEFAULT rule,
        // reported as a configured default rather than a media declaration.
        const ResolvedInputColor defaultRule = nemo::media::resolveInputColor(
            policy, choice, facts, (configPath.parent_path() / "undeclared.png").string(), "test");
        EXPECT_EQ(defaultRule.kind, InputTransformKind::OcioColorspace);
        EXPECT_EQ(defaultRule.origin, InputTransformOrigin::ConfigDefaultRule);
    }

    // 4. Auto with only hints: the legacy metadata interpretation, with the
    // node scope distinguishing NodeInterpretation from SourceInterpretation.
    {
        InputColorChoice choice;
        choice.hints["transfer"] = "gamma22";
        choice.hints["primaries"] = "bt709";
        EncodedColorFacts facts;
        facts.formatName = "tiff";
        const ResolvedInputColor sourceScoped = nemo::media::resolveInputColor(policy, choice, facts, tiff, "test");
        EXPECT_EQ(sourceScoped.kind, InputTransformKind::MetadataTransfer);
        EXPECT_EQ(sourceScoped.transfer, ImageTransfer::Gamma22);
        EXPECT_EQ(sourceScoped.origin, InputTransformOrigin::SourceInterpretation);
        choice.nodeHintKeys = nemo::kSourceHintTransferBit | nemo::kSourceHintPrimariesBit;
        const ResolvedInputColor nodeScoped = nemo::media::resolveInputColor(policy, choice, facts, tiff, "test");
        EXPECT_EQ(nodeScoped.origin, InputTransformOrigin::NodeInterpretation);
    }

    // 5. A partial hint is an authored but incomplete interpretation: an error
    // naming the missing half, never a silent config completion.
    {
        InputColorChoice choice;
        choice.hints["transfer"] = "gamma22";
        EncodedColorFacts facts;
        facts.formatName = "tiff";
        try {
            static_cast<void>(nemo::media::resolveInputColor(policy, choice, facts, tiff, "test"));
            ADD_FAILURE() << "expected a partial interpretation hint to be rejected";
        } catch (const nemo::media::InputColorException& error) {
            EXPECT_NE(std::string(error.what()).find("primaries"), std::string::npos) << error.what();
        }
    }

    // 6. Raw/Data bypasses transfer and gamut conversion.
    {
        InputColorChoice choice;
        choice.mode = nemo::InputTransformMode::Raw;
        EncodedColorFacts facts;
        facts.formatName = "png";
        facts.declaredColorSpace = "srgb_rec709_scene";
        const ResolvedInputColor resolved = nemo::media::resolveInputColor(policy, choice, facts, tiff, "test");
        EXPECT_EQ(resolved.kind, InputTransformKind::Raw);
        EXPECT_EQ(resolved.origin, InputTransformOrigin::Raw);
    }

    // 7. The legacy policy (no config) keeps its strict ambiguity error, and
    // the config-backed policy with CLIP decode-only fields still rejects them.
    {
        InputColorChoice choice;
        EncodedColorFacts facts;
        facts.formatName = "tiff";
        try {
            static_cast<void>(nemo::media::resolveInputColor(SourceColorPolicy{}, choice, facts, tiff, "test"));
            ADD_FAILURE() << "expected an undeclared transfer to stay ambiguous without a project config";
        } catch (const nemo::media::InputColorException& error) {
            EXPECT_NE(std::string(error.what()).find("ambiguous"), std::string::npos) << error.what();
        }
        choice.hints["matrix"] = "bt709";
        try {
            static_cast<void>(nemo::media::resolveInputColor(policy, choice, facts, tiff, "test"));
            ADD_FAILURE() << "expected a Y'CbCr decode field on an RGB source to be rejected";
        } catch (const nemo::media::InputColorException& error) {
            EXPECT_NE(std::string(error.what()).find("not applicable to an RGB image source"), std::string::npos)
                << error.what();
        }
    }
}

// The encoded-domain alpha contract through the public still read: a
// premultiplied image is unassociated BEFORE the nonlinear conversion (so the
// result is not the wrong quantity), a zero-alpha pixel is a deterministic
// zero RGB, a straight image keeps its hidden RGB, and Raw/Data leaves both
// samples and association untouched.
TEST(InputColor, AlphaIsNormalizedInTheEncodedDomainAndRawStaysUntouched) {
    const auto configPath = writeColorConfig();
    const InputColorCache color(SourceColorPolicy{configPath.string(), "working_rec709"});

    // Auto association: EXR + declared sRGB means premultiplied, and the
    // declared transfer applies.
    {
        const auto path = writeDeclaredExr("premultiplied.exr", {0.25F, 0.0F, 0.375F, 0.5F}, "srgb_rec709_scene");
        InputColorChoice choice;
        const ImageFrame frame = nemo::media::readImageFrame(color, choice, path.string(), 0, "alpha test");
        EXPECT_EQ(frame.info.inputColor.alpha, ResolvedAlpha::Premultiplied);
        EXPECT_EQ(frame.info.transfer, ImageTransfer::Srgb);
        EXPECT_EQ(frame.info.color, ColorInterpretation::SceneLinear);
        const auto pixel = frame.image.pixel(0, 0);
        // unassociate 0.25/0.5 = 0.5, then sRGB-decode: the value the
        // "linearize first, divide later" order could never produce.
        EXPECT_NEAR(pixel[0], srgbToLinear(0.5), 1e-4F);
        EXPECT_NEAR(pixel[2], srgbToLinear(0.75), 1e-4F);
        EXPECT_FLOAT_EQ(pixel[3], 0.5F);
        EXPECT_NE(pixel[0], static_cast<float>(srgbToLinear(0.25) / 0.5));
    }

    // Zero alpha: deterministic zero RGB, never a division blow-up or a
    // hidden colour.
    {
        const auto path = writeDeclaredExr("zero-alpha.exr", {0.3F, 0.2F, 0.1F, 0.0F}, "srgb_rec709_scene");
        InputColorChoice choice;
        const ImageFrame frame = nemo::media::readImageFrame(color, choice, path.string(), 0, "zero alpha");
        const auto pixel = frame.image.pixel(0, 0);
        EXPECT_FLOAT_EQ(pixel[0], 0.0F);
        EXPECT_FLOAT_EQ(pixel[1], 0.0F);
        EXPECT_FLOAT_EQ(pixel[2], 0.0F);
        EXPECT_FLOAT_EQ(pixel[3], 0.0F);
    }

    // An explicit Straight choice keeps valid hidden RGB.
    {
        const auto path = writeDeclaredExr("straight.exr", {0.25F, 0.5F, 0.75F, 0.5F}, "srgb_rec709_scene");
        InputColorChoice choice;
        choice.alpha = nemo::AlphaMode::Straight;
        const ImageFrame frame = nemo::media::readImageFrame(color, choice, path.string(), 0, "straight alpha");
        EXPECT_EQ(frame.info.inputColor.alpha, ResolvedAlpha::Straight);
        const auto pixel = frame.image.pixel(0, 0);
        EXPECT_NEAR(pixel[0], srgbToLinear(0.25), 1e-4F);
        EXPECT_NEAR(pixel[1], srgbToLinear(0.5), 1e-4F);
        EXPECT_FLOAT_EQ(pixel[3], 0.5F);
    }

    // Raw/Data: the stored samples and the association are untouched, and the
    // frame is reported as Data — never as managed scene-linear.
    {
        const auto path = writeDeclaredExr("raw.exr", {0.25F, 0.5F, 0.75F, 0.5F}, "srgb_rec709_scene");
        InputColorChoice choice;
        choice.mode = nemo::InputTransformMode::Raw;
        const ImageFrame frame = nemo::media::readImageFrame(color, choice, path.string(), 0, "raw");
        EXPECT_EQ(frame.info.color, ColorInterpretation::Data);
        EXPECT_EQ(frame.image.layout().color, ColorInterpretation::Data);
        EXPECT_EQ(frame.info.inputColor.kind, InputTransformKind::Raw);
        const auto pixel = frame.image.pixel(0, 0);
        EXPECT_FLOAT_EQ(pixel[0], 0.25F);
        EXPECT_FLOAT_EQ(pixel[1], 0.5F);
        EXPECT_FLOAT_EQ(pixel[2], 0.75F);
        EXPECT_FLOAT_EQ(pixel[3], 0.5F);  // premultiplied association left alone
    }
}

// The public CPU provider seam: the resolved request's authored choices drive
// the pixels, transparent black is real source output, a policy error is
// refused, and two independent requests over the SAME file produce different
// pixels without touching the shared reference.
TEST(InputColor, ProviderAppliesTheResolvedRequestIndependently) {
    const auto configPath = writeColorConfig();
    const auto still = writeUndeclaredTiff("provider.tif", {0.5F, 0.5F, 0.5F, 1.0F});

    Document document;
    document.color.workingSpace = "working_rec709";
    SourceReference reference;
    reference.path = still.string();
    document.setSourceReference("media", reference);

    nemo::media::ImageSourceProvider provider(configPath.string());
    const EvaluationRequest request = rasterRequest(1, 1);
    EXPECT_FALSE(provider.colorConfigIdentity().empty());

    // Explicit named input space: the gamma-2.2 conversion of the encoded 0.5.
    EffectiveSourceRequest explicitRequest = sourceRequest("media", still.string());
    explicitRequest.inputTransform = nemo::InputTransformMode::Explicit;
    explicitRequest.inputColorSpace = "rec709_texture";
    const CpuImage converted = provider.frame(document, explicitRequest, request);
    EXPECT_EQ(converted.layout().color, ColorInterpretation::SceneLinear);
    EXPECT_NEAR(converted.pixel(0, 0)[0], std::pow(0.5, 2.2), kRgbAccuracyTolerance);

    // Auto with no declaration resolves through the config's TIFF rule: a
    // DIFFERENT space, so the same file yields different pixels while the
    // shared reference is never mutated.
    EffectiveSourceRequest autoRequest = sourceRequest("media", still.string());
    const CpuImage ruled = provider.frame(document, autoRequest, request);
    EXPECT_NEAR(ruled.pixel(0, 0)[0], std::pow(0.5, 2.2), kRgbAccuracyTolerance);
    EXPECT_EQ(reference.path, still.string());
    EXPECT_TRUE(reference.interpretation.empty());

    // Raw keeps the encoded sample, labelled Data.
    EffectiveSourceRequest rawRequest = sourceRequest("media", still.string());
    rawRequest.inputTransform = nemo::InputTransformMode::Raw;
    const CpuImage raw = provider.frame(document, rawRequest, request);
    EXPECT_EQ(raw.layout().color, ColorInterpretation::Data);
    EXPECT_FLOAT_EQ(raw.pixel(0, 0)[0], 0.5F);

    // Two independent explicit interpretations over one file differ.
    EffectiveSourceRequest other = sourceRequest("media", still.string());
    other.inputTransform = nemo::InputTransformMode::Explicit;
    other.inputColorSpace = "linear";
    const CpuImage alternative = provider.frame(document, other, request);
    EXPECT_FLOAT_EQ(alternative.pixel(0, 0)[0], 0.5F);
    EXPECT_NE(alternative.pixel(0, 0)[0], converted.pixel(0, 0)[0]);

    // Transparent black is the request's own raster, not a substituted frame.
    EffectiveSourceRequest blackRequest = sourceRequest("media", still.string());
    blackRequest.transparentBlack = true;
    const CpuImage black = provider.frame(document, blackRequest, rasterRequest(2, 2));
    EXPECT_EQ(black.width(), 2);
    EXPECT_EQ(black.height(), 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            for (const float channel : black.pixel(x, y)) {
                EXPECT_FLOAT_EQ(channel, 0.0F);
            }
        }
    }

    // A policy error never opens a frame.
    EffectiveSourceRequest failed = sourceRequest("media", still.string());
    failed.policyError = true;
    try {
        static_cast<void>(provider.frame(document, failed, request));
        ADD_FAILURE() << "expected a policy error to be refused";
    } catch (const nemo::media::ImageIoException& error) {
        EXPECT_NE(std::string(error.what()).find("error policy"), std::string::npos) << error.what();
    }

    // A missing named input space names the space when the conversion runs.
    EffectiveSourceRequest missing = sourceRequest("media", still.string());
    missing.inputTransform = nemo::InputTransformMode::Explicit;
    missing.inputColorSpace = "no-such-input";
    try {
        static_cast<void>(provider.frame(document, missing, request));
        ADD_FAILURE() << "expected the missing input space to be rejected";
    } catch (const nemo::media::ImageIoException& error) {
        EXPECT_NE(std::string(error.what()).find("no-such-input"), std::string::npos) << error.what();
    }
}

// A warm source result must NEVER hide a changed working target: the same
// request evaluated again after the document's working target becomes
// unsupported has to report that target, not serve the previously cached image.
// This is the real provider + reuse-cache seam (no call counts, no test-only
// API): the first evaluation decodes through ImageSourceProvider into a warm
// ResultCache, the second reuses it, and the third must fail naming the target.
TEST(InputColor, WarmSourceCacheReportsAChangedWorkingTarget) {
    const auto configPath = writeColorConfig();
    const auto still = writeUndeclaredTiff("warm-target.tif", {0.25F, 0.5F, 0.75F, 1.0F});

    Document document;
    document.color.workingSpace = "working_rec709";
    const NodeId plate = rootGraph(document).addNode("source", "plate");
    rootGraph(document).setParam(plate, "source", nemo::ParameterValue{std::string{"media"}});
    const NodeId output = rootGraph(document).addNode("output", "out");
    static_cast<void>(rootGraph(document).connect(PortRef{plate, 0}, PortRef{output, 0}));
    SourceReference reference;
    reference.path = still.string();
    document.setSourceReference("media", reference);

    nemo::media::ImageSourceProvider provider(configPath.string());
    ResultCache<CpuImage> cache;
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = output;
    request.region = {0, 0, 1, 1};

    // The produced source result is the TIFF file rule's gamma-2.2 conversion.
    const CpuEvaluation first = evaluateCpu(document, request, &cache, &provider);
    EXPECT_NEAR(first.image.pixel(0, 0)[0], std::pow(0.25, 2.2), kRgbAccuracyTolerance);
    // A second identical evaluation is the warm path; it must stay valid.
    const CpuEvaluation second = evaluateCpu(document, request, &cache, &provider);
    EXPECT_NEAR(second.image.pixel(0, 0)[2], std::pow(0.75, 2.2), kRgbAccuracyTolerance);

    // The same request with an unsupported working target: reported, never
    // served from the warm cache, and the message names the authored value.
    document.color.workingSpace = "aces2065";
    try {
        static_cast<void>(evaluateCpu(document, request, &cache, &provider));
        ADD_FAILURE() << "expected the changed unsupported working target to be reported";
    } catch (const std::exception& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("aces2065"), std::string::npos) << what;
        EXPECT_NE(what.find("scene-linear Rec.709"), std::string::npos) << what;
    }
}

// A configuration edited in place (same path, different content) must change
// the content identity AND the pixels on the next evaluation through the
// public provider seam once the explicit refresh boundary has run: a path-only
// cache can never serve the stale transform, and nothing polls the config.
TEST(InputColor, SamePathConfigEditRefreshesIdentityAndPixels) {
    const auto configPath = writeColorConfig();
    const auto still = writeUndeclaredTiff("reload.tif", {0.5F, 0.5F, 0.5F, 1.0F});

    Document document;
    document.color.workingSpace = "working_rec709";
    EvaluationRequest request = rasterRequest(1, 1);
    nemo::media::ImageSourceProvider provider(configPath.string());

    EffectiveSourceRequest source = sourceRequest("media", still.string());
    source.inputTransform = nemo::InputTransformMode::Explicit;
    source.inputColorSpace = "rec709_texture";

    const std::string beforeIdentity = std::string(provider.colorConfigIdentity());
    const CpuImage before = provider.frame(document, source, request);
    EXPECT_NEAR(before.pixel(0, 0)[0], std::pow(0.5, 2.2), kRgbAccuracyTolerance);

    // The engine does not poll the configuration: a same-path content change is
    // seen at the explicit refresh boundary (project replacement / deliberate
    // reload).
    rewriteConfigGamma(configPath, "2.8");
    provider.refreshColorConfig();

    const std::string afterIdentity = std::string(provider.colorConfigIdentity());
    EXPECT_FALSE(beforeIdentity.empty());
    EXPECT_NE(beforeIdentity, afterIdentity) << "a same-path config edit must change the content identity";
    const CpuImage after = provider.frame(document, source, request);
    EXPECT_NEAR(after.pixel(0, 0)[0], std::pow(0.5, 2.8), kRgbAccuracyTolerance);
    EXPECT_NE(before.pixel(0, 0)[0], after.pixel(0, 0)[0]);
}

// Native GPU parity: the retained OCIO pass produces the same working-space
// samples as the CPU reference for the pinned default conversion, with alpha
// untouched. Skips without a usable device.
TEST(InputColor, GpuInputTransformMatchesCpuReference) {
    const std::string pinned{nemo::kBuiltinColorConfigUri};
    const nemo::media::OcioGpuProgram program =
        nemo::media::buildInputTransformGpu(pinned, "Linear Rec.709 (sRGB)", "sRGB Encoded Rec.709 (sRGB)");
    EXPECT_NE(program.description.find("sRGB Encoded Rec.709 (sRGB)"), std::string::npos);

    CpuImage cpuImage = sample2x2();
    const nemo::media::OcioInputTransform transform(pinned, "Linear Rec.709 (sRGB)", "sRGB Encoded Rec.709 (sRGB)");
    transform.apply(cpuImage);

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const std::vector<float> gpuPixels = runGpuProgram(boot, program, sample2x2Flat());

    // Independent expectation as well as CPU parity: the known sRGB decode of
    // one sample, computed in this test.
    const auto known = cpuImage.pixel(1, 0);
    EXPECT_NEAR(known[0], srgbToLinear(0.25), 1e-4F);
    EXPECT_NEAR(gpuPixels[(static_cast<std::size_t>(0) * 2 + 1) * 4 + 0], srgbToLinear(0.25), 1e-4F);
    for (int y = 0; y < cpuImage.height(); ++y) {
        for (int x = 0; x < cpuImage.width(); ++x) {
            const auto expected = cpuImage.pixel(x, y);
            const float* actual = &gpuPixels[(static_cast<std::size_t>(y) * 2 + x) * 4];
            // Measured OCIO 2.5.2 CPU-vs-GPU divergence for this transform is
            // 2.408e-5 per channel (CPU fast-math pow vs generated GLSL); the
            // transform's own correctness is asserted above against the
            // published sRGB curve.
            EXPECT_NEAR(actual[0], expected[0], 1e-4F) << "pixel (" << x << "," << y << ") R";
            EXPECT_NEAR(actual[1], expected[1], 1e-4F) << "pixel (" << x << "," << y << ") G";
            EXPECT_NEAR(actual[2], expected[2], 1e-4F) << "pixel (" << x << "," << y << ") B";
            EXPECT_FLOAT_EQ(actual[3], expected[3]) << "pixel (" << x << "," << y << ") A";
        }
    }
    expectValidationClean(*boot.instance);
}

// Native GPU parity on the fixture config as well: a real nonlinear input
// space in a config-backed working space, with the expected value computed
// independently as a gamma power.
TEST(InputColor, GpuInputTransformMatchesCpuForAConfigBackedWorkingSpace) {
    const auto configPath = writeColorConfig();
    const nemo::media::OcioGpuProgram program =
        nemo::media::buildInputTransformGpu(configPath.string(), "working_rec709", "rec709_texture");

    CpuImage cpuImage = sample2x2();
    const nemo::media::OcioInputTransform transform(configPath.string(), "working_rec709", "rec709_texture");
    transform.apply(cpuImage);

    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_OR_FAIL(boot);
    const std::vector<float> gpuPixels = runGpuProgram(boot, program, sample2x2Flat());

    // Declared operation-specific bar for this input transform: measured
    // OCIO 2.5.2 CPU-vs-GPU divergence is 2.408e-5 (pinned Studio config) and
    // 2.229e-5 (this fixture) per channel, because the CPU processor
    // vectorizes/fast-maths the pow chain while the generated GLSL does not.
    // The bar carries headroom above the measured divergence; the
    // TRANSFORM's correctness is asserted separately against the published
    // curve (1e-4 on a value computed in this test), so this comparison only
    // has to catch the two backends disagreeing. Alpha is exact on both sides.
    constexpr float kNonlinearChainTolerance = 1e-4F;
    for (int y = 0; y < cpuImage.height(); ++y) {
        for (int x = 0; x < cpuImage.width(); ++x) {
            const auto expected = cpuImage.pixel(x, y);
            const float* actual = &gpuPixels[(static_cast<std::size_t>(y) * 2 + x) * 4];
            EXPECT_NEAR(actual[0], expected[0], kNonlinearChainTolerance) << "pixel (" << x << "," << y << ") R";
            EXPECT_NEAR(actual[1], expected[1], kNonlinearChainTolerance) << "pixel (" << x << "," << y << ") G";
            EXPECT_NEAR(actual[2], expected[2], kNonlinearChainTolerance) << "pixel (" << x << "," << y << ") B";
            EXPECT_FLOAT_EQ(actual[3], expected[3]) << "pixel (" << x << "," << y << ") A";
        }
    }
    expectValidationClean(*boot.instance);
}
