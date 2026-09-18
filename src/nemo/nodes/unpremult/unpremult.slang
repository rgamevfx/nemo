// Unpremult v1: divide selected stored primary channels by one selected
// primary channel. Exact zero writes zero; no epsilon or clamp is applied.

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    int4 meta;
    uint4 meta2;
    float4 misc;
    int4 support;
    uint4 channels;
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer UnpremultPayload {
    int4 unpremultOperation;  // (selected role bits, divisor role, 0, 0)
};

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_main;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    if (support.z >= 0 && support.w >= 0 &&
        ((int)p.x < support.x || (int)p.y < support.y || (int)p.x >= support.x + support.z ||
         (int)p.y >= support.y + support.w)) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.y, channels.x);
        return;
    }
    int2 mainPixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    int2 mainExtent = int2(inputGeometry[0].extent.xy);
    bool inside = mainPixel.x >= 0 && mainPixel.y >= 0 && mainPixel.x < mainExtent.x && mainPixel.y < mainExtent.y;
    float4 value = inside ? gpuLoadRgba(in_main, mainPixel, inputGeometry[0].rgba, mainExtent.y,
                                        inputGeometry[0].channels.y)
                          : float4(0.0, 0.0, 0.0, 0.0);
    float divisor = value[unpremultOperation.y];
    for (int role = 0; role < 4; ++role) {
        if ((unpremultOperation.x & (1 << role)) != 0) {
            value[role] = divisor == 0.0 ? 0.0 : value[role] / divisor;
        }
    }
    gpuStorePixel(out_color, int2(p), planeHeight, channels.x, channels.y, channels.z, rgba, value, in_main,
                  mainExtent.y, inputGeometry[0].channels.y);
}
