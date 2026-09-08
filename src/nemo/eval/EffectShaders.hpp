#pragma once

#include <string>

namespace nemo::eval {

// GLSL reference implementations of the initial effect inventory (issue #8,
// spec section 10.4). These are the runtime-compiled (glslang) equivalents
// of the build-time Slang kernels in src/nemo/gpu/shaders — same binding
// contract, same math, same declared semantics — used to demonstrate that
// equivalent effects from both shader front ends execute on the same
// contract and agree within declared tolerances.
//
// The Slang kernels are the native path; these GLSL sources exist to prove
// the contract is front-end-agnostic, exactly as OCIO's generated GLSL runs
// through the same runtime-compile seam (issue #6).
//
// Binding contract (shared with the Slang kernels):
//   set 0, binding 0 : EffectUniforms (std140, uint4/float4 words only)
//   set 1, binding n : input image2D (rgba32f, straight alpha)
//   set 2, binding 0 : output image2D (rgba32f)
// Every kernel bounds-checks against meta.xy (dispatch covers ceil-to-8
// groups; the guard is part of the declared bounds contract).

inline constexpr const char* kGlslPreamble = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    uvec4 meta;   // region width, height, region.x, region.y
    vec4 misc;    // localTime in x
    vec4 param0;
    vec4 param1;
};
)GLSL";

inline constexpr const char* kGlslTestpattern = R"GLSL(
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    // Full-image coordinate frame, identical to the CPU reference anchor.
    int fullX = int(meta.z) + int(p.x);
    int fullY = int(meta.w) + int(p.y);
    int fullWidth = int(meta.z) + int(meta.x);
    int fullHeight = int(meta.w) + int(meta.y);
    float u = fullWidth > 1 ? float(fullX) / float(fullWidth - 1) : 0.0;
    float v = fullHeight > 1 ? float(fullY) / float(fullHeight - 1) : 0.0;
    int barWidth = max(2, fullWidth / 16);
    int barPos = (int(misc.x) * (fullWidth / 8)) % (fullWidth + barWidth);
    bool inBar = fullX >= barPos && fullX < barPos + barWidth;
    imageStore(out_color, ivec2(p), vec4(u, v, inBar ? 1.0 : 0.0, 1.0));
}
)GLSL";

inline constexpr const char* kGlslConstcolor = R"GLSL(
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    imageStore(out_color, ivec2(p), param0);
}
)GLSL";

inline constexpr const char* kGlslMerge = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_a;  // port A: over base
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_b;  // port B: over source
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    // Straight-alpha "over", exactly the CPU reference expression. A
    // premultiplied interpretation is a declared comparison failure.
    vec4 bg = imageLoad(in_a, ivec2(p));
    vec4 fg = imageLoad(in_b, ivec2(p));
    vec4 result;
    result.xyz = fg.a * fg.xyz + (1.0 - fg.a) * bg.xyz;
    result.w = fg.a + (1.0 - fg.a) * bg.a;
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

inline constexpr const char* kGlslOutput = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_color;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    imageStore(out_color, ivec2(p), imageLoad(in_color, ivec2(p)));
}
)GLSL";

}  // namespace nemo::eval
