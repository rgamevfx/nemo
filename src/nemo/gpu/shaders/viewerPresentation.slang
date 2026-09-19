// Presentation only: display-referred RGBA32F -> opaque RGBA8 UNORM for Qt's
// native Vulkan texture interface. The composite presents the STORED RGB and
// never scales it by alpha: an association reaches the pixels only through an
// explicit Premult node, so replacing alpha (Shuffle) cannot change the
// presented color while RGB is untouched. There is no alpha mode: alpha is
// data, demanded by name and presented from the demanded image's RGB (issue
// #99), which is what lets a compressed replay carry a matte at all. No color
// transform, resampling, or scene-linear reuse. Rounding is to the nearest
// 8-bit value. The channel uniform is a presentation-only display isolation:
// it never reaches evaluation or the viewer cache.
[[vk::binding(0, 0)]] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] RWTexture2D<float4> surface;
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
    float4 value = saturate(source[tid.xy]);
    float4 result;
    if (channel == 1u)
        result = float4(value.r, value.r, value.r, 1.0);
    else if (channel == 2u)
        result = float4(value.g, value.g, value.g, 1.0);
    else if (channel == 3u)
        result = float4(value.b, value.b, value.b, 1.0);
    else {
        // The composite carries the stored RGB opaquely; alpha is inspected
        // through its own selection and applied only by an explicit Premult.
        result = float4(value.rgb, 1.0);
    }
    surface[tid.xy] = floor(result * 255.0 + 0.5) / 255.0;
}
