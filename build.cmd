#!/usr/bin/env bash
# Linux build helper; uses the same debug preset as normal development.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

# Reuse the configured checkout without baking a machine-specific path into git.
if [[ -z "${VCPKG_ROOT:-}" && -f build/debug/CMakeCache.txt ]]; then
    while IFS= read -r line; do
        if [[ "$line" == CMAKE_TOOLCHAIN_FILE:*=*/scripts/buildsystems/vcpkg.cmake ]]; then
            toolchain="${line#*=}"
            export VCPKG_ROOT="${toolchain%/scripts/buildsystems/vcpkg.cmake}"
            break
        fi
    done < build/debug/CMakeCache.txt
fi

if [[ -z "${VCPKG_ROOT:-}" ]]; then
    printf 'Set VCPKG_ROOT to your vcpkg checkout, then run ./build.cmd again.\n' >&2
    exit 1
fi

cmake --preset debug -D NEMO_BUILD_UI=ON
cmake --build --preset debug --target nemo-ui --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
printf '\nBuild complete. Run ./launch.cmd to open Nemo.\n'
