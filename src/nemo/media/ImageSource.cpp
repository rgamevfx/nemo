#include "nemo/media/ImageSource.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "nemo/media/ImageIO.hpp"

namespace nemo::media {

namespace {

// Rec.709 / sRGB primaries with D65 white: the document working space.
constexpr std::array<float, 8> kRec709Chromaticities{0.64F, 0.33F, 0.30F, 0.60F, 0.15F, 0.06F, 0.3127F, 0.3290F};
constexpr float kChromaticityTolerance = 1e-4F;

[[noreturn]] void fail(const std::string& path, const std::string& message) {
    throw ImageIoException(path, message);
}

// Mirror the clip path's strict field errors: "(context: ...; ...)".
[[nodiscard]] std::string note(const std::string& context, const std::string& details) {
    return " (context: " + (context.empty() ? std::string{"image source"} : context) + "; " + details + ")";
}

[[nodiscard]] bool hasPattern(const std::string& path) {
    return path.find('#') != std::string::npos || path.find('@') != std::string::npos;
}

[[nodiscard]] std::vector<std::string> splitTokens(const std::string& declared) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : declared) {
        if (character == '_') {
            tokens.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    tokens.push_back(current);
    return tokens;
}

[[nodiscard]] ImageTransfer transferFromDeclared(const std::string& token, const std::string& declared,
                                                 const std::string& path, const std::string& context) {
    static const std::map<std::string, ImageTransfer> byName{{"lin", ImageTransfer::Linear},
                                                             {"srgb", ImageTransfer::Srgb},
                                                             {"g22", ImageTransfer::Gamma22},
                                                             {"g28", ImageTransfer::Gamma28},
                                                             {"bt709", ImageTransfer::Bt709}};
    const auto it = byName.find(token);
    if (it == byName.end()) {
        fail(path, "declared color space '" + declared + "' has unsupported transfer '" + token + "'" +
                       note(context, "supported: lin, srgb, g22, g28, bt709"));
    }
    return it->second;
}

[[nodiscard]] ImagePrimaries primariesFromDeclared(const std::string& token, const std::string& declared,
                                                   const std::string& path, const std::string& context) {
    if (token != "rec709") {
        fail(path, "declared color space '" + declared + "' has unsupported primaries '" + token + "'" +
                       note(context, "supported: rec709 (the working space is scene-linear Rec.709)"));
    }
    return ImagePrimaries::Rec709;
}

// Strict parsing of the reference's interpretation map. Only the two fields
// an RGB image source can honor are accepted; Y'CbCr-only fields are
// rejected explicitly instead of being silently ignored.
[[nodiscard]] ImageTransfer parseTransferOverride(const std::string& value, const std::string& path,
                                                  const std::string& context) {
    static const std::map<std::string, ImageTransfer> byName{{"linear", ImageTransfer::Linear},
                                                             {"srgb", ImageTransfer::Srgb},
                                                             {"gamma22", ImageTransfer::Gamma22},
                                                             {"gamma28", ImageTransfer::Gamma28},
                                                             {"bt709", ImageTransfer::Bt709}};
    const auto it = byName.find(value);
    if (it == byName.end()) {
        fail(path, "source interpretation field 'transfer' has unsupported value '" + value + "'" +
                       note(context, "supported: linear, srgb, gamma22, gamma28, bt709"));
    }
    return it->second;
}

[[nodiscard]] ImagePrimaries parsePrimariesOverride(const std::string& value, const std::string& path,
                                                    const std::string& context) {
    if (value != "bt709") {
        fail(path, "source interpretation field 'primaries' has unsupported value '" + value + "'" +
                       note(context, "supported: bt709"));
    }
    return ImagePrimaries::Rec709;
}

// Declared `chromaticities`, if the file declares them: 8 values
// (rx,ry,gx,gy,bx,by,wx,wy). Absent attributes stay empty.
[[nodiscard]] std::vector<float> declaredChromaticities(const OIIO::ImageSpec& spec) {
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

struct Interpretation {
    ImageTransfer transfer{ImageTransfer::Linear};
    ImagePrimaries primaries{ImagePrimaries::Rec709};
};

// Resolve what the source's RGB samples mean. Precedence: the file's
// declared color space wins, the EXR format default covers a file that
// declares nothing, and the reference's interpretation map fills only the
// fields still unspecified. Ambiguous or unsupported metadata is an error
// naming the file — never guessed.
[[nodiscard]] Interpretation resolveInterpretation(const std::string& path, const std::string& formatName,
                                                   const std::string& declaredColorSpace,
                                                   const std::vector<float>& chromaticities,
                                                   const std::map<std::string, std::string>& overrides,
                                                   const std::string& context) {
    std::optional<ImageTransfer> transfer;
    std::optional<ImagePrimaries> primaries;
    if (!declaredColorSpace.empty()) {
        const std::vector<std::string> tokens = splitTokens(declaredColorSpace);
        transfer = transferFromDeclared(tokens.front(), declaredColorSpace, path, context);
        if (tokens.size() < 2 || tokens[1].empty()) {
            fail(path, "declared color space '" + declaredColorSpace + "' does not declare primaries" +
                           note(context, "supported: rec709 (the working space is scene-linear Rec.709)"));
        }
        primaries = primariesFromDeclared(tokens[1], declaredColorSpace, path, context);
    } else if (formatName == "openexr") {
        // EXR declares scene-linear Rec.709 by format default. Any other
        // declared chromaticities contradict the working space.
        if (!chromaticities.empty()) {
            if (chromaticities.size() != kRec709Chromaticities.size()) {
                fail(path, "declared chromaticities are malformed" + note(context, "expected 8 values"));
            }
            for (std::size_t i = 0; i < kRec709Chromaticities.size(); ++i) {
                if (std::abs(chromaticities[i] - kRec709Chromaticities[i]) > kChromaticityTolerance) {
                    fail(path, "declared chromaticities are not Rec.709; the image source working space is "
                               "scene-linear Rec.709" +
                                   note(context, formatName + " file"));
                }
            }
        }
    } else {
        fail(path, "no declared color space and no format default: the source transfer is ambiguous" +
                       note(context, formatName + " file"));
    }

    for (const auto& [field, value] : overrides) {
        if (field == "transfer") {
            const ImageTransfer parsed = parseTransferOverride(value, path, context);
            if (!transfer) {
                transfer = parsed;
            }
        } else if (field == "primaries") {
            const ImagePrimaries parsed = parsePrimariesOverride(value, path, context);
            if (!primaries) {
                primaries = parsed;
            }
        } else if (field == "matrix" || field == "range" || field == "chromaLocation") {
            fail(path, "source interpretation field '" + field + "' is not applicable to an RGB image source" +
                           note(context, "known fields: transfer, primaries"));
        } else {
            fail(path, "source interpretation has unknown field '" + field + "'" +
                           note(context, "known fields: transfer, primaries"));
        }
    }

    if (!transfer) {
        transfer = ImageTransfer::Linear;
    }
    if (!primaries) {
        primaries = ImagePrimaries::Rec709;
    }
    return Interpretation{*transfer, *primaries};
}

// Header-only facts about one resolved frame path. The stat probe and the
// decoding read resolve the declared interpretation from exactly these
// fields, so a declaration is always validated from the header — before
// any plane is read into memory.
struct FrameHeader {
    std::string path;
    std::string formatName;
    std::string declaredColorSpace;
    std::vector<float> chromaticities;
    std::string nativePrecision;
    int width{0};
    int height{0};
    double pixelAspect{1.0};
};

[[nodiscard]] FrameHeader readFrameHeader(const std::string& path) {
    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        fail(path, OIIO::geterror());
    }
    const OIIO::ImageSpec spec = input->spec();
    FrameHeader header;
    header.path = path;
    header.formatName = std::string(input->format_name());
    header.declaredColorSpace = spec.get_string_attribute("oiio:ColorSpace");
    header.chromaticities = declaredChromaticities(spec);
    header.nativePrecision = spec.format.c_str();
    header.width = spec.full_width > 0 ? spec.full_width : spec.width;
    header.height = spec.full_height > 0 ? spec.full_height : spec.height;
    header.pixelAspect = spec.get_float_attribute("pixelaspectratio", 1.0F);
    static_cast<void>(input->close());
    input.reset();
    return header;
}

// Inverse of the declared transfer for one channel. Alpha is never
// transferred; the CPU reference contract is scene-linear straight alpha.
[[nodiscard]] float toLinear(const float value, const ImageTransfer transfer) {
    switch (transfer) {
    case ImageTransfer::Linear:
        return value;
    case ImageTransfer::Srgb:
        return value < 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    case ImageTransfer::Gamma22:
        return std::copysign(std::pow(std::abs(value), 2.2F), value);
    case ImageTransfer::Gamma28:
        return std::copysign(std::pow(std::abs(value), 2.8F), value);
    case ImageTransfer::Bt709:
        return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
    }
    return value;
}

}  // namespace

bool isImagePath(const std::string& path) {
    // Never decided from the extension: a '#'/'@' pattern names an image
    // sequence by construction, everything else must be openable as image
    // data by OpenImageIO (header open only, then close).
    if (hasPattern(path)) {
        return true;
    }
    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        static_cast<void>(OIIO::geterror());
        return false;
    }
    static_cast<void>(input->close());
    return true;
}

