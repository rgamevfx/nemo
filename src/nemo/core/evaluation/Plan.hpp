#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Ids.hpp"
#include "nemo/core/document/ParameterValue.hpp"
#include "nemo/core/document/ParameterValueJson.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

// Where an image result lives. The CPU reference produces HostCpuReference;
// the native path produces GpuDevice under the GPU module's explicit
// lifetime and completion rules (spec sections 10.3-10.4, ADR-0004).
// Nothing in the plan assumes host memory: consumers address images through
// identity plus residency, never through CPU pixel buffers.
enum class Residency { HostCpuReference, GpuDevice };

[[nodiscard]] inline const char* residencyName(Residency residency) {
    return residency == Residency::GpuDevice ? "gpu-device" : "host-cpu-reference";
}

// Content-addressed identity of a produced image: layout plus a hash of the
// pixel values. Every request/result carries identity so stale work is
// detectable and can never overwrite a newer result (spec section 10.2).
struct ImageIdentity {
    std::uint64_t contentHash{0};
    ImageLayout layout;
    Residency residency{Residency::HostCpuReference};
};

[[nodiscard]] inline nlohmann::json imageIdentityToJson(const ImageIdentity& identity) {
    nlohmann::json json;
    json["contentHash"] = [](std::uint64_t hash) {
        static const char* digits = "0123456789abcdef";
        std::string hex(16, '0');
        for (int i = 15; i >= 0; --i) {
            hex[static_cast<std::size_t>(i)] = digits[hash & 0xF];
            hash >>= 4;
        }
        return hex;
    }(identity.contentHash);
    json["width"] = identity.layout.width;
    json["height"] = identity.layout.height;
    json["pixelAspect"] = identity.layout.pixelAspect;
    json["channels"] = identity.layout.channels;
    json["precision"] = "float32";
    // Report what the image actually is: a Raw/Data source result is not
    // managed scene-linear, and a display-referred viewer result is not either
    // (issue #81).
    json["color"] = colorInterpretationName(identity.layout.color);
    json["residency"] = residencyName(identity.residency);
    return json;
}

struct ScopedPlanInput {
    NetworkId network{kInvalidNetwork};
    NetworkInstanceId instance{kInvalidNetworkInstance};
    NodeId node{kInvalidNode};
    std::uint32_t outputPort{std::numeric_limits<std::uint32_t>::max()};
    std::vector<NetworkInstanceId> path;
};

// One scheduled node: its identity, effective parameter/input state, the
// images it consumes, and the image identity it produces. Inputs are in
// declared port order; an absent optional input keeps its slot as the invalid
// sentinel (`kInvalidNode` / default `ScopedPlanInput` / zero `ImageIdentity`)
// and contributes no dependency. The dependency set of a step is exactly the
// non-sentinel entries of `inputs`.
struct PlanStep {
    NetworkId network{kInvalidNetwork};
    NetworkInstanceId instance{kInvalidNetworkInstance};
    NodeId node{kInvalidNode};
    std::uint32_t outputPort{std::numeric_limits<std::uint32_t>::max()};
    std::vector<NetworkInstanceId> path;
    std::string type;
    std::string name;
    ParameterValues effectiveParams;
    std::vector<NodeId> inputs;
    std::vector<ScopedPlanInput> scopedInputs;
    std::vector<ImageIdentity> inputImages;
    ImageIdentity produced;
    // The described meaning of the image this step produced (issue #88): the
    // node's resolved description, which is what its consumers read it as and
    // what its reuse key carries. Independent of `region`, which reports the
    // rectangle this step actually rastered.
    ImageDescription description;
    // True when this step's result was reused from the evaluator result
    // cache instead of recomputed (issue #9 plan evidence).
    bool cacheReused{false};
    // Actual full-resolution coverage of this step's raster (issue #85). It is
    // the region the executor really produced: the planned demand, escalated to
    // the node's whole useful domain (its described format unioned with its data
    // window) for a whole-frame-only contribution, or a larger resident
    // rectangle served from the cache. A consumer reads this step's
    // image through this coverage, never by assuming the raster starts at the
    // consumer's own origin.
    // GPU output steps include the final device crop when needed. CPU backing
    // steps remain unchanged by delivery; plan.result and plan.request describe
    // its final consumer raster.
    Region region;
};

