// Transform: inverse-mapped geometry with alpha-aware filtering and the
// common mask/mix controls (issue #83 node-local GPU module).
//
// Transform owns its payload (mask, transform tuple, precomputed trig/filter
// flags) at set 0 binding 1 and its own GLSL reference source. The geometry
// and interpolation math is unchanged from issue #34.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/transform/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: the shared mask word, the authored transform tuple, and
// the host-precomputed filter/aspect/trig flags.
struct TransformPayload {
    float mask[4]{};            // (maskChannel, invertMask, mix, maskPresent)
    float transform[4]{};       // (translateX, translateY, scale, rotate degrees)
    float transformFlags[4]{};  // (filter, pixelAspect, cos, sin)
};
static_assert(sizeof(TransformPayload) % 16 == 0);

constexpr const char* kTransformGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform TransformPayload {
    // (maskChannel, invertMask, mix, maskPresent)
    vec4 mask;
    // (translateX, translateY, scale, rotate degrees)
    vec4 transform;
    // (filter 0 Cubic / 1 Linear / 2 Nearest, pixelAspect, cos, sin)
    vec4 transformFlags;
};
)GLSL";

constexpr const char* kTransformGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 1, binding = 1) restrict readonly uniform image2D in_mask;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

// One alpha-aware premultiplied tap of the resampling input, gathered through
// the R/G/B/A plane roles its geometry entry resolved (issue #90); `dims.y` is
// the input's LOGICAL height, which is also its channel plane height.
vec4 premultTexel(ivec2 q, ivec2 dims) {
    if (q.x < 0 || q.y < 0 || q.x >= dims.x || q.y >= dims.y) { return vec4(0.0); }
    vec4 s = gpuLoadRgba(in_main, q, inputGeometry[0].rgba, dims.y);
    return vec4(s.rgb * s.a, s.a);
}

void catmullRomWeights(float t, out float w[4]) {
    // Separable Catmull-Rom, a = -0.5.
    w[0] = -0.5 * t * t * t + t * t - 0.5 * t;
    w[1] = 1.5 * t * t * t - 2.5 * t * t + 1.0;
    w[2] = -1.5 * t * t * t + 2.0 * t * t + 0.5 * t;
    w[3] = 0.5 * t * t * t - 0.5 * t * t;
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // The described image's data support (native binding contract v6): a sample
    // outside the transform's described data window is transparent black, and
    // every plane — auxiliary ones included — is initialized (issue #90).
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), int(meta2.y));
        return;
    }
    // The resampled input may cover a different rectangle of the same lattice
    // (region evaluation, wider cache-backed inputs): its full-resolution
    // region origin and raster extent come from the bound geometry, never
    // from this pass's own request.
    ivec2 inputOrigin = inputGeometry[0].regionAndOffset.xy;
    ivec2 dims = ivec2(inputGeometry[0].extent.xy);
    ivec2 mainPixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    bool mainInside = mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < dims.x && mainPixel.y < dims.y;
    vec4 orig = mainInside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, dims.y) : vec4(0.0);

    int scale = int(meta2.z);
    float fullWidth = float(meta.x);
    float fullHeight = float(meta.y);
    float regionX = float(meta.z);
    float regionY = float(meta.w);
    float aspect = transformFlags.y;

    // Output pixel center in full resolution, then the declared inverse:
    // subtract translation, undo rotation (physical coords), undo scale.
    vec2 fullOut = vec2(regionX + (float(p.x) + 0.5) * float(scale),
                        regionY + (float(p.y) + 0.5) * float(scale));
    vec2 center = vec2(fullWidth * 0.5, fullHeight * 0.5);
    vec2 g = fullOut - center - transform.xy;
    // Host-precomputed cos/sin of the rotation (double radians on the host).
    float c = transformFlags.z;
    float s = transformFlags.w;
    vec2 physical = vec2(g.x * aspect, g.y);
    vec2 rotated = vec2(physical.x * c + physical.y * s, -physical.x * s + physical.y * c);
    vec2 fullIn = center + vec2(rotated.x / aspect, rotated.y) / transform.z;

    vec4 processed = vec4(0.0);  // outside the sampled support: transparent black
    int filterMode = int(transformFlags.x);
    vec2 raster = vec2((fullIn.x - float(inputOrigin.x)) / float(scale),
                       (fullIn.y - float(inputOrigin.y)) / float(scale));
    if (filterMode == 2) {  // Nearest: bound the raster index; outside is transparent black
        if (raster.x >= 0.0 && raster.x < float(dims.x) && raster.y >= 0.0 && raster.y < float(dims.y)) {
            processed = gpuLoadRgba(in_main, ivec2(floor(raster)), inputGeometry[0].rgba, dims.y);
        }
    } else if (raster.x > -3.0 && raster.x < float(dims.x) + 3.0 &&
               raster.y > -3.0 && raster.y < float(dims.y) + 3.0) {
        // Linear/Cubic sample their expanded support directly; taps outside
        // the raster are transparent black, so an edge kernel is never
        // cropped. The coarse guard only skips support lying entirely
        // outside the raster (same black result, bounded index math).
        vec2 base = raster - 0.5;
        ivec2 i0 = ivec2(floor(base));
        vec2 f = base - vec2(i0);
        if (filterMode == 1) {  // Linear (bilinear), alpha-aware
            vec4 p00 = premultTexel(i0, dims);
            vec4 p10 = premultTexel(i0 + ivec2(1, 0), dims);
            vec4 p01 = premultTexel(i0 + ivec2(0, 1), dims);
            vec4 p11 = premultTexel(i0 + ivec2(1, 1), dims);
            vec4 accumulated = mix(mix(p00, p10, f.x), mix(p01, p11, f.x), f.y);
            processed = vec4(accumulated.a != 0.0 ? accumulated.rgb / accumulated.a : vec3(0.0), accumulated.a);
        } else {  // Cubic (separable Catmull-Rom), alpha-aware
            float wx[4];
            float wy[4];
            catmullRomWeights(f.x, wx);
            catmullRomWeights(f.y, wy);
            vec4 accumulated = vec4(0.0);
            for (int j = 0; j < 4; ++j) {
                vec4 row = vec4(0.0);
                for (int i = 0; i < 4; ++i) {
                    row += wx[i] * premultTexel(i0 + ivec2(i - 1, j - 1), dims);
                }
                accumulated += wy[j] * row;
            }
            processed = vec4(accumulated.a != 0.0 ? accumulated.rgb / accumulated.a : vec3(0.0), accumulated.a);
        }
    }

    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        ivec2 maskPixel = ivec2(p) + inputGeometry[1].regionAndOffset.zw;
        ivec2 maskExtent = ivec2(inputGeometry[1].extent.xy);
        bool maskInside =
            maskPixel.x >= 0 && maskPixel.y >= 0 && maskPixel.x < maskExtent.x && maskPixel.y < maskExtent.y;
        float selected =
            maskInside ? clamp(gpuLoadRgba(in_mask, maskPixel, inputGeometry[1].rgba, maskExtent.y)[channel], 0.0, 1.0)
                       : 0.0;
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in mix()).
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : mix(orig, processed, weight));
    gpuStoreRgba(out_color, ivec2(p), rgba, int(meta2.y), result);
    // Auxiliary channels are preserved where they ARE, not resampled: the aux
    // helper resolves the source pixel on the sampling lattice, never the
    // transformed sample this pass computed (issue #90).
    gpuPreserveAuxLattice(out_color, ivec2(p), int(meta2.y), in_main, dims.y, channels.x);
}
)GLSL";

