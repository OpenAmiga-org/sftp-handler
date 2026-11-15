/* sftp-handler - the root of sites and the file system of each site, with
 * a cache of server listings. Public domain. */
#include "sfs.h"
#include "config.h"
#include "platform.h"
#include "remote.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif

#define NBUCKETS     64
#define MAX_LISTINGS 128          /* per site */
#define CFG_CHECK_MS 1000         /* how often the site file is looked at */
#define RETRY_MS     5000         /* between failed connection attempts */
#define KEEPALIVE_MS 60000
#define LIST_KEEP_MS 120000       /* old listings are freed after this */
#define ICON_MAX     (256 * 1024)
#define BIG_DISK     ((int64_t)1 << 40)
/* a site that cannot be reached: AmigaDOS shows "device not mounted" */
#define ERR_OFFLINE  (-ENODEV)

typedef struct {
	char  *name;
	rstat  st;
} entry;

typedef struct listing {
	struct listing *next;
	char           *path;
	int64_t         when;
	entry          *e;
	size_t          n;
	size_t          cap;
} listing;

typedef struct site {
	struct site *next;       /* on the list of removed sites */
	site_cfg     cfg;
	remote      *r;
	int64_t      last_use;
	int64_t      last_keepalive;
	int64_t      last_fail;
	int          last_err;
	int          refused;    /* cancelled or refused: no new tries until reset */
	time_t       refused_kh; /* host key refused: date of known_hosts then */
	int          reported;   /* a connection problem was shown */
	char         password[256];   /* typed into the requester */
	int          have_password;
	listing     *lists[NBUCKETS];
	size_t       nlists;
	int          nfiles;     /* open files */
	int          gone;       /* removed from the site list */
} site;

struct sfs_file {
	sfs_file *next;
	site     *s;
	char     *path;          /* real path in the site */
	int       icon;          /* a site's icon at the root */
	rhandle  *h;
	int       hmode;         /* RO_* the handle was opened with */
	tmpf     *tf;            /* FTP: the new contents */
	int       dirty;
	int       is_new;        /* FTP: not on the server yet */
	int64_t   size;
	time_t    mtime;
	time_t    set_mtime;     /* date to set after the upload */
	int       mode;
	int       lost;          /* collected writes were lost with a connection */
};

struct sfs {
	sfs_options opt;
	site      **site;
	int         nsites;
	site       *gone;
	sfs_file   *files;
	char        sites_file[400];
	time_t      cfg_mtime;
	int64_t     cfg_size;
	int         cfg_exists;
	int64_t     cfg_checked;
	int         cfg_reported;
	char       *icon;
	size_t      iconlen;
	int         icon_loaded;
	int         keep_count;
};

static const char root_path[] = "/";

static unsigned hash(const char *s)
{
	unsigned h = 5381;

	while (*s != '\0')
		h = h * 33 + (unsigned char)*s++;
	return h % NBUCKETS;
}

/* --- listings ----------------------------------------------------------- */

static void listing_free(listing *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->e[i].name);
	free(l->e);
	free(l->path);
	free(l);
}

static void list_flush(site *s)
{
	unsigned i;

	for (i = 0; i < NBUCKETS; i++) {
		while (s->lists[i] != NULL) {
			listing *l = s->lists[i];
			s->lists[i] = l->next;
			listing_free(l);
		}
	}
	s->nlists = 0;
}

static listing *list_find(site *s, const char *path)
{
	listing *l;

	for (l = s->lists[hash(path)]; l != NULL; l = l->next)
		if (strcmp(l->path, path) == 0)
			return l;
	return NULL;
}

static void list_drop(site *s, const char *path)
{
	listing **pp = &s->lists[hash(path)];

	while (*pp != NULL) {
		if (strcmp((*pp)->path, path) == 0) {
			listing *l = *pp;
			*pp = l->next;
			listing_free(l);
			s->nlists--;
			return;
		}
		pp = &(*pp)->next;
	}
}

static void list_drop_under(site *s, const char *path)
{
	unsigned i;

	for (i = 0; i < NBUCKETS; i++) {
		listing **pp = &s->lists[i];

		while (*pp != NULL) {
			if (path_is_under((*pp)->path, path)) {
				listing *l = *pp;
				*pp = l->next;
				listing_free(l);
				s->nlists--;
			} else {
				pp = &(*pp)->next;
			}
		}
	}
}

/* drops the oldest listings when there are too many, and very old ones */
static void list_trim(site *s, int64_t keep_ms)
{
	int64_t now = plat_ms();
	unsigned i;

	for (i = 0; i < NBUCKETS; i++) {
		listing **pp = &s->lists[i];

		while (*pp != NULL) {
			if (now - (*pp)->when > keep_ms) {
				listing *l = *pp;
				*pp = l->next;
				listing_free(l);
				s->nlists--;
			} else {
				pp = &(*pp)->next;
			}
		}
	}
}

static entry *list_entry(listing *l, const char *name, int nocase)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		if (strcmp(l->e[i].name, name) == 0)
			return &l->e[i];
	if (nocase)
		for (i = 0; i < l->n; i++)
			if (name_casecmp(l->e[i].name, name) == 0)
				return &l->e[i];
	return NULL;
}

static int list_add(listing *l, const char *name, const rstat *st)
{
	if (l->n == l->cap) {
		size_t ncap = l->cap ? l->cap * 2 : 32;
		entry *n = realloc(l->e, ncap * sizeof(*n));

		if (n == NULL)
			return -ENOMEM;
		l->e = n;
		l->cap = ncap;
	}
	l->e[l->n].name = xstrdup(name);
	if (l->e[l->n].name == NULL)
		return -ENOMEM;
	l->e[l->n].st = *st;
	l->n++;
	return 0;
}

/* keeps a cached listing in step with a change we made */
static void list_set(site *s, const char *path, const rstat *st)
{
	char *dir = path_parent(path);
	listing *l = dir != NULL ? list_find(s, dir) : NULL;

	if (l != NULL) {
		entry *e = list_entry(l, path_basename(path), 0);

		if (e != NULL)
			e->st = *st;
		else if (list_add(l, path_basename(path), st) != 0)
			list_drop(s, dir);
	}
	free(dir);
}

static void list_remove(site *s, const char *path)
{
	char *dir = path_parent(path);
	listing *l = dir != NULL ? list_find(s, dir) : NULL;

	if (l != NULL) {
		entry *e = list_entry(l, path_basename(path), 0);

		if (e != NULL) {
			free(e->name);
			*e = l->e[--l->n];
		}
	}
	free(dir);
}

/* --- sites and connections -------------------------------------------------- */

static void drop_remote(sfs *fs, site *s);

static void close_handles(sfs *fs, site *s)
{
	sfs_file *f;

	for (f = fs->files; f != NULL; f = f->next) {
		if (f->s == s && f->h != NULL) {
			if (s->r->ops->fclose(s->r, f->h) != 0 && f->hmode != RO_READ)
				f->lost = 1;
			f->h = NULL;
		}
	}
}

/* sends what open handles of a file still hold (SFTP collects writes), so
 * that the server has it all before the file is used another way */
static int flush_path(sfs *fs, site *s, const char *path)
{
	sfs_file *f;
	int rc = 0;

	if (s->r == NULL || s->r->ops->fflush == NULL)
		return 0;
	for (f = fs->files; f != NULL; f = f->next) {
		if (f->s == s && f->h != NULL && f->hmode != RO_READ &&
		    (path == NULL || strcmp(f->path, path) == 0)) {
			int e = s->r->ops->fflush(s->r, f->h);

			if (e != 0 && rc == 0)
				rc = e;
			if (e == -ENOTCONN)
				break;
		}
	}
	if (rc == -ENOTCONN) {
		drop_remote(fs, s);
		rc = -EIO;
	}
	return rc;
}

