#include "nemo/core/evaluation/NativeEffects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
namespace {

// Grade selects channels with a bitmask (R1/G2/B4/A8); Blur reuses the same
// bits for its RGBA/RGB/Alpha choices. Storage is always interleaved RGBA.
constexpr std::array<std::uint32_t, kImageChannels> kChannelBits{1U, 2U, 4U, 8U};

[[nodiscard]] bool isFinite(float value) {
    return std::isfinite(value);
}

// Grade's signed power (issue #34 contract): zero stays zero for every
// exponent and negative/HDR values keep their sign, so the operation is a
// domain-safe extension rather than an implicit clamp or a complex result.
[[nodiscard]] float signedPow(float base, float exponent) {
    if (exponent == 1.0F) {
        return base;
    }
    if (base == 0.0F) {
        return 0.0F;
    }
    return std::copysign(std::pow(std::fabs(base), exponent), base);
}

[[nodiscard]] int clampIndex(int value, int limit) {
    return std::clamp(value, 0, limit - 1);
}

// Filtering and spatial interpolation are alpha-aware: samples are read as
// premultiplied RGB so transparent neighbors cannot contribute color.
[[nodiscard]] std::array<float, kImageChannels> premultipliedPixel(const CpuImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    const std::array<float, kImageChannels> pixel = image.pixel(x, y);
    return {pixel[0] * pixel[3], pixel[1] * pixel[3], pixel[2] * pixel[3], pixel[3]};
}

// Straight (non-premultiplied) output: only an exactly zero alpha forces zero
// RGB; any nonzero alpha (including negative values from cubic ringing or a
// source) divides through without an implicit clamp.
[[nodiscard]] std::array<float, kImageChannels> straightPixel(const std::array<float, kImageChannels>& premultiplied) {
    const float alpha = premultiplied[3];
    if (alpha == 0.0F) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    return {premultiplied[0] / alpha, premultiplied[1] / alpha, premultiplied[2] / alpha, alpha};
}

// --- Grade ---------------------------------------------------------------

struct GradeCoefficients {
    std::array<float, kImageChannels> slope{};
    std::array<float, kImageChannels> intercept{};
    std::array<float, kImageChannels> exponent{};
    std::array<bool, kImageChannels> enabled{};
};

// Forward: y = signedPow(a*x + b, 1/gamma) with a = (gain-lift)*multiply/
// (whitepoint-blackpoint) and b = lift + offset - blackpoint*a.
// Reverse:  x = (signedPow(y, gamma) - b) / a.
// Disabled channels are exact pass-through. The shared metadata seam
// (effectiveGrade) owns validation, including singular and unrepresentable
// enabled-channel settings, so this derives only the coefficients execution
// consumes.
[[nodiscard]] GradeCoefficients resolveGradeCoefficients(const GradeParameters& params) {
    GradeCoefficients coefficients;
    for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
        coefficients.enabled[channel] = (params.channels & kChannelBits[channel]) != 0;
        if (!coefficients.enabled[channel]) {
            continue;
        }
        const float slope = (params.gain[channel] - params.lift[channel]) * params.multiply[channel] /
                            (params.whitepoint[channel] - params.blackpoint[channel]);
        coefficients.slope[channel] = slope;
        coefficients.intercept[channel] =
            params.lift[channel] + params.offset[channel] - params.blackpoint[channel] * slope;
        coefficients.exponent[channel] = params.reverse ? params.gamma[channel] : 1.0F / params.gamma[channel];
    }
    return coefficients;
}

[[nodiscard]] CpuImage applyGrade(const GradeParameters& params, const CpuImage& input) {
    const GradeCoefficients coefficients = resolveGradeCoefficients(params);
    CpuImage output(input.layout());
    const std::size_t pixels = static_cast<std::size_t>(input.width()) * static_cast<std::size_t>(input.height());
    const float* source = input.data();
    float* destination = output.data();
    for (std::size_t index = 0; index < pixels; ++index) {
        for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
            const float value = source[channel];
            if (!coefficients.enabled[channel]) {
                destination[channel] = value;
                continue;
            }
            float result;
            if (params.reverse) {
                result = (signedPow(value, coefficients.exponent[channel]) - coefficients.intercept[channel]) /
                         coefficients.slope[channel];
            } else {
                result = signedPow(coefficients.slope[channel] * value + coefficients.intercept[channel],
                                   coefficients.exponent[channel]);
            }
            if (params.clampBlack && result < 0.0F) {
                result = 0.0F;
            }
            if (params.clampWhite && result > 1.0F) {
                result = 1.0F;
            }
            destination[channel] = result;
        }
        source += kImageChannels;
        destination += kImageChannels;
    }
    return output;
}

// --- Blur ----------------------------------------------------------------

