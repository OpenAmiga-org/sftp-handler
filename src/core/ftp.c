/* sftp-handler - FTP client (RFC 959, 2389, 2428, 3659), passive mode
 * only. A site uses a few control connections: one for commands and one
 * for each download in progress. Public domain. */
#include "remote.h"
#include "ftplist.h"
#include "net.h"
#include "platform.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_CONN      4
#define NOOP_AFTER_MS 20000   /* check an idle connection before using it */
#define EXTRA_IDLE_MS 60000   /* close extra connections idle this long */
#define LINK_PROBES   64      /* symbolic links checked per listing */

typedef struct ftp_remote ftp_remote;

typedef struct {
	int      sock;          /* -1 = not connected */
	uint8_t  addr[4];
	char     rbuf[4096];
	size_t   rlen;
	strbuf   reply;         /* text of the last reply (all lines) */
	int      code;
	char    *cwd;           /* current folder on the server, NULL = unknown */
	rhandle *stream;        /* download using this connection */
	int64_t  used;          /* plat_ms() of the last command */
} ftp_conn;

struct rhandle {
	char    *path;
	int      write;         /* opened to be written (no data connection) */
	ftp_conn *c;            /* while a download runs */
	int      data;          /* data socket, -1 = none */
	int64_t  pos;           /* offset of the next byte from data */
	int      eof;
};

struct ftp_remote {
	remote     base;
	site_cfg   site;
	char       password[256];
	remote_env env;
	ftp_conn   conn[MAX_CONN];
	int        max_conn;
	/* server features */
	int        mlsd, mlst, size, mdtm, mfmt, utf8, rest;
	int        epsv;        /* 1 works, 0 does not, -1 not tried */
	int        list_a;      /* "LIST -a" accepted */
	int        ready;       /* the first login is done (home is known) */
};

static int  conn_open(ftp_remote *fr, ftp_conn *c, char *err, size_t errlen, int *fail);
static void conn_close(ftp_conn *c);
static void stream_stop(ftp_remote *fr, rhandle *h);

/* --- replies ------------------------------------------------------------- */

static int read_line(ftp_remote *fr, ftp_conn *c, char *line, size_t len)
{
	for (;;) {
		char *nl = memchr(c->rbuf, '\n', c->rlen);

		if (nl != NULL) {
			size_t n = (size_t)(nl - c->rbuf) + 1, copy = n;

			if (copy >= len)
				copy = len - 1;
			memcpy(line, c->rbuf, copy);
			line[copy] = '\0';
			while (copy > 0 && (line[copy - 1] == '\n' || line[copy - 1] == '\r'))
				line[--copy] = '\0';
			memmove(c->rbuf, c->rbuf + n, c->rlen - n);
			c->rlen -= n;
			return 0;
		}
		if (c->rlen == sizeof(c->rbuf)) {
			/* an overlong line: drop the start */
			c->rlen = 0;
		}
		{
			int n = net_recv(c->sock, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen,
			                 fr->env.io_timeout_ms);
			if (n == 0)
				return -ENOTCONN;
			if (n < 0)
				return n == -ETIMEDOUT ? n : -ENOTCONN;
			c->rlen += (size_t)n;
		}
	}
}

/* Reads one reply. Returns its code, or -errno; the connection is closed
 * on network errors. */
static int get_reply(ftp_remote *fr, ftp_conn *c)
{
	char line[1024];
	int code = 0, err;

	sb_clear(&c->reply);
	for (;;) {
		err = read_line(fr, c, line, sizeof(line));
		if (err != 0) {
			plat_log(LOG_INFO, "ftp %s: connection lost (%d)\n", fr->site.name, err);
			conn_close(c);
			return err == -ETIMEDOUT ? err : -ENOTCONN;
		}
		plat_log(LOG_DEBUG, "ftp < %s\n", line);
		sb_puts(&c->reply, line);
		sb_puts(&c->reply, "\n");
		if (code == 0) {
			if (strlen(line) < 3 || line[0] < '1' || line[0] > '5')
				continue; /* junk before the reply */
			code = atoi(line);
			if (line[3] != '-')
				break;
		} else if (atoi(line) == code && strlen(line) >= 4 && line[3] == ' ') {
			break;
		}
	}
	c->code = code;
	c->used = plat_ms();
	return code;
}

static int send_cmd(ftp_remote *fr, ftp_conn *c, const char *fmt, va_list ap)
{
	char cmd[1200];
	int n = vsnprintf(cmd, sizeof(cmd) - 2, fmt, ap), err;

	if (n < 0 || (size_t)n >= sizeof(cmd) - 2)
		return -ENAMETOOLONG;
	if (strncmp(cmd, "PASS ", 5) == 0)
		plat_log(LOG_DEBUG, "ftp > PASS ****\n");
	else
		plat_log(LOG_DEBUG, "ftp > %s\n", cmd);
	cmd[n++] = '\r';
	cmd[n++] = '\n';
	err = net_send(c->sock, cmd, (size_t)n, fr->env.io_timeout_ms);
	if (err != 0) {
		conn_close(c);
		return err == -ETIMEDOUT ? err : -ENOTCONN;
	}
	return 0;
}

