// Blur: separable Gaussian with retained scratch and normalized weights
// (issue #83 node-local GPU module).
//
// Blur owns its payload (mask + size/channels/raster-support word), its
// Gaussian weight storage, its three local pass definitions and the callback
// that selects among them:
//   horizontal  Input0            -> Scratch0   (weights)
//   vertical    Scratch0,Input0,Input1 -> Output (weights)
//   identity    Input0,Input0,Input1   -> Output (weights)
// `vertical` and `identity` are the same kernel: its blur.x <= 0 branch is an
// exact identity that never reads the scratch slot, so a size of 0 costs one
// pass and no scratch raster. The shared executor still owns allocation,
// barriers, recording and retirement; preparation only computes values,
// including the intermediate scratch's own coverage (see prepareBlur).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/blur/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: the shared mask word plus the blur tuple
// (size full-res, channels bitmask, raster support, 0).
struct BlurPayload {
    float mask[4]{};  // (maskChannel, invertMask, mix, maskPresent)
    float blur[4]{};  // (size, channels, raster support, 0)
};
static_assert(sizeof(BlurPayload) % 16 == 0);

constexpr const char* kBlurGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform BlurPayload {
    // (maskChannel, invertMask, mix, maskPresent)
    vec4 mask;
    // (size full-res, channels bitmask, raster support, 0)
    vec4 blur;
};
)GLSL";

// Pass 1 of 2: horizontal support. size is the FULL-RESOLUTION support
// radius; the executed raster samples at samplingScale, so the raster support
// is ceil(size/samplingScale) and sample i sits at full-res offset
// i*samplingScale. sigma = size/3; fixed normalized weights; clamp-to-edge.
// size 0 is the exact identity (no shortcut, no normalization degeneracy).
//
// RGBA premultiplies straight RGB by alpha before filtering and is
// unpremultiplied once by the final (vertical) pass; RGB filters RGB only and
// preserves alpha; Alpha filters alpha only and preserves RGB.
//
// The normalized Gaussian weights are precomputed once per Blur preparation
// into a retained read-only storage buffer (set 3 binding 0, index
// i+support); the kernel never evaluates exp() per output pixel.
constexpr const char* kBlurHorizontalGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;
layout(std430, set = 3, binding = 0) readonly buffer BlurWeights { float weights[]; };

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    int mode = int(blur.y);
    // The main input covers a wider rectangle of the same lattice than this
    // scratch pass (the horizontal support on both sides): its raster origin
    // and extent locate the taps, and clamping to that extent is the image's
    // clamp-to-edge border. An input with an empty raster has no pixels to
    // clamp to: the filtered result is transparent black (issue #88).
    ivec2 mainOffset = inputGeometry[0].regionAndOffset.zw;
    ivec2 mainExtent = ivec2(inputGeometry[0].extent.xy);
    if (mainExtent.x <= 0 || mainExtent.y <= 0) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight);
        return;
    }
    int lastX = mainExtent.x - 1;
    int lastY = mainExtent.y - 1;
    ivec2 center = ivec2(p) + mainOffset;
    if (blur.x <= 0.0) {  // exact identity: the whole RGBA projection of the center sample
        vec4 centered = gpuLoadRgba(in_main, ivec2(clamp(center.x, 0, lastX), clamp(center.y, 0, lastY)),
                                    inputGeometry[0].rgba, mainExtent.y);
        gpuStoreRgba(out_color, ivec2(p), rgba, planeHeight, centered);
        gpuPreserveAuxLattice(out_color, ivec2(p), planeHeight, in_main, mainExtent.y, channels.x);
        return;
    }
    // Raster support in samples per axis; named `taps` so the local never
    // shadows the request's `support` word (native binding contract v5).
    int taps = int(blur.z);
    vec4 acc = vec4(0.0);
    for (int i = -taps; i <= taps; ++i) {
        float w = weights[i + taps];
        vec4 s = gpuLoadRgba(in_main, ivec2(clamp(int(p.x) + i + mainOffset.x, 0, lastX),
                                            clamp(int(p.y) + mainOffset.y, 0, lastY)),
                             inputGeometry[0].rgba, mainExtent.y);
        if ((mode & 8) != 0) { s.rgb *= s.a; }  // RGBA: premultiply before filtering
        acc += w * s;
    }
    vec4 centerPixel = gpuLoadRgba(in_main, ivec2(clamp(center.x, 0, lastX), clamp(center.y, 0, lastY)),
                                   inputGeometry[0].rgba, mainExtent.y);
    if (mode == 7) {        // RGB: filter RGB, preserve original alpha
        acc.a = centerPixel.a;
    } else if (mode == 8) { // Alpha: filter alpha, preserve original RGB
        acc.rgb = centerPixel.rgb;
    }
    gpuStoreRgba(out_color, ivec2(p), rgba, planeHeight, acc);
    // The intermediate carries the node's named channels too, so the vertical
    // pass's own preservation reads an unchanged auxiliary value at the same
    // coordinate (issue #90).
    gpuPreserveAuxLattice(out_color, ivec2(p), planeHeight, in_main, mainExtent.y, channels.x);
}
)GLSL";

