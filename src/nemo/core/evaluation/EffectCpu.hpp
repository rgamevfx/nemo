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
// propagates through pass-through effects. Generators keep the square-pixel
// default because they have no main input.
[[nodiscard]] inline ImageLayout effectRasterLayout(const EvaluationRequest& request,
                                                    const CpuImage* mainInput = nullptr) {
    ImageLayout layout;
    layout.width = scaledDimension(request.region.width, request.samplingScale);
    layout.height = scaledDimension(request.region.height, request.samplingScale);
    if (mainInput != nullptr)
        layout.pixelAspect = mainInput->layout().pixelAspect;
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
[[nodiscard]] inline CpuImage blendEffectOutput(const NodeInstance& node, const EffectMaskParameters& maskParams,
                                                const CpuImage& original, CpuImage processed, const CpuImage* mask) {
    if (mask == nullptr || maskParams.channel < 0) {
        if (maskParams.mix == 1.0F) {
            return processed;
        }
        if (maskParams.mix == 0.0F) {
            return original;
        }
        const std::size_t elements =
            static_cast<std::size_t>(original.width()) * static_cast<std::size_t>(original.height()) * kImageChannels;
        const float* source = original.data();
        float* destination = processed.data();
        for (std::size_t index = 0; index < elements; ++index) {
            destination[index] = source[index] + (destination[index] - source[index]) * maskParams.mix;
        }
        return processed;
    }
    if (maskParams.channel > 3) {
        failNode(node, "parameter 'maskChannel' selects unknown channel " + std::to_string(maskParams.channel));
    }
    if (mask->width() != original.width() || mask->height() != original.height()) {
        failNode(node, "mask raster " + std::to_string(mask->width()) + "x" + std::to_string(mask->height()) +
                           " does not match the effect raster " + std::to_string(original.width()) + "x" +
                           std::to_string(original.height()));
    }
    for (int y = 0; y < original.height(); ++y) {
        for (int x = 0; x < original.width(); ++x) {
            const std::array<float, kImageChannels> maskPixel = mask->pixel(x, y);
            float coverage = std::clamp(maskPixel[static_cast<std::size_t>(maskParams.channel)], 0.0F, 1.0F);
            if (maskParams.invert) {
                coverage = 1.0F - coverage;
            }
            const float weight = coverage * maskParams.mix;
            if (weight == 1.0F) {
                continue;
            }
            const std::array<float, kImageChannels> source = original.pixel(x, y);
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
