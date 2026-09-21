# Installed trusted native effect packages (issue #37).
#
# The loader is a core consumer only: it validates manifests, assembles the
# immutable built-in plus package registration and opens package libraries
# through the fixed C ABI in EffectAbi.h with dlopen. It never links Qt, the
# native GPU module or the effect library, so a headless build loads no Qt and
# no device code, and the GPU projection below is a separate optional target.
#
# This directory must be added after nemo_core and, when the native GPU module
# is built, after nemo_eval (see the guard below).
add_library(nemo_extensions
    InstalledPackages.cpp
)
add_library(nemo::extensions ALIAS nemo_extensions)

target_include_directories(nemo_extensions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nemo_extensions PUBLIC nemo::core PRIVATE ${CMAKE_DL_LIBS})

if(NEMO_BUILD_GPU)
    if(NOT TARGET nemo_eval)
        message(FATAL_ERROR
            "src/nemo/extensions must be added after the NEMO_BUILD_GPU block that "
            "creates nemo_eval, so the installed-package GPU projection can link it")
    endif()
    # The external GPU projection: built-in contributions plus one contribution
    # per installed package, whose preparation callback invokes the package's own
    # ABI `prepare` and whose pass carries the package's absolute SPIR-V path.
    add_library(nemo_extensions_gpu
        GpuPackages.cpp
    )
    add_library(nemo::extensions_gpu ALIAS nemo_extensions_gpu)

    target_link_libraries(nemo_extensions_gpu PUBLIC nemo::extensions nemo::eval)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_extensions PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_extensions PRIVATE -Werror)
    endif()
    if(TARGET nemo_extensions_gpu)
        target_compile_options(nemo_extensions_gpu PRIVATE -Wall -Wextra -Wpedantic)
        if(NEMO_WARN_AS_ERROR)
            target_compile_options(nemo_extensions_gpu PRIVATE -Werror)
        endif()
    endif()
endif()

if(NEMO_ENABLE_IPO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET nemo_extensions PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    if(TARGET nemo_extensions_gpu)
        set_property(TARGET nemo_extensions_gpu PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()
