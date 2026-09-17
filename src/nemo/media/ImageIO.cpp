#include "nemo/media/ImageIO.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#include <OpenImageIO/imageio.h>

namespace nemo::media {

namespace {

PixelWindow specWindow(const OIIO::ImageSpec& spec) {
    // OIIO reports the data window as origin (x, y) + extent; EXR display
    // windows equal this for formats without an explicit one. The edge is
    // computed in 64-bit because a declared window is file input; the header
    // validation rejects a declaration that does not fit before this runs.
    return PixelWindow{spec.x, spec.y, static_cast<int>(static_cast<std::int64_t>(spec.x) + spec.width - 1),
                       static_cast<int>(static_cast<std::int64_t>(spec.y) + spec.height - 1)};
}

PixelWindow displayWindow(const OIIO::ImageSpec& spec) {
    // OIIO's full_* fields carry the EXR display window (exrinput/exroutput);
    // degenerate extents fall back to the data window.
    if (spec.full_width <= 0 || spec.full_height <= 0) {
        return specWindow(spec);
    }
    return PixelWindow{spec.full_x, spec.full_y, spec.full_x + spec.full_width - 1, spec.full_y + spec.full_height - 1};
}

[[nodiscard]] std::string_view typeName(OIIO::TypeDesc type) {
    switch (type.basetype) {
    case OIIO::TypeDesc::UINT8:
        return "uint8";
    case OIIO::TypeDesc::INT8:
        return "int8";
    case OIIO::TypeDesc::UINT16:
        return "uint16";
    case OIIO::TypeDesc::INT16:
        return "int16";
    case OIIO::TypeDesc::UINT32:
        return "uint32";
    case OIIO::TypeDesc::INT32:
        return "int32";
    case OIIO::TypeDesc::HALF:
        return "half";
    case OIIO::TypeDesc::FLOAT:
        return "float";
    case OIIO::TypeDesc::DOUBLE:
        return "double";
    default:
        return "unknown";
    }
}

OIIO::TypeDesc outputType(OutputPrecision precision) {
    return precision == OutputPrecision::Half ? OIIO::TypeDesc::HALF : OIIO::TypeDesc::FLOAT;
}

// Declared `chromaticities`, if the file declares them in a numeric form:
// 8 values (rx,ry,gx,gy,bx,by,wx,wy). Absent attributes stay empty.
std::vector<float> declaredChromaticities(const OIIO::ImageSpec& spec) {
    const OIIO::TypeDesc type = spec.getattributetype("chromaticities");
    if ((type.basetype != OIIO::TypeDesc::FLOAT && type.basetype != OIIO::TypeDesc::DOUBLE) ||
        type.numelements() != 8) {
        return {};
    }
    std::vector<float> values(8, 0.0F);
    if (!spec.getattribute("chromaticities", OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8), OIIO::make_span(values))) {
        return {};
    }
    return values;
}

// The declared windows of one spec: the data extent OIIO reports, and the
// display extent its full_* fields carry (a degenerate display declaration
// falls back to the data extent). Every coordinate and extent is validated as
// representable first: a declared window is file input, and the geometry this
// adapter normalizes must not overflow — header parsing is where that is
// decided, so nothing downstream ever sees a window that wrapped.
ImageWindows windowsOf(const OIIO::ImageSpec& spec, const std::string& path) {
    const auto representable = [](const std::int64_t value) {
        return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
    };
    const auto edge = [](const std::int64_t origin, const int extent) {
        return origin + static_cast<std::int64_t>(extent) - 1;
    };
    const auto require = [&](const bool fits, const std::string& what) {
        if (!fits) {
            throw ImageIoException(path, "the declared " + what +
                                             " cannot be represented as image coordinates; the header is not "
                                             "usable as image geometry");
        }
    };
    require(representable(edge(spec.x, spec.width)) && representable(edge(spec.y, spec.height)), "data window");
    const bool hasDisplay = spec.full_width > 0 && spec.full_height > 0;
    if (hasDisplay) {
        require(representable(edge(spec.full_x, spec.full_width)) && representable(edge(spec.full_y, spec.full_height)),
                "display window");
        // Normalization translates the data window by the display origin, so that
        // difference must be representable as well.
        require(representable(static_cast<std::int64_t>(spec.x) - spec.full_x) &&
                    representable(static_cast<std::int64_t>(spec.y) - spec.full_y),
                "display-window normalization offset");
    }

    ImageWindows windows;
    windows.data = specWindow(spec);
    windows.display = displayWindow(spec);
    return windows;
}

// Every header fact this adapter reports, from one already-open input. Shared
// by the header-only inspection and the pixel read, so a description and the
// samples it describes are extracted by the same code.
ImageHeader headerOf(const OIIO::ImageInput& input, const std::string& path) {
    const OIIO::ImageSpec& spec = input.spec();
    ImageHeader header;
    header.path = path;
    header.formatName = input.format_name();
    header.declaredColorSpace = spec.get_string_attribute("oiio:ColorSpace");
    header.chromaticities = declaredChromaticities(spec);
    header.channelNames.assign(spec.channelnames.begin(), spec.channelnames.end());
    header.nativePrecision = std::string(typeName(spec.format));
    header.windows = windowsOf(spec, path);
    header.pixelAspect = spec.get_float_attribute("pixelaspectratio", 1.0F);
    // The declared alpha of the image's ROOT layer: the shared channel-name
    // identification owns which spelling names the root alpha, so a multilayer
    // file's auxiliary `layer.A` is never mistaken for the primary alpha that
    // carries the association.
    header.hasAlpha = rgbaChannelIndices(header.channelNames)[3] >= 0;
    return header;
}

// The one delivery inventory of EXR compression names (issue #94). The check
// exists because the EXR writer does not perform it: an unrecognized
// `compression` string silently becomes ZIP, so a caller's authored setting
// would disagree with the file instead of failing. Closed on purpose — a
// second name is added only with verified round-trip evidence, never inherited
// from the library's own list. Exported because a delivery preflight refuses an
// unsupported setting before any file is touched, from this same list.
constexpr std::array<std::string_view, 5> kSupportedExrCompressions{"zip", "piz", "rle", "none", "dwaa"};

// Opens an image input or reports the offending file (never a bare OIIO error).
[[nodiscard]] std::unique_ptr<OIIO::ImageInput> openImage(const std::string& path) {
    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        throw ImageIoException(path, OIIO::geterror());
    }
    return input;
}

}  // namespace

