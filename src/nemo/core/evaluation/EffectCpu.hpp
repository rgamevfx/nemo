#pragma once

// Shared CPU-side effect conventions (issue #83).
//
// Node-local pixel implementations live with their contribution
// (src/nemo/nodes/<slug>/Contribution.cpp) and stay independent of the GPU
// shaders (ADR-0004). Only conventions genuinely shared by several CPU effects
// remain here: the raster an effect produces, declared-port input access, the
// alpha-aware sampling helpers the filtering effects share, and the single
// optional-mask/Mix application every effect ends with.

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

// Raster an effect produces for `request`: the requested region at the request's
// sampling scale, interpreted exactly as the resolved image description says
// (issue #88). Pixel aspect, named channels, precision and color interpretation
// travel with the description, so an effect over a Data or DisplayReferred input
// produces a raster that says what it is instead of resetting the result to
// scene-linear.
[[nodiscard]] inline ImageLayout effectRasterLayout(const EvaluationRequest& request,
                                                    const ImageDescription& description) {
    return imageLayoutOf(description, scaledDimension(request.region.width, request.samplingScale),
                         scaledDimension(request.region.height, request.samplingScale));
}

// The node's own output raster (issue #88): the geometry of the request at the
// request's sampling scale, interpreted as the planner's resolved description
// for this node says. A generator's description is its owning network's authored
// canvas format, an ordinary effect's the main input's meaning it inherits, so
// no adapter rediscovers either one.
[[nodiscard]] inline ImageLayout effectRasterLayout(const CpuNodeContext& context) {
    return effectRasterLayout(context.request, context.description);
}

// Declared-port input access. An absent optional slot is null, never a
// manufactured source; a required slot that is missing fails naming the node.
[[nodiscard]] inline const CpuImage& requiredImageInput(const CpuNodeContext& context, std::size_t port,
                                                        const char* requirement) {
    if (port >= context.inputs.size() || context.inputs[port] == nullptr)
        failNode(context.node, requirement);
    return *context.inputs[port];
}

[[nodiscard]] inline const CpuImage* optionalImageInput(const CpuNodeContext& context, std::size_t port) {
    return port < context.inputs.size() ? context.inputs[port] : nullptr;
}

// The coverage that anchored declared input `port`'s raster (issue #85): the
// region the executor actually produced it for, which may be larger than this
// node's own request (a halo, a whole-domain escalation, a resident cache
// rectangle). An executor that reports no per-port geometry — a direct adapter
// invocation with a single raster — falls back to the node's own request, which
// is exactly the previous single-raster contract.
[[nodiscard]] inline const EvaluationRequest& inputRequest(const CpuNodeContext& context, std::size_t port) {
    return port < context.inputRequests.size() ? context.inputRequests[port] : context.request;
}

// Where one raster sits inside another (issue #85). Both requests are anchored
// to the same sampling lattice, so the difference of their origins is a whole
// number of samples.
struct InputAnchor {
    int offsetX{0};
    int offsetY{0};
};

[[nodiscard]] inline InputAnchor anchorBetween(const EvaluationRequest& anchor, const EvaluationRequest& target) {
    const int scale = isSamplingScale(target.samplingScale) ? target.samplingScale : 1;
    return InputAnchor{(target.region.x - anchor.region.x) / scale, (target.region.y - anchor.region.y) / scale};
}

// The offset of `target`'s raster inside the raster `image` was produced for as
// `anchor`, or a failure: an image that does not cover what the target needs is
// a real error, never a silently treated image border.
[[nodiscard]] inline InputAnchor requiredAnchor(const CpuImage& image, const EvaluationRequest& anchor,
                                                const EvaluationRequest& target) {
    const InputAnchor offset = anchorBetween(anchor, target);
    const int width = scaledDimension(target.region.width, target.samplingScale);
    const int height = scaledDimension(target.region.height, target.samplingScale);
    if (offset.offsetX < 0 || offset.offsetY < 0 || offset.offsetX + width > image.width() ||
        offset.offsetY + height > image.height()) {
        throw EvaluationException("raster " + std::to_string(image.width()) + "x" + std::to_string(image.height()) +
                                  " at origin (" + std::to_string(anchor.region.x) + "," +
                                  std::to_string(anchor.region.y) + ") does not cover " + std::to_string(width) + "x" +
                                  std::to_string(height) + " at origin (" + std::to_string(target.region.x) + "," +
                                  std::to_string(target.region.y) + ")");
    }
    return offset;
}

