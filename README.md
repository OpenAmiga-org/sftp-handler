# sftp-handler

An AmigaOS 4 filesystem handler for FTP and SFTP servers, and its prefs
program `sftphprefs`.

The handler shows one volume (for example `SFTP:`). Its root lists the
sites configured with sftphprefs, as folders, without connecting. Opening
a site's folder connects to its server and shows the files there. The
root itself is read-only: sites are added, changed and removed only with
sftphprefs.

Built on filesysbox.library, clib4, libssh2 and AmiSSL.

## Layout

- `src/core/`   portable C: site list (XML), FTP client, SFTP (libssh2),
                file system with a listing cache (`sfs.c`)
- `src/os4/`    AmigaOS 4 handler: startup, filesysbox glue, requesters
- `src/prefs/`  sftphprefs (ReAction; "Test connection" uses the core's
                FTP/SFTP code)
- `src/linux/`  Linux FUSE build of the core, for fast tests
- `tests/`      `unit_tests.c`, `run_tests.py` (Linux build against the
                Docker servers), `os4/` (guest test tools)
- `testserver/` throwaway FTP (vsftpd) and SFTP (OpenSSH) servers in Docker
- `package/`    release files: Install script, AmigaGuide, DOSDriver,
                readme, icons (`icons/README.md`), third-party licences
- `tools/`      build scripts, icon pictures (`make_icons.py`, `os4/mkicon.c`)

## Building

    testserver/setup.sh            # once: start the test servers
    make test                      # Linux FUSE build, unit and server tests
    tools/build_deps_os4.sh        # once: libssh2 for clib4 with AmiSSL
    tools/build_os4.sh             # AmigaOS 4 handler and prefs (bumps the version)
    tests/os4/run_guest.py         # functional test in the QEMU guest (MCPd)
    tools/make_lha.sh              # release archive -> dist/SFTPHandler.lha

The OS4 build needs the cross-compiler image
`walkero/amigagccondocker:os4-gcc15-3.6.1` (GCC 15, SDK 54.25, clib4).

## Design notes

- One handler process does all network work (clib4, sockets, libssh2);
  the filesysbox callbacks only pass each call to it as an Exec message
  (`src/os4/fbxglue.c`, `rpc.h`). The startup, seglist and clib4 tricks
  come from nextcloud-handler; before clib4 starts, `sfh_entry` checks the
  clib4.library version and answers DOS itself when it is too old
  (otherwise every access to SFTP: would hang).
- `sfs.c`: the root lists the sites from `ENV:SFTPHandler/sites.xml`
  (checked for changes at most once a second) and a drawer icon per site
  (`ENV:Sys/def_drawer.info`); nothing at the root connects or can be
  changed (EROFS -> "file is write protected"). Each site has a listing
  cache, case-insensitive lookups, and reconnects once when a connection
  is lost. Login problems give ENODEV ("no disk") after one requester;
  cancelled password requesters are not repeated until DiskChange or a
  change of the site.
- FTP (`ftp.c`): passive only, data connections go to the control
  connection's address (PASV addresses behind NAT are often wrong). Up to
  four control connections: downloads keep one each, commands use another;
  when the server allows fewer, a download is stopped and continues later
  with REST. Writes go to a temp file and are uploaded on close (FTP
  cannot change part of a file); a failed upload is kept in KEEPDIR.
- SFTP (`sftp.c`): libssh2 in blocking mode with timeouts, server-side
  handles read and written at any offset. Host keys in OpenSSH
  known_hosts. `libssh2_keepalive_config()` must come after the
  handshake: set before it, the key exchange fails.
- chmod sends only the owner's r/w bits; group/other stay as they are.
- filesysbox maps EACCES, EIO, EXDEV... to "Error -1"; the glue maps them
  to the nearest AmigaDOS errors.

Public domain, see `UNLICENSE`. libssh2 is BSD licensed (its licence goes
into the release archive).
