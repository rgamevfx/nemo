// nemo-cli: headless verification surface for agents and CI.
//
//   nemo-cli validate <project.json>          machine-readable JSON diagnostics
//   nemo-cli evaluate <project.json> --out f.ppm [--frame N] [--width W --height H]
//           [--output NAME] [--network-id ID]
//   nemo-cli render <project.json> --out f.ppm [--frame N] [--width W --height H]
//   nemo-cli deliver <project.nemo> --write <node> [--format exr|mov|mp4]
//           [--profile 422|4444|4444xq] [--fps N] [--bitrate-kbps N]
//           [--color-mode raw|project|colorspace|display] [--output-transform NAME]
//           [--lut FILE] [--out PATH] [--first N --last N] [--preflight]
//
// `evaluate` walks the selected document network topologically and renders its
// Output node from the CPU reference inventory (issue #1). When omitted,
// `--network-id` explicitly resolves to the document root network. It emits
// JSON diagnostics on stdout and writes a Portable Pixmap (P6, binary); the
// PPM bytes are the scene-linear reference values clamped to [0, 1] -- no
// viewing transform is applied (spec section 8). `render` is the older
// single-node pattern writer kept for the CI smoke test.
//
// `deliver` (issue #94) consumes the same native eval::DeliveryQueue as the
// desktop. It preserves source decode selection and evaluates the frozen
// document at full quality; output color and encoding are export-only.
// `--preflight` reports the output plan without writing. A job that does not
// finalize every requested frame/container exits with status 1.
#include "ProjectSessionCommand.hpp"
#include "ViewerCacheCommand.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/media/CodecSweep.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/Probe.hpp"
#include "nemo/media/VideoDecode.hpp"
#ifdef NEMO_BUILD_GPU
#include "nemo/eval/DeliveryJob.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#ifndef NEMO_SLANG_SRC_DIR
#define NEMO_SLANG_SRC_DIR ""
#endif
#else
#define NEMO_BUILD_GPU_UNUSED 0
#endif
namespace {

[[nodiscard]] nemo::NetworkId parseNetworkId(const std::string& text) {
    if (text.empty() || text.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument("--network-id: expected a nonzero decimal integer without whitespace");
    nemo::NetworkId value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == nemo::kInvalidNetwork)
        throw std::invalid_argument("--network-id: expected a nonzero network id");
    return value;
}

// All CLI project readers go through the shared file owner so project-relative
// source and OCIO paths resolve identically and unknown/missing-dependency
// diagnostics are preserved. `report` must already own a "warnings" array.
[[nodiscard]] nemo::ProjectReadResult readProject(const std::string& path, nlohmann::json& report) {
    nemo::ProjectReadResult loaded = nemo::ProjectFile::read(path);
    for (const auto& warning : loaded.warnings)
        report["warnings"].push_back(warning);
    if (!loaded.ok) {
        report["errors"].push_back(loaded.error.message.empty() ? std::string{"cannot open file: " + path}
                                                                : loaded.error.message);
    }
    return loaded;
}

int printUsage() {
    std::cerr << "usage:\n"
                 "  nemo-cli validate <project.json>\n"
                 "  nemo-cli evaluate <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H] [--output NAME] [--network-id ID]\n"
                 "  nemo-cli render <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H]\n"
                 "  nemo-cli deliver <project.nemo> --write <node> [--network-id ID] [--frame N]\n"
                 "          [--out PATH] [--format exr|mov|mp4] [--overwrite]\n"
                 "          [--precision half|float] [--compression zip|piz|rle|none|dwaa]\n"
                 "          [--profile 422|4444|4444xq] [--fps N] [--bitrate-kbps N]\n"
                 "          [--color-mode raw|project|colorspace|display] [--output-transform NAME] [--lut FILE]\n"
                 "          [--first N] [--last N] [--offset N] [--shaders DIR] [--preflight]\n"
                 "  nemo-cli project-session <project.json>  JSON-lines edit/query session\n"
                 "  nemo-cli probe-media [project.json]      hardware codec capability report\n"
                 "  nemo-cli codec-sweep <tagged-viewer-clip> [--codecs a,b] [--chunks a,b] [--max-frames N]\n"
                 "          [--width W --height H] [--profile NAME] [--bit-depth N] [--bitrate-kbps N]\n"
#ifdef NEMO_BUILD_GPU
                 "  nemo-cli cache-viewer <project.json> --cache-dir PATH --frames 1,2,3\n"
                 "          [--network-id ID] [--replay forward|reverse|random] [--width W --height H --scale 1|2|4]\n"
                 "          [--codec ID --chunk-frames N --bitrate-kbps N --shaders DIR]\n"
                 "          [--fidelity] [--stale-supersede]\n"
                 "          [--view-after DISPLAY/VIEW] [--edit-node NAME --edit-key KEY --edit-value VALUE]\n"
                 "  nemo-cli evaluate-gpu <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H] [--output NAME] [--network-id ID]\n"
                 "          [--backend slang|glsl] [--shaders <spv-dir>]\n"
#endif
                 "\n";
    return 2;
}

int commandValidate(const std::vector<std::string>& args) {
    if (args.empty()) {
        return printUsage();
    }
    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}};
    try {
        const nemo::ProjectReadResult loaded = readProject(args.front(), report);
        if (loaded.ok) {
            report["ok"] = true;
            nlohmann::json info;
            info["name"] = loaded.document.name;
            const auto& rootGraph = loaded.document.network(loaded.document.rootNetworkId()).graph();
            info["network"] = loaded.document.rootNetworkId();
            info["nodes"] = rootGraph.nodes().size();
            info["edges"] = rootGraph.edges().size();
            report["document"] = std::move(info);
        }
    } catch (const std::exception& e) {
        report["errors"].push_back(std::string{"error: "} + e.what());
    }
    std::cout << report.dump(2) << '\n';
    return report["ok"].get<bool>() ? 0 : 1;
}