// The offset of this node's output raster inside the raster that anchored
// declared input `port` (issue #85). The only invariant is that the producer
// delivered the raster it was asked for: the offset can be anything, because
// both rasters sit on the same lattice, and everything the node reads outside
// the delivered raster is transparent black (issue #88).
//
// Nothing about this node's own coordinates or the producer's described image
// is an error: a request may reach past what a producer holds — a mixed-format
// merge, a transform whose inverse map leaves the frame, a whole-domain
// escalation, an image with an empty data window — and every such read is
// transparent black, exactly as the whole-image reference has it. A raster
// with no pixels is the empty case of the same rule, and an adapter invoked
// without per-port geometry (a direct call with a single raster) reads at its
// own coordinates as it always did.
[[nodiscard]] inline InputAnchor anchorInput(const CpuNodeContext& context, std::size_t port, const CpuImage& image) {
    const EvaluationRequest& request = context.request;
    const EvaluationRequest& source = inputRequest(context, port);
    const InputAnchor anchor = anchorBetween(source, request);
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    if (source.samplingScale != scale) {
        failNode(context.node, "input raster at sampling scale " + std::to_string(source.samplingScale) +
                                   " cannot be read at this node's scale " + std::to_string(scale));
    }
    if (image.width() <= 0 || image.height() <= 0) {
        return anchor;
    }
    const int demandedWidth = scaledDimension(source.region.width, scale);
    const int demandedHeight = scaledDimension(source.region.height, scale);
    if (image.width() < demandedWidth || image.height() < demandedHeight) {
        failNode(context.node, "input raster " + std::to_string(image.width()) + "x" + std::to_string(image.height()) +
                                   " does not cover the demand placed on this port (" + std::to_string(demandedWidth) +
                                   "x" + std::to_string(demandedHeight) + " at origin (" +
                                   std::to_string(source.region.x) + "," + std::to_string(source.region.y) + "))");
    }
    return anchor;
}

// The domain a producer can actually answer a demand from: its own described
// image, `format ∪ dataBounds` — the exact rectangle its pixels may exist in
// (issue #88). A demand past that boundary needs no pixels at all: that is
// outside the producer's data, and every read of this node returns transparent
// black there. Clipping a node's requirement to it is what keeps a clamp-to-edge
// filter clamped at the real image border, keeps a rotated inverse map from
// demanding coordinates that cannot exist, and keeps a demand bounded by the
// producer's own geometry instead of this node's arithmetic.
//
// An input whose description carries no format at all describes no geometry, so
// the caller's own bounded fallback is used: the producer then serves whatever
// raster its own policy defines (transparent black for an image with no data).
//
// An input that EXTENDS its edge (issue #92) is never clipped: its retained
// domain is not the boundary of what it can answer, so clipping a node's own
// read to it would demand less than the node's math needs and silently replace
// real extended samples with transparent black. The caller's own bounded
// fallback stands instead — a node still bounds its own read, the producer
// decides what lies outside its retained domain.
[[nodiscard]] inline Region requirementDomain(const NodeRegionContext& context, std::size_t port,
                                              const Region& fallback) {
    if (port >= context.inputs.size() || context.inputs[port] == nullptr) {
        return fallback;
    }
    const ImageDescription& described = *context.inputs[port];
    if (hasEdgeExtension(described)) {
        return fallback;
    }
    const Region domain = regionUnion(described.format, described.dataBounds);
    return domain.width > 0 && domain.height > 0 ? domain : fallback;
}