/* Sends a command and reads the reply; returns the code or -errno. */
static int cmd(ftp_remote *fr, ftp_conn *c, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static int cmd(ftp_remote *fr, ftp_conn *c, const char *fmt, ...)
{
	va_list ap;
	int err;

	if (c->sock < 0)
		return -ENOTCONN;
	va_start(ap, fmt);
	err = send_cmd(fr, c, fmt, ap);
	va_end(ap);
	if (err != 0)
		return err;
	return get_reply(fr, c);
}

/* the reply text without the codes, for messages */
static void reply_text(ftp_conn *c, char *out, size_t len)
{
	const char *p = c->reply.data != NULL ? c->reply.data : "";
	size_t o = 0;

	while (*p != '\0' && o + 1 < len) {
		const char *nl = strchr(p, '\n');
		size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);

		if (n > 4 && p[0] >= '1' && p[0] <= '5')
			p += 4, n -= 4;
		if (o > 0 && o + 1 < len)
			out[o++] = ' ';
		while (n > 0 && o + 1 < len) {
			out[o++] = *p++;
			n--;
		}
		p = nl != NULL ? nl + 1 : p + n;
	}
	out[o] = '\0';
}

static void note_error(ftp_remote *fr, ftp_conn *c)
{
	reply_text(c, fr->base.error, sizeof(fr->base.error));
}

/* -errno for a reply code */
static int code_err(int code)
{
	if (code < 0)
		return code;
	switch (code) {
	case 421: return -ENOTCONN;
	case 450: return -EBUSY;
	case 452: case 552: return -ENOSPC;
	case 530: case 532: return -EACCES;
	case 550: return -ENOENT;
	case 553: return -EACCES;
	case 500: case 501: case 502: case 504: return -ENOSYS;
	}
	return code >= 400 ? -EIO : 0;
}

/* --- connections ------------------------------------------------------------ */

static void conn_close(ftp_conn *c)
{
	if (c->sock >= 0)
		net_close(c->sock);
	c->sock = -1;
	c->rlen = 0;
	free(c->cwd);
	c->cwd = NULL;
	c->stream = NULL;
}

static void parse_feat(ftp_remote *fr, const char *text)
{
	const char *p = text;

	while (*p != '\0') {
		const char *nl = strchr(p, '\n');
		char f[64];
		size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p), i = 0;

		while (n > 0 && (*p == ' ' || (p[0] >= '0' && p[0] <= '9' && n > 3 && p[3] == '-' && i == 0))) {
			if (*p == ' ') {
				p++;
				n--;
			} else {
				break; /* "211-Features:" line */
			}
		}
		for (i = 0; i < n && i + 1 < sizeof(f) && p[i] != ' ' && p[i] != '\r'; i++)
			f[i] = p[i];
		f[i] = '\0';
		if (strcasecmp(f, "MLST") == 0 || strcasecmp(f, "MLSD") == 0)
			fr->mlsd = fr->mlst = 1;
		else if (strcasecmp(f, "SIZE") == 0)
			fr->size = 1;
		else if (strcasecmp(f, "MDTM") == 0)
			fr->mdtm = 1;
		else if (strcasecmp(f, "MFMT") == 0)
			fr->mfmt = 1;
		else if (strcasecmp(f, "UTF8") == 0)
			fr->utf8 = 1;
		else if (strcasecmp(f, "REST") == 0)
			fr->rest = 1;
		p = nl != NULL ? nl + 1 : p + n;
	}
}

/* "257 "/home/me" is the current directory" -> /home/me ("" doubles '"') */
static int parse_pwd(const char *reply, char *out, size_t len)
{
	const char *p = strchr(reply, '"');
	size_t o = 0;

	if (p == NULL)
		return -1;
	for (p++; *p != '\0'; p++) {
		if (*p == '"') {
			if (p[1] != '"')
				break;
			p++;
		}
		if (o + 1 < len)
			out[o++] = *p;
	}
	out[o] = '\0';
	return o > 0 ? 0 : -1;
}

