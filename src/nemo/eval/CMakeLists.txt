# Evaluation's native GPU executor (issue #8, spec sections 10.3-10.4):
# consumes the shared evaluation plan contract and executes it through the
# nemo::gpu primitives. Core stays GPU-free; this module is the optional
# native path, built only when the GPU module is available.
add_library(nemo_eval
    GpuExecutor.cpp
)
add_library(nemo::eval ALIAS nemo_eval)

target_include_directories(nemo_eval PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_eval PUBLIC nemo::core nemo::gpu)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_eval PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_eval PRIVATE -Werror)
    endif()
endif()

if(NEMO_ENABLE_IPO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET nemo_eval PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()