// Pass 2 of 2: vertical support and the final result. Filters the
// intermediate of pass 1, converts back to straight alpha (RGBA), then
// applies the declared mask/mix exactly once against the original main pixel.
// The identity selection (size 0) runs this same program with the original
// main image bound to the scratch slot, which that branch never reads.
constexpr const char* kBlurGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 1, binding = 1) restrict readonly uniform image2D in_scratch;
layout(set = 1, binding = 2) restrict readonly uniform image2D in_mask;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;
layout(std430, set = 3, binding = 0) readonly buffer BlurWeights { float weights[]; };

// Binding 0 is the ORIGINAL main image, not the filtered intermediate: this
// pass both reads the original for the mask/mix blend and preserves the named
// channels it does not filter from it (issue #90).
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    // The described image's data support (native binding contract v6): a sample
    // outside it is transparent black, and every plane — auxiliary ones
    // included — is initialized (issue #90). This is the pass that writes the
    // node result; the horizontal pass writes a scratch and declares no support.
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight);
        return;
    }
    int mode = int(blur.y);
    // The original main image (binding 0) and the scratch intermediate (binding
    // 1) each cover their own rectangle of the lattice; the scratch's Y extent
    // is the row range the image's clamp-to-edge border lives on. A pixel
    // outside either raster is outside that image's data, and an empty raster
    // has no pixels at all: transparent black (issue #88), never an
    // out-of-bounds load.
    ivec2 mainPixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    ivec2 mainExtent = ivec2(inputGeometry[0].extent.xy);
    bool mainInside = mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    ivec2 scratchOffset = inputGeometry[1].regionAndOffset.zw;
    ivec2 scratchExtent = ivec2(inputGeometry[1].extent.xy);
    vec4 orig = mainInside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, mainExtent.y) : vec4(0.0);
    vec4 processed;
    if (blur.x <= 0.0) {  // exact identity (both passes are identity)
        processed = orig;
    } else if (scratchExtent.x <= 0 || scratchExtent.y <= 0) {
        processed = vec4(0.0);
    } else {
        int taps = int(blur.z);
        vec4 acc = vec4(0.0);
        for (int i = -taps; i <= taps; ++i) {
            float w = weights[i + taps];
            ivec2 tap = ivec2(clamp(int(p.x) + scratchOffset.x, 0, scratchExtent.x - 1),
                              clamp(int(p.y) + i + scratchOffset.y, 0, scratchExtent.y - 1));
            acc += w * gpuLoadRgba(in_scratch, tap, inputGeometry[1].rgba, scratchExtent.y);
        }
        if (mode == 15) {       // RGBA: unpremultiply once at the final output
            processed.a = acc.a;
            processed.rgb = acc.a != 0.0 ? acc.rgb / acc.a : vec3(0.0);
        } else if (mode == 7) { // RGB: filtered RGB, exact original alpha
            processed.rgb = acc.rgb;
            processed.a = orig.a;
        } else {                // Alpha: filtered alpha, exact original RGB
            processed.rgb = orig.rgb;
            processed.a = acc.a;
        }
    }
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        ivec2 maskPixel = ivec2(p) + inputGeometry[2].regionAndOffset.zw;
        ivec2 maskExtent = ivec2(inputGeometry[2].extent.xy);
        bool maskInside =
            maskPixel.x >= 0 && maskPixel.y >= 0 && maskPixel.x < maskExtent.x && maskPixel.y < maskExtent.y;
        float selected =
            maskInside ? clamp(gpuLoadRgba(in_mask, maskPixel, inputGeometry[2].rgba, maskExtent.y)[channel], 0.0, 1.0)
                       : 0.0;
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in mix()).
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : mix(orig, processed, weight));
    gpuStoreRgba(out_color, ivec2(p), rgba, planeHeight, result);
    // Every plane the blur did not filter keeps its named channel from the
    // original main image at the same coordinate (issue #90).
    gpuPreserveAuxLattice(out_color, ivec2(p), planeHeight, in_main, mainExtent.y, channels.x);
}
)GLSL";

