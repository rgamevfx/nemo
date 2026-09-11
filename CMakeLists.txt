cmake_minimum_required(VERSION 3.28)

project(nemo
    VERSION 0.1.0
    DESCRIPTION "Unified VFX compositor and NLE"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NEMO_BUILD_TESTS "Build core unit tests" ON)
option(NEMO_BUILD_CLI "Build the nemo-cli headless tool" ON)
option(NEMO_BUILD_UI "Build the Qt Quick workspace shell" OFF)
option(NEMO_BUILD_GPU "Build the nemo::gpu Vulkan module" ON)
option(NEMO_ENABLE_IPO "Enable interprocedural optimization in release builds" ON)
option(NEMO_ENABLE_CLANG_TIDY "Run clang-tidy during compilation when found" OFF)
option(NEMO_WARN_AS_ERROR "Treat compiler warnings as errors" OFF)
option(NEMO_TEST_FORBIDDEN_DEPENDENCY
    "Inject a forbidden core/workspace edge to exercise dependency checks" OFF)
mark_as_advanced(NEMO_TEST_FORBIDDEN_DEPENDENCY)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NemoSlangShaders)
include(NemoDependencyChecks)


# Sanitizers, e.g. "address;undefined" (preset asan) or "thread" (preset tsan).
set(NEMO_SANITIZER "" CACHE STRING "Enable the listed sanitizers")

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if(NEMO_SANITIZER)
    string(REPLACE ";" "," _nemo_sanitizer_flags "${NEMO_SANITIZER}")
    add_compile_options(-fsanitize=${_nemo_sanitizer_flags} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_nemo_sanitizer_flags})
endif()

if(NEMO_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_PROGRAM clang-tidy)
    if(CLANG_TIDY_PROGRAM)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_PROGRAM};--warnings-as-errors=*")
    endif()
endif()

if(NEMO_BUILD_GPU)
    # Vulkan is a system package (ADR-0002); VulkanMemoryAllocator is a pinned
    # vcpkg manifest entry. Missing either disables the GPU module at configure
    # time so machines and CI without Vulkan still build the rest of the tree.
    find_package(Vulkan QUIET)
    find_package(VulkanMemoryAllocator QUIET CONFIG)
    if(NOT Vulkan_FOUND OR NOT VulkanMemoryAllocator_FOUND)
        message(STATUS "Vulkan headers/loader or VulkanMemoryAllocator not found; building without nemo::gpu (NEMO_BUILD_GPU=OFF)")
        set(NEMO_BUILD_GPU OFF)
    endif()
endif()

add_subdirectory(src/nemo)
add_subdirectory(src/nemo/gpu/shaders)

if(NEMO_BUILD_CLI)
    add_subdirectory(apps/nemo-cli)
endif()

if(NEMO_BUILD_UI OR NEMO_BUILD_TESTS)
    add_subdirectory(apps/nemo-ui)
endif()

if(NEMO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(NEMO_TEST_FORBIDDEN_DEPENDENCY)
    if(NOT TARGET nemo::workspace)
        message(FATAL_ERROR
            "NEMO_TEST_FORBIDDEN_DEPENDENCY requires the workspace target; "
            "configure with NEMO_BUILD_TESTS=ON or NEMO_BUILD_UI=ON")
    endif()
    target_link_libraries(nemo_core PRIVATE nemo::workspace)
endif()

# All project targets are now present. Inspect their actual link interfaces so
# a forbidden edge fails configure with the source, dependency, and path.
nemo_check_dependency_direction()
