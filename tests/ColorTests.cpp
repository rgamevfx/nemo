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

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Serialization.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include "nemo/media/ViewingTransform.hpp"

using namespace nemo;

namespace {

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
    config += "roles:\n  default: linear\n  scene_linear: linear\n";
    config += "colorspaces:\n";
    config += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
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
    std::optional<gpu::GpuViewedImage> viewed = transform.submit(source);
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
    const NodeId plate = doc.graph.addNode("testpattern", "plate");
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
    const NodeId plate = doc.graph.addNode("testpattern", "plate");
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
    const NodeId plate = doc.graph.addNode("testpattern", "plate");
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
    const NodeId plate = doc.graph.addNode("testpattern", "plate");
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
        ::unsetenv("OCIO");
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

TEST(Color, GpuMatrixViewMatchesCpuReferenceWithinTolerance) {
    // Pure-matrix control path: no LUTs, no textures, no uniforms — both
    // sides evaluate the identical formula in float32.
    const auto configPath = writeColorConfig();
    const media::OcioGpuProgram program = media::buildViewingTransformGpu(configPath.string(), "linear", "sRGB/matrix");
    EXPECT_TRUE(program.textures.empty());
    EXPECT_TRUE(program.uniformBytes.empty());

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
