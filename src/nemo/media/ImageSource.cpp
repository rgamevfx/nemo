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
#include "nemo/media/ViewingTransform.hpp"

namespace nemo::media {

namespace {

[[noreturn]] void fail(const std::string& path, const std::string& message) {
    throw ImageIoException(path, message);
}

// Media color failures keep this adapter's own diagnostic shape: the input
// color layer reports the body and the reader names the file.
[[noreturn]] void failColor(const std::string& path, const std::string& message) {
    throw ImageIoException(path, message);
}

[[nodiscard]] bool hasPattern(const std::string& path) {
    return path.find('#') != std::string::npos || path.find('@') != std::string::npos;
}

// Authored sequence range (issue #61) for the source-scoped read path. Only a
// '#'/'@' pattern has a frame range; a still resolves to one file regardless of
// the requested time, so an authored range never rejects it. A frame outside
// the range is an error that names the path and the range — never a clamped or
// substituted frame. The resolved-request path does not need this: the core
// resolver owns the selected interval and the boundary/missing policies.
void requireFrameInRange(const SourceReference& reference, const std::int64_t frame, const std::string& context) {
    if (!hasPattern(reference.path))
        return;
    if (reference.firstFrame && frame < *reference.firstFrame) {
        fail(reference.path, "requested frame " + std::to_string(frame) + " is before the authored first frame " +
                                 std::to_string(*reference.firstFrame) + " (context: " + context + "; sequence range " +
                                 std::to_string(*reference.firstFrame) + ".." +
                                 (reference.lastFrame ? std::to_string(*reference.lastFrame) : std::string{"end"}) +
                                 ")");
    }
    if (reference.lastFrame && frame > *reference.lastFrame) {
        fail(reference.path, "requested frame " + std::to_string(frame) + " is after the authored last frame " +
                                 std::to_string(*reference.lastFrame) + " (context: " + context + "; sequence range " +
                                 (reference.firstFrame ? std::to_string(*reference.firstFrame) : std::string{"start"}) +
                                 ".." + std::to_string(*reference.lastFrame) + ")");
    }
}

// Header-only facts about one resolved frame path. The stat probe and the
// decoding read resolve the declared interpretation from exactly these fields,
// so a declaration is always validated from the header — before any plane is
// read into memory.
struct FrameHeader {
    std::string path;
    std::string formatName;
    std::string declaredColorSpace;
    std::vector<float> chromaticities;
    std::string nativePrecision;
    std::vector<std::string> channelNames;
    int width{0};
    int height{0};
    double pixelAspect{1.0};
    bool hasAlpha{false};
};

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
    header.channelNames.assign(spec.channelnames.begin(), spec.channelnames.end());
    header.hasAlpha = std::find(header.channelNames.begin(), header.channelNames.end(), std::string{"A"}) !=
                      header.channelNames.end();
    header.width = spec.full_width > 0 ? spec.full_width : spec.width;
    header.height = spec.full_height > 0 ? spec.full_height : spec.height;
    header.pixelAspect = spec.get_float_attribute("pixelaspectratio", 1.0F);
    static_cast<void>(input->close());
    input.reset();
    return header;
}

// The file's own declaration, in the input-color layer's vocabulary.
[[nodiscard]] EncodedColorFacts headerFacts(const FrameHeader& header) {
    EncodedColorFacts facts;
    facts.declaredColorSpace = header.declaredColorSpace;
    facts.chromaticities = header.chromaticities;
    facts.formatName = header.formatName;
    facts.associationKnown = header.hasAlpha;
    facts.premultiplied =
        declaredAlphaAssociation(header.formatName, header.hasAlpha) == AlphaAssociation::Premultiplied;
    return facts;
}

// Runs `fn`, translating the color layer's diagnostics into this adapter's
// path-naming exception (never masking the offending relationship).
template <typename Function>
auto translated(const std::string& path, Function&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const ImageIoException&) {
        throw;
    } catch (const InputColorException& error) {
        failColor(path, error.what());
    } catch (const OcioException& error) {
        failColor(path, error.what());
    }
}

