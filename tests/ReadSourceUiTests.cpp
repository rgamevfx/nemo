// Read node media control tests (issues #61/#82).
//
// These cover the adapter the inspector's node-local file control consumes:
// choosing a real file registers a validated document source + Media Bin entry
// as one undoable command, a movie keeps its container-declared interval (a
// still is the one-media case, never the reverse), a numbered selection asks
// for the explicit Sequence/Single Image choice, an explicit pattern binds and
// is aligned to its discovered first frame, the Read's own
// range/offset/policy/color choices are node-scoped (two Reads of one media
// stay independent), a rejected or superseded selection authors nothing, and
// reload refreshes the SHARED facts and revision without touching authored
// choices. Decode/format discovery itself belongs to MediaImportTests; this
// file asserts what the adapter adds and proves the evaluated pixels follow the
// shipped actions.

#include "MediaLibraryModel.hpp"
#include "NativeFileChooser.hpp"
#include "ReadSourceController.hpp"
#include "ViewerController.hpp"
#include "ViewerRuntime.hpp"

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/ReadSourceCommands.hpp"
#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/MediaImportService.hpp"

// The real-movie fixture below encodes in-process through the media library's
// own FFmpeg dependency (the same pattern MediaImportTests/MediaEncodeTests
// use), so no external tool and no mocked probe are involved.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using nemo::NodeId;
using nemo::ProjectSession;
using nemo::SourceReference;

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        QTest::qWait(20);
    }
    return predicate();
}

// A real, decodable EXR still: the shared import worker reads it through the
// image adapter, so the probe is not simulated.
std::filesystem::path writeExr(const std::filesystem::path& directory, const std::string& name) {
    nemo::CpuImage image(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            image.setPixel(x, y, {0.25F, 0.5F, 0.75F, 1.0F});
    const auto path = directory / (name + ".exr");
    nemo::media::writeImage(path.string(), image, nemo::media::OutputPrecision::Half);
    return path;
}

// A real sequence frame with a distinguishing red value (so an overwritten or
// appended frame is observable in evaluated pixels).
// A minimal OCIO v2 config naming one input space. Written to a fixed path so
// the test can rewrite the SAME path with different content (the case a
// path-keyed cache cannot see).
std::filesystem::path writeInputConfig(const std::filesystem::path& path, const std::string& spaceName) {
    std::string config;
    config += "ocio_profile_version: 2\n";
    config += "search_path: \"\"\n";
    config += "roles:\n  default: linear\n  scene_linear: linear\n";
    config += "colorspaces:\n";
    config += "  - !<ColorSpace>\n    name: linear\n    allocation: linear\n";
    config += "  - !<ColorSpace>\n    name: " + spaceName + "\n    allocation: uniform\n";
    std::ofstream file(path);
    file << config;
    file.close();
    return path;
}

std::filesystem::path writeFrame(const std::filesystem::path& pattern, const std::int64_t frame, const float red) {
    nemo::CpuImage image(2, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            image.setPixel(x, y, {red, 0.25F, 0.5F, 1.0F});
    const std::string path = nemo::media::resolveFramePath(pattern.string(), frame);
    nemo::media::writeImage(path, image, nemo::media::OutputPrecision::Half);
    return path;
}

// Whether the generated stream DECLARES the YUV interpretation it encodes. An
// untagged variant declares nothing (the camera-file case), so every decode
// field must be authored before the file can be interpreted at all.
enum class MovieTagging { Tagged, Untagged };

// MP4 declares its sample count, independently fixing the expected interval.
std::filesystem::path writeMovie(const std::filesystem::path& directory, const int frames,
                                 const MovieTagging tagging = MovieTagging::Tagged) {
    struct Resources {
        AVFormatContext* format{};
        AVCodecContext* codec{};
        AVFrame* frame{};
        AVPacket* packet{};
        ~Resources() {
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&codec);
            if (format) {
                if (format->pb)
                    avio_closep(&format->pb);
                avformat_free_context(format);
            }
        }
    } resources;
    auto& [format, codec, frame, packet] = resources;
    const auto require = [](bool condition, const char* operation) {
        if (!condition)
            throw std::runtime_error(std::string("synthetic movie write: ") + operation);
    };
    const auto path = directory / "shot.mp4";
    require(avformat_alloc_output_context2(&format, nullptr, nullptr, path.string().c_str()) >= 0 && format,
            "allocate output");
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    require(encoder != nullptr, "libx264 unavailable");
    codec = avcodec_alloc_context3(encoder);
    require(codec != nullptr, "allocate encoder");
    codec->width = 64;
    codec->height = 48;
    codec->time_base = AVRational{1, 24};
    codec->framerate = AVRational{24, 1};
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    if (tagging == MovieTagging::Tagged) {
        codec->colorspace = AVCOL_SPC_BT709;
        codec->color_primaries = AVCOL_PRI_BT709;
        codec->color_trc = AVCOL_TRC_BT709;
        codec->color_range = AVCOL_RANGE_MPEG;
        codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
    } else {
        // Leave color metadata unspecified; the codec may still declare layout
        // facts such as its chroma sample location.
        codec->colorspace = AVCOL_SPC_UNSPECIFIED;
        codec->color_primaries = AVCOL_PRI_UNSPECIFIED;
        codec->color_trc = AVCOL_TRC_UNSPECIFIED;
        codec->color_range = AVCOL_RANGE_UNSPECIFIED;
        codec->chroma_sample_location = AVCHROMA_LOC_UNSPECIFIED;
    }
    if (format->oformat->flags & AVFMT_GLOBALHEADER)
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    require(avcodec_open2(codec, encoder, nullptr) >= 0, "open encoder");
    AVStream* stream = avformat_new_stream(format, nullptr);
    require(stream && avcodec_parameters_from_context(stream->codecpar, codec) >= 0, "configure stream");
    stream->time_base = codec->time_base;
    require(avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) >= 0, "open output");
    require(avformat_write_header(format, nullptr) >= 0, "write header");
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    require(frame && packet, "allocate frame and packet");
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    require(av_frame_get_buffer(frame, 0) >= 0, "allocate image");
    for (int plane = 0; plane < 3; ++plane) {
        const int height = plane == 0 ? codec->height : codec->height / 2;
        const int width = plane == 0 ? codec->width : codec->width / 2;
        for (int y = 0; y < height; ++y)
            std::fill_n(frame->data[plane] + y * frame->linesize[plane], width, plane == 0 ? 96 : 128);
    }
    const auto drain = [&] {
        int result = 0;
        while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            require(av_interleaved_write_frame(format, packet) >= 0, "write packet");
            av_packet_unref(packet);
        }
        require(result == AVERROR(EAGAIN) || result == AVERROR_EOF, "receive packet");
    };
    for (int index = 0; index < frames; ++index) {
        frame->pts = index;
        require(avcodec_send_frame(codec, frame) >= 0, "send frame");
        drain();
    }
    require(avcodec_send_frame(codec, nullptr) >= 0, "flush encoder");
    drain();
    require(av_write_trailer(format) >= 0, "write trailer");
    return path;
}

const nemo::MediaCatalogEntry* entryForKey(const nemo::Document& document, const std::string& key) {
    for (const auto& entry : document.mediaCatalog().entries())
        if (entry.sourceKey == key)
            return &entry;
    return nullptr;
}

class ReadSourceFixture {
public:
    ReadSourceFixture() : model_(session_, importer_), controller_(session_, model_, chooser_) {
        first_ = addRead("Read1");
    }

    // A document whose Read lives in its own definition network with two
    // occurrences (the construction the core occurrence tests use).
    ReadSourceFixture(nemo::Document document, const nemo::NetworkId definition, const nemo::NodeId read)
        : session_(std::move(document)), model_(session_, importer_), controller_(session_, model_, chooser_),
          definition_(definition) {
        first_ = read;
    }

    NodeId addRead(const std::string& name) {
        auto created = std::make_shared<NodeId>();
        const auto result =
            session_.submit(nemo::addNodeCommand(session_.document().rootNetworkId(), "source", name, created),
                            {.expectedRevision = session_.revision()});
        EXPECT_TRUE(result.committed);
        return *created;
    }

    [[nodiscard]] QString networkId() const { return QString::number(session_.document().rootNetworkId()); }
    [[nodiscard]] QString definitionId() const { return QString::number(definition_); }
    [[nodiscard]] static QString nodeIdOf(const NodeId node) { return QString::number(node); }
    [[nodiscard]] QString nodeId() const { return nodeIdOf(first_); }
    [[nodiscard]] QVariantMap info(const NodeId node) const { return controller_.info(networkId(), nodeIdOf(node)); }
    [[nodiscard]] QVariantMap info(const NodeId node, const int frame) const {
        return controller_.info(networkId(), nodeIdOf(node), frame);
    }
    [[nodiscard]] QVariantMap info(const NodeId node, const int frame, const QString& instanceId) const {
        return controller_.info(networkId(), nodeIdOf(node), frame, instanceId);
    }
    [[nodiscard]] QVariantMap info() const { return info(first_); }
    [[nodiscard]] bool choiceIsAmbiguous() const { return info().value(QStringLiteral("choiceAmbiguous")).toBool(); }

    [[nodiscard]] const nemo::NodeInstance* instance(const NodeId node) const {
        return session_.document().network(session_.document().rootNetworkId()).graph().node(node);
    }

    // The node's authored Read choices, read through the core owner so the test
    // asserts the same semantic set the resolver and catalog use.
    [[nodiscard]] nemo::ReadNodeOverrides overrides(const NodeId node) const {
        const nemo::NodeInstance* node_ = instance(node);
        if (node_ == nullptr)
            return {};
        return nemo::readAuthoredOverrides(session_.document(), *node_);
    }