struct Rgb {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

// Deterministic headless pattern: horizontal red gradient, vertical green
// gradient, and a blue bar whose position tracks the requested frame. Any
// change to this function is an observable image change.
Rgb testPatternPixel(int x, int y, int width, int height, int frame) {
    const double u = width > 1 ? static_cast<double>(x) / (width - 1) : 0.0;
    const double v = height > 1 ? static_cast<double>(y) / (height - 1) : 0.0;
    const int barWidth = std::max(2, width / 16);
    const int barPos = (frame * (width / 8)) % (width + barWidth);
    const bool inBar = x >= barPos && x < barPos + barWidth;
    return {static_cast<std::uint8_t>(u * 255.0), static_cast<std::uint8_t>(v * 255.0),
            static_cast<std::uint8_t>(inBar ? 255 : 0)};
}

void writePpm(std::ostream& out, int width, int height, int frame) {
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Rgb pixel = testPatternPixel(x, y, width, height, frame);
            out.put(static_cast<char>(pixel.r));
            out.put(static_cast<char>(pixel.g));
            out.put(static_cast<char>(pixel.b));
        }
    }
}

int commandRender(const std::vector<std::string>& args) {
    if (args.empty()) {
        return printUsage();
    }
    std::string outPath;
    int frame = 0;
    int width = 64;
    int height = 64;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size()) {
            std::cerr << "missing value for " << flag << '\n';
            return 2;
        }
        const std::string& value = args[i + 1];
        if (flag == "--out") {
            outPath = value;
        } else if (flag == "--frame") {
            frame = std::stoi(value);
        } else if (flag == "--width") {
            width = std::stoi(value);
        } else if (flag == "--height") {
            height = std::stoi(value);
        } else {
            std::cerr << "unknown flag " << flag << '\n';
            return 2;
        }
    }
    if (outPath.empty() || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        std::cerr << "render requires --out and 1..8192 dimensions\n";
        return 2;
    }

    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}};
    try {
        const nemo::ProjectReadResult loaded = readProject(args.front(), report);
        if (loaded.ok) {
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                report["errors"].push_back("cannot write: " + outPath);
            } else {
                writePpm(out, width, height, frame);
                report["ok"] = true;
                report["rendered"] = {{"path", std::filesystem::absolute(outPath).string()},
                                      {"width", width},
                                      {"height", height},
                                      {"frame", frame}};
            }
        }
    } catch (const std::exception& e) {
        report["errors"].push_back(std::string{"error: "} + e.what());
    }
    return report["ok"].get<bool>() ? 0 : 1;
}

