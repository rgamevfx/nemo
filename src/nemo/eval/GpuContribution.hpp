#pragma once

#include "nemo/core/evaluation/NodeContributions.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace nemo::eval {

// Internal binding contract v2. Only coordinate/time facts are common to
// effects. Node-local payloads have their own layout at set 0, binding 1.
struct EffectRequestUniforms {
    std::uint32_t meta[4]{};
    std::uint32_t meta2[4]{};
    float misc[4]{};
};
static_assert(sizeof(EffectRequestUniforms) == 48);

enum class EffectImageKind { Input, Scratch, External, Output };
struct EffectImageRef {
    EffectImageKind kind{EffectImageKind::Input};
    std::uint32_t index{};
    friend bool operator==(const EffectImageRef&, const EffectImageRef&) = default;
};

// Local pass names never enter the global node inventory. Inputs bind in
// vector order at set 1; the result binds at set 2, binding 0. Scratch images
// have the request's raster/layout. Only a Source role can read External 0.
struct EffectPassDefinition {
    std::string id;
    std::string shader;
    std::string glsl;
    std::vector<EffectImageRef> inputs;
    EffectImageRef output{EffectImageKind::Output, 0};
    bool weights{false};
};

struct GpuPreparation {
    std::vector<std::byte> payload;
    std::vector<float> weights;
    std::vector<std::uint32_t> passes;
};

struct GpuNodeContext {
    const NodeCatalog& catalog;
    const NodeInstance& node;
    const EvaluationRequest& request;
    ParameterValues& effectiveParams;
    bool maskPresent;
    float pixelAspect;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
};

// Preparation is worker-side, reentrant value computation: no device,
// allocator, queue, callbacks into the UI, or borrowed resources. The shared
// executor owns upload, allocation, recording, submission and retirement.
// Version must match the schema; changing code/layout requires a new version.
// Captures must be immutable or synchronized; context references are valid
// only during preparation, and returned payload/weight values are owned.
struct GpuImplementation {
    std::uint64_t version{1};
    std::string payloadLayout;
    std::size_t payloadSize{};
    std::vector<EffectPassDefinition> passes;
    std::function<GpuPreparation(const GpuNodeContext&)> prepare;
};

struct GpuNodeContribution {
    NodeContribution node;
    std::optional<GpuImplementation> gpu;
    std::string gpuUnavailableReason;
};

template <typename T>
[[nodiscard]] std::vector<std::byte> effectPayload(const T& value) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 16 == 0);
    std::vector<std::byte> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

[[nodiscard]] std::vector<GpuNodeContribution> builtinGpuContributions();

}  // namespace nemo::eval