    [[nodiscard]] std::string keyOf(const NodeId node) const {
        const nemo::NodeInstance* node_ = instance(node);
        if (node_ == nullptr)
            return {};
        const auto found = node_->params.find("source");
        if (found == node_->params.end())
            return {};
        const auto* text = std::get_if<std::string>(&found->second);
        return text == nullptr ? std::string{} : *text;
    }

    [[nodiscard]] std::string nodeKey() const { return keyOf(first_); }

    // The public CPU evaluation seam over the real graph: the Read node's
    // output feeds the default Output node (connected once, then reused).
    [[nodiscard]] nemo::CpuEvaluation evaluate(const NodeId node, const std::int64_t localTime, const int size = 2) {
        const nemo::NetworkId network = session_.document().rootNetworkId();
        const NodeId output = nemo::resolveOutput(session_.document(), network);
        const nemo::Graph& graph = session_.document().network(network).graph();
        bool connected = false;
        for (const nemo::Edge& edge : graph.edgesInto(output)) {
            if (edge.from.node == node) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            EXPECT_TRUE(session_
                            .submit(nemo::connectCommand(network, {node, 0}, {output, 0}),
                                    {.expectedRevision = session_.revision()})
                            .committed);
        }
        nemo::EvaluationRequest request;
        request.network = network;
        request.output = output;
        request.localTime = localTime;
        request.region = {0, 0, size, size};
        nemo::media::ImageSourceProvider sources;
        return nemo::evaluateCpu(session_.document(), request, nullptr, &sources);
    }

    // One atomic shared value gesture, the SAME owner the Read editor uses for
    // every value edit (an existing channel authors the current-frame key, an
    // unanimated parameter takes the static value, one history entry).
    [[nodiscard]] bool gesture(const NodeId node, const QVariantMap& values) {
        QStringList keys;
        for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            keys.push_back(it.key());
        const QString token = viewer_.beginNodeParameterEdits(networkId(), nodeIdOf(node), keys);
        if (token.isEmpty())
            return false;
        if (!viewer_.updateNodeParameterEdits(token, values)) {
            viewer_.cancelNodeParameterEdit(token);
            return false;
        }
        return viewer_.commitNodeParameterEdit(token);
    }

    QTemporaryDir temporary_;
    ProjectSession session_;
    nemo::media::MediaImportService importer_;
    nemo::ui::MediaLibraryModel model_;
    nemo::ui::NativeFileChooser chooser_;
    nemo::ui::ReadSourceController controller_;
    nemo::ui::ViewerRuntime runtime_;
    nemo::ui::ViewerController viewer_{&runtime_, session_};
    NodeId first_{nemo::kInvalidNode};
    nemo::NetworkId definition_{nemo::kInvalidNetwork};
};

}  // namespace

TEST(ReadSourceUiTest, ChosenPathRegistersAValidatedReferenceAndEntryAsOneHistoryEntry) {
    ReadSourceFixture fixture;
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "plate");
    const auto baseline = fixture.session_.revision();

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    const std::string key = fixture.nodeKey();
    const auto reference = fixture.session_.document().sources.find(key);
    ASSERT_NE(reference, fixture.session_.document().sources.end());
    EXPECT_EQ(nemo::normalizedSourcePath(reference->second.path), nemo::normalizedSourcePath(still.string()));
    EXPECT_EQ(reference->second.revision, 1u);

    const nemo::MediaCatalogEntry* entry = entryForKey(fixture.session_.document(), key);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->provenance, "oiio");
    // A still is one image with no interval, so it stays usable at any local time.
    EXPECT_FALSE(entry->metadata.committedProbe->firstFrame.has_value());
    EXPECT_FALSE(entry->metadata.committedProbe->lastFrame.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->availableFrameCount, std::optional<std::int64_t>{1});

    // One validated, undoable command; the controller's probe is a proposal,
    // not a document edit.
    EXPECT_EQ(fixture.session_.revision(), baseline + 1);

    const QVariantMap state = fixture.info();
    EXPECT_EQ(state.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    EXPECT_EQ(state.value(QStringLiteral("sourceKey")).toString(), QString::fromStdString(key));
    EXPECT_EQ(state.value(QStringLiteral("originalFirst")).toString(), QString());
    EXPECT_TRUE(state.value(QStringLiteral("error")).toString().isEmpty());

    ASSERT_TRUE(fixture.session_.undo({.expectedRevision = fixture.session_.revision()}).committed);
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("empty"));
}

TEST(ReadSourceUiTest, RejectedAndMissingPathsAuthorNothing) {
    ReadSourceFixture fixture;
    const auto baseline = fixture.session_.revision();

    // No file: the probe reports the missing path and no command is submitted.
    const auto missing = fixture.temporary_.path().toStdString() + "/absent.exr";
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(missing)));
    ASSERT_TRUE(waitFor([&] { return !fixture.info().value(QStringLiteral("error")).toString().isEmpty(); }));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), baseline);

    // Ambiguous color metadata: the adapter's own reason is surfaced and again
    // nothing is authored.
    const auto ambiguous = fixture.temporary_.path().toStdString() + "/ambiguous.tif";
    {
        nemo::CpuImage image(2, 2);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                image.setPixel(x, y, {0.5F, 0.5F, 0.5F, 1.0F});
        nemo::media::writeImage(ambiguous, image, nemo::media::OutputPrecision::Float32);
    }
    fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(ambiguous));
    ASSERT_TRUE(waitFor([&] {
        const QString error = fixture.info().value(QStringLiteral("error")).toString();
        return !error.isEmpty() && !error.contains(QStringLiteral("absent"));
    }));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), baseline);
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("empty"));
}

// A numbered selection whose run matches several files is not silently
// reinterpreted: it waits for the explicit choice, and the Sequence choice
// binds the discovered pattern aligned to the first available frame while the
// Single Image choice binds the literal file as one still.
TEST(ReadSourceUiTest, NumberedSelectionAsksForTheExplicitSequenceOrSingleChoice) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/shot.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    const std::string literal = nemo::media::resolveFramePath(pattern, 1001);
    const auto baseline = fixture.session_.revision();

    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(literal)));
    ASSERT_TRUE(waitFor([&] { return fixture.info().value(QStringLiteral("choiceRequired")).toBool(); }));
    // Nothing is authored until the artist decides.
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_EQ(fixture.session_.revision(), baseline);
    const QVariantMap choice = fixture.info();
    EXPECT_EQ(choice.value(QStringLiteral("choiceFirst")).toString(), QStringLiteral("1001"));
    EXPECT_EQ(choice.value(QStringLiteral("choiceLast")).toString(), QStringLiteral("1002"));

    ASSERT_TRUE(fixture.controller_.confirmSourceChoice(fixture.networkId(), fixture.nodeId(), true));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();
    EXPECT_EQ(fixture.session_.revision(), baseline + 1);
    // The committed reference is the canonical pattern, and the mapping is
    // aligned so the available first frame is local zero.
    EXPECT_EQ(fixture.session_.document().sources.at(key).path, pattern);
    const nemo::ReadNodeOverrides overrides = fixture.overrides(fixture.first_);
    EXPECT_EQ(overrides.frameOffset, 1001);
    EXPECT_EQ(overrides.frameStep, 1);
    EXPECT_EQ(overrides.rangeMode, std::string(nemo::kReadRangeModeAuto));
    const nemo::MediaCatalogEntry* entry = entryForKey(fixture.session_.document(), key);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->firstFrame, std::optional<std::int64_t>{1001});
    EXPECT_EQ(entry->metadata.committedProbe->lastFrame, std::optional<std::int64_t>{1002});

    // The Single Image choice on a sibling node binds the literal file as a
    // still without a range.
    const NodeId single = fixture.addRead("Read2");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(single),
                                                  QString::fromStdString(literal)));
    ASSERT_TRUE(waitFor([&] { return fixture.info(single).value(QStringLiteral("choiceRequired")).toBool(); }));
    ASSERT_TRUE(
        fixture.controller_.confirmSourceChoice(fixture.networkId(), ReadSourceFixture::nodeIdOf(single), false));
    ASSERT_TRUE(waitFor([&] { return !fixture.keyOf(single).empty(); }));
    const nemo::ReadNodeOverrides singleOverrides = fixture.overrides(single);
    EXPECT_EQ(singleOverrides.frameOffset, 0);
    const nemo::MediaCatalogEntry* singleEntry = entryForKey(fixture.session_.document(), fixture.keyOf(single));
    ASSERT_NE(singleEntry, nullptr);
    ASSERT_TRUE(singleEntry->metadata.committedProbe.has_value());
    EXPECT_FALSE(singleEntry->metadata.committedProbe->firstFrame.has_value());
    EXPECT_EQ(singleEntry->metadata.committedProbe->duration, 1);
}

// An explicit pattern binds immediately (the artist authored a sequence) and is
// aligned to the discovered first frame, so a sequence starting at 1001 loads
// without authoring a nonexistent zero frame.
TEST(ReadSourceUiTest, ExplicitPatternBindsAlignedToTheDiscoveredFirstFrame) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/plate.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    writeFrame(pattern, 1004, 0.5F);

    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const nemo::ReadNodeOverrides overrides = fixture.overrides(fixture.first_);
    EXPECT_EQ(overrides.frameOffset, 1001);
    EXPECT_EQ(fixture.info().value(QStringLiteral("originalFirst")).toString(), QStringLiteral("1001"));
    EXPECT_EQ(fixture.info().value(QStringLiteral("originalLast")).toString(), QStringLiteral("1004"));
    EXPECT_EQ(fixture.info().value(QStringLiteral("missingCount")).toString(), QStringLiteral("1"));
    EXPECT_EQ(fixture.info().value(QStringLiteral("coverageQuality")).toString(), QStringLiteral("validated"));
    // The first evaluated frame is the discovered member, not a missing zero.
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
}

