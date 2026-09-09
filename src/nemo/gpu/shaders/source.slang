// Nemo native effect contract (spec section 10.4, issue #11)
// ---------------------------------------------------------
// name: source
// version: 1
// ports:
//   - out_color : image2D output (float4 RGBA, straight alpha, scene-linear)
// parameters:
//   - the node's `source` parameter names a Document source reference
//     (setSourceCommand); the executor resolves the decoded frame and passes
//     its full-resolution dimensions in param0.
// bindings: shared effect contract (see testpattern.slang), plus
//   set 1, binding 0 : the decoded scene-linear full-resolution source frame
//   (rgba32f storage image, GENERAL, straight alpha) — supplied by
//   SourceSession, not by a graph edge.
// dependencies:
//   region: output pixel (x, y) depends only on the source pixel mapped
//     from the full-res coordinate (region.x + x*scale, region.y + y*scale)
//     — the source keeps FULL coordinate semantics while the raster is
//     reduced (spec section 8/10.4: ROI and sampling resolution are distinct
//     concerns, and the reduction is explicit in meta2).
//   channels: RGBA uniform.
//   temporal: the frame is resolved by the executor's source time mapping
//     (frameOffset + localTime*frameStep); the kernel is time-independent.
// quality_modes: 1
// precision: rgba32f (full float)
//
// Fill contract: the decoded frame covers the composition frame implied by
// the request — dimensions (region.x + width, region.y + height), identical
// to the CPU reference anchor. Output pixel p maps to the full-res
// composition coordinate (meta.z + p.x*scale, meta.w + p.y*scale), then to
// the nearest source pixel by the fill ratio floor(full * src / full).
// At scale 1 over a same-size frame this is the identity map, so a full
// representation and its reduced siblings read consistent source pixels.

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y)
    uint4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // param0 = (source width, source height, 0, 0)
    float4 param0;
    float4 param1;
}

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_source;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }

    int scale = (int)meta2.z;
    int fullX = (int)meta.z + (int)p.x * scale;
    int fullY = (int)meta.w + (int)p.y * scale;
    int fullWidth = (int)meta.x;
    int fullHeight = (int)meta.y;
    int srcWidth = (int)param0.x;
    int srcHeight = (int)param0.y;
    // Integer nearest fill, identical to the GLSL reference. Plain 32-bit
    // math: full <= 2*kMaxDimension and src <= 64k bound the product well
    // below 2^31.
    int2 s = int2(clamp((fullX * srcWidth) / max(fullWidth, 1), 0, srcWidth - 1),
                  clamp((fullY * srcHeight) / max(fullHeight, 1), 0, srcHeight - 1));
    out_color[p] = in_source.Load(s);
}
