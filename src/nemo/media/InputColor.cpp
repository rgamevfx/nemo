#include "nemo/media/InputColor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include "nemo/media/ViewingTransform.hpp"

namespace nemo::media {

namespace {

// Rec.709 / sRGB primaries with D65 white: the one gamut the metadata path can
// represent, so a file that declares anything else is an error rather than a
// silent relabelling.
constexpr std::array<float, 8> kRec709Chromaticities{0.64F, 0.33F, 0.30F, 0.60F, 0.15F, 0.06F, 0.3127F, 0.3290F};
constexpr float kChromaticityTolerance = 1e-4F;

// The messages of this layer are the *body* of a media diagnostic: the still
// adapter prefixes "image: <path>: " and the clip adapter prefixes its own
// reason, so the offending relationship is named once and identically.
[[noreturn]] void fail(const std::string& message) {
    throw InputColorException(message);
}

[[nodiscard]] std::string note(const std::string& context, const std::string& details) {
    return " (context: " + (context.empty() ? std::string{"image source"} : context) + "; " + details + ")";
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
                                                 const std::string& context) {
    static const std::map<std::string, ImageTransfer> byName{{"lin", ImageTransfer::Linear},
                                                             {"srgb", ImageTransfer::Srgb},
                                                             {"g22", ImageTransfer::Gamma22},
                                                             {"g28", ImageTransfer::Gamma28},
                                                             {"bt709", ImageTransfer::Bt709}};
    const auto it = byName.find(token);
    if (it == byName.end()) {
        fail("declared color space '" + declared + "' has unsupported transfer '" + token + "'" +
             note(context, "supported: lin, srgb, g22, g28, bt709"));
    }
    return it->second;
}

[[nodiscard]] ImagePrimaries primariesFromDeclared(const std::string& token, const std::string& declared,
                                                   const std::string& context) {
    if (token != "rec709") {
        fail("declared color space '" + declared + "' has unsupported primaries '" + token + "'" +
             note(context, "supported: rec709 (the working space is scene-linear Rec.709)"));
    }
    return ImagePrimaries::Rec709;
}

// The config-backed lookups the resolution needs. The stateless entry point
// performs them directly; a retained `InputColorCache` memoizes them so a
// per-frame reader never re-opens the config for the same work.
struct ConfigLookup {
    virtual ~ConfigLookup() = default;
    virtual void requireWorkingTarget(const SourceColorPolicy& policy) const = 0;
    virtual ConfigFileRule fileRule(const SourceColorPolicy& policy, const std::string& path) const = 0;
};

struct DirectLookup final : ConfigLookup {
    void requireWorkingTarget(const SourceColorPolicy& policy) const override {
        requireSceneLinearRec709(policy.configPath, policy.workingSpace, "source working space");
    }
    [[nodiscard]] ConfigFileRule fileRule(const SourceColorPolicy& policy, const std::string& path) const override {
        return configFileRuleFor(policy.configPath, path);
    }
};

// Whether the media declaration plus the fill-only hints determined the whole
// interpretation. A hint fills only a field the media left unspecified, and the
// EXR format default (a weak, format-level default) is applied last — after the
// hints — exactly as the previous shared implementation did.
struct MetadataResolution {
    bool transfer{false};
    ImageTransfer transferValue{ImageTransfer::Linear};
    bool primaries{false};
    ImagePrimaries primariesValue{ImagePrimaries::Rec709};
    bool anyHint{false};
    bool hintFilled{false};
    InputTransformOrigin hintOrigin{InputTransformOrigin::SourceInterpretation};

