// Shared native-effect binding contract, part v7 (issues #88, #90, #98).
// ---------------------------------------------------------
// The common request (set 0, binding 0) carries the node output's described data
// support as a half-open RASTER-INDEX rectangle; the GLSL front end declares the
// same word (nemo::nodes::kGpuRequestGlsl). Every kernel mirrors it: a sample
// outside the support is transparent black, a non-positive support size means
// the whole raster is transparent black, and (-1,-1,-1,-1) means the raster
// declares no support at all (a node-local scratch) so the kernel writes it in
// full.
//
// The request block itself is declared by each shader (the same fields the GLSL
// front end declares): meta (full width/height, signed region origin), meta2
// (LOGICAL raster width/height, samplingScale, 0), misc (localTime), support,
// channels (this raster's stored channel count, its components per texel,
// whether the executor's channel plan fills any stored channel, 0) and rgba (the
// stored channel of each R, G, B, A role, -1 = role absent).
//
// Per-input spatial geometry (set 0, binding 2) of every local pass that binds
// images at set 1. The GLSL reference front end declares the identical block
// (nemo::nodes::kGpuInputGeometryGlsl); both mirror nemo::eval::
// EffectInputGeometry, which is the layout the executor uploads.
//
// One entry per bound pass input, indexed by the set-1 binding it belongs to:
// a pass input pixel is `p + regionAndOffset.zw` for pass output pixel `p`,
// because every raster in one evaluation sits on the same sampling lattice.
// `regionAndOffset.xy` is the input raster's full-resolution region origin (the
// coordinate frame a geometric resample maps back into) and `extent.xy` its
// LOGICAL raster size in pixels, which bounds a neighborhood read's taps.
//
// Region origins are SIGNED: a request may address negative or beyond-format
// coordinates (issue #88), so `meta.z`/`meta.w` in the common request are ints
// and the offsets here may be negative. A tap outside `[0, extent.xy)` is
// outside the input's data — an input may hold a different, smaller or empty
// rectangle of the same lattice — and reads as transparent black, never as an
// out-of-bounds image load.
//
// An external source frame (EffectImageKind::External, a Read's decoded media)
// carries its coverage the same way: `regionAndOffset.xy` is the coverage's
// full-resolution origin (the media's own display/data window, normalized), and
// `extent.xy` its raster extent at full resolution. `regionAndOffset.zw` is
// unused there.
//
// Native channel images (issues #90, #98): a native image carries every STORED
// channel of its raster, in stored order, and its representation follows from
// the stored channel count alone.
//
//   channels.y == 4  PACKED RGBA32F at the LOGICAL extent W×H: texel (x, y)
//                    holds stored channels 0..3 of logical pixel (x, y) in its
//                    R,G,B,A components, so a whole pixel is ONE vector load or
//                    store. The four stored channels are NOT assumed to be
//                    R,G,B,A: `rgba`/`rolePlanes` is the only authority on which
//                    stored channel plays which role, so a noncanonical order is
//                    read and written in place and never reordered behind the
//                    kernel's back.
//   channels.y == 1  every other count: R32_SFLOAT scalar planes at W×(H*C),
//                    stored channel c of logical pixel (x, y) at
//                    (x, y + c*planeHeight) — one plane per channel, so a one-,
//                    two- or three-channel image is never expanded.
//
// Every image is bound as a FORMATLESS storage image (the device features the
// executor requires), so one declaration reads and writes both representations:
// a scalar plane is component .x of its own texel, a packed pixel is the whole
// texel. `planeHeight` is always the LOGICAL height and is unused for a packed
// image, whose channels share one texel. `components` for a sampled image comes
// from that image's own geometry entry (its ACTUAL storage, never a guess from
// the description); for the produced raster it is the request's `channels.y`.
//
// `rgba` (from the request for this pass's output, from the geometry entry for a
// sampled input) is the pre-resolved stored channel of each R/G/B/A role; the
// projection is exactly nemo::CpuImage::pixel's: a missing RGB role reads 0.0, a
// missing A role reads 1.0 when the image stores at least one RGB role and 0.0
// when it stores none, and a store writes only roles the image stores — no named
// RGB or alpha is manufactured.
//
// The channel plan (set 0, binding 3, one entry per stored channel) is resolved
// by the executor from the described channel names: >= 0 names the source
// channel in this pass's set-1 binding 0 image, -1 means this pass's own math
// produces the channel, and -2 means the channel has no source and is numeric
// zero. Filling the planned channels preserves named auxiliary channels at
// UNCHANGED coordinates; `channels.z` says whether the plan fills any channel at
// all, so a pass whose raster holds only its own roles never reads the buffer.
//
// Every helper is also the destination's: a PACKED produced raster holds its
// whole pixel in one texel, so a kernel assembles the pixel in a register
// (`gpuPixelChannel`) and stores it ONCE (`gpuPixelStore`, or `gpuStorePixel`,
// which does both and fills the planned channels); a SCALAR raster stores each
// channel in its own plane as it is written and the register stays unused. Both
// shapes are the same kernel source. Slang has no preprocessor, so unlike the
// GLSL front end these helpers do not apply the data-support guard themselves:
// the caller's existing support early-return is the write guard, exactly as
// every other store in these kernels.