// Scene-linear reference bytes: clamp to [0, 1] and scale; no viewing
// transform (spec section 8, issue #1 non-goal).
void writeCpuPpm(std::ostream& out, const nemo::CpuImage& image) {
    out << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixel(x, y);
            for (std::size_t c = 0; c < 3; ++c) {
                const float clamped = std::clamp(pixel[c], 0.0F, 1.0F);
                out.put(static_cast<char>(static_cast<int>(clamped * 255.0F + 0.5F)));
            }
        }
    }
}

int commandEvaluate(const std::vector<std::string>& args) {
    if (args.empty()) {
        return printUsage();
    }
    std::string outPath;
    std::string outputName;
    nemo::NetworkId network = nemo::kInvalidNetwork;
    int frame = 0;
    int width = 64;
    int height = 64;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size()) {
            std::cerr << "missing value for " << flag << '\n';
            return 2;
        }
        const std::string& value = args[i + 1];
        if (flag == "--out") {
            outPath = value;
        } else if (flag == "--frame") {
            frame = std::stoi(value);
        } else if (flag == "--width") {
            width = std::stoi(value);
        } else if (flag == "--height") {
            height = std::stoi(value);
        } else if (flag == "--output") {
            outputName = value;
        } else if (flag == "--network-id") {
            network = parseNetworkId(value);
        } else {
            std::cerr << "unknown flag " << flag << '\n';
            return 2;
        }
    }
    if (outPath.empty() || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        std::cerr << "evaluate requires --out and 1..8192 dimensions\n";
        return 2;
    }

    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}};
    try {
        const nemo::ProjectReadResult loaded = readProject(args.front(), report);
        if (loaded.ok) {
            nemo::EvaluationRequest request;
            request.network = network == nemo::kInvalidNetwork ? loaded.document.rootNetworkId() : network;
            request.output = nemo::resolveOutput(loaded.document, request.network, outputName);
            request.localTime = frame;
            request.region = {0, 0, width, height};
            nemo::media::ImageSourceProvider sources;
            const nemo::CpuEvaluation evaluation = nemo::evaluateCpu(loaded.document, request, nullptr, &sources);

            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                report["errors"].push_back("cannot write: " + outPath);
            } else {
                writeCpuPpm(out, evaluation.image);
                report["ok"] = true;
                report["rendered"] = {{"path", std::filesystem::absolute(outPath).string()},
                                      {"network", request.network},
                                      {"width", width},
                                      {"height", height},
                                      {"frame", frame}};
                report["evaluation"] = nemo::planToJson(evaluation.plan);
            }
        }
    } catch (const nemo::EvaluationException& e) {
        report["errors"].push_back(e.what());
    } catch (const std::exception& e) {
        report["errors"].push_back(std::string{"error: "} + e.what());
    }
    std::cout << report.dump(2) << '\n';
    return report["ok"].get<bool>() ? 0 : 1;
}

#ifdef NEMO_BUILD_GPU
// UI and CLI report the settings frozen by the same delivery owner (issue #94).
[[nodiscard]] nlohmann::json deliverySettingsToJson(const nemo::eval::DeliverySettings& settings) {
    const auto& output = settings.output;
    return {{"file", settings.file},
            {"fileType", output.fileType},
            {"createDirectories", settings.createDirectories},
            {"overwrite", settings.overwrite},
            {"frameFirst", settings.frameFirst},
            {"frameLast", settings.frameLast},
            {"frameOffset", settings.frameOffset},
            {"precision", output.precision == nemo::media::OutputPrecision::Half ? "half" : "float"},
            {"compression", output.compression},
            {"profile", output.profile},
            {"frameRate", output.frameRate},
            {"bitrateKbps", output.bitrateKbps},
            {"colorMode", output.colorMode},
            {"outputTransform", output.outputTransform},
            {"lutFile", output.lutFile}};
}

[[nodiscard]] nlohmann::json deliveryFramesToJson(const std::vector<nemo::eval::DeliveryFrame>& frames) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& frame : frames)
        result.push_back(
            {{"documentFrame", frame.documentFrame}, {"fileFrame", frame.fileFrame}, {"path", frame.path}});
    return result;
}

