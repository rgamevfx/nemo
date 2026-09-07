#pragma once

// Image source adapter: reads still images and image sequences through
// OpenImageIO/OpenEXR into the application image contract (spec section
// 10.4: dimensions, bounds, pixel aspect, named channels, precision, alpha
// association, color interpretation). This is the shared source/read path
// for CPU-reference and GPU-native consumption; it imposes no CPU-only
// residency contract (GPU interop validation is issue #10).
//
// Pass-through values, explicit metadata: the read path does not apply
// color management or alpha-association conversion (the OCIO viewing
// transform is a separate, explicit viewing operation — ViewingTransform.hpp,
// issue #6). It reports what the file declares so downstream stages can
// honor it explicitly.

#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/evaluation/Image.hpp"

namespace nemo::media {

// Alpha association of the read buffer, per the source's declaration. EXR
// files are premultiplied by convention when an alpha channel is present.
enum class AlphaAssociation { None, Straight, Premultiplied };

// Inclusive pixel window (spec section 10.4 bounds).
struct PixelWindow {
    int xMin{0};
    int yMin{0};
    int xMax{-1};
    int yMax{-1};

    [[nodiscard]] int width() const { return xMax - xMin + 1; }
    [[nodiscard]] int height() const { return yMax - yMin + 1; }
    [[nodiscard]] bool operator==(const PixelWindow&) const = default;
};

// Native storage precision name of the source data (e.g. "half", "float");
// read values are always converted to the contract's float32.
enum class OutputPrecision { Half, Float32 };

// A successful read: RGBA float32 pixels in the application image layout,
// plus everything the contract requires downstream stages to know.
struct ImageReadResult {
    CpuImage image;
    PixelWindow dataWindow;     // pixel data extent, may be smaller
    PixelWindow displayWindow;  // full image extent, defines dimensions
    AlphaAssociation alpha{AlphaAssociation::None};
    std::vector<std::string> channelNames;  // all channels in storage order
    std::string nativePrecision;            // e.g. "half"
    std::string formatName;                 // e.g. "openexr"
};

// Media I/O failures always identify the offending file (repo rule: errors
// identify the offending relationship).
struct ImageIoException : std::runtime_error {
    ImageIoException(std::string path, std::string message)
        : std::runtime_error("image: " + path + ": " + std::move(message)), path(std::move(path)) {}

    std::string path;
};

// Expands an image-sequence pattern to the path of one frame, OpenImageIO
// style: each '#' in a run is one digit of a zero-padded frame number
// ("shot.####.exr" frame 12 -> "shot.0012.exr"), each '@' is one digit
// starting at one ("shot.@.exr" frame 12 -> "shot.12.exr"). Patterns without
// '#' or '@' return the path unchanged (single still).
[[nodiscard]] std::string resolveFramePath(const std::string& pattern, std::int64_t frame);

// Reads one still image (or sequence frame at the resolved path). Throws
// ImageIoException naming `path` when the file is missing, unreadable, or
// of an unsupported contract (non-RGBA mappable layout is still readable:
// missing R/G/B map to 0 and a missing alpha maps to opaque).
//
// No color management is applied (issue #6): values are stored as-is and
// interpreted scene-linear.
[[nodiscard]] ImageReadResult readImage(const std::string& path);

// Writes `image` as a simple RGBA still (EXR by extension). Values are
// stored verbatim with `precision` storage; call sites declare alpha
// association explicitly because EXR readers will assume premultiplied
// when an alpha channel is present. Throws ImageIoException naming `path`.
void writeImage(const std::string& path, const CpuImage& image, OutputPrecision precision);

}  // namespace nemo::media
