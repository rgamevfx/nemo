#include "nemo/nodes/Builtins.hpp"

#include <algorithm>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor testPatternDescriptor() {
    return NodeDescriptor{.type = "testpattern",
                          .displayName = "Test Pattern",
                          .group = "Generators",
                          .implementationVersion = 2,
                          .inputs = {},
                          .outputs = {{PortKind::Image, "color"}},
                          .parameters = {},
                          .capabilities = builtinCapabilities(true)};
}

// Deterministic reference pattern: horizontal red gradient, vertical green
// gradient, and a blue bar whose position tracks local time. Any change here is
// an observable image change.
//
// Sample the full-resolution image domain, not the ROI's dimensions. Cropping
// and reduced sampling never re-normalize the generator.
CpuImage executeTestPattern(const CpuNodeContext& context) {
    const EvaluationRequest& request = context.request;
    CpuImage output(effectRasterLayout(request));
    const int scale = request.samplingScale;
    const int width = output.width();
    const int height = output.height();
    const int fullX = request.region.x;
    const int fullY = request.region.y;
    const int fullWidth = request.imageWidth();
    const int fullHeight = request.imageHeight();
    const int barWidth = std::max(2, fullWidth / 16);
    const int barPos = static_cast<int>((request.localTime * (fullWidth / 8)) % (fullWidth + barWidth));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int fullPixelX = fullX + x * scale;
            const int fullPixelY = fullY + y * scale;
            const double u = fullWidth > 1 ? static_cast<double>(fullPixelX) / (fullWidth - 1) : 0.0;
            const double v = fullHeight > 1 ? static_cast<double>(fullPixelY) / (fullHeight - 1) : 0.0;
            const bool inBar = fullPixelX >= barPos && fullPixelX < barPos + barWidth;
            output.setPixel(x, y, {static_cast<float>(u), static_cast<float>(v), inBar ? 1.0F : 0.0F, 1.0F});
        }
    }
    return output;
}

}  // namespace

NodeContribution testpatternContribution() {
    NodeContribution contribution;
    contribution.descriptor = testPatternDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeTestPattern};
    return contribution;
}

}  // namespace nemo::nodes
