#version 450
// ColorWarp reference GLSL (issue #37) — the complete, self-contained reference
// kernel of the installed package's local pass "ColorWarp".
//
// Provenance: everything down to the "ColorWarp payload" block below is a
// verbatim copy of the host's generic native-effect GLSL front end
// (nemo::nodes::kGpuRequestGlsl, kGpuInputGeometryGlsl and
// kGpuChannelImageGlsl in src/nemo/nodes/GpuCommon.hpp), with the version
// directive hoisted to the first line because that is the only position glslang
// accepts. An installed package cannot call host C++, so the generic blocks are
// carried here; they are the shared binding contract v8, not a second
// implementation of it. Only the payload block and the kernel below are
// ColorWarp's own.
//
// The kernel is the same contract as the package's Slang kernel and its CPU
// reference, written independently: luma Y = .2126R + .7152G + .0722B, opponent
// u = (2R-G-B)/sqrt(6), v = (G-B)/sqrt(2), chroma c = hypot(u, v), scale
// s = 1 + |Y|, radius = c/(s+c), hue = atan2(v,u)/(2pi) mod 1, cell coordinates
// U = 12*hue and V = 4*radius. The mesh displaces (U, V) by strength * the
// smoothstep-tensor interpolated knot displacement, and the mapped position
// decodes back through the same luma. The inverse uses the complement s/(s+c)
// rather than 1 - radius, a zero interpolated displacement returns the sample
// unchanged, nothing is clamped, and an unrepresentable mapped position writes
// the not-a-number marker instead of a substituted color.
#extension GL_EXT_shader_image_load_formatted : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std140, set = 0, binding = 0) uniform EffectUniforms {
    ivec4 meta;      // full image width/height, pass region.x/region.y (signed)
    uvec4 meta2;     // raster: pass width, pass height (LOGICAL), samplingScale, 0
    vec4 misc;       // localTime in x
    ivec4 support;   // half-open raster-index rect of the samples that are data
    uvec4 channels;  // (stored channel count, components per texel, plan fills, 0)
    ivec4 rgba;      // stored channel per R,G,B,A role; -1 = role absent
};

// Output-support guard (issue #88). `support.x < 0` declares no support — a
// node-local scratch raster, whose only consumer is the node's next pass — and
// masks nothing. A zero-area support (an empty data window) masks every sample.
bool gpuHasData(ivec2 p) {
    return support.x < 0 ||
           (p.x >= support.x && p.y >= support.y && p.x < support.x + support.z &&
            p.y < support.y + support.w);
}

vec4 gpuSupported(vec4 value, ivec2 p) {
    return gpuHasData(p) ? value : vec4(0.0);
}

struct EffectInputGeometry {
    ivec4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uvec4 extent;           // LOGICAL raster width/height, samplingScale, 1
    ivec4 rgba;             // stored channel per R,G,B,A role; -1 = role absent
    uvec4 channels;         // (ACTUAL stored channel count, ACTUAL components per texel, 0, 0)
};
layout(std430, set = 0, binding = 2) readonly buffer EffectInputGeometries {
    EffectInputGeometry inputGeometry[];
};

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

// --- ColorWarp payload ("org.nemo.colorwarp.payload.v1", 592 bytes) ---------
layout(std140, set = 0, binding = 1) uniform ColorWarpPayload {
    // (hue displacement, saturation displacement, 0, 0) per editable point, in
    // cell coordinates and stable index order i = (r-1)*12 + h.
    vec4 control[36];
    // (strength, flags, identity, 0).
    vec4 settings;
};

// --- ColorWarp kernel ------------------------------------------------------
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

const int kSpokeCount = 12;
const int kBoundaryRing = 4;
const float kLumaR = 0.2126;
const float kLumaG = 0.7152;
const float kLumaB = 0.0722;
const float kInvSqrt6 = 0.4082482904638631;
const float kInvSqrt2 = 0.7071067811865476;
const float kSqrtTwoThirds = 0.8164965809277260;
const float kTwoPi = 6.2831853071795865;
const float kInvTwoPi = 0.15915494309189535;
const float kMaxFiniteFloat = 3.402823466e38;

// The not-a-number marker: a mapped position with no representable value is
// reported as "not a number", never as an invented, clamped or echoed color.
float colorWarpNaN() {
    return uintBitsToFloat(0x7FC00000u);
}

// hypot without the overflow: scaling by the larger magnitude keeps the result
// finite for the same inputs the CPU's hypot handles.
float colorWarpHypot(float u, float v) {
    const float magnitude = max(abs(u), abs(v));
    if (!(magnitude > 0.0) || magnitude > kMaxFiniteFloat) { return magnitude; }
    return magnitude * length(vec2(u / magnitude, v / magnitude));
}

float colorWarpSmoothstep(float t) {
    return t * t * (3.0 - 2.0 * t);
}

// One knot's authored displacement. The centre (ring 0) and the outer boundary
// (ring 4) are fixed at zero; spokes wrap.
vec2 colorWarpKnot(int spoke, int ring) {
    if (ring <= 0 || ring >= kBoundaryRing) { return vec2(0.0, 0.0); }
    const int wrapped = ((spoke % kSpokeCount) + kSpokeCount) % kSpokeCount;
    return control[(ring - 1) * kSpokeCount + wrapped].xy;
}

