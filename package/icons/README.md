# Release icons

These `.info` files were saved by AmigaOS 4's icon.library (native format,
64x64 true colour picture, no planar picture). AmigaOS 4 does not read PNG
icons, so they cannot be written on the host.

The pictures were made in the OS 4.1 style (normal and selected picture,
64x64 RGBA PNG, kept in ~/os4icons/out/sftphandler). `tools/make_icons.py`
turns them into the raw pictures mkicon reads. To change an icon:

1. `python3 -I tools/make_icons.py OUT [PICTURES]` writes `OUT/*.argb` and
   `OUT/mkicons.script`.
2. Build `tools/os4/mkicon.c` (ppc-amigaos-gcc -mcrt=clib4 ... -lauto) and copy it,
   the `.argb` files and the script to a drawer in the OS4 guest.
3. Run `Execute mkicons.script` in that drawer (clib4.library 2.5 must be
   first in LIBS:); the icons appear in `out/`.
4. Copy them back here. `Devs_DOSDrivers_SFTP.info` becomes
   `Devs/DOSDrivers/SFTP.info` and `Devs_DOSDrivers.info` becomes
   `Devs/DOSDrivers.info`, `Prefs_sftphprefs.info` becomes `Prefs/sftphprefs.info` in the archive (tools/make_lha.sh does this).

`def_SFTP.info` (a disk icon, `TYPE=DISK`) is the icon of the SFTP:
volume: it goes to `Prefs/Env-Archive/Sys/` in the archive, and Install
copies it to ENVARC:Sys and ENV:Sys, where Workbench looks for
`def_<device>.info` (like the stock def_DF0, def_RAD).
