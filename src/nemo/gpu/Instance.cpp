#include "nemo/gpu/Instance.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace nemo::gpu {

struct Instance::Token {};
Instance::Instance(Token) {}

namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

VkInstance createInstance(bool validation, const std::vector<std::string>& requestedExtensions) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "nemo";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "nemo";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> enabled_layers;
    std::vector<const char*> enabled_extensions;
    if (validation) {
        enabled_layers.push_back(kValidationLayerName);
        enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    uint32_t extensionCount = 0;
    checkVulkan(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
                "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVulkan(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data()),
                "vkEnumerateInstanceExtensionProperties");
    for (const auto& extension : requestedExtensions) {
        if (std::none_of(available.begin(), available.end(),
                         [&](const auto& item) { return extension == item.extensionName; }))
            throw GpuException(GpuError::InvalidRequest, "Vulkan instance extension unavailable: " + extension);
        if (std::none_of(enabled_extensions.begin(), enabled_extensions.end(),
                         [&](const char* name) { return extension == name; }))
            enabled_extensions.push_back(extension.c_str());
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app;
    create_info.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
    create_info.ppEnabledLayerNames = enabled_layers.data();
    create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
    create_info.ppEnabledExtensionNames = enabled_extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult created = vkCreateInstance(&create_info, nullptr, &instance);
    if (created == VK_ERROR_INCOMPATIBLE_DRIVER) {
        throw GpuException(GpuError::NoDevice, "no usable Vulkan driver installed: vkCreateInstance returned "
                                               "VK_ERROR_INCOMPATIBLE_DRIVER");
    }
    checkVulkan(created, "vkCreateInstance");
    return instance;
}

PFN_vkCreateDebugUtilsMessengerEXT loadCreateMessenger(VkInstance instance) {
    return reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
}

PFN_vkDestroyDebugUtilsMessengerEXT loadDestroyMessenger(VkInstance instance) {
    return reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
}

}  // namespace

std::unique_ptr<Instance> Instance::create(const InstanceConfig& config) {
    bool validation = config.validation;
    if (validation) {
        uint32_t count = 0;
        checkVulkan(vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> layers(count);
        if (count > 0) {
            checkVulkan(vkEnumerateInstanceLayerProperties(&count, layers.data()),
                        "vkEnumerateInstanceLayerProperties");
        }
        validation = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
            return std::strcmp(layer.layerName, kValidationLayerName) == 0;
        });
    }

    auto instance = std::make_unique<Instance>(Instance::Token{});
    instance->instance_ = createInstance(validation, config.extensions);

    if (validation) {
        auto vkCreateDebugUtilsMessengerEXT = loadCreateMessenger(instance->instance_);
        if (!vkCreateDebugUtilsMessengerEXT) {
            throw GpuException(GpuError::LayerUnavailable,
                               "VK_LAYER_KHRONOS_validation is installed but does not provide "
                               "VK_EXT_debug_utils entry points");
        }

        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = &Instance::debugCallback;
        messenger_info.pUserData = instance.get();
        checkVulkan(
            vkCreateDebugUtilsMessengerEXT(instance->instance_, &messenger_info, nullptr, &instance->messenger_),
            "vkCreateDebugUtilsMessengerEXT");
    }
    return instance;
}

Instance::~Instance() {
    if (messenger_ != VK_NULL_HANDLE) {
        // Destroy the messenger before the instance so no callback can fire
        // into this object after its members are gone.
        if (auto vkDestroyDebugUtilsMessengerEXT = loadDestroyMessenger(instance_)) {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

std::vector<DebugMessage> Instance::take_debug_messages() {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    return std::exchange(messages_, {});
}

VKAPI_ATTR VkBool32 VKAPI_CALL Instance::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                       VkDebugUtilsMessageTypeFlagsEXT,
                                                       const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                       void* user_data) {
    auto* self = static_cast<Instance*>(user_data);
    std::lock_guard<std::mutex> lock(self->messages_mutex_);
    self->messages_.push_back({severity, data->pMessage ? data->pMessage : ""});
    return VK_FALSE;
}

}  // namespace nemo::gpu
