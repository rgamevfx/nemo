# Media module: OpenImageIO/OpenEXR image source adapter (issue #4), the
# OpenColorIO viewing-transform adapter (issue #6), and — with the GPU
# build — the hardware media decode/encode adapter (issue #10). External
# library types never leak past the adapters: OIIO/OpenEXR/OCIO/FFmpeg
# types stay in the .cpp files; the public surface is the application
# image contract only. The CPU-only configuration keeps ImageIO/
# ViewingTransform buildable (the CLI `render`/`imageinfo` path); the
# hardware adapter requires the GPU interop seam and is added
# conditionally.
find_package(OpenImageIO CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)

add_library(nemo_media
    ImageIO.cpp
    ImageSource.cpp
    MediaImportService.cpp
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
    # System FFmpeg (ADR-0002): hardware media decode/encode adapter
    # (issue #10), which consumes the GPU interop seam.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libavformat libswscale)

    # FFmpeg-dependent viewer interop remains a separate GPU-owned target.
    # The base nemo_gpu target deliberately has no libav dependency.
    add_library(nemo_gpu_viewerinterop
        "${CMAKE_CURRENT_SOURCE_DIR}/../gpu/ViewerEncodeInterop.cpp"
    )
    add_library(nemo::gpu_viewerinterop ALIAS nemo_gpu_viewerinterop)
    target_include_directories(nemo_gpu_viewerinterop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(nemo_gpu_viewerinterop
        PUBLIC nemo::gpu
        PRIVATE PkgConfig::LIBAV
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nemo_gpu_viewerinterop PRIVATE -Wall -Wextra -Wpedantic)
        if(NEMO_WARN_AS_ERROR)
            target_compile_options(nemo_gpu_viewerinterop PRIVATE -Werror)
        endif()
    endif()

    target_sources(nemo_media
        PRIVATE
            Probe.cpp
            VideoDecode.cpp
            ViewerEncode.cpp
            CodecSweep.cpp
    )
    # MediaImportService.cpp is part of the always-built module (stills and
    # sequences decode on the CPU path); this macro enables its clip branch,
    # which needs the FFmpeg adapter above. Without it the service still
    # builds and reports an explicit diagnostic for clip sources instead of
    # silently failing to link.
    target_compile_definitions(nemo_media PRIVATE NEMO_MEDIA_FFMPEG=1)
    target_link_libraries(nemo_media
        PUBLIC nemo::gpu
        PRIVATE nemo::gpu_viewerinterop
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
