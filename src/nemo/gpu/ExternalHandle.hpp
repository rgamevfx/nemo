#pragma once

// Private platform transport; no operating-system handles escape the GPU module.
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan_win32.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nemo::gpu::detail {
#if defined(_WIN32)
inline constexpr auto memoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
inline constexpr auto semaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
using NativeValue = HANDLE;
#else
inline constexpr auto memoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
inline constexpr auto semaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
using NativeValue = int;
#endif

class NativeHandle {
public:
    explicit NativeHandle(NativeValue value) : value_(value) {}
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    ~NativeHandle() {
#if defined(_WIN32)
        if (value_ != nullptr)
            CloseHandle(value_);
#else
        if (value_ >= 0)
            close(value_);
#endif
    }
    [[nodiscard]] NativeValue get() const { return value_; }
    // Successful opaque-FD imports consume the FD; Win32 imports take their
    // own reference and leave this handle owned by the application.
    void imported() {
#if !defined(_WIN32)
        value_ = -1;
#endif
    }

private:
    NativeValue value_;
};
}  // namespace nemo::gpu::detail
