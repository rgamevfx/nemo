// Roto: the native module's payload, retained geometry and GLSL reference
// (issue #93, stories 58-68, 71).
//
// Roto's shapes are TYPED DOCUMENT DATA, not a shader-side lookup table: the
// worker-side preparation in this file resolves the animated hierarchy through
// the model's own sampler, flattens it with the shared RotoGeometry build, and
// packs the whole result — every subframe sample's evaluation program and its
// tessellated vertices — into the executor's retained geometry buffer (set 4,
// binding 0). One dispatch then rasterizes the matte; there is no per-node
// allocation, no per-shape submission and no host wait, and no second upload
// between the samples of one exposure.
//
// The kernel is an INDEPENDENT implementation of the frozen matte arithmetic in
// Contribution.cpp (ADR-0004): it tests every sample against every edge of a
// contour (even-odd fill plus the nearest edge's interpolated feather), where the
// CPU reference rasterizes by scanline with one crossing list per row. Their
// agreement is therefore evidence about the ramp and the compositor, not a shared
// mistake.
//
// Frozen geometry layout ("nemo.nodes.roto.geometry.v1"), all words `uint`:
//
//   [0]  layout version (1)
//   [1]  sample count N
//   [2]  absolute word index of the vertex block (`verticesBase`)
//   [3 + 3*s + 0]  sample s: item offset, relative to `itemsBase`
//   [3 + 3*s + 1]  sample s: item word count
//   [3 + 3*s + 2]  sample s: vertex offset, relative to `verticesBase`, in VERTICES
//   itemsBase = 3 + 3*N, items are contiguous in sample order, and
//   verticesBase = itemsBase + total item words.
//
//   Shape record (12 words):
//     [0] kind 0, [1] blend (0 Combine, 1 Intersect, 2 Subtract),
//     [2] floatBits(opacity), [3] inverted, [4] first vertex (sample-relative),
//     [5] vertex count, [6] feather profile (0 Linear, 1 Smooth),
//     [7] floatBits(falloff), [8..11] bbox x0,y0,x1,y1 as float bits.
//   GroupBegin record (4 words): [0] kind 1, [1] blend, [2] floatBits(opacity),
//     [3] inverted.
//   GroupEnd record (1 word): [0] kind 2.
//   Vertex (3 words): x, y, feather as float bits.
//
// A group's children lie between its own two records, so the kernel walks the
// program with a bounded stack: children combine among themselves, then the
// group's inversion and opacity apply to that value, then it blends with its
// siblings — exactly the CPU adapter's order.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/roto/Parameters.hpp"
#include "nemo/nodes/roto/RotoGeometry.hpp"

namespace nemo::eval::nodes {
namespace {

// The renderer-local geometry vocabulary lives in `nemo::nodes` beside the
// contribution (the core `Roto*` data model is in `nemo` itself).
using nemo::nodes::buildRotoGeometry;
using nemo::nodes::kMaxRotoGeometryWords;
using nemo::nodes::RotoGeometry;
using nemo::nodes::RotoItem;
using nemo::nodes::rotoPhysicalAspect;
using nemo::nodes::RotoVertex;

// Frozen geometry layout version and the item encodings above.
inline constexpr std::uint32_t kRotoGeometryVersion = 1u;
inline constexpr std::uint32_t kRotoShapeWords = 12u;
inline constexpr std::uint32_t kRotoGroupBeginWords = 4u;
inline constexpr std::uint32_t kRotoGroupEndWords = 1u;
inline constexpr std::uint32_t kRotoVertexWords = 3u;

struct RotoPayload {
    // (global opacity, sample weight 1/N, 0, 0)
    float params[4]{};
    // (sample count, target stored channel, replace, 0)
    std::int32_t meta[4]{};
    // (background present, mask present, mask stored channel, invert mask)
    std::int32_t flags[4]{};
};
static_assert(sizeof(RotoPayload) == 48);

constexpr const char* kRotoGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform RotoPayload {
    // (global opacity, sample weight 1/N, 0, 0)
    vec4 rotoParams;
    // (sample count, target stored channel, replace, 0)
    ivec4 rotoMeta;
    // (background present, mask present, mask stored channel, invert mask)
    ivec4 rotoFlags;
};

// The retained evaluation program (see the file header for the word layout).
layout(std430, set = 4, binding = 0) readonly buffer RotoGeometryWords {
    uint rotoGeometry[];
};
)GLSL";

constexpr const char* kRotoGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_bg;
layout(set = 1, binding = 1) restrict readonly uniform image2D in_mask;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

// Record and vertex strides of the frozen geometry layout (file header).
const uint kRotoVertexWords = 3u;
const uint kRotoShapeWords = 12u;
const uint kRotoGroupBeginWords = 4u;
const uint kRotoGroupEndWords = 1u;
const int kRotoMaxFrames = 33;

// One contour's coverage at signed distance `distance` (positive outside) with
// signed feather `feather`, measured in full-resolution pixels.
float rotoFeatherCoverage(float distance, float feather, int profile, float falloff) {
    if (feather == 0.0) { return distance <= 0.0 ? 1.0 : 0.0; }
    const float width = abs(feather);
    const float low = min(0.0, feather);
    const float high = max(0.0, feather);
    float weight = clamp((high - clamp(distance, low, high)) / width, 0.0, 1.0);
    if (profile == 1) { weight = weight * weight * (3.0 - 2.0 * weight); }
    if (falloff != 1.0) { weight = pow(weight, 1.0 / falloff); }
    return clamp(weight, 0.0, 1.0);
}

float rotoCombine(float accumulated, bool seeded, float current, int blend) {
    if (!seeded) { return blend == 2 ? 0.0 : current; }
    if (blend == 1) { return accumulated * current; }
    if (blend == 2) { return accumulated * (1.0 - current); }
    return accumulated + current - accumulated * current;
}

float rotoFinished(float coverage, float opacity, bool inverted) {
    return (inverted ? 1.0 - coverage : coverage) * opacity;
}

// The distance from (px, py) to one segment, with the parameter of the closest
// point so the feather can be interpolated along the edge.
float rotoSegmentDistance(float px, float py, float ax, float ay, float bx, float by, out float parameter) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    float t = 0.0;
    if (lengthSquared > 0.0) { t = clamp(((px - ax) * dx + (py - ay) * dy) / lengthSquared, 0.0, 1.0); }
    parameter = t;
    return length(vec2(px - (ax + t * dx), py - (ay + t * dy)));
}

