#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if ! ./build.cmd; then
    if [[ "${NEMO_PAUSE_ON_ERROR:-0}" == "1" && -t 0 ]]; then
        printf '\nBuild failed. Press Enter to close this window.\n' >&2
        read -r
    fi
    exit 1
fi
exec ./launch.cmd "$@"
