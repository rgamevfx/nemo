// Roto: typed animated vector mattes with a native CPU reference (issue #93,
// stories 58-71).
//
// Roto is a DRAW node, not an image filter: the authored shapes are typed
// document data (`core/document/Roto.hpp`), this module owns what they MEAN as
// a matte, and the node writes that matte into one named output channel while
// every other stored channel keeps its incoming value. The shapes can be used as
// a generator (no background connected) or as a mask over a background image,
// and the optional mask input limits the result.
//
// This file owns:
//
//   * the immutable descriptor (ports, parameters, capabilities) and the
//     node-local editor declaration that hosts the shape list on the global
//     Opacity control;
//   * the CPU reference rasterizer: the INDEPENDENT half of the CPU/native pair
//     (ADR-0004). It rasterizes by SCANLINE — one crossing list and one nearest
//     -distance band per contour and row, then a per-sample walk of the flattened
//     program — while the kernels evaluate every sample against every edge. The
//     two therefore share no pixel loop, and their agreement is evidence;
//   * the output description (channels, data window) and the region/bounds
//     consequences of clipping, feather and motion blur.
//
// Frozen matte policy (the reference documents the controls; the arithmetic
// below is Nemo's, recorded here and mirrored — independently — in Gpu.cpp and
// roto.slang):
//
//   * A contour's coverage at a sample is decided by the SIGNED distance `d` to
//     the tessellated contour (positive outside) and the interpolated signed
//     feather `f` there:
//
//         f == 0            coverage = d <= 0 ? 1 : 0
//         f != 0            t = (hi - clamp(d, lo, hi)) / |f|,  lo = min(0, f), hi = max(0, f)
//                           w = profile(t);  coverage = pow(w, 1 / featherFalloff)
//
//     so a POSITIVE feather ramps outward from the contour (0 at the contour + f)
//     and a NEGATIVE one ramps inward (0 at the contour, 1 at the contour + f) —
//     the reference's "positive feather is outward, negative is inward". The
//     `Linear` profile is `t`; `Smooth` is `smoothstep(t)` (3t^2 - 2t^3), the
//     reference's "smooth" falloff centre. `pow` is applied for every finite
//     falloff > 0 and is the identity at 1.
//   * Each element's own inversion and opacity apply to ITS value before it
//     blends: `value = (inverted ? 1 - c : c) * opacity`. A group's children are
//     combined among themselves first, and the group's inversion/opacity then
//     apply to that combined value before it blends with its siblings — the
//     reference's layer model.
//   * Blending is the frozen ordered accumulation: the first CONTRIBUTING
//     sibling (every element that is visible and inside its lifetime contributes,
//     even where its coverage is zero at this sample) seeds the accumulation —
//     with the element's own value for `Combine` and `Intersect`, and with zero
//     for `Subtract`, which starts from nothing to subtract from. Every later
//     sibling combines as `Combine: a + b - a*b`, `Intersect: a * b`,
//     `Subtract: a * (1 - b)`.
//   * The node multiplies the composed matte by its global `opacity`, then — when
//     a mask channel is named AND a mask input is connected — by the clamped,
//     optionally inverted value of that exact channel.
//   * The result is DRAWN into `outputChannel`: `replace` writes the matte
//     exactly, otherwise the matte is drawn OVER that channel's incoming value
//     (`m + (1 - m) * incoming`, the reference's "existing channels are cleared
//     to black before drawing into them" only when `replace` is enabled). The
//     incoming value is the background's stored channel of the same name, or zero
//     when it does not store that name — a channel this node creates (a generator
//     with no background, a target the image does not carry) therefore starts
//     empty rather than at a projection default.
//   * No alpha association is changed anywhere: the matte is written as a value,
//     RGB is never premultiplied or unpremultiplied, and no automatic
//     association conversion is introduced (issue #89 remains the owner of that
//     policy and is REPORTED as an open gap, not worked around here).