[[nodiscard]] nlohmann::json deliveryFileToJson(const nemo::eval::DeliveryFileResult& file) {
    return {{"documentFrame", file.documentFrame},
            {"fileFrame", file.fileFrame},
            {"path", file.path},
            {"written", file.written},
            {"error", file.error}};
}
#endif

int commandDeliver(const std::vector<std::string>& args) {
#ifndef NEMO_BUILD_GPU
    (void)args;
    std::cerr << "deliver requires the native GPU evaluation build; no CPU export fallback is used\n";
    return 2;
#else
    if (args.empty())
        return printUsage();
    struct Request {
        std::string node;
        nemo::NetworkId network{nemo::kInvalidNetwork};
        std::int64_t frame{0};
        std::optional<std::string> out, format, precision, compression, profile, colorMode, outputTransform, lut;
        std::optional<std::int64_t> first, last, offset;
        std::optional<double> frameRate;
        std::optional<int> bitrateKbps;
        std::string shaders;
        bool overwrite{false};
        bool preflight{false};
    } request;
    try {
        const auto integer = [](const std::string& text, const std::string& option) {
            std::int64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw std::invalid_argument(option + ": expected an integer, got '" + text + "'");
            return value;
        };
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& flag = args[i];
            if (flag == "--overwrite") {
                request.overwrite = true;
                continue;
            }
            if (flag == "--preflight") {
                request.preflight = true;
                continue;
            }
            if (i + 1 >= args.size())
                throw std::invalid_argument("missing value for " + flag);
            const std::string& value = args[++i];
            if (flag == "--write")
                request.node = value;
            else if (flag == "--network-id")
                request.network = parseNetworkId(value);
            else if (flag == "--frame")
                request.frame = integer(value, flag);
            else if (flag == "--out")
                request.out = value;
            else if (flag == "--format")
                request.format = value;
            else if (flag == "--precision") {
                if (value != "half" && value != "float")
                    throw std::invalid_argument("--precision requires half or float");
                request.precision = value;
            } else if (flag == "--compression")
                request.compression = value;
            else if (flag == "--profile")
                request.profile = value;
            else if (flag == "--color-mode")
                request.colorMode = value;
            else if (flag == "--output-transform")
                request.outputTransform = value;
            else if (flag == "--lut")
                request.lut = value;
            else if (flag == "--first")
                request.first = integer(value, flag);
            else if (flag == "--last")
                request.last = integer(value, flag);
            else if (flag == "--offset")
                request.offset = integer(value, flag);
            else if (flag == "--shaders")
                request.shaders = value;
            else if (flag == "--fps") {
                double fps = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), fps);
                if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(fps) || fps <= 0)
                    throw std::invalid_argument("--fps requires a finite positive frame rate");
                request.frameRate = fps;
            } else if (flag == "--bitrate-kbps") {
                const auto bitrate = integer(value, flag);
                if (bitrate <= 0 || bitrate > std::numeric_limits<int>::max())
                    throw std::invalid_argument("--bitrate-kbps is outside the supported positive integer range");
                request.bitrateKbps = static_cast<int>(bitrate);
            } else
                throw std::invalid_argument("unknown flag " + flag);
        }
        if (request.node.empty())
            throw std::invalid_argument("deliver requires --write <node>");
    } catch (const std::exception& error) {
        std::cerr << "deliver: " << error.what() << '\n';
        return 2;
    }
#ifdef NEMO_SLANG_SPV_DIR
    if (request.shaders.empty())
        request.shaders = NEMO_SLANG_SPV_DIR;
