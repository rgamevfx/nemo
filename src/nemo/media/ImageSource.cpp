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
// read into memory. `media::inspectImageHeader` is the single owner of the
// extraction; this adapter never re-parses an OIIO spec.

// The one description builder for still/sequence frames (issue #88): the
// declared format at origin 0, the signed data bounds, the declared pixel
// aspect, the read's logical RGBA channels, the contract's float32 precision,
// the association the produced samples carry, and their interpretation. A
// description is always derived from header facts, never by decoding.
[[nodiscard]] ImageDescription imageDescription(const ImageHeader& header, const ColorInterpretation color,
                                                const ImageAssociation association, const Region& dataBounds) {
    return ImageDescription{
        .format = header.windows.format(),
        .dataBounds = dataBounds,
        .pixelAspect = header.pixelAspect,
        .channels = {"R", "G", "B", "A"},
        .precision = Precision::Float32,
        .association = association,
        .color = color,
    };
}

// Association of the samples this adapter produces: a managed read applies the
// declared association and yields straight samples, while a Raw/Data bypass
// leaves the stored samples alone and therefore reports the declared one.
[[nodiscard]] ImageAssociation producedAssociation(const ImageHeader& header, const bool raw) {
    if (!raw) {
        return ImageAssociation::Straight;
    }
    return declaredAlphaAssociation(header.formatName, header.hasAlpha) == AlphaAssociation::Premultiplied
               ? ImageAssociation::Premultiplied
               : ImageAssociation::Straight;
}

