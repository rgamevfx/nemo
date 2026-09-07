find_package(nlohmann_json CONFIG REQUIRED)

add_library(nemo_core
    core/document/Graph.cpp
    core/document/Document.cpp
    core/document/Serialization.cpp
)
add_library(nemo::core ALIAS nemo_core)

target_include_directories(nemo_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_core PUBLIC nlohmann_json::nlohmann_json)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_core PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_core PRIVATE -Werror)
    endif()
endif()

if(NEMO_ENABLE_IPO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET nemo_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()
