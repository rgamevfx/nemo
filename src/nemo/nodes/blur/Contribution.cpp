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
                          .capabilities = wholeImageCapabilities()};
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
[[nodiscard]] CpuImage applyBlur(const NodeInstance& node, const EvaluationRequest& request,
                                 const BlurParameters& params, const CpuImage& input) {
    if (!isFinite(params.size) || params.size < 0.0F) {
        failNode(node, "parameter 'size' must be finite and nonnegative");
    }
    if (!isSamplingScale(request.samplingScale)) {
        failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    if (params.size == 0.0F) {
        return input;
    }

    const int width = input.width();
    const int height = input.height();
    const int scale = request.samplingScale;
    const BlurKernel kernel = makeBlurKernel(scale, params.size);

    std::array<bool, kImageChannels> filtered{};
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        filtered[channel] = (params.channels & kChannelBits[channel]) != 0;
    }
    const bool premultiply = filtered[0] && filtered[1] && filtered[2] && filtered[3];

    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const auto filterPass = [&](const float* source, float* target, bool alongX, bool premultiplySource) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t base =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
                    kImageChannels;
                for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                    if (!filtered[channel]) {
                        target[base + channel] = source[base + channel];
                        continue;
                    }
                    float sum = 0.0F;
                    for (int tap = -kernel.support; tap <= kernel.support; ++tap) {
                        const int sampleX = alongX ? clampIndex(x + tap, width) : x;
                        const int sampleY = alongX ? y : clampIndex(y + tap, height);
                        const std::size_t sampleIndex =
                            (static_cast<std::size_t>(sampleY) * static_cast<std::size_t>(width) +
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

    CpuImage output(input.layout());
    if (width > 1 && height > 1) {
        // The second pass needs the first pass's result; a single-axis blur
        // never allocates the intermediate buffer.
        std::vector<float> intermediate(pixels * kImageChannels, 0.0F);
        filterPass(input.data(), intermediate.data(), true, premultiply);
        filterPass(intermediate.data(), output.data(), false, false);
    } else if (height > 1) {
        filterPass(input.data(), output.data(), false, premultiply);
    } else {
        filterPass(input.data(), output.data(), true, premultiply);
    }

    if (premultiply) {
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
    CpuImage processed = applyBlur(context.node, context.request,
                                   effectiveBlur(context.catalog, context.node, context.effectiveParams), input);
    return blendEffectOutput(context.node, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             input, std::move(processed), optionalImageInput(context, 1));
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
    return contribution;
}

}  // namespace nemo::nodes
