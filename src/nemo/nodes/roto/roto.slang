// Nemo native effect contract (spec section 10.4, issue #93; contribution-local
// module per issue #83)
// ---------------------------------------------------------
// name: roto
// version: 2
// ports:
//   - in_bg   : image2D input (OPTIONAL background, port 0; a generator node has
//               none and the binding is an unread dummy, so the kernel guards
//               every read with the payload's background-present flag)
//   - in_mask : image2D input (OPTIONAL mask, port 1; guarded the same way)
//   - out_color : image2D output (native channel image, issue #98: packed
//     RGBA32F at the logical raster for four stored channels, otherwise
//     R32_SFLOAT scalar planes at W*(H*C); the inherited alpha convention)
// parameters:
//   - opacity (global node opacity), outputChannel (exact stored channel name
//     the matte is written into), replace (clear the target channel before
//     drawing), clip (data-window rule), shutter + samples (node-wide motion
//     blur), maskChannel + invertMask (exact mask channel limiting the shapes).
//   - The shapes themselves are typed document data; they arrive here already
//     resolved, flattened and tessellated as the retained geometry program.
// bindings (native binding contract v7, see GpuGeometry.slang):
//   set 0, binding 0 : common request uniforms
//   set 0, binding 1 : node-local payload ("nemo.nodes.roto.payload.v1")
//   set 0, binding 2 : per-input spatial geometry (background, mask)
//   set 0, binding 3 : produced-raster channel plan (all -1: this node owns its
//                      layout and writes every stored plane itself)
//   set 4, binding 0 : retained Roto geometry program ("nemo.nodes.roto.geometry.v1")
//   set 1, binding 0 = background, binding 1 = mask
//   set 2, binding 0 : output image2D
// dependencies:
//   region: only the requested raster. Every sample's full-resolution anchor is
//     `region origin + (index + 0.5) * samplingScale`, and the matte is evaluated
//     there from the retained program, so a Half or Quarter request agrees with
//     the Full one wherever their anchors coincide.
//   channels: every stored plane of the produced raster is written here (this
//     node owns its channel layout): the target channel carries the matte drawn
//     over the background's same-named channel, every other plane keeps the
//     background's value at the same coordinates, and a plane the background
//     does not carry is zero. No RGB is invented for an alpha matte.
//   temporal: the exposure's subframe samples. `samples <= 1` or `shutter == 0`
//     evaluates the current frame once; otherwise the COMPLETED hierarchy matte
//     of each uniform midpoint sample is averaged (never per-shape blurring).
// quality_modes: 1
// precision: rgba32f (full float)
// alpha: no association is changed anywhere. The matte is drawn as a value into
//   the named channel; RGB is never premultiplied or unpremultiplied and no
//   automatic conversion happens (issue #89 owns that policy and remains an open
//   gap).
//
// Geometry: coordinates are full-resolution pixels, x right / y down. The
// retained program holds every subframe sample's flattened hierarchy (groups
// opened and closed, leaf contours with world positions and signed world feather
// widths) plus each shape's world bounding box, so a sample outside a contour's
// feather reach costs one comparison. Coverage per contour is the even-odd fill
// and the nearest edge's signed distance, ramped by the interpolated feather:
// the frozen arithmetic is written out independently here and in the GLSL
// reference (`Gpu.cpp`) and the CPU reference (`Contribution.cpp`).

#include "../GpuGeometry.slang"

[[vk::binding(0, 0)]]
cbuffer EffectUniforms {
    // meta: (full image width, full image height, region.x, region.y; signed)
    int4 meta;
    // meta2: raster (image width, image height, samplingScale, 0)
    uint4 meta2;
    // misc: (localTime, 0, 0, 0)
    float4 misc;
    // support: half-open RASTER-INDEX rect of the samples that ARE this image's
    // data; (-1,-1,-1,-1) means the raster declares no support.
    int4 support;
    // channels: (stored channel count, components per texel, whether the channel
    // plan fills a stored channel, 0)
    uint4 channels;
    // rgba: the produced raster's stored channel of each R/G/B/A role; -1 absent.
    int4 rgba;
};

[[vk::binding(1, 0)]]
cbuffer RotoPayload {
    // (global opacity, sample weight 1/N, 0, 0)
    float4 rotoParams;
    // (sample count, target stored channel, replace, 0)
    int4 rotoMeta;
    // (background present, mask present, mask stored channel, invert mask)
    int4 rotoFlags;
};

// The retained evaluation program (see the module header in Gpu.cpp for the word
// layout): a header, one table entry per subframe sample, the item records and
// the tessellated vertices, all `uint` words.
[[vk::binding(0, 4)]]
StructuredBuffer<uint> rotoGeometry;

[[vk::binding(0, 1)]]
RWTexture2D<float4> in_bg;

[[vk::binding(1, 1)]]
RWTexture2D<float4> in_mask;

[[vk::binding(0, 2)]]
RWTexture2D<float4> out_color;

// Record and vertex strides of the frozen geometry layout.
static const uint kRotoVertexWords = 3u;
static const uint kRotoShapeWords = 12u;
static const uint kRotoGroupBeginWords = 4u;
static const uint kRotoGroupEndWords = 1u;
static const int kRotoMaxFrames = 33;