std::string resolveFramePath(const std::string& pattern, std::int64_t frame) {
    auto padded = [](std::int64_t value, std::size_t digits) {
        std::string text = std::to_string(value);
        if (text.size() < digits) {
            text.insert(0, digits - text.size(), '0');
        }
        return text;
    };

    std::string result;
    result.reserve(pattern.size());
    std::size_t index = 0;
    while (index < pattern.size()) {
        const char marker = pattern[index];
        if (marker != '#' && marker != '@') {
            result.push_back(marker);
            ++index;
            continue;
        }
        const std::size_t runStart = index;
        while (index < pattern.size() && pattern[index] == marker) {
            ++index;
        }
        result += padded(frame, index - runStart);
    }
    return result;
}

AlphaAssociation declaredAlphaAssociation(const std::string_view formatName, const bool hasAlpha) {
    if (!hasAlpha) {
        return AlphaAssociation::None;
    }
    return formatName == "openexr" ? AlphaAssociation::Premultiplied : AlphaAssociation::Straight;
}

ImageHeader inspectImageHeader(const std::string& path) {
    const std::unique_ptr<OIIO::ImageInput> input = openImage(path);
    const ImageHeader header = headerOf(*input, path);
    static_cast<void>(input->close());
    return header;
}

ImageReadResult readImage(const std::string& path) {
    const std::unique_ptr<OIIO::ImageInput> input = openImage(path);

    const OIIO::ImageSpec& spec = input->spec();
    ImageReadResult result;
    result.header = headerOf(*input, path);

    const PixelWindow& data = result.header.windows.data;
    const int width = std::max(data.width(), 0);
    const int height = std::max(data.height(), 0);

    // Every channel the file declares is read as authored, in the file's own
    // order: an alpha-only matte, a Z/depth pass and a multilayer render all
    // keep their names and values, and no RGB or alpha is invented for a file
    // that does not declare one. Channel identification (which of these names
    // is the root R/G/B/A) is a resolved-index question for downstream owners,
    // never a read-time rewrite.
    ImageLayout layout;
    // The raster is the data extent, in the file's own pixel order: the display
    // extent is a declared format, not storage (issue #88), so a windowed
    // source is not uploaded as a padded display canvas.
    layout.width = width;
    layout.height = height;
    layout.pixelAspect = result.header.pixelAspect;
    layout.channels = result.header.channelNames;
    result.image = CpuImage(layout);
    const std::size_t channels = result.image.channelCount();
    if (channels == 0) {
        throw ImageIoException(path, "declares no image channels; " + result.header.formatName +
                                         " file has no samples to describe");
    }
    if (width == 0 || height == 0) {
        return result;  // an empty data window is a valid image with no samples
    }

    std::vector<float> raw(static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.height) * channels);
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, raw.data())) {
        throw ImageIoException(path, OIIO::geterror());
    }

    // The contract's raster storage IS this buffer: float32, interleaved in
    // declared channel order, row-major. Samples the file does not declare are
    // never materialized, so off-format coverage stays empty instead of being
    // fabricated as transparent black.
    std::memcpy(result.image.data(), raw.data(), raw.size() * sizeof(float));
    return result;
}