// Straight (non-premultiplied) sample of `image` at raster coordinates, with
// transparent black for anything the raster does not hold (issue #88). A
// producer's raster carries the coverage it was asked for, which is not
// necessarily every sample a consumer would like: a node whose own demand
// reaches past it — a shifted or otherwise inverse-mapped region, a mixed-format
// neighbor, an image with an empty data window — reads outside data as
// transparent black, exactly as the whole-image reference does, instead of
// reading out of bounds or inventing a border pixel.
[[nodiscard]] inline std::array<float, kImageChannels> sampledPixel(const CpuImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    return image.pixel(x, y);
}

// This node's own raster of an input, copied out of that input's raster (which
// may be larger). Only used where the output is honestly a window of an input,
// so the copy is the result rather than a needless intermediate. Every stored
// channel is copied by NAME (issue #90), so an auxiliary or data layer survives
// delivery instead of being projected away; a produced channel the input does
// not carry stays zero, and a sample the input does not hold is zero (issue
// #88: the window may reach past an input whose own data ends earlier).
[[nodiscard]] inline CpuImage windowOf(const CpuImage& image, const InputAnchor& anchor, ImageLayout layout) {
    CpuImage window(std::move(layout));
    const std::vector<std::string>& channels = window.layout().channels;
    std::vector<int> sourceIndex(channels.size(), -1);
    for (std::size_t index = 0; index < channels.size(); ++index) {
        sourceIndex[index] = channelIndex(image.layout().channels, channels[index]);
    }
    for (int y = 0; y < window.height(); ++y) {
        for (int x = 0; x < window.width(); ++x) {
            for (std::size_t index = 0; index < channels.size(); ++index) {
                const int source = sourceIndex[index];
                if (source < 0) {
                    continue;
                }
                window.setChannel(x, y, static_cast<int>(index),
                                  image.channel(anchor.offsetX + x, anchor.offsetY + y, source));
            }
        }
    }
    return window;
}

// Filtering and spatial interpolation are alpha-aware: samples are read as
// premultiplied RGB so transparent neighbors cannot contribute color.
[[nodiscard]] inline std::array<float, kImageChannels> premultipliedPixel(const CpuImage& image, int x, int y) {
    const std::array<float, kImageChannels> pixel = sampledPixel(image, x, y);
    return {pixel[0] * pixel[3], pixel[1] * pixel[3], pixel[2] * pixel[3], pixel[3]};
}

// Straight (non-premultiplied) output: only an exactly zero alpha forces zero
// RGB; any nonzero alpha (including negative values from cubic ringing or a
// source) divides through without an implicit clamp.
[[nodiscard]] inline std::array<float, kImageChannels>
straightPixel(const std::array<float, kImageChannels>& premultiplied) {
    const float alpha = premultiplied[3];
    if (alpha == 0.0F) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    return {premultiplied[0] / alpha, premultiplied[1] / alpha, premultiplied[2] / alpha, alpha};
}

