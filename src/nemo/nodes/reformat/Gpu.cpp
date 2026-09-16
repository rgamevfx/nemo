// Reformat: host-resolved geometry with alpha-aware resampling and the full
// documented filter inventory (issue #92 node-local GPU module).
//
// Reformat owns its payload (the dispatch words) at set 0 binding 1, its
// host-resolved per-axis sampling table at set 3 binding 0 and its own GLSL
// reference source. The host resolves the parameter interpretation, the
// output<->source map and every axis' sampling WINDOW exactly as the CPU adapter
// does (Parameters.hpp), in double precision, and hands the kernel one entry per
// driven output axis; the kernel never decides which taps exist, so no float
// epsilon can move a Notch or Impulse boundary. The filter kernels and the pixel
// math are this front end's own, and the Slang kernel declares them
// independently (ADR-0004).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/GpuCommon.hpp"
#include "nemo/nodes/reformat/Parameters.hpp"

namespace nemo::eval::nodes {
namespace {

// Node-local payload: the resolved geometry the kernel dispatches over. Every
// word is host-computed shared mapping math; the kernel never evaluates a
// placement rule, a canvas rule, a rounding rule or a sampling window itself.
struct ReformatPayload {
    float kernel[4]{};         // (separable kernel minification widening x, y, 0, 0)
    std::int32_t flags[4]{};   // (turn, filter mode, source-Y table word offset, 0)
    std::int32_t window[4]{};  // (input data window low/high raster index x, low/high y)
    std::int32_t extras[4]{};  // (clamp, blackOutside, solidAlpha, premultiply)
};
static_assert(sizeof(ReformatPayload) % 16 == 0);

constexpr const char* kReformatGlslPayload = R"GLSL(
layout(std140, set = 0, binding = 1) uniform ReformatPayload {
    // (separable kernel minification widening x, y, 0, 0)
    vec4 kernel;
    // (turn, filter mode, source-Y table word offset, 0)
    ivec4 flags;
    // (input data window low/high raster index x, low/high y; the edge extension's
    // clamp range, unused while black outside is on)
    ivec4 window;
    // (clamp, blackOutside, solidAlpha, premultiply)
    ivec4 extras;
};
)GLSL";

constexpr const char* kReformatGlslBody = R"GLSL(
layout(set = 1, binding = 0) restrict readonly uniform image2D in_main;
layout(set = 2, binding = 0) restrict writeonly uniform image2D out_color;

// Host-resolved geometry: first/last/base each use two exact 16-bit numeric
// limbs, followed by nearest/fraction/inside. Every transport float is finite,
// and every signed 32-bit index survives without a float precision limit.
layout(std430, set = 3, binding = 0) readonly buffer ReformatAxes { float reformatAxes[]; };

const int reformatAxisWords = 9;

int reformatAxisInteger(uint word) {
    return int(uint(reformatAxes[word]) | (uint(reformatAxes[word + 1u]) << 16u));
}

// One axis' host-resolved sampling window: `first`..`last` are the taps the host
// enumerated CLOSED (its own double-precision guard included), `nearest` is the
// single host-selected tap (Impulse, or a degenerate window), `fraction` and
// `base` carry the tap distance exactly -- `fraction + (base - index) - 0.5`,
// never two large rounded coordinates subtracted -- and `inside` is the host's
// solid-alpha test.
struct ReformatAxis {
    int first;
    int last;
    int base;
    float fraction;
    bool nearest;
    bool inside;
};

ReformatAxis reformatAxisAt(uint word) {
    ReformatAxis axis;
    axis.first = reformatAxisInteger(word);
    axis.last = reformatAxisInteger(word + 2u);
    axis.base = reformatAxisInteger(word + 4u);
    axis.nearest = reformatAxes[word + 6u] != 0.0;
    axis.fraction = reformatAxes[word + 7u];
    axis.inside = reformatAxes[word + 8u] != 0.0;
    return axis;
}

float reformatSinc(float t) {
    if (t == 0.0) { return 1.0; }
    const float scaled = 3.14159265358979323846 * t;
    return sin(scaled) / scaled;
}

// The Mitchell-Netravali BC family: Cubic (0,0), Keys (0,.5), Simon (0,.75),
// Rifman (0,1), Mitchell (1/3,1/3) and Parzen (1,0).
float reformatBc(float b, float c, float distance) {
    const float x = abs(distance);
    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
    }
    if (x < 2.0) {
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x + (-12.0 * b - 48.0 * c) * x +
                (8.0 * b + 24.0 * c)) / 6.0;
    }
    return 0.0;
}