struct BlurKernel {
    int support{0};
    std::vector<float> weights;
};

// `size` is a full-resolution support radius; the raster the kernel works on
// is sampled at `request.samplingScale`, so weights use full-resolution
// distances while taps advance in raster pixels. The kernel is normalized
// per axis and computed once per node evaluation.
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

// --- Transform -----------------------------------------------------------

// Inverse-maps each output pixel center through uniform scale, clockwise
// rotation (stored raster has x right/y down) and translation, rotating in
// physical coordinates (x*pixelAspect, y) so non-square pixels stay rigid.
// The mapped full-resolution coordinate then selects an input raster sample.
[[nodiscard]] CpuImage applyTransform(const NodeInstance& node, const EvaluationRequest& request,
                                      const TransformParameters& params, const CpuImage& input) {
    if (!isFinite(params.translateX) || !isFinite(params.translateY) || !isFinite(params.rotate) ||
        !isFinite(params.scale) || !(params.scale > 0.0F)) {
        failNode(node, "Transform parameters must be finite with a positive 'scale'");
    }
    if (params.filter < 0 || params.filter > 2) {
        failNode(node, "parameter 'filter' has unknown mode " + std::to_string(params.filter));
    }
    if (!isSamplingScale(request.samplingScale)) {
        failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                           " is not a declared reduction (supported scales: 1, 2, 4)");
    }

    const int width = input.width();
    const int height = input.height();
    const int scale = request.samplingScale;
    const float sampling = static_cast<float>(scale);
    const float fullWidth = static_cast<float>(request.imageWidth());
    const float fullHeight = static_cast<float>(request.imageHeight());
    if (!(fullWidth > 0.0F) || !(fullHeight > 0.0F)) {
        failNode(node, "Transform requires a positive full-resolution image domain");
    }
    const float centerX = fullWidth * 0.5F;
    const float centerY = fullHeight * 0.5F;

    const float aspect = input.layout().pixelAspect;
    if (!isFinite(aspect) || !(aspect > 0.0F)) {
        failNode(node, "input pixel aspect " + std::to_string(aspect) + " must be finite and positive");
    }
    // Radians and trigonometry are computed in double and rounded once to
    // float, matching the GPU executor's uniform computation so rotation
    // residuals (e.g. cos 90 degrees) agree without an epsilon or tie change.
    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    const double radians = static_cast<double>(params.rotate) * kDegreesToRadians;
    const float cosine = static_cast<float>(std::cos(radians));
    const float sine = static_cast<float>(std::sin(radians));
    const float originX = static_cast<float>(request.region.x);
    const float originY = static_cast<float>(request.region.y);

    CpuImage output(input.layout());
    for (int y = 0; y < height; ++y) {
        const float outputY = originY + (static_cast<float>(y) + 0.5F) * sampling;
        for (int x = 0; x < width; ++x) {
            const float outputX = originX + (static_cast<float>(x) + 0.5F) * sampling;
            const float offsetX = outputX - centerX - params.translateX;
            const float offsetY = outputY - centerY - params.translateY;
            const float physicalX = offsetX * aspect;
            const float physicalY = offsetY;
            const float rotatedX = cosine * physicalX + sine * physicalY;
            const float rotatedY = -sine * physicalX + cosine * physicalY;
            const float mappedX = centerX + (rotatedX / aspect) / params.scale;
            const float mappedY = centerY + rotatedY / params.scale;

            if (!isFinite(mappedX) || !isFinite(mappedY)) {
                output.setPixel(x, y, {0.0F, 0.0F, 0.0F, 0.0F});
                continue;
            }

            if (params.filter == 2) {
                const float nearestX = std::floor(mappedX / sampling);
                const float nearestY = std::floor(mappedY / sampling);
                if (nearestX < 0.0F || nearestY < 0.0F || nearestX > static_cast<float>(width - 1) ||
                    nearestY > static_cast<float>(height - 1)) {
                    output.setPixel(x, y, {0.0F, 0.0F, 0.0F, 0.0F});
                } else {
                    output.setPixel(x, y, input.pixel(static_cast<int>(nearestX), static_cast<int>(nearestY)));
                }
                continue;
            }

            const float rasterX = mappedX / sampling - 0.5F;
            const float rasterY = mappedY / sampling - 0.5F;
            const float lastX = static_cast<float>(width - 1);
            const float lastY = static_cast<float>(height - 1);
            // The whole support window outside the raster is transparent black
            // either way; bounding it here keeps the floor-to-index conversion
            // inside the int range for wildly translated coordinates.
            if (params.filter == 1) {
                if (rasterX <= -2.0F || rasterX >= lastX + 1.0F || rasterY <= -2.0F || rasterY >= lastY + 1.0F) {
                    output.setPixel(x, y, {0.0F, 0.0F, 0.0F, 0.0F});
                    continue;
                }
            } else if (rasterX <= -3.0F || rasterX >= lastX + 2.0F || rasterY <= -3.0F || rasterY >= lastY + 2.0F) {
                output.setPixel(x, y, {0.0F, 0.0F, 0.0F, 0.0F});
                continue;
            }
            std::array<float, kImageChannels> accumulated{0.0F, 0.0F, 0.0F, 0.0F};

            if (params.filter == 1) {
                const int baseX = static_cast<int>(std::floor(rasterX));
                const int baseY = static_cast<int>(std::floor(rasterY));
                const float fractionX = rasterX - static_cast<float>(baseX);
                const float fractionY = rasterY - static_cast<float>(baseY);
                const float weightsX[2] = {1.0F - fractionX, fractionX};
                const float weightsY[2] = {1.0F - fractionY, fractionY};
                for (int tapY = 0; tapY < 2; ++tapY) {
                    for (int tapX = 0; tapX < 2; ++tapX) {
                        const float weight = weightsX[tapX] * weightsY[tapY];
                        if (weight == 0.0F) {
                            continue;
                        }
                        const auto sample = premultipliedPixel(input, baseX + tapX, baseY + tapY);
                        for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                            accumulated[channel] += weight * sample[channel];
                        }
                    }
                }
            } else {
                const int baseX = static_cast<int>(std::floor(rasterX));
                const int baseY = static_cast<int>(std::floor(rasterY));
                const float fractionX = rasterX - static_cast<float>(baseX);
                const float fractionY = rasterY - static_cast<float>(baseY);
                // Catmull-Rom basis with a = -0.5, taps at base-1 .. base+2.
                const float weightsX[4] = {
                    ((-0.5F * fractionX + 1.0F) * fractionX - 0.5F) * fractionX,
                    ((1.5F * fractionX - 2.5F) * fractionX) * fractionX + 1.0F,
                    ((-1.5F * fractionX + 2.0F) * fractionX + 0.5F) * fractionX,
                    ((0.5F * fractionX - 0.5F) * fractionX) * fractionX,
                };
                const float weightsY[4] = {
                    ((-0.5F * fractionY + 1.0F) * fractionY - 0.5F) * fractionY,
                    ((1.5F * fractionY - 2.5F) * fractionY) * fractionY + 1.0F,
                    ((-1.5F * fractionY + 2.0F) * fractionY + 0.5F) * fractionY,
                    ((0.5F * fractionY - 0.5F) * fractionY) * fractionY,
                };
                for (int tapY = 0; tapY < 4; ++tapY) {
                    for (int tapX = 0; tapX < 4; ++tapX) {
                        const float weight = weightsX[tapX] * weightsY[tapY];
                        if (weight == 0.0F) {
                            continue;
                        }
                        const auto sample = premultipliedPixel(input, baseX + tapX - 1, baseY + tapY - 1);
                        for (std::size_t channel = 0; channel < kImageChannels; ++channel) {
                            accumulated[channel] += weight * sample[channel];
                        }
                    }
                }
            }
            output.setPixel(x, y, straightPixel(accumulated));
        }
    }
    return output;
}