float rotoWordFloat(uint index)
{
    return asfloat(rotoGeometry[index]);
}

// One contour's coverage at signed distance `distance` (positive outside) with
// signed feather `feather`, measured in full-resolution pixels.
float rotoFeatherCoverage(float distance, float feather, int profile, float falloff)
{
    if (feather == 0.0) { return distance <= 0.0 ? 1.0 : 0.0; }
    const float width = abs(feather);
    const float low = min(0.0, feather);
    const float high = max(0.0, feather);
    float weight = clamp((high - clamp(distance, low, high)) / width, 0.0, 1.0);
    if (profile == 1) { weight = weight * weight * (3.0 - 2.0 * weight); }
    if (falloff != 1.0) { weight = pow(weight, 1.0 / falloff); }
    return clamp(weight, 0.0, 1.0);
}

float rotoCombine(float accumulated, bool seeded, float current, int blend)
{
    if (!seeded) { return blend == 2 ? 0.0 : current; }
    if (blend == 1) { return accumulated * current; }
    if (blend == 2) { return accumulated * (1.0 - current); }
    return accumulated + current - accumulated * current;
}

float rotoFinished(float coverage, float opacity, bool inverted)
{
    return (inverted ? 1.0 - coverage : coverage) * opacity;
}

// The distance from (px, py) to one segment, with the parameter of the closest
// point so the feather can be interpolated along the edge.
float rotoSegmentDistance(float px, float py, float ax, float ay, float bx, float by, out float parameter)
{
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    float t = 0.0;
    if (lengthSquared > 0.0) { t = clamp(((px - ax) * dx + (py - ay) * dy) / lengthSquared, 0.0, 1.0); }
    parameter = t;
    return length(float2(px - (ax + t * dx), py - (ay + t * dy)));
}

// One shape's coverage at a sample: the world bounding box (already grown by the
// contour's own feather reach) rejects a shape the sample cannot reach, the
// even-odd crossing rule decides inside from outside, and the nearest edge
// supplies the signed distance and the feather the ramp is measured with.
float rotoShapeCoverage(uint record, uint verticesBase, uint vertexOffset, float px, float py)
{
    const float x0 = rotoWordFloat(record + 8u);
    const float y0 = rotoWordFloat(record + 9u);
    const float x1 = rotoWordFloat(record + 10u);
    const float y1 = rotoWordFloat(record + 11u);
    if (px < x0 || px > x1 || py < y0 || py > y1) { return 0.0; }
    const uint count = rotoGeometry[record + 5u];
    if (count < 3u) { return 0.0; }
    const uint first = verticesBase + kRotoVertexWords * (vertexOffset + rotoGeometry[record + 4u]);
    const int profile = int(rotoGeometry[record + 6u]);
    const float falloff = rotoWordFloat(record + 7u);
    bool inside = false;
    float nearest = 3.4e38;
    float nearestFeather = 0.0;
    for (uint index = 0u; index < count; ++index) {
        const uint a = first + kRotoVertexWords * index;
        const uint b = first + kRotoVertexWords * ((index + 1u) % count);
        const float ax = rotoWordFloat(a);
        const float ay = rotoWordFloat(a + 1u);
        const float bx = rotoWordFloat(b);
        const float by = rotoWordFloat(b + 1u);
        if ((ay <= py) != (by <= py)) {
            const float crossing = ax + (py - ay) * (bx - ax) / (by - ay);
            if (px < crossing) { inside = !inside; }
        }
        float parameter = 0.0;
        const float distance = rotoSegmentDistance(px, py, ax, ay, bx, by, parameter);
        if (distance < nearest) {
            nearest = distance;
            nearestFeather = lerp(rotoWordFloat(a + 2u), rotoWordFloat(b + 2u), parameter);
        }
    }
    return rotoFeatherCoverage(inside ? -nearest : nearest, nearestFeather, profile, falloff);
}

