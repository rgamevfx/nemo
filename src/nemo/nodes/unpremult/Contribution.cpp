#include "nemo/nodes/Builtins.hpp"
#include "nemo/nodes/unpremult/Parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor unpremultDescriptor() {
    return NodeDescriptor{.type = "unpremult",
                          .displayName = "Unpremult",
                          .group = "Channels",
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "in", false}},
                          .outputs = {{PortKind::Image, "out"}},
                          .parameters = {{.name = "divide",
                                          .type = ParameterType::Choice,
                                          .defaultValue = ParameterValue{ChoiceValue{"RGB"}},
                                          .choices = {"RGB", "RGBA", "R", "G", "B", "Alpha", "None"},
                                          .label = "Divide",
                                          .section = "Unpremult",
                                          .editor = {}},
                                         {.name = "by",
                                          .type = ParameterType::Choice,
                                          .defaultValue = ParameterValue{ChoiceValue{"Alpha"}},
                                          .choices = {"R", "G", "B", "Alpha"},
                                          .label = "By",
                                          .section = "Unpremult",
                                          .editor = {}}},
                          .capabilities = builtinCapabilities()};
}

constexpr std::array<std::uint32_t, kImageChannels> kChannelBits{kEffectChannelR, kEffectChannelG, kEffectChannelB,
                                                                 kEffectChannelA};

[[nodiscard]] bool selectedRoleExists(const std::array<int, 4>& roles, std::uint32_t selected) {
    for (std::size_t role = 0; role < roles.size(); ++role) {
        if ((selected & kChannelBits[role]) != 0U && roles[role] >= 0)
            return true;
    }
    return false;
}

void requireDivisor(const NodeInstance& node, const std::vector<std::string>& channels,
                    const UnpremultParameters& params) {
    const auto roles = rgbaChannelIndices(channels);
    if (selectedRoleExists(roles, params.channels) && roles[static_cast<std::size_t>(params.byRole)] < 0) {
        failNode(node, "parameter 'by' selects a channel the input does not carry");
    }
}

std::vector<InputRequirement> unpremultInputRequirements(const NodeRegionContext& context) {
    std::vector<InputRequirement> requirements(1);
    if (context.inputs.empty() || context.inputs[0] == nullptr)
        failNode(context.node, "unpremult requires a connected image input");

    const UnpremultParameters params = effectiveUnpremult(context.catalog, context.node, context.effectiveParams);
    const auto roles = rgbaChannelIndices(context.inputs[0]->channels);
    const auto& demand = context.request.channels.empty() ? context.description.channels : context.request.channels;
    bool changesDemand = false;
    for (const std::string& channel : demand) {
        for (std::size_t role = 0; role < roles.size(); ++role) {
            if ((params.channels & kChannelBits[role]) != 0U && roles[role] >= 0 &&
                channel == context.inputs[0]->channels[static_cast<std::size_t>(roles[role])]) {
                changesDemand = true;
                break;
            }
        }
    }
    if (!changesDemand)
        return requirements;

    requireDivisor(context.node, context.inputs[0]->channels, params);
    requirements[0].channels = demand;
    const std::string& divisor = context.inputs[0]->channels[static_cast<std::size_t>(roles[params.byRole])];
    if (!hasChannel(requirements[0].channels, divisor))
        requirements[0].channels.push_back(divisor);
    return requirements;
}

CpuImage executeUnpremult(const CpuNodeContext& context) {
    const CpuImage& input = requiredImageInput(context, 0, "unpremult requires a connected image input");
    const UnpremultParameters params = effectiveUnpremult(context.catalog, context.node, context.effectiveParams);
    const auto roles = rgbaChannelIndices(input.layout().channels);
    requireDivisor(context.node, input.layout().channels, params);

    CpuImage output(effectRasterLayout(context));
    if (input.width() <= 0 || input.height() <= 0)
        return output;

    const InputAnchor anchor = anchorInput(context, 0, input);
    const std::size_t channelCount = output.channelCount();
    std::vector<int> sourceIndices(channelCount, -1);
    std::vector<int> roleAt(channelCount, -1);
    for (std::size_t index = 0; index < channelCount; ++index) {
        sourceIndices[index] = channelIndex(input.layout().channels, output.layout().channels[index]);
        for (std::size_t role = 0; role < roles.size(); ++role) {
            if (roles[role] == sourceIndices[index]) {
                roleAt[index] = static_cast<int>(role);
                break;
            }
        }
    }

    const std::size_t inputChannels = input.channelCount();
    for (int y = 0; y < output.height(); ++y) {
        const std::size_t sourceRow =
            (static_cast<std::size_t>(anchor.offsetY + y) * static_cast<std::size_t>(input.width()) +
             static_cast<std::size_t>(anchor.offsetX)) *
            inputChannels;
        const float* source = input.data() + sourceRow;
        float* destination =
            output.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(output.width()) * channelCount;
        for (int x = 0; x < output.width(); ++x) {
            const float divisor = source[static_cast<std::size_t>(roles[params.byRole])];
            for (std::size_t index = 0; index < channelCount; ++index) {
                const int from = sourceIndices[index];
                const float value = from >= 0 ? source[static_cast<std::size_t>(from)] : 0.0F;
                const int role = roleAt[index];
                destination[index] = role >= 0 && (params.channels & kChannelBits[static_cast<std::size_t>(role)]) != 0U
                                         ? (divisor == 0.0F ? 0.0F : value / divisor)
                                         : value;
            }
            source += inputChannels;
            destination += channelCount;
        }
    }
    return output;
}

std::optional<std::string> validateUnpremultParameters(const NodeCatalog& catalog, const NodeInstance& node,
                                                       const ParameterValues& effectiveParams) {
    return authoringAdmissibility([&] { static_cast<void>(effectiveUnpremult(catalog, node, effectiveParams)); });
}

}  // namespace

NodeContribution unpremultContribution() {
    NodeContribution contribution;
    contribution.descriptor = unpremultDescriptor();
    contribution.role = NodeRole::Image;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeUnpremult};
    contribution.validateParameters = &validateUnpremultParameters;
    contribution.inputRequirements = &unpremultInputRequirements;
    return contribution;
}

}  // namespace nemo::nodes
