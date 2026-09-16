#include "nemo/nodes/Builtins.hpp"

#include <array>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor constColorDescriptor() {
    return NodeDescriptor{.type = "constcolor",
                          .displayName = "Constant Color",
                          .group = "Generators",
                          // 2: the raster carries the owning network's described
                          // canvas format instead of storage defaults
                          // (issue #88).
                          .implementationVersion = 2,
                          .inputs = {},
                          .outputs = {{PortKind::Image, "color"}},
                          .parameters = {{.name = "color",
                                          .type = ParameterType::Color,
                                          .defaultValue = ParameterValue{ColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}},
                                          .label = "Color",
                                          .section = "Color",
                                          .editor = {}}},
                          .capabilities = builtinCapabilities()};
}

// The catalog declares the one color parameter, so an authored value is already
// typed and bounded; execution resolves it through the shared metadata seam and
// fills the requested raster. The raster's owning network's authored canvas
// format is that node's inherited description (issue #88, #96), so the generator
// never rediscovers format ownership.
CpuImage executeConstcolor(const CpuNodeContext& context) {
    const std::array<float, 4> color = effectiveColor4(context.catalog, context.node, context.effectiveParams, "color");
    CpuImage output(effectRasterLayout(context));
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            output.setPixel(x, y, color);
        }
    }
    return output;
}

}  // namespace

NodeContribution constcolorContribution() {
    NodeContribution contribution;
    contribution.descriptor = constColorDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeConstcolor};
    return contribution;
}

}  // namespace nemo::nodes
