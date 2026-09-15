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
#include <string>
#include <utility>

#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

// Raster an effect produces for `request`: the requested region at the request's
// sampling scale, carrying the main input's pixel aspect so raster metadata
// propagates through pass-through effects. A generator supplies the pixel
// aspect of its owning network's authored canvas.
[[nodiscard]] inline ImageLayout effectRasterLayout(const EvaluationRequest& request,
                                                    const CpuImage* mainInput = nullptr, float pixelAspect = 1.0F) {
    ImageLayout layout;
    layout.width = scaledDimension(request.region.width, request.samplingScale);
    layout.height = scaledDimension(request.region.height, request.samplingScale);
    layout.pixelAspect = mainInput != nullptr ? mainInput->layout().pixelAspect : pixelAspect;
    return layout;
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
// declared input `port` (issue #85), reported as a node-identified failure when
// the input does not cover the node's request.
[[nodiscard]] inline InputAnchor anchorInput(const CpuNodeContext& context, std::size_t port, const CpuImage& image) {
    const EvaluationRequest& request = context.request;
    const EvaluationRequest& source = inputRequest(context, port);
    const InputAnchor anchor = anchorBetween(source, request);
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    const int width = scaledDimension(request.region.width, scale);
    const int height = scaledDimension(request.region.height, scale);
    if (source.samplingScale != scale || anchor.offsetX < 0 || anchor.offsetY < 0 ||
        anchor.offsetX + width > image.width() || anchor.offsetY + height > image.height()) {
        failNode(context.node, "input raster " + std::to_string(image.width()) + "x" + std::to_string(image.height()) +
                                   " at origin (" + std::to_string(source.region.x) + "," +
                                   std::to_string(source.region.y) + ") does not cover the requested region " +
                                   std::to_string(width) + "x" + std::to_string(height) + " at origin (" +
                                   std::to_string(request.region.x) + "," + std::to_string(request.region.y) + ")");
    }
    return anchor;
}

// This node's own raster of an input, copied out of that input's raster (which
// may be larger). Only used where the output is honestly a window of an input,
// so the copy is the result rather than a needless intermediate.
[[nodiscard]] inline CpuImage windowOf(const CpuImage& image, const InputAnchor& anchor, ImageLayout layout) {
    CpuImage window(std::move(layout));
    for (int y = 0; y < window.height(); ++y) {
        for (int x = 0; x < window.width(); ++x) {
            window.setPixel(x, y, image.pixel(anchor.offsetX + x, anchor.offsetY + y));
        }
    }
    return window;
}

// Filtering and spatial interpolation are alpha-aware: samples are read as
// premultiplied RGB so transparent neighbors cannot contribute color.
[[nodiscard]] inline std::array<float, kImageChannels> premultipliedPixel(const CpuImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    const std::array<float, kImageChannels> pixel = image.pixel(x, y);
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
// the node's own region, taken from each input's real origin. `original` is the
// node's main image input (declared port 0) and `maskPort` is the declared port
// its optional mask lives on.
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
                const std::array<float, kImageChannels> source = original.pixel(anchor.offsetX + x, anchor.offsetY + y);
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
                mask->pixel(maskAnchor.offsetX + x, maskAnchor.offsetY + y);
            float coverage = std::clamp(maskPixel[static_cast<std::size_t>(maskParams.channel)], 0.0F, 1.0F);
            if (maskParams.invert) {
                coverage = 1.0F - coverage;
            }
            const float weight = coverage * maskParams.mix;
            if (weight == 1.0F) {
                continue;
            }
            const std::array<float, kImageChannels> source = original.pixel(anchor.offsetX + x, anchor.offsetY + y);
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

}  // namespace nemo
