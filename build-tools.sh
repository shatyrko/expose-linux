#!/usr/bin/env bash
# Build tools/expose-shots (needs gcc, wayland, wayland-protocols; wayland-scanner comes with wayland).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
proto=/usr/share/wayland-protocols/staging
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
for name in ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1 ext-image-capture-source/ext-image-capture-source-v1 \
            ext-image-copy-capture/ext-image-copy-capture-v1; do
    base="$(basename "$name")"
    wayland-scanner client-header "$proto/$name.xml" "$build/$base-client-protocol.h"
    wayland-scanner private-code "$proto/$name.xml" "$build/$base-protocol.c"
done
gcc -O2 -Wall -I"$build" -o "$here/tools/expose-shots" "$here/tools/expose-shots.c" "$build"/*-protocol.c -lwayland-client
echo "built $here/tools/expose-shots"
