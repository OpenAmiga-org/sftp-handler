/* sftp-handler - FTP directory listing lines (MLSD facts, Unix "ls -l"
 * and DOS/IIS style). Public domain. */
#ifndef SFH_FTPLIST_H
#define SFH_FTPLIST_H

#include <stddef.h>
#include <time.h>

#include "remote.h"

enum { FL_SKIP = 0, FL_ENTRY = 1, FL_LINK = 2 };

/* Parses one line (without CR/LF). Returns FL_SKIP for lines that are not
 * entries (totals, ".", ".."), FL_ENTRY, or FL_LINK for a symbolic link
 * whose kind is not known (st->is_dir is then a guess). The name goes to
 * name. now: the current time, to complete "Nov 15 10:25" dates. */
int ftp_parse_mlsd(const char *line, char *name, size_t namelen, rstat *st);
int ftp_parse_list(const char *line, time_t now, char *name, size_t namelen, rstat *st);

/* "20261115102500[.123]" -> UTC seconds; 0 when not valid */
time_t ftp_parse_time14(const char *s);

#endif
