#!/bin/bash
# Builds GameOS's runtime glibc for x86_64 Android: the GameOS Android patch (no system calls that Android's app
# seccomp filter traps, plus a SIGSYS translator), 16 KB page alignment and x86-64-v3 code.
# Usage: WORK=<folder> OUT=<folder> JOBS=<n> runtime/glibc/build.sh
# Every run uses fresh dated folders; nothing is ever deleted.
set -euo pipefail
GLIBC_VERSION=2.44
GLIBC_SHA256=37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$PWD/work}
OUT=${OUT:-$PWD/out}
JOBS=${JOBS:-$(nproc)}
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$WORK" "$OUT"
cd "$WORK"
TARBALL=glibc-$GLIBC_VERSION.tar.xz
[ -f "$TARBALL" ] || curl -sSfL --retry 3 -o "$TARBALL" "https://ftp.gnu.org/gnu/glibc/$TARBALL"
echo "$GLIBC_SHA256  $TARBALL" | sha256sum -c -
SRC=$WORK/src-$STAMP
BUILD=$WORK/build-$STAMP
DEST=$OUT/glibc-$GLIBC_VERSION-$STAMP
mkdir -p "$SRC" "$BUILD"
tar -xf "$TARBALL" -C "$SRC" --strip-components=1
for p in "$HERE"/patches/*.patch; do patch -d "$SRC" -p1 --forward < "$p"; done
cd "$BUILD"
"$SRC/configure" --prefix=/usr --libdir=/usr/lib --libexecdir=/usr/lib \
    --enable-kernel=5.4 --disable-werror --disable-nscd --disable-timezone-tools --disable-profile \
    --enable-stack-protector=strong --enable-bind-now --without-selinux \
    CC="gcc -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384" \
    CFLAGS="-O2 -march=x86-64-v3 -mtune=generic -pipe -g0"
make -j"$JOBS"
make install DESTDIR="$DEST" -j"$JOBS"
# Refuse the build if any shared object is not 16 KB aligned.
bad=0
while IFS= read -r f; do
    if readelf -lW "$f" 2>/dev/null | awk '$1 == "LOAD" && $NF != "0x4000" { found = 1 } END { exit !found }'; then
        echo "not 16 KB aligned: $f"; bad=1
    fi
done < <(find "$DEST" -type f -name '*.so*')
[ "$bad" = 0 ] || { echo "alignment check failed"; exit 1; }
echo "GameOS glibc $GLIBC_VERSION installed in $DEST (16 KB aligned)"