float reformatKernel(int mode, float distance) {
    const float x = abs(distance);
    if (mode == 0) { return x < 0.5 ? 1.0 : 0.0; }                                  // Impulse
    if (mode == 1) { return reformatBc(0.0, 0.0, x); }                              // Cubic
    if (mode == 2) { return reformatBc(0.0, 0.5, x); }                              // Keys
    if (mode == 3) { return reformatBc(0.0, 0.75, x); }                             // Simon
    if (mode == 4) { return reformatBc(0.0, 1.0, x); }                              // Rifman
    if (mode == 5) { return reformatBc(1.0 / 3.0, 1.0 / 3.0, x); }                  // Mitchell
    if (mode == 6) { return reformatBc(1.0, 0.0, x); }                              // Parzen
    // mode 7 (Notch) has no interior shape: its weights are the closed support
    // window itself, evaluated by reformatTapWeight in the window's own units.
    if (mode == 8) { return x < 2.0 ? reformatSinc(x) * reformatSinc(x * 0.5) : 0.0; }
    if (mode == 9) { return x < 3.0 ? reformatSinc(x) * reformatSinc(x / 3.0) : 0.0; }
    if (mode == 10) { return x < 2.0 ? reformatSinc(x) : 0.0; }
    return 0.0;
}

// One ENUMERATED tap's weight, from the host's window: the unit-width box's
// weights ARE that window (the host enumerated it, and this front end normalizes
// them), and every continuous kernel keeps its own declared shape, evaluated on
// the host's exact tap distance divided by the same minification widening the
// window used. Nothing here decides membership.
float reformatTapWeight(int mode, float widen, ReformatAxis axis, int index) {
    if (mode == 7) { return 1.0; }
    return reformatKernel(mode, (axis.fraction + float(axis.base - index) - 0.5) / widen);
}

// One axis' enumerated taps and the normalization their weights share. `nearest`
// is the single-tap case: the host selected it (Impulse, or a degenerate window),
// or the enumerated window fell entirely on kernel zeros, which falls back to the
// host's base exactly as the CPU reference's own fallback does.
struct ReformatNormalizer {
    int first;
    int last;
    bool nearest;
    float scale;
};

ReformatNormalizer reformatNormalize(int mode, float widen, ReformatAxis axis) {
    ReformatNormalizer normalizer;
    normalizer.first = axis.first;
    normalizer.last = axis.last;
    normalizer.nearest = axis.nearest;
    normalizer.scale = 1.0;
    if (axis.nearest) { return normalizer; }
    float total = 0.0;
    for (int index = axis.first; index <= axis.last; ++index) {
        total += reformatTapWeight(mode, widen, axis, index);
    }
    if (total == 0.0) {
        normalizer.first = axis.base;
        normalizer.last = axis.base;
        normalizer.nearest = true;
        return normalizer;
    }
    normalizer.scale = 1.0 / total;
    return normalizer;
}

// The raster sample a tap resolves to, or -1: with black outside on a tap outside
// the raster is transparent black, and with it off every tap is clamped into the
// input's data window first (the documented outermost-pixels extension).
int reformatTapIndex(int tap, bool blackOutside, int low, int high, int extent) {
    int index = tap;
    if (!blackOutside) { index = clamp(index, low, high); }
    if (blackOutside && (index < low || index > high)) { return -1; }
    return (index >= 0 && index < extent) ? index : -1;
}

