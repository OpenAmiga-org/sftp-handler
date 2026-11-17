/* sftp-handler - Linux FUSE front end of the core, used to test the core
 * quickly against real servers. It is not part of the AmigaOS release.
 * Public domain.
 *
 *   sftph-fuse --config DIR [options] MOUNTPOINT [fuse options]
 *
 * DIR holds sites.xml (and known_hosts). Requesters are answered from the
 * environment: SFH_PASSWORD (password to type, unset = Cancel) and
 * SFH_HOSTKEY (save, once or refuse; default refuse).
 */
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <linux/fs.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "remote.h"
#include "sfs.h"
#include "platform.h"

static sfs *g_fs;
static int64_t g_last_idle;

static sfs_file *FH(struct fuse_file_info *fi)
{
	return fi != NULL ? (sfs_file *)(uintptr_t)fi->fh : NULL;
}

/* FUSE has no timer here: the idle work runs between requests */
static void tick(void)
{
	if (plat_ms() - g_last_idle > 5000) {
		g_last_idle = plat_ms();
		sfs_idle(g_fs);
	}
}

static void fill_stat(const sfs_stat *ns, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	if (ns->is_dir) {
		st->st_mode = S_IFDIR | (ns->mode & 0777);
		st->st_nlink = 2;
	} else {
		st->st_mode = S_IFREG | (ns->mode & 0777);
		st->st_nlink = 1;
	}
	st->st_size = (off_t)ns->size;
	st->st_mtime = ns->mtime;
	st->st_atime = ns->mtime;
	st->st_ctime = ns->mtime;
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_blksize = 4096;
	st->st_blocks = (st->st_size + 511) / 512;
}

static int op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	sfs_stat ns;
	int err;

	tick();
	err = FH(fi) != NULL ? sfs_fgetattr(g_fs, FH(fi), &ns) : sfs_getattr(g_fs, path, &ns);
	if (err == 0)
		fill_stat(&ns, st);
	return err;
}

struct fill_ctx {
	void            *buf;
	fuse_fill_dir_t  filler;
};

static int fill_one(void *ud, const char *name, const sfs_stat *ns)
{
	struct fill_ctx *c = ud;
	struct stat st;

	fill_stat(ns, &st);
	return c->filler(c->buf, name, &st, 0, 0);
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
                      struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	struct fill_ctx c = { buf, filler };

	(void)off;
	(void)fi;
	(void)flags;
	tick();
	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	return sfs_readdir(g_fs, path, fill_one, &c);
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
	sfs_file *f;
	int err;

	tick();
	err = sfs_open(g_fs, path, (fi->flags & O_ACCMODE) != O_RDONLY, &f);
	if (err != 0)
		return err;
	if (fi->flags & O_TRUNC) {
		err = sfs_ftruncate(g_fs, f, 0);
		if (err != 0) {
			sfs_release(g_fs, f);
			return err;
		}
	}
	fi->fh = (uint64_t)(uintptr_t)f;
	return 0;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	sfs_file *f;
	int err;

	(void)mode;
	tick();
	err = sfs_create(g_fs, path, &f);
	if (err == 0)
		fi->fh = (uint64_t)(uintptr_t)f;
	return err;
}

static int op_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi)
{
	(void)path;
	return sfs_read(g_fs, FH(fi), buf, size, off);
}

static int op_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi)
{
	(void)path;
	return sfs_write(g_fs, FH(fi), buf, size, off);
}

static int op_flush(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	/* close() waits for flush but not for release: upload here, so that
	 * close() returns after the upload, as Close() does on the Amiga */
	return sfs_flush(g_fs, FH(fi));
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	(void)datasync;
	return sfs_flush(g_fs, FH(fi));
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	sfs_release(g_fs, FH(fi));
	return 0;
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	if (FH(fi) != NULL)
		return sfs_ftruncate(g_fs, FH(fi), size);
	return sfs_truncate(g_fs, path, size);
}

static int op_mkdir(const char *path, mode_t mode)
{
	(void)mode;
	tick();
	return sfs_mkdir(g_fs, path);
}

static int op_unlink(const char *path)
{
	tick();
	return sfs_unlink(g_fs, path);
}

static int op_rmdir(const char *path)
{
	tick();
	return sfs_rmdir(g_fs, path);
}

static int op_rename(const char *from, const char *to, unsigned int flags)
{
	if (flags & RENAME_EXCHANGE)
		return -EINVAL;
	tick();
	return sfs_rename(g_fs, from, to);
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi;
	return sfs_chmod(g_fs, path, (int)(mode & 0777));
}