float rotoWordFloat(uint index) { return uintBitsToFloat(rotoGeometry[index]); }

// One shape's coverage at a sample: the bbox rejects a contour the sample cannot
// reach (its own feather reach included), the even-odd rule decides inside from
// outside, and the nearest edge supplies the signed distance and the feather the
// ramp is measured with.
float rotoShapeCoverage(uint record, uint verticesBase, uint vertexOffset, float px, float py) {
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
            nearestFeather = mix(rotoWordFloat(a + 2u), rotoWordFloat(b + 2u), parameter);
        }
    }
    return rotoFeatherCoverage(inside ? -nearest : nearest, nearestFeather, profile, falloff);
}

// One subframe sample's completed hierarchy matte: the walk is depth-bounded by
// the program's own build-time limit, so the frame stack cannot overflow.
float rotoSampleMatte(int sampleIndex, float px, float py) {
    const uint sampleCount = uint(rotoMeta.x);
    const uint verticesBase = rotoGeometry[2u];
    const uint itemsBase = 3u + 3u * sampleCount;
    const uint table = 3u + 3u * uint(sampleIndex);
    const uint itemsStart = itemsBase + rotoGeometry[table];
    const uint itemsEnd = itemsStart + rotoGeometry[table + 1u];
    const uint vertexOffset = rotoGeometry[table + 2u];

    float accumulated = 0.0;
    bool seeded = false;
    float stackAccumulated[kRotoMaxFrames];
    float stackOpacity[kRotoMaxFrames];
    float stackSeeded[kRotoMaxFrames];
    float stackInverted[kRotoMaxFrames];
    int stackBlend[kRotoMaxFrames];
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
        // A group closes: its combined value is finished, then blended into the
        // parent frame it interrupted.
        if (depth > 0) {
            const float group = rotoFinished(seeded ? accumulated : 0.0, stackOpacity[depth],
                                             stackInverted[depth] != 0.0);
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

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int scale = int(meta2.z);
    const float worldX = float(meta.z) + (float(p.x) + 0.5) * float(scale);
    const float worldY = float(meta.w) + (float(p.y) + 0.5) * float(scale);
    const int sampleCount = rotoMeta.x;
    const int planeHeight = int(meta2.y);

    // The mean of the COMPLETED hierarchy matte over the exposure's subframe
    // samples, then the node's global opacity and the optional mask limit.
    float matte = 0.0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        matte += rotoSampleMatte(sampleIndex, worldX, worldY);
    }
    matte = matte * rotoParams.y * rotoParams.x;
    if (rotoFlags.y != 0) {
        const int channel = rotoFlags.z;
        const ivec2 extent = ivec2(inputGeometry[1].extent.xy);
        const ivec2 q = ivec2(p) + inputGeometry[1].regionAndOffset.zw;
        float coverage = 0.0;
        if (channel >= 0 && q.x >= 0 && q.y >= 0 && q.x < extent.x && q.y < extent.y) {
            coverage = clamp(gpuLoadChannel(in_mask, q, channel, extent.y, inputGeometry[1].channels.y), 0.0, 1.0);
        }
        if (rotoFlags.w != 0) { coverage = 1.0 - coverage; }
        matte *= coverage;
    }

    // Every stored plane of the produced raster (this node owns its channel
    // layout): the target channel carries the matte, drawn over the background's
    // same-named channel when `replace` is off (a channel the background does not
    // carry starts empty), and every other plane keeps the background's value at
    // the same coordinates.
    const int target = rotoMeta.y;
    const bool replace = rotoMeta.z != 0;
    const bool haveBackground = rotoFlags.x != 0;
    const ivec2 backgroundExtent = ivec2(inputGeometry[0].extent.xy);
    const ivec2 backgroundPixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    const bool backgroundInside = haveBackground && backgroundPixel.x >= 0 && backgroundPixel.y >= 0 &&
                                  backgroundPixel.x < backgroundExtent.x && backgroundPixel.y < backgroundExtent.y;
    vec4 pixel = vec4(0.0);
    for (uint channel = 0u; channel < channels.x; ++channel) {
        const int index = int(channel);
        float value = 0.0;
        if (index == target) {
            const float incoming = backgroundInside && index < int(inputGeometry[0].channels.x)
                                       ? gpuLoadChannel(in_bg, backgroundPixel, index, backgroundExtent.y,
                                                                     inputGeometry[0].channels.y)
                                                    : 0.0;
            value = replace ? matte : matte + (1.0 - matte) * incoming;
        } else if (backgroundInside && index < int(inputGeometry[0].channels.x)) {
            value = gpuLoadChannel(in_bg, backgroundPixel, index, backgroundExtent.y, inputGeometry[0].channels.y);
        }
        gpuPixelChannel(pixel, out_color, ivec2(p), planeHeight, channels.y, index, value);
    }
    gpuPixelStore(out_color, ivec2(p), channels.y, pixel);
}
)GLSL";

