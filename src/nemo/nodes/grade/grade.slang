// Nemo native effect contract (spec section 10.4, issue #34; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: grade
// version: 2
// ports:
//   - in_main : image2D input (main image, required port 0)
//   - in_mask : image2D input (optional mask, port 1; bound to the main
//     image as a valid dummy descriptor with maskPresent=0 when unconnected)
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - blackpoint/whitepoint/lift/gain/multiply/offset/gamma : Color, per
//     channel; channels: [RGB, RGBA, R, G, B, Alpha, None]; reverse;
//     clampBlack (default true); clampWhite (default false); plus the common
//     maskChannel/invertMask/mix controls.
// bindings (native binding contract v5, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms (meta is signed)
//   set 0, binding 1 : node-local payload ("nemo.nodes.grade.payload.v1")
//   set 0, binding 2 : per-input spatial geometry (main, mask)
//   set 1, binding 0 = main, binding 1 = mask
//   set 2, binding 0 : output image2D
// dependencies:
//   region: output pixel (x, y) depends only on the input pixel holding the
//     same full-resolution sample; each input's raster origin and extent are
//     bound with it (region evaluation, wider cache-backed inputs, signed or
//     beyond-format origins), so no support-region expansion is needed. A
//     sample outside the bound input raster is outside that input's data and
//     reads as transparent black (issue #88).
//   channels: RGBA uniform; per-channel enables are exact pass-through when
//     disabled (no clamp, no operation).
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight (non-premultiplied), identical to the CPU reference.
//
// Forward (per enabled channel):
//   a = (gain - lift) * multiply / (whitepoint - blackpoint)
//   b = lift + offset - blackpoint * a
//   y = signedPow(a*x + b, 1/gamma)
// Reverse:
//   x = (signedPow(y, gamma) - b) / a   (requires nonzero a)
// signedPow(0, p) = 0 and sign(x)*pow(abs(x), p): the explicit
// negative/HDR extension, not a claim of bitwise Nuke parity. Clamps apply
// after the operation, before mask/mix. Invalid coefficients are rejected by
// the shared metadata validation, never silently substituted.

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y; signed)
    int4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // support: half-open RASTER-INDEX rect (x, y, width, height) of the samples
    // that ARE this image's data; (-1,-1,-1,-1) means the raster declares no
    // support (a node-local scratch), a non-positive size means every sample is
    // transparent black. Samples outside it must be written transparent black.
    int4 support;
};

[[vk::binding(1, 0)]]
cbuffer GradePayload {
    // (maskChannel, invertMask, mix, maskPresent)
    float4 mask;
    float4 gradeBlackpoint;
    float4 gradeWhitepoint;
    float4 gradeLift;
    float4 gradeGain;
    float4 gradeMultiply;
    float4 gradeOffset;
    float4 gradeGamma;
    // (channels bitmask, reverse, clampBlack, clampWhite)
    float4 gradeFlags;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;
[[vk::binding(1, 1)]]
RWTexture2D<float4> in_mask;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

float signedPow(float x, float p)
{
    // signedPow(0, p) = 0; sign(x)*pow(abs(x), p) is the negative/HDR
    // extension (mediaConvert uses the same proven Slang idiom).
    return x == 0.0 ? 0.0 : sign(x) * pow(abs(x), p);
}

float gradeChannel(float x, int c)
{
    if ((int(gradeFlags.x) & (1 << c)) == 0) { return x; }  // disabled: exact pass-through
    float blackpoint = gradeBlackpoint[c];
    float whitepoint = gradeWhitepoint[c];
    float a = ((gradeGain[c] - gradeLift[c]) * gradeMultiply[c]) / (whitepoint - blackpoint);
    float b = gradeLift[c] + gradeOffset[c] - blackpoint * a;
    float y;
    if (gradeFlags.y > 0.5) { y = (signedPow(x, gradeGamma[c]) - b) / a; }
    else { y = signedPow(a * x + b, 1.0 / gradeGamma[c]); }
    // Clamps apply after the operation, before mask/mix (Foundry default black clamp).
    if (gradeFlags.z > 0.5 && y < 0.0) { y = 0.0; }
    if (gradeFlags.w > 0.5 && y > 1.0) { y = 1.0; }
    return y;
}

float4 gradePixel(float4 x)
{
    float4 y = x;
    for (int c = 0; c < 4; ++c) { y[c] = gradeChannel(x[c], c); }
    return y;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    // The described image's data support (native binding contract v5): a sample
    // outside it is transparent black, never grade(0) fabricated there.
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        out_color[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Same-lattice inputs: the pixel holding the same full-resolution sample,
    // located through each input's own raster origin and extent. A sample the
    // input does not hold is outside its data (a smaller or empty data window):
    // transparent black, never an out-of-bounds load.
    int2 mainPixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    int2 mainExtent = int2(inputGeometry[0].extent.xy);
    bool mainInside = mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    float4 orig = mainInside ? in_main.Load(mainPixel) : float4(0.0, 0.0, 0.0, 0.0);
    float4 processed = gradePixel(orig);

    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        int2 maskPixel = int2(p) + inputGeometry[1].regionAndOffset.zw;
        int2 maskExtent = int2(inputGeometry[1].extent.xy);
        bool maskInside =
            maskPixel.x >= 0 && maskPixel.y >= 0 && maskPixel.x < maskExtent.x && maskPixel.y < maskExtent.y;
        float selected = maskInside ? clamp(in_mask.Load(maskPixel)[channel], 0.0, 1.0) : 0.0;
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel, so no HDR 0*inf cancellation occurs in lerp().
    float weight = coverage * mask.z;
    out_color[p] = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : lerp(orig, processed, weight));
}
