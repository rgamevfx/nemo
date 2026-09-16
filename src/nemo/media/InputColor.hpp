#pragma once

// Input color resolution (issue #75/#81): what an encoded source's RGB
// samples mean, where that decision came from, how their alpha is associated,
// and the retained conversion into the project's working space.
//
// One owner for the precedence, shared by the still/sequence adapter
// (ImageSource), the clip decoder (VideoDecode) and the Read inspector's
// resolved-summary query, so no consumer re-derives it:
//
//   1. an explicit OCIO input color space (a Read's Input Transform override)
//      wins outright and performs transfer AND gamut conversion;
//   2. Raw/Data bypasses transfer/gamut conversion (format and decoder
//      validation still apply) and touches neither samples nor association;
//   3. Auto uses the file/stream's own declaration first, with the recognized
//      fill-only interpretation hints (node scope, else the shared reference)
//      filling only fields the media left unspecified — so tagged media keeps
//      winning and legacy pixels are preserved;
//   4. only when nothing was declared or hinted does Auto fall through to the
//      project config's file rule (a configured rule, reported as such — never
//      claimed to be file metadata); a config default rule is reported as a
//      default, not as a file declaration;
//   5. anything still unresolved is an error naming the offending
//      relationship. The decoder never guesses.
//
// The resolution itself is media- and framework-free: no OCIO object and no
// GPU type crosses this header. The OCIO-backed conversion lives in the media
// OCIO adapter (ViewingTransform.hpp) behind the retained `InputColorCache`.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"

namespace nemo::media {

class OcioInputTransform;
class OcioConfigSnapshot;

// ---------------------------------------------------------------------------
// Encoded-image vocabulary
// ---------------------------------------------------------------------------

// Transfer characteristic of the encoded samples. `Linear` is scene-linear
// data (the working space itself when the primaries also match).
enum class ImageTransfer { Linear, Srgb, Gamma22, Gamma28, Bt709 };

// Source chromaticities. Only Rec.709 is representable: the supported working
// space is scene-linear Rec.709, so any other gamut must arrive through a real
// OCIO input color space instead of being relabelled here.
enum class ImagePrimaries { Rec709 };

[[nodiscard]] const char* imageTransferName(ImageTransfer transfer) noexcept;
[[nodiscard]] const char* imagePrimariesName(ImagePrimaries primaries) noexcept;

// Inverse of the two name functions, so a consumer that holds validated NAMES
// (a probe's declared transfer/primaries) maps them to typed facts through the
// same vocabulary instead of re-implementing it. Returns nullopt for an
// unrecognized name; the caller reports it, never a guessed value.
[[nodiscard]] std::optional<ImageTransfer> imageTransferFromName(std::string_view name) noexcept;
[[nodiscard]] std::optional<ImagePrimaries> imagePrimariesFromName(std::string_view name) noexcept;

// Inverse of one declared transfer for one channel, with signed extension
// (matrix expansion may produce negative RGB even from unsigned samples).
// Alpha is never transferred: the internal contract is scene-linear straight
// alpha.
[[nodiscard]] float imageTransferToLinear(float value, ImageTransfer transfer);

// ---------------------------------------------------------------------------
// Project policy and media facts
// ---------------------------------------------------------------------------

// The project color policy a source converts INTO. `workingSpace == "linear"`
// is the legacy sentinel for the built-in scene-linear Rec.709 working space
// with no authored config: it keeps exactly the previous metadata-only behavior
// (no OCIO is opened for source interpretation), so legacy documents are
// unaffected. Any other working space is a real color space of the resolved
// OCIO config and must validate as scene-linear Rec.709.
struct SourceColorPolicy {
    std::string configPath;              // authored reference or pinned 'ocio://' URI; empty = $OCIO / none
    std::string workingSpace{"linear"};  // Document ColorPolicy working space; the default IS the built-in policy

