#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
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

// ---------------------------------------------------------------------------
// Still/sequence source adapter fixtures (issue #62).
// ---------------------------------------------------------------------------

Graph& rootGraph(Document& document) {
    return document.network(document.rootNetworkId()).graph();
}

// Graph: source node (`nodeName`, resolving source key `key`) -> output.
Document stillSourceGraph(const SourceReference& reference, const std::string& key, const std::string& nodeName) {
    Document document;
    rootGraph(document).removeNode(rootGraph(document).nodeByName("Output")->id);
    document.sources[key] = reference;
    const NodeId node = rootGraph(document).addNode("source", nodeName);
    rootGraph(document).setParam(node, "source", key);
    const NodeId output = rootGraph(document).addNode("output", "out");
    static_cast<void>(rootGraph(document).connect(PortRef{node, 0}, PortRef{output, 0}));
    return document;
}

EvaluationRequest rasterRequest(const Document& document, const int width, const int height,
                                const std::int64_t localTime = 0) {
    EvaluationRequest request;
    request.network = document.rootNetworkId();
    request.output = resolveOutput(document, request.network);
    request.localTime = localTime;
    request.region = {0, 0, width, height};
    return request;
}

// Opaque test frame written with known, exactly representable values.
CpuImage knownFrame(const std::array<float, 3>& rgb) {
    CpuImage image(2, 2);
    image.setPixel(0, 0, {rgb[0], rgb[1], rgb[2], 1.0F});
    image.setPixel(1, 0, {rgb[2], rgb[0], rgb[1], 1.0F});
    image.setPixel(0, 1, {rgb[1], rgb[2], rgb[0], 1.0F});
    image.setPixel(1, 1, {0.0F, 0.0F, 0.0F, 1.0F});
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
            // a device the probe can never verify beyond registration.
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

// ---------------------------------------------------------------------------
// Still images and image sequences through the shared source-fill path
// (issue #62): a real-media SourceReference resolves through the evaluator's
// plan with an ImageSourceProvider, and the interpretation is explicit.
// ---------------------------------------------------------------------------

// (i) A real EXR still through evaluateCpu yields the written pixels.
TEST(MediaTest, ExrStillEvaluatesThroughSourceFill) {
    const auto path = tempDir() / "still-eval.exr";
    const CpuImage written = knownFrame({0.25F, 0.5F, 0.75F});
    writeImage(path.string(), written, OutputPrecision::Float32);

    SourceReference reference;
    reference.path = path.string();
    Document document = stillSourceGraph(reference, "plate", "plate");
    ImageSourceProvider sources;
    const CpuEvaluation evaluation = evaluateCpu(document, rasterRequest(document, 2, 2), nullptr, &sources);

    ASSERT_EQ(evaluation.image.width(), 2);
    ASSERT_EQ(evaluation.image.height(), 2);
    EXPECT_EQ(evaluation.image.layout().color, ColorInterpretation::SceneLinear);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            EXPECT_EQ(evaluation.image.pixel(x, y), written.pixel(x, y)) << "at (" << x << ", " << y << ")";
        }
    }
    std::filesystem::remove(path);
}

// (ii) An EXR sequence written as seq.####.exr serves frame 0 and frame 1
// through the reference's frameAt mapping (frameOffset/frameStep).
TEST(MediaTest, ExrSequenceResolvesFramesThroughTimeMapping) {
    const auto pattern = (tempDir() / "eval-seq.####.exr").string();
    const CpuImage frame0 = knownFrame({0.125F, 0.0F, 0.0F});
    const CpuImage frame1 = knownFrame({0.0F, 0.625F, 0.0F});
    writeImage(resolveFramePath(pattern, 0), frame0, OutputPrecision::Float32);
    writeImage(resolveFramePath(pattern, 1), frame1, OutputPrecision::Float32);

    SourceReference reference;
    reference.path = pattern;
    reference.frameOffset = 0;
    reference.frameStep = 1;
    Document document = stillSourceGraph(reference, "plate", "plate");
    ImageSourceProvider sources;
    const CpuEvaluation first = evaluateCpu(document, rasterRequest(document, 2, 2, 0), nullptr, &sources);
    const CpuEvaluation second = evaluateCpu(document, rasterRequest(document, 2, 2, 1), nullptr, &sources);

    EXPECT_EQ(first.image.pixel(0, 0), frame0.pixel(0, 0));
    EXPECT_EQ(second.image.pixel(0, 0), frame1.pixel(0, 0));
    EXPECT_NE(first.image.pixel(0, 0), second.image.pixel(0, 0));
}

