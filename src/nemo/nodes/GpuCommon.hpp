#pragma once

// Shared GPU-local helpers for the built-in node modules (issue #83).
//
// The binding contract itself is owned by nemo/eval/GpuContribution.hpp and
// nemo/eval/GpuExecutor.hpp: set 0 binding 0 common request, set 0 binding 1
// node-local payload, set 1 binding n pass inputs (array order), set 2
// binding 0 result, set 3 binding 0 float weight storage. This header only
// removes duplication that every node module would otherwise repeat: the
// common request block each node-local GLSL reference pass declares, and the
// shared mask word both front ends consume verbatim.
//
// It deliberately contains no pixel math. The CPU, Slang, and GLSL
// implementations of an effect stay independent references, so their
// agreement remains evidence instead of a shared mistake.

#include <array>
#include <string>
#include <string_view>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/eval/GpuContribution.hpp"

namespace nemo::nodes {

// Common request block (set 0, binding 0) of every node-local GLSL reference
// pass: the version/local-size preamble plus the three request words with
// exactly the meaning EffectRequestUniforms gives them. A pass with its own
// payload appends its block at set 0 binding 1.
inline constexpr const char* kGpuRequestGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    uvec4 meta;   // full image width/height, pass region.x/region.y
    uvec4 meta2;  // raster: pass width, pass height, samplingScale, 0
    vec4 misc;    // localTime in x
};
)GLSL";

// Per-input spatial geometry (set 0, binding 2) of every pass that samples an
// image, in the layout EffectInputGeometry declares. Index it by the same
// number the pass binds that input at (set 1, binding n): `p + offset` is the
// input raster pixel that holds the same full-resolution sample as pass output
// pixel `p`, and `extent.xy` bounds the taps of a neighborhood read.
inline constexpr const char* kGpuInputGeometryGlsl = R"GLSL(
struct EffectInputGeometry {
    ivec4 regionAndOffset;  // input region origin x/y, raster offset x/y
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
                                                      ParameterValues& effectiveParams, bool maskPresent) {
    const EffectMaskParameters mask = effectiveEffectMask(catalog, node, effectiveParams);
    return {static_cast<float>(mask.channel), mask.invert ? 1.0F : 0.0F, mask.mix, maskPresent ? 1.0F : 0.0F};
}

}  // namespace nemo::nodes