    [[nodiscard]] bool configBacked() const noexcept { return workingSpace != "linear"; }
    [[nodiscard]] bool operator==(const SourceColorPolicy&) const = default;
};

// What the source media ITSELF declared. An empty/unset member means the media
// declared nothing — the resolver never invents a value for it.
struct EncodedColorFacts {
    // Readers identify complete primary/root RGB; other named planes are data.
    bool hasPrimaryRgb{true};
    // Still/sequence facts.
    std::string declaredColorSpace;     // OpenImageIO `oiio:ColorSpace`, e.g. "srgb_rec709_scene"
    std::vector<float> chromaticities;  // declared chromaticities (8 values) or empty
    std::string formatName;             // "openexr", "png", ...
    // Clip facts: the container/stream tags the decoder resolved, already in
    // this vocabulary. Y'CbCr matrix/range/chroma decoding is NOT here: it is a
    // mandatory codec layout step owned by the clip decoder and is never
    // supplied by an RGB input color space.
    bool clip{false};
    bool transferKnown{false};
    ImageTransfer transfer{ImageTransfer::Bt709};
    bool primariesKnown{false};
    ImagePrimaries primaries{ImagePrimaries::Rec709};
    // Association the reader reported for the samples (stills); a clip has no
    // alpha channel and is always opaque/straight.
    bool associationKnown{false};
    bool premultiplied{false};
};

// Association of the decoded samples after resolution. The internal image
// contract is straight alpha, so `Premultiplied` means the samples still have to
// be unassociated — in the encoded domain, before any nonlinear conversion.
enum class ResolvedAlpha { Straight, Premultiplied };
[[nodiscard]] const char* resolvedAlphaName(ResolvedAlpha alpha) noexcept;

// ---------------------------------------------------------------------------
// Authored choices (from the core EffectiveSourceRequest)
// ---------------------------------------------------------------------------

// The Read's (or shared reference's) authored color choices. `hints` is the
// fill-only interpretation map already merged by the core resolver (a node's
// non-auto parameter first, else the shared reference), and `nodeHintKeys`
// reports which hint keys came from the node scope so the origin can be named
// without re-deriving precedence.
struct InputColorChoice {
    InputTransformMode mode{InputTransformMode::Auto};
    std::string inputColorSpace;
    AlphaMode alpha{AlphaMode::Auto};
    std::map<std::string, std::string> hints;
    std::uint8_t nodeHintKeys{0};
};

// Which conversion produces the working-space samples.
enum class InputTransformKind { OcioColorspace, MetadataTransfer, Raw };

// Where the resolved interpretation came from, in precedence order.
enum class InputTransformOrigin {
    NodeOcioOverride,      // a Read's explicit Input Transform / Raw choice
    NodeInterpretation,    // a Read's own encoded-interpretation hint (source* params)
    SourceInterpretation,  // the shared media reference's interpretation
    FileMetadata,          // the file/stream declaration (or the format default)
    ConfigFileRule,        // the project config's file rule for the path
    ConfigDefaultRule,     // the project config's default rule
    Raw                    // Raw/Data bypass
};
[[nodiscard]] const char* inputTransformOriginName(InputTransformOrigin origin) noexcept;
[[nodiscard]] const char* inputTransformKindName(InputTransformKind kind) noexcept;

// One resolved input interpretation.
struct ResolvedInputColor {
    InputTransformKind kind{InputTransformKind::MetadataTransfer};
    // OCIO input color space name when kind == OcioColorspace; empty otherwise.
    std::string colorSpace;
    // Effective working target this resolution converts into (the project
    // working space for OCIO conversions, "linear" for the legacy metadata
    // path).
    std::string workingSpace;
    // Metadata path: the transfer/primaries the samples are interpreted with.
    ImageTransfer transfer{ImageTransfer::Linear};
    ImagePrimaries primaries{ImagePrimaries::Rec709};
    InputTransformOrigin origin{InputTransformOrigin::FileMetadata};
    ResolvedAlpha alpha{ResolvedAlpha::Straight};