// Two Reads of one media share the reference but not their choices: timing,
// boundary policies and the explicit input transform are node-scoped, and they
// survive save/reopen.
TEST(ReadSourceUiTest, TwoReadsShareMediaWithIndependentChoices) {
    ReadSourceFixture fixture;
    const NodeId second = fixture.addRead("Read2");
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "shared");

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();
    const auto revisionBeforeSecond = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(second),
                                                  QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return fixture.keyOf(second) == key; }));
    EXPECT_EQ(fixture.session_.document().sources.size(), 1u);
    EXPECT_EQ(fixture.session_.revision(), revisionBeforeSecond + 1);

    // Node-scoped choices: the shared reference keeps the media path only.
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("frameOffset"), QStringLiteral("7")},
                                                 {QStringLiteral("frameStep"), QStringLiteral("2")}}));
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("beforePolicy"), QStringLiteral("hold")},
                                                 {QStringLiteral("afterPolicy"), QStringLiteral("black")},
                                                 {QStringLiteral("missingPolicy"), QStringLiteral("black")}}));
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("inputTransform"), QStringLiteral("explicit")},
                                                 {QStringLiteral("inputColorSpace"), QStringLiteral("sRGB")}}));
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("alphaMode"), QStringLiteral("straight")}}));
    const nemo::ReadNodeOverrides first = fixture.overrides(fixture.first_);
    EXPECT_EQ(first.frameOffset, 7);
    EXPECT_EQ(first.frameStep, 2);
    EXPECT_EQ(first.beforePolicy, "hold");
    EXPECT_EQ(first.afterPolicy, "black");
    EXPECT_EQ(first.inputTransform, "explicit");
    EXPECT_EQ(first.inputColorSpace, "sRGB");
    EXPECT_EQ(first.alphaMode, "straight");

    const nemo::ReadNodeOverrides untouched = fixture.overrides(second);
    EXPECT_EQ(untouched.frameOffset, 0);
    EXPECT_EQ(untouched.frameStep, 1);
    EXPECT_EQ(untouched.beforePolicy, "error");
    EXPECT_EQ(untouched.inputTransform, "auto");
    EXPECT_EQ(untouched.alphaMode, "auto");
    // The shared reference is not mutated by a node choice.
    EXPECT_EQ(fixture.session_.document().sources.at(key).frameOffset, 0);
    EXPECT_TRUE(fixture.session_.document().sources.at(key).interpretation.empty());

    // Save/reopen preserves each node's own choices.
    const nlohmann::json saved = nemo::saveDocument(fixture.session_.document());
    const nemo::LoadResult loaded = nemo::loadDocument(saved);
    const nemo::Graph& graph = loaded.document.network(loaded.document.rootNetworkId()).graph();
    const nemo::NodeInstance* read1 = graph.nodeByName("Read1");
    const nemo::NodeInstance* read2 = graph.nodeByName("Read2");
    ASSERT_NE(read1, nullptr);
    ASSERT_NE(read2, nullptr);
    const nemo::ReadNodeOverrides reloadedFirst = nemo::readAuthoredOverrides(loaded.document, *read1);
    const nemo::ReadNodeOverrides reloadedSecond = nemo::readAuthoredOverrides(loaded.document, *read2);
    EXPECT_EQ(reloadedFirst, first);
    EXPECT_EQ(reloadedSecond, untouched);
    EXPECT_EQ(loaded.document.sources.size(), 1u);
}

// Auto/Custom range and Reset to Source, with boundary policies as explicit
// node choices; an invalid edit is rejected and changes nothing.
TEST(ReadSourceUiTest, RangeModeCustomResetAndPoliciesAreNodeChoices) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/range.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    EXPECT_EQ(fixture.info().value(QStringLiteral("selectedFirst")).toString(), QStringLiteral("1001"));

    // Custom range: the selected interval is the artist's own.
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("custom")},
                                                 {QStringLiteral("rangeFirst"), QStringLiteral("1001")},
                                                 {QStringLiteral("rangeLast"), QStringLiteral("1001")}}));
    const QVariantMap custom = fixture.info();
    EXPECT_EQ(custom.value(QStringLiteral("rangeMode")).toString(), QStringLiteral("custom"));
    EXPECT_EQ(custom.value(QStringLiteral("selectedFirst")).toString(), QStringLiteral("1001"));
    EXPECT_EQ(custom.value(QStringLiteral("selectedLast")).toString(), QStringLiteral("1001"));
    // Original Range stays separate from the selected range.
    EXPECT_EQ(custom.value(QStringLiteral("originalLast")).toString(), QStringLiteral("1002"));

    // Reset to Source returns to Auto: the SELECTED interval is the discovered
    // coverage again (the stored Custom endpoints are inactive in Auto — their
    // retained value is incidental and is not what Auto means).
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("auto")}}));
    const QVariantMap reset = fixture.info();
    EXPECT_EQ(reset.value(QStringLiteral("rangeMode")).toString(), QStringLiteral("auto"));
    EXPECT_EQ(reset.value(QStringLiteral("selectedFirst")).toString(), QStringLiteral("1001"));
    EXPECT_EQ(reset.value(QStringLiteral("selectedLast")).toString(), QStringLiteral("1002"));
    EXPECT_TRUE(reset.value(QStringLiteral("error")).toString().isEmpty());

    // An inverted custom range is rejected with the offending relationship and
    // the authored state is unchanged (compared against the state the artist
    // actually authored, never against an incidental stored default).
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("custom")},
                                                 {QStringLiteral("rangeFirst"), QStringLiteral("1001")},
                                                 {QStringLiteral("rangeLast"), QStringLiteral("1001")}}));
    const nemo::ReadNodeOverrides beforeReject = fixture.overrides(fixture.first_);
    const std::uint64_t before = fixture.session_.revision();
    EXPECT_FALSE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("custom")},
                                                  {QStringLiteral("rangeFirst"), QStringLiteral("1010")},
                                                  {QStringLiteral("rangeLast"), QStringLiteral("1000")}}));
    EXPECT_FALSE(fixture.viewer_.error().isEmpty());
    EXPECT_EQ(fixture.session_.revision(), before);
    EXPECT_EQ(fixture.overrides(fixture.first_), beforeReject);

    // A zero Step is rejected; policies are one authored command.
    EXPECT_FALSE(fixture.gesture(fixture.first_, {{QStringLiteral("frameOffset"), QStringLiteral("0")},
                                                  {QStringLiteral("frameStep"), QStringLiteral("0")}}));
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("beforePolicy"), QStringLiteral("black")},
                                                 {QStringLiteral("afterPolicy"), QStringLiteral("hold")},
                                                 {QStringLiteral("missingPolicy"), QStringLiteral("black")}}));
    const nemo::ReadNodeOverrides overrides = fixture.overrides(fixture.first_);
    EXPECT_EQ(overrides.beforePolicy, "black");
    EXPECT_EQ(overrides.afterPolicy, "hold");
    EXPECT_EQ(overrides.missingPolicy, "black");
}

// Offset and Start At are two editors of ONE mapping (core computes it), so
// switching editor without changing the mapping preserves it.
TEST(ReadSourceUiTest, StartAtAndOffsetAreTheSameMapping) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/start.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    EXPECT_EQ(fixture.overrides(fixture.first_).frameOffset, 1001);
    // Start At 0 is exactly the aligned selection.
    EXPECT_EQ(fixture.info().value(QStringLiteral("startAt")).toString(), QStringLiteral("0"));

    // Start At 3 with step 1 and the selected first 1001 -> offset 998, derived
    // by the controller (core's helper) and authored through the shared gesture.
    const QString derived =
        fixture.controller_.startAtOffsetValue(fixture.networkId(), fixture.nodeId(), QStringLiteral("3"));
    ASSERT_FALSE(derived.isEmpty());
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("frameOffset"), derived}}));
    EXPECT_EQ(fixture.overrides(fixture.first_).frameOffset, 998);
    EXPECT_EQ(fixture.info().value(QStringLiteral("startAt")).toString(), QStringLiteral("3"));

    // Offset 1001 again returns to the aligned mapping; no interval is touched.
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("frameOffset"), QStringLiteral("1001")},
                                                 {QStringLiteral("frameStep"), QStringLiteral("1")}}));
    EXPECT_EQ(fixture.info().value(QStringLiteral("startAt")).toString(), QStringLiteral("0"));
}