static void drop_remote(sfs *fs, site *s)
{
	if (s->r == NULL)
		return;
	plat_log(LOG_INFO, "%s: disconnected\n", s->cfg.name);
	close_handles(fs, s);
	s->r->ops->close(s->r);
	s->r = NULL;
	/* other programs may have changed things meanwhile */
	list_flush(s);
}

static void site_free(sfs *fs, site *s)
{
	drop_remote(fs, s);
	list_flush(s);
	memset(s->password, 0, sizeof(s->password));
	memset(s->cfg.password, 0, sizeof(s->cfg.password));
	free(s);
}

static void site_release(sfs *fs, site *s)
{
	if (s->nfiles > 0 || !s->gone)
		return;
	{
		site **pp = &fs->gone;

		while (*pp != NULL && *pp != s)
			pp = &(*pp)->next;
		if (*pp == s)
			*pp = s->next;
	}
	site_free(fs, s);
}

static void report(sfs *fs, site *s, const char *text)
{
	char title[200];

	snprintf(title, sizeof(title), "%s %s", fs->opt.display, s != NULL ? s->cfg.name : "");
	plat_report(title, text);
}

/* the site file changed: keep what is the same, start again for the rest */
static void apply_config(sfs *fs, site_list *l)
{
	site **n = calloc((size_t)(l->count > 0 ? l->count : 1), sizeof(*n));
	int i, j;

	if (n == NULL)
		return;
	for (i = 0; i < l->count; i++) {
		site *s = NULL;

		for (j = 0; j < fs->nsites; j++) {
			if (fs->site[j] != NULL && name_casecmp(fs->site[j]->cfg.name, l->site[i].name) == 0) {
				s = fs->site[j];
				fs->site[j] = NULL;
				break;
			}
		}
		if (s != NULL && !site_cfg_equal(&s->cfg, &l->site[i])) {
			int same_server = strcmp(s->cfg.host, l->site[i].host) == 0 &&
			                  site_port(&s->cfg) == site_port(&l->site[i]) &&
			                  s->cfg.protocol == l->site[i].protocol &&
			                  strcmp(s->cfg.user, l->site[i].user) == 0 &&
			                  strcmp(s->cfg.folder, l->site[i].folder) == 0 &&
			                  strcmp(s->cfg.keyfile, l->site[i].keyfile) == 0;

			if (same_server && s->r != NULL) {
				/* only the name's case or the password: keep the connection */
				s->cfg = l->site[i];
			} else if (s->nfiles > 0) {
				/* files are open on the old server: keep it for them */
				s->gone = 1;
				s->next = fs->gone;
				fs->gone = s;
				s = NULL;
			} else {
				drop_remote(fs, s);
				list_flush(s);
				s->cfg = l->site[i];
				s->have_password = 0;
			}
			if (s != NULL) {
				s->refused = 0;
				s->reported = 0;
				s->last_fail = 0;
			}
		}
		if (s == NULL) {
			s = calloc(1, sizeof(*s));
			if (s == NULL)
				continue;
			s->cfg = l->site[i];
		}
		n[i] = s;
	}
	for (j = 0; j < fs->nsites; j++) {
		site *s = fs->site[j];

		if (s == NULL)
			continue;
		if (s->nfiles > 0) {
			s->gone = 1;
			s->next = fs->gone;
			fs->gone = s;
		} else {
			site_free(fs, s);
		}
	}
	free(fs->site);
	fs->site = n;
	fs->nsites = l->count;
	/* sites that could not be made */
	for (i = 0, j = 0; i < fs->nsites; i++)
		if (n[i] != NULL)
			n[j++] = n[i];
	fs->nsites = j;
	plat_log(LOG_INFO, "%d sites\n", fs->nsites);
}

static void load_config(sfs *fs, int force)
{
	struct stat st;
	site_list l;
	char err[600];
	int rc, exists, skipped = 0;

	if (!force && fs->cfg_checked != 0 && plat_ms() - fs->cfg_checked < CFG_CHECK_MS)
		return;
	fs->cfg_checked = plat_ms();
	exists = stat(fs->sites_file, &st) == 0;
	if (!force && exists == fs->cfg_exists &&
	    (!exists || (st.st_mtime == fs->cfg_mtime && (int64_t)st.st_size == fs->cfg_size)))
		return;
	fs->cfg_exists = exists;
	fs->cfg_mtime = exists ? st.st_mtime : 0;
	fs->cfg_size = exists ? (int64_t)st.st_size : 0;
	sites_init(&l);
	rc = exists ? sites_load(fs->sites_file, &l, err, sizeof(err), &skipped) : -ENOENT;
	if (rc == -EINVAL) {
		/* keep the sites we have */
		if (!fs->cfg_reported) {
			char msg[800];

			fs->cfg_reported = 1;
			snprintf(msg, sizeof(msg), "The site list cannot be read:\n%s\n\n"
			         "Use sftphprefs to make a new one.", err);
			report(fs, NULL, msg);
		}
		sites_free(&l);
		return;
	}
	fs->cfg_reported = 0;
	if (rc != 0 && rc != -ENOENT) {
		sites_free(&l);
		return;
	}
	if (skipped > 0)
		plat_log(LOG_ERR, "%d sites in %s were skipped (bad or repeated names)\n",
		         skipped, fs->sites_file);
	apply_config(fs, &l);
	memset(l.site, 0, (size_t)l.cap * sizeof(*l.site));
	sites_free(&l);
}

/* the password typed into the requester goes into the site file */
static void save_password(sfs *fs, site *s)
{
	const char *dirs[2];
	int i;

	dirs[0] = fs->opt.config_dir;
	dirs[1] = fs->opt.save_dir;
	snprintf(s->cfg.password, sizeof(s->cfg.password), "%s", s->password);
	for (i = 0; i < 2; i++) {
		char file[400];
		site_list l;
		int k;

		if (dirs[i] == NULL)
			continue;
		snprintf(file, sizeof(file), "%s/%s", dirs[i], CFG_SITES);
		sites_init(&l);
		if (sites_load(file, &l, NULL, 0, NULL) == 0 &&
		    (k = sites_find(&l, s->cfg.name)) >= 0) {
			snprintf(l.site[k].password, sizeof(l.site[k].password), "%s", s->password);
			if (sites_save(file, &l) != 0)
				plat_log(LOG_ERR, "cannot save the password in %s\n", file);
		}
		memset(l.site, 0, (size_t)l.cap * sizeof(*l.site));
		sites_free(&l);
	}
	/* that change is ours: no need to start the site again */
	{
		struct stat st;

		if (stat(fs->sites_file, &st) == 0) {
			fs->cfg_mtime = st.st_mtime;
			fs->cfg_size = (int64_t)st.st_size;
		}
	}
}

static int hostkey_cb(void *ud, const char *host, int port, const char *keytype,
                      const char *fingerprint, int mismatch)
{
	site *s = ud;

	return plat_ask_hostkey(s->cfg.name, host, port, keytype, fingerprint, mismatch);
}

static const char *fail_hint(int fail)
{
	switch (fail) {
	case RFAIL_NETWORK:
		return "Check the server address and port in sftphprefs,\n"
		       "and that the network is up.";
	case RFAIL_LOGIN:
		return "The user name or password was not accepted.";
	case RFAIL_HOSTKEY:
		return "If the server's key really changed, remove the old key with\n"
		       "\"Forget host key\" in sftphprefs.";
	case RFAIL_KEYFILE:
		return "Check the key file in sftphprefs.";
	case RFAIL_PROTOCOL:
		return "Check the protocol and port in sftphprefs.";
	case RFAIL_FOLDER:
		return "Check the folder in sftphprefs.";
	}
	return "";
}

