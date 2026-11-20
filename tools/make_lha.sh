#!/bin/bash
# Puts the release together and packs it: dist/SFTPHandler.lha
#
# Off the main branch everything is a beta: the builds must be beta builds
# (tools/build_os4.sh marks them), the archive is dist/SFTPHandler_beta.lha,
# the readme says BETA, and betas never go to OS4Depot.
#
#   tools/make_lha.sh      (after tools/build_os4.sh)
#
# Layout:
#   SFTPHandler/                       drawer (+ icon)
#     Install                          Installation Utility script
#     SFTPHandler.guide                documentation
#     LICENSE.txt, Third-party.txt
#     L/sftp-handler
#     Prefs/sftphprefs
#     Devs/DOSDrivers/SFTP             the DOSDriver
#   SFTPHandler.readme                 OS4Depot readme
#
# The LHA and the readme are also copied to $RELEASES (default: the shared
# folder's releases/), when that folder exists.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"
RELEASES="${RELEASES:-/media/sf_claude_folder/releases}"
STAGE="$ROOT/build/stage"
BRANCH=$(git -C "$ROOT" branch --show-current)
BETA=""
if [ "$BRANCH" != main ]; then
	BETA=_beta
fi
OUT="$ROOT/dist/SFTPHandler$BETA.lha"
LIC="$ROOT/package/licenses"
VH="$ROOT/src/os4/version.h"

ver=$(sed -n 's/^#define SFH_VERSION  *\([0-9]*\)/\1/p' "$VH").$(sed -n 's/^#define SFH_REVISION \([0-9]*\)/\1/p' "$VH")
amigadate=$(sed -n 's/^#define SFH_DATE *"\(.*\)"/\1/p' "$VH")
isodate=$(date +%Y%m%d)

for f in sftp-handler sftphprefs; do
	verstr=$(strings "$ROOT/build/os4/$f" | grep -m1 -F "\$VER: $f $ver (" || true)
	if [ -z "$verstr" ]; then
		echo "build/os4/$f is not version $ver; run tools/build_os4.sh" >&2
		exit 1
	fi
	case "$verstr" in
	*") beta") isbeta=_beta ;;
	*) isbeta="" ;;
	esac
	if [ "$isbeta" != "$BETA" ]; then
		echo "build/os4/$f (\"$verstr\") does not fit branch $BRANCH; run tools/build_os4.sh" >&2
		exit 1
	fi
done

# every AmiSSL 5 must work: src/os4/amissl.c asks for the AmiSSL 5.1 API,
# so the builds may only call functions AmiSSL 5.1 has
python3 -I "$ROOT/tools/check_amissl_api.py"
python3 -I "$ROOT/tools/update_readme.py"

# every release says what changed: the readme and the guide need an entry
# for this version
if ! grep -q "^- $ver[ (]" "$ROOT/package/SFTPHandler.readme"; then
	echo "package/SFTPHandler.readme has no History entry \"- $ver (...)\"" >&2
	exit 1
fi
if ! grep -q "^@{b}$ver[ (]" "$ROOT/package/SFTPHandler/SFTPHandler.guide"; then
	echo "SFTPHandler.guide has no History entry for $ver" >&2
	exit 1
fi

rm -rf "$STAGE"
D="$STAGE/SFTPHandler"
mkdir -p "$D/L" "$D/Prefs" "$D/Devs/DOSDrivers" "$ROOT/dist"

cp "$ROOT/build/os4/sftp-handler" "$D/L/"
cp "$ROOT/build/os4/sftphprefs" "$D/Prefs/"
cp "$ROOT/package/SFTPHandler/Install" "$D/"
cp "$ROOT/package/SFTPHandler/Devs/DOSDrivers/SFTP" "$D/Devs/DOSDrivers/"
# AmigaGuide and readme in Latin-1, as AmigaOS reads them
sed -e "s/^@\$VER: SFTPHandler.guide [0-9.]* ([0-9.]*)/@\$VER: SFTPHandler.guide $ver ($amigadate)/" \
	"$ROOT/package/SFTPHandler/SFTPHandler.guide" | iconv -f UTF-8 -t ISO-8859-1 > "$D/SFTPHandler.guide"
cp "$ROOT/UNLICENSE" "$D/LICENSE.txt"
if [ -n "$BETA" ]; then
	sed -e "s/@VERSION@/$ver beta/" -e "s/@DATE@/$isodate/" \
		-e "s/^\(description: .*\)$/\1 - BETA/" \
		-e "0,/^hend:$/s//hend:\n\nTHIS IS A BETA (branch $BRANCH). Do not upload it to OS4Depot.\n/" \
		"$ROOT/package/SFTPHandler.readme"
else
	sed -e "s/@VERSION@/$ver/" -e "s/@DATE@/$isodate/" "$ROOT/package/SFTPHandler.readme"
fi | iconv -f UTF-8 -t ISO-8859-1 > "$STAGE/SFTPHandler.readme"

{
	echo "sftp-handler and sftphprefs are public domain (LICENSE.txt). The programs"
	echo "contain code from the following libraries, which are distributed under"
	echo "their own licences, reproduced here."
	echo
	echo "======================================================================"
	echo "libssh2 $(sed -n 's/#define LIBSSH2_VERSION *"\(.*\)"/\1/p' "$ROOT/build/deps/libssh2-amissl/include/libssh2.h") - https://libssh2.org/"
	echo "======================================================================"
	cat "$LIC/libssh2-COPYING"
	echo
	echo "======================================================================"
	echo "Expat - https://libexpat.github.io/"
	echo "======================================================================"
	cat "$LIC/expat-COPYING"
	echo
	echo "======================================================================"
	echo "clib4 (C library, linked parts and clib4.library) - https://github.com/AmigaLabs/clib4"
	echo "======================================================================"
	cat "$LIC/clib4-LICENSE"
	echo
	echo "======================================================================"
	echo "AmiSSL link library (libamisslstubs) - https://github.com/jens-maus/amissl"
	echo "Licensed under the Apache License, Version 2.0:"
	echo "======================================================================"
	cat "$LIC/apache-2.0.txt"
} > "$D/Third-party.txt"

# icons made by icon.library itself, see package/icons/README.md
I="$ROOT/package/icons"
cp "$I/SFTPHandler.info" "$STAGE/"
for f in Install SFTPHandler.guide LICENSE.txt Third-party.txt L Prefs Devs; do
	cp "$I/$f.info" "$D/$f.info"
done
cp "$I/Prefs_sftphprefs.info" "$D/Prefs/sftphprefs.info"
cp "$I/Devs_DOSDrivers.info" "$D/Devs/DOSDrivers.info"
cp "$I/Devs_DOSDrivers_SFTP.info" "$D/Devs/DOSDrivers/SFTP.info"
# the volume icon of SFTP: (Install copies it to ENVARC:Sys and ENV:Sys)
mkdir -p "$D/Prefs/Env-Archive/Sys"
cp "$I/def_SFTP.info" "$D/Prefs/Env-Archive/Sys/def_SFTP.info"

rm -f "$OUT"
(cd "$STAGE" && "$LHA" aq2o5 "$OUT" SFTPHandler.info SFTPHandler.readme SFTPHandler)
"$LHA" l "$OUT"
echo "version $ver -> $OUT"
if [ -d "$RELEASES" ]; then
	cp -p "$OUT" "$RELEASES/"
	cp -p "$STAGE/SFTPHandler.readme" "$RELEASES/SFTPHandler$BETA.readme"
	echo "copied LHA and readme to $RELEASES"
else
	echo "note: $RELEASES not found, nothing copied" >&2
fi
