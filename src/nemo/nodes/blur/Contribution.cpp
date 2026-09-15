#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/blur/Parameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor blurDescriptor() {
    return NodeDescriptor{.type = "blur",
                          .displayName = "Blur",
                          .group = "Blur",
                          .implementationVersion = 1,
                          .inputs = effectImageInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              {.name = "size",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .minimum = 0.0,
                               .maximum = 100.0,
                               .step = 0.1,
                               .label = "Size",
                               .section = "Blur",
                               .editor = {}},
                              {.name = "channels",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"RGBA"}},
                               .choices = {"RGBA", "RGB", "Alpha"},
                               .label = "Channels",
                               .section = "Blur",
                               .editor = {}},
                              mixParameterSpec("Blur"),
                          }),
                          .capabilities = builtinCapabilities()};
}

// Blur selects channels with the same bitmask Grade uses (R1/G2/B4/A8).
constexpr std::array<std::uint32_t, kImageChannels> kChannelBits{1U, 2U, 4U, 8U};

[[nodiscard]] bool isFinite(float value) {
    return std::isfinite(value);
}

[[nodiscard]] int clampIndex(int value, int limit) {
    return std::clamp(value, 0, limit - 1);
}

struct BlurKernel {
    int support{0};
    std::vector<float> weights;
};

// `size` is a full-resolution support radius; the raster the kernel works on is
// sampled at `request.samplingScale`, so weights use full-resolution distances
// while taps advance in raster pixels. The kernel is normalized per axis and
// computed once per node evaluation.
[[nodiscard]] BlurKernel makeBlurKernel(int samplingScale, float sizePixels) {
    BlurKernel kernel;
    const float sigma = sizePixels / 3.0F;
    kernel.support = static_cast<int>(std::ceil(sizePixels / static_cast<float>(samplingScale)));
    kernel.weights.resize(2 * static_cast<std::size_t>(kernel.support) + 1);
    float sum = 0.0F;
    for (int tap = -kernel.support; tap <= kernel.support; ++tap) {
        const float distance = static_cast<float>(tap * samplingScale) / sigma;
        const float weight = std::exp(-0.5F * distance * distance);
        // effectiveBlur bounds support to [0, 100], so this index is within [0, 200].
        // NOLINTNEXTLINE(bugprone-misplaced-widening-cast): bounded index
        kernel.weights[static_cast<std::size_t>(tap + kernel.support)] = weight;
        sum += weight;
    }
    for (float& weight : kernel.weights) {
        weight /= sum;
    }
    return kernel;
}

