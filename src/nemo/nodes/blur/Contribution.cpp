#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/blur/Parameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
                          // 3: `size` lost its arbitrary 100 hard maximum — the
                          // declared radius is executed exactly and only an
                          // unrepresentable tap count is refused, and Mix's hard
                          // range became slider travel (issue #103).
                          // 2: the adapter consumes the resolved image description
                          // for its output raster and tolerates an empty input
                          // data window (issue #88).
                          .implementationVersion = 3,
                          .inputs = effectImageInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              // Channels first, then the numeric controls, then
                              // the shared mask footer (issue #102): the schema
                              // order is the row order.
                              {.name = "channels",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"RGBA"}},
                               .choices = {"RGBA", "RGB", "Alpha"},
                               .label = "Channels",
                               .section = "Blur",
                               .editor = {}},
                              {.name = "size",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               // A radius is nonnegative by meaning; it has no
                               // upper hard bound (issue #103). 0..100 stays the
                               // slider's navigation travel only: an authored
                               // value beyond it is executed exactly, never
                               // clipped or rewritten.
                               .minimum = 0.0,
                               .step = 0.1,
                               .label = "Size",
                               .section = "Blur",
                               .editor = {},
                               .softMinimum = 0.0,
                               .softMaximum = 100.0},
                              mixParameterSpec("Blur"),
                          }),
                          .capabilities = builtinCapabilities()};
}

// Blur selects channels with the same bitmask Grade uses (R1/G2/B4/A8).
constexpr std::array<std::uint32_t, kImageChannels> kChannelBits{1U, 2U, 4U, 8U};

[[nodiscard]] int clampIndex(int value, int limit) {
    return std::clamp(value, 0, limit - 1);
}

struct BlurKernel {
    int support{0};
    std::vector<float> weights;
};

