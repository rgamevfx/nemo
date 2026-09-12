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
option(NEMO_ENABLE_CLANG_TIDY
    "Run the pinned clang-tidy 18 analysis gate on the core module" OFF)
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

# Focused static analysis (issue #67): clang-tidy 18 gates the core module and
# its headers only. The selected check set and the warning-as-error policy live
# in the root .clang-tidy, which the launcher passes explicitly so analysis
# never depends on the build directory or on a launcher-side override.
# Analysis is fail-closed: an explicitly requested run with a missing,
# unrunnable, wrong-version, or unusable configuration fails configure by name
# instead of silently reporting success without analyzing.
if(NEMO_ENABLE_CLANG_TIDY)
    # Pinned major. A plain (non-cache) variable, so -D cannot bypass it and
    # analysis can never run a different check set than the one the
    # contributor guide and Linux CI record.
    set(NEMO_CLANG_TIDY_REQUIRED_MAJOR 18)

    # An explicit -DCLANG_TIDY_PROGRAM (used to exercise the negative checks)
    # wins; otherwise resolve the versioned executable, never a bare
    # clang-tidy whose version could differ from the pin.
    if(NOT CLANG_TIDY_PROGRAM)
        find_program(CLANG_TIDY_PROGRAM NAMES clang-tidy-${NEMO_CLANG_TIDY_REQUIRED_MAJOR})
    endif()
    if(NOT CLANG_TIDY_PROGRAM)
        message(FATAL_ERROR
            "NEMO_ENABLE_CLANG_TIDY=ON requires clang-tidy "
            "${NEMO_CLANG_TIDY_REQUIRED_MAJOR}, but clang-tidy-"
            "${NEMO_CLANG_TIDY_REQUIRED_MAJOR} was not found on PATH. "
            "Install it (Ubuntu 24.04: apt install clang-tidy-18) or pass "
            "-DCLANG_TIDY_PROGRAM=/path/to/clang-tidy-18.")
    endif()

    # Confirm the executable runs and reports the pinned major version.
    execute_process(
        COMMAND "${CLANG_TIDY_PROGRAM}" --version
        OUTPUT_VARIABLE _nemo_clang_tidy_version_output
        ERROR_VARIABLE _nemo_clang_tidy_version_error
        RESULT_VARIABLE _nemo_clang_tidy_version_status)
    if(NOT _nemo_clang_tidy_version_status EQUAL 0)
        message(FATAL_ERROR
            "NEMO_ENABLE_CLANG_TIDY=ON: the clang-tidy executable "
            "'${CLANG_TIDY_PROGRAM}' cannot run, so clang-tidy "
            "${NEMO_CLANG_TIDY_REQUIRED_MAJOR} analysis cannot be guaranteed: "
            "${_nemo_clang_tidy_version_error}")
    endif()
    string(REGEX MATCH "version ([0-9]+)" _nemo_clang_tidy_version_match
        "${_nemo_clang_tidy_version_output}")
    if(NOT _nemo_clang_tidy_version_match OR
       NOT CMAKE_MATCH_1 EQUAL NEMO_CLANG_TIDY_REQUIRED_MAJOR)
        message(FATAL_ERROR
            "NEMO_ENABLE_CLANG_TIDY=ON requires clang-tidy "
            "${NEMO_CLANG_TIDY_REQUIRED_MAJOR}, but '${CLANG_TIDY_PROGRAM}' "
            "reports '${_nemo_clang_tidy_version_output}'. Install "
            "clang-tidy-${NEMO_CLANG_TIDY_REQUIRED_MAJOR} or point "
            "-DCLANG_TIDY_PROGRAM at a matching executable.")
    endif()

    # Reject an unusable or empty selected configuration as far as the tool
    # can tell: --verify-config fails on a missing/malformed file or an
    # unknown check, and --list-checks fails when the selected set enables no
    # checks. No analysis is run here; this only validates the configuration.
    set(_nemo_clang_tidy_config "${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy")
    execute_process(
        COMMAND "${CLANG_TIDY_PROGRAM}"
                "--config-file=${_nemo_clang_tidy_config}" --verify-config
        OUTPUT_VARIABLE _nemo_clang_tidy_verify_output
        ERROR_VARIABLE _nemo_clang_tidy_verify_error
        RESULT_VARIABLE _nemo_clang_tidy_verify_status)
    if(NOT _nemo_clang_tidy_verify_status EQUAL 0)
        message(FATAL_ERROR
            "NEMO_ENABLE_CLANG_TIDY=ON: '${_nemo_clang_tidy_config}' is not a "
            "usable clang-tidy configuration: "
            "${_nemo_clang_tidy_verify_error}${_nemo_clang_tidy_verify_output}")
    endif()
    execute_process(
        COMMAND "${CLANG_TIDY_PROGRAM}"
                "--config-file=${_nemo_clang_tidy_config}" --list-checks
        OUTPUT_VARIABLE _nemo_clang_tidy_list_output
        ERROR_VARIABLE _nemo_clang_tidy_list_error
        RESULT_VARIABLE _nemo_clang_tidy_list_status)
    if(NOT _nemo_clang_tidy_list_status EQUAL 0)
        message(FATAL_ERROR
            "NEMO_ENABLE_CLANG_TIDY=ON: '${_nemo_clang_tidy_config}' enables "
            "no checks: "
            "${_nemo_clang_tidy_list_error}${_nemo_clang_tidy_list_output}")
    endif()

    # Consumed by the core target's CXX_CLANG_TIDY property only; no
    # directory-scope CMAKE_CXX_CLANG_TIDY is set, so analysis cannot leak
    # into the gpu/media/cli/ui/test targets.
    set(NEMO_CLANG_TIDY_LAUNCHER
        "${CLANG_TIDY_PROGRAM};--config-file=${_nemo_clang_tidy_config}")
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
