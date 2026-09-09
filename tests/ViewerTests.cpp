// End-to-end native viewer tests (issue #11, spec sections 8/10.4/11).
//
// Real tagged media through the SAME shared dependency plan synthetic
// fixtures use: decoded source → native GPU effects → GPU OCIO viewing
// transform → presentation quantization. The gray oracle is independent:
// the decoded mid-gray value (0.261769 for limited-range 126 through the
// published BT.709 inverse) is pinned by MediaDecodeTests against the
// published formula — nothing here derives it from Nemo's conversion
// helpers. Runtime decode state lives in eval::SourceSession; the
// Document holds only plain-data SourceReferences (acceptance example 6),
// and every plan step records the resolved source/frame evidence.
//
// Readback policy: every test performs at most ONE declared diagnostic
// readback; the production/worker path never reads back. Zero
// validation-layer warnings is the bar (as in GpuTests/GpuEffectTests).
//
// Tests guard themselves: without a usable Vulkan device they skip; when
// compiled Slang kernels are absent (CPU-only configure) the Slang-path
// tests skip with the enabling instructions.

#include "ScopedEnvironment.hpp"
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/eval/Viewer.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/gpu/Submit.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"
#include "nemo/media/VideoDecode.hpp"
#include "nemo/media/ViewingTransform.hpp"

using namespace nemo;

