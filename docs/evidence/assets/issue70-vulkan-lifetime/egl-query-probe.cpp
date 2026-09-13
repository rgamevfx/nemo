// Minimal public EGL query-only reproduction for issue #70; loads no Vulkan.
//
// Usage: egl-query-probe
//
// Exit codes: 0 clean, 1 leak reported by the sanitizer, 2 invalid usage,
// 3 EGL load/symbol failure, 4 unexpected query result.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdio>
#include <dlfcn.h>

int main(int argc, char** argv) {
    if (argc != 1) {
        std::fprintf(stderr, "usage: %s\n", argv[0]);
        return 2;
    }
    void* library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen(libEGL.so.1) failed: %s\n", dlerror());
        return 3;
    }
    auto getProcAddress = reinterpret_cast<decltype(&eglGetProcAddress)>(dlsym(library, "eglGetProcAddress"));
    if (!getProcAddress) {
        std::fprintf(stderr, "libEGL.so.1 does not export eglGetProcAddress: %s\n", dlerror());
        dlclose(library);
        return 3;
    }
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(getProcAddress("eglQueryDevicesEXT"));
    if (!queryDevices) {
        std::fprintf(stderr, "the loaded EGL vendor does not provide eglQueryDevicesEXT\n");
        dlclose(library);
        return 3;
    }
    EGLint count = 0;
    const EGLBoolean result = queryDevices(0, nullptr, &count);
    std::printf("eglQueryDevicesEXT: result=%u devices=%d\n", result, count);
    std::fflush(stdout);
    const int closeResult = dlclose(library);
    if (closeResult != 0) {
        std::fprintf(stderr, "dlclose(libEGL.so.1) failed: %s\n", dlerror());
        return 3;
    }
    if (result != EGL_TRUE || count <= 0) {
        std::fprintf(stderr, "eglQueryDevicesEXT returned %u with %d devices; no EGL device was enumerated\n", result,
                     count);
        return 4;
    }
}
