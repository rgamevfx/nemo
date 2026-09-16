#pragma once

// Shared GPU-local helpers for the built-in node modules (issue #83).
//
// The binding contract itself is owned by nemo/eval/GpuContribution.hpp and
// nemo/eval/GpuExecutor.hpp: set 0 binding 0 common request, set 0 binding 1
// node-local payload, set 0 binding 2 per-input spatial geometry, set 1 binding
// n pass inputs (array order), set 2 binding 0 result, set 3 binding 0 float
// weight storage. That contract is versioned: node modules and the executor move
// together (issue #88, "nemo.native.bindings.v5"), and a node-local payload or
// shader whose layout changed carries its own new payload layout identity.
//
// This header only removes duplication that every node module would otherwise
// repeat: the common request block each node-local GLSL reference pass declares,
// and the shared mask word both front ends consume verbatim.
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
// binding contract v5): a sample outside it must be written transparent black,
// while (-1,-1,-1,-1) means the raster declares no support (a node-local
// scratch) and writes in full. A pass with its own payload appends its block at
// set 0 binding 1.
inline constexpr const char* kGpuRequestGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    ivec4 meta;     // full image width/height, pass region.x/region.y (signed)
    uvec4 meta2;    // raster: pass width, pass height, samplingScale, 0
    vec4 misc;      // localTime in x
    ivec4 support;  // half-open raster-index rect of the samples that are data
};

// Output-support guard (issue #88, native binding contract v5). Declared at
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
inline constexpr const char* kGpuInputGeometryGlsl = R"GLSL(
struct EffectInputGeometry {
    ivec4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uvec4 extent;           // raster width/height, samplingScale, 1
};
layout(std430, set = 0, binding = 2) readonly buffer EffectInputGeometries {
    EffectInputGeometry inputGeometry[];
};
)GLSL";

// Complete GLSL reference source of one local pass: the common request block,
// the per-input geometry block for a pass that samples images, the pass's own
// payload block (empty when the node declares no payload), then the pass body.
[[nodiscard]] inline std::string gpuGlsl(std::string_view payloadDeclarations, std::string_view body,
                                         bool samplesImages) {
    std::string source{kGpuRequestGlsl};
    if (samplesImages)
        source.append(kGpuInputGeometryGlsl);
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
