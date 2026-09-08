// Hardware media interop: NV12 -> scene-linear RGBA (issue #10)
// ---------------------------------------------------------
// name: mediaConvert
// version: 1
//
// Converts one decoded NV12 frame (Vulkan video decode output, two planes:
// Y plane 8-bit, interleaved CbCr plane 8-bit) into the application image
// contract: RGBA float32, straight alpha, scene-linear linear-light values
// (spec section 10.4). The frame is decoded into Vulkan-resident images on
// the application device; this kernel keeps the pixels on device — decode
// to contract conversion never touches the CPU.
//
// Color: BT.709 matrix, limited range (studio swing) as declared for the
// media contract; Y in [16..235], Cb/Cr in [16..240] map to [0..1]. Alpha
// is opaque (video has no alpha).
//
// bindings (shared effect contract, see testpattern.slang):
//   set 0, binding 0 : EffectUniforms (std140) — meta = (width, height, 0, 0)
//   set 1, binding 0 : Y plane  (sampler2D, R8)
//   set 1, binding 1 : UV plane (sampler2D, R8G8)
//   set 2, binding 0 : out_color image2D (rgba32f storage, straight alpha)
// dependencies:
//   region: output pixel depends on its own chroma sample (no neighborhood).
//   channels: RGB only (alpha opaque).
//   temporal: none.
// quality_modes: 1
// precision: rgba32f

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (region width, region height, region.x, region.y)
    uint4 meta;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    float4 param0;
    float4 param1;
}

[[vk::binding(0, 1)]]
Sampler2D luma;

[[vk::binding(1, 1)]]
Sampler2D chroma;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

float3 yuvToRgb709(float y, float u, float v)
{
    // BT.709 limited range: Y' = 16..235, Cb/Cr = 16..240 -> [0..1].
    float yy = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float uu = (u - 128.0 / 255.0) * (255.0 / 224.0);
    float vv = (v - 128.0 / 255.0) * (255.0 / 224.0);
    float r = yy + 1.5748 * vv;
    float g = yy - 0.1873 * uu - 0.4681 * vv;
    float b = yy + 1.8556 * uu;
    return float3(r, g, b);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    // Luma samples at its own pixel center; 4:2:0 co-sited chroma covers
    // luma pixels {2k, 2k+1} at texel k, so chroma coordinate for luma
    // pixel x is (x + 1) / width (texel center on even pixels, midpoint on
    // odd pixels).
    float2 lumaUv = (float2(p) + 0.5) / float2(meta.x, meta.y);
    float2 chromaUv = (float2(p) + 1.0) / float2(meta.x, meta.y);
    // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM plane 1 stores (Cb, Cr): the
    // R8G8 view reads .r = Cb, .g = Cr.
    float2 uvVal = chroma.SampleLevel(chromaUv, 0).rg;  // r = Cb, g = Cr
    float y = luma.SampleLevel(lumaUv, 0).r;
    float3 rgb = yuvToRgb709(y, uvVal.r, uvVal.g);
    out_color[p] = float4(rgb, 1.0);
}
