#!/bin/bash
# Builds GameOS's Intel Vulkan driver (Mesa ANV) for the runtime: Mesa 26.2.5, Wayland and headless only,
# 16 KB page alignment and x86-64-v3 code. Panther Lake no longer needs INTEL_FORCE_PROBE with this Mesa.
# Usage: WORK=<folder> OUT=<folder> JOBS=<n> runtime/mesa/build.sh
# Every run uses fresh dated folders; nothing is ever deleted.
set -euo pipefail
MESA_VERSION=26.2.5
MESA_SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5  # from docs.mesa3d.org/relnotes
WORK=${WORK:-$PWD/work}
OUT=${OUT:-$PWD/out}
JOBS=${JOBS:-$(nproc)}
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$WORK" "$OUT"
cd "$WORK"
TARBALL=mesa-$MESA_VERSION.tar.xz
[ -f "$TARBALL" ] || curl -sSfL --retry 3 -o "$TARBALL" "https://archive.mesa3d.org/$TARBALL"
echo "$MESA_SHA256  $TARBALL" | sha256sum -c -
SRC=$WORK/mesa-src-$STAMP
DEST=$OUT/mesa-$MESA_VERSION-$STAMP
mkdir -p "$SRC"
tar -xf "$TARBALL" -C "$SRC" --strip-components=1
ALIGN="-Wl,-z,max-page-size=16384,-z,common-page-size=16384"
meson setup "$WORK/mesa-build-$STAMP" "$SRC" --prefix=/usr --libdir=lib --buildtype=release -Db_ndebug=true \
    -Dvulkan-drivers=intel -Dgallium-drivers= -Dplatforms=wayland -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dvideo-codecs= -Dzstd=enabled \
    -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dc_args="-march=x86-64-v3" -Dcpp_args="-march=x86-64-v3" -Dc_link_args="$ALIGN" -Dcpp_link_args="$ALIGN"
ninja -C "$WORK/mesa-build-$STAMP" -j"$JOBS"
DESTDIR="$DEST" ninja -C "$WORK/mesa-build-$STAMP" install
bad=0
while IFS= read -r f; do
    if readelf -lW "$f" 2>/dev/null | awk '$1 == "LOAD" && $NF != "0x4000" { found = 1 } END { exit !found }'; then
        echo "not 16 KB aligned: $f"; bad=1
    fi
done < <(find "$DEST" -type f -name '*.so*')
[ "$bad" = 0 ] || { echo "alignment check failed"; exit 1; }
echo "GameOS Mesa $MESA_VERSION installed in $DEST (16 KB aligned)"