static int conn_open(ftp_remote *fr, ftp_conn *c, char *err, size_t errlen, int *fail)
{
	const site_cfg *s = &fr->site;
	const char *user = s->user[0] != '\0' ? s->user : "anonymous";
	const char *pass = s->user[0] != '\0' ? fr->password : "amiga@example.com";
	int first = !fr->ready;
	int code;
	char msg[400];

	conn_close(c);
	*fail = RFAIL_NETWORK;
	c->sock = net_connect(s->host, site_port(s), fr->env.connect_timeout_ms, c->addr, err, errlen);
	if (c->sock < 0) {
		int e = c->sock;
		c->sock = -1;
		return e;
	}
	do {
		code = get_reply(fr, c);
	} while (code == 120);
	if (code != 220) {
		if (code > 0) {
			reply_text(c, msg, sizeof(msg));
			snprintf(err, errlen, "The server refused the connection: %s", msg);
			*fail = code == 421 ? RFAIL_NETWORK : RFAIL_PROTOCOL;
		} else {
			snprintf(err, errlen, "%s did not answer as an FTP server.", s->host);
			*fail = RFAIL_PROTOCOL;
		}
		conn_close(c);
		return code == 421 ? -EAGAIN : -EPROTO;
	}
	code = cmd(fr, c, "USER %s", user);
	if (code == 331 || code == 332)
		code = cmd(fr, c, "PASS %s", pass);
	if (code != 230 && code != 202) {
		if (code > 0) {
			reply_text(c, msg, sizeof(msg));
			snprintf(err, errlen, "Login failed: %s", msg);
		} else {
			snprintf(err, errlen, "The connection was lost during the login.");
		}
		/* 421: too many connections; 530: login refused */
		*fail = code == 530 ? RFAIL_LOGIN : RFAIL_NETWORK;
		conn_close(c);
		return code == 530 ? -EACCES : -EAGAIN;
	}
	if (first) {
		code = cmd(fr, c, "FEAT");
		if (code == 211)
			parse_feat(fr, c->reply.data);
		if (fr->utf8)
			cmd(fr, c, "OPTS UTF8 ON");
	}
	code = cmd(fr, c, "TYPE I");
	if (code != 200) {
		snprintf(err, errlen, "The server does not allow binary transfers.");
		*fail = RFAIL_PROTOCOL;
		conn_close(c);
		return -EPROTO;
	}
	if (first) {
		char path[512];

		if (s->folder[0] != '\0') {
			code = cmd(fr, c, "CWD %s", s->folder);
			if (code != 250) {
				snprintf(err, errlen, "The folder %s was not found on the server.", s->folder);
				*fail = RFAIL_FOLDER;
				conn_close(c);
				return -ENOENT;
			}
		}
		code = cmd(fr, c, "PWD");
		if (code == 257 && parse_pwd(c->reply.data, path, sizeof(path)) == 0) {
			size_t n = strlen(path);

			while (n > 0 && path[n - 1] == '/')
				path[--n] = '\0';
			snprintf(fr->base.home, sizeof(fr->base.home), "%s", path);
			c->cwd = xstrdup(path[0] != '\0' ? path : "/");
		} else if (s->folder[0] == '/') {
			snprintf(fr->base.home, sizeof(fr->base.home), "%s", s->folder);
		}
		if (fr->base.home[0] == '\0')
			snprintf(fr->base.home, sizeof(fr->base.home), "%s", "/");
		/* "" stands for the server root */
		if (strcmp(fr->base.home, "/") == 0)
			fr->base.home[0] = '\0';
		fr->ready = 1;
		plat_log(LOG_INFO, "ftp %s: home \"%s\" mlsd %d size %d mdtm %d mfmt %d utf8 %d\n",
		         s->name, fr->base.home, fr->mlsd, fr->size, fr->mdtm, fr->mfmt, fr->utf8);
	}
	c->used = plat_ms();
	*fail = RFAIL_NONE;
	return 0;
}

static int conn_alive(ftp_remote *fr, ftp_conn *c)
{
	if (c->sock < 0)
		return 0;
	if (plat_ms() - c->used < NOOP_AFTER_MS)
		return 1;
	return cmd(fr, c, "NOOP") == 200;
}

/* A connection for a command (stream = NULL) or for a download. */
static int get_conn(ftp_remote *fr, rhandle *stream, ftp_conn **out)
{
	char err[300];
	int i, fail, rc;

	*out = NULL;
	/* an idle connection */
	for (i = 0; i < fr->max_conn; i++) {
		ftp_conn *c = &fr->conn[i];

		if (c->sock >= 0 && c->stream == NULL) {
			if (conn_alive(fr, c)) {
				c->stream = stream;
				*out = c;
				return 0;
			}
		}
	}
	/* a new one */
	for (i = 0; i < fr->max_conn; i++) {
		ftp_conn *c = &fr->conn[i];

		if (c->sock < 0 && c->stream == NULL) {
			rc = conn_open(fr, c, err, sizeof(err), &fail);
			if (rc == 0) {
				c->stream = stream;
				*out = c;
				return 0;
			}
			snprintf(fr->base.error, sizeof(fr->base.error), "%s", err);
			if (rc != -EAGAIN)
				return rc == -EACCES ? -EACCES : -ENOTCONN;
			/* too many connections: use fewer from now on */
			if (i > 0) {
				fr->max_conn = i;
				plat_log(LOG_INFO, "ftp %s: at most %d connections\n", fr->site.name, i);
			} else {
				return -ENOTCONN;
			}
			break;
		}
	}
	/* take one from a download; that download starts again later */
	for (i = 0; i < fr->max_conn; i++) {
		ftp_conn *c = &fr->conn[i];

		if (c->stream != NULL && c->stream != stream) {
			stream_stop(fr, c->stream);
			if (c->sock < 0 && conn_open(fr, c, err, sizeof(err), &fail) != 0)
				continue;
			c->stream = stream;
			*out = c;
			return 0;
		}
	}
	return -ENOTCONN;
}