/* Connects on first use. Returns 0 or -errno. */
static int need_remote(sfs *fs, site *s)
{
	remote_env env;
	char err[600], msg[1200];
	const char *problem = NULL;
	int tries, rc = ERR_OFFLINE, fail = RFAIL_NETWORK, remember = 0, asked = 0;
	int ask_first;

	s->last_use = plat_ms();
	if (s->r != NULL)
		return 0;
	if (s->refused && s->refused_kh != 0) {
		/* a changed known_hosts (sftphprefs: Forget host key) allows a
		 * new try */
		struct stat st;
		time_t now_kh = fs->opt.known_hosts != NULL && stat(fs->opt.known_hosts, &st) == 0 ?
		                st.st_mtime : 1;

		if (now_kh != s->refused_kh) {
			s->refused = 0;
			s->reported = 0;
			s->last_fail = 0;
		}
	}
	if (s->refused)
		return ERR_OFFLINE;
	if (s->last_fail != 0 && plat_ms() - s->last_fail < RETRY_MS)
		return s->last_err;
	if (s->cfg.protocol == PROTO_SFTP && s->cfg.user[0] == '\0') {
		if (!s->reported) {
			s->reported = 1;
			report(fs, s, "SFTP needs a user name. Set it in sftphprefs.");
		}
		s->refused = 1;
		return ERR_OFFLINE;
	}
	memset(&env, 0, sizeof(env));
	env.ud = s;
	env.hostkey = hostkey_cb;
	env.known_hosts = fs->opt.known_hosts;
	env.connect_timeout_ms = fs->opt.connect_timeout_ms;
	env.io_timeout_ms = fs->opt.io_timeout_ms;
	/* a password is needed first unless the site has one, logs in
	 * anonymously (FTP) or with a key file (SFTP) */
	ask_first = s->cfg.user[0] != '\0' && s->cfg.password[0] == '\0' &&
	            !(s->cfg.protocol == PROTO_SFTP && s->cfg.keyfile[0] != '\0');
	for (tries = 0; tries < 4; tries++) {
		const char *pass;

		if (ask_first && !s->have_password) {
			if (asked >= 3 ||
			    !plat_ask_password(s->cfg.name, s->cfg.user, s->cfg.host,
			                       s->cfg.protocol == PROTO_SFTP && s->cfg.keyfile[0] != '\0' ?
			                       s->cfg.keyfile : NULL,
			                       problem, s->password, sizeof(s->password), &remember)) {
				/* Workbench looks at open folders all the time: do not ask
				 * again until "DiskChange" or a change of the site */
				s->refused = 1;
				memset(s->password, 0, sizeof(s->password));
				return ERR_OFFLINE;
			}
			asked++;
			s->have_password = 1;
		}
		pass = s->have_password ? s->password : s->cfg.password;
		plat_log(LOG_INFO, "%s: connecting to %s:%d (%s)\n", s->cfg.name, s->cfg.host,
		         site_port(&s->cfg), proto_name(s->cfg.protocol));
		if (s->cfg.protocol == PROTO_SFTP)
			rc = sftp_connect(&s->cfg, pass, &env, &s->r, err, sizeof(err), &fail);
		else
			rc = ftp_connect(&s->cfg, pass, &env, &s->r, err, sizeof(err), &fail);
		if (rc == 0) {
			if (remember && s->have_password)
				save_password(fs, s);
			s->reported = 0;
			s->last_fail = 0;
			s->last_use = s->last_keepalive = plat_ms();
			list_flush(s);
			plat_log(LOG_INFO, "%s: connected\n", s->cfg.name);
			return 0;
		}
		plat_log(LOG_ERR, "%s: %s\n", s->cfg.name, err);
		if (fail == RFAIL_LOGIN && s->cfg.user[0] != '\0') {
			/* wrong or missing password: ask (again) */
			s->have_password = 0;
			ask_first = 1;
			problem = err;
			continue;
		}
		break;
	}
	s->refused_kh = 0;
	if (fail == RFAIL_HOSTKEY) {
		struct stat st;

		s->refused = 1;
		s->refused_kh = fs->opt.known_hosts != NULL && stat(fs->opt.known_hosts, &st) == 0 ?
		                st.st_mtime : 1;
	}
	if (!s->reported) {
		s->reported = 1;
		snprintf(msg, sizeof(msg), "Cannot connect to %s (%s, port %d):\n%s\n\n%s",
		         s->cfg.name, s->cfg.host, site_port(&s->cfg), err, fail_hint(fail));
		report(fs, s, msg);
	}
	s->last_fail = plat_ms();
	s->last_err = ERR_OFFLINE;
	return s->last_err;
}

/* runs a server call, connecting first; a lost connection is made again
 * once */
#define RUN(fs, s, rc, call)                                  \
	do {                                                      \
		int run_try_;                                         \
		for (run_try_ = 0; run_try_ < 2; run_try_++) {        \
			rc = need_remote(fs, s);                          \
			if (rc != 0)                                      \
				break;                                        \
			rc = (call);                                      \
			if (rc != -ENOTCONN)                              \
				break;                                        \
			drop_remote(fs, s);                               \
		}                                                     \
		if (rc == -ENOTCONN)                                  \
			rc = -EIO;                                        \
	} while (0)

/* --- paths ----------------------------------------------------------------- */

enum { AT_ROOT, AT_SITE, AT_ICON };

typedef struct {
	int         kind;
	site       *s;
	const char *rest;    /* path in the site ("/" for its top) */
} where;

static site *find_site(sfs *fs, const char *name, size_t len)
{
	int i;

	for (i = 0; i < fs->nsites; i++) {
		const char *n = fs->site[i]->cfg.name;
		char tmp[sizeof(fs->site[i]->cfg.name)];

		if (len >= sizeof(tmp))
			continue;
		memcpy(tmp, name, len);
		tmp[len] = '\0';
		if (name_casecmp(n, tmp) == 0)
			return fs->site[i];
	}
	return NULL;
}

static void load_icon(sfs *fs)
{
	FILE *f;

	if (fs->icon_loaded)
		return;
	fs->icon_loaded = 1;
	free(fs->icon);
	fs->icon = NULL;
	fs->iconlen = 0;
	if (fs->opt.icon_file == NULL || (f = fopen(fs->opt.icon_file, "rb")) == NULL)
		return;
	fs->icon = malloc(ICON_MAX);
	if (fs->icon != NULL) {
		fs->iconlen = fread(fs->icon, 1, ICON_MAX, f);
		if (fs->iconlen == 0 || fs->iconlen == ICON_MAX) {
			free(fs->icon);
			fs->icon = NULL;
			fs->iconlen = 0;
		}
	}
	fclose(f);
}

/* path must be normalised ("/a/b") */
static int locate(sfs *fs, const char *path, where *w)
{
	const char *p = path + 1, *slash;
	size_t len;

	w->s = NULL;
	w->rest = NULL;
	if (path[1] == '\0') {
		w->kind = AT_ROOT;
		return 0;
	}
	load_config(fs, 0);
	slash = strchr(p, '/');
	len = slash != NULL ? (size_t)(slash - p) : strlen(p);
	w->s = find_site(fs, p, len);
	if (w->s != NULL) {
		w->kind = AT_SITE;
		w->rest = slash != NULL ? slash : root_path;
		return 0;
	}
	if (slash == NULL && len > 5 && strcasecmp(p + len - 5, ".info") == 0) {
		load_icon(fs);
		w->s = find_site(fs, p, len - 5);
		if (w->s != NULL && fs->icon != NULL) {
			w->kind = AT_ICON;
			return 0;
		}
	}
	return -ENOENT;
}