    [[nodiscard]] bool decided() const noexcept { return transfer && primaries; }
};

[[nodiscard]] MetadataResolution resolveFromMedia(const EncodedColorFacts& facts, const ParsedEncodedHints& hints,
                                                  const std::string& context) {
    MetadataResolution out;
    bool formatDefault = false;
    if (facts.clip) {
        out.transfer = facts.transferKnown;
        out.transferValue = facts.transfer;
        out.primaries = facts.primariesKnown;
        out.primariesValue = facts.primaries;
    } else if (!facts.declaredColorSpace.empty()) {
        const std::vector<std::string> tokens = splitTokens(facts.declaredColorSpace);
        out.transferValue = transferFromDeclared(tokens.front(), facts.declaredColorSpace, context);
        out.transfer = true;
        if (tokens.size() < 2 || tokens[1].empty()) {
            fail("declared color space '" + facts.declaredColorSpace + "' does not declare primaries" +
                 note(context, "supported: rec709 (the working space is scene-linear Rec.709)"));
        }
        out.primariesValue = primariesFromDeclared(tokens[1], facts.declaredColorSpace, context);
        out.primaries = true;
    } else if (facts.formatName == "openexr") {
        // EXR declares scene-linear Rec.709 by format default. Any other
        // declared chromaticities contradict the working space, and the default
        // itself is weak: an explicit interpretation hint still fills it first.
        if (!facts.chromaticities.empty()) {
            if (facts.chromaticities.size() != kRec709Chromaticities.size()) {
                fail("declared chromaticities are malformed" + note(context, "expected 8 values"));
            }
            for (std::size_t i = 0; i < kRec709Chromaticities.size(); ++i) {
                if (std::abs(facts.chromaticities[i] - kRec709Chromaticities[i]) > kChromaticityTolerance) {
                    fail("declared chromaticities are not Rec.709; the image source working space is scene-linear "
                         "Rec.709" +
                         note(context, facts.formatName + " file"));
                }
            }
        }
        formatDefault = true;
    }
    out.anyHint = hints.any;
    // Fill only what the media left unspecified.
    if (hints.transfer && !out.transfer) {
        out.transfer = true;
        out.transferValue = hints.transferValue;
        out.hintFilled = true;
        if (hints.nodeTransfer) {
            out.hintOrigin = InputTransformOrigin::NodeInterpretation;
        }
    }
    if (hints.primaries && !out.primaries) {
        out.primaries = true;
        out.primariesValue = hints.primariesValue;
        out.hintFilled = true;
        if (hints.nodePrimaries) {
            out.hintOrigin = InputTransformOrigin::NodeInterpretation;
        }
    }
    if (formatDefault) {
        if (!out.transfer) {
            out.transfer = true;
            out.transferValue = ImageTransfer::Linear;
        }
        if (!out.primaries) {
            out.primaries = true;
            out.primariesValue = ImagePrimaries::Rec709;
        }
    }
    return out;
}

[[nodiscard]] ResolvedAlpha resolveAlpha(AlphaMode mode, const EncodedColorFacts& facts) {
    switch (mode) {
    case AlphaMode::Straight:
        return ResolvedAlpha::Straight;
    case AlphaMode::Premultiplied:
        return ResolvedAlpha::Premultiplied;
    case AlphaMode::Auto:
        // Reliable source metadata decides; a clip has no alpha channel at all.
        return facts.associationKnown && facts.premultiplied ? ResolvedAlpha::Premultiplied : ResolvedAlpha::Straight;
    }
    return ResolvedAlpha::Straight;
}

[[nodiscard]] ResolvedInputColor resolveImpl(const SourceColorPolicy& policy, const InputColorChoice& choice,
                                             const EncodedColorFacts& facts, const std::string& path,
                                             const std::string& context, const ConfigLookup& lookup) {
    ResolvedInputColor resolved;
    resolved.workingSpace = policy.workingSpace;
    if (!facts.hasPrimaryRgb) {
        resolved.kind = InputTransformKind::Raw;
        resolved.origin = InputTransformOrigin::Raw;
        return resolved;
    }

    const ParsedEncodedHints hints =
        parseEncodedHints(choice, facts.clip ? HintScope::Clip : HintScope::Image, path, context);
    resolved.alpha = resolveAlpha(choice.alpha, facts);

    switch (choice.mode) {
    case InputTransformMode::Raw:
        // Raw/Data bypasses transfer and gamut conversion. Format and decoder
        // validation already happened; samples and association stay untouched.
        resolved.kind = InputTransformKind::Raw;
        resolved.origin = InputTransformOrigin::Raw;
        return resolved;
    case InputTransformMode::Explicit: {
        if (choice.inputColorSpace.empty()) {
            fail("input transform is explicit but no input color space is set" +
                 note(context, "choose a color space of the project configuration, or use Auto or Raw"));
        }
        if (!policy.configBacked()) {
            fail("input color space '" + choice.inputColorSpace +
                 "' cannot be resolved: the project working space is the built-in scene-linear Rec.709 and no OCIO "
                 "configuration is engaged" +
                 note(context, "set the project color configuration to use named input transforms"));
        }
        // Validates the config, the named space and the working target before
        // any samples are converted into it.
        lookup.requireWorkingTarget(policy);
        resolved.kind = InputTransformKind::OcioColorspace;
        resolved.colorSpace = choice.inputColorSpace;
        resolved.origin = InputTransformOrigin::NodeOcioOverride;
        // The RGB transfer/primaries of the media are irrelevant: the named
        // space performs transfer AND gamut conversion. Y'CbCr matrix/range
        // decoding already happened in the decoder, so linearization is
        // applied exactly once.
        return resolved;
    }
    case InputTransformMode::Auto:
        break;
    }

    // A config-backed project converts into its named working space, so the
    // target is validated by meaning for EVERY interpretation that produces
    // color samples — the metadata-transfer path included, because a name-only
    // working space would otherwise be trusted there. Raw/Data carries no color
    // semantics and needs no target.
    if (policy.configBacked()) {
        lookup.requireWorkingTarget(policy);
    }

    const MetadataResolution metadata = resolveFromMedia(facts, hints, context);
    if (metadata.transfer && metadata.primaries) {
        resolved.transfer = metadata.transferValue;
        resolved.primaries = metadata.primariesValue;
        // A hint that actually filled a field is the origin; a hint that lost
        // to a tagged field never decided anything.
        resolved.origin = metadata.hintFilled ? metadata.hintOrigin : InputTransformOrigin::FileMetadata;
        return resolved;
    }

    // The media declared nothing that determines the interpretation. A partial
    // hint is an authorization to interpret the other half, so it is an error
    // rather than something the config silently completes.
    if (metadata.anyHint) {
        const std::string missing = !metadata.transfer ? "transfer" : "primaries";
        fail("source interpretation hint does not specify the " + missing +
             "; the media declares no color space and an incomplete interpretation is an error" +
             note(context, "set both transfer and primaries, or a named input color space"));
    }

    if (!policy.configBacked()) {
        fail("no declared color space and no format default: the source transfer is ambiguous" +
             note(context, (facts.formatName.empty() ? std::string{"source"} : facts.formatName) + " file"));
    }

    // The project config's own file rule: a configured rule, reported as such —
    // never claimed to be file metadata.
    lookup.requireWorkingTarget(policy);
    const ConfigFileRule rule = lookup.fileRule(policy, path);
    if (!rule.found || rule.colorSpace.empty()) {
        fail("no declared color space, no format default, and the project configuration has no file rule for '" + path +
             "'" + note(context, "choose an explicit input color space"));
    }
    resolved.kind = InputTransformKind::OcioColorspace;
    resolved.colorSpace = rule.colorSpace;
    resolved.origin = rule.defaultRule ? InputTransformOrigin::ConfigDefaultRule : InputTransformOrigin::ConfigFileRule;
    return resolved;
}

// Unassociates premultiplied encoded RGB before any nonlinear conversion: the
// association is a property of the encoded samples, so dividing after the
// transfer would scale the wrong quantity. A zero-alpha pixel becomes a
// deterministic zero RGB rather than a division blow-up or hidden colour.
//
// The identified root RGB and alpha indices are resolved once by the caller:
// this walks pixels only, never names. Without an identified alpha there is no
// association to undo.
void unassociateEncoded(CpuImage& image, const std::array<int, 4>& indices) {
    if (indices[3] < 0) {
        return;
    }
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const float alpha = image.channel(x, y, indices[3]);
            for (int channel = 0; channel < 3; ++channel) {
                float value = image.channel(x, y, indices[channel]);
                if (alpha > 0.0F) {
                    value /= alpha;
                } else {
                    value = 0.0F;
                }
                image.setChannel(x, y, indices[channel], value);
            }
        }
    }
}

}  // namespace