static void put_conn(ftp_conn *c)
{
	c->stream = NULL;
}

/* --- paths and data connections ------------------------------------------- */

static int open_data(ftp_remote *fr, ftp_conn *c)
{
	int code, port = -1;
	char *p;

	if (fr->epsv != 0) {
		code = cmd(fr, c, "EPSV");
		if (code < 0)
			return code;
		if (code == 229 && (p = strstr(c->reply.data, "(|||")) != NULL) {
			port = atoi(p + 4);
			fr->epsv = 1;
		} else if (fr->epsv == -1) {
			fr->epsv = 0;
		}
	}
	if (port < 0) {
		int h[4], p1, p2;

		code = cmd(fr, c, "PASV");
		if (code < 0)
			return code;
		p = strchr(c->reply.data, '(');
		if (code != 227 || p == NULL ||
		    sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &p1, &p2) != 6) {
			/* some servers give the numbers without brackets */
			p = c->reply.data + 4;
			while (*p != '\0' && (*p < '0' || *p > '9'))
				p++;
			if (code != 227 || sscanf(p, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &p1, &p2) != 6) {
				note_error(fr, c);
				return -EPROTO;
			}
		}
		port = p1 * 256 + p2;
	}
	/* the address in a PASV reply is often wrong behind NAT: always use
	 * the address of the control connection */
	return net_connect_addr(c->addr, port, fr->env.connect_timeout_ms);
}

static char *server_path(ftp_remote *fr, const char *path)
{
	strbuf sb;

	sb_init(&sb);
	sb_puts(&sb, fr->base.home);
	if (strcmp(path, "/") != 0 || fr->base.home[0] == '\0')
		sb_puts(&sb, path);
	return sb.failed ? (sb_free(&sb), (char *)NULL) : sb_steal(&sb);
}

static int change_dir(ftp_remote *fr, ftp_conn *c, const char *dir)
{
	int code;

	if (c->cwd != NULL && strcmp(c->cwd, dir) == 0)
		return 0;
	code = cmd(fr, c, "CWD %s", dir);
	free(c->cwd);
	c->cwd = NULL;
	if (code == 250) {
		c->cwd = xstrdup(dir);
		return 0;
	}
	if (code > 0)
		note_error(fr, c);
	return code == 550 ? -ENOENT : code_err(code);
}

/* --- operations ------------------------------------------------------------- */

static void ftp_close(remote *r)
{
	ftp_remote *fr = (ftp_remote *)r;
	int i;

	for (i = 0; i < MAX_CONN; i++) {
		ftp_conn *c = &fr->conn[i];

		if (c->sock >= 0 && c->stream == NULL)
			cmd(fr, c, "QUIT");
		conn_close(c);
		sb_free(&c->reply);
	}
	memset(fr->password, 0, sizeof(fr->password));
	free(fr);
}

struct linebuf {
	char   buf[8192];
	size_t len;
};

typedef int (*line_fn)(void *ud, const char *line);

/* reads a listing from the data connection line by line */
static int read_lines(ftp_remote *fr, int data, line_fn fn, void *ud)
{
	struct linebuf *lb = malloc(sizeof(*lb));
	int rc = 0;

	if (lb == NULL)
		return -ENOMEM;
	lb->len = 0;
	for (;;) {
		char *nl;
		int n = net_recv(data, lb->buf + lb->len, sizeof(lb->buf) - 1 - lb->len,
		                 fr->env.io_timeout_ms);

		if (n < 0) {
			rc = -EIO;
			break;
		}
		lb->len += (size_t)n;
		lb->buf[lb->len] = '\0';
		while ((nl = memchr(lb->buf, '\n', lb->len)) != NULL || (n == 0 && lb->len > 0)) {
			size_t take = nl != NULL ? (size_t)(nl - lb->buf) + 1 : lb->len;
			size_t l = take;

			if (nl != NULL)
				*nl = '\0';
			else
				lb->buf[lb->len] = '\0';
			while (l > 0 && (lb->buf[l - 1] == '\r' || lb->buf[l - 1] == '\n' || lb->buf[l - 1] == '\0'))
				lb->buf[--l] = '\0';
			if (rc == 0 && fn(ud, lb->buf) != 0)
				rc = -ECANCELED;
			memmove(lb->buf, lb->buf + take, lb->len - take);
			lb->len -= take;
		}
		if (n == 0)
			break;
		if (lb->len == sizeof(lb->buf) - 1)
			lb->len = 0; /* overlong line */
	}
	free(lb);
	return rc;
}

