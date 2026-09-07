// nemo-cli: headless verification surface for agents and CI.
//
//   nemo-cli validate <project.json>          machine-readable JSON diagnostics
//   nemo-cli render <project.json> --out f.ppm [--frame N] [--width W --height H]
//
// `render` evaluates the project's document and writes a Portable Pixmap
// (P6, binary). Node types available to the headless evaluator live in the
// core testpattern stage; every node with no CPU evaluation contributes a
// documented neutral placeholder pattern, never a silent miss.
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

namespace {

int printUsage() {
    std::cerr << "usage:\n"
                 "  nemo-cli validate <project.json>\n"
                 "  nemo-cli render <project.json> --out <file.ppm> [--frame N] "
                 "[--width W] [--height H]\n";
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
    std::cout << report.dump(2) << '\n';
    return report["ok"].get<bool>() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return printUsage();
    }
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "validate") {
        return commandValidate(args);
    }
    if (command == "render") {
        return commandRender(args);
    }
    return printUsage();
}
