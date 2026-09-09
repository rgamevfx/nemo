// Presentation only: display-referred straight RGBA32F -> premultiplied
// RGBA8 UNORM for Qt's native Vulkan texture interface. No color transform,
// resampling, or scene-linear reuse. Rounding is to the nearest 8-bit value.
[[vk::binding(0, 0)]] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] RWTexture2D<float4> surface;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width, height;
    surface.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
        return;
    float4 value = saturate(source[tid.xy]);
    value.rgb *= value.a;
    surface[tid.xy] = floor(value * 255.0 + 0.5) / 255.0;
}
