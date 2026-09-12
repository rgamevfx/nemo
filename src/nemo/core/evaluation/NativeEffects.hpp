#pragma once

#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"

namespace nemo {

struct NodeInstance;

// CPU reference implementation of the native Grade/Blur/Transform effect
// family (issue #34). `input` is the raster the executor produced for
// `request` (the requested region at the request's sampling scale) and
// `mask` is null when the optional mask port is absent.
//
// Parameter values are resolved and validated through the shared metadata
// helpers in Params.hpp; the pixel math below is an independent CPU
// reference implementation and shares no shader code (ADR-0004). The result
// keeps the input layout and straight (non-premultiplied) RGBA storage.
//
// Throws EvaluationException identifying `node` for unknown node types,
// invalid/unrepresentable effect coefficients, or a mask raster that does
// not match the input raster.
[[nodiscard]] CpuImage evaluateNativeEffect(const NodeCatalog& catalog, const NodeInstance& node,
                                            ParameterValues& effectiveParams, const EvaluationRequest& request,
                                            const CpuImage& input, const CpuImage* mask);

}  // namespace nemo
