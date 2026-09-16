#pragma once

// Still-image and image-sequence source adapter (issue #62; issue #81 color).
//
// A Document SourceReference that names a still (or a '#'/'@' sequence
// pattern) resolves through the same plan/source-fill path as a clip: the
// evaluator schedules the source node, the provider returns the frame
// interpreted into the document's working space. The interpretation rules are
// the media input-color resolver's single owner (InputColor.hpp): a named OCIO
// input color space from the project config wins outright; otherwise a
// declared `oiio:ColorSpace` (or, for EXR, the format default) resolves the
// transfer and Rec.709 primaries with the fill-only interpretation hints
// filling only fields the file left unspecified; otherwise the project
// config's own file rule applies. Anything unresolved is an error naming the
// file, never a silent conversion or guess. Raw/Data bypasses transfer and
// gamut conversion and is reported as Data, never as managed scene-linear.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/InputColor.hpp"

namespace nemo::media {

struct ImageFrameInfo {
    std::string path;
    std::string formatName;
    std::string declaredColorSpace;
    // Native storage precision of the source samples (e.g. "half", "uint8",
    // "float") and the named channels, reported so probe/import consumers do
    // not re-derive them.
    std::string nativePrecision;
    std::vector<std::string> channelNames;
    // Logical display width/height of the frame (the file's format), kept for
    // probe consumers; `description.format` carries the same extent together
    // with the signed data bounds.
    int width{0};
    int height{0};
    double pixelAspect{1.0};
    // The frame's image description (issue #88): its display format at origin
    // 0, its signed data bounds (negative/off-format samples retained),
    // pixel aspect, channels, precision and interpretation. Header-only, so
    // describing a frame never decodes it.
    ImageDescription description;
    // The geometry of the samples this frame actually produces: the data
    // raster's origin and extent in the frame's own normalized coordinates, so
    // a consumer reads it through `coverage` instead of assuming a raster that
    // starts at the format origin. For a still/sequence read this is the data
    // window's extent (the raster storage extents are exactly `coverage`'s).
    Region coverage;
    ImageTransfer transfer{ImageTransfer::Linear};
    ImagePrimaries primaries{ImagePrimaries::Rec709};
    bool sequence{false};
    // Interpretation of the decoded frame: scene-linear working-space samples,
    // or Data when Raw/Data bypassed color conversion. A consumer must not
    // infer that anything that is not DisplayReferred is managed scene-linear.
    ColorInterpretation color{ColorInterpretation::SceneLinear};
    // The resolved input interpretation and where it came from, so the
    // inspector can report Auto's actual choice and its origin.
    ResolvedInputColor inputColor;
};

struct ImageFrame {
    ImageFrameInfo info;  // scene-linear straight alpha, float32 RGBA
    CpuImage image;       // the data raster, covering info.coverage
};

[[nodiscard]] bool isImagePath(const std::string& path);

// Header-only facts for the frame a source-local time maps to, using the
// shared reference's own mapping and interpretation (the source-scoped path
// existing non-Read consumers use). `localTime` is mapped through the
// reference's offset/step exactly once; the default 0 preserves the original
// head-frame probe.
[[nodiscard]] ImageFrameInfo probeImageFrame(const SourceReference& reference, const std::string& context,
                                             std::int64_t localTime = 0);

// Header-only facts for one already-resolved media path, including the input
// interpretation the request's color choices resolve to against this project
// policy. No pixels are read.
[[nodiscard]] ImageFrameInfo probeImageFrame(const InputColorCache& color, const InputColorChoice& choice,
                                             const std::string& path, const std::string& context);

// The description one already-inspected image header gives the frame a request
// reads (issue #88): the normalized format, the signed data bounds, the pixel
// aspect, the read's logical channels and precision, and the association and
// interpretation the produced samples carry. Header-only, so a consumer can
// describe the frame it is about to decode — and key or validate its own cache
// on that meaning — without a pixel read.
[[nodiscard]] ImageDescription describeImageFrame(const ImageHeader& header, const EffectiveSourceRequest& source);

// Reads one already-inspected frame: the same read as the path overload, from a
// header the caller inspected itself, so a caller that described the frame
// before deciding to decode it does not open the file twice. `header.path` names
// the resolved frame.
[[nodiscard]] ImageFrame readImageFrame(const InputColorCache& color, const InputColorChoice& choice,
                                        const ImageHeader& header, const std::string& context);

// Reads one resolved frame of `path` (no pattern expansion: the caller passes
// the frame it already resolved) and returns it in the project's working space
// with straight alpha. The frame's declared interpretation is validated from
// the header before any plane is loaded, and the resolved conversion is applied
// exactly once. The returned raster covers `info.coverage` (the frame's signed
// data bounds), not the display format: framing is description, not storage.
[[nodiscard]] ImageFrame readImageFrame(const InputColorCache& color, const InputColorChoice& choice,
                                        const std::string& path, std::int64_t frame, const std::string& context);

// Source-scoped read for the shared reference's own mapping and
// interpretation with no project config (the legacy metadata path). Retained
// for the media-import worker and non-Read consumers that resolve a reference
// directly.
[[nodiscard]] ImageFrame readImageFrame(const SourceReference& reference, std::int64_t frame,
                                        const std::string& context);

// Header-only description of one resolved source request over the still/
// sequence path (issue #88): the frame the request reads when that frame
// authors samples, else the geometry its admitted frames declare. No plane is
// loaded, nothing is rendered and nothing is read back, so describing a source
// can never cost a decode. A `policyError` request is refused with the shared
// core wording, because a request that cannot produce pixels must not be given
// a geometry to plan with, and a source whose metadata cannot be read at all is
// refused by name and path rather than described with a fabricated or
// placeholder format.
[[nodiscard]] ImageDescription describeSourceImage(const InputColorCache& color, const InputColorChoice& choice,
                                                   const EffectiveSourceRequest& source, const std::string& context);

// Headless CPU-reference provider over the still/sequence path. `configPath` is
// the project's authored color configuration (a path or the registered
// built-in reference); empty keeps the OCIO application default ($OCIO) for a
// config-backed working space and means "no config at all" for the legacy
// `linear` policy. The provider owns the effective OCIO resolution — including
// the opaque config identity mixed into source-node reuse keys — so callers
// cannot silently evaluate against a stale color configuration.
class ImageSourceProvider final : public SourceProvider {
public:
    explicit ImageSourceProvider(std::string configPath = {});