static int op_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi)
{
	time_t mtime;

	(void)fi;
	if (tv == NULL || tv[1].tv_nsec == UTIME_NOW)
		mtime = time(NULL);
	else if (tv[1].tv_nsec == UTIME_OMIT)
		return 0;
	else
		mtime = tv[1].tv_sec;
	return sfs_utimens(g_fs, path, mtime);
}

static int op_statfs(const char *path, struct statvfs *sv)
{
	int64_t total, avail;
	int err = sfs_statfs(g_fs, path, &total, &avail);

	if (err != 0)
		return err;
	memset(sv, 0, sizeof(*sv));
	sv->f_bsize = 4096;
	sv->f_frsize = 4096;
	sv->f_blocks = (fsblkcnt_t)(total / 4096);
	sv->f_bfree = (fsblkcnt_t)(avail / 4096);
	sv->f_bavail = sv->f_bfree;
	sv->f_namemax = 255;
	return 0;
}

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->attr_timeout = 0;
	cfg->entry_timeout = 0;
	cfg->negative_timeout = 0;
	cfg->direct_io = 1;
	return NULL;
}

static const struct fuse_operations ops = {
	.init        = op_init,
	.getattr     = op_getattr,
	.readdir     = op_readdir,
	.open        = op_open,
	.create      = op_create,
	.read        = op_read,
	.write       = op_write,
	.flush       = op_flush,
	.fsync       = op_fsync,
	.release     = op_release,
	.truncate    = op_truncate,
	.mkdir       = op_mkdir,
	.unlink      = op_unlink,
	.rmdir       = op_rmdir,
	.rename      = op_rename,
	.chmod       = op_chmod,
	.utimens     = op_utimens,
	.statfs      = op_statfs,
};

static void usage(void)
{
	fprintf(stderr,
	        "usage: sftph-fuse --config DIR [--icon FILE] [--cache-ms N] [--idle-ms N] [--code-idle-ms N]\n"
	        "         [--tempdir DIR] [--keepdir DIR] [--readonly] [--verbose|--debug]\n"
	        "         MOUNTPOINT [fuse options]\n"
	        "SFH_PASSWORD answers password requesters, SFH_HOSTKEY (save/once/refuse)\n"
	        "host key questions.\n");
}

int main(int argc, char **argv)
{
	static char kh[600];
	sfs_options fo;
	char *fargv[64];
	int fargc = 0, i, rc;

	memset(&fo, 0, sizeof(fo));
	fo.tempdir = "/tmp";
	fo.keepdir = "/tmp/sftph-kept";
	fo.display = "SFTP:";
	fargv[fargc++] = argv[0];
	fargv[fargc++] = "-s"; /* the core is single-threaded, like filesysbox */
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(a, "--config") == 0 && v) { fo.config_dir = v; fo.save_dir = NULL; i++; }
		else if (strcmp(a, "--icon") == 0 && v) { fo.icon_file = v; i++; }
		else if (strcmp(a, "--cache-ms") == 0 && v) { fo.cache_ms = atoi(v); i++; }
		else if (strcmp(a, "--idle-ms") == 0 && v) { fo.idle_ms = atoi(v); i++; }
		else if (strcmp(a, "--code-idle-ms") == 0 && v) { fo.code_idle_ms = atoi(v); i++; }
		else if (strcmp(a, "--timeout-ms") == 0 && v) { fo.io_timeout_ms = atoi(v); i++; }
		else if (strcmp(a, "--tempdir") == 0 && v) { fo.tempdir = v; i++; }
		else if (strcmp(a, "--keepdir") == 0 && v) { fo.keepdir = v; i++; }
		else if (strcmp(a, "--readonly") == 0) { fo.readonly = 1; }
		else if (strcmp(a, "--debug") == 0) { plat_loglevel = LOG_DEBUG; }
		else if (strcmp(a, "--verbose") == 0) { plat_loglevel = LOG_INFO; }
		else if (fargc < 60) { fargv[fargc++] = argv[i]; }
	}
	fargv[fargc] = NULL;
	if (fo.config_dir == NULL || fargc < 3) {
		usage();
		return 2;
	}
	snprintf(kh, sizeof(kh), "%s/known_hosts", fo.config_dir);
	fo.known_hosts = kh;
	if (sftp_global_init() != 0) {
		fprintf(stderr, "libssh2 cannot be set up\n");
		return 1;
	}
	g_fs = sfs_new(&fo);
	if (g_fs == NULL) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	rc = fuse_main(fargc, fargv, &ops, NULL);
	sfs_free(g_fs);
	sftp_global_exit();
	return rc;
}
