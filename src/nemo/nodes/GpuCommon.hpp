#pragma once

// Shared GPU-local helpers for the built-in node modules (issue #83).
//
// The binding contract itself is owned by nemo/eval/GpuContribution.hpp and
// nemo/eval/GpuExecutor.hpp: set 0 binding 0 common request, set 0 binding 1
// node-local payload, set 0 binding 2 per-input spatial geometry, set 0 binding
// 3 the produced raster's channel plan, set 1 binding n pass inputs (array
// order), set 2 binding 0 result, set 3 binding 0 float weight storage. That
// contract is versioned: node modules and the executor move together (issue
// #90, "nemo.native.bindings.v6"), and a node-local payload or shader whose
// layout changed carries its own new payload layout identity.
//
// This header only removes duplication that every node module would otherwise
// repeat: the common request block each node-local GLSL reference pass declares,
// the shared per-input geometry and channel-plan blocks, the native channel
// plane gather/scatter helpers, and the shared mask word both front ends consume
// verbatim.
//
// It deliberately contains no pixel math. The CPU, Slang, and GLSL
// implementations of an effect stay independent references, so their agreement
// remains evidence instead of a shared mistake.

#include <array>
#include <string>
#include <string_view>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/eval/GpuContribution.hpp"

namespace nemo::nodes {

// Common request block (set 0, binding 0) of every node-local GLSL reference
// pass: the version/local-size preamble plus the request words with exactly the
// meaning EffectRequestUniforms gives them. `meta` is SIGNED: a region request
// may address negative or beyond-format coordinates (issue #88), so the pass
// region origin is an int, not a bit pattern to reinterpret. `support` is the
// node output's described data support in RASTER-INDEX coordinates (native
// binding contract v6): a sample outside it must be written transparent black,
// while (-1,-1,-1,-1) means the raster declares no support (a node-local
// scratch) and writes in full. `channels`/`rgba` are the produced raster's
// channel-plane facts (issue #90): its plane count and the plane index of each
// R/G/B/A projection role, resolved by the executor from the described channel
// names before any dispatch. A pass with its own payload appends its block at
// set 0 binding 1.
inline constexpr const char* kGpuRequestGlsl = R"GLSL(
#version 450
// A native image is an R32_SFLOAT plane image bound as an unformatted storage
// image (issue #90), so reading it needs the explicit "formatted load" form:
// the device feature the executor requires (shaderStorageImageReadWithoutFormat)
// is exactly what makes this legal, and the format qualifier is deliberately
// absent because the binding the executor creates carries no format.
#extension GL_EXT_shader_image_load_formatted : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    ivec4 meta;      // full image width/height, pass region.x/region.y (signed)
    uvec4 meta2;     // raster: pass width, pass height (LOGICAL), samplingScale, 0
    vec4 misc;       // localTime in x
    ivec4 support;   // half-open raster-index rect of the samples that are data
    uvec4 channels;  // produced channel planes: (plane count, 0, 0, 0)
    ivec4 rgba;      // produced plane index per R,G,B,A role; -1 = role absent
};

// Output-support guard (issue #88, native binding contract v6). Declared at
// global scope so a kernel's own locals can never shadow the request's support
// word. `support.x < 0` declares no support — a node-local scratch raster, whose
// only consumer is the node's next pass — and masks nothing. A zero-area support
// (an empty data window) masks every sample, so the produced raster is fully
// transparent: an empty data window is a valid connected image, not an absent
// input. Only the written VALUE is guarded; dispatch extent, coverage, region,
// sampling scale and every read stay exactly as requested.
bool gpuHasData(ivec2 p) {
    return support.x < 0 ||
           (p.x >= support.x && p.y >= support.y && p.x < support.x + support.z &&
            p.y < support.y + support.w);
}

vec4 gpuSupported(vec4 value, ivec2 p) {
    return gpuHasData(p) ? value : vec4(0.0);
}

// Keep the image operand at the callsite: passing a format-qualified storage
// image through an unformatted image2D function parameter produces invalid
// SPIR-V on the reference compiler.
#define gpuStore(target, p, value) imageStore(target, p, gpuSupported(value, p))
)GLSL";