// The file's own declaration, in the input-color layer's vocabulary.
[[nodiscard]] EncodedColorFacts headerFacts(const ImageHeader& header) {
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

[[nodiscard]] ImageFrameInfo frameInfo(const ImageHeader& header, const ResolvedInputColor& resolved) {
    ImageFrameInfo info;
    info.path = header.path;
    info.formatName = header.formatName;
    info.declaredColorSpace = header.declaredColorSpace;
    info.nativePrecision = header.nativePrecision;
    info.channelNames = header.channelNames;
    // Logical display extent (the file's format) for probe consumers.
    info.width = header.windows.format().width;
    info.height = header.windows.format().height;
    info.pixelAspect = header.pixelAspect;
    // Truthful labeling: Raw/Data samples are non-color data, never managed
    // scene-linear; everything else is working-space scene-linear.
    info.color = resolved.raw() ? ColorInterpretation::Data : ColorInterpretation::SceneLinear;
    // The description is header-only, so probing a frame never decodes it, and
    // the samples a read produces cover exactly the declared data extent.
    info.coverage = header.windows.dataBounds();
    info.description = imageDescription(header, info.color, producedAssociation(header, resolved.raw()), info.coverage);
    info.transfer = resolved.transfer;
    info.primaries = resolved.primaries;
    info.inputColor = resolved;
    return info;
}

// The resolved color choice one source request carries, in the media
// vocabulary. One owner, so a description and the read it describes can never
// resolve against different choices.
[[nodiscard]] InputColorChoice colorChoice(const EffectiveSourceRequest& source) {
    InputColorChoice choice;
    choice.mode = source.inputTransform;
    choice.inputColorSpace = source.inputColorSpace;
    choice.alpha = source.alpha;
    choice.hints = source.interpretation;
    choice.nodeHintKeys = source.nodeInterpretationKeys;
    return choice;
}

// The frames a header-read description may consult when the requested frame
// itself produces no authored pixels (a Black boundary or missing policy): the
// interval the resolver admits first, then the discovered coverage, then the
// frame that was asked for. Every frame of one source shares its format, so a
// description reads a header that exists instead of the frame that does not —
// and never a decode.
[[nodiscard]] std::vector<std::int64_t> admittedFrames(const EffectiveSourceRequest& source) {
    std::vector<std::int64_t> frames;
    for (const std::optional<std::int64_t> candidate :
         {source.mapping.firstFrame, source.originalFirstFrame, std::optional<std::int64_t>{source.readFrame}}) {
        if (!candidate) {
            continue;
        }
        if (frames.empty() || frames.back() != *candidate) {
            frames.push_back(*candidate);
        }
    }
    return frames;
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
    const ImageHeader header = inspectImageHeader(path);
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

ImageDescription describeImageFrame(const ImageHeader& header, const EffectiveSourceRequest& source) {
    const bool raw = source.dataBypass();
    return imageDescription(header, raw ? ColorInterpretation::Data : ColorInterpretation::SceneLinear,
                            producedAssociation(header, raw), header.windows.dataBounds());
}

ImageFrame readImageFrame(const InputColorCache& color, const InputColorChoice& choice, const ImageHeader& header,
                          const std::string& context) {
    // The declared interpretation is validated from the header before any plane
    // is touched, matching the clip path's discipline: an ambiguous or
    // unsupported declaration is rejected before the pixels are pulled into
    // memory, not after.
    const EncodedColorFacts facts = headerFacts(header);
    const ResolvedInputColor resolved =
        translated(header.path, [&] { return color.resolve(choice, facts, header.path, context); });

    ImageReadResult read = readImage(header.path);
    CpuImage image = std::move(read.image);
    if (resolved.raw()) {
        // Raw/Data: the samples are the encoded values exactly as stored, and
        // their association is left alone.
        image.setColorInterpretation(ColorInterpretation::Data);
    } else {
        // Encoded-domain unassociation, then the resolved transfer/gamut
        // conversion, applied exactly once.
        translated(header.path, [&] {
            color.apply(image, resolved);
            return 0;
        });
        image.setColorInterpretation(ColorInterpretation::SceneLinear);
    }

    // The header owns every reported fact, and the raster the read returned
    // covers exactly the declared data extent, so the description and the
    // samples can never disagree about geometry.
    ImageFrame result;
    result.info = frameInfo(header, resolved);
    result.image = std::move(image);
    return result;
}

ImageFrame readImageFrame(const InputColorCache& color, const InputColorChoice& choice, const std::string& path,
                          const std::int64_t frame, const std::string& context) {
    const std::string resolvedPath = resolveFramePath(path, frame);
    ImageFrame result = readImageFrame(color, choice, inspectImageHeader(resolvedPath), context);
    result.info.sequence = hasPattern(path);
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

ImageDescription describeSourceImage(const InputColorCache& color, const InputColorChoice& choice,
                                     const EffectiveSourceRequest& source, const std::string& context) {
    // A request that must fail never receives an invented geometry: the same
    // core-owned wording execution reports is raised here, before any pixel
    // work, so a plan cannot be built on a request that cannot produce pixels.
    if (source.policyError) {
        throw EvaluationException("source '" + source.sourceKey + "' (" + source.path +
                                  "): " + sourcePolicyProblem(source));
    }
    const ColorInterpretation colorInterpretation =
        source.dataBypass() ? ColorInterpretation::Data : ColorInterpretation::SceneLinear;
    if (source.transparentBlack) {
        // A Black policy produces a cleared raster, so the requested frame is
        // deliberately not opened. The source's authored geometry is still real
        // though — every frame of one source shares its declared format — so the
        // description reads an admitted frame's header rather than decoding the
        // frame it cannot read. The cleared frame holds no authored samples, so
        // its data bounds are EMPTY: a valid connected image with no data,
        // which is exactly how the approved contract separates known-empty
        // bounds from unavailable geometry. A source whose metadata cannot be
        // read at all is a hard failure naming the source and the path, never a
        // fabricated format.
        for (const std::int64_t candidate : admittedFrames(source)) {
            try {
                const ImageHeader header = inspectImageHeader(resolveFramePath(source.path, candidate));
                // The association follows the same rule an ordinary read of this
                // header produces (Straight for a managed read, the declared one
                // for a Raw bypass), so a boundary or missing-frame policy can
                // never flip the metadata of the frame it replaces.
                return imageDescription(header, colorInterpretation, producedAssociation(header, source.dataBypass()),
                                        Region{});
            } catch (const ImageIoException&) {
                continue;  // this candidate cannot describe the source; try the next admitted frame
            }
        }
        throw EvaluationException("source '" + source.sourceKey + "' (" + source.path +
                                  "): the source's metadata is unavailable; no frame of this source declares a "
                                  "readable header, so its geometry cannot be described");
    }

    // The frame the request actually reads: a Hold boundary frame is the frame
    // that will be decoded, so its header is the one that describes the result.
    const ImageFrameInfo info =
        probeImageFrame(color, choice, resolveFramePath(source.path, source.readFrame), context);
    return info.description;
}

ImageDescription ImageSourceProvider::describe(const Document& document, const EffectiveSourceRequest& source) {
    const std::shared_ptr<const InputColorCache> colors = colorFor(document.color.workingSpace);
    return describeSourceImage(*colors, colorChoice(source), source, "source '" + source.sourceKey + "'");
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
    const InputColorChoice choice = colorChoice(source);

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
    // The header owns the geometry and the read built its raster from exactly
    // that declaration, so a mismatch means the samples cannot be placed
    // honestly: report it instead of sampling through a wrong origin.
    const Region coverage = decoded.info.coverage;
    if (coverage.width != sourceWidth || coverage.height != sourceHeight) {
        fail(source.path, "decoded raster is " + std::to_string(sourceWidth) + "x" + std::to_string(sourceHeight) +
                              " but its described coverage is " + std::to_string(coverage.width) + "x" +
                              std::to_string(coverage.height));
    }

    ImageLayout layout;
    layout.width = width;
    layout.height = height;
    layout.pixelAspect = decoded.info.pixelAspect;
    layout.color = decoded.info.color;
    CpuImage out(layout);

    // Source-fill contract: raster pixel (x, y) reads the source sample authored
    // at full-resolution coordinate (region.x + x*scale, region.y + y*scale),
    // through the source raster's own origin (`coverage`). There is deliberately
    // no fill-ratio resize: a source whose geometry differs from the
    // composition's canvas is read at its authored coordinates rather than
    // stretched, and everything outside its data raster stays transparent black
    // (the raster buffer default).
    for (int y = 0; y < height; ++y) {
        const int sy =
            static_cast<int>(static_cast<std::int64_t>(request.region.y) + static_cast<std::int64_t>(y) * scale) -
            coverage.y;
        if (sy < 0 || sy >= sourceHeight) {
            continue;
        }
        for (int x = 0; x < width; ++x) {
            const int sx =
                static_cast<int>(static_cast<std::int64_t>(request.region.x) + static_cast<std::int64_t>(x) * scale) -
                coverage.x;
            if (sx < 0 || sx >= sourceWidth) {
                continue;
            }
            out.setPixel(x, y, decoded.image.pixel(sx, sy));
        }
    }
    return out;
}

}  // namespace nemo::media
