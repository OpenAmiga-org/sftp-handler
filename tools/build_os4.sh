#!/bin/bash
# Cross-compiles the AmigaOS 4 parts in the walkero/amigagccondocker image.
#
#   tools/build_os4.sh            handler and prefs
#   tools/build_os4.sh handler    build/os4/sftp-handler
#   tools/build_os4.sh prefs      build/os4/sftphprefs
#   tools/build_os4.sh nettest    feasibility test tool
#
# Every build raises SFH_REVISION in src/os4/version.h (the version of the
# whole package, also shown by sftphprefs) and sets today's date.
# Builds off the main branch are betas: " beta" after the version string.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${OS4_IMAGE:-walkero/amigagccondocker:os4-gcc15-3.6.1}"
VH="$ROOT/src/os4/version.h"
if [ $# -eq 0 ]; then
	set -- handler prefs
fi

if [ "$1" != nettest ] && [ "${SFH_NO_BUMP:-}" = "" ]; then
	rev=$(sed -n 's/^#define SFH_REVISION \([0-9]*\)/\1/p' "$VH")
	sed -i "s/^#define SFH_REVISION .*/#define SFH_REVISION $((rev + 1))/" "$VH"
	sed -i "s/^#define SFH_DATE .*/#define SFH_DATE     \"$(date +%-d.%-m.%Y)\"/" "$VH"
fi
# libssh2 again when its patches changed
if [ ! -f "$ROOT/build/deps/libssh2-amissl/lib/libssh2.a" ] || [ ! -d "$ROOT/build/deps/amissl6-sdk/AmiSSL" ] ||
   [ -n "$(find "$ROOT/tools/libssh2-patches" -newer "$ROOT/build/deps/libssh2-amissl/lib/libssh2.a" -name '*.patch')" ]; then
	"$ROOT/tools/build_deps_os4.sh"
fi

beta=()
if [ "$(git -C "$ROOT" branch --show-current)" != main ]; then
	beta=(BETA=1)
	echo "branch $(git -C "$ROOT" branch --show-current): beta build"
fi

mkdir -p "$ROOT/build/os4"
# the AmiSSL stubs for AmiSSL 5 and 6 (see tools/gen_amissl_glue.py)
if [ "$1" != nettest ]; then
	python3 -I "$ROOT/tools/gen_amissl_glue.py" "$ROOT/build/os4"
fi
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/src -w /src "$IMAGE" \
	make -f tools/os4.mk "${beta[@]}" "$@" < /dev/null
for f in sftp-handler sftphprefs; do
	if [ -f "$ROOT/build/os4/$f" ]; then
		strings "$ROOT/build/os4/$f" | grep -m1 "^\$VER" || true
	fi
done
