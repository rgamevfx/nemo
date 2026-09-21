# Media module: OpenImageIO/OpenEXR image source adapter (issue #4), the
# OpenColorIO viewing-transform adapter (issue #6), and — with the GPU
# build — the hardware media decode adapter (issue #10). External
# library types never leak past the adapters: OIIO/OpenEXR/OCIO/FFmpeg
# types stay in the .cpp files; the public surface is the application
# image contract only. The CPU-only configuration keeps ImageIO/
# ViewingTransform buildable (the CLI `render`/`imageinfo` path); the
# hardware decode adapter requires the GPU interop seam and is added
# conditionally.
find_package(OpenImageIO CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)

add_library(nemo_media
    DeliveryOutput.cpp
    ImageIO.cpp
    ImageSource.cpp
    InputColor.cpp
    MediaImportService.cpp
    SequenceDiscovery.cpp
    ViewingTransform.cpp
)
add_library(nemo::media ALIAS nemo_media)

target_include_directories(nemo_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_media
    PUBLIC nemo::core
    PRIVATE OpenImageIO::OpenImageIO
    PRIVATE OpenColorIO::OpenColorIO
)

if(NEMO_BUILD_GPU)
    # System FFmpeg (ADR-0002): the source decode adapter (issue #10). The
    # viewer cache no longer carries a video representation, so media owns no
    # encode/interop target; delivery encoding stays with its own owner.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libswscale)

    target_sources(nemo_media
        PRIVATE
            Probe.cpp
            VideoDecode.cpp
    )
    # MediaImportService.cpp is part of the always-built module (stills and
    # sequences decode on the CPU path); this macro enables its clip branch,
    # which needs FFmpeg. Without it the service still builds and reports an
    # explicit diagnostic for clip sources instead of silently failing to
    # link.
    target_compile_definitions(nemo_media PRIVATE NEMO_MEDIA_FFMPEG=1)
    target_link_libraries(nemo_media
        PUBLIC nemo::gpu
        PRIVATE PkgConfig::LIBAV
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_media PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_media PRIVATE -Werror)
    endif()
endif()

if(NEMO_ENABLE_IPO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET nemo_media PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()
