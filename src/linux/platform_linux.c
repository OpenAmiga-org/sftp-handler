/* sftp-handler - platform functions for the Linux test build.
 * Public domain. */
#define _GNU_SOURCE
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int plat_loglevel = LOG_ERR;

struct tmpf {
	int  fd;
	char path[512];
};

tmpf *tmpf_create(const char *dir, int *err)
{
	tmpf *f = calloc(1, sizeof(*f));

	if (f == NULL) {
		*err = -ENOMEM;
		return NULL;
	}
	snprintf(f->path, sizeof(f->path), "%s/sftph-XXXXXX", dir != NULL ? dir : "/tmp");
	f->fd = mkstemp(f->path);
	if (f->fd < 0) {
		*err = -errno;
		free(f);
		return NULL;
	}
	return f;
}

int tmpf_pread(tmpf *f, void *buf, size_t len, int64_t off)
{
	ssize_t n = pread(f->fd, buf, len, (off_t)off);

	return n < 0 ? -errno : (int)n;
}

int tmpf_pwrite(tmpf *f, const void *buf, size_t len, int64_t off)
{
	ssize_t n = pwrite(f->fd, buf, len, (off_t)off);

	return n < 0 ? -errno : (int)n;
}

int tmpf_truncate(tmpf *f, int64_t size)
{
	return ftruncate(f->fd, (off_t)size) == 0 ? 0 : -errno;
}

int64_t tmpf_size(tmpf *f)
{
	struct stat st;

	if (fstat(f->fd, &st) != 0)
		return -errno;
	return (int64_t)st.st_size;
}

int tmpf_keep(tmpf *f, const char *dir, const char *name, char *out, size_t outlen)
{
	int i;

	mkdir(dir, 0700);
	/* never replace an earlier kept file */
	snprintf(out, outlen, "%s/%s", dir, name);
	for (i = 1; access(out, F_OK) == 0 && i < 1000; i++)
		snprintf(out, outlen, "%s/%s.%d", dir, name, i);
	if (rename(f->path, out) != 0)
		return -errno;
	close(f->fd);
	free(f);
	return 0;
}

void tmpf_close(tmpf *f)
{
	close(f->fd);
	unlink(f->path);
	free(f);
}

time_t plat_time(void)
{
	return time(NULL);
}

int64_t plat_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void plat_log(int level, const char *fmt, ...)
{
	va_list ap;

	if (level > plat_loglevel)
		return;
	va_start(ap, fmt);
	fputs("[sftph] ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void plat_report(const char *title, const char *message)
{
	fprintf(stderr, "[sftph] *** %s ***\n%s\n", title, message);
}

int plat_ask_password(const char *site, const char *user, const char *host,
                      const char *keyfile, const char *problem,
                      char *buf, size_t buflen, int *remember)
{
	const char *pw = getenv("SFH_PASSWORD");
	const char *rem = getenv("SFH_REMEMBER");

	fprintf(stderr, "[sftph] password requester: %s (%s@%s)%s%s%s%s\n", site, user, host,
	        keyfile != NULL ? " key " : "", keyfile != NULL ? keyfile : "",
	        problem != NULL ? " - " : "", problem != NULL ? problem : "");
	*remember = rem != NULL && rem[0] == '1';
	if (pw == NULL)
		return 0;
	snprintf(buf, buflen, "%s", pw);
	return 1;
}

/* SFH_ANSWER_CMD: a command that prints the answer (the tests use
 * tests/totp.py); it gets the question in SFH_PROMPT. Unset or failing:
 * Cancel. */
int plat_ask_answer(const char *site, const char *user, const char *host,
                    const char *name, const char *instruction, const char *prompt,
                    int echo, const char *problem, char *buf, size_t buflen)
{
	const char *cmd = getenv("SFH_ANSWER_CMD");
	FILE *p;
	size_t n;
	int ok = 0;

	fprintf(stderr, "[sftph] question requester: %s (%s@%s) [%s] [%s] \"%s\" echo=%d%s%s\n",
	        site, user, host, name, instruction, prompt, echo,
	        problem != NULL ? " - " : "", problem != NULL ? problem : "");
	buf[0] = '\0';
	if (cmd == NULL)
		return 0;
	setenv("SFH_PROMPT", prompt, 1);
	p = popen(cmd, "r");
	if (p == NULL)
		return 0;
	if (fgets(buf, (int)buflen, p) != NULL) {
		n = strlen(buf);
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
	}
	ok = pclose(p) == 0;
	if (!ok)
		buf[0] = '\0';
	return ok;
}

int plat_ask_hostkey(const char *site, const char *host, int port,
                     const char *keytype, const char *fingerprint, int mismatch)
{
	const char *a = getenv("SFH_HOSTKEY");

	fprintf(stderr, "[sftph] host key requester: %s %s:%d %s %s%s\n", site, host, port,
	        keytype, fingerprint, mismatch ? " (CHANGED)" : "");
	if (a == NULL)
		return 0;
	if (strcmp(a, "save") == 0)
		return 2;
	if (strcmp(a, "once") == 0)
		return 1;
	return 0;
}
