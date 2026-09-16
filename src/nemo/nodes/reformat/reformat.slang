// Nemo native effect contract (spec section 10.4, issue #92; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: reformat
// version: 1
// ports:
//   - in_main : image2D input (main image, required port 0)
//   - out_color : image2D output (native channel image, issue #98: packed
//     RGBA32F at the logical raster for four stored channels, otherwise
//     R32_SFLOAT scalar planes at W*(H*C); straight alpha, scene-linear)
// parameters:
//   - type [format, box, scale], formatSource [composition, custom],
//     width/height/pixelAspect, boxWidth/boxHeight/boxPixelAspect/forceShape,
//     scaleX/scaleY, resize [none, width, height, fit, fill, distort], center,
//     flip, flop, turn, filter [Impulse, Cubic, Keys, Simon, Rifman, Mitchell,
//     Parzen, Notch, Lanczos4, Lanczos6, Sinc4], clamp, blackOutside,
//     preserveBoundingBox. The host resolves the interpreted parameters and the
//     whole output<->source geometry; this kernel only resamples.
// bindings (native binding contract v7, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.reformat.payload.v2")
//   set 0, binding 2 : per-input spatial geometry (main)
//   set 1, binding 0 = main
//   set 2, binding 0 : output image2D
//   set 3, binding 0 : the host-resolved per-axis sampling table
// dependencies:
//   region: output pixel (x, y) inverse-maps to a source pixel coordinate
//     through the host-resolved affine placement (orientation, resize and
//     alignment included); the read window is the filter's separable support,
//     widened by the minification factor. A tap the raster does not hold is
//     transparent black, or the outermost data-window sample when black outside
//     is off (edge extension), never an out-of-bounds load.
//   channels: every stored channel of the produced raster is produced HERE. The
//     primary R/G/B/A roles are resampled association-aware (straight input is
//     premultiplied for the kernel and divided afterwards; premultiplied input
//     is filtered as stored) and each named auxiliary plane is resampled
//     numerically and independently. When black outside is on and the input
//     carries no alpha, the documented solid alpha over the input image area is
//     produced too.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: straight output; the primary RGBA follows the input's declared
//   association and never clamps, and `clamp` bounds a filtered channel by the
//   contributing samples' extrema (never by 0..1), so HDR and negative values
//   survive.
//
// Geometry: the output canvas and the pixel aspect come from the mode (a
// format, a box, or the input scaled); the placement is flop, then flip, then a
// 90-degree counter-clockwise turn, then the resize policy and the alignment,
// exactly as the CPU reference does. The host resolves that whole map, so no
// kernel evaluates a placement rule itself.
//
// Tap positions are INPUT RASTER indices: raster sample i carries the source
// pixel whose center is i + 0.5, and its weights are `K((center - (i + 0.5)) /
// widen) / sum(K)` over the kernel's whole ideal support, so a tap the raster
// does not hold contributes black instead of renormalizing an edge. WHICH taps
// exist is the host's decision, in double precision, from the shared
// reformatSampleWindow rule: the set-3 table carries one resolved window per
// driven output axis (first, last, base, nearest, fraction, inside), so this
// kernel never decides membership and no float epsilon can move a Notch or
// Impulse boundary. The tap distance is `fraction + (base - index) - 0.5`, never
// two large rounded coordinates subtracted. The unit-width box's weights ARE the
// enumerated window (this front end normalizes them), and an enumerated window
// that falls entirely on kernel zeros falls back to the host's base, exactly as
// the CPU reference's own fallback does.

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
    // support (an edge-extended output is written in full).
    int4 support;
    // channels: the produced raster's channel facts (issues #90, #98).
    uint4 channels;
    // rgba: the produced STORED channel of each R/G/B/A role; -1 = role absent.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer ReformatPayload {
    // (separable kernel minification widening x, y, 0, 0)
    float4 kernel;
    // (turn, filter mode, source-Y table word offset, 0)
    int4 flags;
    // (input data window low/high raster index x, low/high y; the edge
    // extension's clamp range, unused while black outside is on)
    int4 window;
    // (clamp, blackOutside, solidAlpha, premultiply)
    int4 extras;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