struct list_ctx {
	ftp_remote *fr;
	int         mlsd;
	time_t      now;
	rlist_fn    fn;
	void       *ud;
	/* symbolic links of unknown kind, checked afterwards */
	char      **links;
	int         nlinks;
	int         stop;
};

static int list_line(void *ud, const char *line)
{
	struct list_ctx *lc = ud;
	char name[1024];
	rstat st;
	int kind;

	if (lc->stop)
		return 0;
	plat_log(LOG_DEBUG, "ftp list: %s\n", line);
	kind = lc->mlsd ? ftp_parse_mlsd(line, name, sizeof(name), &st)
	                : ftp_parse_list(line, lc->now, name, sizeof(name), &st);
	if (kind == FL_SKIP)
		return 0;
	if (kind == FL_LINK) {
		if (lc->nlinks < LINK_PROBES) {
			char **n = realloc(lc->links, (size_t)(lc->nlinks + 1) * sizeof(*n));

			if (n != NULL) {
				lc->links = n;
				n[lc->nlinks] = xstrdup(name);
				if (n[lc->nlinks] != NULL) {
					lc->nlinks++;
					return 0;
				}
			}
		}
		st.is_dir = 0;
	}
	if (lc->fn(lc->ud, name, &st) != 0)
		lc->stop = 1;
	return 0;
}

static int run_list(ftp_remote *fr, ftp_conn *c, struct list_ctx *lc)
{
	int data, code, rc, tries;

	for (tries = 0; tries < 2; tries++) {
		const char *command;

		data = open_data(fr, c);
		if (data < 0)
			return data == -EPROTO ? -EIO : data;
		if (fr->mlsd) {
			command = "MLSD";
			lc->mlsd = 1;
		} else {
			command = fr->list_a ? "LIST -a" : "LIST";
			lc->mlsd = 0;
		}
		code = cmd(fr, c, "%s", command);
		if (code == 150 || code == 125) {
			rc = read_lines(fr, data, list_line, lc);
			net_close(data);
			code = get_reply(fr, c);
			if (rc != 0)
				return rc;
			if (code == 226 || code == 250)
				return 0;
			note_error(fr, c);
			return code == 450 || code == 550 ? -ENOENT : code_err(code);
		}
		net_close(data);
		if (code < 0)
			return code;
		/* an empty folder gives 450/550 on some servers */
		if ((code == 450 || code == 550) && !fr->mlsd)
			return 0;
		if (code >= 500 && fr->mlsd) {
			fr->mlsd = 0;
			continue;
		}
		if (code >= 500 && fr->list_a) {
			fr->list_a = 0;
			continue;
		}
		note_error(fr, c);
		return code_err(code);
	}
	return -EIO;
}

static int ftp_list(remote *r, const char *path, rlist_fn fn, void *ud)
{
	ftp_remote *fr = (ftp_remote *)r;
	struct list_ctx lc;
	ftp_conn *c;
	char *sp;
	int rc, i;

	rc = get_conn(fr, NULL, &c);
	if (rc != 0)
		return rc;
	sp = server_path(fr, path);
	if (sp == NULL) {
		put_conn(c);
		return -ENOMEM;
	}
	rc = change_dir(fr, c, sp[0] != '\0' ? sp : "/");
	memset(&lc, 0, sizeof(lc));
	lc.fr = fr;
	lc.now = plat_time();
	lc.fn = fn;
	lc.ud = ud;
	if (rc == 0)
		rc = run_list(fr, c, &lc);
	/* links: a folder when CWD into it works */
	for (i = 0; i < lc.nlinks; i++) {
		if (rc == 0 && !lc.stop && c->sock >= 0) {
			rstat st;
			int code;

			memset(&st, 0, sizeof(st));
			st.mode = -1;
			code = cmd(fr, c, "CWD %s/%s", sp, lc.links[i]);
			free(c->cwd);
			c->cwd = NULL;
			st.is_dir = code == 250;
			if (!st.is_dir && fr->size) {
				code = cmd(fr, c, "SIZE %s/%s", sp, lc.links[i]);
				if (code == 213)
					st.size = strtoll(c->reply.data + 4, NULL, 10);
			}
			if (fn(ud, lc.links[i], &st) != 0)
				lc.stop = 1;
		}
		free(lc.links[i]);
	}
	free(lc.links);
	free(sp);
	put_conn(c);
	return rc;
}