const char* imageTransferName(const ImageTransfer transfer) noexcept {
    switch (transfer) {
    case ImageTransfer::Linear:
        return "linear";
    case ImageTransfer::Srgb:
        return "srgb";
    case ImageTransfer::Gamma22:
        return "gamma22";
    case ImageTransfer::Gamma28:
        return "gamma28";
    case ImageTransfer::Bt709:
        return "bt709";
    }
    return "unknown";
}

const char* imagePrimariesName(const ImagePrimaries primaries) noexcept {
    switch (primaries) {
    case ImagePrimaries::Rec709:
        return "rec709";
    }
    return "unknown";
}

std::optional<ImageTransfer> imageTransferFromName(const std::string_view name) noexcept {
    static const std::map<std::string_view, ImageTransfer> byName{{"linear", ImageTransfer::Linear},
                                                                  {"srgb", ImageTransfer::Srgb},
                                                                  {"gamma22", ImageTransfer::Gamma22},
                                                                  {"gamma28", ImageTransfer::Gamma28},
                                                                  {"bt709", ImageTransfer::Bt709}};
    const auto found = byName.find(name);
    return found == byName.end() ? std::nullopt : std::optional<ImageTransfer>{found->second};
}

std::optional<ImagePrimaries> imagePrimariesFromName(const std::string_view name) noexcept {
    if (name == "rec709") {
        return ImagePrimaries::Rec709;
    }
    return std::nullopt;
}