// One final blend at output coordinates: original*(1-weight) + processed*weight
// with weight = coverage*mix. An absent mask (or maskChannel none) has full
// coverage independent of inversion; a connected mask contributes its clamped
// selected stored channel, optionally inverted. The effect result is blended in
// place, so an evaluation holds one output buffer (plus the blur scratch while
// it runs) rather than a second full raster.
//
// Both the original and the mask are read through their own anchors (issue
// #85): they may be larger than this node's raster, so the blended samples are
// the node's own region, taken from each input's real origin. Neither input is
// required to hold every sample of that region — an empty or short data window
// contributes transparent black (issue #88), never a border pixel or an
// out-of-bounds read. `original` is the node's main image input (declared port
// 0) and `maskPort` is the declared port its optional mask lives on.
[[nodiscard]] inline CpuImage blendEffectOutput(const CpuNodeContext& context, const EffectMaskParameters& maskParams,
                                                const CpuImage& original, CpuImage processed,
                                                std::size_t maskPort = 1) {
    const NodeInstance& node = context.node;
    const CpuImage* mask = optionalImageInput(context, maskPort);
    const InputAnchor anchor = anchorInput(context, 0, original);
    const int width = processed.width();
    const int height = processed.height();
    const bool perPixelCoverage = mask != nullptr && maskParams.channel >= 0;
    if (maskParams.channel > 3) {
        failNode(node, "parameter 'maskChannel' selects unknown channel " + std::to_string(maskParams.channel));
    }
    if (!perPixelCoverage) {
        if (maskParams.mix == 1.0F) {
            return processed;
        }
        const bool sameRaster =
            anchor.offsetX == 0 && anchor.offsetY == 0 && original.width() == width && original.height() == height;
        if (maskParams.mix == 0.0F && sameRaster) {
            return original;
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::array<float, kImageChannels> source =
                    sampledPixel(original, anchor.offsetX + x, anchor.offsetY + y);
                if (maskParams.mix == 0.0F) {
                    processed.setPixel(x, y, source);
                    continue;
                }
                const std::array<float, kImageChannels> effect = processed.pixel(x, y);
                std::array<float, kImageChannels> result{};
                for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                    result[channel] = source[channel] + (effect[channel] - source[channel]) * maskParams.mix;
                }
                processed.setPixel(x, y, result);
            }
        }
        return processed;
    }
    const InputAnchor maskAnchor = anchorInput(context, maskPort, *mask);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, kImageChannels> maskPixel =
                sampledPixel(*mask, maskAnchor.offsetX + x, maskAnchor.offsetY + y);
            float coverage = std::clamp(maskPixel[static_cast<std::size_t>(maskParams.channel)], 0.0F, 1.0F);
            if (maskParams.invert) {
                coverage = 1.0F - coverage;
            }
            const float weight = coverage * maskParams.mix;
            if (weight == 1.0F) {
                continue;
            }
            const std::array<float, kImageChannels> source =
                sampledPixel(original, anchor.offsetX + x, anchor.offsetY + y);
            if (weight == 0.0F) {
                processed.setPixel(x, y, source);
                continue;
            }
            const std::array<float, kImageChannels> effect = processed.pixel(x, y);
            std::array<float, kImageChannels> result{};
            for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                result[channel] = source[channel] + (effect[channel] - source[channel]) * weight;
            }
            processed.setPixel(x, y, result);
        }
    }
    return processed;
}

// Shared auxiliary-channel preservation (issue #90). An ordinary effect
// addresses its main input's RGBA projection and leaves every other named
// channel of that input alone; the executor applies this to any contribution
// that does not own its channel layout, so a mask, normals, motion or data
// layer survives Read -> effect -> ... untouched, and no effect states a shadow
// rule of its own.
//
// Exactly the produced channels that are NOT an identified primary role and
// that the main input also carries are copied, at UNCHANGED image coordinates
// (the two rasters sit on the same sampling lattice, so the input's sample for
// a produced sample is its anchor offset). A produced auxiliary channel the
// main input does not name stays as the effect left it (zero for a fresh
// raster), and no channel is created: the produced raster's own naming decides
// what exists.
[[nodiscard]] inline CpuImage preserveAuxiliaryChannels(const CpuNodeContext& context, CpuImage produced) {
    const CpuImage* main = optionalImageInput(context, 0);
    if (main == nullptr || produced.channelCount() == 0) {
        return produced;
    }
    const std::vector<std::string>& names = produced.layout().channels;
    std::vector<int> sourceIndex(names.size(), -1);
    bool any = false;
    for (std::size_t index = 0; index < names.size(); ++index) {
        bool primary = false;
        for (std::size_t role = 0; role < kImageChannels && !primary; ++role) {
            primary = channelsDetail::isPrimaryRoleName(names[index], role);
        }
        if (primary) {
            continue;  // the effect's own RGBA math owns this channel
        }
        sourceIndex[index] = channelIndex(main->layout().channels, names[index]);
        any = any || sourceIndex[index] >= 0;
    }
    if (!any) {
        return produced;
    }
    const InputAnchor anchor = anchorInput(context, 0, *main);
    for (int y = 0; y < produced.height(); ++y) {
        for (int x = 0; x < produced.width(); ++x) {
            for (std::size_t index = 0; index < names.size(); ++index) {
                const int source = sourceIndex[index];
                if (source < 0) {
                    continue;
                }
                produced.setChannel(x, y, static_cast<int>(index),
                                    main->channel(anchor.offsetX + x, anchor.offsetY + y, source));
            }
        }
    }
    return produced;
}

}  // namespace nemo
