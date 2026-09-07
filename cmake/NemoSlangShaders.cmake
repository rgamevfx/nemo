# Build-time Slang -> SPIR-V shader compilation (issue #3).
#
# Tooling resolution order:
#   1. NEMO_SLANGC cache variable (explicit path, machine-specific override).
#   2. find_program(slangc) on PATH.
#   3. NEMO_DOWNLOAD_SLANGC=ON: download the pinned Slang release into the
#      build directory and use its slangc (requires network access at
#      configure time).
#
# When no tooling is available the shader target still exists and reports the
# omitted GPU path explicitly at configure and build time. A skipped shader
# path is NOT GPU-gate evidence (see issue #8).
#
# nemo_add_slang_shaders(<target> [ALL] SHADERS <file.slang>...)
#   Compiles each shader to <binary-dir>/spv/<name>.spv. Entry point is
#   "main"; declare it with [shader("compute")] etc. in the source file.

set(NEMO_SLANG_VERSION "2026.17" CACHE STRING
    "Pinned Slang release used by the shader toolchain fallback download")
set(NEMO_SLANGC "" CACHE FILEPATH "Path to the slangc compiler binary")
option(NEMO_DOWNLOAD_SLANGC
    "Download the pinned Slang release when slangc is not found" OFF)

function(_nemo_slang_archive_name out)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(_sys "linux")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_sys "windows")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(_sys "macos")
    else()
        message(FATAL_ERROR
            "No Slang fallback asset for host '${CMAKE_HOST_SYSTEM_NAME}'")
    endif()
    set(_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    if(_arch STREQUAL "arm64" OR _arch STREQUAL "aarch64")
        set(_arch "aarch64")
    elseif(_arch STREQUAL "AMD64" OR _arch STREQUAL "x86_64")
        set(_arch "x86_64")
    else()
        message(FATAL_ERROR "No Slang fallback asset for host arch '${_arch}'")
    endif()
    if(_sys STREQUAL "linux")
        set(${out} "slang-${NEMO_SLANG_VERSION}-${_sys}-${_arch}.tar.gz" PARENT_SCOPE)
    else()
        set(${out} "slang-${NEMO_SLANG_VERSION}-${_sys}-${_arch}.zip" PARENT_SCOPE)
    endif()
endfunction()

function(_nemo_slang_find_or_download)
    if(NEMO_SLANGC)
        if(EXISTS "${NEMO_SLANGC}")
            set(NEMO_SLANGC_TOOL "${NEMO_SLANGC}" PARENT_SCOPE)
            return()
        endif()
        message(FATAL_ERROR "NEMO_SLANGC points at a missing file: ${NEMO_SLANGC}")
    endif()

    find_program(NEMO_SLANGC_TOOL NAMES slangc DOC "slangc shader compiler")
    if(NEMO_SLANGC_TOOL)
        set(NEMO_SLANGC_TOOL "${NEMO_SLANGC_TOOL}" PARENT_SCOPE)
        return()
    endif()

    if(NOT NEMO_DOWNLOAD_SLANGC)
        set(NEMO_SLANGC_TOOL "" PARENT_SCOPE)
        return()
    endif()

    _nemo_slang_archive_name(_archive)
    set(_dir "${CMAKE_BINARY_DIR}/_slang-toolchain-${NEMO_SLANG_VERSION}")
    set(_archive_path "${_dir}/${_archive}")
    set(_url "https://github.com/shader-slang/slang/releases/download/v${NEMO_SLANG_VERSION}/${_archive}")

    if(NOT EXISTS "${_archive_path}")
        message(STATUS "Downloading Slang ${NEMO_SLANG_VERSION} (${_archive})")
        file(MAKE_DIRECTORY "${_dir}")
        file(DOWNLOAD "${_url}" "${_archive_path}" SHOW_PROGRESS STATUS _dl_status)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE "${_archive_path}")
            list(GET _dl_status 1 _dl_msg)
            message(FATAL_ERROR "Slang fallback download failed: ${_dl_msg} (${_url})")
        endif()
    endif()
    if(NOT EXISTS "${_dir}/bin")
        file(MAKE_DIRECTORY "${_dir}/extract")
        file(ARCHIVE_EXTRACT INPUT "${_archive_path}" DESTINATION "${_dir}/extract")
        file(RENAME "${_dir}/extract" "${_dir}/bin")
    endif()

    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_compiler "${_dir}/bin/bin/slangc.exe")
    else()
        set(_compiler "${_dir}/bin/bin/slangc")
    endif()
    if(NOT EXISTS "${_compiler}")
        message(FATAL_ERROR
            "Slang archive extracted but no compiler at ${_compiler}")
    endif()
    set(NEMO_SLANGC_TOOL "${_compiler}" PARENT_SCOPE)
endfunction()
function(nemo_add_slang_shaders target)
    cmake_parse_arguments(ARG "ALL" "" "SHADERS" ${ARGN})
    if(NOT ARG_SHADERS)
        message(FATAL_ERROR "nemo_add_slang_shaders(${target}): no SHADERS given")
    endif()

    _nemo_slang_find_or_download()

    set(_all_flag "")
    if(ARG_ALL)
        set(_all_flag "ALL")
    endif()

    if(NOT NEMO_SLANGC_TOOL)
        message(STATUS
            "GPU shader compilation OMITTED: slangc not found and "
            "NEMO_DOWNLOAD_SLANGC=OFF. Set NEMO_SLANGC, install slangc, or "
            "configure with -D NEMO_DOWNLOAD_SLANGC=ON. This is a CPU-only "
            "configuration and is not GPU-gate evidence (see issue #8).")
        add_custom_target(${target} ${_all_flag}
            COMMAND ${CMAKE_COMMAND} -E echo
                "nemo_shaders: GPU shader path OMITTED (no slangc; see configure output or AGENTS.md). Not GPU-gate evidence."
            VERBATIM)
        return()
    endif()

    message(STATUS "Shader compilation: ${NEMO_SLANGC_TOOL}")
    set(_spv_dir "${CMAKE_CURRENT_BINARY_DIR}/spv")
    file(MAKE_DIRECTORY "${_spv_dir}")
    set(_outputs)
    foreach(_shader ${ARG_SHADERS})
        cmake_path(GET _shader STEM _name)
        set(_spv "${_spv_dir}/${_name}.spv")
        add_custom_command(OUTPUT "${_spv}"
            COMMAND "${NEMO_SLANGC_TOOL}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}"
                -target spirv -entry main -o "${_spv}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}" "${NEMO_SLANGC_TOOL}"
            COMMENT "slangc ${_shader} -> ${_name}.spv"
            VERBATIM)
        list(APPEND _outputs "${_spv}")
    endforeach()
    add_custom_target(${target} ${_all_flag} DEPENDS ${_outputs} SOURCES ${ARG_SHADERS})
endfunction()