static int ftp_stat(remote *r, const char *path, rstat *st)
{
	ftp_remote *fr = (ftp_remote *)r;
	ftp_conn *c;
	char *sp, name[1024];
	int rc, code;

	if (!fr->mlst)
		return -ENOSYS;
	rc = get_conn(fr, NULL, &c);
	if (rc != 0)
		return rc;
	sp = server_path(fr, path);
	if (sp == NULL) {
		put_conn(c);
		return -ENOMEM;
	}
	code = cmd(fr, c, "MLST %s", sp[0] != '\0' ? sp : "/");
	free(sp);
	put_conn(c);
	if (code == 250) {
		/* the facts are on the second line, after a space */
		const char *p = strchr(c->reply.data, '\n');

		if (p != NULL && p[1] == ' ' && ftp_parse_mlsd(p + 2, name, sizeof(name), st) != FL_SKIP)
			return 0;
		/* cdir for a folder */
		if (p != NULL && strstr(p, "type=cdir") != NULL) {
			memset(st, 0, sizeof(*st));
			st->is_dir = 1;
			st->mode = -1;
			return 0;
		}
		return -ENOSYS;
	}
	if (code > 0)
		note_error(fr, c);
	return code == 550 ? -ENOENT : code < 0 ? code : -ENOSYS;
}

/* a command on one path: returns the code or -errno */
static int path_cmd(ftp_remote *fr, const char *verb, const char *path, int *code_out)
{
	ftp_conn *c;
	char *sp;
	int rc, code;

	rc = get_conn(fr, NULL, &c);
	if (rc != 0)
		return rc;
	sp = server_path(fr, path);
	if (sp == NULL) {
		put_conn(c);
		return -ENOMEM;
	}
	code = cmd(fr, c, "%s %s", verb, sp);
	free(sp);
	if (code >= 400)
		note_error(fr, c);
	/* a folder the connection is in may be gone */
	free(c->cwd);
	c->cwd = NULL;
	put_conn(c);
	*code_out = code;
	return code < 0 ? code : 0;
}

static int ftp_mkdir(remote *r, const char *path)
{
	int code, rc = path_cmd((ftp_remote *)r, "MKD", path, &code);

	if (rc != 0)
		return rc;
	return code == 257 || code == 250 ? 0 : code == 550 ? -EACCES : code_err(code);
}

static int ftp_rmdir(remote *r, const char *path)
{
	int code, rc = path_cmd((ftp_remote *)r, "RMD", path, &code);

	if (rc != 0)
		return rc;
	return code == 250 || code == 200 ? 0 : code == 550 ? -EACCES : code_err(code);
}

static int ftp_unlink(remote *r, const char *path)
{
	int code, rc = path_cmd((ftp_remote *)r, "DELE", path, &code);

	if (rc != 0)
		return rc;
	return code == 250 || code == 200 ? 0 : code == 550 ? -EACCES : code_err(code);
}

static int ftp_rename(remote *r, const char *from, const char *to)
{
	ftp_remote *fr = (ftp_remote *)r;
	ftp_conn *c;
	char *sf, *st;
	int rc, code;

	rc = get_conn(fr, NULL, &c);
	if (rc != 0)
		return rc;
	sf = server_path(fr, from);
	st = server_path(fr, to);
	if (sf == NULL || st == NULL) {
		free(sf);
		free(st);
		put_conn(c);
		return -ENOMEM;
	}
	code = cmd(fr, c, "RNFR %s", sf);
	if (code == 350)
		code = cmd(fr, c, "RNTO %s", st);
	if (code >= 400)
		note_error(fr, c);
	free(c->cwd);
	c->cwd = NULL;
	free(sf);
	free(st);
	put_conn(c);
	if (code < 0)
		return code;
	return code == 250 || code == 200 ? 0 : code == 550 ? -EACCES : code_err(code);
}

static int ftp_set_mtime(remote *r, const char *path, time_t t)
{
	ftp_remote *fr = (ftp_remote *)r;
	char verb[40];
	int64_t days = (int64_t)t / 86400, secs = (int64_t)t % 86400;
	int64_t z, era;
	unsigned doe, yoe, doy, mp, d, m;
	int y, code, rc;

	if (!fr->mfmt || t <= 0)
		return -ENOSYS;
	if (secs < 0) {
		secs += 86400;
		days--;
	}
	/* civil_from_days */
	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp + (mp < 10 ? 3 : -9);
	y = (int)(yoe + era * 400) + (m <= 2);
	snprintf(verb, sizeof(verb), "MFMT %04d%02u%02u%02d%02d%02d", y, m, d,
	         (int)(secs / 3600), (int)(secs / 60 % 60), (int)(secs % 60));
	rc = path_cmd(fr, verb, path, &code);
	if (rc != 0)
		return rc;
	if (code == 213 || code == 253 || code == 250)
		return 0;
	if (code >= 500 && code < 550)
		fr->mfmt = 0;
	return -ENOSYS;
}

