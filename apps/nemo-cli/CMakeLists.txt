add_executable(nemo-cli main.cpp)
target_link_libraries(nemo-cli PRIVATE nemo::core nemo::media)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo-cli PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo-cli PRIVATE -Werror)
    endif()
endif()