#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/roto/Parameters.hpp"
#include "nemo/nodes/roto/RotoGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/document/Roto.hpp"
#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor rotoDescriptor() {
    return NodeDescriptor{.type = "roto",
                          .displayName = "Roto",
                          .group = "Draw",
                          // 1: the first published Roto implementation (typed
                          // document data, CPU reference and native kernels).
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "bg", true}, {PortKind::Mask, "mask", true}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters =
                              {
                                  {.name = "opacity",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{1.0},
                                   .minimum = 0.0,
                                   .maximum = 1.0,
                                   .step = 0.01,
                                   .label = "Opacity",
                                   .section = "Roto",
                                   .editor = "nemo.roto.shapes"},
                                  {.name = "outputChannel",
                                   .type = ParameterType::String,
                                   .defaultValue = ParameterValue{std::string{"A"}},
                                   .label = "Output Channel",
                                   .section = "Output",
                                   .editor = {}},
                                  {.name = "replace",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{false},
                                   .label = "Replace",
                                   .section = "Output",
                                   .editor = {}},
                                  {.name = "clip",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"format"}},
                                   .choices = {"format", "bbox", "union", "intersect", "none"},
                                   .label = "Clip",
                                   .section = "Output",
                                   .editor = {}},
                                  {.name = "shutter",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{0.5},
                                   .minimum = 0.0,
                                   .maximum = 1.0,
                                   .step = 0.01,
                                   .label = "Shutter",
                                   .section = "Motion Blur",
                                   .editor = {}},
                                  {.name = "samples",
                                   .type = ParameterType::Integer,
                                   .defaultValue = ParameterValue{std::int64_t{1}},
                                   .minimum = 1.0,
                                   .maximum = 64.0,
                                   .step = 1.0,
                                   .label = "Samples",
                                   .section = "Motion Blur",
                                   .editor = {}},
                                  {.name = "maskChannel",
                                   .type = ParameterType::String,
                                   .defaultValue = ParameterValue{std::string{"none"}},
                                   .label = "Mask Channel",
                                   .section = "Mask",
                                   .editor = {}},
                                  {.name = "invertMask",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{false},
                                   .label = "Invert Mask",
                                   .section = "Mask",
                                   .editor = {}},
                              },
                          .capabilities = builtinCapabilities(true)};
}

// ---------------------------------------------------------------------------
// Frozen matte arithmetic (see the file header). Written once here; the native
// kernels state the same contract independently.
// ---------------------------------------------------------------------------

// One contour's coverage at signed distance `distance` (positive outside) with
// signed feather `feather` (full-resolution pixels).
[[nodiscard]] float rotoFeatherCoverage(float distance, float feather, RotoFeatherProfile profile, float falloff) {
    if (feather == 0.0F) {
        return distance <= 0.0F ? 1.0F : 0.0F;
    }
    const float width = std::fabs(feather);
    const float lo = std::min(0.0F, feather);
    const float hi = std::max(0.0F, feather);
    const float clamped = std::clamp(distance, lo, hi);
    float weight = (hi - clamped) / width;
    weight = std::clamp(weight, 0.0F, 1.0F);
    if (profile == RotoFeatherProfile::Smooth) {
        weight = weight * weight * (3.0F - 2.0F * weight);
    }
    if (falloff != 1.0F) {
        weight = std::pow(weight, 1.0F / falloff);
    }
    return std::clamp(weight, 0.0F, 1.0F);
}

[[nodiscard]] float rotoFinishValue(float coverage, float opacity, bool inverted) {
    const float shaped = inverted ? 1.0F - coverage : coverage;
    return shaped * opacity;
}

[[nodiscard]] float rotoCombine(float accumulated, bool seeded, float current, RotoBlend blend) {
    if (!seeded) {
        return blend == RotoBlend::Subtract ? 0.0F : current;
    }
    switch (blend) {
    case RotoBlend::Intersect:
        return accumulated * current;
    case RotoBlend::Subtract:
        return accumulated * (1.0F - current);
    case RotoBlend::Combine:
        break;
    }
    return accumulated + current - accumulated * current;
}

// ---------------------------------------------------------------------------
// CPU rasterization (scanline; independent of the kernels).
// ---------------------------------------------------------------------------

[[nodiscard]] float rotoPointSegmentDistance(float px, float py, const RotoVertex& from, const RotoVertex& to,
                                             float* parameter) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float lengthSquared = dx * dx + dy * dy;
    float t = 0.0F;
    if (lengthSquared > 0.0F) {
        t = std::clamp(((px - from.x) * dx + (py - from.y) * dy) / lengthSquared, 0.0F, 1.0F);
    }
    *parameter = t;
    const float closestX = from.x + t * dx;
    const float closestY = from.y + t * dy;
    return std::hypot(px - closestX, py - closestY);
}

