// Nemo native effect contract (spec section 10.4, issue #34)
// ---------------------------------------------------------
// name: transform
// version: 1
// ports:
//   - in_main : image2D input (main image, required port 0)
//   - in_mask : image2D input (optional mask, port 1; bound to the main
//     image as a valid dummy descriptor with maskPresent=0 when unconnected)
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - translateX/translateY (pixels), scale, rotate (degrees, positive
//     clockwise in the stored raster), filter [Cubic, Linear, Nearest], plus
//     the common maskChannel/invertMask/mix controls.
// bindings: shared effect contract (see testpattern.slang). Port order is
//   declared: set 1 binding 0 = main, binding 1 = mask.
// dependencies:
//   region: output pixel (x, y) samples the inverse-mapped input; no
//     support-region expansion (whole-image only, supportsRegion=false).
//   channels: RGBA uniform; interpolation is alpha-aware.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight output; interpolation premultiplies, then converts back
//   with zero-alpha RGB = 0 and no implicit RGB clamp. Outside the image is
//   transparent black. The mask stays in output space and is never
//   transformed.
//
// Geometry: image-center pivot (width/2, height/2), pixel centers x+0.5 /
// y+0.5 in FULL resolution, x right / y down. Uniform scale, then rotation
// about the center in physical coordinates (x*pixelAspect, y), then
// translation. The kernel inverse-maps each output center and samples the
// input (Nearest = floor(fullRes/scale); Linear bilinear; Cubic separable
// Catmull-Rom a=-0.5). Full-resolution spatial parameters therefore preserve
// coordinates at every reduced sampling scale.

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    float4 param0;
    float4 param1;
    // issue #34 native effects: append-only extension shared by EVERY effect
    // kernel (existing offsets unchanged, so preexisting kernels stay
    // ABI-aligned with the C++ EffectUniforms mirror).
    float4 mask;             // (maskChannel, invertMask, mix, maskPresent)
    float4 gradeBlackpoint;
    float4 gradeWhitepoint;
    float4 gradeLift;
    float4 gradeGain;
    float4 gradeMultiply;
    float4 gradeOffset;
    float4 gradeGamma;
    float4 gradeFlags;       // (channels bitmask, reverse, clampBlack, clampWhite)
    float4 blur;             // (size full-res, channels bitmask, 0, 0)
    float4 transform;        // (translateX, translateY, scale, rotate degrees)
    float4 transformFlags;   // (filter, pixelAspect, cos, sin)
}

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;
[[vk::binding(1, 1)]]
RWTexture2D<float4> in_mask;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

