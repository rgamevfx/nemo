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
    // issue #34 native effects: append-only extension shared by EVERY
    // effect kernel. Existing member offsets are unchanged, so preexisting
    // kernels stay ABI-aligned.
    vec4 mask;             // (maskChannel, invertMask, mix, maskPresent)
    vec4 gradeBlackpoint;
    vec4 gradeWhitepoint;
    vec4 gradeLift;
    vec4 gradeGain;
    vec4 gradeMultiply;
    vec4 gradeOffset;
    vec4 gradeGamma;
    vec4 gradeFlags;       // (channels bitmask, reverse, clampBlack, clampWhite)
    vec4 blur;             // (size full-res, channels bitmask, 0, 0)
    vec4 transform;        // (translateX, translateY, scale, rotate degrees)
    vec4 transformFlags;   // (filter 0Cubic/1Linear/2Nearest, pixelAspect, cos, sin)
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

// ---------------------------------------------------------------------------
// issue #34: Grade / Blur (two separable passes) / Transform.
//
// All three share the declared common mask/mix controls:
//   coverage = 1 when the mask input is absent or maskChannel is none, else
//   clamp(selected stored channel, 0, 1) inverted when invertMask is set;
//   weight = coverage * mix; the output blends the original pixel at the
//   output coordinate with the fully processed pixel exactly once, after the
//   effect. The mask stays in output-raster space (it is never transformed).
//
// The main input is always set 1 binding 0; the mask is set 1 binding 1 and
// is bound to a valid dummy descriptor (the main image) with maskPresent=0
// when the optional slot is unconnected — no allocated fallback image.
// ---------------------------------------------------------------------------

inline constexpr const char* kGlslGrade = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_mask;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

// signedPow(0, p) = 0 and sign(x)*pow(abs(x), p): the explicit
// negative/HDR extension, not a claim of bitwise Nuke parity.
float signedPow(float x, float p) {
    return x == 0.0 ? 0.0 : sign(x) * pow(abs(x), p);
}

// Forward:  y = signedPow(a*x + b, 1/gamma)
// Reverse:  x = (signedPow(y, gamma) - b) / a
// a = (gain - lift) * multiply / (whitepoint - blackpoint)
// b = lift + offset - blackpoint * a
float gradeChannel(float x, int c) {
    const int channels = int(gradeFlags.x);
    if ((channels & (1 << c)) == 0) { return x; }  // disabled: exact pass-through
    const float blackpoint = gradeBlackpoint[c];
    const float whitepoint = gradeWhitepoint[c];
    const float a = ((gradeGain[c] - gradeLift[c]) * gradeMultiply[c]) / (whitepoint - blackpoint);
    const float b = gradeLift[c] + gradeOffset[c] - blackpoint * a;
    float y;
    if (gradeFlags.y > 0.5) {
        y = (signedPow(x, gradeGamma[c]) - b) / a;  // reverse requires nonzero a (validated)
    } else {
        y = signedPow(a * x + b, 1.0 / gradeGamma[c]);
    }
    // Clamps apply after the operation, before mask/mix (Foundry default black clamp).
    if (gradeFlags.z > 0.5 && y < 0.0) { y = 0.0; }
    if (gradeFlags.w > 0.5 && y > 1.0) { y = 1.0; }
    return y;
}

vec4 gradePixel(vec4 x) {
    vec4 y = x;
    for (int c = 0; c < 4; ++c) { y[c] = gradeChannel(x[c], c); }
    return y;
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    vec4 orig = imageLoad(in_main, ivec2(p));
    vec4 processed = gradePixel(orig);
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        float selected = clamp(imageLoad(in_mask, ivec2(p))[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel, so no HDR 0*inf cancellation occurs in mix().
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : mix(orig, processed, weight));
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

// Blur pass 1 of 2: horizontal support. size is the FULL-RESOLUTION support
// radius; the executed raster samples at samplingScale, so the raster support
// is ceil(size/samplingScale) and sample i sits at full-res offset
// i*samplingScale. sigma = size/3; fixed normalized weights; clamp-to-edge.
// size 0 is the exact identity (no shortcut, no normalization degeneracy).
//
// RGBA premultiplies straight RGB by alpha before filtering and is
// unpremultiplied once by the final (vertical) pass; RGB filters RGB only and
// preserves alpha; Alpha filters alpha only and preserves RGB.
//
// The normalized Gaussian weights are precomputed once per Blur preparation
// into a retained read-only storage buffer (set 3 binding 0, index
// i+support); the kernel never evaluates exp() per output pixel.
inline constexpr const char* kGlslBlurHorizontal = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;
layout(std430, set = 3, binding = 0) readonly buffer BlurWeights { float weights[]; };

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    int mode = int(blur.y);
    if (blur.x <= 0.0) {  // exact identity
        vec4 identity = imageLoad(in_main, ivec2(p));
        imageStore(out_color, ivec2(p), identity);
        return;
    }
    int width = int(meta2.x);
    int support = int(blur.z);
    vec4 acc = vec4(0.0);
    for (int i = -support; i <= support; ++i) {
        float w = weights[i + support];
        vec4 s = imageLoad(in_main, ivec2(clamp(int(p.x) + i, 0, width - 1), int(p.y)));
        if ((mode & 8) != 0) { s.rgb *= s.a; }  // RGBA: premultiply before filtering
        acc += w * s;
    }
    vec4 center = imageLoad(in_main, ivec2(p));
    if (mode == 7) {        // RGB: filter RGB, preserve original alpha
        acc.a = center.a;
    } else if (mode == 8) { // Alpha: filter alpha, preserve original RGB
        acc.rgb = center.rgb;
    }
    imageStore(out_color, ivec2(p), acc);
}
)GLSL";