// Reusable per-row scratch: the coverage of every leaf shape that reaches the
// row, plus the distance band and crossing list one leaf needs. Reused across
// rows and subframe samples, so a motion-blurred evaluation does not allocate
// per sample.
struct RotoRowScratch {
    std::vector<float> coverage;
    std::vector<float> distance;
    std::vector<float> distanceFeather;
    std::vector<unsigned char> inside;
    std::vector<float> crossings;
    std::vector<int> slot;
    std::vector<const RotoItem*> active;

    void reset(std::size_t itemCount, int width) {
        slot.assign(itemCount, -1);
        active.clear();
        distance.assign(static_cast<std::size_t>(width), std::numeric_limits<float>::infinity());
        distanceFeather.assign(static_cast<std::size_t>(width), 0.0F);
        inside.assign(static_cast<std::size_t>(width), 0);
    }
};

// The world x of one raster column's sample centre: full-resolution coordinates,
// so a matte at Half or Quarter resolution samples the same geometry the Full
// one does.
[[nodiscard]] float rotoSampleCentreX(const EvaluationRequest& request, int scale, int column) {
    return static_cast<float>(request.region.x) + (static_cast<float>(column) + 0.5F) * static_cast<float>(scale);
}

[[nodiscard]] float rotoSampleCentreY(const EvaluationRequest& request, int scale, int row) {
    return static_cast<float>(request.region.y) + (static_cast<float>(row) + 0.5F) * static_cast<float>(scale);
}

// The raster column range whose sample centres lie inside `[minX, maxX]`, or an
// empty range.
struct RotoColumnRange {
    int first{0};
    int last{-1};
};

[[nodiscard]] RotoColumnRange rotoColumnRange(const EvaluationRequest& request, int scale, int width, float minX,
                                              float maxX) {
    const float step = static_cast<float>(scale);
    const float origin = static_cast<float>(request.region.x);
    RotoColumnRange range;
    const float first = std::ceil((minX - origin) / step - 0.5F);
    const float last = std::floor((maxX - origin) / step - 0.5F);
    if (!(last >= first)) {
        return range;
    }
    const float clampedFirst = std::clamp(first, 0.0F, static_cast<float>(width - 1));
    const float clampedLast = std::clamp(last, -1.0F, static_cast<float>(width - 1));
    if (!(clampedLast >= clampedFirst)) {
        return range;
    }
    range.first = static_cast<int>(clampedFirst);
    range.last = static_cast<int>(clampedLast);
    return range;
}

