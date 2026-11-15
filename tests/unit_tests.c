/* sftp-handler - unit tests: FTP listing lines, the site file, names.
 * Public domain. */
#include "config.h"
#include "ftplist.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static time_t utc(int y, int mo, int d, int h, int mi, int s)
{
	return (time_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s);
}

static void test_list(void)
{
	time_t now = utc(2026, 11, 15, 12, 0, 0);
	char name[256];
	rstat st;
	int k;

	k = ftp_parse_list("-rw-rw-r--    1 1000     1000            6 Nov 15 10:25 hello.txt", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, "hello.txt") == 0 && st.size == 6 && !st.is_dir);
	CHECK(st.mtime == utc(2026, 11, 15, 10, 25, 0) && st.mode == 0664);

	k = ftp_parse_list("drwxr-xr-x    2 1000     1000         4096 Dec 24  2025 old dir", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, "old dir") == 0 && st.is_dir && st.mtime == utc(2025, 12, 24, 0, 0, 0));

	/* a recent-looking date that would be in the future: last year */
	k = ftp_parse_list("-rw-r--r--   1 ftp ftp  100 Dec 31 23:59 future", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && st.mtime == utc(2025, 12, 31, 23, 59, 0));

	/* leading and double spaces in names are kept */
	k = ftp_parse_list("-rw-r--r--   1 ftp ftp  100 Nov  1 09:00  two  spaces", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, " two  spaces") == 0);

	/* no group column */
	k = ftp_parse_list("-rw-r--r--   1 owner  12345 Jan  5 2024 nogroup.lha", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, "nogroup.lha") == 0 && st.size == 12345);

	/* a number as the name and as the owner */
	k = ftp_parse_list("-rw-r--r--   1 500 500  42 Mar 3 11:11 2024", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, "2024") == 0 && st.size == 42);

	k = ftp_parse_list("lrwxrwxrwx    1 0  0   11 Nov 15 10:00 link -> target dir", now, name, sizeof(name), &st);
	CHECK(k == FL_LINK && strcmp(name, "link") == 0);

	/* ISO dates (ls --time-style=long-iso) */
	k = ftp_parse_list("-rw-r--r-- 1 me me 77 2026-01-02 03:04 iso.txt", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, "iso.txt") == 0 && st.mtime == utc(2026, 1, 2, 3, 4, 0));

	/* sizes above 4 GB */
	k = ftp_parse_list("-rw-r--r--   1 ftp ftp 5000000000 Nov 15 10:25 big.iso", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && st.size == 5000000000LL);

	CHECK(ftp_parse_list("total 12", now, name, sizeof(name), &st) == FL_SKIP);
	CHECK(ftp_parse_list("drwxr-xr-x    2 0 0 4096 Nov 15 10:25 .", now, name, sizeof(name), &st) == FL_SKIP);
	CHECK(ftp_parse_list("drwxr-xr-x    2 0 0 4096 Nov 15 10:25 ..", now, name, sizeof(name), &st) == FL_SKIP);

	/* DOS / IIS */
	k = ftp_parse_list("11-15-26  10:25AM       <DIR>          Program Files", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && st.is_dir && strcmp(name, "Program Files") == 0 && st.mtime == utc(2026, 11, 15, 10, 25, 0));
	k = ftp_parse_list("01-02-2025  12:05PM              1234 readme.txt", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && !st.is_dir && st.size == 1234 && st.mtime == utc(2025, 1, 2, 12, 5, 0));
	k = ftp_parse_list("01-02-25  12:05AM              1 midnight", now, name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && st.mtime == utc(2025, 1, 2, 0, 5, 0));

	/* MLSD */
	k = ftp_parse_mlsd("type=file;size=6;modify=20261115102500;UNIX.mode=0644; hello world.txt", name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && strcmp(name, "hello world.txt") == 0 && st.size == 6 && st.mode == 0644);
	CHECK(st.mtime == utc(2026, 11, 15, 10, 25, 0));
	k = ftp_parse_mlsd("Type=dir;Modify=20250101000000.123; Sub", name, sizeof(name), &st);
	CHECK(k == FL_ENTRY && st.is_dir && strcmp(name, "Sub") == 0);
	CHECK(ftp_parse_mlsd("type=cdir; /home", name, sizeof(name), &st) == FL_SKIP);
	CHECK(ftp_parse_mlsd("type=pdir; ..", name, sizeof(name), &st) == FL_SKIP);
	k = ftp_parse_mlsd("type=OS.unix=symlink;size=4; ln", name, sizeof(name), &st);
	CHECK(k == FL_LINK && strcmp(name, "ln") == 0);
	CHECK(ftp_parse_time14("20261115102500") == utc(2026, 11, 15, 10, 25, 0));
	CHECK(ftp_parse_time14("2026111510250") == 0);
}