    // Header-only description of the frame this request resolves to (issue
    // #88): the declared format, signed data bounds, pixel aspect, channels,
    // precision and interpretation, read from the file header. No plane is
    // loaded and nothing is rendered, so a description can never cost a decode;
    // an unreadable header, an unsupported declaration or a request whose policy
    // requires failure is reported here instead of being guessed at.
    [[nodiscard]] ImageDescription describe(const Document& document, const EffectiveSourceRequest& source) override;

    [[nodiscard]] CpuImage frame(const Document& document, const EffectiveSourceRequest& source,
                                 const EvaluationRequest& request) override;
    // OCIO content identity of the configuration this provider resolves source
    // color against ("" when the project uses the legacy no-config policy).
    [[nodiscard]] std::string colorConfigIdentity() const override;

    // Explicit refresh boundary (see eval::SourceSession::refreshColorConfig):
    // after a project replacement or a deliberate configuration reload, drop
    // the retained identity and processors so the next frame re-reads the
    // configuration content. A read already running keeps its own handle on the
    // previous generation until it finishes.
    void refreshColorConfig() const;

private:
    std::string configPath_;
    mutable std::mutex colorMutex_;
    // One retained input-color context per working space: a sequence decodes
    // many frames through a single validated processor.
    // ONE retained configuration snapshot per generation: the identity the CPU
    // evaluator mixes into reuse keys and the snapshot every read resolves
    // against are the same bytes.
    mutable std::shared_ptr<const OcioConfigSnapshot> snapshot_;
    mutable std::map<std::string, std::shared_ptr<const InputColorCache>> colors_;
    mutable bool identityResolved_{false};
    mutable std::string identity_;

    // Shared owner of the retained color state for one working space; the frame
    // holds it for the whole read.
    [[nodiscard]] std::shared_ptr<const InputColorCache> colorFor(const std::string& workingSpace) const;

    // The retained snapshot (nullptr when no configuration is available).
    // Requires colorMutex_.
    [[nodiscard]] std::shared_ptr<const OcioConfigSnapshot> snapshotLocked() const;
};

}  // namespace nemo::media
