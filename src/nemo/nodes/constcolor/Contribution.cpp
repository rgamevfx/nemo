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
                          .implementationVersion = 1,
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
// fills the requested raster, carrying its owning network's pixel aspect.
CpuImage executeConstcolor(const CpuNodeContext& context) {
    const std::array<float, 4> color = effectiveColor4(context.catalog, context.node, context.effectiveParams, "color");
    CpuImage output(generatorRasterLayout(context));
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