// An animated Read timing parameter must never be edited as a static override
// shadowed by its channel. The Read editor's consumed rows route such an edit
// through the SHARED parameter gesture (panel.beginEditFor -> updateEdit ->
// commitEdit), which is exactly this owner: with a channel present the keyed
// gesture authors the current-frame key. A cancelled gesture publishes nothing,
// one commit is one undo entry, and resetting the value authors the schema
// default at the current frame without touching the other keys or the curve.
TEST(ReadSourceUiTest, AnimatedReadOffsetEditAuthorsAKeyInsteadOfAShadowedStaticValue) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/anim.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();
    const std::int64_t staticOffset = fixture.overrides(fixture.first_).frameOffset;
    ASSERT_EQ(staticOffset, 1001);

    // The artist has already keyed the offset: one HOLD key at frame 0. An
    // Integer parameter is discrete in the catalog (componentCount == 0), so a
    // key on it is representable exactly with Hold interpolation — the same
    // rule the keyed gesture applies.
    const std::int64_t authoredOffset = fixture.overrides(fixture.first_).frameOffset;
    const nemo::ParameterAddress address{fixture.session_.document().rootNetworkId(), fixture.first_, "frameOffset"};
    ASSERT_TRUE(
        fixture.session_
            .submit(nemo::setKeyframesCommand({{address, nemo::Keyframe{0, 0.0, nemo::ParameterValue{staticOffset},
                                                                        nemo::KeyInterpolation::Hold}}}),
                    {.expectedRevision = fixture.session_.revision()})
            .committed);
    const auto original = *fixture.session_.document().animationChannel(address);
    ASSERT_EQ(original.keys.size(), 1U);

    // A cancelled gesture changes neither the channel nor the document.
    const std::uint64_t revision = fixture.session_.revision();
    const auto cancelled = fixture.session_.beginKeyedParameterGesture(
        5.0, {{address, nemo::ParameterValue{std::int64_t{1099}}}}, {.expectedRevision = revision});
    ASSERT_NE(cancelled.snapshot, nullptr);
    // A CANCELLED gesture publishes nothing by design: the owner returns a
    // result that is not `committed` (no document change, no history entry) and
    // reports failure only through `error` — the same contract the animation
    // owner's own gesture tests assert.
    EXPECT_FALSE(fixture.session_.cancelParameterGesture(cancelled.token).error);
    EXPECT_EQ(*fixture.session_.document().animationChannel(address), original);
    EXPECT_EQ(fixture.session_.revision(), revision);

    // Committing authors a KEY at the current frame; the static parameter, the
    // shared reference and the revision budget stay authoritative.
    const auto begin = fixture.session_.beginKeyedParameterGesture(
        5.0, {{address, nemo::ParameterValue{std::int64_t{1005}}}}, {.expectedRevision = revision});
    ASSERT_NE(begin.snapshot, nullptr);
    EXPECT_EQ(fixture.session_.revision(), revision);  // nothing published before commit
    ASSERT_TRUE(fixture.session_.commitParameterGesture(begin.token, {.expectedRevision = revision}).committed);
    const auto* channel = fixture.session_.document().animationChannel(address);
    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(channel->keys.size(), 2U);
    EXPECT_EQ(nemo::animatedParameterValue(fixture.session_.document(), address, 5.0),
              nemo::ParameterValue{std::int64_t{1005}});
    EXPECT_EQ(nemo::animatedParameterValue(fixture.session_.document(), address, 0.0),
              nemo::ParameterValue{staticOffset});
    // No shadowed static override and no shared-policy write.
    EXPECT_EQ(fixture.overrides(fixture.first_).frameOffset, staticOffset);
    EXPECT_EQ(fixture.session_.document().sources.at(key).frameOffset, 0);

    // One commit is one undo entry and restores the original curve.
    ASSERT_TRUE(fixture.session_.undo({.expectedRevision = fixture.session_.revision()}).committed);
    EXPECT_EQ(*fixture.session_.document().animationChannel(address), original);
    EXPECT_EQ(fixture.overrides(fixture.first_).frameOffset, staticOffset);

    // Reset Value: the schema default at the current frame, other keys intact.
    const auto reset = fixture.session_.beginKeyedParameterGesture(
        5.0, {{address, nemo::ParameterValue{std::int64_t{1001}}}}, {.expectedRevision = fixture.session_.revision()});
    ASSERT_NE(reset.snapshot, nullptr);
    ASSERT_TRUE(fixture.session_.commitParameterGesture(reset.token, {.expectedRevision = fixture.session_.revision()})
                    .committed);
    const auto* resetChannel = fixture.session_.document().animationChannel(address);
    ASSERT_NE(resetChannel, nullptr);
    EXPECT_EQ(resetChannel->keys.size(), 2U);  // the frame-0 key survives the reset
    EXPECT_EQ(nemo::animatedParameterValue(fixture.session_.document(), address, 0.0),
              nemo::ParameterValue{staticOffset});
}

// An ambiguous numbered selection authors nothing and asks for an explicit
// pattern: only the single-image choice is offered, and the message names the
// candidate numberings.
TEST(ReadSourceUiTest, AmbiguousNumberingAsksForAnExplicitPattern) {
    ReadSourceFixture fixture;
    const auto dir = fixture.temporary_.path().toStdString();
    // Both runs have real sibling members: the first selects a column
    // (a.1.1 + a.2.1), the second a row (a.1.1 + a.1.2).
    writeFrame(dir + "/a.#.1.exr", 1, 0.125F);
    writeFrame(dir + "/a.#.1.exr", 2, 0.25F);
    writeFrame(dir + "/a.1.#.exr", 2, 0.5F);

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(dir + "/a.1.1.exr")));
    ASSERT_TRUE(waitFor([&] { return fixture.info().value(QStringLiteral("choiceRequired")).toBool(); }));
    const QVariantMap ambiguous = fixture.info();
    EXPECT_TRUE(ambiguous.value(QStringLiteral("choiceAmbiguous")).toBool());
    // Nothing is authored on a guess, the document is unchanged, and the
    // diagnostic names the offending selection (an actionable target) rather
    // than relying on particular English.
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    const std::uint64_t revision = fixture.session_.revision();
    EXPECT_NE(
        ambiguous.value(QStringLiteral("choiceDetail")).toString().indexOf(QString::fromStdString(dir + "/a.1.1.exr")),
        -1);

    // The sequence choice needs the artist's explicit pattern: it is refused and
    // the document stays unchanged.
    EXPECT_FALSE(fixture.controller_.confirmSourceChoice(fixture.networkId(), fixture.nodeId(), true));
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), revision);

    // The single-image choice binds the literal file as one still.
    ASSERT_TRUE(fixture.controller_.confirmSourceChoice(fixture.networkId(), fixture.nodeId(), false));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    EXPECT_EQ(fixture.overrides(fixture.first_).frameOffset, 0);
    const nemo::MediaCatalogEntry* entry = entryForKey(fixture.session_.document(), fixture.nodeKey());
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->duration, 1);
    EXPECT_FALSE(entry->metadata.committedProbe->firstFrame.has_value());
}

// The Input Transform list and the resolved interpretation come from ONE
// retained input-color generation, held per project color policy and dropped
// (never mutated) at the project boundary. The list is the generation's own
// canonical enumeration: a presentation query never loads the configuration,
// a same-path external edit is observed at reopen/replacement, and a legacy
// non-config-backed project offers no named spaces at all.
TEST(ReadSourceUiTest, InputTransformStateFollowsTheProjectBoundary) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto config = writeInputConfig(directory.path().toStdString() + "/reload.ocio", "cfg_before");
    // A config-backed project: the pinned built-in is embedded in OCIO, so no
    // asset is needed and the working space is a real space of that config.
    nemo::Document document;
    document.color.workingSpace = "Linear Rec.709 (sRGB)";
    ProjectSession session{document};
    session.setColorConfigPath("ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5");
    nemo::media::MediaImportService importer;
    nemo::ui::MediaLibraryModel model{session, importer};
    nemo::ui::NativeFileChooser chooser;
    nemo::ui::ReadSourceController controller{session, model, chooser};

    const QStringList builtin = controller.inputTransformChoices();
    ASSERT_FALSE(builtin.isEmpty());
    EXPECT_TRUE(builtin.contains(QStringLiteral("Linear Rec.709 (sRGB)")));

    // Changing the project's config reference is a boundary: the generation is
    // rebuilt from the new reference.
    session.setColorConfigPath(config.string());
    const QStringList fileConfig = controller.inputTransformChoices();
    EXPECT_TRUE(fileConfig.contains(QStringLiteral("cfg_before")));
    EXPECT_FALSE(fileConfig.contains(QStringLiteral("Linear Rec.709 (sRGB)")));

    // A same-path external edit is NOT polled into the panel.
    writeInputConfig(config, "cfg_after");
    const QStringList beforeBoundary = controller.inputTransformChoices();
    EXPECT_TRUE(beforeBoundary.contains(QStringLiteral("cfg_before")));
    EXPECT_FALSE(beforeBoundary.contains(QStringLiteral("cfg_after")));

    // Reopening/replacing the project is the boundary that retires it: the new
    // project's legacy policy offers no named config spaces (and nothing from
    // the previous project survives).
    ASSERT_TRUE(session.replaceDocument(nemo::Document{}).replaced);
    EXPECT_TRUE(controller.inputTransformChoices().isEmpty());
}

