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
    int width{0};
    int height{0};
    double pixelAspect{1.0};
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
    CpuImage image;
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

// Reads one resolved frame of `path` (no pattern expansion: the caller passes
// the frame it already resolved) and returns it in the project's working space
// with straight alpha. The frame's declared interpretation is validated from
// the header before any plane is loaded, and the resolved conversion is applied
// exactly once.
[[nodiscard]] ImageFrame readImageFrame(const InputColorCache& color, const InputColorChoice& choice,
                                        const std::string& path, std::int64_t frame, const std::string& context);

// Source-scoped read for the shared reference's own mapping and
// interpretation with no project config (the legacy metadata path). Retained
// for the media-import worker and non-Read consumers that resolve a reference
// directly.
[[nodiscard]] ImageFrame readImageFrame(const SourceReference& reference, std::int64_t frame,
                                        const std::string& context);

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
