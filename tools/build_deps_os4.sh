#!/bin/bash
# Builds a static libssh2 for AmigaOS 4 (clib4) that uses AmiSSL for its
# cryptography, in the cross-compiler image. Results:
#   build/deps/libssh2-amissl/{include,lib}
#   build/deps/amissl-sdk/AmiSSL/Developer   (AmiSSL 5 SDK: headers, libssh2)
#   build/deps/amissl6-sdk/AmiSSL/Developer  (AmiSSL 6 SDK: interface offsets)
#   build/deps/amissl-clib4/libamisslauto.a
#
# libssh2's configure looks for libssl/libcrypto: empty stand-ins satisfy
# it, the AmiSSL stubs (libamisslstubs.a) provide the functions. The
# AmiSSL SDK has no libamisslauto.a for clib4, so a tiny one that only
# defines IAmiSSL is made; the handler opens AmiSSL itself.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${OS4_IMAGE:-walkero/amigagccondocker:os4-gcc15-3.6.1}"
DEPS="$ROOT/build/deps"
SSH2_VER=1.11.1
# SSH2_POLL=yes builds libssh2 with poll() (clib4 maps it to select());
# the default waits with select() directly
SSH2_POLL="${SSH2_POLL:-no}"
SSH2_PREFIX="${SSH2_PREFIX:-libssh2-amissl}"
SSH2_SHA256=9954cb54c4f548198a7cbebad248bdc87dd64bd26185708a294b2b50771e3769
AMISSL_VER=5.27
AMISSL6_VER=6.1
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"

mkdir -p "$DEPS"
cd "$DEPS"
if [ ! -d "libssh2-$SSH2_VER" ]; then
	curl -s -L -o "libssh2-$SSH2_VER.tar.xz" "https://libssh2.org/download/libssh2-$SSH2_VER.tar.xz"
	echo "$SSH2_SHA256  libssh2-$SSH2_VER.tar.xz" | sha256sum -c -
	tar xf "libssh2-$SSH2_VER.tar.xz"
fi
if [ ! -d amissl-sdk/AmiSSL ]; then
	gh release download "$AMISSL_VER" -R jens-maus/amissl -p "AmiSSL-$AMISSL_VER-SDK.lha" --clobber
	mkdir -p amissl-sdk
	(cd amissl-sdk && "$LHA" xq "../AmiSSL-$AMISSL_VER-SDK.lha" >/dev/null)
fi
if [ ! -d amissl6-sdk/AmiSSL ]; then
	gh release download "$AMISSL6_VER" -R jens-maus/amissl -p "AmiSSL-$AMISSL6_VER-SDK.lha" --clobber
	mkdir -p amissl6-sdk
	(cd amissl6-sdk && "$LHA" xq "../AmiSSL-$AMISSL6_VER-SDK.lha" >/dev/null)
fi

rm -rf ssh2-build "$SSH2_PREFIX" amissl-clib4
mkdir -p ssh2-build amissl-clib4
cat > amissl-clib4/amisslauto.c <<'EOF'
/* stands in for the AmiSSL SDK's libamisslauto.a, which has no clib4 build */
struct AmiSSLIFace;
struct AmiSSLIFace *IAmiSSL;
EOF
echo "int sftph_dummy;" > amissl-clib4/dummy.c

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/src -w /src/build/deps "$IMAGE" sh -ec '
	AM=/src/build/deps/amissl-sdk/AmiSSL/Developer
	ppc-amigaos-gcc -mcrt=clib4 -O2 -c -o amissl-clib4/amisslauto.o amissl-clib4/amisslauto.c
	ppc-amigaos-ar rcs amissl-clib4/libamisslauto.a amissl-clib4/amisslauto.o
	ppc-amigaos-gcc -mcrt=clib4 -c -o amissl-clib4/dummy.o amissl-clib4/dummy.c
	ppc-amigaos-ar rcs amissl-clib4/libssl.a amissl-clib4/dummy.o
	ppc-amigaos-ar rcs amissl-clib4/libcrypto.a amissl-clib4/dummy.o
	cd ssh2-build
	POLLVAR=""
	[ "'"$SSH2_POLL"'" = yes ] || POLLVAR="ac_cv_func_poll=no"
	../libssh2-'"$SSH2_VER"'/configure --host=ppc-amigaos --prefix=/src/build/deps/'"$SSH2_PREFIX"' $POLLVAR \
		CC=ppc-amigaos-gcc AR=ppc-amigaos-ar RANLIB=ppc-amigaos-ranlib \
		CPPFLAGS="-I$AM/include" CFLAGS="-mcrt=clib4 -O2" \
		LDFLAGS="-mcrt=clib4 -L$AM/lib/AmigaOS4 -L/src/build/deps/amissl-clib4" \
		LIBS="-lamisslstubs -lamisslauto" \
		--with-crypto=openssl --with-libssl-prefix=/src/build/deps/amissl-clib4 \
		--disable-shared --enable-static --without-libz --disable-examples-build \
		--disable-docker-tests --disable-sshd-tests > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	grep -E "Crypto library|version:" configure.log
	make -C src -j8 > make.log 2>&1 || { tail -30 make.log; exit 1; }
	make -C src install > /dev/null
' < /dev/null
grep -E "define HAVE_(POLL|SELECT)" ssh2-build/src/libssh2_config.h || true
mkdir -p "$SSH2_PREFIX/include"
cp "libssh2-$SSH2_VER"/include/*.h "$SSH2_PREFIX/include/"
mkdir -p licenses
cp "libssh2-$SSH2_VER/COPYING" licenses/libssh2-COPYING
ls -la "$SSH2_PREFIX/lib/libssh2.a"