namespace {

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

enum class BootstrapOutcome { Created, NoDevice, Failed };
struct Bootstrap {
    std::unique_ptr<gpu::Instance> instance;
    std::unique_ptr<gpu::Device> device;
    std::unique_ptr<gpu::Allocator> allocator;
    BootstrapOutcome outcome = BootstrapOutcome::Created;
    std::string message;
};

[[nodiscard]] Bootstrap createBootstrap(const gpu::DeviceConfig& config = {}) {
    Bootstrap boot;
    try {
        boot.instance = gpu::Instance::create({.validation = true});
        boot.device = gpu::Device::create(*boot.instance, config);
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

#if defined(NEMO_SLANG_SPV_DIR)
[[nodiscard]] std::filesystem::path slangSpvDir() {
    return NEMO_SLANG_SPV_DIR;
}
#else
[[nodiscard]] std::filesystem::path slangSpvDir() {
    return {};
}
#endif

#define NEMO_SKIP_UNLESS_SLANG(boot)                                                                                   \
    do {                                                                                                               \
        NEMO_SKIP_OR_FAIL(boot);                                                                                       \
        if (slangSpvDir().empty()) {                                                                                   \
            GTEST_SKIP() << "no compiled Slang kernels (CPU-only configuration): configure with "                      \
                            "-D NEMO_DOWNLOAD_SLANGC=ON or -D NEMO_SLANGC=<path>";                                     \
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

// A real tagged FFV1/MKV clip: 64x48, constant chroma (cb/cr), one luma
// value per frame. FFV1 preserves the authored integer samples, so the
// decode interpretation below is checked against the PUBLISHED BT.709
// inverse OETF — never against Nemo's encoder or conversion helpers.
class TaggedClip {
public:
    explicit TaggedClip(AVPixelFormat pixels, std::vector<int> yValues, int cb = 128, int cr = 128,
                        AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709, bool tagged = true,
                        bool horizontalRamp = false) {
        path = std::filesystem::temp_directory_path() /
               (std::string("nemo-viewer-") + ::testing::UnitTest::GetInstance()->current_test_info()->name() + "-" +
                av_get_pix_fmt_name(pixels) + "-" + std::to_string(tagged ? static_cast<int>(transfer) : -1) + "-" +
                std::to_string(::getpid()) + "-" + std::to_string(yValues.front()) + ".mkv");
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        if (!encoder)
            throw std::runtime_error("FFV1 encoder unavailable");
        codec.reset(avcodec_alloc_context3(encoder));
        if (!codec)
            throw std::bad_alloc();
        codec->width = 64;
        codec->height = 48;
        codec->pix_fmt = pixels;
        codec->time_base = {1, 24};
        if (horizontalRamp)
            codec->sample_aspect_ratio = {2, 1};
        if (tagged) {
            codec->color_primaries = AVCOL_PRI_BT709;
            codec->color_trc = transfer;
            codec->colorspace = AVCOL_SPC_BT709;
            codec->color_range = AVCOL_RANGE_MPEG;
        } else {
            codec->color_primaries = AVCOL_PRI_UNSPECIFIED;
            codec->color_trc = AVCOL_TRC_UNSPECIFIED;
            codec->colorspace = AVCOL_SPC_UNSPECIFIED;
            codec->color_range = AVCOL_RANGE_UNSPECIFIED;
        }
        codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
        check(avcodec_open2(codec.get(), encoder, nullptr));
        check(avformat_alloc_output_context2(&output.context, nullptr, "matroska", path.c_str()));
        AVFormatContext* format = output.context;
        if (!format)
            throw std::bad_alloc();
        AVStream* stream = avformat_new_stream(format, nullptr);
        if (!stream)
            throw std::bad_alloc();
        check(avcodec_parameters_from_context(stream->codecpar, codec.get()));
        stream->time_base = codec->time_base;
        stream->sample_aspect_ratio = codec->sample_aspect_ratio;
        check(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE));
        check(avformat_write_header(format, nullptr));
        frame.reset(av_frame_alloc());
        packet.reset(av_packet_alloc());
        if (!frame || !packet)
            throw std::bad_alloc();
        frame->format = pixels;
        frame->width = 64;
        frame->height = 48;
        check(av_frame_get_buffer(frame.get(), 0));
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixels);
        const int values[] = {0, cb, cr};
        const auto drain = [&] {
            int status;
            while ((status = avcodec_receive_packet(codec.get(), packet.get())) == 0) {
                av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
                packet->stream_index = stream->index;
                check(av_interleaved_write_frame(format, packet.get()));
                av_packet_unref(packet.get());
            }
            if (status != AVERROR_EOF && status != AVERROR(EAGAIN))
                check(status);
        };
        for (std::size_t index = 0; index < yValues.size(); ++index) {
            check(av_frame_make_writable(frame.get()));
            for (int plane = 0; plane < descriptor->nb_components; ++plane) {
                const int value = plane == 0 ? static_cast<int>(yValues[index]) : values[plane];
                const int planeWidth = plane == 0 ? 64 : (64 >> descriptor->log2_chroma_w);
                const int planeHeight = plane == 0 ? 48 : (48 >> descriptor->log2_chroma_h);
                for (int row = 0; row < planeHeight; ++row) {
                    auto* dest = frame->data[plane] + static_cast<std::size_t>(row) * frame->linesize[plane];
                    if (descriptor->comp[plane].depth == 8) {
                        for (int col = 0; col < planeWidth; ++col) {
                            dest[col] = static_cast<uint8_t>(value + (horizontalRamp && plane == 0 ? col : 0));
                        }
                    } else {
                        auto* wide = reinterpret_cast<uint16_t*>(dest);
                        for (int col = 0; col < planeWidth; ++col) {
                            wide[col] = static_cast<uint16_t>((value + (horizontalRamp && plane == 0 ? col : 0))
                                                              << (descriptor->comp[plane].depth - 8));
                        }
                    }
                }
            }
            frame->pts = static_cast<std::int64_t>(index);
            check(avcodec_send_frame(codec.get(), frame.get()));
            drain();
        }
        check(avcodec_send_frame(codec.get(), nullptr));
        drain();
        check(av_write_trailer(format));
        check(avio_closep(&format->pb));
    }
    ~TaggedClip() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    std::filesystem::path path;

private:
    static void check(int status) {
        if (status < 0)
            throw std::runtime_error("FFV1 fixture failure: " + std::to_string(status));
    }
    struct CodecDelete {
        void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
    };
    struct FrameDelete {
        void operator()(AVFrame* value) const { av_frame_free(&value); }
    };
    struct PacketDelete {
        void operator()(AVPacket* value) const { av_packet_free(&value); }
    };
    std::unique_ptr<AVCodecContext, CodecDelete> codec;
    std::unique_ptr<AVFrame, FrameDelete> frame;
    std::unique_ptr<AVPacket, PacketDelete> packet;
    struct Output {
        AVFormatContext* context = nullptr;
        ~Output() {
            if (context) {
                if (context->pb)
                    avio_closep(&context->pb);
                avformat_free_context(context);
            }
        }
    } output;
};

// A minimal OCIO v2 config: scene-linear working space plus a display
// view (matrix + 3D LUT + gamma + range), the fixture ColorTests pins the
// GPU/CPU OCIO tolerance against.
[[nodiscard]] std::filesystem::path writeColorConfig() {
    const auto dir =
        std::filesystem::temp_directory_path() / ("nemo-viewer-color-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::create_directories(dir);

    std::string config;
    config += "ocio_profile_version: 2\n";
    config += "search_path: \"\"\n";
    config += "roles:\n  default: linear\n  scene_linear: linear\n";
    config += "colorspaces:\n";
    config += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
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

    std::ofstream config_file(dir / "color.ocio");
    config_file << config;
    config_file.close();

    // Per-axis curved diagonal 3D LUT, 4^3, R varies fastest (Iridas .cube
    // convention), matching the ColorTests fixture.
    std::ofstream cube(dir / "view_lut3d.cube");
    cube << "TITLE \"nemo viewer test lut\"\nLUT_3D_SIZE 4\n";
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
    cube.close();
    return dir / "color.ocio";
}

// A source document: `plate` (source, addressed by key) → optional const
// tint over → output. Sources enter through setSourceCommand — the only
// sanctioned mutation; runtime decode state never appears here.
struct SourceComposition {
    Document doc;
    NodeId output = kInvalidNode;
    NodeId over = kInvalidNode;
};

[[nodiscard]] SourceComposition makeSourceComposition(const std::string& key, const SourceReference& reference,
                                                      bool withTint) {
    SourceComposition composition;
    Document& doc = composition.doc;
    doc.name = "viewer-source-composition";
    CommandStack stack(doc);
    stack.push(setSourceCommand(key, reference));
    const NodeId plate = doc.graph.addNode("source", "plate");
    doc.graph.node(plate)->params = {{"source", key}};  // fixture/setup writes are sanctioned
    if (withTint) {
        const NodeId tint = doc.graph.addNode("constcolor", "tint");
        doc.graph.node(tint)->params = {{"color", "1.0 0.5 0.25 0.25"}};
        composition.over = doc.graph.addNode("merge", "over");
        (void)doc.graph.connect({plate, 0}, {composition.over, 0});
        (void)doc.graph.connect({tint, 0}, {composition.over, 1});
        composition.output = doc.graph.addNode("output", "result");
        (void)doc.graph.connect({composition.over, 0}, {composition.output, 0});
    } else {
        composition.output = doc.graph.addNode("output", "result");
        (void)doc.graph.connect({plate, 0}, {composition.output, 0});
    }
    return composition;
}

[[nodiscard]] EvaluationRequest requestFor(const Document& doc, Region region, std::int64_t frame, int scale = 1) {
    EvaluationRequest request;
    request.output = resolveOutput(doc);
    request.localTime = frame;
    request.region = region;
    request.samplingScale = scale;
    request.fullWidth = doc.sources.empty() ? region.x + region.width : 64;
    request.fullHeight = doc.sources.empty() ? region.y + region.height : 48;
    return request;
}

// The declared straight-alpha "over" of the CPU reference, as an oracle.
[[nodiscard]] CpuImage overReference(const CpuImage& base, const std::array<float, 4>& foreground) {
    CpuImage result(base.layout());
    for (int y = 0; y < base.height(); ++y) {
        for (int x = 0; x < base.width(); ++x) {
            const auto bg = base.pixel(x, y);
            std::array<float, 4> out;
            for (int c = 0; c < 3; ++c) {
                out[c] = foreground[3] * foreground[c] + (1.0F - foreground[3]) * bg[c];
            }
            out[3] = foreground[3] + (1.0F - foreground[3]) * bg[3];
            result.setPixel(x, y, out);
        }
    }
    return result;
}

void expectImagesClose(const CpuImage& expected, const CpuImage& actual, float tolerance, const char* what) {
    ASSERT_EQ(expected.width(), actual.width()) << what;
    ASSERT_EQ(expected.height(), actual.height()) << what;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const auto e = expected.pixel(x, y);
            const auto a = actual.pixel(x, y);
            for (std::size_t c = 0; c < CpuImage::channelCount(); ++c) {
                EXPECT_NEAR(a[c], e[c], tolerance) << what << ": pixel (" << x << "," << y << ") channel " << c;
            }
        }
    }
}

// Declared diagnostic-only readback (the ONE per test). The production
// and viewer paths never call this.
[[nodiscard]] CpuImage readBackEvaluation(eval::GpuEvaluation& evaluation, NodeId node, gpu::Device& device,
                                          gpu::Allocator& allocator) {
    return evaluation.readBack(node, device, allocator);
}

[[nodiscard]] std::vector<std::uint32_t> loadSpirv(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("missing SPIR-V module at " + path.string());
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return {reinterpret_cast<const std::uint32_t*>(bytes.data()),
            reinterpret_cast<const std::uint32_t*>(bytes.data()) + bytes.size() / 4};
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Real tagged media through the shared plan: decoded source → native
//    GPU effect, against the independently known gray oracle.
// ---------------------------------------------------------------------------

TEST(Viewer, SourceFlowsThroughSharedPlanWithKnownGrayOracle) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    TaggedClip clip(AV_PIX_FMT_YUV444P, {126});
    SourceComposition composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, false);
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 64, 48}, 0);

    // CPU oracle: the #21-pinned decode (its constant is checked against
    // the published BT.709 inverse OETF in MediaDecodeTests).
    const auto decoded = media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);

    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    auto evaluation = evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL,
                                  nullptr, &sources);
    const CpuImage gpuImage = readBackEvaluation(evaluation, request.output, *boot.device, *boot.allocator);

    // The whole image equals the decoded scene-linear frame (the source
    // fill is the identity at scale 1 over a same-size frame).
    expectImagesClose(decoded.frames[0], gpuImage, 1e-6F, "decoded frame through native GPU plan");
    // The gray oracle: published BT.709 inverse at limited-range 126.
    const auto center = gpuImage.pixel(32, 24);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(center[static_cast<std::size_t>(channel)], 0.261769, 0.003) << "channel " << channel;
    }
    EXPECT_EQ(center[3], 1.0F);

    // Plan evidence: the source step resolved the document reference and
    // its time mapping (acceptance example 6 — Document carries plain
    // data, the plan records resolved state).
    bool sawSource = false;
    for (const auto& step : evaluation.plan.steps) {
        if (step.type == "source") {
            sawSource = true;
            EXPECT_EQ(step.effectiveParams.at("source"), "plate");
            EXPECT_EQ(step.effectiveParams.at("frame"), "0");
            EXPECT_EQ(step.produced.residency, Residency::GpuDevice);
        }
    }
    EXPECT_TRUE(sawSource);

    // Decode-path evidence surfaced verbatim for the UI (FFV1 has no
    // Vulkan hwaccel: the measured software reason, never a guess).
    const auto probe = sources.probe(composition.doc, "plate");
    EXPECT_EQ(probe.info.width, 64);
    EXPECT_EQ(probe.info.height, 48);
    EXPECT_FALSE(probe.decision.hardware);
    EXPECT_FALSE(probe.decision.reason.empty());
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 2. Decoded source → native GPU effect (over a const tint) matches the
//    declared CPU over oracle on BOTH shader front ends.
// ---------------------------------------------------------------------------

