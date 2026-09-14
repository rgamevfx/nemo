// Read (source): real-media fill (issue #83 node-local GPU module).
//
// Decoding is the shared runtime's job (the source session resolves the
// effective source request); this module only receives the decoded frame's
// dimensions, packs them into its own payload, and declares the local pass
// that samples the decoded frame at set 1 binding 0. The frame itself never
// passes through node preparation, so no GPU handle, allocation, or wait
// enters the preparation callback.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: the decoded frame dimensions the fill ratio needs.
struct SourcePayload {
    float sourceWidth{0.0F};
    float sourceHeight{0.0F};
    float reserved0{0.0F};
    float reserved1{0.0F};
};
static_assert(sizeof(SourcePayload) % 16 == 0);

constexpr const char* kSourceGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform SourcePayload {
    // xy = decoded frame dimensions, zw = 0
    vec4 sourceSize;
};
)GLSL";

constexpr const char* kSourceGlslBody = R"GLSL(
// Real-media source fill (issue #11): the decoded, scene-linear,
// full-resolution frame (set 1 binding 0) covers the composition frame
// implied by the request — dimensions (region.x + width, region.y + height)
// — exactly like the CPU reference anchor. Output pixel p of the reduced
// raster maps to full-res composition coordinate (meta.z + p.x*scale,
// meta.w + p.y*scale), then to the nearest source pixel by the fill ratio.
// At scale 1 over a same-size frame this is the identity map. The source
// keeps FULL coordinate semantics while the raster is reduced: the same
// representation request always reads the same source pixels.
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_source;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    int scale = int(meta2.z);
    int fullX = int(meta.z) + int(p.x) * scale;
    int fullY = int(meta.w) + int(p.y) * scale;
    int fullWidth = int(meta.x);
    int fullHeight = int(meta.y);
    int srcWidth = int(sourceSize.x);
    int srcHeight = int(sourceSize.y);
    // Integer nearest fill: floor(full * src / full) clamped into the
    // source. Plain 32-bit math: full <= 2*kMaxDimension and src <= 64k
    // bound the product well below 2^31.
    ivec2 s = ivec2(clamp((fullX * srcWidth) / max(fullWidth, 1), 0, srcWidth - 1),
                    clamp((fullY * srcHeight) / max(fullHeight, 1), 0, srcHeight - 1));
    imageStore(out_color, ivec2(p), imageLoad(in_source, s));
}
)GLSL";

[[nodiscard]] EffectPassDefinition sourcePass() {
    return EffectPassDefinition{
        .id = "source",
        .shader = "source/source",
        .glsl = nemo::nodes::gpuGlsl(kSourceGlslPayload, kSourceGlslBody),
        // The decoded frame is supplied by the shared source session, not by a
        // graph edge: external reference 0.
        .inputs = {EffectImageRef{EffectImageKind::External, 0}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: decoded dimensions in, payload out. The
// shared runtime already resolved and decoded the frame; preparation must not
// touch it.
[[nodiscard]] GpuPreparation prepareSource(const GpuNodeContext& context) {
    if (context.sourceWidth == 0 || context.sourceHeight == 0) {
        throw std::runtime_error("source fill has no decoded frame dimensions "
                                 "(the shared source runtime did not supply a decoded frame)");
    }
    SourcePayload payload;
    payload.sourceWidth = static_cast<float>(context.sourceWidth);
    payload.sourceHeight = static_cast<float>(context.sourceHeight);

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution sourceGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::sourceContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.source.payload.v1";
    implementation.payloadSize = sizeof(SourcePayload);
    implementation.passes = {sourcePass()};
    implementation.prepare = &prepareSource;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
