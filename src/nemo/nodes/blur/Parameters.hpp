#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
// `size` is a full-resolution support radius in pixels: a real distance, so
// finite and nonnegative, but with no authored maximum (issue #103). What the
// execution cannot represent is checked where it becomes a tap count
// (blurSupportAt), not as a ceiling on the authored value.
struct BlurParameters {
    float size{0.0F};
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA};
};

// Raster support must survive both the integer weight index (tap + support)
// and the existing float payload word exactly. Check this particular support,
// not a blanket 2^24 ceiling: larger exactly representable integers are valid.
[[nodiscard]] inline int blurSupportAt(const NodeInstance& node, float size, int samplingScale) {
    const double taps = std::ceil(static_cast<double>(size) / static_cast<double>(samplingScale));
    constexpr int maxSupport = (std::numeric_limits<int>::max() - 1) / 2;
    if (!(taps >= 0.0) || taps > maxSupport || static_cast<double>(static_cast<float>(taps)) != taps) {
        failNode(node, "parameter 'size' (" + std::to_string(size) + ") at sampling scale " +
                           std::to_string(samplingScale) + " needs " + std::to_string(taps) +
                           " raster taps per axis, not representable by the weight index and support payload");
    }
    return static_cast<int>(taps);
}

[[nodiscard]] inline BlurParameters effectiveBlur(const NodeCatalog& catalog, const NodeInstance& node,
                                                  const ParameterValues& effectiveParams) {
    BlurParameters blur;
    blur.size = effectiveNumber(catalog, node, effectiveParams, "size");
    if (!std::isfinite(blur.size) || blur.size < 0.0F) {
        failNode(node, "parameter 'size' must be finite and nonnegative");
    }
    const std::string& channels = effectiveChoice(catalog, node, effectiveParams, "channels");
    if (channels == "RGBA") {
        blur.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA;
    } else if (channels == "RGB") {
        blur.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB;
    } else if (channels == "Alpha") {
        blur.channels = kEffectChannelA;
    } else {
        failNode(node, "parameter 'channels' must be one of RGBA, RGB, Alpha, got '" + channels + "'");
    }
    return blur;
}
}  // namespace nemo
