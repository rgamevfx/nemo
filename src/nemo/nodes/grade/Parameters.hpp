#pragma once

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
// Grade coefficients are per-channel color values; `channels` selects which
// bitmask channels are transformed. Gamma is per channel.
struct GradeParameters {
    std::array<float, 4> blackpoint{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> whitepoint{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> lift{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> gain{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> multiply{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> offset{0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> gamma{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB};
    bool reverse{false};
    bool clampBlack{true};
    bool clampWhite{false};
};

[[nodiscard]] inline GradeParameters effectiveGrade(const NodeCatalog& catalog, const NodeInstance& node,
                                                    const ParameterValues& effectiveParams) {
    GradeParameters grade;
    grade.blackpoint = effectiveColor4(catalog, node, effectiveParams, "blackpoint");
    grade.whitepoint = effectiveColor4(catalog, node, effectiveParams, "whitepoint");
    grade.lift = effectiveColor4(catalog, node, effectiveParams, "lift");
    grade.gain = effectiveColor4(catalog, node, effectiveParams, "gain");
    grade.multiply = effectiveColor4(catalog, node, effectiveParams, "multiply");
    grade.offset = effectiveColor4(catalog, node, effectiveParams, "offset");
    grade.gamma = effectiveColor4(catalog, node, effectiveParams, "gamma");
    const std::string& channels = effectiveChoice(catalog, node, effectiveParams, "channels");
    if (channels == "RGB") {
        grade.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB;
    } else if (channels == "RGBA") {
        grade.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA;
    } else if (channels == "R") {
        grade.channels = kEffectChannelR;
    } else if (channels == "G") {
        grade.channels = kEffectChannelG;
    } else if (channels == "B") {
        grade.channels = kEffectChannelB;
    } else if (channels == "Alpha") {
        grade.channels = kEffectChannelA;
    } else if (channels == "None") {
        grade.channels = 0U;
    } else {
        failNode(node, "parameter 'channels' must be one of RGB, RGBA, R, G, B, Alpha, None, got '" + channels + "'");
    }
    grade.reverse = effectiveFlag(catalog, node, effectiveParams, "reverse");
    grade.clampBlack = effectiveFlag(catalog, node, effectiveParams, "clampBlack");
    grade.clampWhite = effectiveFlag(catalog, node, effectiveParams, "clampWhite");

    // Shared admissibility: every enabled channel must admit a finite forward
    // operation, and a reverse grade must also be invertible. Disabled
    // (unselected) channels pass through untouched and are not constrained,
    // even when their coefficients are singular.
    //
    // Equal endpoints are not a rejection (issue #103). An enabled channel whose
    // Blackpoint and Whitepoint coincide is a constant mapping, tested by exact
    // float equality with no epsilon band: the near-equal discontinuity is
    // accepted. Forward keeps running the remaining controls on the shared
    // endpoint as the range-mapped value; reverse has no source to recover and
    // uses the shared endpoint itself, then the shared clamps and Mask/Mix.
    for (int channel = 0; channel < 4; ++channel) {
        if ((grade.channels & (1U << channel)) == 0)
            continue;
        const auto index = static_cast<std::size_t>(channel);
        const float gamma = grade.gamma[index];
        if (!(gamma > 0.0F) || !std::isfinite(gamma)) {
            failNode(node, "parameter 'gamma' must be finite and positive for every enabled channel");
        }
        // Every coefficient component of an enabled channel must itself be
        // finite; disabled channels are exempt.
        const auto componentFinite = [&](const std::array<float, 4>& values) { return std::isfinite(values[index]); };
        if (!componentFinite(grade.blackpoint) || !componentFinite(grade.whitepoint) || !componentFinite(grade.lift) ||
            !componentFinite(grade.gain) || !componentFinite(grade.multiply) || !componentFinite(grade.offset) ||
            !componentFinite(grade.gamma)) {
            failNode(node, "grade color coefficients must be finite for every enabled channel");
        }
        // Forward uses 1/gamma, reverse uses gamma; the exponent actually
        // applied must be representable as a finite float.
        const float exponent = grade.reverse ? gamma : 1.0F / gamma;
        if (!std::isfinite(exponent)) {
            failNode(node, "parameter 'gamma' has no finite exponent for an enabled channel");
        }
        const float blackpoint = grade.blackpoint[index];
        const float whitepoint = grade.whitepoint[index];
        if (blackpoint == whitepoint) {
            // The shared endpoint is the whole result: forward continues the
            // remaining controls on it, reverse uses it directly. Only the
            // forward constant must additionally be representable, checked in
            // the order the execution evaluates it so no NaN reaches a pixel.
            if (!grade.reverse) {
                const float constant = (grade.gain[index] - grade.lift[index]) * grade.multiply[index] * blackpoint +
                                       grade.lift[index] + grade.offset[index];
                if (!std::isfinite(constant)) {
                    failNode(node, "grade parameters produce an unrepresentable constant for an enabled channel");
                }
            }
            continue;
        }
        const float slope = (grade.gain[index] - grade.lift[index]) * grade.multiply[index] / (whitepoint - blackpoint);
        const float intercept = grade.lift[index] + grade.offset[index] - blackpoint * slope;
        if (!std::isfinite(slope) || !std::isfinite(intercept)) {
            failNode(node, "grade parameters produce unrepresentable coefficients for an enabled channel");
        }
        if (grade.reverse && slope == 0.0F) {
            failNode(node, "reverse grade requires a nonzero slope for every enabled channel");
        }
    }
    return grade;
}
}  // namespace nemo