// --- Common mask/mix -----------------------------------------------------

// One final blend at output coordinates: original*(1-weight) + processed*weight
// with weight = coverage*mix. An absent mask (or maskChannel none) has full
// coverage independent of inversion; a connected mask contributes its
// clamped selected stored channel, optionally inverted. The effect result is
// blended in place, so an evaluation holds one output buffer (plus the blur
// scratch while it runs) rather than a second full raster.
[[nodiscard]] CpuImage blendEffectOutput(const NodeInstance& node, const EffectMaskParameters& maskParams,
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

}  // namespace

CpuImage evaluateNativeEffect(const NodeCatalog& catalog, const NodeInstance& node, ParameterValues& effectiveParams,
                              const EvaluationRequest& request, const CpuImage& input, const CpuImage* mask) {
    if (input.width() <= 0 || input.height() <= 0) {
        failNode(node, "native effect requires a non-empty input raster");
    }

    CpuImage processed = [&]() -> CpuImage {
        if (node.type == "grade") {
            return applyGrade(effectiveGrade(catalog, node, effectiveParams), input);
        }
        if (node.type == "blur") {
            return applyBlur(node, request, effectiveBlur(catalog, node, effectiveParams), input);
        }
        if (node.type == "transform") {
            return applyTransform(node, request, effectiveTransform(catalog, node, effectiveParams), input);
        }
        failNode(node, "node type '" + node.type + "' has no native effect implementation");
    }();

    return blendEffectOutput(node, effectiveEffectMask(catalog, node, effectiveParams), input, std::move(processed),
                             mask);
}

}  // namespace nemo