static void test_config(void)
{
	char file[] = "/tmp/sftph-unit-XXXXXX";
	char err[300];
	site_list l, m;
	site_cfg s;
	int fd = mkstemp(file), skipped = -1;
	FILE *f = fdopen(fd, "w");

	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sftphandler version=\"1\">\n"
	      "<site name=\"One\" protocol=\"ftp\" host=\"a.example\" port=\"2121\" user=\"me\" password=\"p&amp;&quot;w\"/>\n"
	      "<site name=\"one\" protocol=\"sftp\" host=\"dupe\"/>\n"
	      "<site name=\"Bad:name\" protocol=\"ftp\" host=\"x\"/>\n"
	      "<site name=\"x.info\" protocol=\"ftp\" host=\"x\"/>\n"
	      "<site name=\"Two \xc3\xa5\" protocol=\"SFTP\" host=\"b\" keyfile=\"ENVARC:k\" folder=\"/srv\"/>\n"
	      "<site name=\"Three\" protocol=\"gopher\" host=\"c\"/>\n"
	      "</sftphandler>\n", f);
	fclose(f);
	sites_init(&l);
	CHECK(sites_load(file, &l, err, sizeof(err), &skipped) == 0);
	CHECK(l.count == 2 && skipped == 4);
	CHECK(strcmp(l.site[0].name, "One") == 0 && l.site[0].protocol == PROTO_FTP && l.site[0].port == 2121);
	CHECK(strcmp(l.site[0].password, "p&\"w") == 0);
	CHECK(strcmp(l.site[1].name, "Two \xc3\xa5") == 0 && l.site[1].protocol == PROTO_SFTP);
	CHECK(site_port(&l.site[1]) == 22 && strcmp(l.site[1].keyfile, "ENVARC:k") == 0);
	CHECK(sites_find(&l, "TWO \xc3\x85") == 1);

	/* written and read back */
	memset(&s, 0, sizeof(s));
	snprintf(s.name, sizeof(s.name), "Tab\tand <xml>");
	snprintf(s.host, sizeof(s.host), "h");
	snprintf(s.password, sizeof(s.password), "pw with \"quotes\" & <tags>\n");
	sites_add(&l, &s);
	CHECK(sites_save(file, &l) == 0);
	sites_init(&m);
	CHECK(sites_load(file, &m, err, sizeof(err), &skipped) == 0);
	/* the tab is a control character: skipped on reading */
	CHECK(m.count == 2 && skipped == 1);
	sites_remove(&l, 2);
	CHECK(sites_equal(&l, &m));
	sites_free(&m);

	/* broken file */
	f = fopen(file, "w");
	fputs("<sftphandler><site name=\"a\"></sftphandler>", f);
	fclose(f);
	CHECK(sites_load(file, &m, err, sizeof(err), NULL) == -22 && strstr(err, "line 1") != NULL);
	f = fopen(file, "w");
	fputs("<other/>", f);
	fclose(f);
	CHECK(sites_load(file, &m, err, sizeof(err), NULL) == -22);
	unlink(file);
	CHECK(sites_load(file, &m, err, sizeof(err), NULL) == -2);
	sites_free(&m);
	sites_free(&l);

	CHECK(site_name_problem("Good name") == NULL);
	CHECK(site_name_problem("\xe5\xe4\xf6") == NULL);   /* Latin-1 */
	CHECK(site_name_problem("") != NULL);
	CHECK(site_name_problem("a:b") != NULL);
	CHECK(site_name_problem("a/b") != NULL);
	CHECK(site_name_problem(" lead") != NULL);
	CHECK(site_name_problem("Site.INFO") != NULL);
	CHECK(site_name_problem("ctrl\x01") != NULL);
	{
		char longname[200];

		memset(longname, 'a', sizeof(longname) - 1);
		longname[sizeof(longname) - 1] = '\0';
		CHECK(site_name_problem(longname) != NULL);
		longname[107] = '\0';
		CHECK(site_name_problem(longname) == NULL);
	}
}

int main(void)
{
	test_list();
	test_config();
	printf("%s (%d failed)\n", fails ? "FAILED" : "unit tests passed", fails);
	return fails != 0;
}
