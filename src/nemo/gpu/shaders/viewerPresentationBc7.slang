// Direct BC7 replay presentation (issue #106).
// -------------------------------------------
// name: viewerPresentationBc7
// version: 1
//
// Samples a completed display-referred BC7 frame straight into the established
// shared RGBA8 presentation surface. This is the replay counterpart of
// viewerPresentation.slang, and the only pass that turns a cached BC7 frame
// into pixels: there is no reconstruction of a full float frame, no second
// input or viewing transform, and no resampling. The viewing transform was
// baked once when the frame was encoded, so replay re-uses it.
//
// Sampling is deliberately 1:1 and unfiltered: a NEAREST sampler at texel
// centres selects exactly the texel the block encodes, and
// VK_FORMAT_BC7_UNORM_BLOCK is sampled as linear UNORM — the blocks already
// hold display-encoded samples, so no sRGB decode and no Rec.709/sRGB
// reinterpretation can occur. BC7 decompression is the driver's texture
// sampling; the block layout never reaches this kernel.
//
// The channel uniform is the identical presentation-only display isolation the
// live path applies (issue #99): Red/Green/Blue replicate that channel over an
// opaque alpha, and RGBA presents the stored RGB opaquely. Alpha is retained in
// the cache and is decoded by the sampler, but it reaches the surface only
// through the graph's own Premult node — presenting a cached frame cannot
// composite it differently from presenting the live frame beside it.
//
// Bindings (set 0):
//   binding 0 : source  Sampler2D — BC7_UNORM_BLOCK texture, logical extent of
//              the requested raster (odd dimensions stay odd; partial edge
//              blocks are never revealed because the bounds test below rejects
//              every texel outside the surface).
//   binding 1 : surface RWTexture2D<float4> — shared RGBA8 UNORM output.
//   binding 2 : PresentationUniforms — channel selection (see ViewerChannel).
// dependencies:
//   region: output texel depends on its own texel only (no neighbourhood).
//   channels: R,G,B written; A is opaque (the composite never scales by alpha).
//   temporal: none.
// quality_modes: 1
// precision: rgba8 unorm presentation surface
#include "ViewerChannels.slang"

[[vk::binding(0, 0)]]
Sampler2D source;

[[vk::binding(1, 0)]]
RWTexture2D<float4> surface;

[[vk::binding(2, 0)]]
cbuffer PresentationUniforms {
    // Channel::RGBA = 0 (the composite: stored RGB, opaque); Red = 1;
    // Green = 2; Blue = 3. Any other value falls back to RGBA.
    uint channel;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    surface.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
        return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    surface[tid.xy] = presentViewerChannels(source.SampleLevel(uv, 0), channel);
}