// Normalized separable Gaussian with clamp-to-edge borders. RGBA premultiplies
// straight RGB for filtering and unpremultiplies once at the end; RGB filters
// RGB and preserves alpha; Alpha filters alpha and preserves RGB.
//
// The input raster is read through `anchor` (issue #85): a regional blur is
// asked for a halo beyond the region it must produce, so the raster it reads
// starts before it and may be larger. Clamp-to-edge therefore stays what it
// always was — the border of the actual image — because a clamped tap only ever
// happens where the input raster ends, and that is exactly where the image ends.
[[nodiscard]] CpuImage applyBlur(const NodeInstance& node, const EvaluationRequest& request,
                                 const BlurParameters& params, const CpuImage& input, const InputAnchor& anchor) {
    if (!isFinite(params.size) || params.size < 0.0F) {
        failNode(node, "parameter 'size' must be finite and nonnegative");
    }
    if (!isSamplingScale(request.samplingScale)) {
        failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const int width = input.width();
    const int height = input.height();
    const int scale = request.samplingScale;
    const int outputWidth = scaledDimension(request.region.width, scale);
    const int outputHeight = scaledDimension(request.region.height, scale);
    if (params.size == 0.0F) {
        // Exact identity: the whole-image baseline shares the input raster, a
        // regional one is the requested window of it, bit for bit.
        if (anchor.offsetX == 0 && anchor.offsetY == 0 && width == outputWidth && height == outputHeight) {
            return input;
        }
        return windowOf(input, anchor, effectRasterLayout(request, &input));
    }
    const BlurKernel kernel = makeBlurKernel(scale, params.size);

    std::array<bool, kImageChannels> filtered{};
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        filtered[channel] = (params.channels & kChannelBits[channel]) != 0;
    }
    const bool premultiply = filtered[0] && filtered[1] && filtered[2] && filtered[3];

    // One pass over the node's own raster, reading a source raster that may
    // start elsewhere and be larger (`sourceWidth`/`sourceHeight` describe it,
    // the offsets place this node's origin inside it).
    const auto filterPass = [&](const float* source, int sourceWidth, int sourceHeight, const InputAnchor& origin,
                                float* target, int targetHeight, bool alongX, bool premultiplySource) {
        for (int y = 0; y < targetHeight; ++y) {
            for (int x = 0; x < outputWidth; ++x) {
                const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(outputWidth) +
                                          static_cast<std::size_t>(x)) *
                                         kImageChannels;
                const int column = origin.offsetX + x;
                const int row = origin.offsetY + y;
                for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                    if (!filtered[channel]) {
                        target[base + channel] =
                            source[(static_cast<std::size_t>(row) * static_cast<std::size_t>(sourceWidth) +
                                    static_cast<std::size_t>(column)) *
                                       kImageChannels +
                                   channel];
                        continue;
                    }
                    float sum = 0.0F;
                    for (int tap = -kernel.support; tap <= kernel.support; ++tap) {
                        const int sampleX = alongX ? clampIndex(column + tap, sourceWidth) : column;
                        const int sampleY = alongX ? row : clampIndex(row + tap, sourceHeight);
                        const std::size_t sampleIndex =
                            (static_cast<std::size_t>(sampleY) * static_cast<std::size_t>(sourceWidth) +
                             static_cast<std::size_t>(sampleX)) *
                            kImageChannels;
                        float value = source[sampleIndex + channel];
                        if (premultiplySource && channel != 3) {
                            value *= source[sampleIndex + 3];
                        }
                        // effectiveBlur bounds support to [0, 100], so this index is within [0, 200].
                        // NOLINTNEXTLINE(bugprone-misplaced-widening-cast): bounded index
                        sum += kernel.weights[static_cast<std::size_t>(tap + kernel.support)] * value;
                    }
                    target[base + channel] = sum;
                }
            }
        }
    };

    CpuImage output(effectRasterLayout(request, &input));
    if (width > 1 && height > 1) {
        // The intermediate carries the horizontally filtered rows of the whole
        // input raster, not only this node's rows: the vertical pass reads the
        // real vertical neighbors of the halo instead of collapsing them onto
        // the output's own extent.
        std::vector<float> intermediate(
            static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(height) * kImageChannels, 0.0F);
        filterPass(input.data(), width, height, InputAnchor{anchor.offsetX, 0}, intermediate.data(), height, true,
                   premultiply);
        filterPass(intermediate.data(), outputWidth, height, InputAnchor{0, anchor.offsetY}, output.data(),
                   outputHeight, false, false);
    } else if (height > 1) {
        filterPass(input.data(), width, height, anchor, output.data(), outputHeight, false, premultiply);
    } else {
        filterPass(input.data(), width, height, anchor, output.data(), outputHeight, true, premultiply);
    }

    if (premultiply) {
        const std::size_t pixels = static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight);
        float* pointer = output.data();
        for (std::size_t index = 0; index < pixels; ++index) {
            const float alpha = pointer[3];
            if (alpha != 0.0F) {
                pointer[0] /= alpha;
                pointer[1] /= alpha;
                pointer[2] /= alpha;
            } else {
                pointer[0] = 0.0F;
                pointer[1] = 0.0F;
                pointer[2] = 0.0F;
            }
            pointer += kImageChannels;
        }
    }
    return output;
}

CpuImage executeBlur(const CpuNodeContext& context) {
    const CpuImage& input = requiredImageInput(context, 0, "native effect requires a connected main image input");
    if (input.width() <= 0 || input.height() <= 0) {
        failNode(context.node, "native effect requires a non-empty input raster");
    }
    const InputAnchor anchor = anchorInput(context, 0, input);
    CpuImage processed =
        applyBlur(context.node, context.request, effectiveBlur(context.catalog, context.node, context.effectiveParams),
                  input, anchor);
    return blendEffectOutput(context, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             input, std::move(processed));
}

// Blur reads a halo of ceil(size/samplingScale) samples on every side of the
// region it must produce, clipped to the image domain (issue #85); the optional
// mask is read at the output coordinates only. The halo is what makes a
// regional blur identical to the matching window of a whole-image blur: no tap
// inside the domain is ever replaced by a clamped border pixel.
std::vector<Region> blurInputRegions(const NodeRegionContext& context) {
    const BlurParameters params = effectiveBlur(context.catalog, context.node, context.effectiveParams);
    const EvaluationRequest& request = context.request;
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    const int radius = static_cast<int>(std::ceil(params.size / static_cast<float>(scale))) * scale;
    const Region halo{request.region.x - radius, request.region.y - radius, request.region.width + 2 * radius,
                      request.region.height + 2 * radius};
    return {halo, request.region};
}

std::optional<std::string> validateBlurParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                  ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] {
        static_cast<void>(effectiveBlur(catalog, node, effectiveParams));
        static_cast<void>(effectiveEffectMask(catalog, node, effectiveParams));
    });
}

}  // namespace

NodeContribution blurContribution() {
    NodeContribution contribution;
    contribution.descriptor = blurDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeBlur};
    contribution.validateParameters = &validateBlurParameters;
    contribution.inputRegions = &blurInputRegions;
    return contribution;
}

}  // namespace nemo::nodes