// TWO OCCURRENCES of ONE Read definition (same definition network, same child
// node, different instance ids): each occurrence binds and reloads ITS OWN file,
// the pending/error display is per occurrence, and the definition plus the
// sibling occurrence are never touched. The construction is the one the core
// occurrence tests use.
TEST(ReadSourceUiTest, TwoOccurrencesOfOneReadDefinitionStayIndependent) {
    nemo::Document document;
    const nemo::NetworkId definition = document.addNetwork("ReadNet");
    const nemo::NodeId read = document.network(definition).graph().addNode("source", "Read1");
    const nemo::NetworkInstanceId firstInstance = document.addInstance(document.rootNetworkId(), definition, "A");
    const nemo::NetworkInstanceId secondInstance = document.addInstance(document.rootNetworkId(), definition, "B");
    ReadSourceFixture fixture{std::move(document), definition, read};
    const QString definition_ = fixture.definitionId();
    const QString nodeId = ReadSourceFixture::nodeIdOf(fixture.first_);
    const QString firstScope = QString::number(firstInstance);
    const QString secondScope = QString::number(secondInstance);

    const auto plateA = writeExr(fixture.temporary_.path().toStdString(), "occ-a");
    const auto plateB = writeExr(fixture.temporary_.path().toStdString(), "occ-b");

    // The artist binds a DIFFERENT file on each occurrence.
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(definition_, nodeId, QString::fromStdString(plateA.string()), firstScope));
    ASSERT_TRUE(waitFor([&] {
        return !fixture.controller_.info(definition_, nodeId, 0, firstScope)
                    .value(QStringLiteral("sourceKey"))
                    .toString()
                    .isEmpty();
    }));
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(definition_, nodeId, QString::fromStdString(plateB.string()), secondScope));
    ASSERT_TRUE(waitFor([&] {
        return !fixture.controller_.info(definition_, nodeId, 0, secondScope)
                    .value(QStringLiteral("sourceKey"))
                    .toString()
                    .isEmpty();
    }));

    // Each occurrence reports its OWN binding and path...
    const std::string keyA = fixture.controller_.info(definition_, nodeId, 0, firstScope)
                                 .value(QStringLiteral("sourceKey"))
                                 .toString()
                                 .toStdString();
    const std::string keyB = fixture.controller_.info(definition_, nodeId, 0, secondScope)
                                 .value(QStringLiteral("sourceKey"))
                                 .toString()
                                 .toStdString();
    EXPECT_NE(keyA, keyB);
    EXPECT_EQ(fixture.controller_.info(definition_, nodeId, 0, firstScope).value(QStringLiteral("path")).toString(),
              QString::fromStdString(plateA.string()));
    EXPECT_EQ(fixture.controller_.info(definition_, nodeId, 0, secondScope).value(QStringLiteral("path")).toString(),
              QString::fromStdString(plateB.string()));
    // ...and the DEFINITION still authors nothing.
    const nemo::NodeInstance* definitionRead = fixture.session_.document().network(definition).graph().node(read);
    ASSERT_NE(definitionRead, nullptr);
    EXPECT_FALSE(definitionRead->params.contains(std::string{nemo::kReadParamSourceKey}));
    EXPECT_EQ(fixture.session_.document().sources.size(), 2u);

    // Pending/error display is per occurrence: a rejected selection on A shows
    // there and leaves B reporting neither pending nor an error.
    const std::string missing = fixture.temporary_.path().toStdString() + "/absent.exr";
    ASSERT_TRUE(fixture.controller_.setSourcePath(definition_, nodeId, QString::fromStdString(missing), firstScope));
    ASSERT_TRUE(waitFor([&] {
        return !fixture.controller_.info(definition_, nodeId, 0, firstScope)
                    .value(QStringLiteral("error"))
                    .toString()
                    .isEmpty();
    }));
    const QVariantMap other = fixture.controller_.info(definition_, nodeId, 0, secondScope);
    EXPECT_TRUE(other.value(QStringLiteral("error")).toString().isEmpty());
    EXPECT_FALSE(other.value(QStringLiteral("pending")).toBool());
    EXPECT_EQ(fixture.session_.document().sources.at(keyB).path, plateB.string());  // B unchanged

    // A reload on B repairs B's own current-effective binding only.
    const std::uint64_t revisionA = fixture.session_.document().sources.at(keyA).revision;
    const std::uint64_t revisionB = fixture.session_.document().sources.at(keyB).revision;
    const std::uint64_t before = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.reloadSource(definition_, nodeId, secondScope));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > before; }));
    EXPECT_EQ(fixture.session_.document().sources.at(keyB).revision, revisionB + 1);
    EXPECT_EQ(fixture.session_.document().sources.at(keyA).revision, revisionA);
    // A recorded Sequence/Single choice belongs to ONE occurrence: a
    // confirmation aimed at the SIBLING occurrence of the same child Read is
    // refused, and only the owning scope can confirm it.
    const auto numbered = fixture.temporary_.path().toStdString() + "/chosen.####.exr";
    writeFrame(numbered, 1001, 0.125F);
    writeFrame(numbered, 1002, 0.25F);
    const std::string numberedLiteral = nemo::media::resolveFramePath(numbered, 1001);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(definition_, nodeId, QString::fromStdString(numberedLiteral), firstScope));
    ASSERT_TRUE(waitFor([&] {
        return fixture.controller_.info(definition_, nodeId, 0, firstScope)
            .value(QStringLiteral("choiceRequired"))
            .toBool();
    }));
    // The sibling occurrence cannot steal the choice, and a refusal from that
    // scope does not CONSUME it...
    EXPECT_FALSE(fixture.controller_.confirmSourceChoice(definition_, nodeId, true, secondScope));
    EXPECT_TRUE(fixture.controller_.info(definition_, nodeId, 0, secondScope)
                    .value(QStringLiteral("choiceRequired"))
                    .toBool() == false);
    EXPECT_TRUE(
        fixture.controller_.info(definition_, nodeId, 0, firstScope).value(QStringLiteral("choiceRequired")).toBool());
    // ...nor does an ordinary selection made on that sibling scope discard it.
    const auto siblingPlate = writeExr(fixture.temporary_.path().toStdString(), "occ-c");
    ASSERT_TRUE(fixture.controller_.setSourcePath(definition_, nodeId, QString::fromStdString(siblingPlate.string()),
                                                  secondScope));
    ASSERT_TRUE(waitFor([&] {
        return fixture.controller_.info(definition_, nodeId, 0, secondScope).value(QStringLiteral("path")).toString() ==
               QString::fromStdString(siblingPlate.string());
    }));
    EXPECT_TRUE(
        fixture.controller_.info(definition_, nodeId, 0, firstScope).value(QStringLiteral("choiceRequired")).toBool());
    // ...and the owning scope still can.
    ASSERT_TRUE(fixture.controller_.confirmSourceChoice(definition_, nodeId, true, firstScope));
    ASSERT_TRUE(waitFor([&] {
        const std::string key = fixture.controller_.info(definition_, nodeId, 0, firstScope)
                                    .value(QStringLiteral("sourceKey"))
                                    .toString()
                                    .toStdString();
        return !key.empty() && fixture.session_.document().sources.contains(key);
    }));

    EXPECT_FALSE(fixture.session_.document()
                     .network(definition)
                     .graph()
                     .node(read)
                     ->params.contains(std::string{nemo::kReadParamSourceKey}));
}

// A Read field must show the EFFECTIVE value at the current frame, not the stale
// authored one: the effective-value owner is the shared parameter inspector
// query (which composes animation through core), and the Read editor derives
// every control from it while info() keeps the media facts. The derived Start At
// uses the same effective mapping, so an animated step cannot leave it stale.
TEST(ReadSourceUiTest, ControlsFollowTheEffectiveCurrentFrameValue) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/frame.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    // The artist keys the offset: a HOLD key at frame 0 (1001) and another at
    // frame 10 (1500); the STATIC authored value stays whatever the bind left
    // (captured here, never assumed to be an incidental default).
    const std::int64_t authoredOffset = fixture.overrides(fixture.first_).frameOffset;
    const nemo::ParameterAddress address{fixture.session_.document().rootNetworkId(), fixture.first_, "frameOffset"};
    ASSERT_TRUE(fixture.session_
                    .submit(nemo::setKeyframesCommand(
                                {{address, nemo::Keyframe{0, 0.0, nemo::ParameterValue{std::int64_t{1001}},
                                                          nemo::KeyInterpolation::Hold}},
                                 {address, nemo::Keyframe{0, 10.0, nemo::ParameterValue{std::int64_t{1500}},
                                                          nemo::KeyInterpolation::Hold}}}),
                            {.expectedRevision = fixture.session_.revision()})
                    .committed);

    // Move the shared playhead inside the hold range: the EFFECTIVE value is the
    // frame-0 key while the authored static value is unchanged.
    fixture.viewer_.setFrame(5);
    const QVariantMap inspector = fixture.viewer_.parameterInspector(fixture.networkId(), fixture.nodeId());
    ASSERT_TRUE(inspector.value(QStringLiteral("available")).toBool());
    QString effective;
    for (const QVariant& sectionValue : inspector.value(QStringLiteral("sections")).toList()) {
        for (const QVariant& rowValue : sectionValue.toMap().value(QStringLiteral("parameters")).toList()) {
            const QVariantMap row = rowValue.toMap();
            if (row.value(QStringLiteral("key")).toString() == QLatin1String("frameOffset")) {
                effective = row.value(QStringLiteral("valueText")).toString();
                if (effective.isEmpty())
                    effective = row.value(QStringLiteral("value")).toString();
            }
        }
    }
    EXPECT_EQ(effective, QStringLiteral("1001"));
    EXPECT_EQ(fixture.overrides(fixture.first_).frameOffset, authoredOffset);  // the static value is untouched

    // The derived Start At uses the EFFECTIVE mapping at that frame, so it
    // resolves for the same range the artist sees.
    const QString derived =
        fixture.controller_.startAtOffsetValue(fixture.networkId(), fixture.nodeId(), QStringLiteral("0"), 5);
    EXPECT_FALSE(derived.isEmpty());
}