// The kernel the node's own raster is filtered with: the checked, representable
// tap count (`blurSupportAt`) and the weights this reference computes for
// itself. `size` is a full-resolution support radius while the raster samples at
// `samplingScale`, so weights use full-resolution distances as taps advance in
// raster pixels. Distances and the normalization are evaluated in double — the
// declared math — because a float sigma underflows below the float normal range
// and would turn the weights into NaN. The native preparation computes the same
// declared weights in its own code (blur/Gpu.cpp), so neither reference is the
// other's oracle.
[[nodiscard]] BlurKernel makeBlurKernel(int support, int samplingScale, float sizePixels) {
    BlurKernel kernel;
    kernel.support = support;
    const double sigma = static_cast<double>(sizePixels) / 3.0;
    kernel.weights.resize(static_cast<std::size_t>(2) * static_cast<std::size_t>(support) + 1, 1.0F);
    double sum = 0.0;
    for (int tap = -support; tap <= support; ++tap) {
        const double distance = static_cast<double>(tap) * samplingScale / sigma;
        const double weight = std::exp(-0.5 * distance * distance);
        kernel.weights[static_cast<std::size_t>(tap + support)] = static_cast<float>(weight);
        sum += weight;
    }
    for (float& weight : kernel.weights) {
        weight = static_cast<float>(static_cast<double>(weight) / sum);
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
// An input that holds no pixels at all is a valid empty image (issue #88): the
// result is transparent black, never an out-of-bounds read.
[[nodiscard]] CpuImage applyBlur(const CpuNodeContext& context, const BlurParameters& params, const CpuImage& input,
                                 const InputAnchor& anchor) {
    const NodeInstance& node = context.node;
    const EvaluationRequest& request = context.request;
    if (!isSamplingScale(request.samplingScale)) {
        failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }
    const int width = input.width();
    const int height = input.height();
    const int scale = request.samplingScale;
    const int outputWidth = scaledDimension(request.region.width, scale);
    const int outputHeight = scaledDimension(request.region.height, scale);
    if (width <= 0 || height <= 0) {
        // The whole-image reference filters these samples as unpremultiplied
        // transparent black, which stays transparent black.
        return CpuImage(effectRasterLayout(context));
    }
    if (params.size == 0.0F) {
        // Exact identity: the whole-image baseline shares the input raster, a
        // regional one is the requested window of it, bit for bit.
        if (anchor.offsetX == 0 && anchor.offsetY == 0 && width == outputWidth && height == outputHeight) {
            return input;
        }
        return windowOf(input, anchor, effectRasterLayout(context));
    }
    const int support = blurSupportAt(node, params.size, scale);
    // Owner-approved CPU-reference resource exception (#103), not an authored
    // radius or image-memory limit. Native preparation uses the device's range.
    constexpr std::uint64_t kMaxReferenceWeightBytes = 64U * 1024U * 1024U;
    const auto weightCount = static_cast<std::uint64_t>(support) * 2 + 1;
    if (weightCount > kMaxReferenceWeightBytes / sizeof(float)) {
        failNode(node, "parameter 'size' requires a weight table exceeding the CPU reference's 64 MiB budget");
    }
    const BlurKernel kernel = makeBlurKernel(support, scale, params.size);

    std::array<bool, kImageChannels> filtered{};
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        filtered[channel] = (params.channels & kChannelBits[channel]) != 0;
    }

    // Resolve the raster's named channels once (issue #90), outside every pixel
    // loop: which output channel is which primary role, and where the same
    // channel lives in the input raster. Channels that are not a primary role
    // (a mask, normals, any auxiliary layer) are not this effect's business:
    // they stay zero here and the executor's shared preservation step carries
    // them from the main input unchanged. An unselected primary role is copied
    // exactly, which is what RGB/Alpha filtering always promised.
    CpuImage output(effectRasterLayout(context));
    const std::size_t channels = output.channelCount();
    const std::size_t sourceChannels = input.channelCount();
    std::vector<int> sourceIndex(channels, -1);
    std::array<int, kImageChannels> roleAt{-1, -1, -1, -1};
    std::vector<bool> selected(channels, false);
    for (std::size_t index = 0; index < channels; ++index) {
        const std::string& name = output.layout().channels[index];
        sourceIndex[index] = channelIndex(input.layout().channels, name);
        int role = -1;
        for (std::size_t candidate = 0; candidate < kImageChannels; ++candidate) {
            if (channelsDetail::isPrimaryRoleName(name, candidate)) {
                role = static_cast<int>(candidate);
                break;
            }
        }
        if (role < 0) {
            continue;  // auxiliary/data channel: preserved centrally, never filtered
        }
        roleAt[static_cast<std::size_t>(role)] = static_cast<int>(index);
        selected[index] = filtered[static_cast<std::size_t>(role)] && sourceIndex[index] >= 0;
    }
    const int alphaSourceIndex = roleAt[3] >= 0 ? sourceIndex[static_cast<std::size_t>(roleAt[3])] : -1;
    const bool premultiply =
        roleAt[0] >= 0 && roleAt[1] >= 0 && roleAt[2] >= 0 && roleAt[3] >= 0 &&
        selected[static_cast<std::size_t>(roleAt[0])] && selected[static_cast<std::size_t>(roleAt[1])] &&
        selected[static_cast<std::size_t>(roleAt[2])] && selected[static_cast<std::size_t>(roleAt[3])];

    // One pass over the node's own raster, reading a source raster that may
    // start elsewhere and be larger (`sourceWidth`/`sourceHeight` describe it,
    // the offsets place this node's origin inside it).
    const auto filterPass = [&](const float* source, std::size_t sourceStride, int sourceWidth, int sourceHeight,
                                const InputAnchor& origin, float* target, int targetHeight, bool alongX,
                                bool premultiplySource, const std::vector<int>& sourceOf) {
        for (int y = 0; y < targetHeight; ++y) {
            for (int x = 0; x < outputWidth; ++x) {
                const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(outputWidth) +
                                          static_cast<std::size_t>(x)) *
                                         channels;
                const int column = origin.offsetX + x;
                const int row = origin.offsetY + y;
                const std::size_t sampleBase = (static_cast<std::size_t>(row) * static_cast<std::size_t>(sourceWidth) +
                                                static_cast<std::size_t>(column)) *
                                               sourceStride;
                for (std::size_t index = 0; index < channels; ++index) {
                    const int from = sourceOf[index];
                    if (from < 0) {
                        target[base + index] = 0.0F;
                        continue;
                    }
                    if (!selected[index]) {
                        target[base + index] = source[sampleBase + static_cast<std::size_t>(from)];
                        continue;
                    }
                    float sum = 0.0F;
                    for (int tap = -kernel.support; tap <= kernel.support; ++tap) {
                        const int sampleX = alongX ? clampIndex(column + tap, sourceWidth) : column;
                        const int sampleY = alongX ? row : clampIndex(row + tap, sourceHeight);
                        const std::size_t tapBase =
                            (static_cast<std::size_t>(sampleY) * static_cast<std::size_t>(sourceWidth) +
                             static_cast<std::size_t>(sampleX)) *
                            sourceStride;
                        float value = source[tapBase + static_cast<std::size_t>(from)];
                        if (premultiplySource && index != static_cast<std::size_t>(roleAt[3]) &&
                            alphaSourceIndex >= 0) {
                            value *= source[tapBase + static_cast<std::size_t>(alphaSourceIndex)];
                        }
                        // `support` is the checked, representable tap count
                        // (blurSupportAt), so this index is within [0, 2*support].
                        // NOLINTNEXTLINE(bugprone-misplaced-widening-cast): bounded index
                        sum += kernel.weights[static_cast<std::size_t>(tap + kernel.support)] * value;
                    }
                    target[base + index] = sum;
                }
            }
        }
    };

    if (width > 1 && height > 1) {
        // The intermediate carries the horizontally filtered rows of the whole
        // input raster, not only this node's rows: the vertical pass reads the
        // real vertical neighbors of the halo instead of collapsing them onto
        // the output's own extent. It is already in THIS raster's channel order,
        // so the vertical pass indexes it literally.
        std::vector<float> intermediate(
            static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(height) * channels, 0.0F);
        std::vector<int> intermediateIndex(channels);
        for (std::size_t index = 0; index < channels; ++index) {
            intermediateIndex[index] = static_cast<int>(index);
        }
        filterPass(input.data(), sourceChannels, width, height, InputAnchor{anchor.offsetX, 0}, intermediate.data(),
                   height, true, premultiply, sourceIndex);
        filterPass(intermediate.data(), channels, outputWidth, height, InputAnchor{0, anchor.offsetY}, output.data(),
                   outputHeight, false, false, intermediateIndex);
    } else if (height > 1) {
        filterPass(input.data(), sourceChannels, width, height, anchor, output.data(), outputHeight, false, premultiply,
                   sourceIndex);
    } else {
        filterPass(input.data(), sourceChannels, width, height, anchor, output.data(), outputHeight, true, premultiply,
                   sourceIndex);
    }

    if (premultiply) {
        const std::size_t pixels = static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight);
        const std::size_t red = static_cast<std::size_t>(roleAt[0]);
        const std::size_t green = static_cast<std::size_t>(roleAt[1]);
        const std::size_t blue = static_cast<std::size_t>(roleAt[2]);
        const std::size_t alpha = static_cast<std::size_t>(roleAt[3]);
        float* pointer = output.data();
        for (std::size_t index = 0; index < pixels; ++index) {
            const float value = pointer[alpha];
            if (value != 0.0F) {
                pointer[red] /= value;
                pointer[green] /= value;
                pointer[blue] /= value;
            } else {
                pointer[red] = 0.0F;
                pointer[green] = 0.0F;
                pointer[blue] = 0.0F;
            }
            pointer += channels;
        }
    }
    return output;
}

