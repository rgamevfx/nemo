# Media module: OpenImageIO/OpenEXR image source adapter (issue #4) and the
# OpenColorIO viewing-transform adapter (issue #6). External library types
# never leak past the adapters: OIIO/OpenEXR/OCIO types stay in the .cpp
# files; the public surface is the application image contract only.
find_package(OpenImageIO CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)

# System FFmpeg (ADR-0002): hardware media decode/encode adapter (issue #10).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libswscale)

add_library(nemo_media
    ImageIO.cpp
    ViewingTransform.cpp
    Probe.cpp
    VideoDecode.cpp
    ViewerEncode.cpp
    CodecSweep.cpp
)
add_library(nemo::media ALIAS nemo_media)

target_include_directories(nemo_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_media
    PUBLIC nemo::core
    PUBLIC nemo::gpu
    PRIVATE OpenImageIO::OpenImageIO
    PRIVATE OpenColorIO::OpenColorIO
    PRIVATE PkgConfig::LIBAV
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
