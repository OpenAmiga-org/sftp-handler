/* sftp-handler - the site list (XML). Public domain. */
#include "config.h"
#include "util.h"

#include <errno.h>
#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const proto_names[PROTO_COUNT] = { "ftp", "sftp" };

const char *proto_name(int protocol)
{
	return protocol >= 0 && protocol < PROTO_COUNT ? proto_names[protocol] : "?";
}

int proto_from_name(const char *s)
{
	int i;

	for (i = 0; i < PROTO_COUNT; i++)
		if (strcasecmp(s, proto_names[i]) == 0)
			return i;
	return -1;
}

int proto_default_port(int protocol)
{
	return protocol == PROTO_SFTP ? 22 : 21;
}

int site_port(const site_cfg *s)
{
	return s->port > 0 ? s->port : proto_default_port(s->protocol);
}

/* --- site list ---------------------------------------------------------- */

void sites_init(site_list *l)
{
	memset(l, 0, sizeof(*l));
}

void sites_free(site_list *l)
{
	free(l->site);
	memset(l, 0, sizeof(*l));
}

int sites_add(site_list *l, const site_cfg *s)
{
	if (l->count == l->cap) {
		int ncap = l->cap ? l->cap * 2 : 8;
		site_cfg *n = realloc(l->site, (size_t)ncap * sizeof(*n));

		if (n == NULL)
			return -1;
		l->site = n;
		l->cap = ncap;
	}
	l->site[l->count] = *s;
	return l->count++;
}

void sites_remove(site_list *l, int index)
{
	if (index < 0 || index >= l->count)
		return;
	memmove(&l->site[index], &l->site[index + 1],
	        (size_t)(l->count - index - 1) * sizeof(*l->site));
	l->count--;
}

int sites_find(const site_list *l, const char *name)
{
	int i;

	for (i = 0; i < l->count; i++)
		if (name_casecmp(l->site[i].name, name) == 0)
			return i;
	return -1;
}

int site_cfg_equal(const site_cfg *a, const site_cfg *b)
{
	return strcmp(a->name, b->name) == 0 && a->protocol == b->protocol &&
	       strcmp(a->host, b->host) == 0 && site_port(a) == site_port(b) &&
	       strcmp(a->user, b->user) == 0 && strcmp(a->password, b->password) == 0 &&
	       strcmp(a->folder, b->folder) == 0 &&
	       strcmp(a->keyfile, b->keyfile) == 0;
}

int sites_equal(const site_list *a, const site_list *b)
{
	int i;

	if (a->count != b->count)
		return 0;
	for (i = 0; i < a->count; i++)
		if (!site_cfg_equal(&a->site[i], &b->site[i]))
			return 0;
	return 1;
}

/* --- names ---------------------------------------------------------------- */

const char *site_name_problem(const char *name)
{
	const unsigned char *p;
	size_t n = strlen(name);

	if (n == 0)
		return "The name is empty.";
	if (n > SITE_NAME_MAX)
		return "The name is too long (107 characters at most).";
	for (p = (const unsigned char *)name; *p != '\0'; p++) {
		if (*p == ':' || *p == '/')
			return "A name cannot contain ':' or '/'.";
		if (*p < 32 || (*p >= 127 && *p < 160))
			return "The name contains a control character.";
	}
	if (name[0] == ' ' || name[n - 1] == ' ')
		return "A name cannot start or end with a space.";
	if (n >= 5 && strcasecmp(name + n - 5, ".info") == 0)
		return "A name cannot end in \".info\" (that is used for the icons).";
	return NULL;
}

/* checks the UTF-8 form of a name read from the file */
static int utf8_name_ok(const char *name)
{
	const unsigned char *p;
	size_t n = strlen(name);

	if (n == 0 || n > sizeof(((site_cfg *)0)->name) - 1)
		return 0;
	for (p = (const unsigned char *)name; *p != '\0'; p++)
		if (*p == ':' || *p == '/' || *p < 32 || *p == 127)
			return 0;
	if (name[0] == ' ' || name[n - 1] == ' ')
		return 0;
	if (n >= 5 && strcasecmp(name + n - 5, ".info") == 0)
		return 0;
	return 1;
}

/* --- reading ---------------------------------------------------------------- */

struct parse {
	XML_Parser     xp;
	int            depth;
	int            root_ok;
	site_list     *sites;
	int            skipped;
	int            failed;
};

static void copy_attr(char *dst, size_t size, const char *val)
{
	snprintf(dst, size, "%s", val);
}

static void site_element(struct parse *ps, const XML_Char **attr)
{
	site_cfg s;
	int i, bad = 0;

	memset(&s, 0, sizeof(s));
	s.protocol = PROTO_SFTP;
	for (i = 0; attr[i] != NULL; i += 2) {
		const char *k = attr[i], *v = attr[i + 1];

		if (strcmp(k, "name") == 0) {
			if (strlen(v) >= sizeof(s.name))
				bad = 1;
			copy_attr(s.name, sizeof(s.name), v);
		} else if (strcmp(k, "protocol") == 0) {
			s.protocol = proto_from_name(v);
			if (s.protocol < 0)
				bad = 1;
		} else if (strcmp(k, "host") == 0) {
			copy_attr(s.host, sizeof(s.host), v);
		} else if (strcmp(k, "port") == 0) {
			s.port = atoi(v);
			if (s.port < 0 || s.port > 65535)
				s.port = 0;
		} else if (strcmp(k, "user") == 0) {
			copy_attr(s.user, sizeof(s.user), v);
		} else if (strcmp(k, "password") == 0) {
			copy_attr(s.password, sizeof(s.password), v);
		} else if (strcmp(k, "folder") == 0) {
			copy_attr(s.folder, sizeof(s.folder), v);
		} else if (strcmp(k, "keyfile") == 0) {
			copy_attr(s.keyfile, sizeof(s.keyfile), v);
		}
	}
	if (bad || !utf8_name_ok(s.name) || sites_find(ps->sites, s.name) >= 0) {
		ps->skipped++;
		return;
	}
	if (sites_add(ps->sites, &s) < 0)
		ps->failed = 1;
}

