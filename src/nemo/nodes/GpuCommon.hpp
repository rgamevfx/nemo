#pragma once

// Shared GPU-local helpers for the built-in node modules (issue #83).
//
// The binding contract itself is owned by nemo/eval/GpuContribution.hpp and
// nemo/eval/GpuExecutor.hpp: set 0 binding 0 common request, set 0 binding 1
// node-local payload, set 0 binding 2 per-input spatial geometry, set 0 binding
// 3 the produced raster's channel plan, set 1 binding n pass inputs (array
// order), set 2 binding 0 result, set 3 binding 0 float weight storage. That
// contract is versioned: node modules and the executor move together (issues
// #90, #98, "nemo.native.bindings.v7"), and a node-local payload or shader whose
// layout changed carries its own new payload layout identity.
//
// This header only removes duplication that every node module would otherwise
// repeat: the common request block each node-local GLSL reference pass declares,
// the shared per-input geometry and channel blocks, the native channel image
// gather/scatter helpers, and the shared mask word both front ends consume
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
// node output's described data support in RASTER-INDEX coordinates: a sample
// outside it must be written transparent black, while (-1,-1,-1,-1) means the
// raster declares no support (a node-local scratch) and writes in full.
// `channels`/`rgba` are the produced raster's channel facts (issues #90, #98):
// its stored channel count, its components per texel, the mask of channels the
// executor's channel plan fills, and the stored channel of each R/G/B/A role —
// all resolved by the executor before any dispatch. A pass with its own payload
// appends its block at set 0 binding 1.
inline constexpr const char* kGpuRequestGlsl = R"GLSL(
#version 450
// A native image is a formatless storage image (issues #90, #98): the packed
// RGBA32F four-channel representation or the R32_SFLOAT scalar plane
// representation, bound with no format qualifier, so reading it needs the
// explicit "formatted load" form: the device feature the executor requires
// (shaderStorageImageReadWithoutFormat) is exactly what makes this legal, and
// the format qualifier is deliberately absent because the binding the executor
// creates carries no format.
#extension GL_EXT_shader_image_load_formatted : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    ivec4 meta;      // full image width/height, pass region.x/region.y (signed)
    uvec4 meta2;     // raster: pass width, pass height (LOGICAL), samplingScale, 0
    vec4 misc;       // localTime in x
    ivec4 support;   // half-open raster-index rect of the samples that are data
    // channels: the produced raster's channel facts. x = stored channel count,
    // y = components per texel (4 = packed RGBA32F at the logical raster, 1 =
    // R32_SFLOAT scalar planes), z = whether the executor's channel plan fills
    // any stored channel (0 = this pass's own RGBA math produces every one of
    // them, so the plan buffer is never read), w = 0.
    uvec4 channels;
    // rgba: the produced raster's stored channel for each R,G,B,A role; -1 =
    // role absent. A role is NOT implied by the stored channel count.
    ivec4 rgba;
};

// Output-support guard (issue #88). Declared at global scope so a kernel's own
// locals can never shadow the request's support word. `support.x < 0` declares
// no support — a node-local scratch raster, whose only consumer is the node's
// next pass — and masks nothing. A zero-area support (an empty data window)
// masks every sample, so the produced raster is fully transparent: an empty data
// window is a valid connected image, not an absent input. Only the written
// VALUE is guarded; dispatch extent, coverage, region, sampling scale and every
// read stay exactly as requested.
bool gpuHasData(ivec2 p) {
    return support.x < 0 ||
           (p.x >= support.x && p.y >= support.y && p.x < support.x + support.z &&
            p.y < support.y + support.w);
}

vec4 gpuSupported(vec4 value, ivec2 p) {
    return gpuHasData(p) ? value : vec4(0.0);
}
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
// `extent.xy` is the input's LOGICAL raster size (issues #90, #98) and
// `channels` carries the input's ACTUAL storage — its stored channel count and
// its components per texel, derived from the image's real format, never guessed
// from a description. `rgba` is the stored channel of each R/G/B/A role in that
// image (-1 absent), so a kernel never looks a name up per pixel.
inline constexpr const char* kGpuInputGeometryGlsl = R"GLSL(
struct EffectInputGeometry {
    ivec4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uvec4 extent;           // LOGICAL raster width/height, samplingScale, 1
    ivec4 rgba;             // stored channel per R,G,B,A role; -1 = role absent
    uvec4 channels;         // (ACTUAL stored channel count, ACTUAL components per texel, 0, 0)
};
layout(std430, set = 0, binding = 2) readonly buffer EffectInputGeometries {
    EffectInputGeometry inputGeometry[];
};
)GLSL";

