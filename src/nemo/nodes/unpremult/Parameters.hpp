#pragma once

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {

struct UnpremultParameters {
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB};
    int byRole{3};
};

[[nodiscard]] inline UnpremultParameters effectiveUnpremult(const NodeCatalog& catalog, const NodeInstance& node,
                                                            const ParameterValues& effectiveParams) {
    UnpremultParameters result;
    const std::string& divide = effectiveChoice(catalog, node, effectiveParams, "divide");
    if (divide == "RGB") {
        result.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB;
    } else if (divide == "RGBA") {
        result.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA;
    } else if (divide == "R") {
        result.channels = kEffectChannelR;
    } else if (divide == "G") {
        result.channels = kEffectChannelG;
    } else if (divide == "B") {
        result.channels = kEffectChannelB;
    } else if (divide == "Alpha") {
        result.channels = kEffectChannelA;
    } else if (divide == "None") {
        result.channels = 0U;
    } else {
        failNode(node, "parameter 'divide' must be one of RGB, RGBA, R, G, B, Alpha, None, got '" + divide + "'");
    }

    const std::string& by = effectiveChoice(catalog, node, effectiveParams, "by");
    if (by == "R") {
        result.byRole = 0;
    } else if (by == "G") {
        result.byRole = 1;
    } else if (by == "B") {
        result.byRole = 2;
    } else if (by == "Alpha") {
        result.byRole = 3;
    } else {
        failNode(node, "parameter 'by' must be one of R, G, B, Alpha, got '" + by + "'");
    }
    return result;
}

}  // namespace nemo