float4 premultTexel(int2 q, int2 dims)
{
    if (q.x < 0 || q.y < 0 || q.x >= dims.x || q.y >= dims.y) { return float4(0.0, 0.0, 0.0, 0.0); }
    float4 s = in_main.Load(q);
    return float4(s.rgb * s.a, s.a);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    float4 orig = in_main.Load(int2(p));

    int scale = int(meta2.z);
    float fullWidth = float(meta.x);
    float fullHeight = float(meta.y);
    float regionX = float(meta.z);
    float regionY = float(meta.w);
    float aspect = transformFlags.y;
    int2 dims = int2(meta2.xy);

    // Output pixel center in full resolution, then the declared inverse:
    // subtract translation, undo rotation (physical coords), undo scale.
    float2 fullOut = float2(regionX + (float(p.x) + 0.5) * float(scale),
                            regionY + (float(p.y) + 0.5) * float(scale));
    float2 center = float2(fullWidth * 0.5, fullHeight * 0.5);
    float2 g = fullOut - center - transform.xy;
    // Host-precomputed cos/sin of the rotation (double radians on the host).
    float c = transformFlags.z;
    float s = transformFlags.w;
    float2 physical = float2(g.x * aspect, g.y);
    float2 rotated = float2(physical.x * c + physical.y * s, -physical.x * s + physical.y * c);
    float2 fullIn = center + float2(rotated.x / aspect, rotated.y) / transform.z;

    float4 processed = float4(0.0, 0.0, 0.0, 0.0);  // outside the sampled support: transparent black
    int filterMode = int(transformFlags.x);
    float2 raster = float2((fullIn.x - regionX) / float(scale), (fullIn.y - regionY) / float(scale));
    if (filterMode == 2) {  // Nearest: bound the raster index; outside is transparent black
        if (raster.x >= 0.0 && raster.x < float(dims.x) && raster.y >= 0.0 && raster.y < float(dims.y)) {
            processed = in_main.Load(int2(floor(raster)));
        }
    } else if (raster.x > -3.0 && raster.x < float(dims.x) + 3.0 &&
               raster.y > -3.0 && raster.y < float(dims.y) + 3.0) {
        // Linear/Cubic sample their expanded support directly; taps outside
        // the raster are transparent black, so an edge kernel is never
        // cropped. The coarse guard only skips support lying entirely
        // outside the raster (same black result, bounded index math).
        float2 base = raster - 0.5;
        int2 i0 = int2(floor(base));
        float2 f = base - float2(i0);
        if (filterMode == 1) {  // Linear (bilinear), alpha-aware
            float4 p00 = premultTexel(i0, dims);
            float4 p10 = premultTexel(i0 + int2(1, 0), dims);
            float4 p01 = premultTexel(i0 + int2(0, 1), dims);
            float4 p11 = premultTexel(i0 + int2(1, 1), dims);
            float4 accumulated = lerp(lerp(p00, p10, f.x), lerp(p01, p11, f.x), f.y);
            processed = float4(accumulated.a != 0.0 ? accumulated.rgb / accumulated.a : float3(0.0, 0.0, 0.0),
                               accumulated.a);
        } else {  // Cubic: separable Catmull-Rom a=-0.5, alpha-aware
            float w0x = -0.5 * f.x * f.x * f.x + f.x * f.x - 0.5 * f.x;
            float w1x = 1.5 * f.x * f.x * f.x - 2.5 * f.x * f.x + 1.0;
            float w2x = -1.5 * f.x * f.x * f.x + 2.0 * f.x * f.x + 0.5 * f.x;
            float w3x = 0.5 * f.x * f.x * f.x - 0.5 * f.x * f.x;
            float w0y = -0.5 * f.y * f.y * f.y + f.y * f.y - 0.5 * f.y;
            float w1y = 1.5 * f.y * f.y * f.y - 2.5 * f.y * f.y + 1.0;
            float w2y = -1.5 * f.y * f.y * f.y + 2.0 * f.y * f.y + 0.5 * f.y;
            float w3y = 0.5 * f.y * f.y * f.y - 0.5 * f.y * f.y;
            float4 accumulated = float4(0.0, 0.0, 0.0, 0.0);
            for (int j = 0; j < 4; ++j) {
                float wy = j == 0 ? w0y : (j == 1 ? w1y : (j == 2 ? w2y : w3y));
                float4 row = w0x * premultTexel(i0 + int2(-1, j - 1), dims) +
                             w1x * premultTexel(i0 + int2(0, j - 1), dims) +
                             w2x * premultTexel(i0 + int2(1, j - 1), dims) +
                             w3x * premultTexel(i0 + int2(2, j - 1), dims);
                accumulated += wy * row;
            }
            processed = float4(accumulated.a != 0.0 ? accumulated.rgb / accumulated.a : float3(0.0, 0.0, 0.0),
                               accumulated.a);
        }
    }

    float coverage = 1.0;
    int channel = int(mask.x);
    if (mask.w > 0.5 && channel >= 0) {
        float selected = clamp(in_mask.Load(int2(p))[channel], 0.0, 1.0);
        coverage = mask.y > 0.5 ? 1.0 - selected : selected;
    }
    // Endpoints are exact: weight 0 keeps the original, weight 1 the fully
    // processed pixel (avoids HDR 0*inf cancellation in lerp()).
    float weight = coverage * mask.z;
    out_color[p] = weight <= 0.0 ? orig : (weight >= 1.0 ? processed : lerp(orig, processed, weight));
}
