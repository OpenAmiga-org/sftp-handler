/* sftp-handler - the file system the handler shows: the root lists the
 * configured sites, each site is a folder that shows the files on its
 * server. The OS4 (filesysbox) and Linux (FUSE) front ends call these.
 * Paths are UTF-8, "/" is the root. Errors are -errno. Public domain. */
#ifndef SFH_SFS_H
#define SFH_SFS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
	int     is_dir;
	int64_t size;
	time_t  mtime;      /* UTC, 0 = not known */
	int     mode;       /* permission bits (0777) */
	int     readonly;   /* cannot be changed (the root and its entries) */
} sfs_stat;

typedef struct {
	const char *config_dir;   /* sites.xml, passwords.xml: "ENV:SFTPHandler" */
	const char *save_dir;     /* remembered passwords also go here: "ENVARC:SFTPHandler" */
	const char *known_hosts;  /* SFTP host keys (OpenSSH format) */
	const char *icon_file;    /* drawer icon shown for each site, NULL = none */
	const char *tempdir;      /* FTP upload buffers */
	const char *keepdir;      /* failed uploads are saved here */
	const char *display;      /* shown in messages, e.g. "SFTP:" */
	int         cache_ms;     /* how long server listings are trusted */
	int         idle_ms;      /* connections unused this long are closed */
	int         code_idle_ms; /* the same for those whose login needed a code
	                           * (two-factor); 0 = kept until the handler stops */
	int         connect_timeout_ms;
	int         io_timeout_ms;
	const char *ciphers;      /* SFTP: cipher order (libssh2 names, commas), NULL = default */
	int         readonly;     /* refuse all changes */
} sfs_options;

typedef struct sfs sfs;
typedef struct sfs_file sfs_file;

typedef int (*sfs_fill_fn)(void *ud, const char *name, const sfs_stat *st);

sfs  *sfs_new(const sfs_options *opt);
void  sfs_free(sfs *fs);

int sfs_getattr(sfs *fs, const char *path, sfs_stat *st);
int sfs_readdir(sfs *fs, const char *path, sfs_fill_fn fill, void *ud);
int sfs_statfs(sfs *fs, const char *path, int64_t *total, int64_t *avail);

/* write: the file may be written (it is opened for reading and writing) */
int sfs_open(sfs *fs, const char *path, int write, sfs_file **fh);
int sfs_create(sfs *fs, const char *path, sfs_file **fh);
int sfs_read(sfs *fs, sfs_file *fh, char *buf, size_t size, int64_t off);
int sfs_write(sfs *fs, sfs_file *fh, const char *buf, size_t size, int64_t off);
int sfs_fgetattr(sfs *fs, sfs_file *fh, sfs_stat *st);
int sfs_ftruncate(sfs *fs, sfs_file *fh, int64_t size);
int sfs_flush(sfs *fs, sfs_file *fh);      /* FTP: uploads pending data */
int sfs_release(sfs *fs, sfs_file *fh);    /* uploads and frees */

int sfs_truncate(sfs *fs, const char *path, int64_t size);
int sfs_mkdir(sfs *fs, const char *path);
int sfs_unlink(sfs *fs, const char *path);
int sfs_rmdir(sfs *fs, const char *path);
int sfs_rename(sfs *fs, const char *from, const char *to);
int sfs_utimens(sfs *fs, const char *path, time_t mtime);
int sfs_chmod(sfs *fs, const char *path, int mode);

/* Call every few seconds: closes idle connections, keeps others alive. */
void sfs_idle(sfs *fs);
/* "DiskChange": asks again for passwords that were cancelled, shows
 * connection problems again, reads the site list again. */
void sfs_reset(sfs *fs);
/* Number of connected sites. */
int  sfs_connected(sfs *fs);

#endif