// Blur pass 2 of 2: vertical support and the final result. Filters the
// intermediate of pass 1, converts back to straight alpha (RGBA), then
// applies the declared mask/mix exactly once against the original main pixel.
inline constexpr const char* kGlslBlur = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_scratch;
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_main;
layout(rgba32f, set = 1, binding = 2) restrict readonly uniform image2D in_mask;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;
layout(std430, set = 3, binding = 0) readonly buffer BlurWeights { float weights[]; };

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    int mode = int(blur.y);
    vec4 orig = imageLoad(in_main, ivec2(p));
    vec4 processed;
    if (blur.x <= 0.0) {  // exact identity (both passes are identity)
        processed = orig;
    } else {
        int height = int(meta2.y);
        int support = int(blur.z);
        vec4 acc = vec4(0.0);
        for (int i = -support; i <= support; ++i) {
            float w = weights[i + support];
            acc += w * imageLoad(in_scratch, ivec2(int(p.x), clamp(int(p.y) + i, 0, height - 1)));
        }
        if (mode == 15) {       // RGBA: unpremultiply once at the final output
            processed.a = acc.a;
            processed.rgb = acc.a != 0.0 ? acc.rgb / acc.a : vec3(0.0);
        } else if (mode == 7) { // RGB: filtered RGB, exact original alpha
            processed.rgb = acc.rgb;
            processed.a = orig.a;
        } else {                // Alpha: filtered alpha, exact original RGB
            processed.rgb = orig.rgb;
            processed.a = acc.a;
        }
    }
    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        float selected = clamp(imageLoad(in_mask, ivec2(p))[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in mix()).
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : mix(orig, processed, weight));
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

// Transform: image-center pivot (width/2, height/2), pixel centers x+0.5/y+0.5
// in FULL resolution, x right / y down, positive angle clockwise in the stored
// raster. Uniform scale, then rotation about the center in physical
// coordinates (x*pixelAspect, y), then translation. Inverse mapping samples
// the input; outside the image is transparent black. Interpolation is
// alpha-aware (premultiplied) and converted back to straight with zero-alpha
// RGB = 0; no implicit RGB clamp. Nearest emits floor(fullResCenter/scale).
// The mask stays in output space and is never transformed.
inline constexpr const char* kGlslTransform = R"GLSL(
layout(rgba32f, set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(rgba32f, set = 1, binding = 1) restrict readonly uniform image2D in_mask;
layout(rgba32f, set = 2, binding = 0) restrict writeonly uniform image2D out_color;

vec4 premultTexel(ivec2 q, ivec2 dims) {
    if (q.x < 0 || q.y < 0 || q.x >= dims.x || q.y >= dims.y) { return vec4(0.0); }
    vec4 s = imageLoad(in_main, q);
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
    vec4 orig = imageLoad(in_main, ivec2(p));

    int scale = int(meta2.z);
    float fullWidth = float(meta.x);
    float fullHeight = float(meta.y);
    float regionX = float(meta.z);
    float regionY = float(meta.w);
    float aspect = transformFlags.y;
    ivec2 dims = ivec2(meta2.xy);

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
    vec2 raster = vec2((fullIn.x - regionX) / float(scale), (fullIn.y - regionY) / float(scale));
    if (filterMode == 2) {  // Nearest: bound the raster index; outside is transparent black
        if (raster.x >= 0.0 && raster.x < float(dims.x) && raster.y >= 0.0 && raster.y < float(dims.y)) {
            processed = imageLoad(in_main, ivec2(floor(raster)));
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
        float selected = clamp(imageLoad(in_mask, ivec2(p))[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in mix()).
    float weight = coverage * mask.z;
    vec4 result = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : mix(orig, processed, weight));
    imageStore(out_color, ivec2(p), result);
}
)GLSL";

}  // namespace nemo::eval
