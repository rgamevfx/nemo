#pragma once

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {
struct TransformParameters {
    float translateX{0.0F};
    float translateY{0.0F};
    float scale{1.0F};
    float rotate{0.0F};
    // 0 Cubic, 1 Linear, 2 Nearest (a plain int, not an enum, per contract).
    int filter{0};
};

[[nodiscard]] inline TransformParameters effectiveTransform(const NodeCatalog& catalog, const NodeInstance& node,
                                                            const ParameterValues& effectiveParams) {
    TransformParameters transform;
    transform.translateX = effectiveNumber(catalog, node, effectiveParams, "translateX");
    transform.translateY = effectiveNumber(catalog, node, effectiveParams, "translateY");
    transform.scale = effectiveNumber(catalog, node, effectiveParams, "scale");
    // Positive finite scale with a finite reciprocal: the archive's 0.1..3 was
    // a useful slider range, not an equation limit, so typed scale 4 or 0.05
    // stay usable while zero, negatives and reciprocals that overflow remain
    // inadmissible.
    if (!(transform.scale > 0.0F) || !std::isfinite(1.0F / transform.scale)) {
        failNode(node, "parameter 'scale' must be positive and finite with a finite reciprocal");
    }
    transform.rotate = effectiveNumber(catalog, node, effectiveParams, "rotate");
    const std::string& filter = effectiveChoice(catalog, node, effectiveParams, "filter");
    if (filter == "Cubic") {
        transform.filter = 0;
    } else if (filter == "Linear") {
        transform.filter = 1;
    } else if (filter == "Nearest") {
        transform.filter = 2;
    } else {
        failNode(node, "parameter 'filter' must be one of Cubic, Linear, Nearest, got '" + filter + "'");
    }
    return transform;
}
}  // namespace nemo
