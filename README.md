# sftp-handler

An AmigaOS 4 filesystem handler for FTP and SFTP servers, and its prefs
program `sftphprefs`.

The handler shows one volume (for example `SFTP:`). Its root lists the
sites configured with sftphprefs, as folders, without connecting. Opening
a site's folder connects to its server and shows the files there. The
root itself is read-only: sites are added, changed and removed only with
sftphprefs.

Built on filesysbox.library, clib4, libssh2 and AmiSSL.

Download: [SFTPHandler on OS4Depot](https://os4depot.net/?function=showfile&file=driver/handler/sftphandler.lha)
(test versions: [GitHub releases](https://github.com/orgin/sftp-handler/releases)).

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
                started by the tests; `cap/`: OpenSSH whose read replies
                are cut to a cap (testserver/cap-data/cap/cap), with CBC
                ciphers allowed, for the cipher and short-read tests
- `package/`    release files: Install script, AmigaGuide, DOSDriver,
                readme, icons (`icons/README.md`), third-party licences
- `tools/`      build scripts, icon conversion (`make_icons.py`, `os4/mkicon.c`),
                `update_readme.py` (the changelog below, from the readme);
                `libssh2-patches/`: our changes to libssh2 1.11.1, applied by
                `build_deps_os4.sh` and `build_deps_linux.sh` (the Linux test
                build links the same patched libssh2, built by the Makefile)

## Building

    testserver/setup.sh            # once: start the test servers
    make test                      # Linux FUSE build, unit and server tests
    tools/build_deps_os4.sh        # libssh2 for clib4 with AmiSSL (build_os4.sh
                                   # runs it when a patch changed)
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
- libssh2 patches (`tools/libssh2-patches`, reported upstream as
  libssh2#2734 and #2735): 0001 decrypts the middle of a packet in one
  `crypt()` call (1.11.1 makes one call per 16-byte block for every cipher
  but chacha20-poly1305: aes128-ctr downloads 3.1 -> 8.45 MB/s in the
  guest); 0002 handles short read replies (1.11.1 fails with "Read Packet
  At Unexpected Offset" when a server gives less than asked for while
  more requests are in flight, even at its default 30000 bytes) and adds
  `libssh2_sftp_set_read_size()`: the handler asks for 255 KB per request
  (OpenSSH's maximum); a server that gives less makes the handle ask for
  less (learned only when the file goes on, so a short reply at the end of
  a file does not shrink it). Tests: `t_sftp_ciphers` (every cipher
  family) and `t_sftp_short_reads` (testserver/cap: OpenSSH behind a proxy
  that cuts read requests to a cap) on Linux, `tests/os4/ciphers.py` in the
  guest. DOSDriver option `CIPHERS` / `--ciphers` picks the ciphers.
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

## Changelog

<!-- changelog start: made by tools/update_readme.py from package/SFTPHandler.readme -->
- **0.15** (2026-11-18): faster SFTP downloads: each read request asks for 255 KB instead of 30000 bytes, and with AES ciphers (servers without chacha20-poly1305) a packet is decrypted in one go instead of 16 bytes at a time (about 2.7 times as fast). Servers that give less than asked for in one read reply work now. New DOSDriver option CIPHERS.
- **0.14** (2026-11-18): FTP: big folders on ZitaFTP Server no longer end in "No disk present", and open much faster. Workbench reads only the start of each file; ZitaFTP often sent no reply for such a cut download (the handler waited 5 seconds and logged in again) and sometimes one reply too many (the replies got out of step). Cut downloads now end with ABOR, and late replies are dropped.
- **0.13** (2026-11-18): clearer messages instead of "Error -1": a server that refuses to save a file gives "file is write protected" (also when the file is opened or saved), "file too large" gives "disk is full".
- **0.12** (2026-11-18): FTP: when the server has no data connection free ("425 cannot open data connection", for example ZitaFTP Server after many quick transfers), the handler waits and asks again instead of failing with "No disk present". Workbench reads the start of every file of a folder it opens, so a big folder needs many data connections.
- **0.11** (2026-11-17): SFTP two-factor logins (first in the 0.10 beta): the server's questions (such as "Verification code:") are asked with a requester, logins in two steps (key and code, key and password) work, a connection that needed a code stays open while unused (DOSDriver option CODEIDLETIME). Works with AmiSSL 6 and with every AmiSSL 5 (5.1 or newer): 0.9 needed AmiSSL 5.27 and said "needs AmiSSL 5 or newer" with an older one such as 5.25 or with only AmiSSL 6. New icons in the AmigaOS 4.1 style, and a volume icon for SFTP: (Install copies it to ENVARC:Sys/def_SFTP.info).
- **0.10 beta** (2026-11-17): BETA from the 2fa branch, not for OS4Depot. SFTP two-factor logins: the server's questions (such as "Verification code:") are asked with a requester, logins in two steps (key and code, key and password) work. A connection that needed a code stays open while unused (new DOSDriver option CODEIDLETIME). sftphprefs: Test connection asks the same questions.
- **0.9** (2026-11-15): first release. Install can update an installation that is in use (it stops SFTP: and mounts it again).
<!-- changelog end -->

Public domain, see `UNLICENSE`. libssh2 is BSD licensed (its licence goes
into the release archive).
