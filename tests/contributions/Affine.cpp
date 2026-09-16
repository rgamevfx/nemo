// Test-only affine RGB contribution: scale*x + offset, alpha preserved, no
// HDR/negative clamp (issue #83). The CPU and Slang pixel algorithms are
// authored independently; their agreement is evidence, not an oracle.
//
// Issue #88 extension: the example also declares its own image meaning and its
// own input demand. The authored `shiftX`/`shiftY` pair (in full-resolution
// pixels; this node declares samplingScale {1}, so they are also raster samples)
// makes every sample read at an absolute image-space offset:
//
//   output(x, y) = scale * input(x - shiftX, y - shiftY) + offset
//
// The node therefore DESCRIBES a data window translated by (+shiftX, +shiftY)
// and REQUESTS its input over the region translated by (-shiftX, -shiftY) — a
// demand that genuinely differs from the node's own region. The default shift 0
// declares exactly the shared inheritance default (unchanged image properties,
// own region), so the module adds no rules for the common case and needs no
// central dispatch, serializer or registry change for the extended one.

#include "Affine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

namespace nemo::test::affine {
namespace {

// Node-local payload: exactly the authored colors the shader consumes plus the
// authored read offset. Trivially copyable and a multiple of 16 bytes, so it
// travels as the common uniform-buffer payload at set 0, binding 1 without a
// shared uniform struct.
struct AffinePayload {
    float scale[4]{};
    float offset[4]{};
    float shift[4]{};  // (shiftX, shiftY, 0, 0) in full-resolution pixels
};
static_assert(sizeof(AffinePayload) % 16 == 0);

// The node's authored read offset, already validated.
struct AffineShift {
    int x{0};
    int y{0};
};

ParameterSpec scaleSpec() {
    return ParameterSpec{
        .name = "scale",
        .type = ParameterType::Color,
        .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
        .label = "Scale",
    };
}

ParameterSpec offsetSpec() {
    return ParameterSpec{
        .name = "offset",
        .type = ParameterType::Color,
        .defaultValue = ParameterValue{ColorValue{{0.0F, 0.0F, 0.0F, 0.0F}}},
        .label = "Offset",
    };
}

ParameterSpec shiftSpec(const char* name, const char* label) {
    return ParameterSpec{
        .name = name,
        .type = ParameterType::Integer,
        .defaultValue = ParameterValue{std::int64_t{0}},
        .label = label,
        .section = "Read Offset",
    };
}

// The example's authored read offset. Wrong types and values beyond the
// declared bound are node-identifying errors, never silent clamps: the offset
// participates in the described data window and in the declared input demand, so
// an unrepresentable one must fail rather than describe something else.
[[nodiscard]] AffineShift effectiveShift(const NodeCatalog& catalog, const NodeInstance& node,
                                         const ParameterValues& effectiveParams) {
    const auto resolve = [&](const char* key) {
        const ParameterValue& value = effectiveParameter(catalog, node, effectiveParams, key);
        const auto* count = std::get_if<std::int64_t>(&value);
        if (count == nullptr) {
            failNode(node, std::string("parameter '") + key + "' must be an integer, got '" +
                               parameterValueText(value) + "'");
        }
        if (*count < -kMaxAffineShift || *count > kMaxAffineShift) {
            failNode(node, std::string("parameter '") + key + "' must be within [" + std::to_string(-kMaxAffineShift) +
                               ", " + std::to_string(kMaxAffineShift) + "]");
        }
        return static_cast<int>(*count);
    };
    return AffineShift{resolve("shiftX"), resolve("shiftY")};
}

// Independent CPU algorithm. Alpha is copied through untouched; RGB carry
// signed, out-of-range scene-linear values without clamping.
//
// Each sample reads the input at the ABSOLUTE image coordinate minus the
// authored shift, expressed in the input raster's own samples through the real
// anchor (issue #85): the producer's raster covers the region this node
// demanded — the node's own region translated by the negative shift — and may
// be anchored anywhere on the lattice, so the anchor is the only fact that
// holds. A sample the raster does not hold is outside the producer's data and
// reads as transparent black (issue #88), which is exactly the whole-image
// reference's behavior for a read that falls off the image.
[[nodiscard]] CpuImage executeAffine(const CpuNodeContext& context) {
    const std::array<float, 4> scale = effectiveColor4(context.catalog, context.node, context.effectiveParams, "scale");
    const std::array<float, 4> offset =
        effectiveColor4(context.catalog, context.node, context.effectiveParams, "offset");
    const AffineShift shift = effectiveShift(context.catalog, context.node, context.effectiveParams);
    if (context.inputs.empty() || context.inputs[0] == nullptr) {
        failNode(context.node, "affine requires a connected input image");
    }
    const CpuImage& input = *context.inputs[0];
    const InputAnchor anchor = anchorBetween(inputRequest(context, 0), context.request);
    CpuImage output(effectRasterLayout(context));
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            const std::array<float, 4> in =
                sampledPixel(input, anchor.offsetX + x - shift.x, anchor.offsetY + y - shift.y);
            output.setPixel(
                x, y,
                {scale[0] * in[0] + offset[0], scale[1] * in[1] + offset[1], scale[2] * in[2] + offset[2], in[3]});
        }
    }
    return output;
}