// (iii) Negative and overflowing source times and a missing sequence frame
// are explicit errors, never clamped to a neighboring frame.
TEST(MediaTest, SourceTimeMappingAndMissingFramesFailExplicitly) {
    const auto pattern = (tempDir() / "eval-errors.####.exr").string();
    writeImage(resolveFramePath(pattern, 0), knownFrame({0.5F, 0.5F, 0.5F}), OutputPrecision::Float32);

    SourceReference reference;
    reference.path = pattern;
    reference.frameOffset = 0;
    reference.frameStep = 1;
    Document document = stillSourceGraph(reference, "plate", "plate");
    ImageSourceProvider sources;

    try {
        static_cast<void>(evaluateCpu(document, rasterRequest(document, 2, 2, -1), nullptr, &sources));
        FAIL() << "expected a negative source time to fail";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("negative"), std::string::npos) << error.what();
    }

    SourceReference overflowing = reference;
    overflowing.frameStep = std::numeric_limits<std::int64_t>::max();
    Document overflowDocument = stillSourceGraph(overflowing, "plate", "plate");
    try {
        static_cast<void>(evaluateCpu(overflowDocument, rasterRequest(overflowDocument, 2, 2, 2), nullptr, &sources));
        FAIL() << "expected an overflowing source time to fail";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("overflow"), std::string::npos) << error.what();
    }

    try {
        static_cast<void>(evaluateCpu(document, rasterRequest(document, 2, 2, 5), nullptr, &sources));
        FAIL() << "expected a missing sequence frame to fail";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find(resolveFramePath(pattern, 5)), std::string::npos) << error.what();
    }
}

// (iv) A TIFF with no color metadata has no declared transfer and no format
// default: rejected as ambiguous, naming the file and format.
TEST(MediaTest, UndeclaredTiffColorMetadataIsRejectedAsAmbiguous) {
    const auto path = tempDir() / "no-colorspace.tif";
    writeImage(path.string(), knownFrame({0.5F, 0.25F, 0.125F}), OutputPrecision::Float32);

    SourceReference reference;
    reference.path = path.string();
    try {
        static_cast<void>(readImageFrame(reference, 0, "undeclared tiff"));
        FAIL() << "expected the ambiguous TIFF to be rejected";
    } catch (const ImageIoException& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find(path.string()), std::string::npos) << what;
        EXPECT_NE(what.find("tiff"), std::string::npos) << what;
        EXPECT_NE(what.find("ambiguous"), std::string::npos) << what;
    }
    std::filesystem::remove(path);
}

