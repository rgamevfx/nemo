#!/usr/bin/env bash
# Linux launcher; the .cmd suffix is a convenience, not Windows batch syntax.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

app="$PWD/build/debug/apps/nemo-ui/nemo-ui"
if [[ ! -x "$app" ]]; then
    printf 'Nemo is not built yet. Run ./build.cmd first.\n' >&2
    exit 1
fi

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
exec "$app" "$@"