const char* resolvedAlphaName(const ResolvedAlpha alpha) noexcept {
    switch (alpha) {
    case ResolvedAlpha::Straight:
        return "straight";
    case ResolvedAlpha::Premultiplied:
        return "premultiplied";
    }
    return "unknown";
}

const char* inputTransformKindName(const InputTransformKind kind) noexcept {
    switch (kind) {
    case InputTransformKind::OcioColorspace:
        return "ocio";
    case InputTransformKind::MetadataTransfer:
        return "metadata";
    case InputTransformKind::Raw:
        return "raw";
    }
    return "unknown";
}

const char* inputTransformOriginName(const InputTransformOrigin origin) noexcept {
    switch (origin) {
    case InputTransformOrigin::NodeOcioOverride:
        return "node-ocio-override";
    case InputTransformOrigin::NodeInterpretation:
        return "node-interpretation";
    case InputTransformOrigin::SourceInterpretation:
        return "source-interpretation";
    case InputTransformOrigin::FileMetadata:
        return "file-metadata";
    case InputTransformOrigin::ConfigFileRule:
        return "config-file-rule";
    case InputTransformOrigin::ConfigDefaultRule:
        return "config-default-rule";
    case InputTransformOrigin::Raw:
        return "raw";
    }
    return "unknown";
}

