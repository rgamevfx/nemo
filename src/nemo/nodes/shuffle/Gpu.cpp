// Shuffle: named channel remapping with two inputs, constants and output
// channel creation (issue #90 node-local GPU module).
//
// Shuffle owns its payload (eight resolved mapping rows plus the base format the
// zero/one constants cover) at set 0 binding 1 and its own independent GLSL
// reference source; its Slang kernel is shuffle.slang. Port 0 is B (the
// required base), port 1 is A (optional).
//
// This node OWNS its channel layout (NodeContribution::ownsChannelLayout): the
// executor's common auxiliary preservation deliberately does not run for it,
// because every output plane is produced here by the authored mapping — the
// untouched B channels, each row's value, and the frozen zero of a channel no
// rule provides. The mapping is resolved to plane indices in preparation, never
// per pixel.

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/shuffle/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Row kinds as the kernel consumes them: the authored ShuffleSource, plus -1
// for a row that writes nothing (disabled, or an output plane the description
// does not carry).
enum class ShuffleRowKind : std::int32_t { Disabled = -1, B = 0, A = 1, Zero = 2, One = 3 };

// Node-local payload: the eight resolved rows and the base format.
//
// A row is (kind, source plane, output plane, 0). `kind` is the authored
// source; `source plane` is the exact stored channel plane of the named input
// (-1 when that input does not carry the name, which is the frozen zero policy,
// and always -1 for a constant row); `output plane` is the plane of this node's
// described result the row writes.
//
// `format` is the base input's format (width, height, 0, 0): the zero/one
// constants are defined on the main image's format, so a white constant is 1
// only where the base image claims a pixel — never on an overscan sample the
// union data window may carry from A.
struct ShufflePayload {
    std::int32_t rows[kShuffleRows][4]{};
    std::int32_t format[4]{};
};
static_assert(sizeof(ShufflePayload) % 16 == 0);

constexpr const char* kShuffleGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform ShufflePayload {
    // (kind, source plane, output plane, 0) per authored row; kind -1 = the row
    // writes nothing, 0 = input B, 1 = input A, 2 = constant zero, 3 = constant
    // one.
    ivec4 shuffleRows[8];
    // (base format width, base format height, 0, 0).
    ivec4 shuffleFormat;
};
)GLSL";

constexpr const char* kShuffleGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_base;   // port 0: B
layout(set = 1, binding = 1) restrict readonly uniform image2D in_second;  // port 1: A (optional)
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    const int planeHeight = int(meta2.y);
    // The described result's data support (native binding contract v6). Shuffle
    // produces every plane itself, so the outside-support branch zeroes them
    // explicitly instead of relying on a channel plan (issue #90).
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), planeHeight);
        return;
    }

    // Each input is located through its own raster origin and extent: a sample
    // it does not hold is outside its data (transparent black), and A's samples
    // keep their own coordinates rather than being stretched onto B.
    ivec2 basePixel = ivec2(p) + inputGeometry[0].regionAndOffset.zw;
    ivec2 baseExtent = ivec2(inputGeometry[0].extent.xy);
    bool baseInside =
        basePixel.x >= 0 && basePixel.y >= 0 && basePixel.x < baseExtent.x && basePixel.y < baseExtent.y;
    ivec2 secondPixel = ivec2(p) + inputGeometry[1].regionAndOffset.zw;
    ivec2 secondExtent = ivec2(inputGeometry[1].extent.xy);
    bool secondInside =
        secondPixel.x >= 0 && secondPixel.y >= 0 && secondPixel.x < secondExtent.x && secondPixel.y < secondExtent.y;

    // Untouched B: the result carries B's channels first, in B's own order, so
    // a plane B physically holds and no row replaces keeps B's value at
    // unchanged coordinates. Bounding by the bound image's plane count (not by
    // the description) keeps a cleared frame's absent planes at numeric zero.
    const int basePlanes = int(inputGeometry[0].channels.x);
    const int untouched = int(channels.x) < basePlanes ? int(channels.x) : basePlanes;
    for (int plane = 0; plane < untouched; ++plane) {
        float value = baseInside ? gpuLoadPlane(in_base, basePixel, plane, baseExtent.y) : 0.0;
        gpuStorePlane(out_color, ivec2(p), plane, planeHeight, value);
    }

    // The constants cover the base image's FORMAT, in full-resolution
    // coordinates: a constant row writes 1 only where the base format claims a
    // pixel, even when the result's data window includes A's overscan.
    const int scale = int(meta2.z);
    ivec2 full = ivec2(meta.z + int(p.x) * scale, meta.w + int(p.y) * scale);
    bool insideFormat = full.x >= 0 && full.y >= 0 && full.x < shuffleFormat.x && full.y < shuffleFormat.y;

    for (int row = 0; row < 8; ++row) {
        const ivec4 entry = shuffleRows[row];
        if (entry.x < 0) { continue; }  // nothing to write
        const int outputPlane = entry.z;
        if (outputPlane < 0 || outputPlane >= int(channels.x)) { continue; }
        float value = 0.0;
        if (entry.x == 0) {  // input B: the exact named channel, or zero when absent
            value = (entry.y >= 0 && baseInside) ? gpuLoadPlane(in_base, basePixel, entry.y, baseExtent.y) : 0.0;
        } else if (entry.x == 1) {  // input A at its own coordinates, or zero
            value =
                (entry.y >= 0 && secondInside) ? gpuLoadPlane(in_second, secondPixel, entry.y, secondExtent.y) : 0.0;
        } else if (entry.x == 3) {  // constant one, defined on the base format
            value = insideFormat ? 1.0 : 0.0;
        }
        // kind 2 (constant zero) needs no read and writes numeric zero.
        gpuStorePlane(out_color, ivec2(p), outputPlane, planeHeight, value);
    }
}
)GLSL";

