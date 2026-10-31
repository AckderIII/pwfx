#!/bin/bash
# make appimage: build the pwfx AppImage in a Debian 12 container, then start
# it in a fresh, stock Debian 12 to check it runs and finds its filters.
#
# Needs podman. Writes dist/pwfx-<version>-x86_64.AppImage and nothing else
# in the tree: the source is mounted read-only and copied inside.
set -euo pipefail
cd "$(dirname "$0")/../.."

IMAGE=localhost/pwfx-appimage-build:debian12
CACHE=pwfx-appimage-cargo            # crates, kept between builds
mkdir -p dist

app_version=$(sed -n 's/^version *= *"\(.*\)"/\1/p' app/Cargo.toml | head -1)
rev=$(git describe --always --dirty 2>/dev/null || echo unknown)
VERSION="${app_version}-${rev}"

echo "== build image"
podman build -t "$IMAGE" -f packaging/appimage/Containerfile packaging/appimage

echo "== build"
# label=disable rather than :Z, which would relabel the whole checkout.
podman run --rm --security-opt label=disable \
    -v "$PWD":/src:ro \
    -v "$PWD/dist":/out \
    -v "$CACHE":/root/.cargo/registry \
    -e VERSION="$VERSION" \
    "$IMAGE" bash /src/packaging/appimage/inside.sh

out="dist/pwfx-${VERSION}-x86_64.AppImage"

echo "== smoke test on a stock Debian 12"
# Nothing but the PipeWire client library, as on a user's machine; no daemon,
# no display. Headless mode with an empty state starts the engine, which
# finds the filters next to the executable -- and says so in the log if it
# finds none. --appimage-extract-and-run because there is no FUSE here.
podman run --rm --security-opt label=disable \
    -v "$PWD/dist":/dist:ro \
    -e FILTERS="$(ls src/pw_*.c | wc -l)" \
    docker.io/library/debian:12 bash -c '
        set -e
        apt-get update -qq >/dev/null
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
            libpipewire-0.3-0 >/dev/null
        export XDG_RUNTIME_DIR=/tmp/run PWFX_CONFIG_DIR=/tmp/cfg
        mkdir -p $XDG_RUNTIME_DIR $PWFX_CONFIG_DIR
        echo "{\"version\":1}" > /tmp/empty.json
        app=/dist/'"$(basename "$out")"'
        $app --appimage-extract-and-run --help > /tmp/help
        head -2 /tmp/help
        cd /tmp && $app --appimage-extract >/dev/null
        n=$(ls /tmp/squashfs-root/usr/bin/pw_* | wc -l)
        [ "$n" = "$FILTERS" ] || { echo "FAIL: $n filters in the image, not $FILTERS" >&2; exit 1; }
        echo "ok: all $n filters are in the image"
        timeout 4 $app --appimage-extract-and-run --headless /tmp/empty.json \
            > /tmp/log 2>&1 || true
        cat /tmp/log
        if grep -q "no pw_\* filters found" /tmp/log; then
            echo "FAIL: the app did not find its filters" >&2; exit 1
        fi
        grep -q "running 0 chains" /tmp/log || { echo "FAIL: did not start" >&2; exit 1; }
        echo "ok: started on a stock Debian 12 and found its filters"
    '
echo
echo "built $out"
