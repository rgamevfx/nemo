find_package(nlohmann_json CONFIG REQUIRED)

add_library(nemo_core
    core/document/Graph.cpp
    core/document/Document.cpp
    core/document/Serialization.cpp
    core/evaluation/CpuReference.cpp
)
add_library(nemo::core ALIAS nemo_core)
add_subdirectory(media)

target_include_directories(nemo_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_core PUBLIC nlohmann_json::nlohmann_json)

if(NEMO_BUILD_GPU)
    add_library(nemo_gpu
        gpu/Allocator.cpp
        gpu/Error.cpp
        gpu/Instance.cpp
        gpu/Device.cpp
        gpu/Submit.cpp
    )
    add_library(nemo::gpu ALIAS nemo_gpu)
    target_include_directories(nemo_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(nemo_gpu PUBLIC Vulkan::Vulkan)
    target_link_libraries(nemo_gpu PRIVATE GPUOpen::VulkanMemoryAllocator)
endif()

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
