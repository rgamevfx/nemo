// Hardware media interop: NV12 -> scene-linear RGBA (issues #10/#21)
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
// Color (issue #21): the conversion is declared in two steps, because
// matrix/range expansion alone yields NONLINEAR R′G′B′, never
// scene-linear RGB. First the Y′CbCr planes are expanded by the declared
// quantization range (param0.y: 0 = limited studio swing, 1 = full) and
// matrix (param0.z: 0 = BT.709, 1 = BT.601) into R′G′B′. Then the declared
// transfer (param0.x: 0 = BT.709, 1 = sRGB, 2 = gamma 2.2, 3 = gamma 2.8,
// 4 = linear pass-through) is inverted into scene-linear Rec.709 — the
// only supported working space; no primaries mapping is applied, and the
// media module rejects other primaries before this kernel ever runs.
// Chroma is sampled at the documented left position: horizontally
// co-sited with even luma columns, vertically midway between luma rows.
// Alpha is opaque (video has no alpha).
//
// bindings (shared effect contract, see testpattern.slang):
//   set 0, binding 0 : EffectUniforms (std140) — meta = (width, height, 0, 0)
//                      param0 = (transfer, range, matrix, 0)
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
    // param0: (transfer, range, matrix, 0) — declared color interpretation
    float4 param0;
    float4 param1;
}

[[vk::binding(0, 1)]]
Sampler2D luma;

[[vk::binding(1, 1)]]
Sampler2D chroma;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

// Y′CbCr → nonlinear R′G′B′ by the declared range and matrix.
float3 ycbcrToRgbPrime(float y, float u, float v, uint range, uint matrix)
{
    float yy, uu, vv;
    if (range == 0) {
        // Limited (studio swing): Y in [16..255 scaled], Cb/Cr centered at 128.
        yy = (y - 16.0 / 255.0) * (255.0 / 219.0);
        uu = (u - 128.0 / 255.0) * (255.0 / 224.0);
        vv = (v - 128.0 / 255.0) * (255.0 / 224.0);
    } else {
        // Full range: nominal [0..1], chroma centered at 128/255.
        yy = y;
        uu = u - 128.0 / 255.0;
        vv = v - 128.0 / 255.0;
    }
    if (matrix == 1) {
        // BT.601: 0.299/0.587/0.114 luma weights.
        return float3(yy + 1.402 * vv, yy - 0.344136 * uu - 0.714136 * vv, yy + 1.772 * uu);
    }
    // BT.709: 0.2126/0.7152/0.0722 luma weights.
    return float3(yy + 1.5748 * vv, yy - 0.1873 * uu - 0.4681 * vv, yy + 1.8556 * uu);
}

// Inverse transfer with signed extension for negative matrix-expanded RGB.
float transferToLinear(float v, uint transfer)
{
    if (transfer == 4) { return v; }  // linear source: already linear-light
    if (transfer == 1) {
        // sRGB.
        return v < 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    if (transfer == 2) { return sign(v) * pow(abs(v), 2.2); }
    if (transfer == 3) { return sign(v) * pow(abs(v), 2.8); }
    // BT.709 (also SMPTE 170M: same published curve).
    return v < 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1.0 / 0.45);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta.x || p.y >= meta.y) { return; }
    // Left chroma position: the chroma sample is horizontally co-sited with
    // the even luma column and vertically midway between luma rows, so the
    // chroma coordinate for luma pixel x is (x + 1) / width (texel center on
    // even pixels, midpoint on odd pixels).
    float2 lumaUv = (float2(p) + 0.5) / float2(meta.x, meta.y);
    float2 chromaUv = (float2(p) + float2(1.0, 0.5)) / float2(meta.x, meta.y);
    // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM plane 1 stores (Cb, Cr): the
    // R8G8 view reads .r = Cb, .g = Cr.
    float2 uvVal = chroma.SampleLevel(chromaUv, 0).rg;  // r = Cb, g = Cr
    float y = luma.SampleLevel(lumaUv, 0).r;
    float3 prime = ycbcrToRgbPrime(y, uvVal.r, uvVal.g, uint(param0.y), uint(param0.z));
    float3 linear = float3(transferToLinear(prime.r, uint(param0.x)),
                           transferToLinear(prime.g, uint(param0.x)),
                           transferToLinear(prime.b, uint(param0.x)));
    out_color[p] = float4(linear, 1.0);
}