float imageTransferToLinear(const float value, const ImageTransfer transfer) {
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

ParsedEncodedHints parseEncodedHints(const InputColorChoice& choice, const HintScope scope, const std::string&,
                                     const std::string& context) {
    ParsedEncodedHints out;
    for (const auto& [field, value] : choice.hints) {
        const bool fromNode = sourceHintBit(field) != 0 && (choice.nodeHintKeys & sourceHintBit(field)) != 0;
        if (field == kSourceHintTransfer) {
            static const std::map<std::string, ImageTransfer> byName{{"bt709", ImageTransfer::Bt709},
                                                                     {"srgb", ImageTransfer::Srgb},
                                                                     {"gamma22", ImageTransfer::Gamma22},
                                                                     {"gamma28", ImageTransfer::Gamma28},
                                                                     {"linear", ImageTransfer::Linear}};
            const auto it = byName.find(value);
            if (it == byName.end()) {
                fail("source interpretation field 'transfer' has unsupported value '" + value + "'" +
                     note(context, "supported: bt709, srgb, gamma22, gamma28, linear"));
            }
            out.transfer = true;
            out.transferValue = it->second;
            out.nodeTransfer = fromNode;
            out.any = true;
        } else if (field == kSourceHintPrimaries) {
            if (value != "bt709") {
                fail("source interpretation field 'primaries' has unsupported value '" + value + "'" +
                     note(context, "supported: bt709"));
            }
            out.primaries = true;
            out.primariesValue = ImagePrimaries::Rec709;
            out.nodePrimaries = fromNode;
            out.any = true;
        } else if (scope == HintScope::Image &&
                   (field == kSourceHintMatrix || field == kSourceHintRange || field == kSourceHintChromaLocation)) {
            // A still is RGB: the Y'CbCr decode fields cannot be honored, and
            // silently ignoring them would misrepresent the samples.
            fail("source interpretation field '" + field + "' is not applicable to an RGB image source" +
                 note(context, "known fields: transfer, primaries"));
        } else if (field != kSourceHintMatrix && field != kSourceHintRange && field != kSourceHintChromaLocation) {
            fail("source interpretation has unknown field '" + field + "'" +
                 note(context,
                      "known fields: transfer, primaries" +
                          (scope == HintScope::Clip ? std::string{", matrix, range, chromaLocation"} : std::string{})));
        }
    }
    return out;
}

ResolvedInputColor resolveInputColor(const SourceColorPolicy& policy, const InputColorChoice& choice,
                                     const EncodedColorFacts& facts, const std::string& path,
                                     const std::string& context) {
    const DirectLookup lookup;
    return resolveImpl(policy, choice, facts, path, context, lookup);
}

// ---------------------------------------------------------------------------
// Retained conversion
// ---------------------------------------------------------------------------

struct InputColorCache::Impl {
    Impl(SourceColorPolicy value, std::shared_ptr<const OcioConfigSnapshot> held)
        : policy(std::move(value)), snapshot(std::move(held)) {}

    SourceColorPolicy policy;
    // ONE immutable configuration snapshot per generation (owner-supplied or
    // loaded lazily on first use): identity, working validation, file rules and
    // processors all come from these bytes, and the generation is never
    // mutated afterwards.
    mutable std::shared_ptr<const OcioConfigSnapshot> snapshot;
    mutable std::mutex mutex;
    mutable bool identityResolved{false};
    mutable std::string identity;
    mutable bool workingValidated{false};
    // One retained processor per resolved input color space. A conversion holds
    // its own shared owner across the whole conversion, so replacing a
    // generation (owner-side) only drops these lookup references and lets the
    // previous generation die with its last consumer.
    mutable std::map<std::string, std::shared_ptr<const OcioInputTransform>> transforms;

    [[nodiscard]] const std::shared_ptr<const OcioConfigSnapshot>& snapshotFor() const {
        if (!snapshot) {
            try {
                snapshot = std::make_shared<const OcioConfigSnapshot>(policy.configPath);
            } catch (const OcioException& error) {
                // The configuration could not even be loaded, but the caller
                // still has to learn the authored working relationship it asked
                // for — the same suffix owner the validation failure uses.
                throw OcioException(std::string(error.what()) +
                                    workingTargetContext("source working space", policy.workingSpace));
            }
        }
        return snapshot;
    }

    void requireWorkingTarget() const {
        if (workingValidated) {
            return;
        }
        snapshotFor()->requireSceneLinearRec709(policy.workingSpace, "source working space");
        workingValidated = true;
    }

    // An in-memory rule lookup on the retained snapshot: no reload, and no
    // per-frame cache of resolved frame paths (a sequence must not grow one
    // entry per decoded frame).
    [[nodiscard]] ConfigFileRule fileRule(const std::string& path) const { return snapshotFor()->fileRuleFor(path); }

    // One retained OCIO processor per resolved input color space. Requires the
    // cache mutex; the shared owner keeps the processor alive for a caller that
    // releases the lock before converting.
    [[nodiscard]] std::shared_ptr<const OcioInputTransform>
    retainedTransform(const std::string& inputColorSpace) const {
        const auto found = transforms.find(inputColorSpace);
        if (found != transforms.end()) {
            return found->second;
        }
        auto created = snapshotFor()->inputTransform(policy.workingSpace, inputColorSpace);
        transforms.emplace(inputColorSpace, created);
        return created;
    }
};

InputColorCache::InputColorCache(SourceColorPolicy policy, std::shared_ptr<const OcioConfigSnapshot> snapshot)
    : impl_(std::make_unique<Impl>(std::move(policy), std::move(snapshot))) {}
InputColorCache::~InputColorCache() = default;
InputColorCache::InputColorCache(InputColorCache&&) noexcept = default;
InputColorCache& InputColorCache::operator=(InputColorCache&&) noexcept = default;

const SourceColorPolicy& InputColorCache::policy() const noexcept {
    return impl_->policy;
}

void InputColorCache::requireWorkingTarget() const {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->policy.configBacked()) {
        return;  // the built-in policy converts to its own scene-linear Rec.709
    }
    impl_->requireWorkingTarget();
}