// Native channel images (issues #90, #98, binding contract v7). A native image
// carries every STORED channel of its raster, in stored order, and never drops
// or pads one. Its representation follows from the stored channel count alone:
//
//   components == 4  PACKED RGBA32F at the LOGICAL extent W×H: texel (x, y)
//                    holds stored channels 0..3 of logical pixel (x, y) in its
//                    R,G,B,A components, so a whole pixel is ONE vector load or
//                    store. The four stored channels are NOT assumed to be
//                    R,G,B,A — `rgba`/`rolePlanes` is the only authority on
//                    which stored channel plays which role, so a noncanonical
//                    order is read and written in place.
//   components == 1  every other count: R32_SFLOAT scalar planes at W×(H*C),
//                    stored channel c of logical pixel (x, y) at
//                    (x, y + c*planeHeight), so one-, two- and three-channel
//                    data is never expanded into four components.
//
// Every image is bound as a FORMATLESS storage image (the device features the
// executor requires), so ONE declaration reads and writes both representations:
// a scalar plane is component .x of its own texel, a packed pixel is the whole
// texel. `planeHeight` is always the LOGICAL height; it is unused for a packed
// image, whose channels share one texel.
//
// The projection rules are exactly `CpuImage::pixel`'s: a missing R/G/B role
// reads 0.0, a missing A role reads 1.0 when the image carries at least one RGB
// role and 0.0 when it carries none, and a write goes only to roles the image
// actually stores — no named RGB or alpha is ever manufactured.
//
// The channel plan (set 0 binding 3, one entry per stored channel, resolved by
// the executor from the described channel names) names, for every channel this
// pass's own RGBA math does not write, the source channel in the pass's set-1
// binding 0 image: >= 0 that stored channel, -2 numeric zero (the plan never
// leaves a produced channel undefined). Filling the planned channels preserves
// named auxiliary channels at UNCHANGED coordinates, and the request's
// `channels.z` says whether the plan fills anything at all, so a pass whose
// raster holds only its own roles never reads the plan buffer.
//
// A PACKED produced raster holds its whole pixel in one texel, so the kernel
// assembles the pixel in a register (`gpuPixelChannel`) and stores it ONCE
// (`gpuPixelStore`, or `gpuStorePixel`, which does both and fills the planned
// channels); a SCALAR raster stores each channel in its own plane as it is
// written and the register stays unused. Both shapes are the same kernel source.
inline constexpr const char* kGpuChannelImageGlsl = R"GLSL(
layout(std430, set = 0, binding = 3) readonly buffer EffectChannelPlans {
    ivec2 channelPlan[];
};

ivec2 gpuPlanePixel(ivec2 p, int channel, int planeHeight) {
    return ivec2(p.x, p.y + channel * planeHeight);
}

float gpuLoadChannel(readonly image2D src, ivec2 p, int channel, int planeHeight, uint components) {
    if (components == 4u) { return imageLoad(src, ivec2(p))[clamp(channel, 0, 3)]; }
    return imageLoad(src, gpuPlanePixel(p, channel, planeHeight)).x;
}

vec4 gpuLoadRgba(readonly image2D src, ivec2 p, ivec4 rolePlanes, int planeHeight, uint components) {
    if (components == 4u) {
        const vec4 texel = imageLoad(src, ivec2(p));
        if (all(equal(rolePlanes, ivec4(0, 1, 2, 3)))) { return texel; }
        return vec4(rolePlanes.x >= 0 ? texel[clamp(rolePlanes.x, 0, 3)] : 0.0,
                    rolePlanes.y >= 0 ? texel[clamp(rolePlanes.y, 0, 3)] : 0.0,
                    rolePlanes.z >= 0 ? texel[clamp(rolePlanes.z, 0, 3)] : 0.0,
                    rolePlanes.w >= 0 ? texel[clamp(rolePlanes.w, 0, 3)]
                                      : ((rolePlanes.x >= 0 || rolePlanes.y >= 0 || rolePlanes.z >= 0) ? 1.0 : 0.0));
    }
    vec4 value = vec4(0.0, 0.0, 0.0, (rolePlanes.x >= 0 || rolePlanes.y >= 0 || rolePlanes.z >= 0) ? 1.0 : 0.0);
    if (rolePlanes.x >= 0) { value.x = gpuLoadChannel(src, p, rolePlanes.x, planeHeight, components); }
    if (rolePlanes.y >= 0) { value.y = gpuLoadChannel(src, p, rolePlanes.y, planeHeight, components); }
    if (rolePlanes.z >= 0) { value.z = gpuLoadChannel(src, p, rolePlanes.z, planeHeight, components); }
    if (rolePlanes.w >= 0) { value.w = gpuLoadChannel(src, p, rolePlanes.w, planeHeight, components); }
    return value;
}

