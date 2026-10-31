#!/bin/bash
# Runs inside the build container (see build.sh): builds the filters and the
# app from a copy of the tree, proves the filters are still byte-identical to
# their references under this compiler, and packs the AppImage into /out.
set -euo pipefail

VERSION=${VERSION:?}

# A copy, so nothing built here lands in the developer's bin/ or app/target --
# binaries built against this older glibc are not the ones the tree's own
# tests are run with.
rm -rf /build && mkdir /build
tar -C /src --exclude=./bin --exclude=./build --exclude=./app/target \
    --exclude=./dist --exclude=./.git -cf - . | tar -C /build -xf -
cd /build

echo "== the filters"
make -j"$(nproc)"
echo "== make test: the DSP cores against their references, under this toolchain"
make test

echo "== the app"
(cd app && cargo build --release --locked)

echo "== the AppDir"
AD=/build/AppDir
mkdir -p "$AD/usr/bin" "$AD/usr/share/applications" \
         "$AD/usr/share/icons/hicolor/scalable/apps"
# pwfx looks for its filters next to its own executable, which inside the
# mounted image is usr/bin: no path to set, no wrapper script needed.
install -m755 app/target/release/pwfx "$AD/usr/bin/"
install -m755 bin/pw_* "$AD/usr/bin/"
strip "$AD"/usr/bin/*
ln -s usr/bin/pwfx "$AD/AppRun"
install -m644 packaging/appimage/pwfx.desktop "$AD/"
install -m644 packaging/appimage/pwfx.desktop "$AD/usr/share/applications/"
install -m644 packaging/appimage/pwfx.svg "$AD/"
install -m644 packaging/appimage/pwfx.svg "$AD/usr/share/icons/hicolor/scalable/apps/"
ln -s pwfx.svg "$AD/.DirIcon"

# Nothing is bundled. Every binary links only libc, libm, libgcc_s and
# libpipewire, and everything graphical is loaded at runtime from the host --
# which is how GPU and display libraries have to be found anyway. libpipewire
# in particular must be the host's: it loads its own protocol and SPA modules
# from the host's paths, and a bundled copy would mix with those. Say so if
# that ever stops being true.
echo "== what the binaries need from the host"
for b in "$AD"/usr/bin/*; do
    ldd "$b" | awk '{print $1}' | grep -v -e '^/' -e 'linux-vdso'
done | sort -u | tee /tmp/needed
unexpected=$(grep -v -e '^libc\.so' -e '^libm\.so' -e '^libgcc_s\.so' \
                     -e '^libpipewire-0\.3\.so' /tmp/needed || true)
if [ -n "$unexpected" ]; then
    echo "error: a binary now needs more than libc, libm, libgcc_s and libpipewire:" >&2
    echo "$unexpected" >&2
    exit 1
fi
top=$(objdump -T "$AD"/usr/bin/* | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "newest glibc symbol needed: $top (this system: $(ldd --version | head -1 | awk '{print $NF}'))"

echo "== pack"
out="/out/pwfx-${VERSION}-x86_64.AppImage"
ARCH=x86_64 appimagetool --no-appstream --runtime-file /opt/runtime-x86_64 "$AD" "$out"
chmod 755 "$out"
ls -la "$out"
