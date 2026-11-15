/* sftp-handler - one connection to a site, FTP or SFTP. The site file
 * system (sitefs.c) uses these. Paths are UTF-8 and absolute on the
 * server ("/home/me/file"). Errors are -errno; -ENOTCONN means the
 * connection is gone and must be made again. Public domain. */
#ifndef SFH_REMOTE_H
#define SFH_REMOTE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "config.h"

typedef struct {
	int     is_dir;
	int64_t size;
	time_t  mtime;     /* UTC, 0 = not known */
	int     mode;      /* permission bits (0777), -1 = not known */
} rstat;

typedef int (*rlist_fn)(void *ud, const char *name, const rstat *st);

/* What the connection needs from the one using it. */
typedef struct {
	void *ud;
	/* SFTP: the server's host key is not known (mismatch = 0) or differs
	 * from the saved one (mismatch = 1). fingerprint: "SHA256:..." as
	 * OpenSSH shows it. Returns 0 = refuse, 1 = accept once, 2 = accept
	 * and save. */
	int (*hostkey)(void *ud, const char *host, int port, const char *keytype,
	               const char *fingerprint, int mismatch);
	/* where known_hosts is kept (OpenSSH format) */
	const char *known_hosts;
	int connect_timeout_ms;
	int io_timeout_ms;
} remote_env;

/* Why a connection failed. */
enum {
	RFAIL_NONE = 0,
	RFAIL_NETWORK,     /* name not found, refused, timeout: may pass */
	RFAIL_LOGIN,       /* user name or password not accepted */
	RFAIL_HOSTKEY,     /* host key refused or changed */
	RFAIL_KEYFILE,     /* the private key file cannot be used */
	RFAIL_PROTOCOL,    /* the server does not speak the protocol */
	RFAIL_FOLDER       /* the start folder does not exist */
};

typedef struct remote remote;
typedef struct rhandle rhandle;

/* open modes */
enum { RO_READ = 0, RO_WRITE = 1, RO_CREATE = 2 };

/* uploads read their data through this; returns bytes, 0 at the end, or
 * -errno */
typedef int (*rsource_fn)(void *ud, char *buf, size_t len);

typedef struct {
	void (*close)(remote *r);
	int  (*list)(remote *r, const char *path, rlist_fn fn, void *ud);
	/* -ENOSYS when the protocol has no reliable way */
	int  (*stat)(remote *r, const char *path, rstat *st);
	int  (*mkdir)(remote *r, const char *path);
	int  (*rmdir)(remote *r, const char *path);
	int  (*unlink)(remote *r, const char *path);
	int  (*rename)(remote *r, const char *from, const char *to);
	int  (*set_mtime)(remote *r, const char *path, time_t t);
	int  (*chmod)(remote *r, const char *path, int mode);
	int  (*truncate)(remote *r, const char *path, int64_t size);
	int  (*statfs)(remote *r, const char *path, int64_t *total, int64_t *avail);
	/* files: SFTP has handles that can be read and written anywhere;
	 * FTP handles only read (a download from the offset on), and new
	 * contents go up in one piece with upload. */
	int  (*open)(remote *r, const char *path, int mode, rhandle **h);
	int  (*pread)(remote *r, rhandle *h, char *buf, size_t len, int64_t off);
	int  (*pwrite)(remote *r, rhandle *h, const char *buf, size_t len, int64_t off);
	int  (*ftruncate)(remote *r, rhandle *h, int64_t size);
	int  (*fclose)(remote *r, rhandle *h);
	int  (*upload)(remote *r, const char *path, rsource_fn src, void *ud);
	/* called now and then while idle; 0, or -ENOTCONN when it is gone */
	int  (*idle)(remote *r);
	int  random_write;    /* pwrite works */
} remote_ops;

struct remote {
	const remote_ops *ops;
	char  home[512];      /* the site's folder on the server, no trailing '/' */
	char  error[512];     /* last error text from the server */
	int   fail;           /* RFAIL_* of the last connect */
};

/* Connects and logs in. password may be NULL. On failure *out is NULL,
 * the error text goes to err and the kind to *fail. */
int  ftp_connect(const site_cfg *site, const char *password, const remote_env *env,
                 remote **out, char *err, size_t errlen, int *fail);
int  sftp_connect(const site_cfg *site, const char *password, const remote_env *env,
                  remote **out, char *err, size_t errlen, int *fail);
/* SFTP: library set-up for the whole program */
int  sftp_global_init(void);
void sftp_global_exit(void);

#endif
