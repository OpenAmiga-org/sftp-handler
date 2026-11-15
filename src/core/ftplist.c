/* sftp-handler - FTP directory listing lines. Public domain. */
#include "ftplist.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static time_t mk_utc(int y, int mo, int d, int h, int mi, int s)
{
	if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
	    mi < 0 || mi > 59 || s < 0 || s > 60)
		return 0;
	return (time_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 +
	                h * 3600 + mi * 60 + s);
}

time_t ftp_parse_time14(const char *s)
{
	int y, mo, d, h, mi, sec, i;

	for (i = 0; i < 14; i++)
		if (!isdigit((unsigned char)s[i]))
			return 0;
	if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &sec) != 6)
		return 0;
	return mk_utc(y, mo, d, h, mi, sec);
}

static void set_name(char *name, size_t namelen, const char *s, size_t n)
{
	if (n >= namelen)
		n = namelen - 1;
	memcpy(name, s, n);
	name[n] = '\0';
}

static int skip_name(const char *name)
{
	return name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* --- MLSD: "type=file;size=6;modify=20261115102500;UNIX.mode=0644; name" */

int ftp_parse_mlsd(const char *line, char *name, size_t namelen, rstat *st)
{
	const char *p = line, *sp = strchr(line, ' ');
	int kind = FL_ENTRY, have_type = 0;

	memset(st, 0, sizeof(*st));
	st->mode = -1;
	if (sp == NULL)
		return FL_SKIP;
	while (p < sp) {
		const char *semi = memchr(p, ';', (size_t)(sp - p));
		const char *eq = memchr(p, '=', (size_t)((semi != NULL ? semi : sp) - p));
		const char *end = semi != NULL ? semi : sp;

		if (eq != NULL) {
			size_t klen = (size_t)(eq - p);
			char val[64];

			set_name(val, sizeof(val), eq + 1, (size_t)(end - eq - 1));
			if (klen == 4 && strncasecmp(p, "type", 4) == 0) {
				have_type = 1;
				if (strcasecmp(val, "cdir") == 0 || strcasecmp(val, "pdir") == 0)
					return FL_SKIP;
				if (strcasecmp(val, "dir") == 0)
					st->is_dir = 1;
				else if (strcasecmp(val, "file") != 0)
					kind = FL_LINK; /* OS.unix=symlink, OS.unix=slink:... */
			} else if (klen == 4 && strncasecmp(p, "size", 4) == 0) {
				st->size = strtoll(val, NULL, 10);
			} else if (klen == 6 && strncasecmp(p, "modify", 6) == 0) {
				st->mtime = ftp_parse_time14(val);
			} else if (klen == 9 && strncasecmp(p, "UNIX.mode", 9) == 0) {
				st->mode = (int)(strtol(val, NULL, 8) & 07777);
			}
		}
		if (semi == NULL)
			break;
		p = semi + 1;
	}
	if (!have_type)
		return FL_SKIP;
	set_name(name, namelen, sp + 1, strlen(sp + 1));
	if (skip_name(name))
		return FL_SKIP;
	return kind;
}

/* --- LIST ------------------------------------------------------------------ */

static int month_index(const char *s, size_t n)
{
	static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
	int i;

	if (n != 3)
		return -1;
	for (i = 0; i < 12; i++)
		if (strncasecmp(months + i * 3, s, 3) == 0)
			return i;
	return -1;
}

static int all_digits(const char *s, size_t n)
{
	size_t i;

	if (n == 0)
		return 0;
	for (i = 0; i < n; i++)
		if (!isdigit((unsigned char)s[i]))
			return 0;
	return 1;
}

static int perm_bits(const char *p)
{
	static const int bits[9] = { 0400, 0200, 0100, 040, 020, 010, 04, 02, 01 };
	int i, mode = 0;

	for (i = 0; i < 9; i++) {
		char c = p[1 + i];
		if (c != '-' && c != 'S' && c != 'T')
			mode |= bits[i];
	}
	return mode;
}

#define MAXTOK 16

struct tok {
	const char *s;
	size_t      n;
};

static int tokenize(const char *line, struct tok *t, int max)
{
	const char *p = line;
	int n = 0;

	while (*p != '\0' && n < max) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;
		t[n].s = p;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
		t[n].n = (size_t)(p - t[n].s);
		n++;
	}
	return n;
}

/* "Nov 15 10:25" or "Nov 15  2024" or "2026-11-15 10:25" starting at token i;
 * returns the index of the last date token, or -1 */