// One local pass: input port 0 -> node result. No scratch, no weights.
eval::EffectPassDefinition affinePass() {
    return eval::EffectPassDefinition{
        .id = "affine",
        .shader = "Affine",
        .inputs = {eval::EffectImageRef{eval::EffectImageKind::Input, 0}},
        .output = eval::EffectImageRef{eval::EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation only: typed parameters become the node-local
// payload; no device, allocation, wait, or UI callback happens here.
[[nodiscard]] eval::GpuPreparation prepareAffine(const eval::GpuNodeContext& context) {
    const std::array<float, 4> scale = effectiveColor4(context.catalog, context.node, context.effectiveParams, "scale");
    const std::array<float, 4> offset =
        effectiveColor4(context.catalog, context.node, context.effectiveParams, "offset");
    // The shader recomputes each read from its absolute full-resolution
    // coordinate and this offset, so the example's two front ends share no
    // coordinate helper: they agree only if both derive the same sample.
    const AffineShift shift = effectiveShift(context.catalog, context.node, context.effectiveParams);
    AffinePayload payload;
    std::copy(scale.begin(), scale.end(), payload.scale);
    std::copy(offset.begin(), offset.end(), payload.offset);
    payload.shift[0] = static_cast<float>(shift.x);
    payload.shift[1] = static_cast<float>(shift.y);

    eval::GpuPreparation preparation;
    preparation.payload = eval::effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

// The example's declared input demand (issue #88): the node's own region
// translated by the negative offset, because that is exactly the input coverage
// `output(x, y) = scale * input(x - shiftX, y - shiftY) + offset` reads. The
// unshifted region is deliberately NOT demanded as well: a read that falls off
// the producer's data is transparent black, not a coverage failure. The demand
// is clipped to what the producer's own description can hold, so it never asks
// for coordinates that producer does not have.
std::vector<InputRequirement> affineInputRequirements(const NodeRegionContext& context) {
    const AffineShift shift = effectiveShift(context.catalog, context.node, context.effectiveParams);
    const Region& region = context.request.region;
    const Region wanted{region.x - shift.x, region.y - shift.y, region.width, region.height};
    return {InputRequirement{regionIntersection(wanted, requirementDomain(context, 0, wanted)), "RGBA"}};
}

// The example's declared output description (issue #88): everything is inherited
// from the main input — format, pixel aspect, channels, interpretation, so a
// Data input stays Data — except the data window, which is translated by the
// authored read offset because that is where the node's samples come from. An
// empty input window stays empty: a translated empty image holds nothing.
[[nodiscard]] ImageDescription describeAffine(const NodeDescriptionContext& context) {
    const ParameterValues& authored = context.node.params;
    const AffineShift shift = effectiveShift(context.catalog, context.node, authored);
    ImageDescription described = context.inherited;
    Region& bounds = described.dataBounds;
    if ((shift.x != 0 || shift.y != 0) && bounds.width > 0 && bounds.height > 0) {
        bounds = Region{bounds.x + shift.x, bounds.y + shift.y, bounds.width, bounds.height};
    }
    return described;
}

}  // namespace

NodeDescriptor affineDescriptor() {
    return NodeDescriptor{
        .type = std::string(kNodeType),
        .displayName = "Affine (test)",
        .group = "Tests",
        .isOutput = false,
        .implementationVersion = kImplementationVersion,
        .inputs = {PortSpec{.kind = PortKind::Image, .name = "input"}},
        .outputs = {PortSpec{.kind = PortKind::Image, .name = "output"}},
        .parameters = {scaleSpec(), offsetSpec(), shiftSpec("shiftX", "Shift X"), shiftSpec("shiftY", "Shift Y")},
        // Full-resolution pixels are raster samples here, so the declared read
        // offset means the same thing at every declared scale.
        .capabilities = NodeCapabilities{.samplingScales = {1}, .qualityModes = {Quality::Full}, .channels = {"RGBA"}},
    };
}

// Identical pixel declaration, whole-frame-only capability: a region request
// over this type must escalate the node (and its inputs) to the whole image
// domain, and the consumer still receives its own rectangle.
NodeDescriptor affineWholeFrameDescriptor() {
    NodeDescriptor descriptor = affineDescriptor();
    descriptor.type = std::string(kWholeFrameNodeType);
    descriptor.displayName = "Affine whole-frame (test)";
    descriptor.capabilities.supportsRegion = false;
    return descriptor;
}

NodeContribution affineContribution() {
    return NodeContribution{
        .descriptor = affineDescriptor(),
        .role = NodeRole::Image,
        .cpu = CpuImplementation{.version = kImplementationVersion, .execute = &executeAffine},
        .nativeGpu = true,
        .inputRequirements = &affineInputRequirements,
        .describe = &describeAffine,
    };
}

NodeContribution affineWholeFrameContribution() {
    NodeContribution contribution = affineContribution();
    contribution.descriptor = affineWholeFrameDescriptor();
    return contribution;
}

eval::GpuNodeContribution affineGpuContribution() {
    eval::GpuNodeContribution contribution;
    contribution.node = affineContribution();
    contribution.gpu = eval::GpuImplementation{
        .version = kImplementationVersion,
        .payloadLayout = "nemo.test.affine.payload.v2",
        .payloadSize = sizeof(AffinePayload),
        .passes = {affinePass()},
        .prepare = &prepareAffine,
    };
    return contribution;
}

eval::GpuNodeContribution affineWholeFrameGpuContribution() {
    eval::GpuNodeContribution contribution = affineGpuContribution();
    contribution.node = affineWholeFrameContribution();
    return contribution;
}

}  // namespace nemo::test::affine