/* a site's top folder or the root and its entries cannot be changed */
static int fixed(const where *w)
{
	return w->kind != AT_SITE || strcmp(w->rest, "/") == 0;
}

/* open files that are not on the server yet */
static sfs_file *pending_file(sfs *fs, site *s, const char *dir, const char *name, int nocase)
{
	sfs_file *f;

	for (f = fs->files; f != NULL; f = f->next) {
		char *fp;
		int same;

		if (f->s != s || !f->is_new)
			continue;
		fp = path_parent(f->path);
		same = fp != NULL && strcmp(fp, dir) == 0 &&
		       (nocase ? name_casecmp(path_basename(f->path), name) == 0
		               : strcmp(path_basename(f->path), name) == 0);
		free(fp);
		if (same)
			return f;
	}
	return NULL;
}

static void file_rstat(sfs_file *f, rstat *st)
{
	memset(st, 0, sizeof(*st));
	st->size = f->size;
	st->mtime = f->mtime;
	st->mode = f->mode;
}

struct fetch_ctx {
	listing *l;
	int      failed;
};

static int fetch_one(void *ud, const char *name, const rstat *st)
{
	struct fetch_ctx *c = ud;

	if (list_add(c->l, name, st) != 0) {
		c->failed = 1;
		return 1;
	}
	return 0;
}

static int list_get(sfs *fs, site *s, const char *path, listing **out)
{
	listing *l = list_find(s, path);
	struct fetch_ctx c;
	int rc;

	if (l != NULL && plat_ms() - l->when < (int64_t)fs->opt.cache_ms) {
		*out = l;
		return 0;
	}
	if (l != NULL)
		list_drop(s, path);
	if (s->nlists >= MAX_LISTINGS)
		list_trim(s, fs->opt.cache_ms);
	l = calloc(1, sizeof(*l));
	if (l == NULL)
		return -ENOMEM;
	l->path = xstrdup(path);
	if (l->path == NULL) {
		free(l);
		return -ENOMEM;
	}
	for (;;) {
		c.l = l;
		c.failed = 0;
		rc = need_remote(fs, s);
		if (rc != 0)
			break;
		rc = s->r->ops->list(s->r, path, fetch_one, &c);
		if (rc == 0 && c.failed)
			rc = -ENOMEM;
		if (rc == -ENOTCONN && l->n == 0 && !s->gone) {
			/* once more with a new connection */
			drop_remote(fs, s);
			rc = need_remote(fs, s);
			if (rc != 0)
				break;
			rc = s->r->ops->list(s->r, path, fetch_one, &c);
			if (rc == 0 && c.failed)
				rc = -ENOMEM;
		}
		break;
	}
	if (rc == -ENOTCONN) {
		drop_remote(fs, s);
		rc = -EIO;
	}
	if (rc != 0) {
		listing_free(l);
		return rc;
	}
	l->when = plat_ms();
	l->next = s->lists[hash(path)];
	s->lists[hash(path)] = l;
	s->nlists++;
	*out = l;
	return 0;
}

/* Finds path in a site, without regard to case where needed. On success
 * *real is the path as the server has it (malloc'ed). For a missing last
 * part -ENOENT is returned and *real is the path to create (the folder as
 * the server has it, the name as given). */
static int resolve(sfs *fs, site *s, const char *path, char **real, rstat *st)
{
	char *dir, *cur = NULL, *norm = NULL, *p, *next;
	const char *name;
	listing *l;
	entry *e;
	sfs_file *pf;
	int rc;

	*real = NULL;
	if (strcmp(path, "/") == 0) {
		memset(st, 0, sizeof(*st));
		st->is_dir = 1;
		st->mode = -1;
		*real = xstrdup("/");
		return *real != NULL ? 0 : -ENOMEM;
	}
	/* usually the folder is right and only its listing is needed */
	dir = path_parent(path);
	if (dir == NULL)
		return -ENOMEM;
	name = path_basename(path);
	rc = list_get(fs, s, dir, &l);
	if (rc == 0) {
		e = list_entry(l, name, 1);
		if (e != NULL) {
			*st = e->st;
			*real = path_join(dir, e->name);
		} else if ((pf = pending_file(fs, s, dir, name, 1)) != NULL) {
			file_rstat(pf, st);
			*real = path_join(dir, path_basename(pf->path));
		} else {
			*real = path_join(dir, name);
			free(dir);
			return *real != NULL ? -ENOENT : -ENOMEM;
		}
		free(dir);
		return *real != NULL ? 0 : -ENOMEM;
	}
	free(dir);
	if (rc != -ENOENT && rc != -ENOTDIR && rc != -EACCES)
		return rc;

	/* walk from the top, matching each part without regard to case */
	norm = xstrdup(path);
	cur = xstrdup("/");
	if (norm == NULL || cur == NULL) {
		free(norm);
		free(cur);
		return -ENOMEM;
	}
	p = norm + 1;
	while (*p != '\0') {
		char *cand;
		int last;

		next = strchr(p, '/');
		if (next != NULL)
			*next = '\0';
		last = next == NULL;
		rc = list_get(fs, s, cur, &l);
		if (rc != 0)
			goto fail;
		e = list_entry(l, p, 1);
		if (e == NULL) {
			pf = pending_file(fs, s, cur, p, 1);
			if (pf != NULL && last) {
				file_rstat(pf, st);
				cand = path_join(cur, path_basename(pf->path));
				free(cur);
				free(norm);
				*real = cand;
				return cand != NULL ? 0 : -ENOMEM;
			}
			if (last) {
				*real = path_join(cur, p);
				free(cur);
				free(norm);
				return *real != NULL ? -ENOENT : -ENOMEM;
			}
			rc = -ENOENT;
			goto fail;
		}
		if (!last && !e->st.is_dir) {
			rc = -ENOTDIR;
			goto fail;
		}
		*st = e->st;
		cand = path_join(cur, e->name);
		if (cand == NULL) {
			rc = -ENOMEM;
			goto fail;
		}
		free(cur);
		cur = cand;
		if (last)
			break;
		p = next + 1;
	}
	free(norm);
	*real = cur;
	return 0;

fail:
	free(cur);
	free(norm);
	return rc;
}

static void to_sfs_stat(sfs *fs, const rstat *r, sfs_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->is_dir = r->is_dir;
	st->size = r->is_dir ? 0 : r->size;
	st->mtime = r->mtime;
	if (r->mode < 0)
		st->mode = 0755;
	else if (r->is_dir)
		st->mode = r->mode & 0777;
	else
		/* files show the "e" bit, as on other Amiga disks */
		st->mode = (r->mode & 0777) | 0100;
	st->readonly = fs->opt.readonly;
}

/* the root and its entries carry the date of the site list */
static void root_dir_stat(sfs *fs, sfs_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->mtime = fs->cfg_mtime;
	st->is_dir = 1;
	st->mode = 0555;
	st->readonly = 1;
}

/* overlays what open files know better than a listing */
static void open_file_stat(sfs *fs, site *s, const char *real, rstat *st)
{
	sfs_file *f;

	for (f = fs->files; f != NULL; f = f->next) {
		if (f->s == s && (f->tf != NULL || f->hmode != RO_READ) && strcmp(f->path, real) == 0) {
			st->size = f->size;
			if (f->mtime != 0)
				st->mtime = f->mtime;
			return;
		}
	}
}

/* --- set-up ------------------------------------------------------------------- */