// One named plane resampled numerically and independently of the primary RGBA
// (issue #90). Auxiliary planes are rare, but the tap geometry is resolved ONCE
// from the host's table and shared with the primary path, so no plane ever
// re-decides a window this front end already resolved.
float reformatNumericPlane(int channel, ReformatAxis axisX, ReformatNormalizer tapsX, ReformatAxis axisY,
                           ReformatNormalizer tapsY, vec2 widen, int mode, bool blackOutside, bool doClamp) {
    const ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    const int planeHeight = int(inputGeometry[0].extent.y);
    const uint components = inputGeometry[0].channels.y;
    float accumulated = 0.0;
    float lowest = 1e30;
    float highest = -1e30;
    bool contributed = false;
    for (int j = tapsY.first; j <= tapsY.last; ++j) {
        const int ty = reformatTapIndex(j, blackOutside, window.z, window.w, extent.y);
        if (ty < 0) { continue; }
        const float wy = tapsY.nearest ? 1.0 : reformatTapWeight(mode, widen.y, axisY, j) * tapsY.scale;
        if (wy == 0.0) { continue; }
        for (int i = tapsX.first; i <= tapsX.last; ++i) {
            const int tx = reformatTapIndex(i, blackOutside, window.x, window.y, extent.x);
            if (tx < 0) { continue; }
            const float wx = tapsX.nearest ? 1.0 : reformatTapWeight(mode, widen.x, axisX, i) * tapsX.scale;
            const float weight = wx * wy;
            if (weight == 0.0) { continue; }
            const float value = gpuLoadChannel(in_main, ivec2(tx, ty), channel, planeHeight, components);
            accumulated += weight * value;
            lowest = min(lowest, value);
            highest = max(highest, value);
            contributed = true;
        }
    }
    if (doClamp && contributed) { accumulated = clamp(accumulated, lowest, highest); }
    return accumulated;
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= meta2.x || p.y >= meta2.y) { return; }
    // A sample outside the described data support is transparent black; an
    // edge-extended output declares no support and is written in full.
    if (!gpuHasData(ivec2(p))) {
        gpuZeroPlanes(out_color, ivec2(p), int(meta2.y), channels.y, channels.x);
        return;
    }

    const int mode = flags.y;
    const bool blackOutside = extras.y > 0;
    const bool doClamp = extras.x > 0;
    const bool solidAlpha = extras.z > 0;
    const bool premultiply = extras.w > 0;
    const ivec2 extent = ivec2(inputGeometry[0].extent.xy);
    const int planeHeight = int(inputGeometry[0].extent.y);
    const uint components = inputGeometry[0].channels.y;
    const vec2 widen = vec2(kernel.x, kernel.y);

    // The host's table is indexed by the OUTPUT axis that actually drives each
    // source axis: a 90-degree turn CROSSES them, so the source-X entry belongs to
    // the output row and the source-Y entry to the output column (the CPU
    // reference's driving-axis loop). Both this pixel's windows and the solid
    // alpha's inside test come from those two entries, never from a float
    // re-evaluation of the map.
    const bool crosses = flags.x > 0;
    const uint wordX = uint(crosses ? p.y : p.x) * uint(reformatAxisWords);
    const uint wordY = uint(flags.z) + uint(crosses ? p.x : p.y) * uint(reformatAxisWords);
    const ReformatAxis axisX = reformatAxisAt(wordX);
    const ReformatAxis axisY = reformatAxisAt(wordY);
    const ReformatNormalizer tapsX = reformatNormalize(mode, widen.x, axisX);
    const ReformatNormalizer tapsY = reformatNormalize(mode, widen.y, axisY);

    // The primary RGBA: association-aware (a straight input is premultiplied for
    // the kernel), with the contributing samples' extrema kept for `clamp`.
    vec4 accumulated = vec4(0.0);
    vec4 lowest = vec4(1e30);
    vec4 highest = vec4(-1e30);
    bool contributed = false;
    for (int j = tapsY.first; j <= tapsY.last; ++j) {
        const int ty = reformatTapIndex(j, blackOutside, window.z, window.w, extent.y);
        if (ty < 0) { continue; }
        const float wy = tapsY.nearest ? 1.0 : reformatTapWeight(mode, widen.y, axisY, j) * tapsY.scale;
        if (wy == 0.0) { continue; }
        for (int i = tapsX.first; i <= tapsX.last; ++i) {
            const int tx = reformatTapIndex(i, blackOutside, window.x, window.y, extent.x);
            if (tx < 0) { continue; }
            const float wx = tapsX.nearest ? 1.0 : reformatTapWeight(mode, widen.x, axisX, i) * tapsX.scale;
            const float weight = wx * wy;
            if (weight == 0.0) { continue; }
            vec4 tapValue = gpuLoadRgba(in_main, ivec2(tx, ty), inputGeometry[0].rgba, planeHeight, components);
            if (premultiply) { tapValue = vec4(tapValue.rgb * tapValue.a, tapValue.a); }
            accumulated += weight * tapValue;
            lowest = min(lowest, tapValue);
            highest = max(highest, tapValue);
            contributed = true;
        }
    }
    if (doClamp && contributed) { accumulated = clamp(accumulated, lowest, highest); }
    vec4 filtered = accumulated;
    if (premultiply) {
        filtered = accumulated.a != 0.0 ? vec4(accumulated.rgb / accumulated.a, accumulated.a) : vec4(0.0);
    }
    if (solidAlpha) {
        // The documented "solid alpha over the input image area", decided by the
        // host in source pixel coordinates -- exactly the test the CPU reference
        // makes, and on the same driving axes.
        filtered.a = (axisX.inside && axisY.inside) ? 1.0 : 0.0;
    }

    // Every stored channel of the produced raster: the primary roles from the
    // filtered RGBA, each named auxiliary plane from its own numeric resample,
    // and anything the input does not carry as zero.
    const int inputChannels = int(inputGeometry[0].channels.x);
    vec4 pixel = vec4(0.0);
    for (uint channel = 0u; channel < channels.x; ++channel) {
        int role = -1;
        if (rgba.x == int(channel)) { role = 0; }
        else if (rgba.y == int(channel)) { role = 1; }
        else if (rgba.z == int(channel)) { role = 2; }
        else if (rgba.w == int(channel)) { role = 3; }
        float value = 0.0;
        if (role >= 0) {
            value = filtered[role];
        } else if (int(channel) < inputChannels) {
            value = reformatNumericPlane(int(channel), axisX, tapsX, axisY, tapsY, widen, mode, blackOutside, doClamp);
        }
        gpuPixelChannel(pixel, out_color, ivec2(p), int(meta2.y), channels.y, int(channel), value);
    }
    gpuPixelStore(out_color, ivec2(p), channels.y, pixel);
}
)GLSL";