static void XMLCALL on_start(void *ud, const XML_Char *el, const XML_Char **attr)
{
	struct parse *ps = ud;

	ps->depth++;
	if (ps->depth == 1) {
		ps->root_ok = strcmp(el, "sftphandler") == 0;
		return;
	}
	if (ps->depth == 2 && ps->root_ok && strcmp(el, "site") == 0)
		site_element(ps, attr);
}

static void XMLCALL on_end(void *ud, const XML_Char *el)
{
	struct parse *ps = ud;

	(void)el;
	ps->depth--;
}

static char *read_file(const char *file, size_t *len, int *err)
{
	FILE *f = fopen(file, "rb");
	char *buf = NULL;
	size_t cap = 0, n = 0, got;

	if (f == NULL) {
		*err = -ENOENT;
		return NULL;
	}
	do {
		if (n + 4096 > cap) {
			char *nb = realloc(buf, cap + 16384);

			if (nb == NULL) {
				free(buf);
				fclose(f);
				*err = -ENOMEM;
				return NULL;
			}
			buf = nb;
			cap += 16384;
		}
		got = fread(buf + n, 1, cap - n, f);
		n += got;
	} while (got > 0);
	fclose(f);
	*len = n;
	*err = 0;
	return buf;
}

static int parse_file(const char *file, struct parse *ps, char *err, size_t errlen)
{
	size_t len = 0;
	int rc;
	char *buf = read_file(file, &len, &rc);

	if (buf == NULL)
		return rc;
	ps->xp = XML_ParserCreate(NULL);
	if (ps->xp == NULL) {
		free(buf);
		return -ENOMEM;
	}
	XML_SetUserData(ps->xp, ps);
	XML_SetElementHandler(ps->xp, on_start, on_end);
	rc = 0;
	if (XML_Parse(ps->xp, buf, (int)len, 1) == XML_STATUS_ERROR) {
		if (err != NULL)
			snprintf(err, errlen, "%s: line %lu: %s", file,
			         (unsigned long)XML_GetCurrentLineNumber(ps->xp),
			         XML_ErrorString(XML_GetErrorCode(ps->xp)));
		rc = -EINVAL;
	} else if (!ps->root_ok) {
		if (err != NULL)
			snprintf(err, errlen, "%s is not a sftp-handler file.", file);
		rc = -EINVAL;
	} else if (ps->failed) {
		rc = -ENOMEM;
	}
	XML_ParserFree(ps->xp);
	free(buf);
	return rc;
}

int sites_load(const char *file, site_list *l, char *err, size_t errlen, int *skipped)
{
	struct parse ps;
	int rc;

	memset(&ps, 0, sizeof(ps));
	ps.sites = l;
	l->count = 0;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	rc = parse_file(file, &ps, err, errlen);
	if (skipped != NULL)
		*skipped = ps.skipped;
	return rc;
}

/* --- writing ---------------------------------------------------------------- */

static void put_escaped(FILE *f, const char *s)
{
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '&':  fputs("&amp;", f); break;
		case '<':  fputs("&lt;", f); break;
		case '>':  fputs("&gt;", f); break;
		case '"':  fputs("&quot;", f); break;
		case '\t': fputs("&#9;", f); break;
		case '\n': fputs("&#10;", f); break;
		case '\r': fputs("&#13;", f); break;
		default:
			if ((unsigned char)*s >= 32)
				fputc(*s, f);
			break;
		}
	}
}

static void put_attr(FILE *f, const char *name, const char *val)
{
	fprintf(f, " %s=\"", name);
	put_escaped(f, val);
	fputc('"', f);
}

/* writes to file.new, then replaces the file (AmigaDOS cannot rename over
 * an existing file) */
static FILE *open_new(const char *file, char *tmp, size_t tmplen)
{
	snprintf(tmp, tmplen, "%s.new", file);
	return fopen(tmp, "wb");
}

static int replace_file(FILE *f, const char *tmp, const char *file)
{
	int bad = ferror(f);

	if (fclose(f) != 0 || bad) {
		remove(tmp);
		return -EIO;
	}
	remove(file);
	if (rename(tmp, file) != 0) {
		int e = errno;
		remove(tmp);
		return -(e != 0 ? e : EIO);
	}
	return 0;
}

int sites_save(const char *file, const site_list *l)
{
	char tmp[600];
	FILE *f = open_new(file, tmp, sizeof(tmp));
	int i;

	if (f == NULL)
		return -(errno != 0 ? errno : EIO);
	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sftphandler version=\"1\">\n", f);
	for (i = 0; i < l->count; i++) {
		const site_cfg *s = &l->site[i];
		char port[16];

		fputs("  <site", f);
		put_attr(f, "name", s->name);
		put_attr(f, "protocol", proto_name(s->protocol));
		put_attr(f, "host", s->host);
		snprintf(port, sizeof(port), "%d", site_port(s));
		put_attr(f, "port", port);
		put_attr(f, "user", s->user);
		put_attr(f, "password", s->password);
		put_attr(f, "folder", s->folder);
		if (s->protocol == PROTO_SFTP)
			put_attr(f, "keyfile", s->keyfile);
		fputs("/>\n", f);
	}
	fputs("</sftphandler>\n", f);
	return replace_file(f, tmp, file);
}
