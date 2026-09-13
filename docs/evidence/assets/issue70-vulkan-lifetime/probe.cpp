// Standalone public Vulkan lifetime reproduction for issue #70; links no Nemo code.
//
// Usage: probe [instance_cycles=1] [enumerations_per_instance=1]
//
// Exit codes: 0 clean, 1 leak reported by the sanitizer, 2 invalid usage,
// 3 Vulkan environment failure, 4 unexpected enumeration result.
#include <vulkan/vulkan.h>

#include <charconv>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    unsigned cycles = 1;
    unsigned enumerations = 1;
    if (argc > 3) {
        std::fprintf(stderr, "usage: %s [instance_cycles=1] [enumerations_per_instance=1]\n", argv[0]);
        return 2;
    }
    for (int arg = 1; arg < argc; ++arg) {
        unsigned& value = arg == 1 ? cycles : enumerations;
        const char* end = argv[arg] + std::strlen(argv[arg]);
        const auto parsed = std::from_chars(argv[arg], end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            std::fprintf(stderr, "argument %s is not a non-negative integer\n", argv[arg]);
            return 2;
        }
    }
    if (cycles == 0) {
        std::fprintf(stderr, "instance_cycles must be at least 1\n");
        return 2;
    }
    unsigned initialCount = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        VkInstance instance{};
        const VkResult created = vkCreateInstance(&info, nullptr, &instance);
        if (created != VK_SUCCESS) {
            std::fprintf(stderr, "cycle %u: vkCreateInstance failed with VkResult %d; no usable Vulkan ICD\n", cycle,
                         created);
            return 3;
        }
        for (unsigned enumeration = 0; enumeration < enumerations; ++enumeration) {
            unsigned count = 0;
            const VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
            std::printf("cycle=%u enumeration=%u result=%d devices=%u\n", cycle, enumeration, result, count);
            std::fflush(stdout);
            if (result != VK_SUCCESS || count == 0) {
                std::fprintf(stderr, "cycle %u enumeration %u: result=%d devices=%u\n", cycle, enumeration, result,
                             count);
                vkDestroyInstance(instance, nullptr);
                return 4;
            }
            if (initialCount == 0)
                initialCount = count;
            if (count != initialCount) {
                std::fprintf(stderr, "Device count changed from %u to %u; GPU coverage was lost.\n", initialCount,
                             count);
                vkDestroyInstance(instance, nullptr);
                return 4;
            }
        }
        vkDestroyInstance(instance, nullptr);
    }
}