// (v) A PNG's declared sRGB transfer is inverted on read, checked against
// the published sRGB EOTF computed here (not against the adapter).
TEST(MediaTest, SrgbPngIsLinearizedThroughTheDeclaredTransfer) {
    const auto path = tempDir() / "srgb-eval.png";
    {
        auto output = OIIO::ImageOutput::create(path.string());
        ASSERT_NE(output, nullptr);
        OIIO::ImageSpec spec(2, 1, 3, OIIO::TypeDesc::UINT8);
        spec.channelnames = {"R", "G", "B"};
        ASSERT_TRUE(output->open(path.string(), spec));
        const std::vector<unsigned char> pixels{10, 64, 188, 255, 128, 0};
        ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()));
        ASSERT_TRUE(output->close());
    }

    SourceReference reference;
    reference.path = path.string();
    const ImageFrame frame = readImageFrame(reference, 0, "srgb png");
    ASSERT_EQ(frame.info.transfer, ImageTransfer::Srgb);
    ASSERT_EQ(frame.image.width(), 2);

    const auto srgbToLinear = [](const double value) {
        return value < 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    // Encoded values below and above the sRGB linear-segment knee.
    EXPECT_NEAR(frame.image.pixel(0, 0)[0], srgbToLinear(10.0 / 255.0), 1e-6);
    EXPECT_NEAR(frame.image.pixel(0, 0)[1], srgbToLinear(64.0 / 255.0), 1e-6);
    EXPECT_NEAR(frame.image.pixel(0, 0)[2], srgbToLinear(188.0 / 255.0), 1e-6);
    EXPECT_NEAR(frame.image.pixel(1, 0)[0], srgbToLinear(255.0 / 255.0), 1e-6);
    EXPECT_NEAR(frame.image.pixel(1, 0)[1], srgbToLinear(128.0 / 255.0), 1e-6);
    EXPECT_FLOAT_EQ(frame.image.pixel(1, 0)[3], 1.0F);  // no alpha channel -> opaque
    std::filesystem::remove(path);
}

// (vi) A single-channel (grayscale) source cannot satisfy the RGB image
// contract: rejected naming the file, format, and reason.
TEST(MediaTest, SingleChannelSourceIsRejected) {
    const auto path = tempDir() / "gray.png";
    {
        auto output = OIIO::ImageOutput::create(path.string());
        ASSERT_NE(output, nullptr);
        OIIO::ImageSpec spec(2, 2, 1, OIIO::TypeDesc::UINT8);
        spec.channelnames = {"Y"};
        ASSERT_TRUE(output->open(path.string(), spec));
        const std::vector<unsigned char> pixels{0, 64, 128, 255};
        ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()));
        ASSERT_TRUE(output->close());
    }

    SourceReference reference;
    reference.path = path.string();
    try {
        static_cast<void>(readImageFrame(reference, 0, "grayscale source"));
        FAIL() << "expected the grayscale source to be rejected";
    } catch (const ImageIoException& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find(path.string()), std::string::npos) << what;
        EXPECT_NE(what.find("png"), std::string::npos) << what;
        EXPECT_NE(what.find("R/G/B"), std::string::npos) << what;
    }
    std::filesystem::remove(path);
}

// EXR format default boundary: an EXR with no declared color space must
// declare Rec.709 chromaticities (or none) to match the scene-linear
// Rec.709 working space; a different primary set is rejected, never
// relabeled.
TEST(MediaTest, ExrChromaticitiesMustMatchTheWorkingSpace) {
    const auto write = [&](const std::filesystem::path& path, const std::array<float, 8>& chromaticities) {
        auto output = OIIO::ImageOutput::create(path.string());
        EXPECT_NE(output, nullptr);
        OIIO::ImageSpec spec(2, 2, 4, OIIO::TypeDesc::FLOAT);
        spec.channelnames = {"R", "G", "B", "A"};
        spec.attribute("chromaticities", OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8), chromaticities.data());
        EXPECT_TRUE(output->open(path.string(), spec));
        const std::vector<float> pixels(2 * 2 * 4, 1.0F);
        EXPECT_TRUE(output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()));
        EXPECT_TRUE(output->close());
    };

    // DCI-P3 primaries: outside the Rec.709 working space.
    const auto p3Path = tempDir() / "p3-chroma.exr";
    write(p3Path, {0.68F, 0.32F, 0.265F, 0.69F, 0.15F, 0.06F, 0.3127F, 0.3290F});
    SourceReference p3Reference;
    p3Reference.path = p3Path.string();
    try {
        static_cast<void>(readImageFrame(p3Reference, 0, "p3 exr"));
        FAIL() << "expected non-Rec.709 chromaticities to be rejected";
    } catch (const ImageIoException& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find(p3Path.string()), std::string::npos) << what;
        EXPECT_NE(what.find("Rec.709"), std::string::npos) << what;
    }
    std::filesystem::remove(p3Path);

    // Explicit Rec.709 chromaticities are the working space itself: accepted.
    const auto rec709Path = tempDir() / "rec709-chroma.exr";
    write(rec709Path, {0.64F, 0.33F, 0.30F, 0.60F, 0.15F, 0.06F, 0.3127F, 0.3290F});
    SourceReference rec709Reference;
    rec709Reference.path = rec709Path.string();
    const ImageFrame frame = readImageFrame(rec709Reference, 0, "rec709 exr");
    EXPECT_EQ(frame.info.transfer, ImageTransfer::Linear);
    EXPECT_EQ(frame.info.primaries, ImagePrimaries::Rec709);
    std::filesystem::remove(rec709Path);
}

