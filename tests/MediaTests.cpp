#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "nemo/media/ImageIO.hpp"
#include "nemo/media/Probe.hpp"

using namespace nemo;
using namespace nemo::media;

namespace {

std::filesystem::path tempDir() {
    const auto dir = std::filesystem::temp_directory_path() / "nemo-media-tests";
    std::filesystem::create_directories(dir);
    return dir;
}

// Tiny RGBA image generated in-test (issue #4): gradients plus a cycling
// alpha ramp. Values chosen exactly representable in half so identity round
// trips are bit-exact.
CpuImage makeTestImage(int width, int height) {
    CpuImage image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float r = static_cast<float>(x % 4) * 0.25F;  // exact in half
            const float g = static_cast<float>(y % 4) * 0.25F;  // exact in half
            // Quarter steps (0, 0.25, 0.5, 1) are exact in half.
            const float a = (x + y) % 4 == 0 ? 0.0F : (x + y) % 4 == 1 ? 0.25F : (x + y) % 4 == 2 ? 0.5F : 1.0F;
            image.setPixel(x, y, {r, g, 0.5F, a});
        }
    }
    return image;
}

}  // namespace

// Acceptance example 1: half-float RGBA EXR written then read yields
// identical dimensions, channel names, and pixel values within
// half-precision identity.
TEST(MediaTest, HalfFloatExrRoundTripIsIdentity) {
    const auto path = tempDir() / "roundtrip.exr";
    const CpuImage written = makeTestImage(6, 4);
    writeImage(path.string(), written, OutputPrecision::Half);

    const ImageReadResult read = readImage(path.string());

    EXPECT_EQ(read.image.width(), 6);
    EXPECT_EQ(read.image.height(), 4);
    EXPECT_EQ(read.image.layout(), written.layout());
    EXPECT_EQ(read.image.layout().precision, Precision::Float32);
    EXPECT_EQ(read.image.layout().color, ColorInterpretation::SceneLinear);
    EXPECT_EQ(read.nativePrecision, "half");
    EXPECT_EQ(read.formatName, "openexr");
    EXPECT_EQ(read.channelNames, (std::vector<std::string>{"R", "G", "B", "A"}));
    EXPECT_EQ(read.alpha, AlphaAssociation::Premultiplied);
    EXPECT_EQ(read.dataWindow, (PixelWindow{0, 0, 5, 3}));
    EXPECT_EQ(read.displayWindow, (PixelWindow{0, 0, 5, 3}));
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) {
            EXPECT_EQ(read.image.pixel(x, y), written.pixel(x, y)) << "at (" << x << ", " << y << ")";
        }
    }
    std::filesystem::remove(path);
}

// Float storage also round trips, and the reported native precision names it.
TEST(MediaTest, FloatExrRoundTrip) {
    const auto path = tempDir() / "roundtrip-f32.exr";
    const CpuImage written = makeTestImage(3, 2);
    writeImage(path.string(), written, OutputPrecision::Float32);

    const ImageReadResult read = readImage(path.string());
    EXPECT_EQ(read.nativePrecision, "float");
    EXPECT_EQ(read.image.layout(), written.layout());
    std::filesystem::remove(path);
}

// EXR display/data windows: the adapter reports both and lands the data at
// its display offset; untouched display area stays black/opaque.
TEST(MediaTest, DisplayAndDataWindows) {
    const auto path = tempDir() / "windowed.exr";
    auto output = OIIO::ImageOutput::create(path.string());
    ASSERT_NE(output, nullptr);
    OIIO::ImageSpec spec(4, 2, 4, OIIO::TypeDesc::HALF);
    spec.channelnames = {"R", "G", "B", "A"};
    spec.x = 2;  // data window at display offset (2, 1)
    spec.y = 1;
    spec.full_x = 0;  // display window covering the full 8x6 extent
    spec.full_y = 0;
    spec.full_width = 8;
    spec.full_height = 6;
    ASSERT_TRUE(output->open(path.string(), spec));
    const std::vector<float> pixels(4 * 2 * 4, 0.25F);
    ASSERT_TRUE(output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()));
    ASSERT_TRUE(output->close());
    output.reset();

    const ImageReadResult read = readImage(path.string());
    EXPECT_EQ(read.dataWindow, (PixelWindow{2, 1, 5, 2}));
    EXPECT_EQ(read.displayWindow, (PixelWindow{0, 0, 7, 5}));
    EXPECT_EQ(read.image.width(), 8);
    EXPECT_EQ(read.image.height(), 6);
    EXPECT_EQ(read.image.pixel(2, 1), (std::array<float, 4>{0.25F, 0.25F, 0.25F, 0.25F}));
    EXPECT_EQ(read.image.pixel(5, 2), (std::array<float, 4>{0.25F, 0.25F, 0.25F, 0.25F}));
    EXPECT_EQ(read.image.pixel(0, 0), (std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F}));
    EXPECT_EQ(read.image.pixel(7, 5), (std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F}));
    std::filesystem::remove(path);
}

