/* sftp-handler - SFTP through libssh2. Public domain. */
#include "remote.h"
#include "net.h"
#include "platform.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

typedef struct {
	remote           base;
	site_cfg         site;
	char             password[256];
	remote_env       env;
	int              sock;
	LIBSSH2_SESSION *ss;
	LIBSSH2_SFTP    *sftp;
	int              dead;
	int              statvfs;      /* 1 works, 0 does not, -1 not tried */
} sftp_remote;

/* Reads and writes are each a round trip to the server. Amiga programs
 * often read and write a few KB at a time, so a handle reads ahead in
 * growing blocks and collects writes, and libssh2 then keeps many requests
 * in flight. */
#define RA_MIN   (32 * 1024)     /* first read-ahead block */
#define RA_MAX   (512 * 1024)    /* largest; libssh2 asks for 4x ahead */
#define WB_SIZE  (1024 * 1024)   /* write-behind buffer */

struct rhandle {
	LIBSSH2_SFTP_HANDLE *h;
	int64_t              pos;      /* libssh2's position, -1 = unknown */
	/* read-ahead */
	char                *rb;
	size_t               rb_len;
	int64_t              rb_off;
	size_t               ra;       /* current block size, grows */
	int64_t              next;     /* where a sequential read continues */
	/* write-behind */
	char                *wb;
	size_t               wb_len;
	int64_t              wb_off;
};

static int global_ready;

int sftp_global_init(void)
{
	if (!global_ready && libssh2_init(0) != 0)
		return -1;
	global_ready = 1;
	return 0;
}

void sftp_global_exit(void)
{
	if (global_ready)
		libssh2_exit();
	global_ready = 0;
}

/* --- errors ------------------------------------------------------------------ */

static int fx_err(unsigned long fx)
{
	switch (fx) {
	case LIBSSH2_FX_NO_SUCH_FILE:
	case LIBSSH2_FX_NO_SUCH_PATH:          return -ENOENT;
	case LIBSSH2_FX_PERMISSION_DENIED:
	case LIBSSH2_FX_UNKNOWN_PRINCIPAL:     return -EACCES;
	case LIBSSH2_FX_NO_CONNECTION:
	case LIBSSH2_FX_CONNECTION_LOST:       return -ENOTCONN;
	case LIBSSH2_FX_OP_UNSUPPORTED:        return -ENOSYS;
	case LIBSSH2_FX_INVALID_HANDLE:        return -EBADF;
	case LIBSSH2_FX_FILE_ALREADY_EXISTS:   return -EEXIST;
	case LIBSSH2_FX_WRITE_PROTECT:         return -EROFS;
	case LIBSSH2_FX_NO_MEDIA:              return -ENODEV;
	case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
	case LIBSSH2_FX_QUOTA_EXCEEDED:        return -ENOSPC;
	case LIBSSH2_FX_LOCK_CONFLICT:         return -EBUSY;
	case LIBSSH2_FX_DIR_NOT_EMPTY:         return -ENOTEMPTY;
	case LIBSSH2_FX_NOT_A_DIRECTORY:       return -ENOTDIR;
	case LIBSSH2_FX_INVALID_FILENAME:      return -EINVAL;
	case LIBSSH2_FX_LINK_LOOP:             return -ELOOP;
	}
	return -EIO;
}

/* the error of the last failed call */
static int last_err(sftp_remote *sr)
{
	char *msg = NULL;
	int rc = libssh2_session_last_error(sr->ss, &msg, NULL, 0);

	if (rc == LIBSSH2_ERROR_SFTP_PROTOCOL) {
		unsigned long fx = libssh2_sftp_last_error(sr->sftp);

		snprintf(sr->base.error, sizeof(sr->base.error), "SFTP status %lu", fx);
		return fx_err(fx);
	}
	if (msg != NULL)
		snprintf(sr->base.error, sizeof(sr->base.error), "%s", msg);
	switch (rc) {
	case LIBSSH2_ERROR_ALLOC:
		return -ENOMEM;
	case LIBSSH2_ERROR_NONE:
		return -EIO;
	case LIBSSH2_ERROR_TIMEOUT:
	case LIBSSH2_ERROR_SOCKET_SEND:
	case LIBSSH2_ERROR_SOCKET_RECV:
	case LIBSSH2_ERROR_SOCKET_DISCONNECT:
	case LIBSSH2_ERROR_SOCKET_TIMEOUT:
	case LIBSSH2_ERROR_CHANNEL_CLOSED:
	case LIBSSH2_ERROR_CHANNEL_EOF_SENT:
	case LIBSSH2_ERROR_CHANNEL_FAILURE:
	case LIBSSH2_ERROR_BAD_SOCKET:
	case LIBSSH2_ERROR_KEX_FAILURE:
	case LIBSSH2_ERROR_DECRYPT:
	case LIBSSH2_ERROR_PROTO:
	default:
		plat_log(LOG_INFO, "sftp %s: connection lost (%d %s)\n", sr->site.name, rc,
		         msg != NULL ? msg : "");
		sr->dead = 1;
		return -ENOTCONN;
	}
}

