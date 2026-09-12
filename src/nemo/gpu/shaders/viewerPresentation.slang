// Presentation only: display-referred straight RGBA32F -> premultiplied
// RGBA8 UNORM for Qt's native Vulkan texture interface. No color transform,
// resampling, or scene-linear reuse. Rounding is to the nearest 8-bit value.
// The channel uniform is a presentation-only display isolation: it never
// reaches evaluation or the viewer cache.
[[vk::binding(0, 0)]] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] RWTexture2D<float4> surface;
[[vk::binding(2, 0)]]
cbuffer PresentationUniforms {
    // Channel::RGBA = 0 (premultiplied, unchanged); Red = 1; Green = 2;
    // Blue = 3; Alpha = 4. Any other value falls back to RGBA.
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
    float4 value = saturate(source[tid.xy]);
    float4 result;
    if (channel == 1u)
        result = float4(value.r, value.r, value.r, 1.0);
    else if (channel == 2u)
        result = float4(value.g, value.g, value.g, 1.0);
    else if (channel == 3u)
        result = float4(value.b, value.b, value.b, 1.0);
    else if (channel == 4u)
        result = float4(value.a, value.a, value.a, 1.0);
    else {
        value.rgb *= value.a;
        result = value;
    }
    surface[tid.xy] = floor(result * 255.0 + 0.5) / 255.0;
}