// Replacing a Read's media (an existing binding) must PRESERVE the artist's
// authored choices — a Custom trim, the Step and the policies — exactly like an
// explicit reload, while a NEW binding returns the range to Auto and aligns the
// mapping to the discovered first frame.
TEST(ReadSourceUiTest, ReplacingMediaPreservesAuthoredTiming) {
    ReadSourceFixture fixture;
    const auto firstPattern = fixture.temporary_.path().toStdString() + "/first.####.exr";
    writeFrame(firstPattern, 1001, 0.125F);
    writeFrame(firstPattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(firstPattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    // The artist authors a Custom trim, a Step and a policy on the binding.
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("custom")},
                                                 {QStringLiteral("rangeFirst"), QStringLiteral("1001")},
                                                 {QStringLiteral("rangeLast"), QStringLiteral("1001")},
                                                 {QStringLiteral("frameStep"), QStringLiteral("2")},
                                                 {QStringLiteral("afterPolicy"), QStringLiteral("hold")}}));
    const nemo::ReadNodeOverrides trimmed = fixture.overrides(fixture.first_);

    // Replace the media with another sequence starting above zero.
    const auto secondPattern = fixture.temporary_.path().toStdString() + "/second.####.exr";
    writeFrame(secondPattern, 2001, 0.5F);
    writeFrame(secondPattern, 2002, 0.75F);
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(secondPattern)));
    ASSERT_TRUE(waitFor([&] {
        const std::string key = fixture.nodeKey();
        return !key.empty() && fixture.session_.document().sources.at(key).path == secondPattern;
    }));
    // Every authored choice survives a replacement, mapping included.
    EXPECT_EQ(fixture.overrides(fixture.first_), trimmed);
    const nemo::MediaCatalogEntry* entry = entryForKey(fixture.session_.document(), fixture.nodeKey());
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->firstFrame, std::optional<std::int64_t>{2001});

    // The same rule for an AUTO Read with an authored Offset/Step: Auto is a
    // range policy only, so the mapping is preserved too (the selected coverage
    // follows the new facts, the artist's offset does not move).
    const NodeId autoRead = fixture.addRead("Read3");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(autoRead),
                                                  QString::fromStdString(firstPattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.keyOf(autoRead).empty(); }));
    ASSERT_TRUE(fixture.gesture(autoRead, {{QStringLiteral("frameOffset"), QStringLiteral("7")},
                                           {QStringLiteral("frameStep"), QStringLiteral("3")}}));
    const nemo::ReadNodeOverrides authoredOffset = fixture.overrides(autoRead);
    ASSERT_EQ(authoredOffset.rangeMode, std::string(nemo::kReadRangeModeAuto));

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(autoRead),
                                                  QString::fromStdString(secondPattern)));
    ASSERT_TRUE(waitFor([&] {
        const std::string key = fixture.keyOf(autoRead);
        return !key.empty() && fixture.session_.document().sources.at(key).path == secondPattern;
    }));
    EXPECT_EQ(fixture.overrides(autoRead), authoredOffset);
    EXPECT_EQ(fixture.overrides(autoRead).frameOffset, 7);
    EXPECT_EQ(fixture.overrides(autoRead).frameStep, 3);
    EXPECT_EQ(fixture.info(autoRead).value(QStringLiteral("selectedFirst")).toString(),
              QStringLiteral("2001"));  // Auto coverage follows the new facts

    // A FIRST bind still returns the range to Auto and aligns the mapping.
    const NodeId fresh = fixture.addRead("Read4");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(fresh),
                                                  QString::fromStdString(secondPattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.keyOf(fresh).empty(); }));
    const nemo::ReadNodeOverrides aligned = fixture.overrides(fresh);
    EXPECT_EQ(aligned.rangeMode, std::string(nemo::kReadRangeModeAuto));
    EXPECT_EQ(aligned.frameOffset, 2001);
    EXPECT_EQ(aligned.frameStep, 1);
}

// An ANIMATED `source` binding (definition scope): the facts follow the CURRENT
// FRAME, the sibling Read and the shared reference are untouched, and a
// Reload/Relink repairs exactly the key the REQUESTED frame resolves — the
// request's own frame is honored, never a hardcoded zero. An unusable
// occurrence identity is refused rather than normalised to the definition.
// (The real two-occurrence case is the separate regression below.)
TEST(ReadSourceUiTest, AnimatedSourceBindingFollowsTheCurrentFrame) {
    ReadSourceFixture fixture;
    const auto first = writeExr(fixture.temporary_.path().toStdString(), "occ-a");
    const auto second = writeExr(fixture.temporary_.path().toStdString(), "occ-b");

    // TWO definition Reads, each bound to its OWN still.
    const NodeId animatedRead = fixture.addRead("Read2");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(first.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string definitionKey = fixture.nodeKey();
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(animatedRead),
                                                  QString::fromStdString(second.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.keyOf(animatedRead).empty(); }));
    const std::string siblingKey = fixture.keyOf(animatedRead);
    EXPECT_NE(definitionKey, siblingKey);

    // Each Read reports its OWN effective binding and path.
    EXPECT_EQ(fixture.info().value(QStringLiteral("sourceKey")).toString(), QString::fromStdString(definitionKey));
    EXPECT_EQ(fixture.info(animatedRead).value(QStringLiteral("sourceKey")).toString(),
              QString::fromStdString(siblingKey));
    EXPECT_EQ(fixture.info().value(QStringLiteral("path")).toString(), QString::fromStdString(first.string()));
    EXPECT_EQ(fixture.info(animatedRead).value(QStringLiteral("path")).toString(),
              QString::fromStdString(second.string()));

    // An animated `source` on one Read: the facts follow the CURRENT FRAME, and
    // the other Read is untouched.
    const nemo::ParameterAddress address{fixture.session_.document().rootNetworkId(), animatedRead,
                                         std::string{nemo::kReadParamSourceKey}};
    ASSERT_TRUE(
        fixture.session_
            .submit(nemo::setKeyframesCommand({{address, nemo::Keyframe{0, 0.0, nemo::ParameterValue{siblingKey},
                                                                        nemo::KeyInterpolation::Hold}},
                                               {address, nemo::Keyframe{0, 10.0, nemo::ParameterValue{definitionKey},
                                                                        nemo::KeyInterpolation::Hold}}}),
                    {.expectedRevision = fixture.session_.revision()})
            .committed);
    const QVariantMap atZero = fixture.info(animatedRead, 0);
    const QVariantMap atTen = fixture.info(animatedRead, 10);
    EXPECT_EQ(atZero.value(QStringLiteral("sourceKey")).toString(), QString::fromStdString(siblingKey));
    EXPECT_EQ(atZero.value(QStringLiteral("path")).toString(), QString::fromStdString(second.string()));
    EXPECT_EQ(atTen.value(QStringLiteral("sourceKey")).toString(), QString::fromStdString(definitionKey));
    EXPECT_EQ(atTen.value(QStringLiteral("path")).toString(), QString::fromStdString(first.string()));
    // The definition Read never changes.
    EXPECT_EQ(fixture.nodeKey(), definitionKey);

    // A reload repairs the SHARED KEY the REQUESTED frame resolves: frame 10
    // repairs the frame-10 binding and leaves the frame-0 binding untouched...
    const std::uint64_t beforeTen = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), ReadSourceFixture::nodeIdOf(animatedRead),
                                                 QVariant{}, 10));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > beforeTen; }));
    EXPECT_EQ(fixture.session_.document().sources.at(definitionKey).revision, 2u);
    EXPECT_EQ(fixture.session_.document().sources.at(siblingKey).revision, 1u);

    // ...and a frame-0 reload repairs that frame's binding instead.
    const std::uint64_t beforeZero = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), ReadSourceFixture::nodeIdOf(animatedRead),
                                                 QVariant{}, 0));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > beforeZero; }));
    EXPECT_EQ(fixture.session_.document().sources.at(siblingKey).revision, 2u);
    EXPECT_EQ(fixture.session_.document().sources.at(definitionKey).revision, 2u);

    // A relink is the same shared repair at the requested frame: the frame-10
    // reference is rewritten in place, the frame-0 reference is untouched.
    const auto relinked = writeExr(fixture.temporary_.path().toStdString(), "relinked");
    ASSERT_TRUE(fixture.controller_.relinkSource(fixture.networkId(), ReadSourceFixture::nodeIdOf(animatedRead),
                                                 QString::fromStdString(relinked.string()), QVariant{}, 10));
    ASSERT_TRUE(
        waitFor([&] { return fixture.session_.document().sources.at(definitionKey).path == relinked.string(); }));
    EXPECT_EQ(fixture.session_.document().sources.at(siblingKey).path, second.string());
    EXPECT_EQ(fixture.info(animatedRead, 10).value(QStringLiteral("path")).toString(),
              QString::fromStdString(relinked.string()));

    // An unusable occurrence identity is refused, never treated as definition.
    EXPECT_TRUE(fixture.info(fixture.first_, 0, QStringLiteral("not-an-occurrence"))
                    .value(QStringLiteral("error"))
                    .toString()
                    .isEmpty() == false);
}

