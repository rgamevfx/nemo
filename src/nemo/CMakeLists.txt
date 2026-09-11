find_package(nlohmann_json CONFIG REQUIRED)

add_library(nemo_core
    core/document/Graph.cpp
    core/document/Document.cpp
    core/document/Serialization.cpp
    core/session/ProjectSession.cpp
    core/nodes/NodeCatalog.cpp
    core/evaluation/CpuReference.cpp
    core/evaluation/Reuse.cpp
)
add_library(nemo::core ALIAS nemo_core)


target_include_directories(nemo_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_core PUBLIC nlohmann_json::nlohmann_json)

if(NEMO_BUILD_GPU)
    add_library(nemo_gpu
        gpu/Allocator.cpp
        gpu/Compile.cpp
        gpu/ComputePass.cpp
        gpu/GpuViewingTransform.cpp
        gpu/ViewerPresentation.cpp
        gpu/Error.cpp
        gpu/MediaInterop.cpp
        gpu/Instance.cpp
        gpu/Device.cpp
        gpu/Submit.cpp
    )
    add_library(nemo::gpu ALIAS nemo_gpu)
    target_include_directories(nemo_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(nemo_gpu PUBLIC Vulkan::Vulkan)
    # Runtime GLSL -> SPIR-V for OCIO-generated viewing-transform programs
    # (issue #6); Nemo-authored effects stay Slang (issue #3/#8).
    find_package(glslang CONFIG REQUIRED)
    target_link_libraries(nemo_gpu PUBLIC glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
    target_link_libraries(nemo_gpu PRIVATE GPUOpen::VulkanMemoryAllocator)
    # vcpkg builds glslang with -fno-rtti. UBSan's vptr check references
    # typeinfo for glslang::TShader/TProgram, which that build does not
    # provide; the check is meaningless for library classes we cannot
    # rebuild, so it is disabled for this translation unit only.
    set_source_files_properties(gpu/Compile.cpp PROPERTIES COMPILE_OPTIONS
        "$<$<CXX_COMPILER_ID:GNU,Clang>:-fno-sanitize=vptr>")
    add_subdirectory(eval)
 endif()
add_subdirectory(media)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_core PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_BUILD_GPU)
        target_compile_options(nemo_gpu PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_core PRIVATE -Werror)
        if(NEMO_BUILD_GPU)
            target_compile_options(nemo_gpu PRIVATE -Werror)
        endif()
    endif()
endif()

if(NEMO_ENABLE_IPO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET nemo_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()