// Pixel aspect travels through the EXR header into the layout.
TEST(MediaTest, PixelAspectFromHeader) {
    const auto path = tempDir() / "aspect.exr";
    auto output = OIIO::ImageOutput::create(path.string());
    ASSERT_NE(output, nullptr);
    OIIO::ImageSpec spec(2, 2, 4, OIIO::TypeDesc::HALF);
    spec.channelnames = {"R", "G", "B", "A"};
    spec.attribute("pixelaspectratio", 2.0F);
    ASSERT_TRUE(output->open(path.string(), spec));
    const std::vector<float> pixels(2 * 2 * 4, 0.5F);
    ASSERT_TRUE(output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()));
    ASSERT_TRUE(output->close());
    output.reset();

    const ImageReadResult read = readImage(path.string());
    EXPECT_FLOAT_EQ(read.image.layout().pixelAspect, 2.0F);
    std::filesystem::remove(path);
}

// Sequence pattern expansion, OpenImageIO style.
TEST(MediaTest, ResolveFramePathPatterns) {
    EXPECT_EQ(resolveFramePath("shot.####.exr", 12), "shot.0012.exr");
    EXPECT_EQ(resolveFramePath("shot.####.exr", 123456), "shot.123456.exr");
    EXPECT_EQ(resolveFramePath("shot.@.exr", 7), "shot.7.exr");
    EXPECT_EQ(resolveFramePath("still.png", 42), "still.png");
}

// A written sequence pattern reads back per-frame through the adapter.
TEST(MediaTest, SequenceRoundTrip) {
    const auto pattern = (tempDir() / "seq.####.exr").string();
    CpuImage frame0 = makeTestImage(2, 2);
    CpuImage frame1 = makeTestImage(2, 2);
    frame1.setPixel(0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
    writeImage(resolveFramePath(pattern, 0), frame0, OutputPrecision::Half);
    writeImage(resolveFramePath(pattern, 1), frame1, OutputPrecision::Half);

    EXPECT_EQ(readImage(resolveFramePath(pattern, 0)).image.pixel(0, 0), frame0.pixel(0, 0));
    EXPECT_EQ(readImage(resolveFramePath(pattern, 1)).image.pixel(0, 0), frame1.pixel(0, 0));
}

// Reading a nonexistent path produces an error naming the missing file
// (acceptance example 2; surfaced machine-readable via `nemo-cli imageinfo`).
TEST(MediaTest, MissingFileErrorNamesPath) {
    const auto path = tempDir() / "does-not-exist-4217.exr";
    try {
        static_cast<void>(readImage(path.string()));
        FAIL() << "expected ImageIoException";
    } catch (const ImageIoException& e) {
        EXPECT_EQ(e.path, path.string());
        EXPECT_NE(std::string(e.what()).find(path.string()), std::string::npos);
    }
}

// Capability probe (issue #10 acceptance example 1/2): every reported
// capability carries evidence; an unavailable capability names a clear
// reason; no claim is "supported" without init verification. The probe is
// self-consistent on any machine — hardware presence is measured, and
// devices without video queues must never claim Vulkan decode.
TEST(MediaTest, ProbeReportsEvidenceAndReasons) {
    const MediaCapabilities capabilities = probeMediaCapabilities(nullptr);

    ASSERT_FALSE(capabilities.decoders.empty());
    for (const MediaCapability& decoder : capabilities.decoders) {
        if (decoder.evidence == CapabilityEvidence::Unavailable) {
            EXPECT_FALSE(decoder.reason.empty()) << decoder.codec;
        } else {
            EXPECT_TRUE(decoder.reason.empty()) << decoder.codec << ": " << decoder.reason;
        }
        if (decoder.codec == "h264-vulkan") {
            // Vulkan decode needs the reserved video decode queue; without
            // a device the probe can never init-verify it.
            EXPECT_EQ(decoder.evidence, CapabilityEvidence::RegisteredOnly) << decoder.reason;
        }
    }
    for (const MediaCapability& encoder : capabilities.encoders) {
        if (encoder.evidence == CapabilityEvidence::Unavailable) {
            EXPECT_FALSE(encoder.reason.empty()) << encoder.codec;
        }
    }
    EXPECT_FALSE(capabilities.vulkanVideoDecodeQueues);
}