// One leaf shape's coverage across one row: the even-odd fill decides inside from
// outside, and the nearest edge within the feather band supplies the signed
// distance and the interpolated feather the ramp is measured with.
void rasterizeLeafRow(const RotoGeometry& geometry, const RotoItem& item, const EvaluationRequest& request, int scale,
                      int width, float rowY, RotoRowScratch& scratch, float* out) {
    const RotoVertex* vertices = geometry.vertices.data() + item.firstVertex;
    const std::uint32_t count = item.vertexCount;
    std::fill(scratch.distance.begin(), scratch.distance.end(), std::numeric_limits<float>::infinity());
    std::fill(scratch.inside.begin(), scratch.inside.end(), static_cast<unsigned char>(0));

    float maxFeather = 0.0F;
    for (std::uint32_t index = 0; index < count; ++index) {
        maxFeather = std::max(maxFeather, std::fabs(vertices[index].feather));
    }

    // Even-odd crossings of the closed contour through this row: a half-open
    // rule, so a horizontal edge and a vertex exactly on the row are neither
    // double-counted nor missed.
    scratch.crossings.clear();
    for (std::uint32_t index = 0; index < count; ++index) {
        const RotoVertex& from = vertices[index];
        const RotoVertex& to = vertices[(index + 1) % count];
        if ((from.y <= rowY) == (to.y <= rowY)) {
            continue;
        }
        scratch.crossings.push_back(from.x + (rowY - from.y) * (to.x - from.x) / (to.y - from.y));
    }
    std::sort(scratch.crossings.begin(), scratch.crossings.end());
    std::size_t column = 0;
    for (std::size_t crossing = 0; crossing + 1 < scratch.crossings.size(); crossing += 2) {
        const float left = scratch.crossings[crossing];
        const float right = scratch.crossings[crossing + 1];
        while (column < static_cast<std::size_t>(width) &&
               rotoSampleCentreX(request, scale, static_cast<int>(column)) < left) {
            ++column;
        }
        std::size_t end = column;
        while (end < static_cast<std::size_t>(width) &&
               rotoSampleCentreX(request, scale, static_cast<int>(end)) < right) {
            ++end;
        }
        for (std::size_t fill = column; fill < end; ++fill) {
            scratch.inside[fill] = 1;
        }
        column = end;
    }

    // Nearest-edge band: only edges whose own row distance is within the
    // contour's largest feather can move a sample, so a wide feather costs
    // proportional work and a hairline one costs almost nothing.
    for (std::uint32_t index = 0; index < count; ++index) {
        const RotoVertex& from = vertices[index];
        const RotoVertex& to = vertices[(index + 1) % count];
        if (rowY < std::min(from.y, to.y) - maxFeather || rowY > std::max(from.y, to.y) + maxFeather) {
            continue;
        }
        if (from.x == to.x && from.y == to.y) {
            continue;
        }
        const RotoColumnRange range = rotoColumnRange(request, scale, width, std::min(from.x, to.x) - maxFeather,
                                                      std::max(from.x, to.x) + maxFeather);
        for (int col = range.first; col <= range.last; ++col) {
            float parameter = 0.0F;
            const float distance =
                rotoPointSegmentDistance(rotoSampleCentreX(request, scale, col), rowY, from, to, &parameter);
            if (distance < scratch.distance[static_cast<std::size_t>(col)]) {
                scratch.distance[static_cast<std::size_t>(col)] = distance;
                scratch.distanceFeather[static_cast<std::size_t>(col)] =
                    from.feather + parameter * (to.feather - from.feather);
            }
        }
    }

    for (int col = 0; col < width; ++col) {
        const float distance = scratch.distance[static_cast<std::size_t>(col)];
        if (!std::isfinite(distance)) {
            out[col] = scratch.inside[static_cast<std::size_t>(col)] != 0 ? 1.0F : 0.0F;
            continue;
        }
        const float signedDistance = scratch.inside[static_cast<std::size_t>(col)] != 0 ? -distance : distance;
        out[col] = rotoFeatherCoverage(signedDistance, scratch.distanceFeather[static_cast<std::size_t>(col)],
                                       item.profile, item.falloff);
    }
}

// Walk the flattened program for one sample: groups open a nested accumulation,
// a group's own inversion/opacity then apply to its combined value, and every
// shape contributes its own inversion/opacity before blending. The walk is
// allocation-free; the depth bound is enforced when the program is built.
[[nodiscard]] float rotoSampleValue(const RotoGeometry& geometry, const RotoRowScratch& scratch, int width,
                                    int column) {
    struct Frame {
        float accumulated{0.0F};
        float opacity{1.0F};
        RotoBlend blend{RotoBlend::Combine};
        bool seeded{false};
        bool inverted{false};
    };
    std::array<Frame, 2 * kMaxRotoGroupDepth + 2> stack{};
    std::size_t depth = 0;
    for (std::size_t index = 0; index < geometry.items.size(); ++index) {
        const RotoItem& item = geometry.items[index];
        if (item.kind == RotoItem::Kind::Shape) {
            const int slot = scratch.slot[index];
            const float coverage = slot >= 0 ? scratch.coverage[static_cast<std::size_t>(slot) * width + column] : 0.0F;
            const float value = rotoFinishValue(coverage, item.opacity, item.inverted);
            Frame& frame = stack[depth];
            frame.accumulated = rotoCombine(frame.accumulated, frame.seeded, value, item.blend);
            frame.seeded = true;
            continue;
        }
        if (item.kind == RotoItem::Kind::GroupBegin) {
            if (depth + 1 < stack.size()) {
                ++depth;
                Frame& frame = stack[depth];
                frame = Frame{};
                frame.opacity = item.opacity;
                frame.blend = item.blend;
                frame.inverted = item.inverted;
            }
            continue;
        }
        const Frame frame = stack[depth];
        if (depth > 0) {
            --depth;
        }
        const float value = rotoFinishValue(frame.seeded ? frame.accumulated : 0.0F, frame.opacity, frame.inverted);
        Frame& parent = stack[depth];
        parent.accumulated = rotoCombine(parent.accumulated, parent.seeded, value, frame.blend);
        parent.seeded = true;
    }
    return stack[0].seeded ? stack[0].accumulated : 0.0F;
}