// Host-resolved geometry: first/last/base each use two exact 16-bit numeric
// limbs, followed by nearest/fraction/inside. No native tap-membership decision
// or lossy conversion of a large signed index to float occurs here.
[[vk::binding(0, 3)]]
StructuredBuffer<float> reformatAxes;

static const int kReformatAxisWords = 9;

int reformatAxisInteger(uint word) {
    return asint(uint(reformatAxes[word]) | (uint(reformatAxes[word + 1u]) << 16u));
}

// One axis' host-resolved sampling window: `first`..`last` are the taps the host
// enumerated CLOSED (its own double-precision guard included), `nearest` is the
// single host-selected tap (Impulse, or a degenerate window), `fraction` and
// `base` carry the tap distance exactly -- `fraction + (base - index) - 0.5`,
// never two large rounded coordinates subtracted -- and `inside` is the host's
// solid-alpha test.
struct ReformatAxis {
    int first;
    int last;
    int base;
    float fraction;
    bool nearest;
    bool inside;
};

ReformatAxis reformatAxisAt(uint word)
{
    ReformatAxis axis;
    axis.first = reformatAxisInteger(word);
    axis.last = reformatAxisInteger(word + 2u);
    axis.base = reformatAxisInteger(word + 4u);
    axis.nearest = reformatAxes[word + 6u] != 0.0;
    axis.fraction = reformatAxes[word + 7u];
    axis.inside = reformatAxes[word + 8u] != 0.0;
    return axis;
}

float reformatSinc(float t)
{
    if (t == 0.0) { return 1.0; }
    const float scaled = 3.14159265358979323846 * t;
    return sin(scaled) / scaled;
}

// The Mitchell-Netravali BC family: Cubic (0,0), Keys (0,.5), Simon (0,.75),
// Rifman (0,1), Mitchell (1/3,1/3) and Parzen (1,0).
float reformatBc(float b, float c, float distance)
{
    const float x = abs(distance);
    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
    }
    if (x < 2.0) {
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x + (-12.0 * b - 48.0 * c) * x +
                (8.0 * b + 24.0 * c)) /
               6.0;
    }
    return 0.0;
}

float reformatKernel(int mode, float distance)
{
    const float x = abs(distance);
    if (mode == 0) { return x < 0.5 ? 1.0 : 0.0; }                              // Impulse
    if (mode == 1) { return reformatBc(0.0, 0.0, x); }                          // Cubic
    if (mode == 2) { return reformatBc(0.0, 0.5, x); }                          // Keys
    if (mode == 3) { return reformatBc(0.0, 0.75, x); }                         // Simon
    if (mode == 4) { return reformatBc(0.0, 1.0, x); }                          // Rifman
    if (mode == 5) { return reformatBc(1.0 / 3.0, 1.0 / 3.0, x); }              // Mitchell
    if (mode == 6) { return reformatBc(1.0, 0.0, x); }                          // Parzen
    // mode 7 (Notch) has no interior shape: its weights are the closed support
    // window itself, evaluated by reformatTapWeight in the window's own units.
    if (mode == 8) { return x < 2.0 ? reformatSinc(x) * reformatSinc(x * 0.5) : 0.0; }
    if (mode == 9) { return x < 3.0 ? reformatSinc(x) * reformatSinc(x / 3.0) : 0.0; }
    if (mode == 10) { return x < 2.0 ? reformatSinc(x) : 0.0; }
    return 0.0;
}

// One ENUMERATED tap's weight, from the host's window: the unit-width box's
// weights ARE that window (the host enumerated it, and this front end normalizes
// them), and every continuous kernel keeps its own declared shape, evaluated on
// the host's exact tap distance divided by the same minification widening the
// window used. Nothing here decides membership.
float reformatTapWeight(int mode, float widen, ReformatAxis axis, int index)
{
    if (mode == 7) { return 1.0; }
    return reformatKernel(mode, (axis.fraction + float(axis.base - index) - 0.5) / widen);
}