sfs *sfs_new(const sfs_options *opt)
{
	sfs *fs = calloc(1, sizeof(*fs));

	if (fs == NULL)
		return NULL;
	fs->opt = *opt;
	if (fs->opt.cache_ms <= 0)
		fs->opt.cache_ms = 5000;
	if (fs->opt.idle_ms <= 0)
		fs->opt.idle_ms = 300000;
	if (fs->opt.connect_timeout_ms <= 0)
		fs->opt.connect_timeout_ms = 20000;
	if (fs->opt.io_timeout_ms <= 0)
		fs->opt.io_timeout_ms = 60000;
	if (fs->opt.display == NULL)
		fs->opt.display = "SFTP:";
	snprintf(fs->sites_file, sizeof(fs->sites_file), "%s/%s", opt->config_dir, CFG_SITES);
	load_config(fs, 1);
	return fs;
}

void sfs_free(sfs *fs)
{
	int i;

	if (fs == NULL)
		return;
	while (fs->files != NULL)
		sfs_release(fs, fs->files);
	for (i = 0; i < fs->nsites; i++)
		site_free(fs, fs->site[i]);
	while (fs->gone != NULL) {
		site *s = fs->gone;
		fs->gone = s->next;
		site_free(fs, s);
	}
	free(fs->site);
	free(fs->icon);
	free(fs);
}

int sfs_connected(sfs *fs)
{
	int i, n = 0;

	for (i = 0; i < fs->nsites; i++)
		if (fs->site[i]->r != NULL)
			n++;
	return n;
}

void sfs_idle(sfs *fs)
{
	int64_t now = plat_ms();
	int i;

	for (i = 0; i < fs->nsites; i++) {
		site *s = fs->site[i];

		if (s->nlists > 0)
			list_trim(s, LIST_KEEP_MS);
		if (s->r == NULL)
			continue;
		if (s->nfiles == 0 && now - s->last_use > fs->opt.idle_ms) {
			drop_remote(fs, s);
			continue;
		}
		if (now - s->last_keepalive > KEEPALIVE_MS) {
			s->last_keepalive = now;
			if (s->r->ops->idle(s->r) == -ENOTCONN)
				drop_remote(fs, s);
		}
	}
}

void sfs_reset(sfs *fs)
{
	int i;

	load_config(fs, 1);
	for (i = 0; i < fs->nsites; i++) {
		fs->site[i]->refused = 0;
		fs->site[i]->reported = 0;
		fs->site[i]->last_fail = 0;
	}
	fs->cfg_reported = 0;
	fs->icon_loaded = 0;
}

/* --- reading ------------------------------------------------------------------ */

