#pragma once

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {

struct PremultParameters {
    std::uint32_t channels{kEffectChannelR | kEffectChannelG | kEffectChannelB};
    int byRole{3};
};

[[nodiscard]] inline PremultParameters effectivePremult(const NodeCatalog& catalog, const NodeInstance& node,
                                                        const ParameterValues& effectiveParams) {
    PremultParameters result;
    const std::string& multiply = effectiveChoice(catalog, node, effectiveParams, "multiply");
    if (multiply == "RGB") {
        result.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB;
    } else if (multiply == "RGBA") {
        result.channels = kEffectChannelR | kEffectChannelG | kEffectChannelB | kEffectChannelA;
    } else if (multiply == "R") {
        result.channels = kEffectChannelR;
    } else if (multiply == "G") {
        result.channels = kEffectChannelG;
    } else if (multiply == "B") {
        result.channels = kEffectChannelB;
    } else if (multiply == "Alpha") {
        result.channels = kEffectChannelA;
    } else if (multiply == "None") {
        result.channels = 0U;
    } else {
        failNode(node, "parameter 'multiply' must be one of RGB, RGBA, R, G, B, Alpha, None, got '" + multiply + "'");
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