// One axis' enumerated taps and the normalization their weights share. `nearest`
// is the single-tap case: the host selected it (Impulse, or a degenerate window),
// or the enumerated window fell entirely on kernel zeros, which falls back to the
// host's base exactly as the CPU reference's own fallback does.
struct ReformatNormalizer {
    int first;
    int last;
    bool nearest;
    float scale;
};

ReformatNormalizer reformatNormalize(int mode, float widen, ReformatAxis axis)
{
    ReformatNormalizer normalizer;
    normalizer.first = axis.first;
    normalizer.last = axis.last;
    normalizer.nearest = axis.nearest;
    normalizer.scale = 1.0;
    if (axis.nearest) { return normalizer; }
    float total = 0.0;
    for (int index = axis.first; index <= axis.last; ++index) {
        total += reformatTapWeight(mode, widen, axis, index);
    }
    if (total == 0.0) {
        normalizer.first = axis.base;
        normalizer.last = axis.base;
        normalizer.nearest = true;
        return normalizer;
    }
    normalizer.scale = 1.0 / total;
    return normalizer;
}

// The raster sample a tap resolves to, or -1 when it contributes nothing: with
// black outside on a tap outside the raster is transparent black, and with it off
// every tap is clamped into the input's data window first (the documented
// outermost-pixels extension).
int reformatTapIndex(int tap, bool blackOutside, int low, int high, int extent)
{
    int index = tap;
    if (!blackOutside) { index = clamp(index, low, high); }
    if (blackOutside && (index < low || index > high)) { return -1; }
    return (index >= 0 && index < extent) ? index : -1;
}

