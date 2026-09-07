#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#include "nemo/gpu/Error.hpp"

namespace nemo::gpu {

struct DebugMessage {
    VkDebugUtilsMessageSeverityFlagBitsEXT severity;
    std::string text;
};

struct InstanceConfig {
    // Enable VK_LAYER_KHRONOS_validation plus a debug messenger when the
    // layer is installed. Layer absence is not an error: bootstrap runs
    // without validation and reports validation_enabled() == false.
    bool validation = true;
};

// Owns the VkInstance and, when validation is available, the debug messenger
// that collects validation output for tests and diagnostics.
//
// Threading contract: validation callbacks can fire from any Vulkan thread, so
// take_debug_messages() is safe from any thread; all other members are
// main/bootstrap-thread only.
class Instance {
public:
    // Pass-key construction: `Token` is a private type, so callers outside
    // this header cannot construct an Instance without going through
    // create(); the constructor itself is public because std::make_unique
    // constructs inside create().
    struct Token;
    explicit Instance(Token);
    ~Instance();

    static std::unique_ptr<Instance> create(const InstanceConfig& config = {});

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    [[nodiscard]] VkInstance handle() const { return instance_; }
    [[nodiscard]] bool validation_enabled() const { return messenger_ != VK_NULL_HANDLE; }

    // Drains collected validation messages (info, warnings, and errors).
    [[nodiscard]] std::vector<DebugMessage> take_debug_messages();

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT type,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                        void* user_data);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::mutex messages_mutex_;
    std::vector<DebugMessage> messages_;
};

}  // namespace nemo::gpu