TEST(Viewer, SourceEffectCompositionMatchesDeclaredOverOracle) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    TaggedClip clip(AV_PIX_FMT_YUV444P, {126});
    SourceComposition composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, true);
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 64, 48}, 0);

    const auto decoded = media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    const CpuImage expected = overReference(decoded.frames[0], {1.0F, 0.5F, 0.25F, 0.25F});

    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    auto slangEvaluation = evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator,
                                       10'000'000'000ULL, nullptr, &sources);
    expectImagesClose(expected, readBackEvaluation(slangEvaluation, request.output, *boot.device, *boot.allocator),
                      2e-7F, "slang composition over decoded source");

    // The GLSL reference front end meets the same contract on real media.
    const eval::EffectLibrary glsl = eval::glslEffectLibrary();
    auto glslEvaluation = evaluateGpu(composition.doc, request, glsl, *boot.device, *boot.allocator, 10'000'000'000ULL,
                                      nullptr, &sources);
    expectImagesClose(expected, readBackEvaluation(glslEvaluation, request.output, *boot.device, *boot.allocator),
                      2e-7F, "glsl composition over decoded source");
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 3. Persistent identity/time mapping in the Document; runtime decode
//    state in the session: forward decode, mapped frames, and backwards
//    re-entry (reopen then decode) all serve the right frames; mapping
//    errors are precise. (acceptance example 6)
// ---------------------------------------------------------------------------