[[nodiscard]] EffectPassDefinition shufflePass() {
    return EffectPassDefinition{
        .id = "shuffle",
        .shader = "shuffle/shuffle",
        .glsl = nemo::nodes::gpuGlsl(kShuffleGlslPayload, kShuffleGlslBody),
        // Declared port order: B (required base), A (optional).
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}, EffectImageRef{EffectImageKind::Input, 1}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
    };
}

// Worker-side value preparation: the authored mapping becomes plane indices of
// this node's described result and of each input's stored channels. A name the
// chosen input does not carry (or a disconnected A) resolves to -1, which the
// kernel writes as the frozen zero; it never invents a channel.
[[nodiscard]] GpuPreparation prepareShuffle(const GpuNodeContext& context) {
    const ShuffleParameters params = effectiveShuffle(context.catalog, context.node, context.effectiveParams);
    if (context.inputDescriptions.empty() || context.inputDescriptions[0] == nullptr)
        throw std::runtime_error("shuffle requires a connected B input");
    const std::vector<std::string>& outputChannels = context.description.channels;
    const std::vector<std::string>& baseChannels = context.inputDescriptions[0]->channels;
    const ImageDescription* second = context.inputDescriptions.size() > 1 ? context.inputDescriptions[1] : nullptr;

    ShufflePayload payload;
    for (auto& row : payload.rows) {
        row[0] = static_cast<std::int32_t>(ShuffleRowKind::Disabled);
    }
    for (std::size_t index = 0; index < kShuffleRows; ++index) {
        const ShuffleRow& row = params.rows[index];
        if (!row.enabled()) {
            continue;
        }
        const int outputPlane = channelIndex(outputChannels, row.outputChannel);
        if (outputPlane < 0)
            throw std::runtime_error("authored output channel '" + row.outputChannel +
                                     "' is not part of the described result");
        std::int32_t kind = static_cast<std::int32_t>(ShuffleRowKind::Zero);
        int sourcePlane = -1;
        switch (row.source) {
        case ShuffleSource::InputB:
            kind = static_cast<std::int32_t>(ShuffleRowKind::B);
            sourcePlane = channelIndex(baseChannels, row.sourceChannel);
            break;
        case ShuffleSource::InputA:
            kind = static_cast<std::int32_t>(ShuffleRowKind::A);
            sourcePlane = second != nullptr ? channelIndex(second->channels, row.sourceChannel) : -1;
            break;
        case ShuffleSource::Zero:
            break;
        case ShuffleSource::One:
            kind = static_cast<std::int32_t>(ShuffleRowKind::One);
            break;
        }
        payload.rows[index][0] = kind;
        payload.rows[index][1] = sourcePlane;
        payload.rows[index][2] = outputPlane;
    }
    // B's format, which the constants cover: the node's described format is the
    // base input's, resolved by the shared planner.
    payload.format[0] = context.description.format.width;
    payload.format[1] = context.description.format.height;

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution shuffleGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::shuffleContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.shuffle.payload.v1";
    implementation.payloadSize = sizeof(ShufflePayload);
    implementation.passes = {shufflePass()};
    implementation.prepare = &prepareShuffle;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
