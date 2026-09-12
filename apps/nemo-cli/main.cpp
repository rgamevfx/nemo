// nemo-cli: headless verification surface for agents and CI.
//
//   nemo-cli validate <project.json>          machine-readable JSON diagnostics
//   nemo-cli evaluate <project.json> --out f.ppm [--frame N] [--width W --height H]
//           [--output NAME] [--network-id ID]
//   nemo-cli render <project.json> --out f.ppm [--frame N] [--width W --height H]
//
// `evaluate` walks the selected document network topologically and renders its
// Output node from the CPU reference inventory (issue #1). When omitted,
// `--network-id` explicitly resolves to the document root network. It emits
// JSON diagnostics on stdout and writes a Portable Pixmap (P6, binary); the
// PPM bytes are the scene-linear reference values clamped to [0, 1] -- no
// viewing transform is applied (spec section 8). `render` is the older
// single-node pattern writer kept for the CI smoke test.
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
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/media/CodecSweep.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ImageSource.hpp"
#include "nemo/media/Probe.hpp"
#include "nemo/media/VideoDecode.hpp"
#ifdef NEMO_BUILD_GPU
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

int printUsage() {
    std::cerr << "usage:\n"
                 "  nemo-cli validate <project.json>\n"
                 "  nemo-cli evaluate <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H] [--output NAME] [--network-id ID]\n"
                 "  nemo-cli render <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H]\n"
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
        std::ifstream in(args.front());
        if (!in) {
            report["errors"].push_back("cannot open file: " + args.front());
        } else {
            const auto parsed = nlohmann::json::parse(in);
            auto loaded = nemo::loadDocument(parsed);
            report["ok"] = true;
            report["warnings"] = loaded.warnings;
            nlohmann::json info;
            info["name"] = loaded.document.name;
            const auto& rootGraph = loaded.document.network(loaded.document.rootNetworkId()).graph();
            info["network"] = loaded.document.rootNetworkId();
            info["nodes"] = rootGraph.nodes().size();
            info["edges"] = rootGraph.edges().size();
            report["document"] = std::move(info);
        }
    } catch (const nemo::DeserializeError& e) {
        report["errors"].push_back(std::string{"deserialize: "} + e.what());
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

    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}};
    try {
        std::ifstream in(args.front());
        if (!in) {
            report["errors"].push_back("cannot open file: " + args.front());
        } else {
            const auto loaded = nemo::loadDocument(nlohmann::json::parse(in));
            report["warnings"] = loaded.warnings;
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

    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}};
    try {
        std::ifstream in(args.front());
        if (!in) {
            report["errors"].push_back("cannot open file: " + args.front());
        } else {
            const auto loaded = nemo::loadDocument(nlohmann::json::parse(in));
            report["warnings"] = loaded.warnings;

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

    nlohmann::json report{{"ok", false}, {"errors", nlohmann::json::array()}};
    try {
        std::ifstream in(args.front());
        if (!in) {
            report["errors"].push_back("cannot open file: " + args.front());
        } else {
            const auto loaded = nemo::loadDocument(nlohmann::json::parse(in));
            report["warnings"] = loaded.warnings;

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
                nemo::eval::SourceSession sources(*instance, *device, *allocator, mediaShaderDir / "mediaConvert.spv");
                nemo::eval::GpuEvaluation evaluation = nemo::eval::evaluateGpu(
                    loaded.document, request, effects, *device, *allocator, 10'000'000'000ULL, nullptr, &sources);

                // Declared diagnostic-only readback of the requested output.
                const nemo::CpuImage image = evaluation.readBack(request.output, *device, *allocator);

                std::ofstream out(outPath, std::ios::binary);
                if (!out) {
                    report["errors"].push_back("cannot write: " + outPath);
                } else {
                    writeCpuPpm(out, image);
                    report["ok"] = true;
                    report["rendered"] = {
                        {"path", std::filesystem::absolute(outPath).string()},
                        {"network", request.network},
                        {"width", width},
                        {"height", height},
                        {"frame", frame},
                        {"backend", backend},
                        {"device", device->properties().deviceName},
                        {"precision", image.layout().precision == nemo::Precision::Float32 ? "float32" : "unknown"},
                        {"color", "scene-linear"}};
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
        report["ok"] = true;
        report["image"] = {
            {"path", path},
            {"format", read.formatName},
            {"width", read.image.width()},
            {"height", read.image.height()},
            {"pixel_aspect", read.image.layout().pixelAspect},
            {"channels", read.channelNames},
            {"precision", read.nativePrecision},
            {"alpha", read.alpha == nemo::media::AlphaAssociation::Premultiplied ? "premultiplied"
                      : read.alpha == nemo::media::AlphaAssociation::Straight    ? "straight"
                                                                                 : "none"},
            {"data_window", {read.dataWindow.xMin, read.dataWindow.yMin, read.dataWindow.xMax, read.dataWindow.yMax}},
            {"display_window",
             {read.displayWindow.xMin, read.displayWindow.yMin, read.displayWindow.xMax, read.displayWindow.yMax}}};
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