// Accumulate one subframe sample's completed hierarchy matte into `matte`, at
// `weight` (1 for a single sample, 1/samples otherwise).
void accumulateRotoSample(const NodeInstance& node, const RotoGeometry& geometry, const EvaluationRequest& request,
                          int scale, int width, int height, float weight, RotoRowScratch& scratch,
                          std::vector<float>& matte) {
    if (geometry.empty()) {
        return;
    }
    scratch.reset(geometry.items.size(), width);
    const bool hasInversion =
        std::any_of(geometry.items.begin(), geometry.items.end(), [](const RotoItem& item) { return item.inverted; });
    for (int row = 0; row < height; ++row) {
        const float rowY = rotoSampleCentreY(request, scale, row);
        scratch.active.clear();
        std::fill(scratch.slot.begin(), scratch.slot.end(), -1);
        for (std::size_t index = 0; index < geometry.items.size(); ++index) {
            const RotoItem& item = geometry.items[index];
            if (item.kind == RotoItem::Kind::Shape && rowY >= item.bounds[1] && rowY <= item.bounds[3]) {
                scratch.slot[index] = static_cast<int>(scratch.active.size());
                scratch.active.push_back(&item);
            }
        }
        if (scratch.active.empty() && !hasInversion) {
            continue;
        }
        const std::size_t active = scratch.active.size();
        if (active > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(width) ||
            active * static_cast<std::size_t>(width) > kMaxRotoRasterScratchSamples) {
            failNode(node, "this request needs " + std::to_string(active) + " shape coverage rows of " +
                               std::to_string(width) + " samples at once, beyond the " +
                               std::to_string(kMaxRotoRasterScratchSamples) +
                               "-sample Roto coverage bound; reduce the request region, the sampling scale or the "
                               "number of overlapping shapes (the request is refused, not partially evaluated)");
        }
        scratch.coverage.assign(active * static_cast<std::size_t>(width), 0.0F);
        for (std::size_t leaf = 0; leaf < active; ++leaf) {
            rasterizeLeafRow(geometry, *scratch.active[leaf], request, scale, width, rowY, scratch,
                             scratch.coverage.data() + leaf * static_cast<std::size_t>(width));
        }
        float* target = matte.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        for (int col = 0; col < width; ++col) {
            target[col] += weight * rotoSampleValue(geometry, scratch, width, col);
        }
    }
}

// The resolved shape snapshot one subframe evaluation rasterizes: the animated
// channels are resolved through the model's own sampler, and a node without a
// network scope (a direct adapter invocation) reads the authored data it holds.
[[nodiscard]] RotoData rotoSnapshotAt(const Document& document, NetworkId network, const NodeInstance& node,
                                      double time, bool hasNetwork) {
    if (hasNetwork) {
        return evaluateRoto(document, network, node.id, time);
    }
    return node.roto != nullptr ? *node.roto : RotoData{};
}

