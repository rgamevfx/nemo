# Media module: OpenImageIO/OpenEXR image source adapter (issue #4).
# External formats never leak past this adapter: OIIO/OpenEXR types stay in
# the .cpp files; the public surface is the application image contract only.
find_package(OpenImageIO CONFIG REQUIRED)

add_library(nemo_media
    ImageIO.cpp
)
add_library(nemo::media ALIAS nemo_media)

target_include_directories(nemo_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_media
    PUBLIC nemo::core
    PRIVATE OpenImageIO::OpenImageIO
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_media PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_media PRIVATE -Werror)
    endif()
endif()

if(NEMO_ENABLE_IPO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET nemo_media PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()
