// Merge: two-input composite with an optional mask (issue #83 node-local GPU
// module).
//
// The operation code and the shared mask word travel in this node's own
// payload (set 0 binding 1); A/B/mask bind at set 1 in declared port order.
// The composite math is Merge's own and shares nothing with the CPU
// reference beyond the common typed metadata.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/merge/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: the shared mask word plus the operation code.
struct MergePayload {
    float mask[4]{};  // (maskChannel, invertMask, mix, maskPresent)
    float op[4]{};    // x = operation code (0 Over .. 4 Difference)
};
static_assert(sizeof(MergePayload) % 16 == 0);

constexpr const char* kMergeGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform MergePayload {
    // (maskChannel, invertMask, mix, maskPresent)
    vec4 mask;
    // x = operation code
    vec4 op;
};
)GLSL";

constexpr const char* kMergeGlslBody = R"GLSL(
// Issue #75: A (background) at set 1 binding 0, B (foreground) at binding 1,
// optional mask at binding 2. The operation code travels in op.x
// (0 Over, 1 Plus, 2 Multiply, 3 Screen, 4 Difference) and the shared mask
// word drives the final coverage*mix interpolation. An absent mask is bound
// to a valid dummy descriptor with maskPresent = 0. Every input is read at
// the pixel holding the same full-resolution sample, located through its own
// raster origin (region evaluation, wider cache-backed inputs).
layout(set = 1, binding = 0) restrict readonly uniform image2D in_a;     // port A: background
layout(set = 1, binding = 1) restrict readonly uniform image2D in_b;     // port B: foreground
layout(set = 1, binding = 2) restrict readonly uniform image2D in_mask;  // optional port 2
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // The described image's data support (native binding contract v7): a sample
    // outside it is transparent black, never a composite of operands that hold
    // nothing there, and every stored channel — auxiliary ones included — is
    // initialized
    // (issue #90).
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), int(meta2.y), channels.y, channels.x);
        return;
    }
    // Each operand is read at the pixel holding the same full-resolution sample,
    // located through its own raster origin and extent. A pixel outside the
    // operand's raster is outside its data (a smaller, differently anchored or
    // empty data window): transparent black, never an out-of-bounds load.
    ivec2 bgPixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    ivec2 bgExtent = ivec2(inputGeometry[0].extent.xy);
    ivec2 fgPixel = ivec2(p) + inputGeometry[1].regionAndOffset.zw;
    ivec2 fgExtent = ivec2(inputGeometry[1].extent.xy);
    bool bgInside = bgPixel.x >= 0 && bgPixel.y >= 0 && bgPixel.x < bgExtent.x && bgPixel.y < bgExtent.y;
    bool fgInside = fgPixel.x >= 0 && fgPixel.y >= 0 && fgPixel.x < fgExtent.x && fgPixel.y < fgExtent.y;
    // Port A is the background/base and port B the foreground/source: each is
    // gathered through the plane roles of its own geometry entry (issue #90).
    vec4 bg = bgInside ? gpuLoadRgba(in_a, bgPixel, inputGeometry[0].rgba, bgExtent.y,
                                     inputGeometry[0].channels.y) : vec4(0.0);
    vec4 fg = fgInside ? gpuLoadRgba(in_b, fgPixel, inputGeometry[1].rgba, fgExtent.y,
                                     inputGeometry[1].channels.y) : vec4(0.0);

    int operation = int(op.x);
    vec4 composite;
    if (operation == 0) {
        // Over keeps the CPU reference's existing expression exactly. A
        // premultiplied interpretation is a declared comparison failure.
        composite.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    } else {
        vec3 target;
        if (operation == 1) { target = bg.xyz + fg.xyz; }
        else if (operation == 2) { target = bg.xyz * fg.xyz; }
        else if (operation == 3) { target = vec3(1.0) - (vec3(1.0) - bg.xyz) * (vec3(1.0) - fg.xyz); }
        else { target = abs(bg.xyz - fg.xyz); }  // Difference
        composite.xyz = bg.xyz + fg.a * (target - bg.xyz);
    }
    // Alpha is operation-independent, as in current Over.
    composite.w = fg.a + (1.0 - fg.a) * bg.a;

    // Shared mask/mix coverage; scene-linear RGB is never clamped.
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        ivec2 maskPixel = ivec2(p) + inputGeometry[2].regionAndOffset.zw;
        ivec2 maskExtent = ivec2(inputGeometry[2].extent.xy);
        bool maskInside =
            maskPixel.x >= 0 && maskPixel.y >= 0 && maskPixel.x < maskExtent.x && maskPixel.y < maskExtent.y;
        float selected =
            maskInside ? clamp(gpuLoadRgba(in_mask, maskPixel, inputGeometry[2].rgba, maskExtent.y,
                                           inputGeometry[2].channels.y)[channel], 0.0, 1.0)
                       : 0.0;
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the background, weight 1 the
    // unmasked composite, so Mix 0 or zero coverage returns the background.
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? bg : (weight >= 1.0 ? composite : mix(bg, composite, weight));
    // The background is this node's main input: every stored channel the composite did
    // not write keeps its named channel from A at the same coordinate (#90).
    gpuStorePixel(out_color, ivec2(p), int(meta2.y), channels.x, channels.y, channels.z, rgba, result, in_a,
                  bgExtent.y, inputGeometry[0].channels.y);
}
)GLSL";

[[nodiscard]] EffectPassDefinition mergePass() {
    return EffectPassDefinition{
        .id = "merge",
        .shader = "merge/merge",
        .glsl = nemo::nodes::gpuGlsl(kMergeGlslPayload, kMergeGlslBody),
        // Declared port order: A, B, optional mask.
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Input, 1},
                   EffectImageRef{EffectImageKind::Input, 2}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: shared typed metadata (operation, mask)
// becomes this node's payload.
[[nodiscard]] GpuPreparation prepareMerge(const GpuNodeContext& context) {
    const std::array<float, 4> mask =
        nemo::nodes::gpuMaskWord(context.catalog, context.node, context.effectiveParams, context.maskPresent);
    const auto operation =
        static_cast<int>(effectiveMergeOperation(context.catalog, context.node, context.effectiveParams));

    MergePayload payload;
    std::copy(mask.begin(), mask.end(), payload.mask);
    payload.op[0] = static_cast<float>(operation);

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution mergeGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::mergeContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.merge.payload.v1";
    implementation.payloadSize = sizeof(MergePayload);
    implementation.passes = {mergePass()};
    implementation.prepare = &prepareMerge;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
