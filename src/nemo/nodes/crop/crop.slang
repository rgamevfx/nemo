// Nemo native effect contract (spec section 10.4, issue #92; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: crop
// version: 1
// ports:
//   - in_main : image2D input (the image to crop, required port 0)
//   - out_color : image2D output (native channel image, issue #98: packed
//     RGBA32F at the logical raster for four stored channels, otherwise
//     R32_SFLOAT scalar planes at W*(H*C); the incoming alpha convention,
//     scene-linear)
// parameters:
//   - x/y, right/top (bottom-left box in the INCOMING IMAGE's own format,
//     fractional; the saved composition canvas only supplied the box's initial
//     values at creation), softness (full-resolution pixels), reformat,
//     intersect, blackOutside.
// bindings (native binding contract v7, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.crop.payload.v1")
//   set 0, binding 2 : per-input spatial geometry (main)
//   set 1, binding 0 = main
//   set 2, binding 0 : output image2D
// dependencies:
//   region: the retained box (its floor/ceil enclosure, intersected with the
//     incoming data window when `intersect` is enabled), plus the box edges'
//     softness ramp inside it. Nothing is resampled: a sample the box covers
//     keeps the incoming value at its own coordinates. When black outside is
//     disabled the retained bounds are the edge domain the description
//     publishes, and every sample outside them reads the CLAMPED retained
//     sample, so a consumer outside the box still sees real data instead of
//     transparent black. A sample the incoming image does not hold stays
//     transparent black either way (issue #88): the crop never manufactures
//     data its input does not have.
//   channels: every stored plane is produced here (this node owns its channel
//     layout): the primary roles from the incoming image's R/G/B/A projection,
//     every other plane from the same-named incoming plane at the sample read.
//     Black outside adds one alpha channel when the image stores none: 1 where
//     the sample's full-resolution pixel lies in the incoming data window.
//   temporal: none.
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: the incoming convention is preserved. A straight image fades through
//   alpha (RGB kept, alpha scaled); a premultiplied one scales RGB and alpha
//   together; an output with no alpha at all (only reachable with black outside
//   disabled and no incoming alpha) scales its stored values numerically.
//   Auxiliary planes always scale numerically.
//
// Geometry: coordinates are full-resolution pixels, x right / y down, and the
// reformat translation is carried as the read base, so the kernel works in the
// OUTPUT frame the request is stated in while reading the incoming raster at
// its own origin. The ramp is measured from the box's fractional edges to the
// sample's own full-resolution anchor, which is what keeps Full, Half and
// Quarter requests consistent wherever their anchors coincide.

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
    // channels: the produced raster's channel facts (issues #90, #98):
    // (stored channel count, components per texel — 4 = packed four-channel
    // RGBA32F at the logical raster, 1 = R32_SFLOAT scalar planes — whether the
    // channel plan fills any stored channel, 0).
    uint4 channels;
    // rgba: the produced STORED channel of each R/G/B/A role; -1 = role absent.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer CropPayload {
    // (left, right, top [y down], bottom) fractional box edges in
    // full-resolution output coordinates.
    float4 cropBox;
    // (softness in full-resolution pixels, premultiplied, black outside, added
    // alpha).
    float4 cropFlags;
    // The retained domain's half-open raster-index rectangle inside this pass's
    // raster (x, y, width, height).
    int4 cropRetained;
    // The retained domain in full-resolution output coordinates.
    int4 cropDomain;
    // The incoming image's data window in full-resolution output coordinates.
    int4 cropData;
    // (incoming raster column for output column 0, row for output row 0, 0, 0).
    int4 cropBase;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

// Floor division for signed values: integer division truncates toward zero,
// which is wrong on the left/bottom half of the lattice.
int cropFloorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