[[nodiscard]] CpuImage executeRoto(const CpuNodeContext& context) {
    const NodeInstance& node = context.node;
    const RotoParameters params = effectiveRoto(context.catalog, node, context.effectiveParams);
    const EvaluationRequest& request = context.request;
    if (!isSamplingScale(request.samplingScale)) {
        failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const int scale = request.samplingScale;
    const float aspect = rotoPhysicalAspect(node, context.owningFormat, context.description.pixelAspect);
    const int width = scaledDimension(request.region.width, scale);
    const int height = scaledDimension(request.region.height, scale);

    CpuImage output(effectRasterLayout(context));
    const std::vector<std::string>& names = output.layout().channels;
    // `describe` produced this layout, so the target is present by construction;
    // a layout that lost it is reported rather than silently writing nothing.
    const int target = channelIndex(names, params.outputChannel);
    if (target < 0) {
        failNode(node, "the described output does not carry the target channel '" + params.outputChannel + "'");
    }
    std::vector<int> backgroundChannel(names.size(), -1);
    const CpuImage* background = optionalImageInput(context, 0);
    InputAnchor backgroundAnchor;
    if (background != nullptr) {
        backgroundAnchor = anchorInput(context, 0, *background);
        for (std::size_t index = 0; index < names.size(); ++index) {
            backgroundChannel[index] = channelIndex(background->layout().channels, names[index]);
        }
    }
    const CpuImage* mask = optionalImageInput(context, 1);
    int maskChannel = -1;
    InputAnchor maskAnchor;
    if (!params.maskChannel.empty() && mask != nullptr) {
        // An exact name that the connected mask does not carry is the frozen
        // zero-fill policy, not an invented channel: the limit is zero.
        maskChannel = channelIndex(mask->layout().channels, params.maskChannel);
        maskAnchor = anchorInput(context, 1, *mask);
    }

    const RotoSampleTimes samples = rotoSampleTimes(params, request.localTime);
    const float weight = 1.0F / static_cast<float>(samples.count);
    const bool hasNetwork = context.network != kInvalidNetwork;
    std::vector<float> matte(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0F);
    RotoRowScratch scratch;
    for (std::size_t sample = 0; sample < samples.count; ++sample) {
        const RotoData snapshot =
            rotoSnapshotAt(context.document, context.network, node, samples.times[sample], hasNetwork);
        const RotoGeometry geometry = buildRotoGeometry(node, snapshot, aspect);
        accumulateRotoSample(node, geometry, request, scale, width, height, weight, scratch, matte);
    }

    const double opacity = static_cast<double>(params.opacity);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            double value =
                static_cast<double>(matte[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + col]) *
                opacity;
            if (mask != nullptr && !params.maskChannel.empty()) {
                const double coverage =
                    maskChannel >= 0 ? std::clamp(static_cast<double>(mask->channel(
                                                      maskAnchor.offsetX + col, maskAnchor.offsetY + row, maskChannel)),
                                                  0.0, 1.0)
                                     : 0.0;
                value *= params.invertMask ? 1.0 - coverage : coverage;
            }
            for (std::size_t index = 0; index < names.size(); ++index) {
                double result = 0.0;
                if (static_cast<int>(index) == target) {
                    const int source = backgroundChannel[index];
                    const double incoming =
                        background != nullptr && source >= 0
                            ? static_cast<double>(background->channel(backgroundAnchor.offsetX + col,
                                                                      backgroundAnchor.offsetY + row, source))
                            : 0.0;
                    result = params.replace ? value : value + (1.0 - value) * incoming;
                } else if (background != nullptr && backgroundChannel[index] >= 0) {
                    result = static_cast<double>(background->channel(
                        backgroundAnchor.offsetX + col, backgroundAnchor.offsetY + row, backgroundChannel[index]));
                }
                output.setChannel(col, row, static_cast<int>(index), static_cast<float>(result));
            }
        }
    }
    return output;
}

// ---------------------------------------------------------------------------
// Output description and bounds.
// ---------------------------------------------------------------------------

// The matte's own extent over every subframe sample: the union of the flattened
// shapes' world boxes, which already carry each contour's feather reach.
[[nodiscard]] bool rotoMatteRegion(const Document& document, NetworkId network, const NodeInstance& node,
                                   const RotoParameters& params, float aspect, std::int64_t localTime,
                                   const Region& canvas, Region& out) {
    const RotoSampleTimes samples = rotoSampleTimes(params, localTime);
    const bool hasNetwork = network != kInvalidNetwork;
    bool any = false;
    bool hasInversion = false;
    float bounds[4]{0.0F, 0.0F, 0.0F, 0.0F};
    for (std::size_t sample = 0; sample < samples.count; ++sample) {
        const RotoData snapshot = rotoSnapshotAt(document, network, node, samples.times[sample], hasNetwork);
        const RotoGeometry geometry = buildRotoGeometry(node, snapshot, aspect);
        hasInversion = hasInversion || std::any_of(geometry.items.begin(), geometry.items.end(),
                                                   [](const RotoItem& item) { return item.inverted; });
        float sampleBounds[4]{0.0F, 0.0F, 0.0F, 0.0F};
        if (!rotoGeometryBounds(geometry, sampleBounds)) {
            continue;
        }
        if (!any) {
            std::copy(sampleBounds, sampleBounds + 4, bounds);
            any = true;
            continue;
        }
        bounds[0] = std::min(bounds[0], sampleBounds[0]);
        bounds[1] = std::min(bounds[1], sampleBounds[1]);
        bounds[2] = std::max(bounds[2], sampleBounds[2]);
        bounds[3] = std::max(bounds[3], sampleBounds[3]);
    }
    if (!any) {
        out = hasInversion ? canvas : Region{};
        return hasInversion;
    }
    // A bound this node's own geometry cannot represent is reported, never
    // truncated into a demand for a different window.
    for (const float value : bounds) {
        if (!std::isfinite(value) || std::abs(value) > static_cast<float>(kMaxDescribedCoordinate)) {
            failNode(node,
                     "the Roto shapes' extent lies outside the representable image coordinates (|coordinate| <= " +
                         std::to_string(kMaxDescribedCoordinate) + ")");
        }
    }
    const int x = static_cast<int>(std::floor(bounds[0]));
    const int y = static_cast<int>(std::floor(bounds[1]));
    out = Region{x, y, static_cast<int>(std::ceil(bounds[2])) - x, static_cast<int>(std::ceil(bounds[3])) - y};
    if (hasInversion)
        out = regionUnion(out, canvas);
    return true;
}