[[nodiscard]] ImageFrameInfo frameInfo(const FrameHeader& header, const ResolvedInputColor& resolved) {
    ImageFrameInfo info;
    info.path = header.path;
    info.formatName = header.formatName;
    info.declaredColorSpace = header.declaredColorSpace;
    info.nativePrecision = header.nativePrecision;
    info.channelNames = header.channelNames;
    info.width = header.width;
    info.height = header.height;
    info.pixelAspect = header.pixelAspect;
    info.transfer = resolved.transfer;
    info.primaries = resolved.primaries;
    info.inputColor = resolved;
    // Truthful labeling: Raw/Data samples are non-color data, never managed
    // scene-linear; everything else is working-space scene-linear.
    info.color = resolved.raw() ? ColorInterpretation::Data : ColorInterpretation::SceneLinear;
    return info;
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

ImageFrameInfo probeImageFrame(const InputColorCache& color, const InputColorChoice& choice, const std::string& path,
                               const std::string& context) {
    const FrameHeader header = readFrameHeader(path);
    const EncodedColorFacts facts = headerFacts(header);
    const ResolvedInputColor resolved =
        translated(path, [&] { return color.resolve(choice, facts, header.path, context); });
    ImageFrameInfo info = frameInfo(header, resolved);
    info.sequence = hasPattern(path);
    return info;
}

ImageFrameInfo probeImageFrame(const SourceReference& reference, const std::string& context,
                               const std::int64_t localTime) {
    std::int64_t frame = 0;
    try {
        frame = reference.frameAt(localTime);
    } catch (const std::exception& error) {
        fail(reference.path, std::string("source time mapping failed: ") + error.what());
    }
    requireFrameInRange(reference, frame, context);
    // Source-scoped probe: the shared reference's own mapping and
    // interpretation, with no project configuration engaged.
    const InputColorCache color(SourceColorPolicy{});
    InputColorChoice choice;
    choice.hints = reference.interpretation;
    ImageFrameInfo info = probeImageFrame(color, choice, resolveFramePath(reference.path, frame), context);
    // The source-scoped probe reports the sequence it was asked about, not the
    // expanded frame path.
    info.sequence = hasPattern(reference.path);
    return info;
}

ImageFrame readImageFrame(const InputColorCache& color, const InputColorChoice& choice, const std::string& path,
                          const std::int64_t frame, const std::string& context) {
    const std::string resolvedPath = resolveFramePath(path, frame);
    // The declared interpretation is validated from the header before any
    // plane is touched, matching the clip path's discipline: an ambiguous or
    // unsupported declaration is rejected before the pixels are pulled into
    // memory, not after.
    const FrameHeader header = readFrameHeader(resolvedPath);
    const EncodedColorFacts facts = headerFacts(header);
    const ResolvedInputColor resolved =
        translated(resolvedPath, [&] { return color.resolve(choice, facts, header.path, context); });

    ImageReadResult read = readImage(resolvedPath);
    CpuImage image = std::move(read.image);
    if (resolved.raw()) {
        // Raw/Data: the samples are the encoded values exactly as stored, and
        // their association is left alone.
        image.setColorInterpretation(ColorInterpretation::Data);
    } else {
        // Encoded-domain unassociation, then the resolved transfer/gamut
        // conversion, applied exactly once.
        translated(resolvedPath, [&] {
            color.apply(image, resolved);
            return 0;
        });
        image.setColorInterpretation(ColorInterpretation::SceneLinear);
    }

    ImageFrame result;
    result.info = frameInfo(header, resolved);
    result.info.path = resolvedPath;
    result.info.formatName = read.formatName;
    result.info.declaredColorSpace = read.declaredColorSpace;
    result.info.nativePrecision = read.nativePrecision;
    result.info.width = image.width();
    result.info.height = image.height();
    result.info.pixelAspect = image.layout().pixelAspect;
    result.info.sequence = hasPattern(path);
    result.image = std::move(image);
    return result;
}

ImageFrame readImageFrame(const SourceReference& reference, const std::int64_t frame, const std::string& context) {
    requireFrameInRange(reference, frame, context);
    // Source-scoped read: the shared reference's own interpretation with no
    // project configuration, so existing non-Read consumers keep their exact
    // behavior.
    const InputColorCache color(SourceColorPolicy{});
    InputColorChoice choice;
    choice.hints = reference.interpretation;
    return readImageFrame(color, choice, reference.path, frame, context);
}

ImageSourceProvider::ImageSourceProvider(std::string configPath) : configPath_(std::move(configPath)) {}

std::shared_ptr<const OcioConfigSnapshot> ImageSourceProvider::snapshotLocked() const {
    if (!snapshot_) {
        try {
            // The effective configuration: the authored reference, or the OCIO
            // application default when the project declares none.
            snapshot_ = std::make_shared<const OcioConfigSnapshot>(configPath_);
        } catch (const OcioException&) {
            // No usable configuration: the legacy metadata-only policy. A
            // config-backed working space fails explicitly at resolve time.
            return nullptr;
        }
    }
    return snapshot_;
}

void ImageSourceProvider::refreshColorConfig() const {
    const std::lock_guard lock(colorMutex_);
    // Owner-side generation replacement: the next read loads the configuration
    // content afresh and rebuilds every working-space cache. A read already
    // running holds its own shared owner on the previous generation until it
    // finishes, and the last identity string is left in place rather than
    // cleared underneath a caller.
    snapshot_.reset();
    colors_.clear();
    identityResolved_ = false;
}

std::string ImageSourceProvider::colorConfigIdentity() const {
    std::lock_guard lock(colorMutex_);
    if (!identityResolved_) {
        identityResolved_ = true;
        const std::shared_ptr<const OcioConfigSnapshot> snapshot = snapshotLocked();
        identity_ = snapshot ? snapshot->identity() : std::string{};
    }
    return identity_;
}

std::shared_ptr<const InputColorCache> ImageSourceProvider::colorFor(const std::string& workingSpace) const {
    std::lock_guard lock(colorMutex_);
    const auto found = colors_.find(workingSpace);
    if (found != colors_.end()) {
        return found->second;
    }
    // Same snapshot as the identity: new config bytes can never produce pixels
    // under an old identity.
    auto created =
        std::make_shared<const InputColorCache>(SourceColorPolicy{configPath_, workingSpace}, snapshotLocked());
    colors_.emplace(workingSpace, created);
    return created;
}

CpuImage ImageSourceProvider::frame(const Document& document, const EffectiveSourceRequest& source,
                                    const EvaluationRequest& request) {
    const int scale = request.samplingScale;
    const int width = scaledDimension(request.region.width, scale);
    const int height = scaledDimension(request.region.height, scale);
    const std::string context = "source '" + source.sourceKey + "'";

    // A resolved policy decision is never re-decided here: a policy error is
    // raised and a Black policy produces real transparent black in the requested
    // raster (never a substituted frame).
    if (source.policyError) {
        fail(source.path, "the source request resolves to an error policy; the evaluator must report it before any "
                          "frame is opened");
    }
    if (source.transparentBlack) {
        ImageLayout layout;
        layout.width = width;
        layout.height = height;
        // The buffer default is zero everywhere: transparent black, alpha 0.
        return CpuImage(layout);
    }

    const std::shared_ptr<const InputColorCache> color = colorFor(document.color.workingSpace);
    InputColorChoice choice;
    choice.mode = source.inputTransform;
    choice.inputColorSpace = source.inputColorSpace;
    choice.alpha = source.alpha;
    choice.hints = source.interpretation;
    choice.nodeHintKeys = source.nodeInterpretationKeys;

    // No pre-classification here: the reader reports its own reason naming the
    // path (unreadable data, unsupported layout, ambiguous or unsupported
    // interpretation). A reference that is a video clip simply cannot be read
    // as image data, which is the honest diagnostic for a provider that decodes
    // stills/sequences only.
    const ImageFrame decoded = readImageFrame(*color, choice, source.path, source.readFrame, context);
    const int sourceWidth = decoded.image.width();
    const int sourceHeight = decoded.image.height();
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        fail(source.path, "decoded frame has no pixels");
    }

    ImageLayout layout;
    layout.width = width;
    layout.height = height;
    layout.pixelAspect = decoded.info.pixelAspect;
    layout.color = decoded.info.color;
    CpuImage out(layout);

    // Source-fill contract, identical to src/nemo/nodes/source/source.slang:
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