int sfs_getattr(sfs *fs, const char *path, sfs_stat *st)
{
	char *norm = path_normalize(path), *real = NULL;
	where w;
	rstat rs;
	int rc;

	if (norm == NULL)
		return -ENOMEM;
	rc = locate(fs, norm, &w);
	if (rc != 0)
		goto out;
	if (w.kind == AT_ROOT || (w.kind == AT_SITE && strcmp(w.rest, "/") == 0)) {
		/* no connection for the root and the sites' own folders */
		root_dir_stat(fs, st);
		goto out;
	}
	if (w.kind == AT_ICON) {
		memset(st, 0, sizeof(*st));
		st->mtime = fs->cfg_mtime;
		st->size = (int64_t)fs->iconlen;
		st->mode = 0444;
		st->readonly = 1;
		goto out;
	}
	rc = resolve(fs, w.s, w.rest, &real, &rs);
	if (rc == 0) {
		open_file_stat(fs, w.s, real, &rs);
		to_sfs_stat(fs, &rs, st);
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_readdir(sfs *fs, const char *path, sfs_fill_fn fill, void *ud)
{
	char *norm = path_normalize(path), *real = NULL;
	where w;
	rstat rs;
	listing *l;
	sfs_file *f;
	size_t i;
	int rc;

	if (norm == NULL)
		return -ENOMEM;
	rc = locate(fs, norm, &w);
	if (rc != 0)
		goto out;
	if (w.kind == AT_ICON) {
		rc = -ENOTDIR;
		goto out;
	}
	if (w.kind == AT_ROOT) {
		sfs_stat st;
		int k;

		load_config(fs, 1);
		load_icon(fs);
		for (k = 0; k < fs->nsites; k++) {
			root_dir_stat(fs, &st);
			if (fill(ud, fs->site[k]->cfg.name, &st) != 0)
				goto out;
		}
		if (fs->icon != NULL) {
			for (k = 0; k < fs->nsites; k++) {
				char name[sizeof(fs->site[k]->cfg.name) + 6];

				memset(&st, 0, sizeof(st));
				st.mtime = fs->cfg_mtime;
				st.size = (int64_t)fs->iconlen;
				st.mode = 0444;
				st.readonly = 1;
				snprintf(name, sizeof(name), "%s.info", fs->site[k]->cfg.name);
				if (fill(ud, name, &st) != 0)
					goto out;
			}
		}
		goto out;
	}
	rc = resolve(fs, w.s, w.rest, &real, &rs);
	if (rc != 0)
		goto out;
	if (!rs.is_dir) {
		rc = -ENOTDIR;
		goto out;
	}
	rc = list_get(fs, w.s, real, &l);
	if (rc != 0)
		goto out;
	for (i = 0; i < l->n; i++) {
		sfs_stat st;
		rstat es = l->e[i].st;
		char *ep = path_join(real, l->e[i].name);

		if (ep != NULL) {
			open_file_stat(fs, w.s, ep, &es);
			free(ep);
		}
		to_sfs_stat(fs, &es, &st);
		if (fill(ud, l->e[i].name, &st) != 0)
			goto out;
	}
	/* new files still being written (FTP) */
	for (f = fs->files; f != NULL; f = f->next) {
		char *fp;

		if (f->s != w.s || !f->is_new)
			continue;
		fp = path_parent(f->path);
		if (fp != NULL && strcmp(fp, real) == 0 && list_entry(l, path_basename(f->path), 0) == NULL) {
			sfs_stat st;

			file_rstat(f, &rs);
			to_sfs_stat(fs, &rs, &st);
			if (fill(ud, path_basename(f->path), &st) != 0) {
				free(fp);
				goto out;
			}
		}
		free(fp);
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_statfs(sfs *fs, const char *path, int64_t *total, int64_t *avail)
{
	char *norm = path_normalize(path);
	where w;
	int rc;

	/* a network volume has no size of its own: show plenty of room, so
	 * that copying is not refused */
	*total = BIG_DISK;
	*avail = fs->opt.readonly ? 0 : BIG_DISK;
	if (norm == NULL)
		return 0;
	rc = locate(fs, norm, &w);
	if (rc == 0 && w.kind == AT_SITE && w.s->r != NULL) {
		int64_t t, a;

		if (w.s->r->ops->statfs(w.s->r, w.rest, &t, &a) == 0 && t > 0) {
			*total = t;
			*avail = fs->opt.readonly ? 0 : a;
		}
	}
	free(norm);
	return 0;
}

/* --- files ---------------------------------------------------------------------- */

static sfs_file *file_new(sfs *fs, site *s, char *real)
{
	sfs_file *f = calloc(1, sizeof(*f));

	if (f == NULL) {
		free(real);
		return NULL;
	}
	f->s = s;
	f->path = real;
	f->hmode = RO_READ;
	f->mode = -1;
	f->next = fs->files;
	fs->files = f;
	if (s != NULL)
		s->nfiles++;
	return f;
}

int sfs_open(sfs *fs, const char *path, int write, sfs_file **fh)
{
	char *norm = path_normalize(path), *real = NULL;
	sfs_file *f;
	where w;
	rstat rs;
	int rc;

	(void)write;   /* the server is asked on the first write */
	if (norm == NULL)
		return -ENOMEM;
	rc = locate(fs, norm, &w);
	if (rc != 0)
		goto out;
	if (w.kind == AT_ROOT || (w.kind == AT_SITE && strcmp(w.rest, "/") == 0)) {
		rc = -EISDIR;
		goto out;
	}
	if (w.kind == AT_ICON) {
		f = file_new(fs, w.s, xstrdup(norm));
		if (f == NULL) {
			rc = -ENOMEM;
			goto out;
		}
		f->icon = 1;
		f->size = (int64_t)fs->iconlen;
		*fh = f;
		goto out;
	}
	rc = resolve(fs, w.s, w.rest, &real, &rs);
	if (rc != 0)
		goto out;
	if (rs.is_dir) {
		rc = -EISDIR;
		goto out;
	}
	/* another handle may still hold writes to it */
	flush_path(fs, w.s, real);
	f = file_new(fs, w.s, real);
	real = NULL;
	if (f == NULL) {
		rc = -ENOMEM;
		goto out;
	}
	f->size = rs.size;
	f->mtime = rs.mtime;
	f->mode = rs.mode;
	/* an FTP file still being written by another handle */
	{
		sfs_file *o;

		for (o = fs->files; o != NULL; o = o->next)
			if (o != f && o->s == w.s && o->is_new && strcmp(o->path, f->path) == 0)
				f->size = o->size;
	}
	*fh = f;
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_create(sfs *fs, const char *path, sfs_file **fh)
{
	char *norm = path_normalize(path), *real = NULL;
	sfs_file *f;
	where w;
	rstat rs;
	int rc;

	if (norm == NULL)
		return -ENOMEM;
	rc = locate(fs, norm, &w);
	if (rc == -ENOENT && strchr(norm + 1, '/') == NULL)
		rc = -EROFS; /* a new entry at the root */
	if (rc != 0)
		goto out;
	if (fixed(&w)) {
		rc = -EROFS;
		goto out;
	}
	if (fs->opt.readonly) {
		rc = -EROFS;
		goto out;
	}
	rc = resolve(fs, w.s, w.rest, &real, &rs);
	if (rc == 0 && rs.is_dir) {
		rc = -EISDIR;
		goto out;
	}
	if (rc != 0 && rc != -ENOENT)
		goto out;
	f = file_new(fs, w.s, real);
	real = NULL;
	if (f == NULL) {
		rc = -ENOMEM;
		goto out;
	}
	f->mtime = plat_time();
	f->mode = rc == 0 ? rs.mode : 0644;
	if (w.s->cfg.protocol == PROTO_SFTP) {
		RUN(fs, w.s, rc, w.s->r->ops->open(w.s->r, f->path, RO_CREATE, &f->h));
		if (rc == 0)
			f->hmode = RO_CREATE;
	} else {
		f->tf = tmpf_create(fs->opt.tempdir, &rc);
		if (f->tf != NULL) {
			rc = 0;
			f->is_new = 1;
			f->dirty = 1;
		}
	}
	if (rc != 0) {
		sfs_release(fs, f);
		goto out;
	}
	file_rstat(f, &rs);
	list_set(w.s, f->path, &rs);
	*fh = f;
out:
	free(real);
	free(norm);
	return rc;
}

/* opens the server handle (again, after a reconnect) */
static int ensure_handle(sfs *fs, sfs_file *f, int mode)
{
	site *s = f->s;
	int rc;

	if (f->h != NULL && (mode == RO_READ || f->hmode != RO_READ))
		return 0;
	if (f->h != NULL) {
		/* from reading to writing */
		s->r->ops->fclose(s->r, f->h);
		f->h = NULL;
	}
	if (f->hmode == RO_CREATE)
		mode = RO_WRITE;   /* created already: do not empty it again */
	else if (f->hmode != RO_READ)
		mode = RO_WRITE;
	RUN(fs, s, rc, s->r->ops->open(s->r, f->path, mode, &f->h));
	if (rc == 0 && f->hmode == RO_READ)
		f->hmode = mode;
	return rc;
}

int sfs_read(sfs *fs, sfs_file *f, char *buf, size_t size, int64_t off)
{
	site *s = f->s;
	int rc, tries;

	if (f->icon) {
		if (off >= (int64_t)fs->iconlen)
			return 0;
		if ((int64_t)size > (int64_t)fs->iconlen - off)
			size = (size_t)((int64_t)fs->iconlen - off);
		memcpy(buf, fs->icon + off, size);
		return (int)size;
	}
	if (size == 0)
		return 0;
	if (f->tf != NULL)
		return tmpf_pread(f->tf, buf, size, off);
	/* FTP: another handle is writing this file; its contents are in that
	 * handle's temp file until it is closed */
	{
		sfs_file *w;

		for (w = fs->files; w != NULL; w = w->next)
			if (w != f && w->s == s && w->tf != NULL && w->dirty && strcmp(w->path, f->path) == 0)
				return tmpf_pread(w->tf, buf, size, off);
	}
	/* FTP cannot read past the end cheaply: the listing knows the size */
	if (s->cfg.protocol == PROTO_FTP && f->size > 0 && off >= f->size)
		return 0;
	for (tries = 0; tries < 2; tries++) {
		rc = ensure_handle(fs, f, RO_READ);
		if (rc != 0)
			return rc;
		rc = s->r->ops->pread(s->r, f->h, buf, size, off);
		if (rc != -ENOTCONN)
			break;
		drop_remote(fs, s);
	}
	s->last_use = plat_ms();
	return rc == -ENOTCONN ? -EIO : rc;
}

/* FTP: the file's contents go into the temp file before the first change */
static int ensure_temp(sfs *fs, sfs_file *f)
{
	char *buf;
	int64_t off = 0;
	int rc = 0;

	if (f->tf != NULL)
		return 0;
	f->tf = tmpf_create(fs->opt.tempdir, &rc);
	if (f->tf == NULL)
		return rc != 0 ? rc : -EIO;
	if (f->size == 0)
		return 0;
	buf = malloc(65536);
	if (buf == NULL) {
		rc = -ENOMEM;
		goto fail;
	}
	for (;;) {
		int n;

		rc = ensure_handle(fs, f, RO_READ);
		if (rc != 0)
			break;
		n = f->s->r->ops->pread(f->s->r, f->h, buf, 65536, off);
		if (n == -ENOTCONN) {
			drop_remote(fs, f->s);
			n = ensure_handle(fs, f, RO_READ);
			if (n == 0)
				n = f->s->r->ops->pread(f->s->r, f->h, buf, 65536, off);
		}
		if (n < 0) {
			rc = n == -ENOTCONN ? -EIO : n;
			break;
		}
		if (n == 0)
			break;
		if (tmpf_pwrite(f->tf, buf, (size_t)n, off) != n) {
			rc = -EIO;
			break;
		}
		off += n;
	}
	free(buf);
	if (rc != 0)
		goto fail;
	f->size = off;
	if (f->h != NULL) {
		f->s->r->ops->fclose(f->s->r, f->h);
		f->h = NULL;
	}
	return 0;
fail:
	tmpf_close(f->tf);
	f->tf = NULL;
	return rc;
}

static void note_written(sfs_file *f)
{
	rstat rs;

	f->mtime = plat_time();
	file_rstat(f, &rs);
	list_set(f->s, f->path, &rs);
}

int sfs_write(sfs *fs, sfs_file *f, const char *buf, size_t size, int64_t off)
{
	site *s = f->s;
	int rc, tries;

	if (f->icon)
		return -EROFS;
	if (fs->opt.readonly)
		return -EROFS;
	if (size == 0)
		return 0;
	if (s->cfg.protocol == PROTO_FTP) {
		rc = ensure_temp(fs, f);
		if (rc != 0)
			return rc;
		rc = tmpf_pwrite(f->tf, buf, size, off);
		if (rc > 0) {
			f->dirty = 1;
			if (off + rc > f->size)
				f->size = off + rc;
			note_written(f);
		}
		return rc;
	}
	for (tries = 0; tries < 2; tries++) {
		rc = ensure_handle(fs, f, RO_WRITE);
		if (rc != 0)
			return rc;
		rc = s->r->ops->pwrite(s->r, f->h, buf, size, off);
		if (rc != -ENOTCONN)
			break;
		/* writes collected before this one are gone with the connection:
		 * trying again would leave a hole, so this write fails */
		drop_remote(fs, s);
		f->lost = 1;
		return -EIO;
	}
	if (rc > 0) {
		if (off + rc > f->size)
			f->size = off + rc;
		note_written(f);
	}
	s->last_use = plat_ms();
	return rc == -ENOTCONN ? -EIO : rc;
}

int sfs_fgetattr(sfs *fs, sfs_file *f, sfs_stat *st)
{
	rstat rs;

	if (f->icon) {
		memset(st, 0, sizeof(*st));
		st->mtime = fs->cfg_mtime;
		st->size = (int64_t)fs->iconlen;
		st->mode = 0444;
		st->readonly = 1;
		return 0;
	}
	file_rstat(f, &rs);
	to_sfs_stat(fs, &rs, st);
	return 0;
}

int sfs_ftruncate(sfs *fs, sfs_file *f, int64_t size)
{
	site *s = f->s;
	int rc;

	if (f->icon)
		return -EROFS;
	if (fs->opt.readonly)
		return -EROFS;
	if (s->cfg.protocol == PROTO_FTP) {
		if (size == 0 && f->tf == NULL) {
			/* nothing to fetch first */
			f->size = 0;
		}
		rc = ensure_temp(fs, f);
		if (rc == 0)
			rc = tmpf_truncate(f->tf, size);
		if (rc == 0) {
			f->dirty = 1;
			f->size = size;
			note_written(f);
		}
		return rc;
	}
	rc = ensure_handle(fs, f, RO_WRITE);
	if (rc != 0)
		return rc;
	RUN(fs, s, rc, s->r->ops->ftruncate(s->r, f->h, size));
	if (rc == 0) {
		f->size = size;
		note_written(f);
	}
	return rc;
}

struct up_ctx {
	tmpf   *tf;
	int64_t off;
};

static int up_read(void *ud, char *buf, size_t len)
{
	struct up_ctx *u = ud;
	int n = tmpf_pread(u->tf, buf, len, u->off);

	if (n > 0)
		u->off += n;
	return n;
}

static int upload(sfs *fs, sfs_file *f)
{
	site *s = f->s;
	struct up_ctx u;
	rstat rs;
	int rc, tries;

	for (tries = 0; tries < 2; tries++) {
		u.tf = f->tf;
		u.off = 0;
		rc = need_remote(fs, s);
		if (rc != 0)
			break;
		rc = s->r->ops->upload(s->r, f->path, up_read, &u);
		if (rc != -ENOTCONN)
			break;
		drop_remote(fs, s);
	}
	if (rc == -ENOTCONN)
		rc = -EIO;
	if (rc != 0)
		return rc;
	f->dirty = 0;
	f->is_new = 0;
	f->mtime = plat_time();
	if (f->set_mtime != 0 && s->r != NULL &&
	    s->r->ops->set_mtime(s->r, f->path, f->set_mtime) == 0)
		f->mtime = f->set_mtime;
	file_rstat(f, &rs);
	list_set(s, f->path, &rs);
	return 0;
}

int sfs_flush(sfs *fs, sfs_file *f)
{
	if (f->icon)
		return 0;
	if (f->h != NULL && f->hmode != RO_READ && f->s->r != NULL && f->s->r->ops->fflush != NULL) {
		int rc = f->s->r->ops->fflush(f->s->r, f->h);

		if (rc == -ENOTCONN) {
			drop_remote(fs, f->s);
			return -EIO;
		}
		return rc;
	}
	if (f->tf == NULL || !f->dirty)
		return 0;
	return upload(fs, f);
}

int sfs_release(sfs *fs, sfs_file *f)
{
	sfs_file **pp;
	site *s = f->s;
	int rc = 0;

	if (f->tf != NULL && f->dirty && !f->icon) {
		rc = upload(fs, f);
		if (rc != 0) {
			char kept[600], msg[1200];
			char name[300];

			snprintf(name, sizeof(name), "%s", path_basename(f->path));
			if (tmpf_keep(f->tf, fs->opt.keepdir, name, kept, sizeof(kept)) == 0) {
				snprintf(msg, sizeof(msg), "%s%s could not be saved on the server:\n%s\n\n"
				         "Its contents were kept in\n%s", s->cfg.name, f->path,
				         s->r != NULL && s->r->error[0] != '\0' ? s->r->error : strerror(-rc),
				         kept);
			} else {
				snprintf(msg, sizeof(msg), "%s%s could not be saved on the server:\n%s",
				         s->cfg.name, f->path, strerror(-rc));
				tmpf_close(f->tf);
			}
			f->tf = NULL;
			report(fs, s, msg);
			/* the listing must not show what is not there */
			if (f->is_new)
				list_remove(s, f->path);
		}
	}
	if (f->tf != NULL)
		tmpf_close(f->tf);
	if (f->h != NULL && s != NULL && s->r != NULL) {
		int e = s->r->ops->fclose(s->r, f->h);

		f->h = NULL;
		if (e != 0 && f->hmode != RO_READ) {
			f->lost = 1;
			if (rc == 0)
				rc = e;
			if (e == -ENOTCONN)
				drop_remote(fs, s);
		}
	}
	if (f->lost && s != NULL) {
		char msg[1200];

		/* filesysbox does not pass errors from Close on */
		snprintf(msg, sizeof(msg), "%s%s may be incomplete on the server:\n"
		         "the last data written could not be sent (%s).\n\n"
		         "Save or copy the file again.", s->cfg.name, f->path,
		         s->r != NULL && s->r->error[0] != '\0' ? s->r->error : "connection lost");
		report(fs, s, msg);
	}
	for (pp = &fs->files; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == f) {
			*pp = f->next;
			break;
		}
	}
	if (s != NULL) {
		s->nfiles--;
		s->last_use = plat_ms();
		site_release(fs, s);
	}
	free(f->path);
	free(f);
	return rc;
}

/* --- changes ------------------------------------------------------------------- */

/* locates a path for a change; returns the site and the real path */
static int prepare(sfs *fs, const char *path, where *w, char **norm, char **real, rstat *rs,
                   int want_exists)
{
	int rc;

	*real = NULL;
	*norm = path_normalize(path);
	if (*norm == NULL)
		return -ENOMEM;
	rc = locate(fs, *norm, w);
	if (rc == -ENOENT && strchr(*norm + 1, '/') == NULL && !want_exists)
		return -EROFS; /* a new entry at the root */
	if (rc != 0)
		return rc;
	if (fixed(w))
		return -EROFS;
	if (fs->opt.readonly)
		return -EROFS;
	rc = resolve(fs, w->s, w->rest, real, rs);
	if (rc == -ENOENT && !want_exists)
		return 1;
	return rc;
}

int sfs_truncate(sfs *fs, const char *path, int64_t size)
{
	char *norm, *real;
	sfs_file *f;
	where w;
	rstat rs;
	int rc = prepare(fs, path, &w, &norm, &real, &rs, 1);

	if (rc != 0)
		goto out;
	if (rs.is_dir) {
		rc = -EISDIR;
		goto out;
	}
	/* a file that is open here */
	for (f = fs->files; f != NULL; f = f->next) {
		if (f->s == w.s && (f->tf != NULL || f->hmode != RO_READ) && strcmp(f->path, real) == 0) {
			rc = sfs_ftruncate(fs, f, size);
			goto out;
		}
	}
	if (w.s->cfg.protocol == PROTO_SFTP) {
		RUN(fs, w.s, rc, w.s->r->ops->truncate(w.s->r, real, size));
	} else if (size == rs.size) {
		rc = 0;
	} else if (size == 0) {
		struct up_ctx u;
		tmpf *tf = tmpf_create(fs->opt.tempdir, &rc);

		if (tf != NULL) {
			u.tf = tf;
			u.off = 0;
			RUN(fs, w.s, rc, w.s->r->ops->upload(w.s->r, real, up_read, &u));
			tmpf_close(tf);
		}
	} else {
		rc = -ENOTSUP;
	}
	if (rc == 0) {
		rs.size = size;
		rs.mtime = plat_time();
		list_set(w.s, real, &rs);
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_mkdir(sfs *fs, const char *path)
{
	char *norm, *real;
	where w;
	rstat rs;
	int rc = prepare(fs, path, &w, &norm, &real, &rs, 0);

	if (rc == 0) {
		rc = -EEXIST;
		goto out;
	}
	if (rc != 1)
		goto out;
	RUN(fs, w.s, rc, w.s->r->ops->mkdir(w.s->r, real));
	if (rc == 0) {
		memset(&rs, 0, sizeof(rs));
		rs.is_dir = 1;
		rs.mode = 0755;
		rs.mtime = plat_time();
		list_set(w.s, real, &rs);
	} else if (rc == -EACCES || rc == -EIO) {
		/* the server may have refused because it exists */
		list_drop_under(w.s, "/");
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_unlink(sfs *fs, const char *path)
{
	char *norm, *real;
	where w;
	rstat rs;
	int rc = prepare(fs, path, &w, &norm, &real, &rs, 1);

	if (rc != 0)
		goto out;
	if (rs.is_dir) {
		rc = -EISDIR;
		goto out;
	}
	RUN(fs, w.s, rc, w.s->r->ops->unlink(w.s->r, real));
	if (rc == 0) {
		list_remove(w.s, real);
	} else if (rc == -ENOENT) {
		char *dir = path_parent(real);
		/* gone already: the listing was old */
		if (dir != NULL)
			list_drop(w.s, dir);
		free(dir);
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_rmdir(sfs *fs, const char *path)
{
	char *norm, *real;
	listing *l;
	where w;
	rstat rs;
	int rc = prepare(fs, path, &w, &norm, &real, &rs, 1);

	if (rc != 0)
		goto out;
	if (!rs.is_dir) {
		rc = -ENOTDIR;
		goto out;
	}
	/* servers say little more than "failed" for a folder with files in it */
	list_drop(w.s, real);
	rc = list_get(fs, w.s, real, &l);
	if (rc == 0 && l->n > 0)
		rc = -ENOTEMPTY;
	if (rc != 0)
		goto out;
	RUN(fs, w.s, rc, w.s->r->ops->rmdir(w.s->r, real));
	if (rc == 0) {
		list_drop_under(w.s, real);
		list_remove(w.s, real);
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_rename(sfs *fs, const char *from, const char *to)
{
	char *nf = NULL, *nt = NULL, *rf = NULL, *rt = NULL, *dir = NULL;
	where wf, wt;
	rstat sf, st;
	sfs_file *f;
	int rc;

	rc = prepare(fs, from, &wf, &nf, &rf, &sf, 1);
	if (rc != 0)
		goto out;
	nt = path_normalize(to);
	if (nt == NULL) {
		rc = -ENOMEM;
		goto out;
	}
	rc = locate(fs, nt, &wt);
	if (rc == -ENOENT && strchr(nt + 1, '/') == NULL)
		rc = -EROFS;
	if (rc != 0)
		goto out;
	if (fixed(&wt)) {
		rc = -EROFS;
		goto out;
	}
	if (wt.s != wf.s) {
		rc = -EXDEV;
		goto out;
	}
	rc = resolve(fs, wt.s, wt.rest, &rt, &st);
	if (rc == 0 && name_casecmp(rt, rf) != 0) {
		rc = -EEXIST;
		goto out;
	}
	if (rc != 0 && rc != -ENOENT)
		goto out;
	/* the new name as given (a change of case is a real rename here) */
	dir = path_parent(rt);
	free(rt);
	rt = dir != NULL ? path_join(dir, path_basename(wt.rest)) : NULL;
	if (rt == NULL) {
		rc = -ENOMEM;
		goto out;
	}
	if (sf.is_dir && path_is_under(rt, rf)) {
		rc = -EINVAL;
		goto out;
	}
	if (strcmp(rf, rt) == 0) {
		rc = 0;
		goto out;
	}
	RUN(fs, wf.s, rc, wf.s->r->ops->rename(wf.s->r, rf, rt));
	if (rc != 0)
		goto out;
	list_remove(wf.s, rf);
	list_set(wf.s, rt, &sf);
	if (sf.is_dir)
		list_drop_under(wf.s, rf);
	/* open files follow */
	for (f = fs->files; f != NULL; f = f->next) {
		if (f->s == wf.s && path_is_under(f->path, rf)) {
			char *np = path_join(rt, f->path + strlen(rf));

			if (strcmp(f->path, rf) == 0) {
				free(np);
				np = xstrdup(rt);
			}
			if (np != NULL) {
				free(f->path);
				f->path = np;
			}
		}
	}
out:
	free(nf);
	free(nt);
	free(rf);
	free(rt);
	return rc;
}

int sfs_utimens(sfs *fs, const char *path, time_t mtime)
{
	char *norm, *real;
	sfs_file *f;
	where w;
	rstat rs;
	int rc = prepare(fs, path, &w, &norm, &real, &rs, 1);

	if (rc == -EROFS && !fs->opt.readonly) {
		rc = 0; /* the root's dates: nothing to keep */
		goto out;
	}
	if (rc != 0)
		goto out;
	for (f = fs->files; f != NULL; f = f->next) {
		if (f->s == w.s && f->tf != NULL && strcmp(f->path, real) == 0) {
			/* set after the upload */
			f->set_mtime = mtime;
			f->mtime = mtime;
			rc = 0;
			goto out;
		}
	}
	/* data sent later would change the date again */
	flush_path(fs, w.s, real);
	RUN(fs, w.s, rc, w.s->r->ops->set_mtime(w.s->r, real, mtime));
	if (rc == -ENOSYS)
		rc = 0;   /* the server cannot: not worth an error */
	else if (rc == 0) {
		rs.mtime = mtime;
		list_set(w.s, real, &rs);
		for (f = fs->files; f != NULL; f = f->next)
			if (f->s == w.s && strcmp(f->path, real) == 0)
				f->mtime = mtime;
	}
out:
	free(real);
	free(norm);
	return rc;
}

int sfs_chmod(sfs *fs, const char *path, int mode)
{
	char *norm, *real;
	where w;
	rstat rs;
	int rc = prepare(fs, path, &w, &norm, &real, &rs, 1), newmode;

	if (rc != 0)
		goto out;
	/* Only the owner's read and write bits go to the server. The group and
	 * other bits (which AmigaDOS programs rarely set on purpose: Copy
	 * CLONE would make a web page private) and "x" (shown on every file,
	 * as Amiga files have "e") stay as they are. */
	if (rs.mode < 0) {
		rc = 0;
		goto out;
	}
	newmode = (rs.mode & 07177) | (mode & 0600);
	if (newmode == rs.mode)
		goto out;
	RUN(fs, w.s, rc, w.s->r->ops->chmod(w.s->r, real, newmode));
	if (rc == -ENOSYS)
		rc = 0;
	else if (rc == 0) {
		rs.mode = newmode;
		list_set(w.s, real, &rs);
	}
out:
	free(real);
	free(norm);
	return rc;
}