    [[nodiscard]] bool ocio() const noexcept { return kind == InputTransformKind::OcioColorspace; }
    [[nodiscard]] bool raw() const noexcept { return kind == InputTransformKind::Raw; }
    [[nodiscard]] bool operator==(const ResolvedInputColor&) const = default;
};

// Media failures in this layer name the offending relationship.
struct InputColorException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Resolves one source's input color. `path` is the resolved media path (used
// for config file rules) and `context` names the offending node/source in
// errors. Throws InputColorException, or the OCIO adapter's OcioException when
// the config itself is missing/unusable, naming the offending relationship.
[[nodiscard]] ResolvedInputColor resolveInputColor(const SourceColorPolicy& policy, const InputColorChoice& choice,
                                                   const EncodedColorFacts& facts, const std::string& path,
                                                   const std::string& context);

// Scope of the fill-only hint map: a still/sequence RGB source can honor only
// the RGB fields, a clip also honors the Y'CbCr decode fields.
enum class HintScope { Image, Clip };

struct ParsedEncodedHints {
    bool transfer{false};
    ImageTransfer transferValue{ImageTransfer::Linear};
    bool primaries{false};
    ImagePrimaries primariesValue{ImagePrimaries::Rec709};
    // True when a recognized hint key was present at all (a key that filled
    // nothing still counts as authored, so a partial hint is reported rather
    // than silently ignored).
    bool any{false};
    bool nodeTransfer{false};
    bool nodePrimaries{false};
};

// Parses `choice.hints` in the given scope. Unknown keys or unsupported values
// throw InputColorException naming the field, the value and the supported set —
// never silently ignored or guessed.
[[nodiscard]] ParsedEncodedHints parseEncodedHints(const InputColorChoice& choice, HintScope scope,
                                                   const std::string& path, const std::string& context);

// ---------------------------------------------------------------------------
// Retained conversion into the working space
// ---------------------------------------------------------------------------

// Immutable input-color state for one decode generation (a source session, an
// image provider, a clip decoder): the resolved config's content identity and
// ONE retained OCIO processor per resolved input color space, so a sequence or
// clip decodes many frames through a single processor build. A generation is
// never mutated — the owner that crosses a replacement/reload boundary replaces
// the handle, and the previous generation dies with its last consumer (each
// consumer keeps its own shared handle for as long as it uses the cache).
// Thread-safe: decode sessions are serialized but the identity query is not.
//
// The conversion itself is the internal image contract: RGB only, alpha passes
// through, the result is scene-linear straight alpha in the working space.
class InputColorCache {
public:
    // `snapshot` is the retained configuration snapshot the whole cache
    // generation resolves against (identity, working validation, file rules,
    // processors). The owner that already holds one passes it so the pre-cache
    // identity and the decoded pixels come from the same bytes; omitted, the
    // cache loads its own snapshot once, on first use.
    explicit InputColorCache(SourceColorPolicy policy, std::shared_ptr<const OcioConfigSnapshot> snapshot = {});
    ~InputColorCache();
    InputColorCache(InputColorCache&&) noexcept;
    InputColorCache& operator=(InputColorCache&&) noexcept;
    InputColorCache(const InputColorCache&) = delete;
    InputColorCache& operator=(const InputColorCache&) = delete;

    [[nodiscard]] const SourceColorPolicy& policy() const noexcept;
    // The retained snapshot this generation resolves against: identity,
    // canonical space enumeration and file rules all come from here, never from
    // a second load of the same path.
    [[nodiscard]] const OcioConfigSnapshot& snapshot() const;
    // OCIO content identity of the resolved config ("" for the legacy
    // no-config policy). Materialized once and cached.
    [[nodiscard]] const std::string& configIdentity() const;
    // Resolves one request's input color, retaining the working-target
    // validation and the config file rule for a media path, so a per-frame
    // reader does not re-open the config per frame. Same precedence as the
    // stateless `resolveInputColor`.
    [[nodiscard]] ResolvedInputColor resolve(const InputColorChoice& choice, const EncodedColorFacts& facts,
                                             const std::string& path, const std::string& context) const;
    // Validates the working target for this generation (memoized after the
    // first check; a no-op for the legacy built-in policy, which has no
    // configuration to validate against). A consumer that serves a cached
    // decode must still report an invalid target instead of silently reusing
    // pixels produced for another meaning.
    void requireWorkingTarget() const;

    // Applies `resolved` to a decoded pixel buffer in place:
    //   * Premultiplied samples are unassociated in the ENCODED domain, before
    //     any nonlinear conversion, and a zero-alpha pixel becomes a
    //     deterministic zero RGB (never NaN, never the hidden colour);
    //   * Straight samples keep their valid hidden RGB;
    //   * Raw/Data leaves both samples and association untouched;
    //   * an OCIO input space runs the retained processor (RGB only).
    // Throws InputColorException when `resolved.ocio()` has no color space.
    void apply(CpuImage& image, const ResolvedInputColor& resolved) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::media
