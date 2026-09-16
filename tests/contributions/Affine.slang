// Test-only affine RGB contribution (issue #83 extension proof).
// -------------------------------------------------------------
// Separate Slang adapter for the same declared contract the CPU adapter
// implements independently: out.rgb = scale.rgb * in.rgb + offset.rgb, alpha
// copied through unchanged, no clamp of negative or HDR values.
//
// Issue #88 extension: the sample is derived from its ABSOLUTE full-resolution
// coordinate minus the authored read offset, then located in the input raster
// through that raster's own origin and extent. The two front ends share no
// coordinate helper, so their agreement is evidence that the declared demand and
// the read agree. This node declares samplingScale {1}, so a full-resolution
// pixel offset is also a raster sample.
//
// bindings (native binding contract v7):
//   set 0 binding 0 : common request uniforms (meta/meta2/misc; meta is signed)
//   set 0 binding 1 : this node's own payload ("nemo.test.affine.payload.v2")
//   set 0 binding 2 : per-input spatial geometry (the input raster's own
//     full-resolution origin and extent)
//   set 1 binding 0 : input image, port 0
//   set 2 binding 0 : node result
// precision: float32 native channel image (packed RGBA32F for four stored channels, else R32 planes)
// alpha: straight (non-premultiplied), identical to the CPU adapter.

#include "../../src/nemo/nodes/GpuGeometry.slang"

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
    uint4 channels;
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer AffinePayload {
    float4 scale;
    float4 offset;
    // (shiftX, shiftY, 0, 0) in full-resolution pixels
    float4 shift;
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_image;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_image;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }


    // The described image's data support (native binding contract v7): a sample
    // outside the example's described data window (the input's, translated by
    // the authored read offset) is transparent black.
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_image, int2(p), int(meta2.y), channels.y, channels.x);
        return;
    }

    int sampling = (int)meta2.z;
    // This output sample's absolute full-resolution coordinate, then the
    // authored read offset: the input sample the node declares it reads.
    int2 fullOut = int2(meta.z + (int)p.x * sampling, meta.w + (int)p.y * sampling);
    int2 wanted = fullOut - int2((int)shift.x, (int)shift.y);
    // The producer's own raster origin and extent place that sample; the demand
    // this node declared is its region translated by the negative offset, so the
    // origin is not this pass's own. sampling is 1 for this node's declared
    // capability, so the division is exact.
    int2 origin = inputGeometry[0].regionAndOffset.xy;
    int2 extent = int2(inputGeometry[0].extent.xy);
    int2 q = (wanted - origin) / sampling;
    bool covered = q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y;
    // Outside the producer's data is transparent black: the read is never
    // clamped to a border pixel and never out of bounds.
    float4 x = covered ? gpuLoadRgba(in_image, q, inputGeometry[0].rgba, int(extent.y),
                                     inputGeometry[0].channels.y)
                       : float4(0.0);
    // The produced pixel in one call: the roles this raster stores, every
    // planned channel from the input at unchanged coordinates, and — for a
    // packed four-channel raster — ONE vector store (issues #90, #98).
    gpuStorePixel(out_image, int2(p), int(meta2.y), channels.x, channels.y, channels.z, rgba,
                  float4(scale.x * x.x + offset.x, scale.y * x.y + offset.y,
                         scale.z * x.z + offset.z, x.w),
                  in_image, int(inputGeometry[0].extent.y), inputGeometry[0].channels.y);
}
