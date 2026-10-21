#!/bin/bash
# Builds GameOS's Wine: Proton's Wine 11 (ValveSoftware/wine, branch proton_11.0) at a pinned commit with the GameOS
# patches (GameOS Sync instead of /dev/ntsync, 32-bit TEB through wrfsbase, SIGSYS chaining to the runtime glibc),
# in new WoW64 mode (64-bit Unix side only), with Wayland and Vulkan, 16 KB page alignment and x86-64-v3 code.
# Usage: WORK=<folder> OUT=<folder> JOBS=<n> wine/build.sh
# Every run uses fresh dated folders; nothing is ever deleted.
set -euo pipefail
WINE_REPO=https://github.com/ValveSoftware/wine.git
WINE_COMMIT=e60892cc2398b0a83c332a2078c7b85e47bdb42b
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=${WORK:-$PWD/work}
OUT=${OUT:-$PWD/out}
JOBS=${JOBS:-$(nproc)}
STAMP=$(date +%Y%m%d-%H%M%S)
SRC=$WORK/wine-src-$STAMP
BUILD=$WORK/wine-build-$STAMP
DEST=$OUT/wine-gameos-$STAMP
mkdir -p "$SRC" "$BUILD" "$OUT"

# Source: the exact commit, so the content is verified by its hash.
git -C "$SRC" init -q
git -C "$SRC" fetch -q --depth 1 "$WINE_REPO" "$WINE_COMMIT"
git -C "$SRC" checkout -q FETCH_HEAD
test "$(git -C "$SRC" rev-parse HEAD)" = "$WINE_COMMIT"

for p in "$HERE"/patches/*.patch; do patch -d "$SRC" -p1 --forward < "$p"; done
# GameOS Sync goes into the wineserver and into ntdll's Unix side (marked for makedep as Unix code).
for d in server dlls/ntdll/unix; do
    cp "$ROOT/sync/gsync.c" "$ROOT/sync/gsync.h" "$ROOT/sync/wine/gameos_ntsync.c" "$ROOT/sync/wine/gameos_ntsync.h" "$SRC/$d/"
done
for f in gsync.c gameos_ntsync.c; do
    { printf '#if 0\n#pragma makedep unix\n#endif\n\n'; cat "$SRC/dlls/ntdll/unix/$f"; } > "$SRC/dlls/ntdll/unix/$f.gameos"
    mv "$SRC/dlls/ntdll/unix/$f.gameos" "$SRC/dlls/ntdll/unix/$f"
done

# Generated files that Proton's own build creates.
(cd "$SRC" && autoreconf -f && ./tools/make_requests && ./tools/make_specfiles)
(cd "$SRC/dlls/winevulkan" && python3 ./make_vulkan)

cd "$BUILD"
"$SRC/configure" --prefix=/wine --enable-archs=i386,x86_64 --without-x --with-wayland --with-vulkan \
    --without-oss --without-alsa --without-pulse --without-gstreamer --without-cups --without-sane --without-krb5 \
    --without-pcap --without-usb --without-v4l2 --without-capi --without-netapi --disable-tests \
    CFLAGS="-O2 -march=x86-64-v3 -mtune=generic -pipe -g0" \
    CROSSCFLAGS="-O2 -march=x86-64-v3 -mtune=generic -pipe -g0" \
    LDFLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"
make -j"$JOBS"
make install DESTDIR="$DEST" -j"$JOBS"

# Refuse the build if any Unix-side ELF file is not 16 KB aligned.
bad=0
while IFS= read -r f; do
    if file -b "$f" | grep -q '^ELF' && readelf -lW "$f" 2>/dev/null | awk '$1 == "LOAD" && $NF != "0x4000" { found = 1 } END { exit !found }'; then
        echo "not 16 KB aligned: $f"; bad=1
    fi
done < <(find "$DEST" -type f \( -name '*.so' -o -perm -u+x \))
[ "$bad" = 0 ] || { echo "alignment check failed"; exit 1; }
echo "GameOS Wine installed in $DEST (16 KB aligned)"
