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
- `src/prefs/`  sftphprefs (ReAction)
- `src/linux/`  Linux FUSE build of the core, for fast tests
- `tests/`      `unit_tests.c`, `run_tests.py` (Linux build against the
                Docker servers), `os4/` (guest test tools)
- `testserver/` throwaway FTP (vsftpd) and SFTP (OpenSSH) servers in Docker
- `tools/`      build scripts

## Building

    testserver/setup.sh            # once: start the test servers
    make test                      # Linux FUSE build, unit and server tests
    tools/build_deps_os4.sh        # once: libssh2 for clib4 with AmiSSL
    tools/build_os4.sh             # AmigaOS 4 handler and prefs

The OS4 build needs the cross-compiler image
`walkero/amigagccondocker:os4-gcc15-3.6.1` (GCC 15, SDK 54.25, clib4).

Public domain, see `UNLICENSE`. libssh2 is BSD licensed (its licence goes
into the release archive).