static int ftp_chmod(remote *r, const char *path, int mode)
{
	char verb[40];
	int code, rc;

	snprintf(verb, sizeof(verb), "SITE CHMOD %03o", mode & 0777);
	rc = path_cmd((ftp_remote *)r, verb, path, &code);
	if (rc != 0)
		return rc;
	return code == 200 || code == 250 ? 0 : -ENOSYS;
}

static int ftp_truncate(remote *r, const char *path, int64_t size)
{
	(void)r;
	(void)path;
	(void)size;
	return -ENOSYS;
}

static int ftp_statfs(remote *r, const char *path, int64_t *total, int64_t *avail)
{
	(void)r;
	(void)path;
	(void)total;
	(void)avail;
	return -ENOSYS;
}

/* --- downloads ----------------------------------------------------------- */

static int ftp_open(remote *r, const char *path, int mode, rhandle **out)
{
	ftp_remote *fr = (ftp_remote *)r;
	rhandle *h = calloc(1, sizeof(*h));

	if (h == NULL)
		return -ENOMEM;
	h->path = server_path(fr, path);
	if (h->path == NULL) {
		free(h);
		return -ENOMEM;
	}
	h->write = mode != RO_READ;
	h->data = -1;
	h->pos = -1;
	*out = h;
	return 0;
}

/* ends a download: closes the data connection and reads the reply to the
 * RETR (426 when it was cut short). A server that does not answer loses
 * its control connection. */
static void stream_stop(ftp_remote *fr, rhandle *h)
{
	ftp_conn *c = h->c;

	if (h->data >= 0)
		net_close(h->data);
	h->data = -1;
	h->pos = -1;
	h->eof = 0;
	if (c == NULL)
		return;
	h->c = NULL;
	if (c->sock >= 0) {
		int saved = fr->env.io_timeout_ms, code;

		fr->env.io_timeout_ms = 5000;
		code = get_reply(fr, c);
		fr->env.io_timeout_ms = saved;
		if (code < 0 || code < 200)
			conn_close(c);
	}
	put_conn(c);
}

static int stream_start(ftp_remote *fr, rhandle *h, int64_t off)
{
	ftp_conn *c;
	int rc, code;

	stream_stop(fr, h);
	rc = get_conn(fr, h, &c);
	if (rc != 0)
		return rc;
	if (off > 0) {
		code = cmd(fr, c, "REST %lld", (long long)off);
		if (code != 350) {
			put_conn(c);
			if (code > 0)
				note_error(fr, c);
			/* without REST only reading from the start works */
			return code < 0 ? code : -ESPIPE;
		}
	}
	h->data = open_data(fr, c);
	if (h->data < 0) {
		rc = h->data;
		h->data = -1;
		put_conn(c);
		return rc == -EPROTO ? -EIO : rc;
	}
	code = cmd(fr, c, "RETR %s", h->path);
	if (code != 150 && code != 125) {
		net_close(h->data);
		h->data = -1;
		if (code > 0)
			note_error(fr, c);
		put_conn(c);
		if (code == 550)
			return -ENOENT;
		return code_err(code);
	}
	h->c = c;
	h->pos = off;
	h->eof = 0;
	return 0;
}

static int ftp_pread(remote *r, rhandle *h, char *buf, size_t len, int64_t off)
{
	ftp_remote *fr = (ftp_remote *)r;
	size_t got = 0;
	int rc, tries = 0;

	if (h->eof && off == h->pos)
		return 0;
	while (got < len) {
		int n;

		if (h->data < 0 || h->pos != off + (int64_t)got) {
			if (h->eof && h->pos == off + (int64_t)got)
				break;
			rc = stream_start(fr, h, off + (int64_t)got);
			if (rc != 0)
				return got > 0 ? (int)got : rc;
		}
		n = net_recv(h->data, buf + got, len - got, fr->env.io_timeout_ms);
		if (n > 0) {
			got += (size_t)n;
			h->pos += n;
			continue;
		}
		if (n == 0) {
			/* the end of the file: the server confirms with 226 */
			ftp_conn *c = h->c;
			int code;

			net_close(h->data);
			h->data = -1;
			h->c = NULL;
			code = get_reply(fr, c);
			put_conn(c);
			if (code == 226 || code == 250) {
				h->eof = 1;
				break;
			}
			if (code > 0)
				note_error(fr, c);
			/* cut off: try again from here once */
			if (++tries < 2)
				continue;
			return got > 0 ? (int)got : -EIO;
		}
		/* network error on the data connection */
		stream_stop(fr, h);
		if (++tries < 2)
			continue;
		return got > 0 ? (int)got : -EIO;
	}
	return (int)got;
}