#endif
    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}};
    try {
        const auto loaded = readProject(args.front(), report);
        if (loaded.ok) {
            const auto network =
                request.network == nemo::kInvalidNetwork ? loaded.document.rootNetworkId() : request.network;
            const auto* write = loaded.document.network(network).graph().nodeByName(request.node);
            if (!write)
                throw std::invalid_argument("network " + std::to_string(network) + " has no Write node named '" +
                                            request.node + "'");
            auto settings = nemo::eval::deliverySettings(loaded.document, network, write->id, request.frame);
            if (request.out)
                settings.file = *request.out;
            if (request.overwrite)
                settings.overwrite = true;
            auto& output = settings.output;
            if (request.format)
                output.fileType = *request.format;
            if (request.precision)
                output.precision = *request.precision == "half" ? nemo::media::OutputPrecision::Half
                                                                : nemo::media::OutputPrecision::Float32;
            if (request.compression)
                output.compression = *request.compression;
            if (request.profile)
                output.profile = *request.profile;
            if (request.frameRate)
                output.frameRate = *request.frameRate;
            if (request.bitrateKbps)
                output.bitrateKbps = *request.bitrateKbps;
            if (request.colorMode)
                output.colorMode = *request.colorMode;
            if (request.outputTransform)
                output.outputTransform = *request.outputTransform;
            if (request.lut)
                output.lutFile = *request.lut;
            if (request.first)
                settings.frameFirst = *request.first;
            if (request.last)
                settings.frameLast = *request.last;
            if (request.offset)
                settings.frameOffset = *request.offset;
            if (request.shaders.empty())
                throw std::invalid_argument("deliver needs compiled Slang shaders; pass --shaders DIR");

            // The headless counterpart of the desktop's injected native owners.
            // Delivery borrows them; it never constructs a private device.
            auto instance = nemo::gpu::Instance::create({.validation = true});
            auto device = nemo::gpu::Device::create(*instance);
            auto allocator = nemo::gpu::Allocator::create(*instance, *device, {.max_device_bytes = 2ULL << 30});
            nemo::eval::DeliveryQueue queue(*instance, *device, *allocator, request.shaders);
            if (request.preflight) {
                const auto plan =
                    queue.plan(loaded.document, network, write->id, settings, request.frame, loaded.colorConfigPath);
                auto planJson = deliverySettingsToJson(plan.settings);
                planJson["frames"] = deliveryFramesToJson(plan.frames);
                planJson["collisions"] = plan.collisions;
                planJson["problem"] = plan.problem;
                planJson["width"] = plan.width;
                planJson["height"] = plan.height;
                planJson["channels"] = plan.channels;
                report["plan"] = std::move(planJson);
                report["ok"] = plan.ok();
                if (!plan.problem.empty())
                    report["errors"].push_back(plan.problem);
                if (!plan.settings.overwrite)
                    for (const auto& collision : plan.collisions)
                        report["errors"].push_back("refusing to overwrite '" + collision +
                                                   "' (--overwrite authorizes it)");
            } else {
                const auto id =
                    queue.submit(loaded.document, network, write->id, settings, request.frame, loaded.colorConfigPath);
                queue.waitForIdle();
                const auto job = queue.status(id);
                auto jobJson = deliverySettingsToJson(job.settings);
                jobJson["id"] = job.id;
                jobJson["state"] = nemo::eval::deliveryStateName(job.state);
                jobJson["network"] = job.network;
                jobJson["node"] = job.node;
                jobJson["nodeName"] = job.nodeName;
                jobJson["totalFrames"] = job.totalFrames;
                jobJson["writtenFrames"] = job.writtenFrames;
                jobJson["failedFrames"] = job.failedFrames;
                jobJson["progress"] = job.progress();
                jobJson["width"] = job.width;
                jobJson["height"] = job.height;
                jobJson["channels"] = job.channels;
                jobJson["fullQuality"] = job.fullQuality;
                jobJson["execution"] = "native";
                jobJson["nativeStaging"] = job.nativeStaging;
                jobJson["stagingBytes"] = job.stagingBytes;
                jobJson["movie"] = job.movie;
                jobJson["error"] = job.error;
                auto files = nlohmann::json::array();
                for (const auto& file : job.files)
                    files.push_back(deliveryFileToJson(file));
                jobJson["files"] = std::move(files);
                report["job"] = std::move(jobJson);
                report["ok"] = job.state == nemo::eval::DeliveryState::Completed;
                if (!job.error.empty())
                    report["errors"].push_back(job.error);
                for (const auto& file : job.files)
                    if (!file.written && !file.error.empty())
                        report["errors"].push_back(file.error);
            }
        }
    } catch (const std::exception& error) {
        report["errors"].push_back(std::string("deliver: ") + error.what());
    }
    std::cout << report.dump(2) << '\n';
    return report["ok"].get<bool>() ? 0 : 1;
#endif
}