void gpuStorePlane(writeonly image2D dst, ivec2 p, int planeHeight, int channel, float value) {
    imageStore(dst, gpuPlanePixel(p, channel, planeHeight), gpuSupported(vec4(value), p));
}

void gpuPixelChannel(inout vec4 pixel, writeonly image2D dst, ivec2 p, int planeHeight, uint components,
                     int channel, float value) {
    if (components == 4u) {
        pixel[clamp(channel, 0, 3)] = value;
        return;
    }
    gpuStorePlane(dst, p, planeHeight, channel, value);
}

void gpuPixelStore(writeonly image2D dst, ivec2 p, uint components, vec4 pixel) {
    if (components == 4u) { imageStore(dst, ivec2(p), gpuSupported(pixel, p)); }
}

void gpuStoreRgba(writeonly image2D dst, ivec2 p, int planeHeight, uint components, ivec4 rolePlanes, vec4 value) {
    if (components == 4u) {
        if (all(equal(rolePlanes, ivec4(0, 1, 2, 3)))) {
            imageStore(dst, ivec2(p), gpuSupported(value, p));
            return;
        }
        vec4 pixel = vec4(0.0);
        if (rolePlanes.x >= 0) { pixel[clamp(rolePlanes.x, 0, 3)] = value.x; }
        if (rolePlanes.y >= 0) { pixel[clamp(rolePlanes.y, 0, 3)] = value.y; }
        if (rolePlanes.z >= 0) { pixel[clamp(rolePlanes.z, 0, 3)] = value.z; }
        if (rolePlanes.w >= 0) { pixel[clamp(rolePlanes.w, 0, 3)] = value.w; }
        imageStore(dst, ivec2(p), gpuSupported(pixel, p));
        return;
    }
    if (rolePlanes.x >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.x, value.x); }
    if (rolePlanes.y >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.y, value.y); }
    if (rolePlanes.z >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.z, value.z); }
    if (rolePlanes.w >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.w, value.w); }
}

void gpuStorePixel(writeonly image2D dst, ivec2 p, int planeHeight, uint channelCount, uint components,
                   uint planChannels, ivec4 rolePlanes, vec4 value, readonly image2D src, int srcPlaneHeight,
                   uint srcComponents) {
    if (components == 4u && planChannels == 0u && all(equal(rolePlanes, ivec4(0, 1, 2, 3)))) {
        imageStore(dst, ivec2(p), gpuSupported(value, p));
        return;
    }
    vec4 pixel = vec4(0.0);
    if (rolePlanes.x >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.x, value.x); }
    if (rolePlanes.y >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.y, value.y); }
    if (rolePlanes.z >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.z, value.z); }
    if (rolePlanes.w >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.w, value.w); }
    if (planChannels != 0u) {
        const ivec2 q = p + inputGeometry[0].regionAndOffset.zw;
        const ivec2 extent = ivec2(inputGeometry[0].extent.xy);
        const bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
        for (uint channel = 0u; channel < channelCount; ++channel) {
            const int source = channelPlan[channel].x;
            if (source == -1) { continue; }
            const float planned =
                (source >= 0 && inside) ? gpuLoadChannel(src, q, source, srcPlaneHeight, srcComponents) : 0.0;
            gpuPixelChannel(pixel, dst, p, planeHeight, components, int(channel), planned);
        }
    }
    gpuPixelStore(dst, p, components, pixel);
}

void gpuZeroPlanes(writeonly image2D dst, ivec2 p, int planeHeight, uint components, uint channelCount) {
    if (components == 4u) {
        imageStore(dst, ivec2(p), vec4(0.0));
        return;
    }
    for (uint channel = 0u; channel < channelCount; ++channel) {
        imageStore(dst, gpuPlanePixel(p, int(channel), planeHeight), vec4(0.0));
    }
}
)GLSL";

// Complete GLSL reference source of one local pass: the common request block,
// the per-input geometry block, the native channel image block, the pass's own
// payload declarations, then the pass body. Every pass declares all three
// shared blocks (and the executor binds them for every pass), so a kernel that
// uses only some of them is never a special case.
[[nodiscard]] inline std::string gpuGlsl(std::string_view payloadDeclarations, std::string_view body) {
    std::string source{kGpuRequestGlsl};
    source.append(kGpuInputGeometryGlsl);
    source.append(kGpuChannelImageGlsl);
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