// The described output: everything except the stored channels and the data
// window is inherited from the background (or, for a generator, from the owning
// network's saved canvas), which is the shared default.
//
//   * A GENERATOR produces exactly the named target channel: an alpha-only
//     matte invents neither RGB nor any other stored channel.
//   * A connected node keeps the background's whole stored channel list and
//     APPENDS the target when the background does not carry that name, so a
//     `matte.coverage` output is added without disturbing RGBA.
//   * `clip` restricts the data window to the incoming bbox and/or format
//     exactly as the reference documents it. A generator has no incoming bbox,
//     so `bbox` is the matte's own extent and `union`/`intersect` are taken
//     against the format; `none` never restricts anything, so it keeps the union
//     of format, incoming bbox and the matte's own extent.
[[nodiscard]] ImageDescription describeRoto(const NodeDescriptionContext& context) {
    const RotoParameters params = effectiveRoto(context.catalog, context.node, context.node.params);
    ImageDescription described = context.inherited;
    const ImageDescription* background = context.inputs.empty() ? nullptr : context.inputs[0];
    if (background == nullptr) {
        described.channels = {params.outputChannel};
    } else if (channelIndex(described.channels, params.outputChannel) < 0) {
        described.channels.push_back(params.outputChannel);
    }
    const float aspect = rotoPhysicalAspect(context.node, context.owningFormat, described.pixelAspect);
    Region matte;
    static_cast<void>(rotoMatteRegion(context.document, context.network, context.node, params, aspect,
                                      context.localTime, described.format, matte));
    const Region incoming = background != nullptr ? background->dataBounds : Region{};
    const Region own = background != nullptr ? incoming : matte;
    switch (params.clip) {
    case RotoClip::Format:
        described.dataBounds = described.format;
        break;
    case RotoClip::Bbox:
        described.dataBounds = own;
        break;
    case RotoClip::Union:
        described.dataBounds = regionUnion(described.format, own);
        break;
    case RotoClip::Intersect:
        described.dataBounds = regionIntersection(described.format, own);
        break;
    case RotoClip::None:
        described.dataBounds = regionUnion(regionUnion(described.format, incoming), matte);
        break;
    }
    return described;
}

std::vector<InputRequirement> rotoInputRequirements(const NodeRegionContext& context) {
    const auto params = effectiveRoto(context.catalog, context.node, context.effectiveParams);
    if (params.maskChannel.empty() || context.inputs.size() < 2 || !context.inputs[1] ||
        channelIndex(context.inputs[1]->channels, params.maskChannel) < 0)
        return {};
    std::vector<InputRequirement> requirements(2);
    requirements[1].channels = {params.maskChannel};
    return requirements;
}

std::optional<std::string> validateRotoParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] { static_cast<void>(effectiveRoto(catalog, node, effectiveParams)); });
}

}  // namespace

NodeContribution rotoContribution() {
    NodeContribution contribution;
    contribution.descriptor = rotoDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeRoto};
    contribution.validateParameters = &validateRotoParameters;
    contribution.describe = &describeRoto;
    contribution.inputRequirements = &rotoInputRequirements;
    // Every stored plane is written here: the target channel carries the matte,
    // every other plane keeps the background's value at the same coordinates. The
    // shared auxiliary preservation must therefore not run — it copies a plane
    // from the main input at unchanged coordinates, which is exactly what this
    // node does itself, with the target channel excluded.
    contribution.ownsChannelLayout = true;
    contribution.editors = {NodeEditorContribution{.id = "nemo.roto.shapes",
                                                   .source = "qrc:/qt/qml/Nemo/qml/RotoEditor.qml",
                                                   .consumes = {"opacity"},
                                                   .presentation = "section"}};
    return contribution;
}

}  // namespace nemo::nodes