[[nodiscard]] EffectPassDefinition reformatPass() {
    return EffectPassDefinition{
        .id = "reformat",
        .shader = "reformat/reformat",
        .glsl = nemo::nodes::gpuGlsl(kReformatGlslPayload, kReformatGlslBody),
        .inputs = {EffectImageRef{EffectImageKind::Input, 0}},
        .output = EffectImageRef{EffectImageKind::Output, 0},
        // The host-resolved per-axis sampling table (set 3 binding 0).
        .weights = true,
    };
}

// The raster-index range (inclusive) of one axis' data window in the bound input
// raster, or a degenerate range no tap can satisfy. The producer's raster always
// covers the whole data window (the planner clips every demand to the producer's
// own domain), so the range needs no raster extent of its own.
void windowRange(double first, double last, std::int32_t& low, std::int32_t& high) {
    low = static_cast<std::int32_t>(std::max(0.0, std::ceil(first)));
    high = static_cast<std::int32_t>(std::min(static_cast<double>(kMaxDescribedCoordinate), std::ceil(last) - 1.0));
    if (low > high) {
        low = -1;
        high = -1;
    }
}

// Two exact 16-bit limbs per signed index, then nearest/fraction/inside.
// The shared executor intentionally accepts only finite float table entries.
constexpr int kAxisWords = 9;