// Image source probe: reports the image contract for one still or sequence
// frame as machine-readable JSON diagnostics (issue #4 acceptance: a
// missing path surfaces the offending file in `errors`).
#ifdef NEMO_BUILD_GPU
// Headless GPU execution smoke scenario (issue #8): evaluates the document
// through the native Vulkan effect path (GPU-resident intermediates, no
// routine readback), then performs ONE declared diagnostic readback of the
// requested output for the machine-readable report and the PPM. The
// readback is the verification seam; the executor path itself is
// readback-free.
int commandEvaluateGpu(const std::vector<std::string>& args) {
    if (args.empty()) {
        return printUsage();
    }
    std::string outPath;
    std::string outputName;
    nemo::NetworkId network = nemo::kInvalidNetwork;
    std::string shaderDir;
    std::string backend = "slang";
    int frame = 0;
    int width = 64;
    int height = 64;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size()) {
            std::cerr << "missing value for " << flag << '\n';
            return 2;
        }
        const std::string& value = args[i + 1];
        if (flag == "--out") {
            outPath = value;
        } else if (flag == "--frame") {
            frame = std::stoi(value);
        } else if (flag == "--width") {
            width = std::stoi(value);
        } else if (flag == "--height") {
            height = std::stoi(value);
        } else if (flag == "--output") {
            outputName = value;
        } else if (flag == "--network-id") {
            network = parseNetworkId(value);
        } else if (flag == "--shaders") {
            shaderDir = value;
        } else if (flag == "--backend") {
            backend = value;
        } else {
            std::cerr << "unknown flag " << flag << '\n';
            return 2;
        }
    }
    if (outPath.empty() || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        std::cerr << "evaluate-gpu requires --out and 1..8192 dimensions\n";
        return 2;
    }
    if (backend != "slang" && backend != "glsl") {
        std::cerr << "unknown backend '" << backend << "' (supported: slang, glsl)\n";
        return 2;
    }
    if (backend == "slang" && shaderDir.empty()) {
#ifdef NEMO_SLANG_SPV_DIR
        shaderDir = NEMO_SLANG_SPV_DIR;
#endif
    }

    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}};
    try {
        const nemo::ProjectReadResult loaded = readProject(args.front(), report);
        if (loaded.ok) {
            nemo::EvaluationRequest request;
            request.network = network == nemo::kInvalidNetwork ? loaded.document.rootNetworkId() : network;
            request.output = nemo::resolveOutput(loaded.document, request.network, outputName);
            request.localTime = frame;
            request.region = {0, 0, width, height};

            auto instance = nemo::gpu::Instance::create({.validation = true});
            auto device = nemo::gpu::Device::create(*instance);
            auto allocator = nemo::gpu::Allocator::create(*instance, *device, {.max_device_bytes = 256u << 20});

            nemo::eval::EffectLibrary effects;
            if (backend == "slang") {
                if (shaderDir.empty()) {
                    report["errors"].push_back("no Slang shader directory: pass --shaders <spv-dir> or configure with "
                                               "-D NEMO_DOWNLOAD_SLANGC=ON / -D NEMO_SLANGC=<path>");
                } else {
                    effects = nemo::eval::loadSlangEffectLibrary(shaderDir, NEMO_SLANG_SRC_DIR);
                }
            } else {
                effects = nemo::eval::glslEffectLibrary();
            }
            if (report["errors"].empty()) {
                std::filesystem::path mediaShaderDir = shaderDir;
#ifdef NEMO_SLANG_SPV_DIR
                if (mediaShaderDir.empty())
                    mediaShaderDir = NEMO_SLANG_SPV_DIR;
#endif
                // The authored project color config drives the session's OCIO
                // decode, and its content identity participates in the plan
                // keys so an edited config can never reuse a stale decode.
                nemo::eval::SourceSession sources(*instance, *device, *allocator, mediaShaderDir / "mediaConvert.spv",
                                                  loaded.colorConfigPath);
                nemo::eval::GpuEvaluation evaluation =
                    nemo::eval::evaluateGpu(loaded.document, request, effects, *device, *allocator, 10'000'000'000ULL,
                                            nullptr, &sources, sources.colorConfigIdentity());

                // Declared diagnostic-only readback of the requested output.
                const nemo::CpuImage image = evaluation.readBack(request.output, *device, *allocator);

                std::ofstream out(outPath, std::ios::binary);
                if (!out) {
                    report["errors"].push_back("cannot write: " + outPath);
                } else {
                    writeCpuPpm(out, image);
                    report["ok"] = true;
                    // Report what the produced image actually is, through the
                    // shared identity encoder: a Raw/Data source result is not
                    // managed scene-linear (issue #81).
                    const nlohmann::json produced = nemo::imageIdentityToJson(evaluation.plan.result);
                    report["rendered"] = {
                        {"path", std::filesystem::absolute(outPath).string()},
                        {"network", request.network},
                        {"width", width},
                        {"height", height},
                        {"frame", frame},
                        {"backend", backend},
                        {"device", device->properties().deviceName},
                        {"precision", image.layout().precision == nemo::Precision::Float32 ? "float32" : "unknown"},
                        {"color", produced.at("color")}};
                    report["readback"] = {
                        {"network", request.network},
                        {"node", request.output},
                        {"note", "diagnostic-only; the executor path itself performs no host readback"}};
                    report["evaluation"] = nemo::planToJson(evaluation.plan);
                }
            }
        }
    } catch (const nemo::EvaluationException& e) {
        report["errors"].push_back(e.what());
    } catch (const nemo::gpu::GpuException& e) {
        report["errors"].push_back(std::string{"gpu: "} + e.what());
    } catch (const std::exception& e) {
        report["errors"].push_back(std::string{"error: "} + e.what());
    }
    std::cout << report.dump(2) << '\n';
    return report["ok"].get<bool>() ? 0 : 1;
}
#endif  // NEMO_BUILD_GPU