[[nodiscard]] EffectPassDefinition rotoPass() {
    return EffectPassDefinition{
        .id = "roto",
        .shader = "roto/roto",
        .glsl = nemo::nodes::gpuGlsl(kRotoGlslPayload, kRotoGlslBody),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Input, 1}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
        // The flattened hierarchy is retained in the generic geometry binding
        // (set 4, binding 0) for the whole dispatch: one upload covers every
        // subframe sample of the exposure.
        .geometry = true,
    };
}

[[nodiscard]] std::uint32_t rotoFloatBits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Append one sample's evaluation program and vertices, returning the sample's
// (item offset, item words, vertex offset).
struct RotoPackedSample {
    std::uint32_t itemOffset{0};
    std::uint32_t itemWords{0};
    std::uint32_t vertexOffset{0};
};

[[nodiscard]] RotoPackedSample packRotoGeometry(const RotoGeometry& geometry, std::vector<std::uint32_t>& items,
                                                std::vector<std::uint32_t>& vertices) {
    RotoPackedSample packed;
    packed.itemOffset = static_cast<std::uint32_t>(items.size());
    packed.vertexOffset = static_cast<std::uint32_t>(vertices.size() / kRotoVertexWords);
    for (const RotoItem& item : geometry.items) {
        if (item.kind == RotoItem::Kind::GroupBegin) {
            items.push_back(1u);
            items.push_back(static_cast<std::uint32_t>(item.blend));
            items.push_back(rotoFloatBits(item.opacity));
            items.push_back(item.inverted ? 1u : 0u);
            continue;
        }
        if (item.kind == RotoItem::Kind::GroupEnd) {
            items.push_back(2u);
            continue;
        }
        items.push_back(0u);
        items.push_back(static_cast<std::uint32_t>(item.blend));
        items.push_back(rotoFloatBits(item.opacity));
        items.push_back(item.inverted ? 1u : 0u);
        items.push_back(item.firstVertex);
        items.push_back(item.vertexCount);
        items.push_back(item.profile == RotoFeatherProfile::Smooth ? 1u : 0u);
        items.push_back(rotoFloatBits(item.falloff));
        for (const float bound : item.bounds) {
            items.push_back(rotoFloatBits(bound));
        }
        for (std::uint32_t index = 0; index < item.vertexCount; ++index) {
            const RotoVertex& vertex = geometry.vertices[item.firstVertex + index];
            vertices.push_back(rotoFloatBits(vertex.x));
            vertices.push_back(rotoFloatBits(vertex.y));
            vertices.push_back(rotoFloatBits(vertex.feather));
        }
    }
    packed.itemWords = static_cast<std::uint32_t>(items.size()) - packed.itemOffset;
    return packed;
}