#define ALIVE(sr) do { if ((sr)->dead) return -ENOTCONN; } while (0)

static char *server_path(sftp_remote *sr, const char *path)
{
	strbuf sb;

	sb_init(&sb);
	sb_puts(&sb, sr->base.home);
	if (strcmp(path, "/") != 0 || sr->base.home[0] == '\0')
		sb_puts(&sb, path);
	return sb.failed ? (sb_free(&sb), (char *)NULL) : sb_steal(&sb);
}

static void to_rstat(const LIBSSH2_SFTP_ATTRIBUTES *a, rstat *st)
{
	memset(st, 0, sizeof(*st));
	st->mode = -1;
	if (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
		st->is_dir = LIBSSH2_SFTP_S_ISDIR(a->permissions);
		st->mode = (int)(a->permissions & 07777);
	}
	if (a->flags & LIBSSH2_SFTP_ATTR_SIZE)
		st->size = st->is_dir ? 0 : (int64_t)a->filesize;
	if (a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
		st->mtime = (time_t)a->mtime;
}

/* --- operations -------------------------------------------------------------- */

static void sftp_close(remote *r)
{
	sftp_remote *sr = (sftp_remote *)r;

	if (sr->sftp != NULL && !sr->dead)
		libssh2_sftp_shutdown(sr->sftp);
	sr->sftp = NULL;
	if (sr->ss != NULL) {
		if (!sr->dead) {
			/* do not wait long for a server that is going away */
			libssh2_session_set_timeout(sr->ss, 3000);
			libssh2_session_disconnect(sr->ss, "bye");
		}
		libssh2_session_free(sr->ss);
	}
	net_close(sr->sock);
	memset(sr->password, 0, sizeof(sr->password));
	free(sr);
}

static int sftp_list(remote *r, const char *path, rlist_fn fn, void *ud)
{
	sftp_remote *sr = (sftp_remote *)r;
	LIBSSH2_SFTP_HANDLE *h;
	LIBSSH2_SFTP_ATTRIBUTES a;
	char *sp, *name, *full;
	size_t splen;
	int n, rc = 0;

	ALIVE(sr);
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	h = libssh2_sftp_opendir(sr->sftp, sp[0] != '\0' ? sp : "/");
	if (h == NULL) {
		free(sp);
		return last_err(sr);
	}
	name = malloc(1024);
	full = malloc(2048);
	if (name == NULL || full == NULL) {
		free(name);
		free(full);
		libssh2_sftp_closedir(h);
		free(sp);
		return -ENOMEM;
	}
	splen = strlen(sp);
	while ((n = libssh2_sftp_readdir(h, name, 1024, &a)) > 0) {
		rstat st;

		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		if ((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(a.permissions)) {
			/* show what the link points to */
			LIBSSH2_SFTP_ATTRIBUTES t;

			snprintf(full, 2048, "%s/%s", splen > 0 ? sp : "", name);
			if (libssh2_sftp_stat(sr->sftp, full, &t) == 0) {
				a = t;
			} else {
				a.permissions = LIBSSH2_SFTP_S_IFREG | 0444;
				a.filesize = 0;
			}
		}
		to_rstat(&a, &st);
		if (fn(ud, name, &st) != 0)
			break;
	}
	if (n < 0)
		rc = last_err(sr);
	free(name);
	free(full);
	if (!sr->dead)
		libssh2_sftp_closedir(h);
	free(sp);
	return rc;
}

static int sftp_stat(remote *r, const char *path, rstat *st)
{
	sftp_remote *sr = (sftp_remote *)r;
	LIBSSH2_SFTP_ATTRIBUTES a;
	char *sp;
	int rc;

	ALIVE(sr);
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	rc = libssh2_sftp_stat(sr->sftp, sp[0] != '\0' ? sp : "/", &a);
	free(sp);
	if (rc != 0)
		return last_err(sr);
	to_rstat(&a, st);
	return 0;
}

static int sftp_mkdir(remote *r, const char *path)
{
	sftp_remote *sr = (sftp_remote *)r;
	char *sp;
	int rc;

	ALIVE(sr);
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	rc = libssh2_sftp_mkdir(sr->sftp, sp, 0755);
	free(sp);
	return rc == 0 ? 0 : last_err(sr);
}

static int sftp_rmdir(remote *r, const char *path)
{
	sftp_remote *sr = (sftp_remote *)r;
	char *sp;
	int rc;

	ALIVE(sr);
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	rc = libssh2_sftp_rmdir(sr->sftp, sp);
	free(sp);
	return rc == 0 ? 0 : last_err(sr);
}

static int sftp_unlink(remote *r, const char *path)
{
	sftp_remote *sr = (sftp_remote *)r;
	char *sp;
	int rc;

	ALIVE(sr);
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	rc = libssh2_sftp_unlink(sr->sftp, sp);
	free(sp);
	return rc == 0 ? 0 : last_err(sr);
}

static int sftp_rename(remote *r, const char *from, const char *to)
{
	sftp_remote *sr = (sftp_remote *)r;
	char *sf, *st;
	int rc;

	ALIVE(sr);
	sf = server_path(sr, from);
	st = server_path(sr, to);
	if (sf == NULL || st == NULL) {
		free(sf);
		free(st);
		return -ENOMEM;
	}
	rc = libssh2_sftp_rename_ex(sr->sftp, sf, (unsigned)strlen(sf), st, (unsigned)strlen(st),
	                            LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE);
	free(sf);
	free(st);
	return rc == 0 ? 0 : last_err(sr);
}

static int set_attrs(sftp_remote *sr, const char *path, LIBSSH2_SFTP_ATTRIBUTES *a)
{
	char *sp;
	int rc;

	ALIVE(sr);
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	rc = libssh2_sftp_setstat(sr->sftp, sp, a);
	free(sp);
	return rc == 0 ? 0 : last_err(sr);
}

static int sftp_set_mtime(remote *r, const char *path, time_t t)
{
	LIBSSH2_SFTP_ATTRIBUTES a;

	memset(&a, 0, sizeof(a));
	a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
	a.atime = (unsigned long)t;
	a.mtime = (unsigned long)t;
	return set_attrs((sftp_remote *)r, path, &a);
}

static int sftp_chmod(remote *r, const char *path, int mode)
{
	LIBSSH2_SFTP_ATTRIBUTES a;

	memset(&a, 0, sizeof(a));
	a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
	a.permissions = (unsigned long)(mode & 07777);
	return set_attrs((sftp_remote *)r, path, &a);
}

static int sftp_truncate(remote *r, const char *path, int64_t size)
{
	LIBSSH2_SFTP_ATTRIBUTES a;

	memset(&a, 0, sizeof(a));
	a.flags = LIBSSH2_SFTP_ATTR_SIZE;
	a.filesize = (libssh2_uint64_t)size;
	return set_attrs((sftp_remote *)r, path, &a);
}

static int sftp_statfs(remote *r, const char *path, int64_t *total, int64_t *avail)
{
	sftp_remote *sr = (sftp_remote *)r;
	LIBSSH2_SFTP_STATVFS v;
	char *sp;
	int rc;

	ALIVE(sr);
	if (sr->statvfs == 0)
		return -ENOSYS;
	sp = server_path(sr, path);
	if (sp == NULL)
		return -ENOMEM;
	if (sp[0] == '\0') {
		free(sp);
		sp = xstrdup("/");
		if (sp == NULL)
			return -ENOMEM;
	}
	rc = libssh2_sftp_statvfs(sr->sftp, sp, strlen(sp), &v);
	free(sp);
	if (rc != 0) {
		rc = last_err(sr);
		if (rc != -ENOTCONN)
			sr->statvfs = 0;
		return rc == -ENOTCONN ? rc : -ENOSYS;
	}
	sr->statvfs = 1;
	*total = (int64_t)v.f_blocks * (int64_t)(v.f_frsize ? v.f_frsize : v.f_bsize);
	*avail = (int64_t)v.f_bavail * (int64_t)(v.f_frsize ? v.f_frsize : v.f_bsize);
	return 0;
}

/* --- files -------------------------------------------------------------------- */

static int sftp_open(remote *r, const char *path, int mode, rhandle **out)
{
	sftp_remote *sr = (sftp_remote *)r;
	unsigned long flags = LIBSSH2_FXF_READ;
	rhandle *h;
	char *sp;

	ALIVE(sr);
	if (mode == RO_WRITE)
		flags |= LIBSSH2_FXF_WRITE;
	else if (mode == RO_CREATE)
		flags |= LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC;
	h = calloc(1, sizeof(*h));
	sp = server_path(sr, path);
	if (h == NULL || sp == NULL) {
		free(h);
		free(sp);
		return -ENOMEM;
	}
	h->h = libssh2_sftp_open(sr->sftp, sp, flags, 0644);
	free(sp);
	if (h->h == NULL) {
		int rc = last_err(sr);

		free(h);
		return rc;
	}
	*out = h;
	return 0;
}

static void seek_to(rhandle *h, int64_t off)
{
	if (h->pos != off) {
		libssh2_sftp_seek64(h->h, (libssh2_uint64_t)off);
		h->pos = off;
	}
}

/* reads len bytes at off (less at the end of the file) */
static int raw_read(sftp_remote *sr, rhandle *h, char *buf, size_t len, int64_t off)
{
	size_t got = 0;

	seek_to(h, off);
	while (got < len) {
		ssize_t n = libssh2_sftp_read(h->h, buf + got, len - got);

		if (n == 0)
			break;
		if (n < 0) {
			int rc = last_err(sr);

			/* the next read must set the position again */
			h->pos = -1;
			return got > 0 ? (int)got : rc;
		}
		got += (size_t)n;
		h->pos += n;
	}
	return (int)got;
}

static int raw_write(sftp_remote *sr, rhandle *h, const char *buf, size_t len, int64_t off)
{
	size_t done = 0;

	seek_to(h, off);
	while (done < len) {
		/* libssh2 sends the whole buffer at once and returns when the
		 * first part is acknowledged; the rest stays in flight */
		ssize_t n = libssh2_sftp_write(h->h, buf + done, len - done);

		if (n <= 0) {
			int rc = n < 0 ? last_err(sr) : -EIO;

			h->pos = -1;
			return rc;
		}
		done += (size_t)n;
		h->pos += n;
	}
	return (int)done;
}

static int write_flush(sftp_remote *sr, rhandle *h)
{
	int rc = 0;

	if (h->wb_len > 0) {
		rc = raw_write(sr, h, h->wb, h->wb_len, h->wb_off);
		h->wb_len = 0;
		if (rc > 0)
			rc = 0;
	}
	return rc;
}

static int sftp_pread(remote *r, rhandle *h, char *buf, size_t len, int64_t off)
{
	sftp_remote *sr = (sftp_remote *)r;
	size_t got = 0;
	int rc;

	ALIVE(sr);
	rc = write_flush(sr, h);
	if (rc != 0)
		return rc;
	/* grow the blocks while the reads follow each other, start small
	 * again after a jump */
	if (off == h->next) {
		if (h->ra < RA_MAX)
			h->ra = h->ra == 0 ? RA_MIN : h->ra * 2;
	} else {
		h->ra = RA_MIN;
	}
	h->next = off + (int64_t)len;
	while (got < len) {
		int64_t at = off + (int64_t)got;
		size_t want = len - got;

		if (h->rb_len > 0 && at >= h->rb_off && at < h->rb_off + (int64_t)h->rb_len) {
			size_t from = (size_t)(at - h->rb_off), n = h->rb_len - from;

			if (n > want)
				n = want;
			memcpy(buf + got, h->rb + from, n);
			got += n;
			continue;
		}
		if (want >= h->ra) {
			/* big enough by itself: straight into the caller's buffer */
			rc = raw_read(sr, h, buf + got, want, at);
			if (rc < 0)
				return got > 0 ? (int)got : rc;
			got += (size_t)rc;
			break;
		}
		if (h->rb == NULL) {
			h->rb = malloc(RA_MAX);
			if (h->rb == NULL)
				return got > 0 ? (int)got : -ENOMEM;
		}
		h->rb_len = 0;
		rc = raw_read(sr, h, h->rb, h->ra, at);
		if (rc < 0)
			return got > 0 ? (int)got : rc;
		h->rb_off = at;
		h->rb_len = (size_t)rc;
		if (rc == 0)
			break; /* end of the file */
	}
	return (int)got;
}

static int sftp_pwrite(remote *r, rhandle *h, const char *buf, size_t len, int64_t off)
{
	sftp_remote *sr = (sftp_remote *)r;
	int rc;

	ALIVE(sr);
	/* what was read ahead may change */
	h->rb_len = 0;
	if (h->wb_len > 0 && (off != h->wb_off + (int64_t)h->wb_len || h->wb_len + len > WB_SIZE)) {
		rc = write_flush(sr, h);
		if (rc != 0)
			return rc;
	}
	if (len >= WB_SIZE)
		return raw_write(sr, h, buf, len, off);
	if (h->wb == NULL) {
		h->wb = malloc(WB_SIZE);
		if (h->wb == NULL)
			return raw_write(sr, h, buf, len, off);
	}
	if (h->wb_len == 0)
		h->wb_off = off;
	memcpy(h->wb + h->wb_len, buf, len);
	h->wb_len += len;
	return (int)len;
}

static int sftp_fflush(remote *r, rhandle *h)
{
	sftp_remote *sr = (sftp_remote *)r;

	if (h->wb_len == 0)
		return 0;
	ALIVE(sr);
	return write_flush(sr, h);
}

static int sftp_ftruncate(remote *r, rhandle *h, int64_t size)
{
	sftp_remote *sr = (sftp_remote *)r;
	LIBSSH2_SFTP_ATTRIBUTES a;
	int rc;

	ALIVE(sr);
	rc = write_flush(sr, h);
	if (rc != 0)
		return rc;
	h->rb_len = 0;
	memset(&a, 0, sizeof(a));
	a.flags = LIBSSH2_SFTP_ATTR_SIZE;
	a.filesize = (libssh2_uint64_t)size;
	if (libssh2_sftp_fsetstat(h->h, &a) != 0)
		return last_err(sr);
	return 0;
}

static int sftp_fclose(remote *r, rhandle *h)
{
	sftp_remote *sr = (sftp_remote *)r;
	int rc = 0;

	if (!sr->dead) {
		rc = write_flush(sr, h);
		if (!sr->dead && libssh2_sftp_close(h->h) != 0 && rc == 0) {
			rc = last_err(sr);
			/* everything sent was acknowledged: a connection lost only
			 * now loses nothing (the server closes the file itself) */
			if (rc == -ENOTCONN)
				rc = 0;
		}
	} else if (h->wb_len > 0) {
		rc = -ENOTCONN;   /* collected data could not be sent */
	}
	free(h->rb);
	free(h->wb);
	free(h);
	return rc;
}

static int sftp_upload(remote *r, const char *path, rsource_fn src, void *ud)
{
	(void)r;
	(void)path;
	(void)src;
	(void)ud;
	return -ENOSYS;
}

static int sftp_idle(remote *r)
{
	sftp_remote *sr = (sftp_remote *)r;
	int next = 0;

	ALIVE(sr);
	if (libssh2_keepalive_send(sr->ss, &next) != 0)
		return last_err(sr);
	return 0;
}

static const remote_ops sftp_ops = {
	.close     = sftp_close,
	.list      = sftp_list,
	.stat      = sftp_stat,
	.mkdir     = sftp_mkdir,
	.rmdir     = sftp_rmdir,
	.unlink    = sftp_unlink,
	.rename    = sftp_rename,
	.set_mtime = sftp_set_mtime,
	.chmod     = sftp_chmod,
	.truncate  = sftp_truncate,
	.statfs    = sftp_statfs,
	.open      = sftp_open,
	.pread     = sftp_pread,
	.pwrite    = sftp_pwrite,
	.ftruncate = sftp_ftruncate,
	.fflush    = sftp_fflush,
	.fclose    = sftp_fclose,
	.upload    = sftp_upload,
	.idle      = sftp_idle,
	.random_write = 1,
};

/* --- connecting ------------------------------------------------------------- */

static void b64(const unsigned char *in, size_t len, char *out, size_t outlen)
{
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;

	for (i = 0; i < len && o + 5 < outlen; i += 3) {
		unsigned v = (unsigned)in[i] << 16;
		int n = 1;

		if (i + 1 < len) {
			v |= (unsigned)in[i + 1] << 8;
			n++;
		}
		if (i + 2 < len) {
			v |= in[i + 2];
			n++;
		}
		out[o++] = tab[(v >> 18) & 63];
		out[o++] = tab[(v >> 12) & 63];
		if (n > 1)
			out[o++] = tab[(v >> 6) & 63];
		if (n > 2)
			out[o++] = tab[v & 63];
	}
	out[o] = '\0';
}

static int knownhost_keybit(int type)
{
	switch (type) {
	case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
	case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
	case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
	}
	return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
}

static const char *key_type_name(int type)
{
	switch (type) {
	case LIBSSH2_HOSTKEY_TYPE_RSA:       return "RSA";
	case LIBSSH2_HOSTKEY_TYPE_DSS:       return "DSA";
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
	case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA";
	case LIBSSH2_HOSTKEY_TYPE_ED25519:   return "ED25519";
	}
	return "unknown";
}

static int check_hostkey(sftp_remote *sr, char *err, size_t errlen)
{
	const site_cfg *s = &sr->site;
	LIBSSH2_KNOWNHOSTS *kh;
	struct libssh2_knownhost *found = NULL;
	const char *key, *hash;
	char fp[80], spec[300];
	size_t keylen;
	int type, check, answer, bit, port = site_port(s);

	key = libssh2_session_hostkey(sr->ss, &keylen, &type);
	hash = libssh2_hostkey_hash(sr->ss, LIBSSH2_HOSTKEY_HASH_SHA256);
	if (key == NULL || hash == NULL) {
		snprintf(err, errlen, "The server sent no host key.");
		return -EPROTO;
	}
	strcpy(fp, "SHA256:");
	b64((const unsigned char *)hash, 32, fp + 7, sizeof(fp) - 7);
	bit = knownhost_keybit(type);

	kh = libssh2_knownhost_init(sr->ss);
	if (kh == NULL) {
		snprintf(err, errlen, "Out of memory.");
		return -ENOMEM;
	}
	if (sr->env.known_hosts != NULL)
		libssh2_knownhost_readfile(kh, sr->env.known_hosts, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
	check = libssh2_knownhost_checkp(kh, s->host, port, key, keylen,
	                                 LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | bit,
	                                 &found);
	plat_log(LOG_INFO, "sftp %s: host key %s %s check %d\n", s->name, key_type_name(type), fp, check);
	if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
		libssh2_knownhost_free(kh);
		return 0;
	}
	answer = 0;
	if (sr->env.hostkey != NULL)
		answer = sr->env.hostkey(sr->env.ud, s->host, port, key_type_name(type), fp,
		                         check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH);
	if (answer == 0) {
		snprintf(err, errlen, check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH ?
		         "The host key of %s has changed (%s %s). The connection was refused." :
		         "The host key of %s (%s %s) was not accepted.",
		         s->host, key_type_name(type), fp);
		libssh2_knownhost_free(kh);
		return -EACCES;
	}
	if (answer == 2 && sr->env.known_hosts != NULL) {
		if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH && found != NULL)
			libssh2_knownhost_del(kh, found);
		if (port == 22)
			snprintf(spec, sizeof(spec), "%s", s->host);
		else
			snprintf(spec, sizeof(spec), "[%s]:%d", s->host, port);
		if (libssh2_knownhost_addc(kh, spec, NULL, key, keylen, NULL, 0,
		                           LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | bit,
		                           NULL) != 0 ||
		    libssh2_knownhost_writefile(kh, sr->env.known_hosts, LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0)
			plat_log(LOG_ERR, "cannot save the host key in %s\n", sr->env.known_hosts);
	}
	libssh2_knownhost_free(kh);
	return 0;
}

static void kbd_callback(const char *name, int name_len, const char *instruction,
                         int instruction_len, int num_prompts,
                         const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                         LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
{
	sftp_remote *sr = *abstract;
	int i;

	(void)name;
	(void)name_len;
	(void)instruction;
	(void)instruction_len;
	(void)prompts;
	/* every prompt gets the password (usually there is one: "Password:") */
	for (i = 0; i < num_prompts; i++) {
		size_t n = strlen(sr->password);

		responses[i].text = malloc(n + 1);
		if (responses[i].text != NULL) {
			memcpy(responses[i].text, sr->password, n + 1);
			responses[i].length = (unsigned int)n;
		}
	}
}

static int has_method(const char *list, const char *m)
{
	size_t n = strlen(m);
	const char *p = list;

	while (p != NULL && *p != '\0') {
		if (strncmp(p, m, n) == 0 && (p[n] == ',' || p[n] == '\0'))
			return 1;
		p = strchr(p, ',');
		if (p != NULL)
			p++;
	}
	return 0;
}

static int login(sftp_remote *sr, char *err, size_t errlen, int *fail)
{
	const site_cfg *s = &sr->site;
	const char *methods;
	char *msg;
	int rc;

	methods = libssh2_userauth_list(sr->ss, s->user, (unsigned)strlen(s->user));
	if (methods == NULL) {
		if (libssh2_userauth_authenticated(sr->ss))
			return 0;
		libssh2_session_last_error(sr->ss, &msg, NULL, 0);
		snprintf(err, errlen, "The login failed: %s", msg != NULL ? msg : "");
		return -EIO;
	}
	plat_log(LOG_INFO, "sftp %s: login methods %s\n", s->name, methods);
	if (s->keyfile[0] != '\0' && has_method(methods, "publickey")) {
		rc = libssh2_userauth_publickey_fromfile_ex(sr->ss, s->user, (unsigned)strlen(s->user),
		                                            NULL, s->keyfile, sr->password);
		if (rc == 0)
			return 0;
		libssh2_session_last_error(sr->ss, &msg, NULL, 0);
		plat_log(LOG_INFO, "sftp %s: key login failed: %d %s\n", s->name, rc, msg);
		if (rc == LIBSSH2_ERROR_FILE || rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED) {
			*fail = sr->password[0] == '\0' ? RFAIL_LOGIN : RFAIL_KEYFILE;
			snprintf(err, errlen, "The key file %s cannot be used%s (%s).", s->keyfile,
			         sr->password[0] == '\0' ? " without its passphrase" : "",
			         msg != NULL ? msg : "");
			return -EACCES;
		}
		/* key refused: try a password below */
	}
	if (sr->password[0] == '\0') {
		*fail = RFAIL_LOGIN;
		snprintf(err, errlen, "A password is needed.");
		return -EACCES;
	}
	if (has_method(methods, "password")) {
		rc = libssh2_userauth_password(sr->ss, s->user, sr->password);
		if (rc == 0)
			return 0;
		if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED && rc != LIBSSH2_ERROR_PASSWORD_EXPIRED) {
			libssh2_session_last_error(sr->ss, &msg, NULL, 0);
			snprintf(err, errlen, "The login failed: %s", msg != NULL ? msg : "");
			return -EIO;
		}
		if (!has_method(methods, "keyboard-interactive")) {
			*fail = RFAIL_LOGIN;
			snprintf(err, errlen, rc == LIBSSH2_ERROR_PASSWORD_EXPIRED ?
			         "The password has expired." :
			         "The server did not accept the user name or password.");
			return -EACCES;
		}
	}
	if (has_method(methods, "keyboard-interactive")) {
		rc = libssh2_userauth_keyboard_interactive(sr->ss, s->user, kbd_callback);
		if (rc == 0)
			return 0;
		*fail = RFAIL_LOGIN;
		snprintf(err, errlen, "The server did not accept the user name or password.");
		return -EACCES;
	}
	*fail = RFAIL_LOGIN;
	snprintf(err, errlen, "The server allows none of the login methods this handler has (%s).",
	         methods);
	return -EACCES;
}

int sftp_connect(const site_cfg *site, const char *password, const remote_env *env,
                 remote **out, char *err, size_t errlen, int *fail)
{
	sftp_remote *sr = calloc(1, sizeof(*sr));
	char path[sizeof(sr->base.home)];
	char *msg;
	int rc;

	*out = NULL;
	*fail = RFAIL_NETWORK;
	if (sr == NULL) {
		snprintf(err, errlen, "Out of memory.");
		return -ENOMEM;
	}
	sr->base.ops = &sftp_ops;
	sr->site = *site;
	if (password != NULL)
		snprintf(sr->password, sizeof(sr->password), "%s", password);
	sr->env = *env;
	sr->statvfs = -1;
	sr->sock = net_connect(site->host, site_port(site), env->connect_timeout_ms, NULL, err, errlen);
	if (sr->sock < 0) {
		rc = sr->sock;
		sr->sock = -1;
		goto fail;
	}
	sr->ss = libssh2_session_init_ex(NULL, NULL, NULL, sr);
	if (sr->ss == NULL) {
		snprintf(err, errlen, "Out of memory.");
		rc = -ENOMEM;
		goto fail;
	}
	libssh2_session_set_blocking(sr->ss, 1);
	libssh2_session_set_timeout(sr->ss, env->io_timeout_ms);
	rc = libssh2_session_handshake(sr->ss, sr->sock);
	if (rc != 0) {
		libssh2_session_last_error(sr->ss, &msg, NULL, 0);
		if (rc == LIBSSH2_ERROR_TIMEOUT || rc == LIBSSH2_ERROR_SOCKET_RECV ||
		    rc == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
			snprintf(err, errlen, "%s did not answer as an SSH server (%s).", site->host,
			         msg != NULL ? msg : "");
		} else {
			*fail = RFAIL_PROTOCOL;
			snprintf(err, errlen, "The SSH connection to %s failed: %s", site->host,
			         msg != NULL ? msg : "");
		}
		rc = -EPROTO;
		goto fail;
	}
	rc = check_hostkey(sr, err, errlen);
	if (rc != 0) {
		*fail = RFAIL_HOSTKEY;
		goto fail;
	}
	*fail = RFAIL_PROTOCOL;
	rc = login(sr, err, errlen, fail);
	if (rc != 0)
		goto fail;
	/* only after the key exchange: set before it, the exchange fails */
	libssh2_keepalive_config(sr->ss, 1, 60);
	sr->sftp = libssh2_sftp_init(sr->ss);
	if (sr->sftp == NULL) {
		libssh2_session_last_error(sr->ss, &msg, NULL, 0);
		snprintf(err, errlen, "The server has no SFTP service: %s", msg != NULL ? msg : "");
		*fail = RFAIL_PROTOCOL;
		rc = -EPROTO;
		goto fail;
	}
	/* the site folder */
	rc = libssh2_sftp_realpath(sr->sftp, site->folder[0] != '\0' ? site->folder : ".",
	                           path, sizeof(path) - 1);
	if (rc <= 0) {
		if (site->folder[0] != '\0') {
			snprintf(err, errlen, "The folder %s was not found on the server.", site->folder);
			*fail = RFAIL_FOLDER;
			rc = -ENOENT;
			goto fail;
		}
		strcpy(path, "/");
	} else {
		path[rc] = '\0';
	}
	if (site->folder[0] != '\0') {
		LIBSSH2_SFTP_ATTRIBUTES a;

		if (libssh2_sftp_stat(sr->sftp, path, &a) != 0 ||
		    !((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions))) {
			snprintf(err, errlen, "The folder %s was not found on the server.", site->folder);
			*fail = RFAIL_FOLDER;
			rc = -ENOENT;
			goto fail;
		}
	}
	{
		size_t n = strlen(path);

		while (n > 0 && path[n - 1] == '/')
			path[--n] = '\0';
		snprintf(sr->base.home, sizeof(sr->base.home), "%s", path);
	}
	plat_log(LOG_INFO, "sftp %s: home \"%s\"\n", site->name, sr->base.home);
	*fail = RFAIL_NONE;
	*out = &sr->base;
	return 0;

fail:
	sr->dead = 1;
	sftp_close(&sr->base);
	return rc;
}
