// Presentation-only channel isolation shared by live float and BC7 replay.
// Stored RGB is opaque (#99); alpha is inspected through its demanded data
// channel or applied by an explicit Premult node, never by the viewer copy.
float4 presentViewerChannels(float4 value, uint channel)
{
    value = saturate(value);
    float4 result;
    if (channel == 1u)
        result = float4(value.r, value.r, value.r, 1.0);
    else if (channel == 2u)
        result = float4(value.g, value.g, value.g, 1.0);
    else if (channel == 3u)
        result = float4(value.b, value.b, value.b, 1.0);
    else
        result = float4(value.rgb, 1.0);
    return floor(result * 255.0 + 0.5) / 255.0;
}