int cropCeilDiv(int value, int divisor)
{
    return cropFloorDiv(value + divisor - 1, divisor);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);

    // An empty retained domain is a valid fully transparent image (story 49),
    // and every stored channel is written, so nothing is left undefined.
    if (cropDomain.z <= 0 || cropDomain.w <= 0) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.y, channels.x);
        return;
    }
    const int scale = int(meta2.z);
    // Each axis is decided independently: a retained domain can be
    // representable at this density on one axis and not on the other (a box two
    // pixels tall at quarter resolution), and the clamped edge is then the
    // nearest anchor this raster can actually hold.
    const bool haveX = cropRetained.z > 0;
    const bool haveY = cropRetained.w > 0;
    const bool insideX =
        haveX && int(p.x) >= cropRetained.x && int(p.x) < cropRetained.x + cropRetained.z;
    const bool insideY =
        haveY && int(p.y) >= cropRetained.y && int(p.y) < cropRetained.y + cropRetained.w;
    // Black outside the retained box: the raster's own zero, in every stored
    // plane. The clamped extension reads the nearest retained sample instead.
    if (!(insideX && insideY) && cropFlags.z > 0.5) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.y, channels.x);
        return;
    }
    int column = int(p.x);
    int row = int(p.y);
    if (!insideX) {
        column = haveX ? clamp(int(p.x), cropRetained.x, cropRetained.x + cropRetained.z - 1)
                       : clamp(cropCeilDiv(cropDomain.x - meta.z, scale), 0, max(0, int(meta2.x) - 1));
    }
    if (!insideY) {
        row = haveY ? clamp(int(p.y), cropRetained.y, cropRetained.y + cropRetained.w - 1)
                    : clamp(cropCeilDiv(cropDomain.y - meta.w, scale), 0, max(0, int(meta2.y) - 1));
    }
    const int anchorX = meta.z + column * scale;
    const int anchorY = meta.w + row * scale;

    // The reference vignette, in full-resolution pixels: an inward linear ramp
    // per edge, the product at corners, zero hard. Measured from the sample's
    // own anchor so Full, Half and Quarter requests agree wherever their anchors
    // coincide.
    float weight = 1.0;
    if (cropFlags.x > 0.0) {
        const float ax = float(anchorX);
        const float ay = float(anchorY);
        weight = clamp((ax - cropBox.x) / cropFlags.x, 0.0, 1.0) *
                 clamp((cropBox.y - ax) / cropFlags.x, 0.0, 1.0) *
                 clamp((ay - cropBox.z) / cropFlags.x, 0.0, 1.0) *
                 clamp((cropBox.w - ay) / cropFlags.x, 0.0, 1.0);
    }

    // The incoming sample at the retained position this output sample reads.
    // Where the incoming image holds no sample the read is transparent black
    // (issue #88): the crop never manufactures data the input does not have.
    const int2 extent = int2(inputGeometry[0].extent.xy);
    const int2 inPixel = int2(column + cropBase.x, row + cropBase.y);
    const bool inInside =
        inPixel.x >= 0 && inPixel.y >= 0 && inPixel.x < extent.x && inPixel.y < extent.y;
    float4 value = inInside ? gpuLoadRgba(in_main, inPixel, inputGeometry[0].rgba, extent.y,
                                          inputGeometry[0].channels.y)
                            : float4(0.0);
    // Black outside adds the reference's solid alpha covering the incoming image
    // area when the image stores none (story 48).
    if (cropFlags.w > 0.5) {
        const bool covered = anchorX >= cropData.x && anchorY >= cropData.y &&
                             anchorX < cropData.x + cropData.z && anchorY < cropData.y + cropData.w;
        value.w = covered ? 1.0 : 0.0;
    }
    // Declared-association fade: straight primary RGB fades through alpha,
    // premultiplied RGB and alpha scale together. An output with no alpha has
    // nothing to fade through, so its stored values scale numerically.
    const bool numericFade = rgba.w < 0;
    if (weight != 1.0) {
        if (numericFade || cropFlags.y > 0.5) {
            value *= weight;
        } else {
            value.w *= weight;
        }
    }

    // Every stored plane of the produced raster (this node owns its channel
    // layout): the primary roles from the projection above, every other plane
    // from the same-named incoming plane at the sample just read, scaled
    // numerically by the same weight.
    float4 pixel = float4(0.0, 0.0, 0.0, 0.0);
    if (rgba.x >= 0) { gpuPixelChannel(pixel, out_color, int2(p), planeHeight, channels.y, rgba.x, value.x); }
    if (rgba.y >= 0) { gpuPixelChannel(pixel, out_color, int2(p), planeHeight, channels.y, rgba.y, value.y); }
    if (rgba.z >= 0) { gpuPixelChannel(pixel, out_color, int2(p), planeHeight, channels.y, rgba.z, value.z); }
    if (rgba.w >= 0) { gpuPixelChannel(pixel, out_color, int2(p), planeHeight, channels.y, rgba.w, value.w); }
    const int incomingChannels = int(inputGeometry[0].channels.x);
    for (uint plane = 0u; plane < channels.x; ++plane) {
        const int index = int(plane);
        if (index == rgba.x || index == rgba.y || index == rgba.z || index == rgba.w) { continue; }
        float auxiliary = 0.0;
        if (index < incomingChannels && inInside) {
            auxiliary = gpuLoadChannel(in_main, inPixel, index, extent.y, inputGeometry[0].channels.y) * weight;
        }
        gpuPixelChannel(pixel, out_color, int2(p), planeHeight, channels.y, index, auxiliary);
    }
    gpuPixelStore(out_color, int2(p), channels.y, pixel);
}
