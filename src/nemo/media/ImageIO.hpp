#pragma once

// Image source adapter: reads still images and image sequences through
// OpenImageIO/OpenEXR into the application image contract (spec section
// 10.4: dimensions, bounds, pixel aspect, named channels, precision, alpha
// association, color interpretation). This is the shared source/read path
// for CPU-reference and GPU-native consumption; it imposes no CPU-only
// residency contract (GPU interop validation is issue #10).
//
// Windowed images (issue #88): a file declares a data window (the samples it
// stores) and a display window (its format). The read path reports both, moves
// the display window origin to the logical origin 0 and translates the data
// window by the same offset, so negative and off-format samples keep their
// authored coordinates instead of being clamped away. The returned raster is
// exactly the data extent: a declared format is description, not storage, so a
// windowed source is never uploaded as a padded display canvas.
//
// Pass-through values, explicit metadata: this reader does not itself apply
// color management or alpha-association conversion. It reports what the file
// declares (declared color space, chromaticities, alpha association) so the
// source adapter applies the *resolved input color* explicitly and exactly
// once (ImageSource.hpp / InputColor.hpp, issue #81) — an input transform is a
// source operation, never a viewing transform.

#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo::media {

// Alpha association of the read buffer, per the source's declaration. EXR
// files are premultiplied by convention when an alpha channel is present.
enum class AlphaAssociation { None, Straight, Premultiplied };

// The association a reader reports for `formatName` when it has an alpha
// channel: EXR is premultiplied by convention, every other format keeps
// straight alpha. One owner for the rule, so a header-only probe and the read
// itself agree by construction.
[[nodiscard]] AlphaAssociation declaredAlphaAssociation(std::string_view formatName, bool hasAlpha);

// Inclusive pixel window in the file's own coordinates (spec section 10.4
// bounds). The accessors widen before they subtract: a declared window is file
// input, and `inspectImageHeader` rejects a declaration whose coordinates or
// extents cannot be represented, so the narrowed result is always defined.
struct PixelWindow {
    int xMin{0};
    int yMin{0};
    int xMax{-1};
    int yMax{-1};

    [[nodiscard]] int width() const { return static_cast<int>(static_cast<std::int64_t>(xMax) - xMin + 1); }
    [[nodiscard]] int height() const { return static_cast<int>(static_cast<std::int64_t>(yMax) - yMin + 1); }
    [[nodiscard]] bool operator==(const PixelWindow&) const = default;
};

// The windows one image declares (issue #88): the data extent (the samples the
// file stores) and the display extent (the image format), both in the file's
// own coordinates.
struct ImageWindows {
    PixelWindow data;
    PixelWindow display;

    [[nodiscard]] bool operator==(const ImageWindows&) const = default;

    // The normalized geometry contract: the display window origin becomes the
    // logical origin 0 and the data window is translated by the same offset.
    // `format` is the display extent at origin 0; `dataBounds` is the
    // translated, half-open data extent — possibly negative, possibly outside
    // the format, empty when the file declares no samples. A consumer reads the
    // raster through `dataBounds` and never assumes it starts at the format
    // origin. Both subtract in 64-bit and narrow once: the header inspection
    // that produced these windows has already rejected a declaration whose
    // normalized coordinates or extents cannot be represented.
    [[nodiscard]] Region format() const {
        return Region{0, 0, std::max(display.width(), 0), std::max(display.height(), 0)};
    }
    [[nodiscard]] Region dataBounds() const {
        const int width = data.width();
        const int height = data.height();
        if (width <= 0 || height <= 0) {
            return Region{};
        }
        return Region{static_cast<int>(static_cast<std::int64_t>(data.xMin) - display.xMin),
                      static_cast<int>(static_cast<std::int64_t>(data.yMin) - display.yMin), width, height};
    }
};

// Header-only facts of one image (issue #88): everything the adapter reports
// about a file without loading a plane. This is the single owner of the
// declared geometry normalization and of the declared color metadata, consumed
// by the pixel read and by the metadata probe alike, so a description never
// costs a decode and can never disagree with the samples it describes.
struct ImageHeader {
    std::string path;
    std::string formatName;                 // e.g. "openexr"
    std::string declaredColorSpace;         // OpenImageIO `oiio:ColorSpace`, "" when undeclared
    std::vector<float> chromaticities;      // 8 values (rx,ry,gx,gy,bx,by,wx,wy) when declared, else empty
    std::vector<std::string> channelNames;  // all channels in storage order
    std::string nativePrecision;            // e.g. "half"
    ImageWindows windows;                   // declared data/display windows
    float pixelAspect{1.0F};
    bool hasAlpha{false};
};

// Inspects one image's header. No plane is loaded and nothing is decoded, so an
// unavailable geometry is diagnosed from the header rather than discovered by
// rendering. Throws ImageIoException naming `path` when the file is missing,
// unreadable, or not image data.
[[nodiscard]] ImageHeader inspectImageHeader(const std::string& path);

// Native storage precision name of the source data (e.g. "half", "float");
// read values are always converted to the contract's float32.
enum class OutputPrecision { Half, Float32 };

// A successful read: RGBA float32 pixels in the application image layout,
// plus everything the contract requires downstream stages to know.
struct ImageReadResult {
    // The data raster. It covers exactly the declared data extent: pixel (i, j)
    // holds the sample authored at normalized full-resolution coordinate
    // (header.windows.dataBounds().x + i, .y + j). Samples the file does not
    // declare are transparent black, and the display window is deliberately
    // *not* padded into the raster — framing is the description's job.
    CpuImage image;
    ImageHeader header;
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