// One named plane resampled numerically and independently of the primary RGBA
// (issue #90). Auxiliary planes are rare, but the tap geometry is resolved ONCE
// from the host's table and shared with the primary path, so no plane ever
// re-decides a window this front end already resolved.
float reformatNumericPlane(int channel, ReformatAxis axisX, ReformatNormalizer tapsX, ReformatAxis axisY,
                           ReformatNormalizer tapsY, float2 widen, int mode, bool blackOutside, bool doClamp)
{
    const int extentX = int(inputGeometry[0].extent.x);
    const int extentY = int(inputGeometry[0].extent.y);
    const int planeHeight = int(inputGeometry[0].extent.y);
    const uint components = inputGeometry[0].channels.y;
    float accumulated = 0.0;
    float lowest = 1e30;
    float highest = -1e30;
    bool contributed = false;
    for (int j = tapsY.first; j <= tapsY.last; ++j) {
        const int ty = reformatTapIndex(j, blackOutside, window.z, window.w, extentY);
        if (ty < 0) { continue; }
        const float wy = tapsY.nearest ? 1.0 : reformatTapWeight(mode, widen.y, axisY, j) * tapsY.scale;
        if (wy == 0.0) { continue; }
        for (int i = tapsX.first; i <= tapsX.last; ++i) {
            const int tx = reformatTapIndex(i, blackOutside, window.x, window.y, extentX);
            if (tx < 0) { continue; }
            const float tapX = reformatTapWeight(mode, widen.x, axisX, i);
            const float wx = tapsX.nearest ? 1.0 : tapX * tapsX.scale;
            const float weight = wx * wy;
            if (weight == 0.0) { continue; }
            const float value = gpuLoadChannel(in_main, int2(tx, ty), channel, planeHeight, components);
            accumulated += weight * value;
            lowest = min(lowest, value);
            highest = max(highest, value);
            contributed = true;
        }
    }
    if (doClamp && contributed) { accumulated = clamp(accumulated, lowest, highest); }
    return accumulated;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    // A sample outside the described data support is transparent black (an
    // edge-extended output declares no support, so it is written in full).
    if (support.x >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), int(meta2.y), channels.y, channels.x);
        return;
    }

    const int mode = flags.y;
    const bool blackOutside = extras.y > 0;
    const bool doClamp = extras.x > 0;
    const bool solidAlpha = extras.z > 0;
    const bool premultiply = extras.w > 0;
    const int extentX = int(inputGeometry[0].extent.x);
    const int extentY = int(inputGeometry[0].extent.y);
    const int planeHeight = int(inputGeometry[0].extent.y);
    const uint components = inputGeometry[0].channels.y;
    const float2 widen = float2(kernel.x, kernel.y);

    // The host's table is indexed by the OUTPUT axis that actually drives each
    // source axis: a 90-degree turn CROSSES them, so the source-X entry belongs to
    // the output row and the source-Y entry to the output column (the CPU
    // reference's driving-axis loop). Both this pixel's windows and the solid
    // alpha's inside test come from those two entries, never from a float
    // re-evaluation of the map.
    const bool crosses = flags.x > 0;
    const uint wordX = uint(crosses ? p.y : p.x) * uint(kReformatAxisWords);
    const uint wordY = uint(flags.z) + uint(crosses ? p.x : p.y) * uint(kReformatAxisWords);
    const ReformatAxis axisX = reformatAxisAt(wordX);
    const ReformatAxis axisY = reformatAxisAt(wordY);
    const ReformatNormalizer tapsX = reformatNormalize(mode, widen.x, axisX);
    const ReformatNormalizer tapsY = reformatNormalize(mode, widen.y, axisY);

    // The primary RGBA: association-aware (a straight input is premultiplied for
    // the kernel), with the contributing samples' extrema kept for `clamp`.
    float4 accumulated = float4(0.0);
    float4 lowest = float4(1e30);
    float4 highest = float4(-1e30);
    bool contributed = false;
    for (int j = tapsY.first; j <= tapsY.last; ++j) {
        const int ty = reformatTapIndex(j, blackOutside, window.z, window.w, extentY);
        if (ty < 0) { continue; }
        const float wy = tapsY.nearest ? 1.0 : reformatTapWeight(mode, widen.y, axisY, j) * tapsY.scale;
        if (wy == 0.0) { continue; }
        for (int i = tapsX.first; i <= tapsX.last; ++i) {
            const int tx = reformatTapIndex(i, blackOutside, window.x, window.y, extentX);
            if (tx < 0) { continue; }
            const float wx = tapsX.nearest ? 1.0 : reformatTapWeight(mode, widen.x, axisX, i) * tapsX.scale;
            const float weight = wx * wy;
            if (weight == 0.0) { continue; }
            float4 tapValue = gpuLoadRgba(in_main, int2(tx, ty), inputGeometry[0].rgba, planeHeight, components);
            if (premultiply) { tapValue = float4(tapValue.rgb * tapValue.a, tapValue.a); }
            accumulated += weight * tapValue;
            lowest = min(lowest, tapValue);
            highest = max(highest, tapValue);
            contributed = true;
        }
    }
    if (doClamp && contributed) { accumulated = clamp(accumulated, lowest, highest); }
    float4 filtered = accumulated;
    if (premultiply) {
        filtered = accumulated.a != 0.0 ? float4(accumulated.rgb / accumulated.a, accumulated.a) : float4(0.0);
    }
    if (solidAlpha) {
        // The documented "solid alpha over the input image area", decided by the
        // host in source pixel coordinates -- exactly the test the CPU reference
        // makes, and on the same driving axes.
        filtered.a = (axisX.inside && axisY.inside) ? 1.0 : 0.0;
    }

    // Every stored channel of the produced raster: the primary roles from the
    // filtered RGBA, each named auxiliary plane from its own numeric resample,
    // and anything the input does not carry as zero.
    const int inputChannels = int(inputGeometry[0].channels.x);
    float4 pixel = float4(0.0);
    for (uint channel = 0u; channel < channels.x; ++channel) {
        int role = -1;
        if (rgba.x == int(channel)) { role = 0; }
        else if (rgba.y == int(channel)) { role = 1; }
        else if (rgba.z == int(channel)) { role = 2; }
        else if (rgba.w == int(channel)) { role = 3; }
        float value = 0.0;
        if (role >= 0) {
            value = filtered[role];
        } else if (int(channel) < inputChannels) {
            value = reformatNumericPlane(int(channel), axisX, tapsX, axisY, tapsY, widen, mode, blackOutside, doClamp);
        }
        gpuPixelChannel(pixel, out_color, int2(p), int(meta2.y), channels.y, int(channel), value);
    }
    gpuPixelStore(out_color, int2(p), channels.y, pixel);
}
