add_executable(nemo-cli main.cpp ProjectSessionCommand.cpp ViewerCacheCommand.cpp)
target_link_libraries(nemo-cli PRIVATE nemo::core nemo::media nemo::extensions)

if(NEMO_BUILD_GPU)
    # The headless GPU smoke scenario (issue #8): nemo-cli evaluate-gpu.
    target_link_libraries(nemo-cli PRIVATE nemo::eval nemo::extensions_gpu)
    target_compile_definitions(nemo-cli PRIVATE NEMO_BUILD_GPU=1)
    if(DEFINED NEMO_SLANG_SPV_DIR)
        target_compile_definitions(nemo-cli PRIVATE NEMO_SLANG_SPV_DIR="${NEMO_SLANG_SPV_DIR}")
    endif()
    target_compile_definitions(nemo-cli PRIVATE
        NEMO_SLANG_SRC_DIR="${CMAKE_SOURCE_DIR}/src/nemo/nodes")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo-cli PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo-cli PRIVATE -Werror)
    endif()
endif()