// Per-input spatial geometry (set 0, binding 2) of every pass that samples an
// image, in the layout EffectInputGeometry declares. Index it by the same
// number the pass binds that input at (set 1, binding n): `p + offset` is the
// input raster pixel that holds the same full-resolution sample as pass output
// pixel `p`, and `extent.xy` bounds the taps of a neighborhood read. An index
// that falls outside `[0, extent.xy)` is outside the input's data — transparent
// black, never an out-of-bounds read — because an input may hold a different,
// smaller or empty rectangle of the same lattice (issue #88). An external
// source frame (EffectImageKind::External) is described the same way, with its
// coverage's full-resolution origin in `regionAndOffset.xy` and its raster
// extent in `extent.xy`.
//
// `extent.xy` is the input's LOGICAL raster size (issue #90): a channel-plane
// image is `(extent.x, extent.y * channels.x)` texels on the device, and plane
// `c` starts at device row `c * extent.y`. `rgba` is the plane index of each
// R/G/B/A role in that image (-1 absent), so a kernel never looks a name up per
// pixel.
inline constexpr const char* kGpuInputGeometryGlsl = R"GLSL(
struct EffectInputGeometry {
    ivec4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uvec4 extent;           // LOGICAL raster width/height, samplingScale, 1
    ivec4 rgba;             // plane index per R,G,B,A role; -1 = role absent
    uvec4 channels;         // (DEVICE channel plane count, 0, 0, 0)
};
layout(std430, set = 0, binding = 2) readonly buffer EffectInputGeometries {
    EffectInputGeometry inputGeometry[];
};
)GLSL";

// Native channel planes (issue #90, binding contract v6). One device image
// carries every named channel of a raster as a vertical plane: logical pixel
// `(x, y)` and channel `c` live at `(x, y + c*planeHeight)`, and dispatch stays
// the LOGICAL raster. The projection rules are exactly `CpuImage::pixel`'s: a
// missing R/G/B role reads 0.0, a missing A role reads 1.0 when the image
// carries at least one RGB role and 0.0 when it carries none, and a write goes
// only to roles the image actually stores — no named RGB or alpha is ever
// manufactured.
//
// The channel plan (set 0 binding 3, one entry per produced plane) is resolved
// by the executor from the described channel names. Every plane this pass's own
// RGBA math does not write is filled from the pass's set-1 binding 0 image at
// UNCHANGED lattice coordinates, so named auxiliary channels survive an effect
// that does not select them, and a plane with no source in that image is
// numeric zero rather than undefined memory.
inline constexpr const char* kGpuChannelPlanGlsl = R"GLSL(
layout(std430, set = 0, binding = 3) readonly buffer EffectChannelPlans {
    // >= 0: source plane in this pass's set-1 binding 0 image.
    // -1: this pass's own math produces the plane.
    // -2: the plane has no source and is numeric zero.
    ivec2 channelPlan[];
};

ivec2 gpuPlanePixel(ivec2 p, int plane, int planeHeight) {
    return ivec2(p.x, p.y + plane * planeHeight);
}

float gpuLoadPlane(readonly image2D src, ivec2 p, int plane, int planeHeight) {
    return imageLoad(src, gpuPlanePixel(p, plane, planeHeight)).x;
}

// Store one plane value under the produced raster's data support (issue #88):
// a sample outside it stays transparent black, never a computed value.
void gpuStorePlane(writeonly image2D dst, ivec2 p, int plane, int planeHeight, float value) {
    imageStore(dst, gpuPlanePixel(p, plane, planeHeight), gpuSupported(vec4(value), p));
}

// The R/G/B/A projection of one sampled input image.
vec4 gpuLoadRgba(readonly image2D src, ivec2 p, ivec4 rolePlanes, int planeHeight) {
    vec4 value = vec4(0.0, 0.0, 0.0, (rolePlanes.x >= 0 || rolePlanes.y >= 0 || rolePlanes.z >= 0) ? 1.0 : 0.0);
    if (rolePlanes.x >= 0) { value.x = gpuLoadPlane(src, p, rolePlanes.x, planeHeight); }
    if (rolePlanes.y >= 0) { value.y = gpuLoadPlane(src, p, rolePlanes.y, planeHeight); }
    if (rolePlanes.z >= 0) { value.z = gpuLoadPlane(src, p, rolePlanes.z, planeHeight); }
    if (rolePlanes.w >= 0) { value.w = gpuLoadPlane(src, p, rolePlanes.w, planeHeight); }
    return value;
}

