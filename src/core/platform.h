/* sftp-handler - what the core needs from the operating system.
 * Implemented in src/os4/platform_os4.c and src/linux/platform_linux.c.
 * Public domain. */
#ifndef SFH_PLATFORM_H
#define SFH_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Temporary file used to buffer FTP uploads. 64-bit offsets. */
typedef struct tmpf tmpf;

tmpf   *tmpf_create(const char *dir, int *err);        /* err = -errno */
int     tmpf_pread(tmpf *f, void *buf, size_t len, int64_t off);
int     tmpf_pwrite(tmpf *f, const void *buf, size_t len, int64_t off);
int     tmpf_truncate(tmpf *f, int64_t size);
int64_t tmpf_size(tmpf *f);
/* Moves the file to dir/name instead of deleting it (used when an upload
 * failed). The final path is written to out. Returns 0 or -errno. */
int     tmpf_keep(tmpf *f, const char *dir, const char *name, char *out, size_t outlen);
void    tmpf_close(tmpf *f);                            /* deletes the file */

/* Wall clock (UTC seconds since 1970) and a monotonic millisecond clock. */
time_t  plat_time(void);
int64_t plat_ms(void);

enum { LOG_ERR = 0, LOG_INFO = 1, LOG_DEBUG = 2 };
extern int plat_loglevel;
void    plat_log(int level, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

/* Tells the user about a problem (title and message in UTF-8). */
void    plat_report(const char *title, const char *message);

/* Asks for the password of a site. problem: why it is asked again (or
 * NULL); keyfile: the passphrase of this key file is wanted instead.
 * Returns 1 for OK (password in buf, *remember set), 0 for Cancel. */
int     plat_ask_password(const char *site, const char *user, const char *host,
                          const char *keyfile, const char *problem,
                          char *buf, size_t buflen, int *remember);

/* An SFTP server asks something during the login that is not the
 * password, usually the code of a two-factor login. name, instruction:
 * the server's title and text (may be empty); prompt: its question; echo:
 * the answer may be shown while typed; problem: why it is asked again (or
 * NULL). Returns 1 for OK (answer in buf), 0 for Cancel. */
int     plat_ask_answer(const char *site, const char *user, const char *host,
                        const char *name, const char *instruction, const char *prompt,
                        int echo, const char *problem, char *buf, size_t buflen);

/* An SFTP server's host key is new (mismatch 0) or has changed
 * (mismatch 1). Returns 0 = refuse, 1 = accept once, 2 = accept and save. */
int     plat_ask_hostkey(const char *site, const char *host, int port,
                         const char *keytype, const char *fingerprint, int mismatch);

#endif
