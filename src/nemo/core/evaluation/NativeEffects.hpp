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

// CPU reference implementation of Merge (issue #75). Port roles follow the
// declared schema: `background` is port A (base), `foreground` is port B
// (source). `mask` is null when the optional mask port is absent. The
// operation and the shared mask/mix controls are resolved and validated
// through Params.hpp, so this agrees with the GPU executor by construction;
// the pixel math below is an independent reference (ADR-0004).
//
// Straight-alpha contract, op-independent for alpha and Over-preserving for
// RGB: Over keeps the exact opaque/translucent expression the reference has
// always used, the four extended modes interpolate background -> blend target
// by foreground alpha, and the final RGBA result blends the unmasked composite
// over the background by the shared coverage*mix weight. Scene-linear RGB is
// never clamped.
[[nodiscard]] CpuImage evaluateMerge(const NodeCatalog& catalog, const NodeInstance& node,
                                     ParameterValues& effectiveParams, const CpuImage& background,
                                     const CpuImage& foreground, const CpuImage* mask);

}  // namespace nemo