// Worker-side value preparation: the same typed parameters and the same shared
// geometry build the CPU adapter uses, packed once for the whole exposure. A
// parameter problem, an unrepresentable shape or an exceeded bound names the node
// here, before any device work — and the packed words become the retained
// geometry buffer the executor owns.
[[nodiscard]] GpuPreparation prepareRoto(const GpuNodeContext& context) {
    const RotoParameters params = effectiveRoto(context.catalog, context.node, context.effectiveParams);
    if (!isSamplingScale(context.request.samplingScale)) {
        failNode(context.node, "sampling scale " + std::to_string(context.request.samplingScale) +
                                   " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const float aspect = rotoPhysicalAspect(context.node, context.owningFormat, context.pixelAspect);
    const std::vector<std::string>& described = context.description.channels;
    const int target = channelIndex(described, params.outputChannel);
    if (target < 0) {
        failNode(context.node, "the described output does not carry the target channel '" + params.outputChannel + "'");
    }
    const bool background = !context.inputDescriptions.empty() && context.inputDescriptions[0] != nullptr;
    const bool maskPresent = context.maskPresent;
    int maskChannel = -1;
    if (!params.maskChannel.empty() && maskPresent) {
        const ImageDescription* mask = context.inputDescriptions.size() > 1 ? context.inputDescriptions[1] : nullptr;
        maskChannel = mask != nullptr ? channelIndex(mask->channels, params.maskChannel) : -1;
    }

    const RotoSampleTimes samples = rotoSampleTimes(params, context.request.localTime);
    const std::size_t sampleCount = samples.count;
    const std::size_t itemsBase = 3 + 3 * sampleCount;
    std::vector<std::uint32_t> items;
    std::vector<std::uint32_t> vertices;
    std::vector<RotoPackedSample> table;
    table.reserve(sampleCount);
    for (std::size_t sample = 0; sample < sampleCount; ++sample) {
        const RotoData snapshot =
            context.document != nullptr && context.network != kInvalidNetwork
                ? evaluateRoto(*context.document, context.network, context.node.id, samples.times[sample])
                : (context.node.roto != nullptr ? *context.node.roto : RotoData{});
        table.push_back(packRotoGeometry(buildRotoGeometry(context.node, snapshot, aspect), items, vertices));
        // Bounded as it is built, so a dense scene is refused after one sample's
        // overshoot instead of after allocating the whole program.
        if (itemsBase + items.size() + vertices.size() > kMaxRotoGeometryWords) {
            failNode(context.node, "the Roto geometry for " + std::to_string(sampleCount) +
                                       " motion-blur samples exceeds " + std::to_string(kMaxRotoGeometryWords) +
                                       " geometry words; reduce the shape count, the motion-blur samples or the "
                                       "contour detail (the geometry is refused, not truncated)");
        }
    }

    std::vector<std::uint32_t> words(itemsBase + items.size() + vertices.size(), 0u);
    words[0] = kRotoGeometryVersion;
    words[1] = static_cast<std::uint32_t>(sampleCount);
    words[2] = static_cast<std::uint32_t>(itemsBase + items.size());
    for (std::size_t sample = 0; sample < sampleCount; ++sample) {
        const std::size_t entry = 3 + 3 * sample;
        words[entry] = table[sample].itemOffset;
        words[entry + 1] = table[sample].itemWords;
        words[entry + 2] = table[sample].vertexOffset;
    }
    std::copy(items.begin(), items.end(), words.begin() + static_cast<std::ptrdiff_t>(itemsBase));
    std::copy(vertices.begin(), vertices.end(), words.begin() + static_cast<std::ptrdiff_t>(itemsBase + items.size()));

    RotoPayload payload;
    payload.params[0] = params.opacity;
    payload.params[1] = 1.0F / static_cast<float>(sampleCount);
    payload.meta[0] = static_cast<std::int32_t>(sampleCount);
    payload.meta[1] = target;
    payload.meta[2] = params.replace ? 1 : 0;
    payload.flags[0] = background ? 1 : 0;
    payload.flags[1] = maskPresent && !params.maskChannel.empty() ? 1 : 0;
    payload.flags[2] = maskChannel;
    payload.flags[3] = params.invertMask ? 1 : 0;

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.geometry.resize(words.size() * sizeof(std::uint32_t));
    std::memcpy(preparation.geometry.data(), words.data(), preparation.geometry.size());
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution rotoGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::rotoContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.roto.payload.v1";
    implementation.payloadSize = sizeof(RotoPayload);
    implementation.passes = {rotoPass()};
    implementation.prepare = &prepareRoto;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