// Reference interpretation fills only fields the file left unspecified, and
// fields an RGB image source cannot honor are rejected rather than ignored.
TEST(MediaTest, ReferenceInterpretationFillsOnlyUnspecifiedFields) {
    const auto exrPath = tempDir() / "override-transfer.exr";
    CpuImage written(1, 1);
    written.setPixel(0, 0, {0.5F, 0.25F, 0.75F, 1.0F});
    writeImage(exrPath.string(), written, OutputPrecision::Float32);

    SourceReference reference;
    reference.path = exrPath.string();
    reference.interpretation["transfer"] = "gamma22";
    const ImageFrame overridden = readImageFrame(reference, 0, "override exr");
    EXPECT_EQ(overridden.info.transfer, ImageTransfer::Gamma22);
    EXPECT_NEAR(overridden.image.pixel(0, 0)[0], std::pow(0.5, 2.2), 1e-6);
    EXPECT_NEAR(overridden.image.pixel(0, 0)[1], std::pow(0.25, 2.2), 1e-6);

    reference.interpretation["transfer"] = "gamma28";
    const ImageFrame gamma28 = readImageFrame(reference, 0, "override exr");
    EXPECT_EQ(gamma28.info.transfer, ImageTransfer::Gamma28);
    EXPECT_NEAR(gamma28.image.pixel(0, 0)[2], std::pow(0.75, 2.8), 1e-6);

    reference.interpretation["transfer"] = "bt709";
    const ImageFrame bt709 = readImageFrame(reference, 0, "override exr");
    EXPECT_EQ(bt709.info.transfer, ImageTransfer::Bt709);
    const auto bt709ToLinear = [](const double value) {
        return value < 0.081 ? value / 4.5 : std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
    };
    EXPECT_NEAR(bt709.image.pixel(0, 0)[0], bt709ToLinear(0.5), 1e-6);
    EXPECT_NEAR(bt709.image.pixel(0, 0)[1], bt709ToLinear(0.25), 1e-6);
    std::filesystem::remove(exrPath);

    // A declared color space wins over an override.
    const auto pngPath = tempDir() / "declared-wins.png";
    {
        auto output = OIIO::ImageOutput::create(pngPath.string());
        ASSERT_NE(output, nullptr);
        OIIO::ImageSpec spec(1, 1, 3, OIIO::TypeDesc::UINT8);
        spec.channelnames = {"R", "G", "B"};
        ASSERT_TRUE(output->open(pngPath.string(), spec));
        const std::vector<unsigned char> pixel{188, 64, 10};
        ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixel.data()));
        ASSERT_TRUE(output->close());
    }
    SourceReference declared;
    declared.path = pngPath.string();
    declared.interpretation["transfer"] = "gamma22";
    const ImageFrame winning = readImageFrame(declared, 0, "declared wins");
    EXPECT_EQ(winning.info.transfer, ImageTransfer::Srgb);
    std::filesystem::remove(pngPath);

    // Y'CbCr-only fields are not applicable to an RGB image source.
    const auto matrixPath = tempDir() / "matrix-field.exr";
    writeImage(matrixPath.string(), written, OutputPrecision::Float32);
    SourceReference streamOnly;
    streamOnly.path = matrixPath.string();
    streamOnly.interpretation["matrix"] = "bt709";
    try {
        static_cast<void>(readImageFrame(streamOnly, 0, "matrix field"));
        FAIL() << "expected a matrix field to be rejected";
    } catch (const ImageIoException& error) {
        EXPECT_NE(std::string(error.what()).find("not applicable to an RGB image source"), std::string::npos)
            << error.what();
    }
    std::filesystem::remove(matrixPath);
}