// Reload re-reads the SHARED source: the content revision advances once, the
// discovered facts are refreshed (an appended member appears), and the node's
// authored choices are preserved.
TEST(ReadSourceUiTest, ReloadRefreshesSharedFactsAndKeepsAuthoredChoices) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/reload.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();
    const std::uint64_t revision = fixture.session_.document().sources.at(key).revision;

    // The artist's own trim, which reload must not touch.
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("custom")},
                                                 {QStringLiteral("rangeFirst"), QStringLiteral("1001")},
                                                 {QStringLiteral("rangeLast"), QStringLiteral("1001")}}));

    // The render appends a frame.
    writeFrame(pattern, 1003, 0.5F);
    const std::uint64_t before = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), fixture.nodeId()));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > before; }));

    const SourceReference& reloaded = fixture.session_.document().sources.at(key);
    EXPECT_EQ(reloaded.revision, revision + 1);
    EXPECT_EQ(reloaded.path, pattern);
    const nemo::MediaCatalogEntry* entry = entryForKey(fixture.session_.document(), key);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->metadata.committedProbe.has_value());
    EXPECT_EQ(entry->metadata.committedProbe->lastFrame, std::optional<std::int64_t>{1003});
    EXPECT_EQ(entry->metadata.committedProbe->availableFrameCount, std::optional<std::int64_t>{3});
    // The custom trim and its mode survive.
    const nemo::ReadNodeOverrides overrides = fixture.overrides(fixture.first_);
    EXPECT_EQ(overrides.rangeMode, std::string(nemo::kReadRangeModeCustom));
    EXPECT_EQ(overrides.rangeFirst, 1001);
    EXPECT_EQ(overrides.rangeLast, 1001);
    EXPECT_EQ(fixture.info().value(QStringLiteral("originalLast")).toString(), QStringLiteral("1003"));
}

// Auto range follows the refreshed discovered facts while a Custom trim on a
// sibling Read of the same media is untouched.
TEST(ReadSourceUiTest, ReloadUpdatesAutoRangeButNeverACustomTrim) {
    ReadSourceFixture fixture;
    const NodeId custom = fixture.addRead("Read2");
    const auto pattern = fixture.temporary_.path().toStdString() + "/both.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), ReadSourceFixture::nodeIdOf(custom),
                                                  QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.keyOf(custom).empty(); }));
    ASSERT_TRUE(fixture.gesture(custom, {{QStringLiteral("rangeMode"), QStringLiteral("custom")},
                                         {QStringLiteral("rangeFirst"), QStringLiteral("1002")},
                                         {QStringLiteral("rangeLast"), QStringLiteral("1002")}}));

    writeFrame(pattern, 1005, 0.75F);
    const std::uint64_t before = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), fixture.nodeId()));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > before; }));

    // Auto followed the facts; Custom kept its endpoints.
    EXPECT_EQ(fixture.info().value(QStringLiteral("selectedLast")).toString(), QStringLiteral("1005"));
    const nemo::ReadNodeOverrides customOverrides = fixture.overrides(custom);
    EXPECT_EQ(customOverrides.rangeMode, std::string(nemo::kReadRangeModeCustom));
    EXPECT_EQ(customOverrides.rangeFirst, 1002);
    EXPECT_EQ(customOverrides.rangeLast, 1002);
    EXPECT_EQ(fixture.info(custom).value(QStringLiteral("selectedLast")).toString(), QStringLiteral("1002"));
}

// A newer selection supersedes a pending probe: the earlier result never
// authors, so a stale completion cannot point the node at the wrong media.
TEST(ReadSourceUiTest, SupersededSelectionNeverAuthors) {
    ReadSourceFixture fixture;
    const auto first = writeExr(fixture.temporary_.path().toStdString(), "first");
    const auto second = writeExr(fixture.temporary_.path().toStdString(), "second");

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(first.string())));
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(second.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    EXPECT_EQ(nemo::normalizedSourcePath(fixture.session_.document().sources.at(fixture.nodeKey()).path),
              nemo::normalizedSourcePath(second.string()));
    EXPECT_EQ(fixture.session_.document().sources.size(), 1u);
}

// A repair probe must NOT publish into a target that changed while it ran: the
// Reload freezes the KEY and the shared REFERENCE it was requested for, so a
// binding re-pointed at another source is refused outright, and a reference
// changed underneath is refused by the core command's own expected-reference
// check. Either way the late result changes neither the newer source, the
// history, nor the committed facts. Deterministic without mocks or delays: the
// probe callback is delivered by the event pump, so a synchronous document edit
// made right after the request lands strictly before the completion.
TEST(ReadSourceUiTest, StaleReloadProbeNeverPublishesIntoAChangedTarget) {
    ReadSourceFixture fixture;
    const auto bound = writeExr(fixture.temporary_.path().toStdString(), "probe-bound");
    const auto relocated = writeExr(fixture.temporary_.path().toStdString(), "probe-relocated");
    const auto rebound = writeExr(fixture.temporary_.path().toStdString(), "probe-rebound");
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(bound.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));
    const std::string key = fixture.nodeKey();
    ASSERT_EQ(fixture.session_.document().sources.at(key).revision, 1u);

    // (1) The SHARED REFERENCE moves while the reload probe is in flight.
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), fixture.nodeId()));
    ASSERT_TRUE(fixture.info().value(QStringLiteral("pending")).toBool());
    {
        const nemo::EditResult moved =
            fixture.session_.submit(nemo::relinkReadSourceCommand(key, fixture.session_.document().sources.at(key),
                                                                  relocated.string(), nemo::MediaKind::Unknown, {}),
                                    {.expectedRevision = fixture.session_.revision()});
        ASSERT_TRUE(moved.committed);
    }
    const std::uint64_t afterMove = fixture.session_.revision();
    ASSERT_TRUE(waitFor([&] { return !fixture.info().value(QStringLiteral("pending")).toBool(); }));
    EXPECT_EQ(fixture.session_.document().sources.at(key).path, relocated.string());
    EXPECT_EQ(fixture.session_.document().sources.at(key).revision, 2u);
    EXPECT_EQ(fixture.session_.revision(), afterMove);
    EXPECT_FALSE(fixture.info().value(QStringLiteral("error")).toString().isEmpty());

    // (2) The BINDING moves to another source while the reload probe is in
    // flight: the result names a key this node no longer resolves there.
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), fixture.nodeId()));
    ASSERT_TRUE(fixture.info().value(QStringLiteral("pending")).toBool());
    const auto assigned = std::make_shared<std::string>();
    ASSERT_TRUE(fixture.session_
                    .submit(nemo::registerReadSourceCommand({fixture.session_.document().rootNetworkId(),
                                                             fixture.first_, std::string{nemo::kReadParamSourceKey}},
                                                            0.0, rebound.string(), std::nullopt,
                                                            nemo::MediaKind::Unknown, {}, assigned),
                            {.expectedRevision = fixture.session_.revision()})
                    .committed);
    const std::uint64_t afterRebind = fixture.session_.revision();
    const std::string reboundKey = *assigned;
    ASSERT_NE(reboundKey, key);
    ASSERT_TRUE(waitFor([&] { return !fixture.info().value(QStringLiteral("pending")).toBool(); }));
    EXPECT_EQ(fixture.nodeKey(), reboundKey);
    EXPECT_EQ(fixture.session_.document().sources.at(reboundKey).path, rebound.string());
    EXPECT_EQ(fixture.session_.document().sources.at(reboundKey).revision, 1u);
    EXPECT_EQ(fixture.session_.document().sources.at(key).revision, 2u);
    EXPECT_EQ(fixture.session_.revision(), afterRebind);
    // The stale probe's facts reached neither source.
    const nemo::MediaCatalogEntry* reboundEntry = entryForKey(fixture.session_.document(), reboundKey);
    ASSERT_NE(reboundEntry, nullptr);
    EXPECT_FALSE(reboundEntry->metadata.committedProbe.has_value());
}

// The shipped reload action changes the evaluated pixels and makes an appended
// member resolvable, through the public CPU evaluation seam.
TEST(ReadSourceUiTest, ReloadChangesEvaluatedPixelsAndPicksUpAppendedFrames) {
    ReadSourceFixture fixture;
    const auto pattern = fixture.temporary_.path().toStdString() + "/pixels.####.exr";
    writeFrame(pattern, 1001, 0.125F);
    writeFrame(pattern, 1002, 0.25F);
    ASSERT_TRUE(
        fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(pattern)));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    const nemo::CpuEvaluation head = fixture.evaluate(fixture.first_, 0);
    EXPECT_FLOAT_EQ(head.image.pixel(0, 0)[0], 0.125F);
    const nemo::CpuEvaluation second = fixture.evaluate(fixture.first_, 1);
    EXPECT_FLOAT_EQ(second.image.pixel(0, 0)[0], 0.25F);

    // The render overwrites the mapped frame and appends the next one.
    writeFrame(pattern, 1001, 0.5F);
    writeFrame(pattern, 1003, 0.75F);
    const std::uint64_t before = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.reloadSource(fixture.networkId(), fixture.nodeId()));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > before; }));

    const nemo::CpuEvaluation overwritten = fixture.evaluate(fixture.first_, 0);
    EXPECT_FLOAT_EQ(overwritten.image.pixel(0, 0)[0], 0.5F);
    // The appended frame is inside the refreshed Auto range and now resolves.
    const nemo::CpuEvaluation appended = fixture.evaluate(fixture.first_, 2);
    EXPECT_FLOAT_EQ(appended.image.pixel(0, 0)[0], 0.75F);
}

