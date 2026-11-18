#!/bin/bash
# Builds the same patched libssh2 as the AmigaOS 4 build (tools/libssh2-patches)
# for the Linux test build, against the system's OpenSSL:
#   build/deps/libssh2-linux/{include,lib/libssh2.a}
# The Makefile runs it when the library is missing or a patch changed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/build/deps"
SSH2_VER=1.11.1
SSH2_SHA256=9954cb54c4f548198a7cbebad248bdc87dd64bd26185708a294b2b50771e3769
OUT="$DEPS/libssh2-linux"
SRC="$DEPS/libssh2-linux-src"

mkdir -p "$DEPS"
cd "$DEPS"
if [ ! -f "libssh2-$SSH2_VER.tar.xz" ]; then
	curl -s -L -o "libssh2-$SSH2_VER.tar.xz" "https://libssh2.org/download/libssh2-$SSH2_VER.tar.xz"
fi
echo "$SSH2_SHA256  libssh2-$SSH2_VER.tar.xz" | sha256sum -c - > /dev/null
rm -rf "$SRC" "$OUT"
mkdir -p "$SRC"
tar xf "libssh2-$SSH2_VER.tar.xz" -C "$SRC" --strip-components=1
for p in "$ROOT"/tools/libssh2-patches/*.patch; do
	patch -s -p1 -d "$SRC" < "$p"
done
cd "$SRC"
./configure --prefix="$OUT" --with-crypto=openssl --disable-shared --enable-static \
	--without-libz --disable-examples-build --disable-docker-tests --disable-sshd-tests \
	CFLAGS="-O2 -g" > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -C src -j8 > make.log 2>&1 || { tail -30 make.log; exit 1; }
make -C src install > /dev/null
mkdir -p "$OUT/include"
cp include/*.h "$OUT/include/"
echo "libssh2 $SSH2_VER with $(ls "$ROOT"/tools/libssh2-patches/*.patch | wc -l) patches -> $OUT"
