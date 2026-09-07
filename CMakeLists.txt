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
option(NEMO_ENABLE_IPO "Enable interprocedural optimization in release builds" ON)
option(NEMO_ENABLE_CLANG_TIDY "Run clang-tidy during compilation when found" OFF)
option(NEMO_WARN_AS_ERROR "Treat compiler warnings as errors" OFF)
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

add_subdirectory(src/nemo)

if(NEMO_BUILD_CLI)
    add_subdirectory(apps/nemo-cli)
endif()

if(NEMO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
