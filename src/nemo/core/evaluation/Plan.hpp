#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/document/Ids.hpp"
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
    json["color"] = "scene-linear";
    json["residency"] = residencyName(identity.residency);
    return json;
}

// One scheduled node: its identity, effective parameter/input state, the
// images it consumes, and the image identity it produces. Inputs are in
// declared port order; the dependency set of a step is exactly `inputs`.
struct PlanStep {
    NodeId node{kInvalidNode};
    std::string type;
    std::string name;
    std::map<std::string, std::string> effectiveParams;
    std::vector<NodeId> inputs;
    std::vector<ImageIdentity> inputImages;
    ImageIdentity produced;
};

// The executable plan: what the evaluator scheduled, in execution order
// (dependencies first). This is the GPU-ready contract (spec section 10.3,
// ADR-0004): a native executor consumes the same plan and records GpuDevice
// residency instead of running CPU reference kernels.
struct EvaluationPlan {
    EvaluationRequest request;
    std::vector<PlanStep> steps;
    ImageIdentity result;
};

[[nodiscard]] inline nlohmann::json planToJson(const EvaluationPlan& plan) {
    nlohmann::json steps = nlohmann::json::array();
    for (const auto& step : plan.steps) {
        nlohmann::json json;
        json["node"] = step.node;
        json["type"] = step.type;
        json["name"] = step.name;
        json["effectiveParams"] = step.effectiveParams;
        json["inputs"] = step.inputs;
        nlohmann::json inputImages = nlohmann::json::array();
        for (const auto& image : step.inputImages) {
            inputImages.push_back(imageIdentityToJson(image));
        }
        json["inputImages"] = std::move(inputImages);
        json["produced"] = imageIdentityToJson(step.produced);
        steps.push_back(std::move(json));
    }
    nlohmann::json json;
    json["request"] = {{"output", plan.request.output},
                       {"localTime", plan.request.localTime},
                       {"region",
                        {{"x", plan.request.region.x},
                         {"y", plan.request.region.y},
                         {"width", plan.request.region.width},
                         {"height", plan.request.region.height}}},
                       {"channels", plan.request.channels},
                       {"quality", qualityName(plan.request.quality)}};
    json["steps"] = std::move(steps);
    json["result"] = imageIdentityToJson(plan.result);
    return json;
}

}  // namespace nemo