[[nodiscard]] EffectPassDefinition blurHorizontalPass() {
    return EffectPassDefinition{
        .id = "horizontal",
        .shader = "blur/blurHorizontal",
        .glsl = nemo::nodes::gpuGlsl(kBlurGlslPayload, kBlurHorizontalGlslBody),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}},
        .output = EffectImageRef{EffectImageKind::Scratch, 0},
        .weights = true,
    };
}

[[nodiscard]] EffectPassDefinition blurVerticalPass() {
    return EffectPassDefinition{
        .id = "vertical",
        .shader = "blur/blur",
        .glsl = nemo::nodes::gpuGlsl(kBlurGlslPayload, kBlurGlslBody),
        // Binding 0 is the original main image (see the kernel): the final pass
        // preserves its named channels, so the filtered scratch never stands in
        // for the untouched auxiliary values (issue #90).
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Scratch, 0},
                   EffectImageRef{EffectImageKind::Input, 1}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
        .weights = true,
    };
}

// size 0 stays a single pass: the vertical kernel's identity branch, with the
// main image standing in for the scratch slot it never reads.
[[nodiscard]] EffectPassDefinition blurIdentityPass() {
    return EffectPassDefinition{
        .id = "identity",
        .shader = "blur/blur",
        .glsl = nemo::nodes::gpuGlsl(kBlurGlslPayload, kBlurGlslBody),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Input, 0},
                   EffectImageRef{EffectImageKind::Input, 1}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
        .weights = true,
    };
}

// Normalized separable Gaussian weights: size is the full-res support radius,
// sigma = size/3, and raster sample i sits at full-res offset i*scale.
[[nodiscard]] std::vector<float> blurWeights(double size, int scale, int support) {
    std::vector<float> weights(static_cast<std::size_t>(2 * support + 1), 1.0F);
    const double sigma = size / 3.0;
    double total = 0.0;
    for (int i = -support; i <= support; ++i) {
        const double weight = std::exp(-0.5 * std::pow(static_cast<double>(i * scale) / sigma, 2.0));
        weights[static_cast<std::size_t>(i + support)] = static_cast<float>(weight);
        total += weight;
    }
    for (float& weight : weights) {
        weight = static_cast<float>(static_cast<double>(weight) / total);
    }
    return weights;
}

// Worker-side value preparation: payload, selected local passes, the
// normalized weights, and the intermediate's coverage.
//
// The horizontal pass writes the node's own X range (only the columns the
// vertical pass will read) but must cover the main input's whole Y range,
// because every output row's taps reach ceil(size/scale) rows beyond it in
// both directions. The planner already requested exactly that expanded,
// clipped input region, so the scratch takes its Y range from the actual
// bound input: clip-to-edge at the scratch border is then the image's own
// border, and no node-name branch in the executor needs to know this.
[[nodiscard]] GpuPreparation prepareBlur(const GpuNodeContext& context) {
    const BlurParameters blur = effectiveBlur(context.catalog, context.node, context.effectiveParams);
    const std::array<float, 4> mask =
        nemo::nodes::gpuMaskWord(context.catalog, context.node, context.effectiveParams, context.maskPresent);
    const int scale = context.request.samplingScale;

    if (context.inputRequests.empty() || context.inputRequests[0].region.width <= 0 ||
        context.inputRequests[0].region.height <= 0) {
        throw std::runtime_error("native effect requires a connected main image input");
    }

    BlurPayload payload;
    std::copy(mask.begin(), mask.end(), payload.mask);
    payload.blur[0] = blur.size;
    payload.blur[1] = static_cast<float>(blur.channels);
    // Raster support: size is a full-resolution radius, so a reduced raster
    // needs ceil(size/samplingScale) taps per axis.
    payload.blur[2] = static_cast<float>(static_cast<int>(std::ceil(blur.size / static_cast<float>(scale))));

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    if (blur.size <= 0.0F) {
        // Exact identity: one pass, no scratch. The declared weight binding
        // stays valid with a single unused float.
        preparation.weights = {1.0F};
        preparation.passes = {2u};
    } else {
        const int support = static_cast<int>(payload.blur[2]);
        preparation.weights = blurWeights(static_cast<double>(blur.size), scale, support);
        preparation.passes = {0u, 1u};
        const Region& request = context.request.region;
        const Region& main = context.inputRequests[0].region;
        preparation.scratch = {EffectScratchRegion{0u, Region{request.x, main.y, request.width, main.height}}};
    }
    return preparation;
}

}  // namespace

GpuNodeContribution blurGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::blurContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.blur.payload.v1";
    implementation.payloadSize = sizeof(BlurPayload);
    implementation.passes = {blurHorizontalPass(), blurVerticalPass(), blurIdentityPass()};
    implementation.prepare = &prepareBlur;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
