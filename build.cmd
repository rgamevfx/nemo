#!/usr/bin/env bash
# Linux build helper; uses the same debug preset as normal development.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

# Reuse the configured checkout without baking a machine-specific path into git.
if [[ -z "${VCPKG_ROOT:-}" && -f build/debug/CMakeCache.txt ]]; then
    while IFS= read -r line; do
        if [[ "$line" == CMAKE_TOOLCHAIN_FILE:*=*/scripts/buildsystems/vcpkg.cmake ]]; then
            toolchain="${line#*=}"
            candidate="${toolchain%/scripts/buildsystems/vcpkg.cmake}"
            if [[ -f "$candidate/scripts/buildsystems/vcpkg.cmake" ]]; then
                export VCPKG_ROOT="$candidate"
                break
            fi
        fi
    done < build/debug/CMakeCache.txt
fi

# Use common per-user checkout locations when the shell has no VCPKG_ROOT.
if [[ -z "${VCPKG_ROOT:-}" ]]; then
    for candidate in "${XDG_DATA_HOME:-$HOME/.local/share}/vcpkg" "$HOME/vcpkg" /opt/vcpkg; do
        if [[ -f "$candidate/scripts/buildsystems/vcpkg.cmake" ]]; then
            export VCPKG_ROOT="$candidate"
            break
        fi
    done
fi

if [[ -z "${VCPKG_ROOT:-}" ]]; then
    printf 'No vcpkg checkout found. Expected VCPKG_ROOT, ~/.local/share/vcpkg, ~/vcpkg, or /opt/vcpkg.\n' >&2
    exit 1
fi

cmake --preset debug -D NEMO_BUILD_UI=ON
cmake --build --preset debug --target nemo-ui --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
printf '\nBuild complete. Run ./launch.cmd to open Nemo.\n'