const OcioConfigSnapshot& InputColorCache::snapshot() const {
    const std::lock_guard lock(impl_->mutex);
    return *impl_->snapshotFor();
}

const std::string& InputColorCache::configIdentity() const {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->policy.configBacked()) {
        impl_->identity.clear();
        impl_->identityResolved = true;
        return impl_->identity;
    }
    if (!impl_->identityResolved) {
        impl_->identityResolved = true;
        try {
            // From the SAME snapshot the processors and rules come from.
            impl_->identity = impl_->snapshotFor()->identity();
        } catch (const OcioException&) {
            // No usable configuration: a config-backed working space fails
            // explicitly when it resolves, naming the relationship.
            impl_->identity.clear();
        }
    }
    return impl_->identity;
}

ResolvedInputColor InputColorCache::resolve(const InputColorChoice& choice, const EncodedColorFacts& facts,
                                            const std::string& path, const std::string& context) const {
    // A session validates its working target and reads a file rule at most
    // once, so a per-frame reader never re-opens the config for the same work.
    struct RetainedLookup final : ConfigLookup {
        explicit RetainedLookup(const Impl& source) : impl(source) {}
        const Impl& impl;
        void requireWorkingTarget(const SourceColorPolicy&) const override { impl.requireWorkingTarget(); }
        [[nodiscard]] ConfigFileRule fileRule(const SourceColorPolicy&, const std::string& path) const override {
            return impl.fileRule(path);
        }
    };
    const RetainedLookup lookup{*impl_};
    const std::lock_guard lock(impl_->mutex);
    return resolveImpl(impl_->policy, choice, facts, path, context, lookup);
}

void InputColorCache::apply(CpuImage& image, const ResolvedInputColor& resolved) const {
    // Raw/Data: the samples mean exactly what the decoder produced; neither a
    // transfer conversion nor an association change is applied to them.
    if (resolved.raw()) {
        return;
    }
    // Only an identified complete root RGB set is colour. An image without one
    // (an alpha-only matte, a Z pass) carries no colour channels: nothing is
    // converted and nothing is unassociated, so every declared channel survives
    // bitwise. The indices are resolved once, outside the pixel loops, and
    // never re-derived per sample.
    const std::array<int, 4> indices = image.rgbaIndices();
    if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0) {
        return;
    }
    const bool premultiplied = resolved.alpha == ResolvedAlpha::Premultiplied;
    if (resolved.ocio()) {
        if (premultiplied) {
            unassociateEncoded(image, indices);
        }
        // The retained processor is looked up under the cache's own lock; the
        // returned object stays alive for the whole conversion because a
        // configuration change retires the previous generation instead of
        // destroying it under an in-flight user.
        std::shared_ptr<const OcioInputTransform> transform;
        {
            const std::lock_guard lock(impl_->mutex);
            transform = impl_->retainedTransform(resolved.colorSpace);
        }
        transform->apply(image);
        return;
    }
    if (premultiplied) {
        unassociateEncoded(image, indices);
    }
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const int index = indices[channel];
                image.setChannel(x, y, index, imageTransferToLinear(image.channel(x, y, index), resolved.transfer));
            }
        }
    }
}

}  // namespace nemo::media
