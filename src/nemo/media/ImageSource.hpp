#pragma once

// Still-image and image-sequence source adapter (issue #62).
//
// A Document SourceReference that names a still (or a '#'/'@' sequence
// pattern) resolves through the same plan/source-fill path as a clip: the
// evaluator schedules the source node, the provider returns the frame
// interpreted into the document's scene-linear Rec.709 working space. The
// interpretation rules are explicit: a declared `oiio:ColorSpace` (or, for
// EXR, the format default) resolves the transfer and Rec.709 primaries, a
// reference interpretation map fills only fields the file left
// unspecified, and anything ambiguous or unsupported is an error naming
// the file — never a silent conversion or guess.
#include <cstdint>
#include <string>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Image.hpp"
namespace nemo::media {
enum class ImageTransfer { Linear, Srgb, Gamma22, Gamma28, Bt709 };
enum class ImagePrimaries { Rec709 };
struct ImageFrameInfo {
    std::string path;
    std::string formatName;
    std::string declaredColorSpace;
    // Native storage precision of the source samples (e.g. "half", "uint8",
    // "float"), reported so probe/import consumers do not re-derive it.
    std::string nativePrecision;
    int width{0};
    int height{0};
    double pixelAspect{1.0};
    ImageTransfer transfer{ImageTransfer::Linear};
    ImagePrimaries primaries{ImagePrimaries::Rec709};
    bool sequence{false};
};
struct ImageFrame {
    ImageFrameInfo info;
    CpuImage image;  // scene-linear, straight alpha, float32 RGBA
};
[[nodiscard]] bool isImagePath(const std::string& path);
// Header-only facts for the frame a source-local time maps to. `localTime`
// is mapped through the reference's offset/step (frameAt) exactly once, so a
// sequence reference with a non-identity mapping probes the frame it will
// decode; the default 0 preserves the original head-frame probe.
[[nodiscard]] ImageFrameInfo probeImageFrame(const SourceReference& reference, const std::string& context,
                                             std::int64_t localTime = 0);
[[nodiscard]] ImageFrame readImageFrame(const SourceReference& reference, std::int64_t frame,
                                        const std::string& context);
// Headless CPU-reference provider over the still/sequence path: resolves a
// `source` node's reference through the image adapter and maps the decoded
// frame to the request raster exactly as the source fill does. A reference
// that is a video clip cannot be read as image data and is reported by the
// reader's own path/format/reason diagnostic — the headless CPU reference
// decodes still images and '#'/'@' image sequences only; clips need the
// native GPU decode path (`eval::SourceSession`).
class ImageSourceProvider final : public SourceProvider {
public:
    [[nodiscard]] CpuImage frame(const Document& document, const SourceReference& source, std::int64_t mappedFrame,
                                 const EvaluationRequest& request) override;
};
}  // namespace nemo::media