static int parse_unix_date(const struct tok *t, int nt, int i, time_t now, time_t *out)
{
	int mon = month_index(t[i].s, t[i].n);

	if (mon >= 0 && i + 2 < nt && all_digits(t[i + 1].s, t[i + 1].n) && t[i + 1].n <= 2) {
		int day = atoi(t[i + 1].s), h = 0, mi = 0, y;
		const struct tok *yt = &t[i + 2];

		if (yt->n == 4 && all_digits(yt->s, 4)) {
			y = atoi(yt->s);
			*out = mk_utc(y, mon + 1, day, 0, 0, 0);
			return i + 2;
		}
		if ((yt->n == 5 || yt->n == 8) && yt->s[2] == ':' && sscanf(yt->s, "%d:%d", &h, &mi) == 2) {
			time_t t1;
			int64_t days = (int64_t)now / 86400;
			/* the year of now (UTC) */
			int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
			unsigned doe = (unsigned)(z - era * 146097);
			unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
			unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
			unsigned mp = (5 * doy + 2) / 153;
			unsigned m = mp + (mp < 10 ? 3 : -9);

			y = (int)(yoe + era * 400) + (m <= 2);
			t1 = mk_utc(y, mon + 1, day, h, mi, 0);
			/* "ls" shows the time for dates in the last six months:
			 * a date more than a day ahead belongs to last year */
			if (t1 > now + 86400)
				t1 = mk_utc(y - 1, mon + 1, day, h, mi, 0);
			*out = t1;
			return i + 2;
		}
		return -1;
	}
	/* ISO: 2026-11-15 10:25 */
	if (t[i].n == 10 && t[i].s[4] == '-' && t[i].s[7] == '-' && i + 1 < nt) {
		int y, mo, d, h, mi;

		if (sscanf(t[i].s, "%4d-%2d-%2d", &y, &mo, &d) == 3 &&
		    sscanf(t[i + 1].s, "%d:%d", &h, &mi) == 2) {
			*out = mk_utc(y, mo, d, h, mi, 0);
			return i + 1;
		}
	}
	return -1;
}

static int parse_unix(const char *line, time_t now, char *name, size_t namelen, rstat *st)
{
	struct tok t[MAXTOK];
	int nt = tokenize(line, t, MAXTOK), i, last = -1;
	char type = line[0];
	const char *np;
	int kind = FL_ENTRY;

	if (nt < 4 || t[0].n < 10)
		return FL_SKIP;
	/* the date follows the size; the size is the first number before it */
	for (i = 2; i < nt - 1; i++) {
		time_t when = 0;

		if (all_digits(t[i - 1].s, t[i - 1].n)) {
			last = parse_unix_date(t, nt, i, now, &when);
			if (last >= 0) {
				st->size = strtoll(t[i - 1].s, NULL, 10);
				st->mtime = when;
				break;
			}
		}
	}
	if (last < 0 || last + 1 > nt)
		return FL_SKIP;
	/* the name is everything after the last date token and one space */
	np = t[last].s + t[last].n;
	if (*np == ' ' || *np == '\t')
		np++;
	if (*np == '\0')
		return FL_SKIP;
	st->mode = perm_bits(t[0].s);
	if (type == 'd') {
		st->is_dir = 1;
	} else if (type == 'l') {
		const char *arrow = strstr(np, " -> ");

		kind = FL_LINK;
		if (arrow != NULL) {
			set_name(name, namelen, np, (size_t)(arrow - np));
			return skip_name(name) ? FL_SKIP : kind;
		}
	} else if (type != '-' && type != 'f') {
		/* devices, pipes, sockets: shown as empty files */
		st->size = 0;
	}
	set_name(name, namelen, np, strlen(np));
	return skip_name(name) ? FL_SKIP : kind;
}

/* "11-15-26  10:25AM       <DIR>          name" (IIS, DOS style) */
static int parse_dos(const char *line, char *name, size_t namelen, rstat *st)
{
	int mo, d, y, h, mi, n = 0;
	const char *p;

	if (sscanf(line, "%d-%d-%d %d:%d%n", &mo, &d, &y, &h, &mi, &n) != 5 || n == 0)
		return FL_SKIP;
	p = line + n;
	/* 12-hour times end in AM or PM */
	if ((p[0] == 'A' || p[0] == 'a' || p[0] == 'P' || p[0] == 'p') && (p[1] == 'M' || p[1] == 'm')) {
		if (p[0] == 'P' || p[0] == 'p') {
			if (h < 12)
				h += 12;
		} else if (h == 12) {
			h = 0;
		}
		p += 2;
	}
	if (y < 70)
		y += 2000;
	else if (y < 100)
		y += 1900;
	st->mtime = mk_utc(y, mo, d, h, mi, 0);
	while (*p == ' ')
		p++;
	if (strncasecmp(p, "<DIR>", 5) == 0) {
		st->is_dir = 1;
		p += 5;
	} else {
		char *end;

		if (!isdigit((unsigned char)*p))
			return FL_SKIP;
		st->size = strtoll(p, &end, 10);
		p = end;
	}
	while (*p == ' ')
		p++;
	set_name(name, namelen, p, strlen(p));
	return skip_name(name) ? FL_SKIP : FL_ENTRY;
}

int ftp_parse_list(const char *line, time_t now, char *name, size_t namelen, rstat *st)
{
	memset(st, 0, sizeof(*st));
	st->mode = -1;
	if (isdigit((unsigned char)line[0]))
		return parse_dos(line, name, namelen, st);
	if (strchr("-dlfbcps", line[0]) != NULL && strlen(line) > 10)
		return parse_unix(line, now, name, namelen, st);
	return FL_SKIP;
}
