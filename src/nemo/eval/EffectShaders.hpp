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
// Every kernel bounds-checks against meta2.xy (the dispatched raster's
// actual dimensions; the guard is part of the declared bounds contract).
//
// Representation contract (issue #11, spec section 8/10.4): a request's
// Region is FULL-RESOLUTION; the executed raster is the region sampled at
// `samplingScale` (1, 2, or 4), so image2D dimensions are
// ceil(region.width/scale) x ceil(region.height/scale) while every
// coordinate semantic stays full-resolution:
//   meta  = (full image width, full image height, region.x, region.y)
//   meta2 = (image width, image height, samplingScale, 0)      [raster]
//   misc  = (localTime, 0, 0, 0); param0/param1 effect-specific.

inline constexpr const char* kGlslPreamble = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    uvec4 meta;   // full image width/height, region.x/region.y
    uvec4 meta2;  // raster: image width, image height, samplingScale, 0
    vec4 misc;    // localTime in x
    vec4 param0;
    vec4 param1;
};
)GLSL";

inline constexpr const char* kGlslTestpattern = R"GLSL(
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // Full-resolution coordinate frame (issue #11): the reduced raster
    // samples the frame at fullX = region.x + p.x * scale, so the pattern
    // is the SAME image every representation — not a smaller replica.
    int scale = int(meta2.z);
    int fullX = int(meta.z) + int(p.x) * scale;
    int fullY = int(meta.w) + int(p.y) * scale;
    int fullWidth = int(meta.x);
    int fullHeight = int(meta.y);
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
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    imageStore(out_color, ivec2(p), param0);
}
)GLSL";

inline constexpr const char* kGlslMerge = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_a;  // port A: over base
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_b;  // port B: over source
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
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
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    imageStore(out_color, ivec2(p), imageLoad(in_color, ivec2(p)));
}
)GLSL";

inline constexpr const char* kGlslSource = R"GLSL(
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
    int srcWidth = int(param0.x);
    int srcHeight = int(param0.y);
    // Integer nearest fill: floor(full * src / full) clamped into the
    // source. Plain 32-bit math: full <= 2*kMaxDimension and src <= 64k
    // bound the product well below 2^31.
    ivec2 s = ivec2(clamp((fullX * srcWidth) / max(fullWidth, 1), 0, srcWidth - 1),
                    clamp((fullY * srcHeight) / max(fullHeight, 1), 0, srcHeight - 1));
    imageStore(out_color, ivec2(p), imageLoad(in_source, s));
}
)GLSL";

}  // namespace nemo::eval