// Write the roles the produced raster actually carries; a role it does not
// store is dropped, never manufactured.
void gpuStoreRgba(writeonly image2D dst, ivec2 p, ivec4 rolePlanes, int planeHeight, vec4 value) {
    if (rolePlanes.x >= 0) { gpuStorePlane(dst, p, rolePlanes.x, planeHeight, value.x); }
    if (rolePlanes.y >= 0) { gpuStorePlane(dst, p, rolePlanes.y, planeHeight, value.y); }
    if (rolePlanes.z >= 0) { gpuStorePlane(dst, p, rolePlanes.z, planeHeight, value.z); }
    if (rolePlanes.w >= 0) { gpuStorePlane(dst, p, rolePlanes.w, planeHeight, value.w); }
}

// Fill every planned plane of the produced raster. `q` is the pixel of this
// pass's set-1 binding 0 image that holds the same full-resolution sample as
// output pixel `p`, and `inside` says whether that image holds it. A geometric
// (resampling) kernel MUST pass the UNCHANGED-coordinate pixel here, never its
// resampled one: auxiliary channels are preserved where they are, not
// transformed. Without a source plane, or outside the source's raster, the
// plane is numeric zero; the produced raster's data support still zeroes every
// sample outside it.
void gpuPreserveAux(writeonly image2D dst, ivec2 p, int dstPlaneHeight, readonly image2D src, ivec2 q, bool inside,
                    int srcPlaneHeight, uint planeCount) {
    for (uint plane = 0u; plane < planeCount; ++plane) {
        const int source = channelPlan[plane].x;
        if (source == -1) { continue; }  // this pass's own math produces the plane
        const float value = (source >= 0 && inside) ? gpuLoadPlane(src, q, source, srcPlaneHeight) : 0.0;
        gpuStorePlane(dst, p, int(plane), dstPlaneHeight, value);
    }
}

// The aux helper for a pass whose binding 0 image sits on the SAME sampling
// lattice as this pass's output: the source pixel is the same full-resolution
// coordinate, `p + inputGeometry[0].regionAndOffset.zw`.
void gpuPreserveAuxLattice(writeonly image2D dst, ivec2 p, int dstPlaneHeight, readonly image2D src, int srcPlaneHeight,
                           uint planeCount) {
    const ivec2 q = p + inputGeometry[0].regionAndOffset.zw;
    const ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    const bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
    gpuPreserveAux(dst, p, dstPlaneHeight, src, q, inside, srcPlaneHeight, planeCount);
}

// Every plane of the produced raster at transparent black. The outside-support
// branch of a kernel writes this instead of running its pixel math, so a sample
// outside the described data window — and every auxiliary plane — is
// initialized rather than left undefined device memory.
void gpuZeroPlanes(writeonly image2D dst, ivec2 p, int planeHeight) {
    for (uint plane = 0u; plane < channels.x; ++plane) {
        gpuStorePlane(dst, p, int(plane), planeHeight, 0.0);
    }
}
)GLSL";

// Complete GLSL reference source of one local pass: the common request block,
// the per-input geometry block, the native channel-plane block, the pass's own
// payload declarations, then the pass body. Every pass declares all three
// shared blocks (and the executor binds them for every pass), so a kernel that
// uses only some of them is never a special case.
[[nodiscard]] inline std::string gpuGlsl(std::string_view payloadDeclarations, std::string_view body) {
    std::string source{kGpuRequestGlsl};
    source.append(kGpuInputGeometryGlsl);
    source.append(kGpuChannelPlanGlsl);
    source.append(payloadDeclarations);
    source.append(body);
    return source;
}

// The shared mask/mix word every masked effect packs into its own payload:
// (maskChannel [-1 none, 0 R, 1 G, 2 B, 3 A], invertMask, mix, maskPresent).
// The interpretation is shared typed metadata (Params.hpp); only the packing
// is shared, never the pixel operation that consumes it.
[[nodiscard]] inline std::array<float, 4> gpuMaskWord(const NodeCatalog& catalog, const NodeInstance& node,
                                                      const ParameterValues& effectiveParams, bool maskPresent) {
    const EffectMaskParameters mask = effectiveEffectMask(catalog, node, effectiveParams);
    return {static_cast<float>(mask.channel), mask.invert ? 1.0F : 0.0F, mask.mix, maskPresent ? 1.0F : 0.0F};
}

}  // namespace nemo::nodes