// Probing reports header facts without decoding pixels and resolves the
// sequence frame from the reference's time mapping (frameOffset).
TEST(MediaTest, ProbeReportsHeaderFactsWithoutDecoding) {
    const auto still = tempDir() / "probe-still.exr";
    writeImage(still.string(), knownFrame({0.25F, 0.5F, 0.75F}), OutputPrecision::Float32);
    SourceReference stillReference;
    stillReference.path = still.string();
    const ImageFrameInfo info = probeImageFrame(stillReference, "probe still");
    EXPECT_EQ(info.path, still.string());
    EXPECT_EQ(info.formatName, "openexr");
    EXPECT_EQ(info.width, 2);
    EXPECT_EQ(info.height, 2);
    EXPECT_FALSE(info.sequence);
    EXPECT_EQ(info.transfer, ImageTransfer::Linear);
    EXPECT_EQ(info.primaries, ImagePrimaries::Rec709);
    std::filesystem::remove(still);

    const auto pattern = (tempDir() / "probe-seq.####.exr").string();
    writeImage(resolveFramePath(pattern, 3), knownFrame({0.25F, 0.5F, 0.75F}), OutputPrecision::Float32);
    SourceReference sequenceReference;
    sequenceReference.path = pattern;
    sequenceReference.frameOffset = 3;
    const ImageFrameInfo sequence = probeImageFrame(sequenceReference, "probe sequence");
    EXPECT_EQ(sequence.path, resolveFramePath(pattern, 3));
    EXPECT_TRUE(sequence.sequence);
    EXPECT_EQ(sequence.formatName, "openexr");
}

// (vii) A non-image path is not decodable by the headless CPU reference:
// the provider fails explicitly instead of producing anything.
TEST(MediaTest, ProviderRejectsNonImagePathExplicitly) {
    const auto path = tempDir() / "not-media.txt";
    {
        std::ofstream out(path);
        ASSERT_TRUE(out.is_open());
        out << "this is not image data\n";
    }
    EXPECT_FALSE(isImagePath(path.string()));

    Document document;
    SourceReference reference;
    reference.path = path.string();
    EvaluationRequest request;
    request.region = {0, 0, 2, 2};
    ImageSourceProvider provider;
    try {
        static_cast<void>(provider.frame(document, reference, 0, request));
        FAIL() << "expected the non-image path to be rejected";
    } catch (const ImageIoException& error) {
        // The image adapter reports its own reason and names the path; the
        // provider must not mask it with a generic "not image data" message.
        const std::string what = error.what();
        EXPECT_NE(what.find(path.string()), std::string::npos) << what;
        EXPECT_EQ(what.rfind("image: ", 0), 0u) << what;
    }
    std::filesystem::remove(path);
}

// A document working space outside the declared scene-linear Rec.709
// default is rejected for source interpretation, never silently assumed.
TEST(MediaTest, ProviderRejectsUnsupportedDocumentWorkingSpace) {
    const auto path = tempDir() / "working-space.exr";
    writeImage(path.string(), knownFrame({0.5F, 0.5F, 0.5F}), OutputPrecision::Float32);

    Document document;
    document.color.workingSpace = "aces2065";
    SourceReference reference;
    reference.path = path.string();
    EvaluationRequest request;
    request.region = {0, 0, 2, 2};
    ImageSourceProvider provider;
    try {
        static_cast<void>(provider.frame(document, reference, 0, request));
        FAIL() << "expected the unsupported working space to be rejected";
    } catch (const ImageIoException& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("aces2065"), std::string::npos) << what;
        EXPECT_NE(what.find("scene-linear Rec.709"), std::string::npos) << what;
    }
    std::filesystem::remove(path);
}