// A movie is not a still: registering one must keep the container's VALIDATED
// interval, and the Read's effective coverage must be that whole interval. The
// committed facts therefore hold the fixture's generated first/last/count, the
// last generated frame still resolves while the frame after it does not, and
// switching to Custom preserves the initialized interval rather than collapsing
// a multi-frame movie to the zero-width default.
TEST(ReadSourceUiTest, MovieBindingKeepsItsValidatedIntervalAndCustomStartsThere) {
    ReadSourceFixture fixture;
    constexpr int kFrames = 8;
    const auto movie = writeMovie(fixture.temporary_.path().toStdString(), kFrames);
    ASSERT_TRUE(std::filesystem::exists(movie));

    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(),
                                                  QString::fromStdString(movie.string())));
    ASSERT_TRUE(waitFor([&] { return !fixture.nodeKey().empty(); }));

    const QVariantMap bound = fixture.info();
    EXPECT_TRUE(bound.value(QStringLiteral("error")).toString().isEmpty());
    EXPECT_EQ(bound.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    EXPECT_EQ(bound.value(QStringLiteral("kind")).toString(), QStringLiteral("video"));
    // The container's declared interval and count are the fixture's own.
    EXPECT_EQ(bound.value(QStringLiteral("originalFirst")).toString(), QStringLiteral("0"));
    EXPECT_EQ(bound.value(QStringLiteral("originalLast")).toString(), QString::number(kFrames - 1));
    EXPECT_EQ(bound.value(QStringLiteral("originalCount")).toString(), QString::number(kFrames));
    EXPECT_EQ(bound.value(QStringLiteral("frameSpan")).toString(), QString::number(kFrames));
    EXPECT_EQ(bound.value(QStringLiteral("coverageQuality")).toString(), QStringLiteral("validated"));

    // Auto coverage IS that interval: the whole movie is usable, and the frame
    // just past its last generated one already falls outside the range.
    EXPECT_EQ(bound.value(QStringLiteral("selectedFirst")).toString(), QStringLiteral("0"));
    EXPECT_EQ(bound.value(QStringLiteral("selectedLast")).toString(), QString::number(kFrames - 1));
    EXPECT_EQ(fixture.info(fixture.first_, kFrames - 1).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(fixture.info(fixture.first_, kFrames).value(QStringLiteral("status")).toString(),
              QStringLiteral("after-range"));

    // A mode-only gesture uses the source interval seeded during registration.
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("rangeMode"), QStringLiteral("custom")}}));
    const QVariantMap custom = fixture.info();
    EXPECT_EQ(custom.value(QStringLiteral("rangeMode")).toString(), QStringLiteral("custom"));
    // The authored trim is the movie's interval, never a one-frame 0..0 bound.
    EXPECT_EQ(custom.value(QStringLiteral("rangeFirst")).toString(), QStringLiteral("0"));
    EXPECT_EQ(custom.value(QStringLiteral("rangeLast")).toString(), QString::number(kFrames - 1));
    EXPECT_NE(custom.value(QStringLiteral("rangeFirst")).toString(),
              custom.value(QStringLiteral("rangeLast")).toString());
    // The original interval stays reported separately, and the end of the movie
    // is still inside the authored trim.
    EXPECT_EQ(custom.value(QStringLiteral("originalLast")).toString(), QString::number(kFrames - 1));
    EXPECT_EQ(custom.value(QStringLiteral("selectedFirst")).toString(), QStringLiteral("0"));
    EXPECT_EQ(custom.value(QStringLiteral("selectedLast")).toString(), QString::number(kFrames - 1));
    EXPECT_EQ(fixture.info(fixture.first_, kFrames - 1).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(fixture.info(fixture.first_, kFrames).value(QStringLiteral("status")).toString(),
              QStringLiteral("after-range"));
    EXPECT_EQ(fixture.overrides(fixture.first_).rangeMode, std::string(nemo::kReadRangeModeCustom));

    // Relinking to a different media kind updates the facts without resetting
    // the trim; undo restores the movie's kind and interval atomically.
    const auto still = writeExr(fixture.temporary_.path().toStdString(), "replacement");
    const auto revision = fixture.session_.revision();
    ASSERT_TRUE(fixture.controller_.relinkSource(fixture.networkId(), fixture.nodeId(),
                                                 QString::fromStdString(still.string())));
    ASSERT_TRUE(waitFor([&] { return fixture.session_.revision() > revision; }));
    EXPECT_EQ(fixture.info().value(QStringLiteral("kind")).toString(), QStringLiteral("image"));
    EXPECT_EQ(fixture.info().value(QStringLiteral("rangeLast")).toString(), QString::number(kFrames - 1));
    ASSERT_TRUE(fixture.session_.undo({.expectedRevision = fixture.session_.revision()}).committed);
    EXPECT_EQ(fixture.info().value(QStringLiteral("kind")).toString(), QStringLiteral("video"));
    EXPECT_EQ(fixture.info().value(QStringLiteral("originalLast")).toString(), QString::number(kFrames - 1));
}

// A movie that declares NO YUV interpretation cannot be decoded by guessing: the
// first selection of an untagged movie is REFUSED naming the missing
// relationship, and nothing is authored (no binding, no history). The recovery
// is the artist's own: author the missing interpretation through the same shared
// value gesture every other Read choice uses, then explicitly retry the SAME
// selection. Raw/Data bypasses the RGB transfer/gamut transform but never the
// mandatory Y'CbCr matrix/range/chroma decoding, which is exactly why the
// authored set is those three decode fields plus the bypass; the retry then
// binds the movie with its container-validated interval intact, and the shared
// reference every other consumer reads keeps the media path only.
TEST(ReadSourceUiTest, UntaggedMovieRequiresAuthoredInterpretationThenExplicitRetry) {
    ReadSourceFixture fixture;
    constexpr int kFrames = 8;
    const auto movie = writeMovie(fixture.temporary_.path().toStdString(), kFrames, MovieTagging::Untagged);
    ASSERT_TRUE(std::filesystem::exists(movie));
    const std::string path = movie.string();
    const auto baseline = fixture.session_.revision();

    // The untagged stream declares no matrix coefficients, so the probe refuses
    // it naming that relationship and authors nothing.
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(path)));
    ASSERT_TRUE(waitFor([&] { return !fixture.info().value(QStringLiteral("error")).toString().isEmpty(); }));
    const QString rejection = fixture.info().value(QStringLiteral("error")).toString();
    EXPECT_TRUE(rejection.contains(QStringLiteral("matrix"))) << rejection.toStdString();
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), baseline);
    EXPECT_EQ(fixture.info().value(QStringLiteral("state")).toString(), QStringLiteral("empty"));

    // The artist authors the declaration the file does not carry: the Raw/Data
    // bypass for the RGB half plus the three mandatory decode fields, in the
    // schema's own vocabulary (bt709 matrix, limited range, left chroma).
    ASSERT_TRUE(fixture.gesture(fixture.first_, {{QStringLiteral("inputTransform"), QStringLiteral("raw")},
                                                 {QStringLiteral("sourceMatrix"), QStringLiteral("bt709")},
                                                 {QStringLiteral("sourceRange"), QStringLiteral("limited")},
                                                 {QStringLiteral("sourceChromaLocation"), QStringLiteral("left")}}));
    const QVariantMap authored = fixture.info();
    EXPECT_EQ(authored.value(QStringLiteral("inputTransform")).toString(), QStringLiteral("raw"));
    EXPECT_EQ(authored.value(QStringLiteral("sourceMatrix")).toString(), QStringLiteral("bt709"));
    EXPECT_EQ(authored.value(QStringLiteral("sourceRange")).toString(), QStringLiteral("limited"));
    EXPECT_EQ(authored.value(QStringLiteral("sourceChromaLocation")).toString(), QStringLiteral("left"));
    // Authoring the interpretation is not a retry: nothing is bound yet, and
    // the gesture is the only history entry so far.
    EXPECT_TRUE(fixture.nodeKey().empty());
    EXPECT_TRUE(fixture.session_.document().sources.empty());
    EXPECT_EQ(fixture.session_.revision(), baseline + 1);

    // The explicit retry of the SAME path binds: one more history entry, the
    // movie's own validated interval, and the whole interval usable.
    ASSERT_TRUE(fixture.controller_.setSourcePath(fixture.networkId(), fixture.nodeId(), QString::fromStdString(path)));
    ASSERT_TRUE(waitFor([&] { return !fixture.info().value(QStringLiteral("pending")).toBool(); }));
    ASSERT_FALSE(fixture.nodeKey().empty()) << fixture.info().value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(fixture.session_.revision(), baseline + 2);
    const QVariantMap bound = fixture.info();
    EXPECT_TRUE(bound.value(QStringLiteral("error")).toString().isEmpty());
    EXPECT_EQ(bound.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    EXPECT_EQ(bound.value(QStringLiteral("kind")).toString(), QStringLiteral("video"));
    // The authored interpretation survived the bind (the registration never
    // resets a choice the Read already owns).
    EXPECT_EQ(bound.value(QStringLiteral("inputTransform")).toString(), QStringLiteral("raw"));
    EXPECT_EQ(bound.value(QStringLiteral("sourceMatrix")).toString(), QStringLiteral("bt709"));
    EXPECT_EQ(bound.value(QStringLiteral("originalFirst")).toString(), QStringLiteral("0"));
    EXPECT_EQ(bound.value(QStringLiteral("originalLast")).toString(), QString::number(kFrames - 1));
    EXPECT_EQ(bound.value(QStringLiteral("originalCount")).toString(), QString::number(kFrames));
    EXPECT_EQ(bound.value(QStringLiteral("coverageQuality")).toString(), QStringLiteral("validated"));
    EXPECT_EQ(bound.value(QStringLiteral("selectedFirst")).toString(), QStringLiteral("0"));
    EXPECT_EQ(bound.value(QStringLiteral("selectedLast")).toString(), QString::number(kFrames - 1));
    EXPECT_EQ(fixture.info(fixture.first_, kFrames - 1).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(fixture.info(fixture.first_, kFrames).value(QStringLiteral("status")).toString(),
              QStringLiteral("after-range"));
    // The authored interpretation stays the Read's OWN choice: the shared
    // reference is not mutated to carry it.
    EXPECT_TRUE(fixture.session_.document().sources.at(fixture.nodeKey()).interpretation.empty());
}