// Image source probe: reports the image contract for one still or sequence
// frame as machine-readable JSON diagnostics (issue #4 acceptance: a
// missing path surfaces the offending file in `errors`).
int commandImageInfo(const std::vector<std::string>& args) {
    if (args.empty()) {
        return printUsage();
    }
    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}};
    try {
        std::int64_t frame = 0;
        for (std::size_t i = 1; i < args.size(); i += 2) {
            if (args[i] == "--frame" && i + 1 < args.size()) {
                frame = std::stoll(args[i + 1]);
            } else {
                std::cerr << "unknown flag " << args[i] << '\n';
                return printUsage();
            }
        }
        const std::string path = nemo::media::resolveFramePath(args.front(), frame);
        const nemo::media::ImageReadResult read = nemo::media::readImage(path);
        const nemo::media::ImageHeader& header = read.header;
        const nemo::Region format = header.windows.format();
        const nemo::Region dataBounds = header.windows.dataBounds();
        report["ok"] = true;
        // Geometry is reported as the adapter describes it: the format at the
        // normalized origin 0, the signed data bounds the samples occupy, and
        // the raster actually read (issue #88). A consumer that needs the file's
        // own window offsets never has to infer them from the raster.
        const auto alpha = nemo::media::declaredAlphaAssociation(header.formatName, header.hasAlpha);
        report["image"] = {
            {"path", path},
            {"format", header.formatName},
            {"width", format.width},
            {"height", format.height},
            {"pixel_aspect", header.pixelAspect},
            {"channels", header.channelNames},
            {"precision", header.nativePrecision},
            {"alpha", alpha == nemo::media::AlphaAssociation::Premultiplied ? "premultiplied"
                      : alpha == nemo::media::AlphaAssociation::Straight    ? "straight"
                                                                            : "none"},
            {"data_bounds", {dataBounds.x, dataBounds.y, dataBounds.width, dataBounds.height}},
            {"raster", {read.image.width(), read.image.height()}},
            {"data_window",
             {header.windows.data.xMin, header.windows.data.yMin, header.windows.data.xMax, header.windows.data.yMax}},
            {"display_window",
             {header.windows.display.xMin, header.windows.display.yMin, header.windows.display.xMax,
              header.windows.display.yMax}}};
    } catch (const nemo::media::ImageIoException& e) {
        report["errors"].push_back(e.what());
    } catch (const std::exception& e) {
        report["errors"].push_back(std::string{"error: "} + e.what());
    }
    std::cout << report.dump(2) << '\n';
    return report["ok"].get<bool>() ? 0 : 1;
}