TEST(Viewer, SourceTimeMappingAndBackwardsReEntry) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    TaggedClip clip(AV_PIX_FMT_YUV444P, {60, 70, 80, 90, 100, 110, 120});
    SourceReference reference;
    reference.path = clip.path.string();
    reference.frameOffset = 1;
    reference.frameStep = 1;
    SourceComposition composition = makeSourceComposition("plate", reference, false);
    const auto decoded = media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 7u);

    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());

    const auto renderAt = [&](std::int64_t localTime) {
        auto evaluation = evaluateGpu(composition.doc, requestFor(composition.doc, {0, 0, 64, 48}, localTime), slang,
                                      *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
        return readBackEvaluation(evaluation, evaluation.plan.request.output, *boot.device, *boot.allocator);
    };

    // Advance beyond retained decoder frames before re-entering the first
    // mapped time; the old frame must be reopened, not read at stream EOF.
    expectImagesClose(decoded.frames[1], renderAt(0), 1e-6F, "mapped frame 1 (forward decode)");
    expectImagesClose(decoded.frames[2], renderAt(1), 1e-6F, "mapped frame 2 (forward decode)");
    expectImagesClose(decoded.frames[6], renderAt(5), 1e-6F, "mapped frame 6 (forward decode)");
    expectImagesClose(decoded.frames[1], renderAt(0), 1e-6F, "mapped frame 1 (backwards re-entry)");

    // A mapped frame past the end of stream is an error naming the frame,
    // never a silent clamp.
    try {
        (void)renderAt(10);
        ADD_FAILURE() << "expected end-of-stream EvaluationException";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("past the end of source"), std::string::npos) << error.what();
    }

    // Unknown source key: the offending relationship is named, with the
    // available keys as evidence.
    try {
        Document missing = composition.doc;
        missing.graph.nodeByName("plate")->params = {{"source", "missing"}};
        (void)evaluateGpu(missing, requestFor(missing, {0, 0, 64, 48}, 0), slang, *boot.device, *boot.allocator,
                          10'000'000'000ULL, nullptr, &sources);
        ADD_FAILURE() << "expected unknown-key EvaluationException";
    } catch (const EvaluationException& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("no source reference named 'missing'"), std::string::npos) << message;
        EXPECT_NE(message.find("plate"), std::string::npos) << message;
    }

    // Negative mappings are errors (SourceReference::frameAt contract,
    // surfaced node-identifying through the executor).
    SourceReference negative;
    negative.path = clip.path.string();
    negative.frameOffset = -1;
    SourceComposition bad = makeSourceComposition("plate", negative, false);
    try {
        (void)evaluateGpu(bad.doc, requestFor(bad.doc, {0, 0, 64, 48}, 0), slang, *boot.device, *boot.allocator,
                          10'000'000'000ULL, nullptr, &sources);
        ADD_FAILURE() << "expected negative-frame EvaluationException";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("'plate'"), std::string::npos) << error.what();
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 4. SamplingScale representations: the reduced raster is the SAME image
//    (full coordinate semantics), a declared scale only, and front ends
//    agree on the reduced representation.
// ---------------------------------------------------------------------------

TEST(Viewer, SamplingScaleProducesConsistentRepresentations) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    Document doc;
    doc.name = "viewer-scale";
    const NodeId pattern = doc.graph.addNode("testpattern", "pattern");
    const NodeId out = doc.graph.addNode("output", "result");
    (void)doc.graph.connect({pattern, 0}, {out, 0});
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());

    auto full = evaluateGpu(doc, requestFor(doc, {0, 0, 8, 8}, 0, 1), slang, *boot.device, *boot.allocator);
    auto half = evaluateGpu(doc, requestFor(doc, {0, 0, 8, 8}, 0, 2), slang, *boot.device, *boot.allocator);
    const CpuImage fullImage = readBackEvaluation(full, out, *boot.device, *boot.allocator);
    const CpuImage halfImage = readBackEvaluation(half, out, *boot.device, *boot.allocator);

    ASSERT_EQ(halfImage.width(), 4);
    ASSERT_EQ(halfImage.height(), 4);
    ASSERT_EQ(fullImage.width(), 8);
    // Full-res coordinate semantics: the reduced raster samples the SAME
    // full-image coordinates, so (x, y) at scale 2 equals (2x, 2y) at
    // scale 1 — identical integer and float math, bit-equal.
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            EXPECT_EQ(halfImage.pixel(x, y), fullImage.pixel(2 * x, 2 * y)) << "pixel (" << x << "," << y << ")";
        }
    }

    // Only declared scales are allowed; an undeclared one is an error,
    // never a silent approximation.
    EXPECT_THROW((void)evaluateGpu(doc, requestFor(doc, {0, 0, 8, 8}, 0, 8), slang, *boot.device, *boot.allocator),
                 EvaluationException);
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 5. ViewerSession render: transform applied exactly once, GPU-complete,
//    matches the CPU OCIO reference; re-applying the transform to the
//    display-referred result is rejected. (acceptance examples 3/5)
// ---------------------------------------------------------------------------