// The displacement field: tensor (smoothstep-weighted bilinear) interpolation of
// one cell's four knots, C1 across every edge. `v` is used as given — the
// encoding produces [0, 4], and the fixed boundary ring is the identity there.
vec2 colorWarpDisplacement(float u, float v) {
    const int cellU = int(floor(u));
    const int cellV = min(int(floor(v)), kBoundaryRing - 1);
    const float su = colorWarpSmoothstep(u - floor(u));
    const float sv = colorWarpSmoothstep(v - float(cellV));
    const vec2 bottomLeft = colorWarpKnot(cellU, cellV);
    const vec2 bottomRight = colorWarpKnot(cellU + 1, cellV);
    const vec2 topLeft = colorWarpKnot(cellU, cellV + 1);
    const vec2 topRight = colorWarpKnot(cellU + 1, cellV + 1);
    const float w0 = (1.0 - su) * (1.0 - sv);
    const float w1 = su * (1.0 - sv);
    const float w2 = (1.0 - su) * sv;
    const float w3 = su * sv;
    return w0 * bottomLeft + w1 * bottomRight + w2 * topLeft + w3 * topRight;
}

// The mapping, on straight (non-premultiplied) scene-linear RGB. Returns the
// sample itself when the interpolated displacement is exactly zero, and the
// not-a-number marker when the wheel has no representable value at the mapped
// position or the mapped position has no finite float value of its own.
vec3 colorWarpStraight(vec3 rgb) {
    const float luma = kLumaR * rgb.x + kLumaG * rgb.y + kLumaB * rgb.z;
    const float opponentU = (2.0 * rgb.x - rgb.y - rgb.z) * kInvSqrt6;
    const float opponentV = (rgb.y - rgb.z) * kInvSqrt2;
    const float chroma = colorWarpHypot(opponentU, opponentV);
    if (chroma == 0.0) { return rgb; }  // the neutral axis is fixed: no invented hue
    const float scale = 1.0 + abs(luma);
    const float total = scale + chroma;
    float hue = atan(opponentV, opponentU) * kInvTwoPi;  // (-0.5, 0.5]
    if (hue < 0.0) { hue += 1.0; }                       // [0, 1)
    const float cellU = hue * float(kSpokeCount);                 // [0, 12)
    const float cellV = (chroma / total) * float(kBoundaryRing);  // [0, 4)
    if (!(abs(cellU) <= kMaxFiniteFloat) || !(abs(cellV) <= kMaxFiniteFloat)) { return vec3(colorWarpNaN()); }
    const vec2 displacement = colorWarpDisplacement(cellU, cellV);
    if (displacement.x == 0.0 && displacement.y == 0.0) { return rgb; }  // exact identity
    // 1 - mapped radius, from the stable complement rather than from 1 - radius.
    const float complement = scale / total - settings.x * displacement.y / float(kBoundaryRing);
    if (!(complement > 0.0)) { return vec3(colorWarpNaN()); }
    const float mappedChroma = scale * (1.0 - complement) / complement;
    float mappedTurns = (cellU + settings.x * displacement.x) / float(kSpokeCount);
    mappedTurns -= floor(mappedTurns);  // [0, 1): the hue axis is periodic
    const float mappedU = mappedChroma * cos(kTwoPi * mappedTurns);
    const float mappedV = mappedChroma * sin(kTwoPi * mappedTurns);
    const float q0 = kSqrtTwoThirds * mappedU;
    const float q1 = -mappedU * kInvSqrt6 + mappedV * kInvSqrt2;
    const float q2 = -mappedU * kInvSqrt6 - mappedV * kInvSqrt2;
    // The same luma is restored exactly: the correction is orthogonal to the
    // opponent plane, so the mapping never changes Y.
    const float correction = luma - (kLumaR * q0 + kLumaG * q1 + kLumaB * q2);
    return vec3(q0 + correction, q1 + correction, q2 + correction);
}

// One sample: bypass/identity pass it through untouched, premultiplication is
// unassociated and re-associated explicitly, alpha is never warped.
vec4 colorWarpPixel(vec4 source) {
    const int flags = int(settings.y);
    const bool premultiplied = (flags & 1) != 0;
    // identity: every displacement is zero, or strength is zero.
    if (settings.z != 0.0) { return source; }
    // bypass bit: an explicit bypass, and a data-only or incomplete-RGB raster,
    // whose meaning a color effect must not invent.
    if ((flags & 2) != 0) { return source; }
    const float alpha = source.w;
    // A transparent premultiplied sample keeps its own values: there is no
    // unassociated color to warp and none is invented.
    if (premultiplied && alpha == 0.0) { return source; }
    // A sample that is not a number has no color to map.
    if (!all(lessThanEqual(abs(source.xyz), vec3(kMaxFiniteFloat)))) {
        return vec4(colorWarpNaN(), colorWarpNaN(), colorWarpNaN(), source.w);
    }
    const vec3 straight = premultiplied ? source.xyz / alpha : source.xyz;
    const vec3 mapped = colorWarpStraight(straight);
    return vec4(premultiplied ? mapped * alpha : mapped, source.w);
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);

    // The described image's data support: a sample outside it is transparent
    // black, and every stored channel — auxiliary ones included — is initialized.
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight, channels.y, channels.x);
        return;
    }

    // Same-lattice input: the pixel holding the same full-resolution sample,
    // located through the input's own raster origin and extent. A sample the
    // input does not hold is outside its data: transparent black, never an
    // out-of-bounds load.
    const ivec2 mainPixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    const ivec2 mainExtent = ivec2(inputGeometry[0].extent.xy);
    const bool mainInside =
        mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    const vec4 source =
        mainInside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, mainExtent.y, inputGeometry[0].channels.y)
                   : vec4(0.0, 0.0, 0.0, 0.0);

    const vec4 result = colorWarpPixel(source);
    // Every stored channel this pass's own math did not write keeps its named
    // channel from the main input at the SAME coordinate (issue #90).
    gpuStorePixel(out_color, ivec2(p), planeHeight, channels.x, channels.y, channels.z, rgba, result, in_main,
                  int(mainExtent.y), inputGeometry[0].channels.y);
}