// probe-media: measured hardware decode/encode capability report
// (issue #10). With the GPU build, Vulkan video queue evidence comes from
// the real device; without it, claims are registered-only by definition.
int commandProbeMedia(const std::vector<std::string>& args) {
    static_cast<void>(args);
#ifdef NEMO_BUILD_GPU
    std::unique_ptr<nemo::gpu::Instance> instance;
    std::unique_ptr<nemo::gpu::Device> device;
    try {
        instance = nemo::gpu::Instance::create();
        device = nemo::gpu::Device::create(*instance);
    } catch (const nemo::gpu::GpuException&) {
        device.reset();  // no usable device: probe reports registered-only claims
    }
    const nemo::media::MediaCapabilities capabilities = nemo::media::probeMediaCapabilities(device.get());
    std::cout << nemo::media::formatMediaCapabilities(capabilities);
    return 0;
#else
    const nemo::media::MediaCapabilities capabilities = nemo::media::probeMediaCapabilities(nullptr);
    std::cout << nemo::media::formatMediaCapabilities(capabilities);
    return 0;
#endif
}

// Tagged display-referred reference preparation, not source/viewer integration.
int commandCodecSweep(const std::vector<std::string>& args) {
    if (args.empty())
        return printUsage();
    nemo::media::SweepOptions options;
    const auto positive = [](const std::string& text, const std::string& option) {
        int64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value <= 0 ||
            value > std::numeric_limits<int>::max())
            throw std::invalid_argument(option + ": expected integer in 1..2147483647, got '" + text + "'");
        return static_cast<int>(value);
    };
    const auto list = [](const std::string& text, const std::string& option) {
        std::vector<std::string> result;
        size_t start = 0;
        do {
            const auto end = text.find(',', start);
            auto token = text.substr(start, end == std::string::npos ? end : end - start);
            if (token.empty() || token.find_first_of(" \t\n\r") != std::string::npos)
                throw std::invalid_argument(option + ": expected nonempty comma-separated values without whitespace");
            result.push_back(std::move(token));
            if (end == std::string::npos)
                break;
            start = end + 1;
        } while (true);
        return result;
    };
    for (size_t i = 1; i < args.size(); i += 2) {
        const auto& option = args[i];
        if (i + 1 == args.size())
            throw std::invalid_argument(option + ": missing value");
        const auto& value = args[i + 1];
        if (option == "--codecs") {
            options.codecs = list(value, option);
        } else if (option == "--chunks") {
            options.chunkSizes.clear();
            for (const auto& token : list(value, option))
                options.chunkSizes.push_back(positive(token, option));
        } else if (option == "--max-frames") {
            options.maxFrames = positive(value, option);
        } else if (option == "--width") {
            options.width = positive(value, option);
        } else if (option == "--height") {
            options.height = positive(value, option);
        } else if (option == "--bitrate-kbps") {
            options.bitrateKbps = positive(value, option);
        } else if (option == "--bit-depth") {
            options.bitDepth = positive(value, option);
        } else if (option == "--profile") {
            if (value.empty())
                throw std::invalid_argument(option + ": empty profile");
            options.profile = value;
        } else {
            throw std::invalid_argument("unknown option " + option);
        }
    }
    const auto report = nemo::media::runCodecSweep(args[0], options);
    std::cout << report.table();
    return std::ranges::any_of(report.entries, [](const auto& entry) { return entry.measurements.has_value(); }) ? 0
                                                                                                                 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return printUsage();
    }
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "cache-viewer") {
        return commandViewerCache(args);
    }
    if (command == "probe-media") {
        return commandProbeMedia(args);
    }
    if (command == "project-session") {
        return commandProjectSession(args);
    }
    if (argc < 3) {
        return printUsage();
    }
    if (command == "validate") {
        return commandValidate(args);
    }
    if (command == "render") {
        return commandRender(args);
    }
    if (command == "evaluate") {
        return commandEvaluate(args);
    }
    if (command == "deliver") {
        return commandDeliver(args);
    }
#ifdef NEMO_BUILD_GPU
    if (command == "evaluate-gpu") {
        return commandEvaluateGpu(args);
    }
#endif
    if (command == "imageinfo") {
        return commandImageInfo(args);
    }
    if (command == "codec-sweep") {
        try {
            return commandCodecSweep(args);
        } catch (const std::exception& error) {
            std::cerr << "codec-sweep: " << error.what() << "\n";
            return 1;
        }
    }
    return printUsage();
}