struct EffectInputGeometry {
    int4 regionAndOffset;  // input region origin x/y (signed), raster offset x/y
    uint4 extent;          // LOGICAL raster width/height, samplingScale, 1
    int4 rgba;             // stored channel per R,G,B,A role; -1 = role absent
    uint4 channels;        // (ACTUAL stored channel count, ACTUAL components per texel, 0, 0)
};

[[vk::binding(2, 0)]]
StructuredBuffer<EffectInputGeometry> inputGeometry;

[[vk::binding(3, 0)]]
StructuredBuffer<int2> channelPlan;

// The texel of one scalar plane at logical pixel `p`.
int2 gpuPlanePixel(int2 p, int channel, int planeHeight)
{
    return int2(p.x, p.y + channel * planeHeight);
}

// One stored channel of a sampled image, from either representation.
float gpuLoadChannel(RWTexture2D<float4> src, int2 p, int channel, int planeHeight, uint components)
{
    if (components == 4u) {
        // Index the loaded value, not Slang's image-element lvalue: the latter
        // emits an invalid vector OpImageTexelPointer for dynamic components.
        const float4 texel = src[uint2(p)];
        return texel[clamp(channel, 0, 3)];
    }
    return src[uint2(gpuPlanePixel(p, channel, planeHeight))].x;
}

// The R/G/B/A projection of one sampled image.
float4 gpuLoadRgba(RWTexture2D<float4> src, int2 p, int4 rolePlanes, int planeHeight, uint components)
{
    if (components == 4u) {
        const float4 texel = src[uint2(p)];  // one vector load for the whole pixel
        // Canonical RGBA avoids dynamic component indexing in every filter tap.
        if (all(rolePlanes == int4(0, 1, 2, 3))) { return texel; }
        return float4(rolePlanes.x >= 0 ? texel[clamp(rolePlanes.x, 0, 3)] : 0.0,
                      rolePlanes.y >= 0 ? texel[clamp(rolePlanes.y, 0, 3)] : 0.0,
                      rolePlanes.z >= 0 ? texel[clamp(rolePlanes.z, 0, 3)] : 0.0,
                      rolePlanes.w >= 0 ? texel[clamp(rolePlanes.w, 0, 3)]
                                        : ((rolePlanes.x >= 0 || rolePlanes.y >= 0 || rolePlanes.z >= 0) ? 1.0 : 0.0));
    }
    float4 value = float4(0.0, 0.0, 0.0, (rolePlanes.x >= 0 || rolePlanes.y >= 0 || rolePlanes.z >= 0) ? 1.0 : 0.0);
    if (rolePlanes.x >= 0) { value.x = gpuLoadChannel(src, p, rolePlanes.x, planeHeight, components); }
    if (rolePlanes.y >= 0) { value.y = gpuLoadChannel(src, p, rolePlanes.y, planeHeight, components); }
    if (rolePlanes.z >= 0) { value.z = gpuLoadChannel(src, p, rolePlanes.z, planeHeight, components); }
    if (rolePlanes.w >= 0) { value.w = gpuLoadChannel(src, p, rolePlanes.w, planeHeight, components); }
    return value;
}

// Store one scalar plane value.
void gpuStorePlane(RWTexture2D<float4> dst, int2 p, int planeHeight, int channel, float value)
{
    dst[uint2(gpuPlanePixel(p, channel, planeHeight))] = float4(value, 0.0, 0.0, 0.0);
}

// One stored channel of the produced pixel: into the register for a packed
// raster, onto its own plane row for a scalar one.
void gpuPixelChannel(inout float4 pixel, RWTexture2D<float4> dst, int2 p, int planeHeight, uint components, int channel,
                     float value)
{
    if (components == 4u) {
        pixel[clamp(channel, 0, 3)] = value;
        return;
    }
    gpuStorePlane(dst, p, planeHeight, channel, value);
}

