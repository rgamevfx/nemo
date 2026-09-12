#include "nemo/media/ImageIO.hpp"

#include <array>
#include <string_view>

#include <OpenImageIO/imageio.h>

namespace nemo::media {

namespace {

PixelWindow specWindow(const OIIO::ImageSpec& spec) {
    // OIIO reports the data window as origin (x, y) + extent; EXR display
    // windows equal this for formats without an explicit one.
    return PixelWindow{spec.x, spec.y, spec.x + spec.width - 1, spec.y + spec.height - 1};
}

int channelIndex(const OIIO::ImageSpec& spec, std::string_view name) {
    for (std::size_t c = 0; c < spec.channelnames.size(); ++c) {
        if (spec.channelnames[c] == name) {
            return static_cast<int>(c);
        }
    }
    return -1;
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

ImageReadResult readImage(const std::string& path) {
    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        throw ImageIoException(path, OIIO::geterror());
    }

    const OIIO::ImageSpec spec = input->spec();
    ImageReadResult result;
    result.dataWindow = specWindow(spec);
    result.displayWindow = displayWindow(spec);
    result.channelNames.assign(spec.channelnames.begin(), spec.channelnames.end());
    result.nativePrecision = std::string(typeName(spec.format));
    result.formatName = std::string(input->format_name());
    result.declaredColorSpace = spec.get_string_attribute("oiio:ColorSpace");
    result.chromaticities = declaredChromaticities(spec);

    std::vector<float> raw(static_cast<std::size_t>(spec.width) * static_cast<std::size_t>(spec.height) *
                           static_cast<std::size_t>(spec.nchannels));
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, raw.data())) {
        throw ImageIoException(path, OIIO::geterror());
    }
    input.reset();

    const int r = channelIndex(spec, "R");
    const int g = channelIndex(spec, "G");
    const int b = channelIndex(spec, "B");
    const int a = channelIndex(spec, "A");
    if (r < 0 || g < 0 || b < 0) {
        throw ImageIoException(path, "no R/G/B channels in " + result.formatName +
                                         " file: " + std::to_string(spec.nchannels) + " channels");
    }

    ImageLayout layout;
    layout.width = result.displayWindow.width();
    layout.height = result.displayWindow.height();
    layout.pixelAspect = spec.get_float_attribute("pixelaspectratio", 1.0F);
    result.image = CpuImage(layout);

    // Display pixels outside the data window are opaque black (the buffer
    // default is 0 everywhere).
    for (int y = 0; y < layout.height; ++y) {
        for (int x = 0; x < layout.width; ++x) {
            result.image.setPixel(x, y, {0.0F, 0.0F, 0.0F, 1.0F});
        }
    }

    for (int row = result.dataWindow.yMin; row <= result.dataWindow.yMax; ++row) {
        for (int col = result.dataWindow.xMin; col <= result.dataWindow.xMax; ++col) {
            const int x = col - result.displayWindow.xMin;
            const int y = row - result.displayWindow.yMin;
            if (x < 0 || y < 0 || x >= layout.width || y >= layout.height) {
                continue;  // data pixels outside the display extent are not representable here
            }
            const float* src = &raw[(static_cast<std::size_t>(row - spec.y) * static_cast<std::size_t>(spec.width) +
                                     static_cast<std::size_t>(col - spec.x)) *
                                    static_cast<std::size_t>(spec.nchannels)];
            std::array<float, kImageChannels> rgba{src[r], src[g], src[b], 1.0F};
            if (a >= 0) {
                rgba[3] = src[a];
                // Declared association, not converted (issue #6 owns
                // association/color application). EXR's convention is
                // premultiplied when an alpha channel is present; other
                // formats keep straight alpha.
                result.alpha =
                    result.formatName == "openexr" ? AlphaAssociation::Premultiplied : AlphaAssociation::Straight;
            }
            result.image.setPixel(x, y, rgba);
        }
    }
    return result;
}

void writeImage(const std::string& path, const CpuImage& image, const OutputPrecision precision) {
    auto output = OIIO::ImageOutput::create(path);
    if (!output) {
        throw ImageIoException(path, OIIO::geterror());
    }
    OIIO::ImageSpec spec(image.width(), image.height(), kImageChannels, outputType(precision));
    spec.channelnames = {"R", "G", "B", "A"};
    spec.attribute("pixelaspectratio", image.layout().pixelAspect);
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

}  // namespace nemo::media