[[nodiscard]] EffectPassDefinition transformPass() {
    return EffectPassDefinition{
        .id = "transform",
        .shader = "transform/transform",
        .glsl = nemo::nodes::gpuGlsl(kTransformGlslPayload, kTransformGlslBody),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Input, 1}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: the shared typed Transform metadata becomes
// this node's payload. An unrepresentable aspect is a node+parameter error,
// never a silent fallback.
[[nodiscard]] GpuPreparation prepareTransform(const GpuNodeContext& context) {
    const TransformParameters transform = effectiveTransform(context.catalog, context.node, context.effectiveParams);
    const std::array<float, 4> mask =
        nemo::nodes::gpuMaskWord(context.catalog, context.node, context.effectiveParams, context.maskPresent);
    if (!std::isfinite(context.pixelAspect) || context.pixelAspect <= 0.0F) {
        throw std::runtime_error("transform main input has an invalid pixel aspect (" +
                                 std::to_string(context.pixelAspect) + ")");
    }

    TransformPayload payload;
    std::copy(mask.begin(), mask.end(), payload.mask);
    payload.transform[0] = transform.translateX;
    payload.transform[1] = transform.translateY;
    payload.transform[2] = transform.scale;
    payload.transform[3] = transform.rotate;
    // Rotation trig is computed once on the host in double precision so exact
    // 90-degree ties stay exact in float; the kernel never evaluates
    // radians/cos/sin per output pixel (issue #34 parity).
    const double angleRadians = static_cast<double>(transform.rotate) * (std::numbers::pi / 180.0);
    payload.transformFlags[0] = static_cast<float>(transform.filter);
    payload.transformFlags[1] = context.pixelAspect;
    payload.transformFlags[2] = static_cast<float>(std::cos(angleRadians));
    payload.transformFlags[3] = static_cast<float>(std::sin(angleRadians));

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution transformGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::transformContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.transform.payload.v1";
    implementation.payloadSize = sizeof(TransformPayload);
    implementation.passes = {transformPass()};
    implementation.prepare = &prepareTransform;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