ImageFrameInfo probeImageFrame(const SourceReference& reference, const std::string& context,
                               const std::int64_t localTime) {
    std::int64_t frame = 0;
    try {
        frame = reference.frameAt(localTime);
    } catch (const std::exception& error) {
        fail(reference.path, std::string("source time mapping failed: ") + error.what());
    }
    const std::string path = resolveFramePath(reference.path, frame);
    const FrameHeader header = readFrameHeader(path);

    ImageFrameInfo info;
    info.path = header.path;
    info.formatName = header.formatName;
    info.declaredColorSpace = header.declaredColorSpace;
    info.nativePrecision = header.nativePrecision;
    info.width = header.width;
    info.height = header.height;
    info.pixelAspect = header.pixelAspect;
    const Interpretation interpretation = resolveInterpretation(
        path, header.formatName, header.declaredColorSpace, header.chromaticities, reference.interpretation, context);
    info.transfer = interpretation.transfer;
    info.primaries = interpretation.primaries;
    info.sequence = hasPattern(reference.path);
    return info;
}

ImageFrame readImageFrame(const SourceReference& reference, const std::int64_t frame, const std::string& context) {
    const std::string path = resolveFramePath(reference.path, frame);
    // Resolve the declared interpretation from a header-only probe before
    // any plane is touched, matching the clip path's discipline: an
    // ambiguous or unsupported declaration is rejected before the pixels
    // are pulled into memory, not after.
    const FrameHeader header = readFrameHeader(path);
    const Interpretation interpretation = resolveInterpretation(
        path, header.formatName, header.declaredColorSpace, header.chromaticities, reference.interpretation, context);

    ImageReadResult read = readImage(path);

    CpuImage image = std::move(read.image);
    image.setColorInterpretation(ColorInterpretation::SceneLinear);
    const bool premultiplied = read.alpha == AlphaAssociation::Premultiplied;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            std::array<float, kImageChannels> pixel = image.pixel(x, y);
            pixel[0] = toLinear(pixel[0], interpretation.transfer);
            pixel[1] = toLinear(pixel[1], interpretation.transfer);
            pixel[2] = toLinear(pixel[2], interpretation.transfer);
            if (premultiplied && pixel[3] > 0.0F) {
                pixel[0] /= pixel[3];
                pixel[1] /= pixel[3];
                pixel[2] /= pixel[3];
            }
            image.setPixel(x, y, pixel);
        }
    }

    ImageFrame result;
    result.info.path = path;
    result.info.formatName = read.formatName;
    result.info.declaredColorSpace = read.declaredColorSpace;
    result.info.nativePrecision = read.nativePrecision;
    result.info.width = image.width();
    result.info.height = image.height();
    result.info.pixelAspect = image.layout().pixelAspect;
    result.info.transfer = interpretation.transfer;
    result.info.primaries = interpretation.primaries;
    result.info.sequence = hasPattern(reference.path);
    result.image = std::move(image);
    return result;
}

