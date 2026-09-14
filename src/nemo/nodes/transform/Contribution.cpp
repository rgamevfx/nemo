#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/transform/Parameters.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor transformDescriptor() {
    return NodeDescriptor{.type = "transform",
                          .displayName = "Transform",
                          .group = "Transform",
                          .implementationVersion = 1,
                          .inputs = effectImageInputs(),
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = withMaskParameters({
                              {.name = "translateX",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .step = 1.0,
                               .label = "X",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = -200.0,
                               .softMaximum = 200.0,
                               .row = "Translate"},
                              {.name = "translateY",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .step = 1.0,
                               .label = "Y",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = -200.0,
                               .softMaximum = 200.0,
                               .row = "Translate"},
                              {.name = "scale",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{1.0},
                               .minimum = 0.0,
                               .step = 0.001,
                               .label = "Scale",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = 0.1,
                               .softMaximum = 3.0,
                               .nonzero = true},
                              {.name = "rotate",
                               .type = ParameterType::Float,
                               .defaultValue = ParameterValue{0.0},
                               .step = 0.1,
                               .label = "Rotate",
                               .section = "Transform",
                               .editor = {},
                               .softMinimum = -180.0,
                               .softMaximum = 180.0},
                              mixParameterSpec("Transform"),
                              {.name = "filter",
                               .type = ParameterType::Choice,
                               .defaultValue = ParameterValue{ChoiceValue{"Cubic"}},
                               .choices = {"Cubic", "Linear", "Nearest"},
                               .label = "Filter",
                               .section = "Sampling",
                               .editor = {}},
                          }),
                          .capabilities = wholeImageCapabilities()};
}

[[nodiscard]] bool isFinite(float value) {
    return std::isfinite(value);
}

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

CpuImage executeTransform(const CpuNodeContext& context) {
    const CpuImage& input = requiredImageInput(context, 0, "native effect requires a connected main image input");
    if (input.width() <= 0 || input.height() <= 0) {
        failNode(context.node, "native effect requires a non-empty input raster");
    }
    CpuImage processed =
        applyTransform(context.node, context.request,
                       effectiveTransform(context.catalog, context.node, context.effectiveParams), input);
    return blendEffectOutput(context.node, effectiveEffectMask(context.catalog, context.node, context.effectiveParams),
                             input, std::move(processed), optionalImageInput(context, 1));
}

std::optional<std::string> validateTransformParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                       ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] {
        static_cast<void>(effectiveTransform(catalog, node, effectiveParams));
        static_cast<void>(effectiveEffectMask(catalog, node, effectiveParams));
    });
}

}  // namespace

NodeContribution transformContribution() {
    NodeContribution contribution;
    contribution.descriptor = transformDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeTransform};
    contribution.validateParameters = &validateTransformParameters;
    return contribution;
}

}  // namespace nemo::nodes