bool isSupportedExrCompression(const std::string_view compression) {
    return std::find(kSupportedExrCompressions.begin(), kSupportedExrCompressions.end(), compression) !=
           kSupportedExrCompressions.end();
}

std::string supportedExrCompressions() {
    std::string list;
    for (const std::string_view name : kSupportedExrCompressions) {
        if (!list.empty()) {
            list += ", ";
        }
        list += name;
    }
    return list;
}

void writeImage(const std::string& path, const CpuImage& image, const ImageWriteOptions& options) {
    // The image's declared channels ARE the file's channels: an alpha-only
    // matte writes one channel named A, a multilayer render writes every
    // auxiliary layer channel it carries, and nothing is padded to four or
    // renamed on the way out. Values are stored verbatim.
    const std::vector<std::string>& channels = image.layout().channels;
    if (channels.empty()) {
        throw ImageIoException(path, "the image declares no channels; there is nothing to write");
    }
    // The compression name is validated before the encoder exists: the EXR
    // writer maps a name it does not know back to its default, so an
    // unsupported setting must be reported here with its own name rather than
    // producing a file whose stored compression silently differs from the
    // authored one (issue #94, story 76/77).
    if (!options.compression.empty() && !isSupportedExrCompression(options.compression)) {
        throw ImageIoException(
            path, "compression '" + options.compression +
                      "' is not a supported EXR compression (supported: " + supportedExrCompressions() + ")");
    }
    auto output = OIIO::ImageOutput::create(path);
    if (!output) {
        throw ImageIoException(path, OIIO::geterror());
    }
    // The raster keeps its authored data-window origin while the display window
    // is the image's own FORMAT when the caller states one, so the single
    // signed offset a reader reports (`ImageReadResult::header.windows`)
    // returns exactly the described geometry — a negative or off-format data
    // window is stored where the description says it is instead of being moved
    // to the origin (issue #88 window contract, issue #94 story 77). A caller
    // that states no format keeps the previous framing: the raster's extent at
    // the logical origin, which is exactly the raster's own data window. The
    // data window is stated on the spec this build's OpenImageIO has: its
    // constructor takes the raster extent, and the window origins are the
    // explicit fields.
    const bool hasFormat = options.formatWidth > 0 && options.formatHeight > 0;
    OIIO::ImageSpec spec(image.width(), image.height(), static_cast<int>(channels.size()),
                         outputType(options.precision));
    spec.x = options.windowX;
    spec.y = options.windowY;
    spec.full_x = hasFormat ? options.formatX : 0;
    spec.full_y = hasFormat ? options.formatY : 0;
    spec.full_width = hasFormat ? options.formatWidth : image.width();
    spec.full_height = hasFormat ? options.formatHeight : image.height();
    spec.channelnames = channels;
    spec.attribute("pixelaspectratio", options.pixelAspect > 0.0F ? options.pixelAspect : image.layout().pixelAspect);
    if (!options.compression.empty()) {
        // Empty keeps the format's own default (never a module-private guess).
        spec.attribute("compression", options.compression);
    }
    if (!output->open(path, spec)) {
        throw ImageIoException(path, OIIO::geterror());
    }
    if (!output->write_image(OIIO::TypeDesc::FLOAT, image.data())) {
        throw ImageIoException(path, OIIO::geterror());
    }
    if (!output->close()) {
        throw ImageIoException(path, OIIO::geterror());
    }
}

void writeImage(const std::string& path, const CpuImage& image, const OutputPrecision precision) {
    // The still write is the delivery write with the format's own default
    // storage settings: one implementation, so a caller that names no
    // compression can never diverge from one that does.
    ImageWriteOptions options;
    options.precision = precision;
    writeImage(path, image, options);
}

}  // namespace nemo::media