CpuImage ImageSourceProvider::frame(const Document& document, const SourceReference& source,
                                    const std::int64_t mappedFrame, const EvaluationRequest& request) {
    if (document.color.workingSpace != "linear") {
        fail(source.path, "working space '" + document.color.workingSpace +
                              "' is outside the image source's explicit supported subset; only the declared "
                              "default 'linear' (scene-linear Rec.709) is supported for source interpretation");
    }
    // No pre-classification here: readImageFrame reports the reader's own
    // reason naming the path (unreadable data, unsupported layout,
    // ambiguous or unsupported interpretation). A reference that is a video
    // clip simply cannot be read as image data, which is the honest
    // diagnostic for a provider that decodes stills/sequences only.
    const std::string context = "source '" + source.path + "'";
    const ImageFrame decoded = readImageFrame(source, mappedFrame, context);
    const int scale = request.samplingScale;
    const int width = scaledDimension(request.region.width, scale);
    const int height = scaledDimension(request.region.height, scale);
    const int sourceWidth = decoded.image.width();
    const int sourceHeight = decoded.image.height();
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        fail(source.path, "decoded frame has no pixels");
    }

    ImageLayout layout;
    layout.width = width;
    layout.height = height;
    layout.pixelAspect = decoded.info.pixelAspect;
    CpuImage out(layout);

    // Source-fill contract, identical to src/nemo/gpu/shaders/source.slang:
    // raster pixel (x,y) reads the source pixel nearest full-resolution
    // coordinate (region.x + x*scale, region.y + y*scale) by the fill ratio.
    const std::int64_t fullWidth = std::max(request.imageWidth(), 1);
    const std::int64_t fullHeight = std::max(request.imageHeight(), 1);
    for (int y = 0; y < height; ++y) {
        const std::int64_t fullY = static_cast<std::int64_t>(request.region.y) + static_cast<std::int64_t>(y) * scale;
        const int sy = static_cast<int>(std::clamp((fullY * sourceHeight) / fullHeight, std::int64_t{0},
                                                   static_cast<std::int64_t>(sourceHeight - 1)));
        for (int x = 0; x < width; ++x) {
            const std::int64_t fullX =
                static_cast<std::int64_t>(request.region.x) + static_cast<std::int64_t>(x) * scale;
            const int sx = static_cast<int>(std::clamp((fullX * sourceWidth) / fullWidth, std::int64_t{0},
                                                       static_cast<std::int64_t>(sourceWidth - 1)));
            out.setPixel(x, y, decoded.image.pixel(sx, sy));
        }
    }
    return out;
}

}  // namespace nemo::media
