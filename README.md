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
- `tests/`      `os4/cryptobench.c`: how fast an Amiga decrypts with each
                SSH cipher (libssh2's chacha20-poly1305, AmiSSL's AES and
                HMACs); `unit_tests.c`, `run_tests.py` (Linux build against the
                Docker servers), `os4/` (guest test tools)
- `testserver/` throwaway FTP (vsftpd) and SFTP (OpenSSH) servers in Docker,
                and `otp/`: OpenSSH with two-factor logins (TOTP codes from
                `tests/totp.py`); `zitasim.py`: an FTP server that behaves
                like ZitaFTP Server (no reply or two replies after a cut
                download, "421 max clients exceeded"; optionally a small
                busy data port range: "425 cannot open data connection"),
                started by the tests
- `package/`    release files: Install script, AmigaGuide, DOSDriver,
                readme, icons (`icons/README.md`), third-party licences
- `tools/`      build scripts, icon conversion (`make_icons.py`, `os4/mkicon.c`)

## Building

    testserver/setup.sh            # once: start the test servers
    make test                      # Linux FUSE build, unit and server tests
    tools/build_deps_os4.sh        # once: libssh2 for clib4 with AmiSSL
    tools/build_os4.sh             # AmigaOS 4 handler and prefs (bumps the version)
    tests/os4/run_guest.py         # functional test in the QEMU guest (MCPd)
    tools/make_lha.sh              # release archive -> dist/SFTPHandler.lha

Builds and archives made off the `main` branch are betas: `build_os4.sh`
puts " beta" after the version string, `make_lha.sh` writes
`dist/SFTPHandler_beta.lha` (readme marked BETA), and
`tools/os4depot_submit.py` refuses to send anything that is not from main.
Betas never go to OS4Depot.

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
  Workbench reads the start of every file of a folder it opens, so a big
  folder means many downloads that are cut short: those end with ABOR,
  and replies that come after the one for ABOR are dropped (before each
  command, and late 225/226/426 are skipped). ZitaFTP Server often sends
  no reply for a cut download (the handler waited 5 s and logged in
  again before 0.14) and sometimes one reply too many (the replies got
  out of step: "No disk present" for everything after). "425 cannot open
  data connection" (a server's data port range used up) is waited out
  with growing pauses, up to the I/O timeout; then -EBUSY ("Object in
  use"), not EIO ("No disk present").
- SFTP (`sftp.c`): libssh2 in blocking mode with timeouts, server-side
  handles read and written at any offset. Host keys in OpenSSH
  known_hosts. `libssh2_keepalive_config()` must come after the
  handshake: set before it, the key exchange fails.
- Two-factor logins (SFTP): keyboard-interactive questions that are not the
  password go to `remote_env.ask` (a requester). libssh2 1.11 reports the
  first of two login steps (OpenSSH AuthenticationMethods) as a failure and
  drops the "partial success" flag, so `login()` asks the server for its
  methods again after each failed step and goes on with what is offered
  then. A refused code is asked again (three times) without asking for the
  password; Cancel works like a cancelled password requester. Connections
  whose login needed a code are kept while idle (CODEIDLETIME, default 0 =
  kept), as each new one needs a new code.
- AmiSSL (`src/os4/amissl.c`): one build works with AmiSSL 6 (OpenSSL 4,
  `AMISSL_V4xx`, used when amisslmaster is 6 or newer) and with every
  AmiSSL 5 (`AMISSL_V3xx` = 5.1; 0.9 asked for the SDK's
  `AMISSL_CURRENT_VERSION`, 5.27, and refused 5.25 or AmiSSL 6 alone).
  Their interface tables differ, so `-lamisslstubs` (fixed AmiSSL 5
  offsets) is not linked: `tools/gen_amissl_glue.py` makes a stub for each
  function in `tools/amissl_functions.txt` that calls through a table,
  filled at open time with the offsets of the opened interface
  (`offsetof()` compiled with the AmiSSL 5.27 and 6.1 headers). A function
  missing from the list shows as an undefined reference.
  `tools/check_amissl_api.py` (make_lha.sh runs it) checks that each one
  is in AmiSSL 5.1 at the same place and in AmiSSL 6 with the same
  prototype.
- chmod sends only the owner's r/w bits; group/other stay as they are.
- filesysbox maps EACCES, EIO, EXDEV... to "Error -1"; the glue maps them
  to the nearest AmigaDOS errors.

Public domain, see `UNLICENSE`. libssh2 is BSD licensed (its licence goes
into the release archive).
