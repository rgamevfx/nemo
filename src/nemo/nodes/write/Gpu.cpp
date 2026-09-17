// Write: the delivery sink adapter (issue #94 slice, modelled on the Output
// node-local GPU module of issue #83).
//
// Write owns a plain one-pass copy of port 0 to the node result, exactly like
// the CPU adapter: the node's pixel behavior is a pass-through and the file
// delivery itself is a headless job submitted through nemo::eval::DeliveryQueue
// (issue #94), never a side effect of evaluation. It declares no payload: the
// kernel needs only the common request. The role-specific meaning of the node
// stays with the shared evaluator; this module only supplies its declared pixel
// behavior.

#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"

namespace nemo::eval::nodes {
namespace {

constexpr const char* kWriteGlsl = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_color;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // The described image's data support (native binding contract v7): a sample
    // outside it is transparent black, never an upstream value carried there,
    // and every stored channel — auxiliary ones included — is initialized (issue
    // #90).
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), int(meta2.y), channels.y, channels.x);
        return;
    }
    // The upstream result may cover a wider, differently anchored or smaller
    // rectangle of the same lattice, so the pass locates it through its own
    // origin and extent instead of assuming its own. A sample it does not hold
    // is outside its data: transparent black (issue #88), never an
    // out-of-bounds load.
    ivec2 q = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
    vec4 value = inside ? gpuLoadRgba(in_color, q, inputGeometry[0].rgba, extent.y,
                                      inputGeometry[0].channels.y) : vec4(0.0);
    // The adapter carries every described channel, not just the four roles
    // (issue #90).
    gpuStorePixel(out_color, ivec2(p), int(meta2.y), channels.x, channels.y, channels.z, rgba, value, in_color,
                  extent.y, inputGeometry[0].channels.y);
}
)GLSL";

[[nodiscard]] EffectPassDefinition writePass() {
    return EffectPassDefinition{
        .id = "write",
        .shader = "write/write",
        .glsl = nemo::nodes::gpuGlsl({}, kWriteGlsl),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

[[nodiscard]] GpuPreparation prepareWrite(const GpuNodeContext&) {
    GpuPreparation preparation;
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution writeGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::writeContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.passes = {writePass()};
    implementation.prepare = &prepareWrite;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
