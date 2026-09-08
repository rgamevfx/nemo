// nemo-cli: headless verification surface for agents and CI.
//
//   nemo-cli validate <project.json>          machine-readable JSON diagnostics
//   nemo-cli evaluate <project.json> --out f.ppm [--frame N] [--width W --height H]
//           [--output NAME]
//   nemo-cli render <project.json> --out f.ppm [--frame N] [--width W --height H]
//
// `evaluate` walks the document graph topologically and renders the Output
// node from the CPU reference inventory (issue #1). It emits JSON
// diagnostics on stdout and writes a Portable Pixmap (P6, binary); the PPM
// bytes are the scene-linear reference values clamped to [0, 1] -- no
// viewing transform is applied (spec section 8). `render` is the older
// single-node pattern writer kept for the CI smoke test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/media/CodecSweep.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/Probe.hpp"
#include "nemo/media/VideoDecode.hpp"
#ifdef NEMO_BUILD_GPU
#include "nemo/eval/GpuExecutor.hpp"
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

int printUsage() {
    std::cerr << "usage:\n"
                 "  nemo-cli validate <project.json>\n"
                 "  nemo-cli evaluate <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H] [--output NAME]\n"
                 "  nemo-cli render <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H]\n"
                 "  nemo-cli imageinfo <image> [--frame N]\n"
                 "  nemo-cli probe-media [project.json]      hardware codec capability report\n"
                 "  nemo-cli codec-sweep <clip> [--codecs a,b] [--chunks a,b] [--max-frames N]\n"
#ifdef NEMO_BUILD_GPU
                 "  nemo-cli evaluate-gpu <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H] [--output NAME]\n"
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
            info["nodes"] = loaded.document.graph.nodes().size();
            info["edges"] = loaded.document.graph.edges().size();
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
            request.output = nemo::resolveOutput(loaded.document, outputName);
            request.localTime = frame;
            request.region = {0, 0, width, height};
            const nemo::CpuEvaluation evaluation = nemo::evaluateCpu(loaded.document, request);

            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                report["errors"].push_back("cannot write: " + outPath);
            } else {
                writeCpuPpm(out, evaluation.image);
                report["ok"] = true;
                report["rendered"] = {{"path", std::filesystem::absolute(outPath).string()},
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
            request.output = nemo::resolveOutput(loaded.document, outputName);
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
                nemo::eval::GpuEvaluation evaluation =
                    nemo::eval::evaluateGpu(loaded.document, request, effects, *device, *allocator);

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
                        {"width", width},
                        {"height", height},
                        {"frame", frame},
                        {"backend", backend},
                        {"device", device->properties().deviceName},
                        {"precision", image.layout().precision == nemo::Precision::Float32 ? "float32" : "unknown"},
                        {"color", "scene-linear"}};
                    report["readback"] = {
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

#ifdef NEMO_BUILD_GPU
std::filesystem::path shaderSpvDir() {
#ifdef NEMO_SLANG_SPV_DIR
    return NEMO_SLANG_SPV_DIR;
#else
    return {};
#endif
}
#endif

// codec-sweep: the codec/chunk experiment harness (issue #10 acceptance
// example 2). Decodes the source clip on the software reference path,
// re-encodes it with each candidate codec at each chunk size as
// independently decodable chunks, and prints the measured table.
int commandCodecSweep(const std::vector<std::string>& args) {
    if (args.empty()) {
        return printUsage();
    }
    const std::string clipPath = args[0];
    std::vector<std::string> codecs = {"h264-nvenc", "hevc-nvenc", "libx264-cpu", "libx265-cpu"};
    std::vector<int> chunks = {12, 24, 48};
    int64_t maxFrames = 96;
    for (size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--codecs") {
            codecs.clear();
            std::string value = args[i + 1];
            value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
            std::string token;
            std::istringstream tokens(value);
            while (std::getline(tokens, token, ',')) {
                if (!token.empty()) {
                    codecs.push_back(token);
                }
            }
        } else if (args[i] == "--chunks") {
            chunks.clear();
            std::string value = args[i + 1];
            value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
            std::string chunkToken;
            std::istringstream chunkTokens(value);
            while (std::getline(chunkTokens, chunkToken, ',')) {
                if (!chunkToken.empty()) {
                    chunks.push_back(std::stoi(chunkToken));
                }
            }
        } else if (args[i] == "--max-frames") {
            maxFrames = std::stoll(args[i + 1]);
        } else {
            std::cerr << "codec-sweep: unknown option " << args[i] << "\n";
            return printUsage();
        }
    }
    nemo::media::SoftwareClip source = nemo::media::decodeClipSoftware(clipPath, maxFrames);
    if (source.frames.empty()) {
        std::cerr << "codec-sweep: cannot decode " << clipPath << "\n";
        return 1;
    }
    std::cout << "source: " << clipPath << " (" << source.info.width << "x" << source.info.height << ", "
              << source.frames.size() << " frames)\n";
    const nemo::media::SweepReport report = nemo::media::runCodecSweep(source.frames, codecs, chunks);
    std::cout << report.table();

#ifdef NEMO_BUILD_GPU
    // Hardware decode + interop cost, measured on the real device path
    // (acceptance example 3: capability-dependent transfers exposed).
    try {
        auto instance = nemo::gpu::Instance::create();
        auto device = nemo::gpu::Device::create(*instance);
        auto allocator = nemo::gpu::Allocator::create(*instance, *device, {.max_device_bytes = 1 << 30});
        const std::filesystem::path spv = shaderSpvDir();
        auto decoder = nemo::media::ClipDecoder::open(*instance, *device, *allocator, clipPath,
                                                      spv / "mediaConvert.spv");
        if (decoder->decision().hardware) {
            const auto decodeStart = std::chrono::steady_clock::now();
            int hardwareFrames = 0;
            while (decoder->next(1'000'000'000ULL) != nullptr) {
                ++hardwareFrames;
            }
            const double decodeNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decodeStart)
                    .count();
            std::cout << "hw-decode (" << decoder->info().codecName
                      << "-vulkan, device-resident, no CPU readback): "
                      << (hardwareFrames > 0 ? decodeNs / 1e6 / hardwareFrames : 0.0) << " ms/frame over "
                      << hardwareFrames << " frames\n";
        } else {
            std::cout << "hw-decode unavailable: " << decoder->decision().reason << "\n";
        }
    } catch (const nemo::gpu::GpuException& error) {
        std::cout << "hw-decode unavailable (no device): " << error.what() << "\n";
    }
#endif
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return printUsage();
    }
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "probe-media") {
        return commandProbeMedia(args);
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
        return commandCodecSweep(args);
    }
    return printUsage();
}