TEST(Viewer, ViewerRenderAppliesTransformOnceAndMatchesCpuOcio) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const auto configPath = writeColorConfig();
    const test::ScopedEnvironment ocio("OCIO", configPath.string());

    TaggedClip clip(AV_PIX_FMT_YUV444P, {126});
    SourceComposition composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, false);
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 64, 48}, 0);

    // CPU oracle: decoded scene-linear frame through the OCIO CPU
    // processor under the SAME policy.
    const auto decoded = media::decodeClipSoftware(clip.path.string());
    ASSERT_EQ(decoded.frames.size(), 1u);
    CpuImage cpuViewed = decoded.frames[0];
    media::applyViewingTransformCpu(cpuViewed, configPath.string(), ColorPolicy{});
    ASSERT_EQ(cpuViewed.layout().color, ColorInterpretation::DisplayReferred);

    eval::ViewerSession session(*boot.instance, *boot.device, *boot.allocator, slangSpvDir());
    eval::ViewerFrame frame = session.render(composition.doc, request);
    EXPECT_EQ(frame.layout.color, ColorInterpretation::DisplayReferred);
    EXPECT_EQ(frame.layout.width, 64);
    EXPECT_EQ(frame.layout.height, 48);
    EXPECT_EQ(frame.revision, composition.doc.stateRevision());
    EXPECT_EQ(frame.request.samplingScale, 1);  // echoed request identity

    // One declared diagnostic readback of the viewed (display-referred)
    // frame; the presentation path itself is verified by test 6.
    auto& queue = boot.device->submissions(boot.device->graphics_family());
    CpuImage gpuViewed(frame.layout.width, frame.layout.height);
    gpu::downloadImage(queue, *boot.allocator, frame.image, gpuViewed.data(),
                       static_cast<std::size_t>(gpuViewed.width()) * static_cast<std::size_t>(gpuViewed.height()) * 4 *
                           sizeof(float),
                       10'000'000'000ULL);
    // OCIO CPU vs its own generated GPU program: the ColorTests-pinned
    // operation tolerance for this scene-linear→display transform.
    expectImagesClose(cpuViewed, gpuViewed, 2e-5F, "viewed frame vs OCIO CPU reference");

    // Transform exactly once: submitting the ALREADY display-referred
    // result again is rejected before any GPU work.
    const media::OcioGpuProgram program = media::buildViewingTransformGpu(configPath.string(), "linear", "sRGB/rec709");
    gpu::GpuViewingTransform transform(*boot.device, *boot.allocator, program);
    try {
        (void)transform.submit(frame.image, ColorInterpretation::DisplayReferred);
        ADD_FAILURE() << "expected double-transform rejection";
    } catch (const std::exception& error) {
        EXPECT_NE(std::string(error.what()).find("display"), std::string::npos) << error.what();
    }

    // requestId correlates calls monotonically.
    eval::ViewerFrame repeat = session.render(composition.doc, request);
    EXPECT_GT(repeat.requestId, frame.requestId);
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 6. Presentation quantization: display-referred values are premultiplied
//    and quantized WITHOUT another transfer; scene-linear input is
//    rejected. (.5,.25,1,.5 → bytes 64,32,128,128 — round-nearest of
//    v*alpha*255.)
// ---------------------------------------------------------------------------

TEST(Viewer, PresentationQuantizesExactlyOnceWithoutTransfer) {
    const Bootstrap boot = createBootstrap({.externalSharing = true});
    NEMO_SKIP_UNLESS_SLANG(boot);
    auto consumer = gpu::Device::create(*boot.instance, {.externalSharing = true, .physical = boot.device->physical()});
    auto consumerAllocator = gpu::Allocator::create(*boot.instance, *consumer, {.max_device_bytes = 16u << 20});
    auto& consumerQueue = consumer->submissions(consumer->graphics_family());

    const std::vector<std::uint32_t> spirv = loadSpirv(slangSpvDir() / "viewerPresentation.spv");
    auto& queue = boot.device->submissions(boot.device->graphics_family());

    gpu::Image source = boot.allocator->create_image(1, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                     2);
    const std::array<float, 4> display{0.5F, 0.25F, 1.0F, 0.5F};
    gpu::uploadImage(queue, *boot.allocator, source, display.data(), display.size() * sizeof(float), 10'000'000'000ULL);
    // uploadImage ends SHADER_READ_ONLY_OPTIMAL; the presentation contract
    // consumes GENERAL storage images.
    gpu::imageBarrier(queue, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 10'000'000'000ULL);

    // The sole diagnostic readback runs on the other logical device.
    // Its pixels prove external memory and semaphore handoff, not just
    // that the producer quantized into its own allocation successfully.
    gpu::ViewerPresentation presented = gpu::prepareViewerPresentation(*boot.device, *boot.allocator, *consumer, source,
                                                                       ColorInterpretation::DisplayReferred, spirv);
    consumerQueue.submit_and_wait(
        [&](VkCommandBuffer command) {
            gpu::acquireViewerPresentation(*consumer, presented, command);
            gpu::recordImageBarrier(command, presented.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
        },
        10'000'000'000ULL);
    std::uint8_t bytes[4] = {};
    gpu::downloadImage(consumerQueue, *consumerAllocator, presented.image, bytes, sizeof(bytes), 10'000'000'000ULL);
    // Premultiplied (v * alpha), round-nearest quantization — no second
    // transfer applied to display values.
    EXPECT_EQ(bytes[0], 64);
    EXPECT_EQ(bytes[1], 32);
    EXPECT_EQ(bytes[2], 128);
    EXPECT_EQ(bytes[3], 128);

    // Scene-linear input is rejected: quantizing a scene-linear buffer
    // would silently change operation meaning.
    EXPECT_ANY_THROW((void)gpu::prepareViewerPresentation(*boot.device, *boot.allocator, *consumer, source,
                                                          ColorInterpretation::SceneLinear, spirv));
    expectValidationClean(*boot.instance);
}

TEST(Viewer, SharedImageChargeSurvivesDroppedHandlesUntilConsumerCompletion) {
    const Bootstrap boot = createBootstrap({.externalSharing = true});
    NEMO_SKIP_OR_FAIL(boot);
    auto consumer = gpu::Device::create(*boot.instance, {.externalSharing = true, .physical = boot.device->physical()});
    auto& queue = consumer->submissions(consumer->graphics_family());
    const auto before = boot.allocator->charged_bytes();
    auto [producerImage, consumerImage] = boot.allocator->create_shared_image(
        *consumer, 16, 16, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const auto charged = boot.allocator->charged_bytes();
    std::weak_ptr<const void> allocation = consumerImage.retain();
    struct Gate {
        VkDevice device{};
        VkSemaphore semaphore{};
        ~Gate() { vkDestroySemaphore(device, semaphore, nullptr); }
    };
    auto gate = std::make_shared<Gate>();
    gate->device = consumer->handle();
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE,
                                   0};
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type, 0};
    ASSERT_EQ(vkCreateSemaphore(gate->device, &create, nullptr, &gate->semaphore), VK_SUCCESS);
    struct Release {
        std::shared_ptr<Gate> gate;
        bool released{};
        VkResult signal() {
            if (released)
                return VK_SUCCESS;
            VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, gate->semaphore, 1};
            const auto result = vkSignalSemaphore(gate->device, &info);
            released = result == VK_SUCCESS;
            return result;
        }
        ~Release() { (void)signal(); }
    } release{gate};
    gpu::SubmissionQueue::TimelineSemaphores dependencies;
    dependencies.wait = {gate->semaphore};
    dependencies.waitValues = {1};
    const auto completion = queue.submit(
        [&](VkCommandBuffer command) {
            // Neither side has used this allocation yet. The consumer is its
            // first owner and clears it after the host releases the gate.
            gpu::recordImageBarrier(command, consumerImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkClearColorValue color{{0.25F, 0.5F, 0.75F, 1.0F}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command, consumerImage.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                                 &range);
        },
        {consumerImage.retain(), gate}, dependencies);
    ASSERT_TRUE(completion);
    EXPECT_FALSE(queue.wait(*completion, 0));
    producerImage = {};
    consumerImage = {};
    EXPECT_FALSE(allocation.expired());
    EXPECT_EQ(boot.allocator->charged_bytes(), charged);
    ASSERT_EQ(release.signal(), VK_SUCCESS);
    ASSERT_TRUE(queue.wait(*completion, 5'000'000'000ULL));
    EXPECT_TRUE(allocation.expired());
    EXPECT_EQ(boot.allocator->charged_bytes(), before);
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 7. Unsupported formats fail with a precise reason naming clip and
//    format — never a silent substitution. (acceptance example 5)
// ---------------------------------------------------------------------------

TEST(Viewer, UnsupportedFormatRejectedWithPreciseReason) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    // 8-bit 4:2:2 is outside the supported decode subset (#21): planar
    // 420/444, NV12, and grayscale are supported; 422 is not.
    TaggedClip clip(AV_PIX_FMT_YUV422P, {126});
    SourceComposition composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, false);
    const EvaluationRequest request = requestFor(composition.doc, {0, 0, 64, 48}, 0);
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    try {
        (void)evaluateGpu(composition.doc, request, slang, *boot.device, *boot.allocator, 10'000'000'000ULL, nullptr,
                          &sources);
        ADD_FAILURE() << "expected MediaDecodeError";
    } catch (const media::MediaDecodeError& error) {
        EXPECT_EQ(error.clip, clip.path.string());
        EXPECT_NE(error.format.find("yuv422p"), std::string::npos) << error.reason;
        EXPECT_FALSE(error.reason.empty());
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 8. Interpretation maps: strict parsing (unknown field/value rejected);
//    a stream-tagged field keeps its declared value even when an override
//    names something else (the stream is authoritative).
// ---------------------------------------------------------------------------

TEST(Viewer, InterpretationMapIsParsedStrictly) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    TaggedClip clip(AV_PIX_FMT_YUV444P, {126});
    const eval::EffectLibrary slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    const auto runOnce = [&](const SourceReference& reference) {
        SourceComposition composition = makeSourceComposition("plate", reference, false);
        eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
        return evaluateGpu(composition.doc, requestFor(composition.doc, {0, 0, 64, 48}, 0), slang, *boot.device,
                           *boot.allocator, 10'000'000'000ULL, nullptr, &sources);
    };

    // Unknown field.
    SourceReference unknownField;
    unknownField.path = clip.path.string();
    unknownField.interpretation = {{"colorimetry", "bt709"}};
    try {
        (void)runOnce(unknownField);
        ADD_FAILURE() << "expected unknown-field EvaluationException";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("unknown field 'colorimetry'"), std::string::npos) << error.what();
    }

    // Unknown value.
    SourceReference unknownValue;
    unknownValue.path = clip.path.string();
    unknownValue.interpretation = {{"transfer", "weird"}};
    try {
        (void)runOnce(unknownValue);
        ADD_FAILURE() << "expected unknown-value EvaluationException";
    } catch (const EvaluationException& error) {
        EXPECT_NE(std::string(error.what()).find("unsupported value 'weird'"), std::string::npos) << error.what();
    }

    // The stream is authoritative: an override naming an already-tagged
    // field is ignored, so the decode stays the published BT.709 gray.
    SourceReference overridden;
    overridden.path = clip.path.string();
    overridden.interpretation = {{"transfer", "gamma22"}};
    auto evaluation = runOnce(overridden);
    const CpuImage gpuImage =
        readBackEvaluation(evaluation, evaluation.plan.request.output, *boot.device, *boot.allocator);
    const auto center = gpuImage.pixel(32, 24);
    for (int channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(center[static_cast<std::size_t>(channel)], 0.261769, 0.003) << "channel " << channel;
    }
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 9. Under delayed GPU completion the decoded source frame and effect
//    intermediates stay owned by the completion; a dropped evaluation
//    cannot retire them, and a retry is correct. (acceptance example 7)
// ---------------------------------------------------------------------------

TEST(Viewer, SourceRetentionUnderDelayedCompletion) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    auto& queue = boot.device->submissions(boot.device->graphics_family());
    TaggedClip clip(AV_PIX_FMT_YUV444P, {126});
    auto composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, true);
    const auto request = requestFor(composition.doc, {0, 0, 16, 16}, 0);
    const auto slang = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    auto sources = std::make_unique<eval::SourceSession>(*boot.instance, *boot.device, *boot.allocator,
                                                         slangSpvDir() / "mediaConvert.spv");
    std::weak_ptr<const void> sourceAllocation;
    {
        // Decode before blocking execution: decode owns synchronous upload.
        auto decoded =
            sources->acquire(composition.doc, *composition.doc.graph.nodeByName("plate"), 0, 10'000'000'000ULL);
        sourceAllocation = decoded.image->retain();
    }
    struct Gate {
        VkDevice device{};
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
    struct Release {
        std::shared_ptr<Gate> gate;
        bool released{};
        VkResult signal() {
            if (released)
                return VK_SUCCESS;
            VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, gate->semaphore, 1};
            const auto result = vkSignalSemaphore(gate->device, &info);
            released = result == VK_SUCCESS;
            return result;
        }
        ~Release() { (void)signal(); }
    } release{gate};
    gpu::SubmissionQueue::TimelineSemaphores dependencies;
    dependencies.wait = {gate->semaphore};
    dependencies.waitValues = {1};
    const auto blocker = queue.submit([](VkCommandBuffer) {}, {gate}, dependencies);
    ASSERT_TRUE(blocker);
    auto pending = eval::submitGpu(composition.doc, request, slang, *boot.device, *boot.allocator, sources.get());
    ASSERT_TRUE(pending);
    ASSERT_TRUE(pending->completion);
    const auto completion = *pending->completion;
    EXPECT_FALSE(queue.wait(completion, 0));
    auto output = pending->images.at(request.output);
    pending.reset();
    sources.reset();  // Drop decoder/cache and publication before completion.
    EXPECT_FALSE(sourceAllocation.expired());
    ASSERT_EQ(release.signal(), VK_SUCCESS);
    ASSERT_TRUE(queue.wait(completion, 5'000'000'000ULL));
    EXPECT_TRUE(sourceAllocation.expired());
    CpuImage completed(16, 16);
    gpu::downloadImage(queue, *boot.allocator, output->image, completed.data(), 16 * 16 * 4 * sizeof(float),
                       5'000'000'000ULL);
    // Gray 126 -> linear 0.261769, then 25% tint (1,.5,.25) over.
    const auto pixel = completed.pixel(8, 8);
    EXPECT_NEAR(pixel[0], 0.44632675F, 0.003F);
    EXPECT_NEAR(pixel[1], 0.32132675F, 0.003F);
    EXPECT_NEAR(pixel[2], 0.25882675F, 0.003F);
    EXPECT_FLOAT_EQ(pixel[3], 1.0F);
    expectValidationClean(*boot.instance);
}

// ---------------------------------------------------------------------------
// 10. Sibling representations coexist: scale 1 and scale 2 viewer frames
//     render side by side without erasing each other, and the repeat
//     scale-1 render is served from the content cache (observable
//     avoided work, issue #9 counters).
// ---------------------------------------------------------------------------

TEST(Viewer, SiblingRepresentationReuseAcrossViewerRenders) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);

    const auto configPath = writeColorConfig();
    const test::ScopedEnvironment ocio("OCIO", configPath.string());

    TaggedClip clip(AV_PIX_FMT_YUV444P, {126});
    SourceComposition composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, false);
    eval::ViewerSession session(*boot.instance, *boot.device, *boot.allocator, slangSpvDir());

    eval::ViewerFrame full = session.render(composition.doc, requestFor(composition.doc, {0, 0, 64, 48}, 0, 1));
    EXPECT_EQ(full.layout.width, 64);
    EXPECT_EQ(full.layout.height, 48);

    // A second representation coexists — the sibling scene-linear results
    // stay valid in the cache while a different representation renders.
    eval::ViewerFrame half = session.render(composition.doc, requestFor(composition.doc, {0, 0, 64, 48}, 0, 2));
    EXPECT_EQ(half.layout.width, 32);
    EXPECT_EQ(half.layout.height, 24);

    // The repeat scale-1 render is served from content reuse (the decoded
    // frame and the plan results), not recomputed from scratch.
    const CacheCounts before = session.reuseCounts();
    eval::ViewerFrame again = session.render(composition.doc, requestFor(composition.doc, {0, 0, 64, 48}, 0, 1));
    const CacheCounts after = session.reuseCounts();
    EXPECT_GT(after.hits, before.hits);
    EXPECT_GT(again.requestId, half.requestId);
    EXPECT_EQ(again.layout.width, 64);
    expectValidationClean(*boot.instance);
}

TEST(Viewer, SourceReplacementAndWorkingPolicyCannotReplayRetiredDecode) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    TaggedClip first(AV_PIX_FMT_YUV444P, {126});
    TaggedClip second(AV_PIX_FMT_YUV444P10LE, {60});
    auto composition = makeSourceComposition("plate", SourceReference{first.path.string()}, false);
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    const auto effects = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    const auto request = requestFor(composition.doc, {7, 9, 1, 1}, 0);
    const auto render = [&] {
        auto frame = eval::evaluateGpu(composition.doc, request, effects, *boot.device, *boot.allocator,
                                       10'000'000'000ULL, nullptr, &sources);
        return readBackEvaluation(frame, request.output, *boot.device, *boot.allocator).pixel(0, 0);
    };
    EXPECT_NEAR(render()[0], 0.261769F, 0.003F);
    CommandStack edits(composition.doc);
    edits.push(setSourceCommand("plate", SourceReference{second.path.string()}));
    // 10-bit 4:4:4 fixture: Y=240, limited 64..940, BT.709 inverse OETF.
    EXPECT_NEAR(render()[0], 0.055803586F, 0.0001F);
    ColorPolicy unsupported;
    unsupported.workingSpace = "ACEScg";
    edits.push(setColorPolicyCommand(unsupported));
    EXPECT_THROW((void)render(), media::MediaDecodeError);
    ASSERT_TRUE(edits.undo());
    ASSERT_TRUE(edits.undo());
    EXPECT_NEAR(render()[0], 0.261769F, 0.003F);
    expectValidationClean(*boot.instance);
}

TEST(Viewer, CroppedSourceReductionPreservesFullImageCoordinates) {
    const Bootstrap boot = createBootstrap();
    NEMO_SKIP_UNLESS_SLANG(boot);
    TaggedClip clip(AV_PIX_FMT_YUV444P, {60}, 128, 128, AVCOL_TRC_BT709, true, true);
    auto composition = makeSourceComposition("plate", SourceReference{clip.path.string()}, false);
    eval::SourceSession sources(*boot.instance, *boot.device, *boot.allocator, slangSpvDir() / "mediaConvert.spv");
    EXPECT_DOUBLE_EQ(sources.probe(composition.doc, "plate").info.pixelAspect, 2.0);
    const auto effects = eval::loadSlangEffectLibrary(slangSpvDir(), slangSpvDir());
    const auto request = requestFor(composition.doc, {8, 4, 16, 12}, 0, 4);
    auto evaluation = eval::evaluateGpu(composition.doc, request, effects, *boot.device, *boot.allocator,
                                        10'000'000'000ULL, nullptr, &sources);
    const auto image = readBackEvaluation(evaluation, request.output, *boot.device, *boot.allocator);
    ASSERT_EQ(image.width(), 4);
    ASSERT_EQ(image.height(), 3);
    for (int x = 0; x < image.width(); ++x) {
        // Authored limited-range luma Y=60+fullX; published BT.709 inverse.
        // ROI must not stretch the entire source into its smaller domain.
        const double signal = (60.0 + 8 + 4 * x - 16) / 219.0;
        const double expected = std::pow((signal + 0.099) / 1.099, 1.0 / 0.45);
        for (int channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(image.pixel(x, 0)[channel], expected, 0.0001);
    }
    expectValidationClean(*boot.instance);
}