CpuImage executeBlur(const CpuNodeContext& context) {
    const CpuImage& input = requiredImageInput(context, 0, "native effect requires a connected main image input");
    const InputAnchor anchor = anchorInput(context, 0, input);
    CpuImage processed =
        applyBlur(context, effectiveBlur(context.catalog, context.node, context.effectiveParams), input, anchor);
    return blendEffectOutput(context, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             input, std::move(processed));
}

// Blur reads a halo of ceil(size/samplingScale) samples on every side of the
// region it must produce (issue #85); the optional mask is read at the output
// coordinates only. The halo is what makes a regional blur identical to the
// matching window of a whole-image blur: no tap inside the image is ever
// replaced by a clamped border pixel.
//
// The halo is clipped to the producer's own described image (issue #88), so the
// raster border a clamp-to-edge tap folds onto is the real image border and
// never a coordinate that only this node's arithmetic invented. The halo is a
// READ demand, never an output bound: Blur describes its output as exactly the
// description it inherited from its main input, because clamp-to-edge keeps
// every filtered sample inside the input's own data window.
//
// Neither port declares channels (issue #90): a filtering effect reads the
// channels its own raster names, and the inherited demand is filtered to what
// each producer really carries, so an alpha-only or multilayer input is never
// asked for channels it does not have and keeps them through the filter.
std::vector<InputRequirement> blurInputRequirements(const NodeRegionContext& context) {
    const BlurParameters params = effectiveBlur(context.catalog, context.node, context.effectiveParams);
    const EvaluationRequest& request = context.request;
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    // Widen before expanding the demand. Refuse only a halo the Region contract
    // cannot represent; a large authored radius is not a navigation limit.
    const std::int64_t radius = static_cast<std::int64_t>(blurSupportAt(context.node, params.size, scale)) * scale;
    const std::int64_t x = static_cast<std::int64_t>(request.region.x) - radius;
    const std::int64_t y = static_cast<std::int64_t>(request.region.y) - radius;
    const std::int64_t width = static_cast<std::int64_t>(request.region.width) + 2 * radius;
    const std::int64_t height = static_cast<std::int64_t>(request.region.height) + 2 * radius;
    constexpr auto low = std::numeric_limits<int>::min();
    constexpr auto high = std::numeric_limits<int>::max();
    if (x < low || y < low || width > high || height > high || x + width > high || y + height > high) {
        failNode(context.node, "parameter 'size' needs a halo outside the integer region representation");
    }
    const Region halo{static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height)};
    const Region mask = regionIntersection(request.region, requirementDomain(context, 1, request.region));
    return {InputRequirement{regionIntersection(halo, requirementDomain(context, 0, halo)), {}},
            InputRequirement{mask, {}}};
}

std::optional<std::string> validateBlurParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams) {
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
    contribution.inputRequirements = &blurInputRequirements;
    return contribution;
}

}  // namespace nemo::nodes
