# ColorWarp installed effect package (issue #37).
#
# This directory is a complete external package: it builds a native shared
# library against the SDK's ABI header, compiles its own Slang kernel to
# SPIR-V, and installs itself as a package folder that an InstalledPackages root
# can discover. It registers nothing in the built-in inventory, links no host
# target, and has no Qt or Vulkan dependency.
#
# Build standalone:
#   cmake -S examples/colorwarp -B build/colorwarp \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
#   cmake --build build/colorwarp --target colorwarp_package
#   cmake --install build/colorwarp --prefix "$HOME/.local"
#
# The assembled package folder is <build-dir>/package/colorwarp (published as the
# cache variable NEMO_COLORWARP_PACKAGE_ROOT, whose direct child it is), and the
# installed one is <prefix>/share/nemo/extensions/colorwarp. Both contain exactly
# what manifest.json declares.

cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nemo_colorwarp LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include(GNUInstallDirs)

# The source SDK this package compiles against: the ABI header
# (src/nemo/extensions/EffectAbi.h), the shared Slang binding support
# (src/nemo/nodes/GpuGeometry.slang) and the shared shader toolchain module
# (cmake/NemoSlangShaders.cmake). Nothing else of the host is used.
set(NEMO_COLORWARP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Nemo source SDK root the ColorWarp package compiles against")
set(NEMO_COLORWARP_ABI_HEADER "${NEMO_COLORWARP_SDK_DIR}/src/nemo/extensions/EffectAbi.h")
set(NEMO_COLORWARP_SLANG_SUPPORT "${NEMO_COLORWARP_SDK_DIR}/src/nemo/nodes/GpuGeometry.slang")
if(NOT EXISTS "${NEMO_COLORWARP_ABI_HEADER}" OR NOT EXISTS "${NEMO_COLORWARP_SLANG_SUPPORT}")
    message(FATAL_ERROR
        "ColorWarp needs the source SDK at NEMO_COLORWARP_SDK_DIR=${NEMO_COLORWARP_SDK_DIR}: "
        "expected ${NEMO_COLORWARP_ABI_HEADER} and ${NEMO_COLORWARP_SLANG_SUPPORT}. "
        "Point -D NEMO_COLORWARP_SDK_DIR at a Nemo checkout.")
endif()

# The package's declared content. The manifest is the declaration and these are
# its files; a mismatch is a package defect, so it fails here by name.
set(NEMO_COLORWARP_QML_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/ColorWarpEditor.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/ColorWarpPanel.qml")
foreach(_file "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ColorWarp.glsl"
              ${NEMO_COLORWARP_QML_SOURCES})
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "ColorWarp: manifest-declared file is missing: ${_file}")
    endif()
endforeach()

# The assembled package folder, and the root whose direct child it is (the value
# a NEMO_EXTENSION_PATH or an InstalledPackages fixture points at).
set(NEMO_COLORWARP_PACKAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/package" CACHE PATH
    "Directory whose direct child 'colorwarp' is the assembled ColorWarp package")
set(NEMO_COLORWARP_PACKAGE_DIR "${NEMO_COLORWARP_PACKAGE_ROOT}/colorwarp")

find_package(nlohmann_json CONFIG REQUIRED)

# The native library: ABI 1 entry point plus the validate/process/prepare
# callbacks. nlohmann is private (never across the ABI) and the SDK include root
# is what makes "nemo/extensions/EffectAbi.h" resolve, exactly as the host
# includes it.
add_library(nemo_colorwarp SHARED src/ColorWarpLibrary.cpp)
target_include_directories(nemo_colorwarp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${NEMO_COLORWARP_SDK_DIR}/src")
target_link_libraries(nemo_colorwarp PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(nemo_colorwarp PRIVATE NEMO_EFFECT_BUILD)
target_compile_features(nemo_colorwarp PRIVATE cxx_std_20)
set_target_properties(nemo_colorwarp PROPERTIES
    # The manifest declares this exact base name; no version suffix is added, so
    # the produced file is prefix + name + suffix for the platform.
    OUTPUT_NAME nemo_colorwarp
    LIBRARY_OUTPUT_DIRECTORY "${NEMO_COLORWARP_PACKAGE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${NEMO_COLORWARP_PACKAGE_DIR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# The package's declared library file, in the platform's own shared-library
# spelling: libnemo_colorwarp.so, nemo_colorwarp.dll or libnemo_colorwarp.dylib.
# The source manifest.json is the reviewed declaration and keeps the Linux
# default; the manifest that is assembled and installed is generated from it here
# so the declared file name is the one this platform actually produces.
set(NEMO_COLORWARP_LIBRARY_FILE
    "${CMAKE_SHARED_LIBRARY_PREFIX}nemo_colorwarp${CMAKE_SHARED_LIBRARY_SUFFIX}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" _colorwarp_manifest)
string(REGEX REPLACE "\"library\"[ \t]*:[ \t]*\"[^\"]*\""
    "\"library\": \"${NEMO_COLORWARP_LIBRARY_FILE}\"" _colorwarp_manifest "${_colorwarp_manifest}")
if(NOT _colorwarp_manifest MATCHES "\"library\": \"${NEMO_COLORWARP_LIBRARY_FILE}\"")
    message(FATAL_ERROR "ColorWarp: manifest.json has no 'library' entry to declare "
        "'${NEMO_COLORWARP_LIBRARY_FILE}' in")
endif()
set(NEMO_COLORWARP_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
file(WRITE "${NEMO_COLORWARP_MANIFEST}" "${_colorwarp_manifest}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nemo_colorwarp PRIVATE -Wall -Wextra -Wpedantic)
    if(NEMO_WARN_AS_ERROR)
        target_compile_options(nemo_colorwarp PRIVATE -Werror)
    endif()
endif()

# Slang -> SPIR-V through the repository's shared shader toolchain (NEMO_SLANGC,
# find_program, or the pinned download), never a second copy of that resolution.
# The kernel imports binding support from the selected SDK, not a fixed checkout
# location, and is rebuilt whenever that contract changes.
if(NOT COMMAND nemo_add_slang_shaders)
    include("${NEMO_COLORWARP_SDK_DIR}/cmake/NemoSlangShaders.cmake")
endif()
nemo_add_slang_shaders(colorwarp_shaders ALL
    SHADERS
        shaders/ColorWarp.slang
    INCLUDE_DIRECTORIES "${NEMO_COLORWARP_SDK_DIR}/src/nemo/nodes"
    DEPENDS
        "${NEMO_COLORWARP_SLANG_SUPPORT}")
if(NOT DEFINED NEMO_SLANG_SPV_DIR)
    message(FATAL_ERROR
        "ColorWarp declares a native GPU implementation: set NEMO_SLANGC, install slangc, "
        "or configure with NEMO_DOWNLOAD_SLANGC=ON before building this package.")
endif()
set(NEMO_COLORWARP_SPIRV "${NEMO_SLANG_SPV_DIR}/ColorWarp.spv")

set(_colorwarp_package_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NEMO_COLORWARP_PACKAGE_DIR}/qml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NEMO_COLORWARP_MANIFEST}" "${NEMO_COLORWARP_PACKAGE_DIR}/manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ColorWarp.glsl" "${NEMO_COLORWARP_PACKAGE_DIR}/ColorWarp.glsl"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/ColorWarpEditor.qml" "${NEMO_COLORWARP_PACKAGE_DIR}/qml/ColorWarpEditor.qml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/ColorWarpPanel.qml" "${NEMO_COLORWARP_PACKAGE_DIR}/qml/ColorWarpPanel.qml")
list(APPEND _colorwarp_package_commands
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${NEMO_COLORWARP_SPIRV}" "${NEMO_COLORWARP_PACKAGE_DIR}/ColorWarp.spv")

# The package assembled in the build tree, for consumers that run against it
# directly (a NEMO_EXTENSION_PATH pointing at NEMO_COLORWARP_PACKAGE_ROOT).
add_custom_target(colorwarp_package ALL
    ${_colorwarp_package_commands}
    DEPENDS nemo_colorwarp colorwarp_shaders
    COMMENT "Assembling the ColorWarp package in ${NEMO_COLORWARP_PACKAGE_DIR}"
    VERBATIM)

# The installed package folder: <prefix>/share/nemo/extensions/colorwarp, whose
# parent is the root InstalledPackages reads.
set(NEMO_COLORWARP_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/nemo/extensions/colorwarp" CACHE STRING
    "Install destination of the ColorWarp package folder, relative to the prefix")
install(FILES "${NEMO_COLORWARP_MANIFEST}" DESTINATION "${NEMO_COLORWARP_INSTALL_DIR}")
install(TARGETS nemo_colorwarp
    LIBRARY DESTINATION "${NEMO_COLORWARP_INSTALL_DIR}"
    RUNTIME DESTINATION "${NEMO_COLORWARP_INSTALL_DIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ColorWarp.glsl" DESTINATION "${NEMO_COLORWARP_INSTALL_DIR}")
install(FILES ${NEMO_COLORWARP_QML_SOURCES} DESTINATION "${NEMO_COLORWARP_INSTALL_DIR}/qml")
install(FILES "${NEMO_COLORWARP_SPIRV}" DESTINATION "${NEMO_COLORWARP_INSTALL_DIR}")
