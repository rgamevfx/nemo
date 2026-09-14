#pragma once

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
struct BlurParameters {
    float size{0.0F};
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA};
};

[[nodiscard]] inline BlurParameters effectiveBlur(const NodeCatalog& catalog, const NodeInstance& node,
                                                  ParameterValues& effectiveParams) {
    BlurParameters blur;
    blur.size = effectiveNumber(catalog, node, effectiveParams, "size");
    if (!(blur.size >= 0.0F) || !(blur.size <= 100.0F)) {
        failNode(node, "parameter 'size' must be within [0, 100]");
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