void appendAxisInteger(std::vector<float>& table, int value) {
    const auto bits = std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value));
    table.push_back(static_cast<float>(bits & 0xffffu));
    table.push_back(static_cast<float>(bits >> 16u));
}

// One driven output axis' resolved entry: the window the shared host-double rule
// decides (reformatSampleWindow) plus this front end's own solid-alpha test,
// computed exactly as the CPU reference computes both.
void appendAxisEntry(std::vector<float>& table, const nemo::nodes::ReformatGeometry& geometry, double widen,
                     double center, bool inside, const NodeInstance& node) {
    const nemo::nodes::ReformatSampleWindow window =
        nemo::nodes::reformatSampleWindow(geometry.filter, geometry.radius, widen, center, node);
    appendAxisInteger(table, window.first);
    appendAxisInteger(table, window.last);
    appendAxisInteger(table, window.base);
    table.push_back(window.nearest ? 1.0F : 0.0F);
    table.push_back(static_cast<float>(window.fraction));
    table.push_back(inside ? 1.0F : 0.0F);
}

// Worker-side value preparation: the shared typed Reformat metadata and the
// planner's resolved output description become this node's payload and its
// per-axis sampling table. Every placement word and every sampling window is
// computed here, once per evaluation, so no dispatch evaluates a canvas, rounding,
// alignment or window rule behind the shared description's back.
[[nodiscard]] GpuPreparation prepareReformat(const GpuNodeContext& context) {
    const nemo::nodes::ReformatParameters params =
        nemo::nodes::effectiveReformat(context.catalog, context.node, context.effectiveParams);
    if (context.inputDescriptions.empty() || context.inputDescriptions.front() == nullptr) {
        throw std::runtime_error("reformat requires a described main image input");
    }
    const ImageDescription& input = *context.inputDescriptions.front();
    const bool sourceHasAlpha = rgbaChannelIndices(input.channels)[3] >= 0;
    const nemo::nodes::ReformatGeometry geometry = nemo::nodes::resolveReformatGeometry(
        params,
        nemo::nodes::ReformatCanvas{context.description.format.width, context.description.format.height,
                                    context.description.pixelAspect},
        nemo::nodes::ReformatCanvas{input.format.width, input.format.height, input.pixelAspect}, input.dataBounds,
        sourceHasAlpha, context.node);
    const int scale = context.request.samplingScale;
    if (!isSamplingScale(scale)) {
        throw std::runtime_error("reformat at sampling scale " + std::to_string(scale) +
                                 " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const EvaluationRequest& source = context.request;
    const auto bound = [&](std::size_t port) -> const EvaluationRequest& {
        return port < context.inputRequests.size() ? context.inputRequests[port] : source;
    };
    const EvaluationRequest& inputRequest = bound(0);

    // The native geometry table: one host-resolved window per DRIVEN output axis,
    // exactly the CPU reference's per-axis loop -- a turn CROSSES the axes, so the
    // source-X table is then indexed by the output ROW -- evaluated on this
    // request's own lattice (its region origin and sampling scale) against the
    // raster the executor actually bound for the main input.
    const double sampling = static_cast<double>(scale);
    const bool crosses = geometry.turn;
    const int rasterWidth = scaledDimension(source.region.width, scale);
    const int rasterHeight = scaledDimension(source.region.height, scale);
    const int driveX = crosses ? rasterHeight : rasterWidth;
    const int driveY = crosses ? rasterWidth : rasterHeight;
    const double regionX = static_cast<double>(source.region.x);
    const double regionY = static_cast<double>(source.region.y);
    const double shiftX = static_cast<double>(inputRequest.region.x) / sampling;
    const double shiftY = static_cast<double>(inputRequest.region.y) / sampling;
    const Region& bounds = geometry.sourceBounds;
    const double insideLeft = static_cast<double>(bounds.x);
    const double insideRight = static_cast<double>(bounds.x + bounds.width);
    const double insideTop = static_cast<double>(bounds.y);
    const double insideBottom = static_cast<double>(bounds.y + bounds.height);
    std::vector<float> table;
    table.reserve(static_cast<std::size_t>(kAxisWords) * static_cast<std::size_t>(driveX + driveY));
    for (int index = 0; index < driveX; ++index) {
        // The nominal coordinate on the other output axis is irrelevant: the map
        // resolves each source coordinate from exactly one output axis.
        const double outputX = regionX + (crosses ? 0.5 : (static_cast<double>(index) + 0.5) * sampling);
        const double outputY = regionY + (crosses ? (static_cast<double>(index) + 0.5) * sampling : 0.5);
        double sourceX = 0.0;
        double sourceY = 0.0;
        nemo::nodes::reformatMapToSource(geometry, outputX, outputY, sourceX, sourceY);
        appendAxisEntry(table, geometry, geometry.widenX, sourceX / sampling - shiftX,
                        sourceX >= insideLeft && sourceX < insideRight, context.node);
    }
    for (int index = 0; index < driveY; ++index) {
        const double outputX = regionX + (crosses ? (static_cast<double>(index) + 0.5) * sampling : 0.5);
        const double outputY = regionY + (crosses ? 0.5 : (static_cast<double>(index) + 0.5) * sampling);
        double sourceX = 0.0;
        double sourceY = 0.0;
        nemo::nodes::reformatMapToSource(geometry, outputX, outputY, sourceX, sourceY);
        appendAxisEntry(table, geometry, geometry.widenY, sourceY / sampling - shiftY,
                        sourceY >= insideTop && sourceY < insideBottom, context.node);
    }

    if (table.empty()) {
        // A zero-area request (an empty consumer demand) dispatches no sample, so
        // the table is never read; the pass still declares its weight binding, and
        // one unused entry keeps the preparation consistent with it -- the same
        // placeholder Blur's identity pass uses.
        table.assign(static_cast<std::size_t>(kAxisWords), 0.0F);
    }

    ReformatPayload payload;
    payload.kernel[0] = static_cast<float>(geometry.widenX);
    payload.kernel[1] = static_cast<float>(geometry.widenY);
    payload.flags[0] = crosses ? 1 : 0;
    payload.flags[1] = static_cast<std::int32_t>(geometry.filter);
    payload.flags[2] = kAxisWords * driveX;  // where the source-Y table starts
    // The input raster may hold a different rectangle of the same lattice, so the
    // edge-clamp window is stated in that raster's own index space.
    windowRange((static_cast<double>(geometry.sourceBounds.x) - inputRequest.region.x) / sampling,
                (static_cast<double>(geometry.sourceBounds.x + geometry.sourceBounds.width) - inputRequest.region.x) /
                    sampling,
                payload.window[0], payload.window[1]);
    windowRange((static_cast<double>(geometry.sourceBounds.y) - inputRequest.region.y) / sampling,
                (static_cast<double>(geometry.sourceBounds.y + geometry.sourceBounds.height) - inputRequest.region.y) /
                    sampling,
                payload.window[2], payload.window[3]);
    payload.extras[0] = geometry.clamp ? 1 : 0;
    payload.extras[1] = geometry.blackOutside ? 1 : 0;
    payload.extras[2] = geometry.solidAlpha ? 1 : 0;
    payload.extras[3] = (sourceHasAlpha && input.association == ImageAssociation::Straight) ? 1 : 0;

    GpuPreparation preparation;
    preparation.payload = effectPayload(payload);
    preparation.weights = std::move(table);
    preparation.passes = {0u};
    return preparation;
}

}  // namespace

GpuNodeContribution reformatGpuContribution() {
    GpuNodeContribution contribution;
    contribution.node = nemo::nodes::reformatContribution();
    GpuImplementation implementation;
    implementation.version = contribution.node.descriptor.implementationVersion;
    implementation.payloadLayout = "nemo.nodes.reformat.payload.v2";
    implementation.payloadSize = sizeof(ReformatPayload);
    implementation.passes = {reformatPass()};
    implementation.prepare = &prepareReformat;
    contribution.gpu = std::move(implementation);
    return contribution;
}

}  // namespace nemo::eval::nodes