// The executable plan: what the evaluator scheduled, in execution order
// (dependencies first). This is the GPU-ready contract (spec section 10.3,
// ADR-0004): a native executor consumes the same plan and records GpuDevice
// residency instead of running CPU reference kernels.
struct EvaluationPlan {
    EvaluationRequest request;
    std::vector<PlanStep> steps;
    ImageIdentity result;
    // The described meaning of the target this plan produced (issue #88): the
    // image the caller asked for, as the dependency graph describes it. Its
    // format is the requested raster's logical format, independently of the
    // requested region and sampling scale.
    ImageDescription description;
};

[[nodiscard]] inline nlohmann::json regionToJson(const Region& region) {
    return {{"x", region.x}, {"y", region.y}, {"width", region.width}, {"height", region.height}};
}

[[nodiscard]] inline nlohmann::json imageDescriptionToJson(const ImageDescription& description) {
    nlohmann::json json;
    json["format"] = regionToJson(description.format);
    json["dataBounds"] = regionToJson(description.dataBounds);
    json["pixelAspect"] = description.pixelAspect;
    json["channels"] = description.channels;
    json["precision"] = "float32";
    json["association"] = imageAssociationName(description.association);
    json["color"] = colorInterpretationName(description.color);
    return json;
}

[[nodiscard]] inline nlohmann::json planToJson(const EvaluationPlan& plan) {
    nlohmann::json steps = nlohmann::json::array();
    for (const auto& step : plan.steps) {
        nlohmann::json json;
        json["instance"] = step.instance;
        json["outputPort"] = step.outputPort;
        json["network"] = step.network;
        json["node"] = step.node;
        json["path"] = step.path;
        nlohmann::json scopedInputs = nlohmann::json::array();
        for (const auto& input : step.scopedInputs)
            scopedInputs.push_back({{"network", input.network},
                                    {"instance", input.instance},
                                    {"node", input.node},
                                    {"outputPort", input.outputPort},
                                    {"path", input.path}});
        json["scopedInputs"] = std::move(scopedInputs);
        json["name"] = step.name;
        nlohmann::json effectiveParams = nlohmann::json::object();
        for (const auto& [key, value] : step.effectiveParams)
            effectiveParams[key] = parameterValueToJson(value);
        json["effectiveParams"] = std::move(effectiveParams);
        json["inputs"] = step.inputs;
        nlohmann::json inputImages = nlohmann::json::array();
        for (const auto& image : step.inputImages) {
            inputImages.push_back(imageIdentityToJson(image));
        }
        json["inputImages"] = std::move(inputImages);
        json["produced"] = imageIdentityToJson(step.produced);
        json["description"] = imageDescriptionToJson(step.description);
        json["reused"] = step.cacheReused;
        json["region"] = regionToJson(step.region);
        steps.push_back(std::move(json));
    }
    nlohmann::json json;
    json["request"] = {{"network", plan.request.network},
                       {"output", plan.request.output},
                       {"localTime", plan.request.localTime},
                       {"region", regionToJson(plan.request.region)},
                       {"channels", plan.request.channels},
                       {"quality", qualityName(plan.request.quality)},
                       {"samplingScale", plan.request.samplingScale}};
    json["request"]["fullWidth"] = plan.request.imageWidth();
    json["request"]["fullHeight"] = plan.request.imageHeight();
    json["description"] = imageDescriptionToJson(plan.description);
    json["steps"] = std::move(steps);
    json["result"] = imageIdentityToJson(plan.result);
    return json;
}

}  // namespace nemo