// The produced pixel's ONE store. A no-op for a scalar raster, whose channels
// each already went to their own plane.
void gpuPixelStore(RWTexture2D<float4> dst, int2 p, uint components, float4 pixel)
{
    if (components == 4u) { dst[uint2(p)] = pixel; }
}

// Write the roles this pass's raster actually stores; a role it does not store is
// dropped, never manufactured. This is the whole store for a pass whose raster
// has no planned channel (a generator), which the executor enforces by refusing
// a described channel it cannot fill.
void gpuStoreRgba(RWTexture2D<float4> dst, int2 p, int planeHeight, uint components, int4 rolePlanes, float4 value)
{
    if (components == 4u) {
        if (all(rolePlanes == int4(0, 1, 2, 3))) {
            dst[uint2(p)] = value;
            return;
        }
        float4 pixel = float4(0.0, 0.0, 0.0, 0.0);
        if (rolePlanes.x >= 0) { pixel[clamp(rolePlanes.x, 0, 3)] = value.x; }
        if (rolePlanes.y >= 0) { pixel[clamp(rolePlanes.y, 0, 3)] = value.y; }
        if (rolePlanes.z >= 0) { pixel[clamp(rolePlanes.z, 0, 3)] = value.z; }
        if (rolePlanes.w >= 0) { pixel[clamp(rolePlanes.w, 0, 3)] = value.w; }
        dst[uint2(p)] = pixel;
        return;
    }
    if (rolePlanes.x >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.x, value.x); }
    if (rolePlanes.y >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.y, value.y); }
    if (rolePlanes.z >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.z, value.z); }
    if (rolePlanes.w >= 0) { gpuStorePlane(dst, p, planeHeight, rolePlanes.w, value.w); }
}

// The produced pixel in one call: the pass's RGBA math result in the roles its
// raster stores, every planned channel filled from `src` (this pass's set-1
// binding 0 image) at the SAME full-resolution coordinate, and ONE store. A
// geometric (resampling) kernel passes the unchanged-coordinate source pixel
// through `src`, never its resampled one: auxiliary channels are preserved where
// they are, not transformed. A planned channel with no source channel, or
// outside the source's raster, is numeric zero.
void gpuStorePixel(RWTexture2D<float4> dst, int2 p, int planeHeight, uint channelCount, uint components,
                   uint planChannels, int4 rolePlanes, float4 value, RWTexture2D<float4> src, int srcPlaneHeight,
                   uint srcComponents)
{
    if (components == 4u && planChannels == 0u && all(rolePlanes == int4(0, 1, 2, 3))) {
        dst[uint2(p)] = value;
        return;
    }
    float4 pixel = float4(0.0, 0.0, 0.0, 0.0);
    if (rolePlanes.x >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.x, value.x); }
    if (rolePlanes.y >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.y, value.y); }
    if (rolePlanes.z >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.z, value.z); }
    if (rolePlanes.w >= 0) { gpuPixelChannel(pixel, dst, p, planeHeight, components, rolePlanes.w, value.w); }
    if (planChannels != 0u) {
        const int2 q = p + inputGeometry[0].regionAndOffset.zw;
        const int2 extent = int2(inputGeometry[0].extent.xy);
        const bool inside = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
        for (uint channel = 0u; channel < channelCount; ++channel) {
            const int source = channelPlan[channel].x;
            if (source == -1) { continue; }  // this pass's own math produces the channel
            const float planned =
                (source >= 0 && inside) ? gpuLoadChannel(src, q, source, srcPlaneHeight, srcComponents) : 0.0;
            gpuPixelChannel(pixel, dst, p, planeHeight, components, int(channel), planned);
        }
    }
    gpuPixelStore(dst, p, components, pixel);
}

// Every stored channel of the produced raster at transparent black: one store
// for a packed raster, one per plane for a scalar one. The outside-support
// branch of a kernel writes this instead of running its pixel math, so a sample
// outside the described data window — and every planned channel — is initialized
// rather than left undefined device memory. It is explicit here because this
// front end has no view of the caller's support word.
void gpuZeroPlanes(RWTexture2D<float4> dst, int2 p, int planeHeight, uint components, uint channelCount)
{
    if (components == 4u) {
        dst[uint2(p)] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    for (uint channel = 0u; channel < channelCount; ++channel) {
        dst[uint2(gpuPlanePixel(p, int(channel), planeHeight))] = float4(0.0, 0.0, 0.0, 0.0);
    }
}