// One subframe sample's completed hierarchy matte: groups accumulate their
// children, then apply their own inversion and opacity, then blend with their
// siblings. The frame stack is bounded by the program's own build-time depth
// limit.
float rotoSampleMatte(int sampleIndex, float px, float py)
{
    const uint sampleCount = uint(rotoMeta.x);
    const uint verticesBase = rotoGeometry[2u];
    const uint itemsBase = 3u + 3u * sampleCount;
    const uint table = 3u + 3u * uint(sampleIndex);
    const uint itemsStart = itemsBase + rotoGeometry[table];
    const uint itemsEnd = itemsStart + rotoGeometry[table + 1u];
    const uint vertexOffset = rotoGeometry[table + 2u];

    float accumulated = 0.0;
    bool seeded = false;
    // Every slot is written before it is read (a group always pushes before its
    // own GroupEnd pops), but the arrays are zero-initialized so no path can read
    // an undefined lane even if a program were ever malformed.
    float stackAccumulated[kRotoMaxFrames] = {};
    float stackOpacity[kRotoMaxFrames] = {};
    float stackSeeded[kRotoMaxFrames] = {};
    float stackInverted[kRotoMaxFrames] = {};
    int stackBlend[kRotoMaxFrames] = {};
    int depth = 0;
    for (uint cursor = itemsStart; cursor < itemsEnd;) {
        const int kind = int(rotoGeometry[cursor]);
        if (kind == 0) {
            const float coverage = rotoShapeCoverage(cursor, verticesBase, vertexOffset, px, py);
            const float value = rotoFinished(coverage, rotoWordFloat(cursor + 2u), rotoGeometry[cursor + 3u] != 0u);
            accumulated = rotoCombine(accumulated, seeded, value, int(rotoGeometry[cursor + 1u]));
            seeded = true;
            cursor += kRotoShapeWords;
            continue;
        }
        if (kind == 1) {
            if (depth + 1 < kRotoMaxFrames) {
                ++depth;
                stackAccumulated[depth] = accumulated;
                stackSeeded[depth] = seeded ? 1.0 : 0.0;
                stackOpacity[depth] = rotoWordFloat(cursor + 2u);
                stackInverted[depth] = rotoGeometry[cursor + 3u] != 0u ? 1.0 : 0.0;
                stackBlend[depth] = int(rotoGeometry[cursor + 1u]);
                accumulated = 0.0;
                seeded = false;
            }
            cursor += kRotoGroupBeginWords;
            continue;
        }
        if (depth > 0) {
            const float group =
                rotoFinished(seeded ? accumulated : 0.0, stackOpacity[depth], stackInverted[depth] != 0.0);
            accumulated = stackAccumulated[depth];
            seeded = stackSeeded[depth] != 0.0;
            accumulated = rotoCombine(accumulated, seeded, group, stackBlend[depth]);
            seeded = true;
            --depth;
        }
        cursor += kRotoGroupEndWords;
    }
    return seeded ? accumulated : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    // The described image's data support (native binding contract v7): a sample
    // outside it is transparent black in every stored plane.
    if (support.z >= 0 && support.w >= 0 &&
        ((int(p.x) < support.x || int(p.y) < support.y || int(p.x) >= support.x + support.z ||
          int(p.y) >= support.y + support.w))) {
        gpuZeroPlanes(out_color, int2(p), planeHeight, channels.y, channels.x);
        return;
    }

    const int scale = int(meta2.z);
    const float worldX = float(meta.z) + (float(p.x) + 0.5) * float(scale);
    const float worldY = float(meta.w) + (float(p.y) + 0.5) * float(scale);
    const int sampleCount = rotoMeta.x;

    // The mean of the COMPLETED hierarchy matte over the exposure's subframe
    // samples, then the node's global opacity and the optional mask limit.
    float matte = 0.0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        matte += rotoSampleMatte(sampleIndex, worldX, worldY);
    }
    matte = matte * rotoParams.y * rotoParams.x;
    if (rotoFlags.y != 0) {
        const int channel = rotoFlags.z;
        const int2 extent = int2(inputGeometry[1].extent.xy);
        const int2 q = int2(p) + inputGeometry[1].regionAndOffset.zw;
        float coverage = 0.0;
        if (channel >= 0 && q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y) {
            coverage = clamp(gpuLoadChannel(in_mask, q, channel, extent.y, inputGeometry[1].channels.y), 0.0, 1.0);
        }
        if (rotoFlags.w != 0) { coverage = 1.0 - coverage; }
        matte *= coverage;
    }

    // Every stored plane of the produced raster (this node owns its channel
    // layout): the target channel carries the matte, drawn over the background's
    // same-named channel unless `replace` clears it first, and every other plane
    // keeps the background's value at the same coordinate.
    const int target = rotoMeta.y;
    const bool replace = rotoMeta.z != 0;
    const bool haveBackground = rotoFlags.x != 0;
    const int2 backgroundExtent = int2(inputGeometry[0].extent.xy);
    const int2 backgroundPixel = int2(p) + inputGeometry[0].regionAndOffset.zw;
    const bool backgroundInside = haveBackground && backgroundPixel.x >= 0 && backgroundPixel.y >= 0 &&
                                  backgroundPixel.x < backgroundExtent.x && backgroundPixel.y < backgroundExtent.y;
    float4 pixel = float4(0.0, 0.0, 0.0, 0.0);
    for (uint channel = 0u; channel < channels.x; ++channel) {
        const int index = int(channel);
        float value = 0.0;
        if (index == target) {
            const float incoming =
                backgroundInside && index < int(inputGeometry[0].channels.x)
                    ? gpuLoadChannel(in_bg, backgroundPixel, index, backgroundExtent.y, inputGeometry[0].channels.y)
                    : 0.0;
            value = replace ? matte : matte + (1.0 - matte) * incoming;
        } else if (backgroundInside && index < int(inputGeometry[0].channels.x)) {
            value = gpuLoadChannel(in_bg, backgroundPixel, index, backgroundExtent.y, inputGeometry[0].channels.y);
        }
        gpuPixelChannel(pixel, out_color, int2(p), planeHeight, channels.y, index, value);
    }
    gpuPixelStore(out_color, int2(p), channels.y, pixel);
}