static int ftp_pwrite(remote *r, rhandle *h, const char *buf, size_t len, int64_t off)
{
	(void)r;
	(void)h;
	(void)buf;
	(void)len;
	(void)off;
	return -ENOSYS;
}

static int ftp_ftruncate(remote *r, rhandle *h, int64_t size)
{
	(void)r;
	(void)h;
	(void)size;
	return -ENOSYS;
}

static int ftp_fclose(remote *r, rhandle *h)
{
	ftp_remote *fr = (ftp_remote *)r;

	stream_stop(fr, h);
	free(h->path);
	free(h);
	return 0;
}

/* --- uploads -------------------------------------------------------------- */

static int ftp_upload(remote *r, const char *path, rsource_fn src, void *ud)
{
	ftp_remote *fr = (ftp_remote *)r;
	ftp_conn *c;
	char *sp, *buf;
	int rc, code, data;

	buf = malloc(65536);
	if (buf == NULL)
		return -ENOMEM;
	rc = get_conn(fr, NULL, &c);
	if (rc != 0) {
		free(buf);
		return rc;
	}
	sp = server_path(fr, path);
	if (sp == NULL) {
		put_conn(c);
		free(buf);
		return -ENOMEM;
	}
	data = open_data(fr, c);
	if (data < 0) {
		put_conn(c);
		free(sp);
		free(buf);
		return data == -EPROTO ? -EIO : data;
	}
	code = cmd(fr, c, "STOR %s", sp);
	free(sp);
	if (code != 150 && code != 125) {
		net_close(data);
		if (code > 0)
			note_error(fr, c);
		put_conn(c);
		free(buf);
		return code == 550 ? -EACCES : code_err(code);
	}
	rc = 0;
	for (;;) {
		int n = src(ud, buf, 65536);

		if (n < 0) {
			rc = n;
			break;
		}
		if (n == 0)
			break;
		rc = net_send(data, buf, (size_t)n, fr->env.io_timeout_ms);
		if (rc != 0) {
			rc = -EIO;
			break;
		}
	}
	free(buf);
	net_shutdown_send(data);
	net_close(data);
	code = get_reply(fr, c);
	if (code >= 400)
		note_error(fr, c);
	put_conn(c);
	if (rc != 0)
		return rc;
	if (code == 226 || code == 250)
		return 0;
	return code < 0 ? -EIO : code == 550 ? -EACCES : code_err(code);
}

static int ftp_idle(remote *r)
{
	ftp_remote *fr = (ftp_remote *)r;
	int64_t now = plat_ms();
	int i, open = 0;

	for (i = 0; i < MAX_CONN; i++) {
		ftp_conn *c = &fr->conn[i];

		if (c->sock < 0 || c->stream != NULL)
			continue;
		/* keep one; close the others when idle */
		if (open > 0 && now - c->used > EXTRA_IDLE_MS) {
			cmd(fr, c, "QUIT");
			conn_close(c);
			continue;
		}
		open++;
	}
	return 0;
}

static const remote_ops ftp_ops = {
	.close     = ftp_close,
	.list      = ftp_list,
	.stat      = ftp_stat,
	.mkdir     = ftp_mkdir,
	.rmdir     = ftp_rmdir,
	.unlink    = ftp_unlink,
	.rename    = ftp_rename,
	.set_mtime = ftp_set_mtime,
	.chmod     = ftp_chmod,
	.truncate  = ftp_truncate,
	.statfs    = ftp_statfs,
	.open      = ftp_open,
	.pread     = ftp_pread,
	.pwrite    = ftp_pwrite,
	.ftruncate = ftp_ftruncate,
	.fclose    = ftp_fclose,
	.upload    = ftp_upload,
	.idle      = ftp_idle,
	.random_write = 0,
};

int ftp_connect(const site_cfg *site, const char *password, const remote_env *env,
                remote **out, char *err, size_t errlen, int *fail)
{
	ftp_remote *fr = calloc(1, sizeof(*fr));
	int i, rc;

	*out = NULL;
	*fail = RFAIL_NETWORK;
	if (fr == NULL) {
		snprintf(err, errlen, "Out of memory.");
		return -ENOMEM;
	}
	fr->base.ops = &ftp_ops;
	fr->site = *site;
	if (password != NULL)
		snprintf(fr->password, sizeof(fr->password), "%s", password);
	fr->env = *env;
	fr->max_conn = MAX_CONN;
	fr->epsv = -1;
	fr->list_a = 1;
	for (i = 0; i < MAX_CONN; i++) {
		fr->conn[i].sock = -1;
		sb_init(&fr->conn[i].reply);
	}
	rc = conn_open(fr, &fr->conn[0], err, errlen, fail);
	if (rc != 0) {
		ftp_close(&fr->base);
		return rc;
	}
	*out = &fr->base;
	return 0;
}
