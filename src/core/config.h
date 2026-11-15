/* sftp-handler - the site list (shared by the handler and sftphprefs).
 *
 * ENV:SFTPHandler/sites.xml (saved copy in ENVARC:):
 *
 *   <?xml version="1.0" encoding="UTF-8"?>
 *   <sftphandler version="1">
 *     <site name="My server" protocol="sftp" host="example.com" port="22"
 *           user="me" password="secret" folder="/home/me" keyfile=""/>
 *   </sftphandler>
 *
 * An empty password means: ask when connecting. All strings are UTF-8.
 * Public domain. */
#ifndef SFH_CONFIG_H
#define SFH_CONFIG_H

#include <stddef.h>

#define CFG_DIR        "SFTPHandler"
#define CFG_SITES      "sites.xml"
#define CFG_KNOWNHOSTS "known_hosts"

enum { PROTO_FTP = 0, PROTO_SFTP = 1, PROTO_COUNT };

/* Longest site name in bytes in the local character set: the FFS limit
 * for names, so that a site can always be copied to a normal disk. */
#define SITE_NAME_MAX 107

typedef struct {
	char name[SITE_NAME_MAX * 3 + 1];  /* UTF-8; a valid AmigaDOS name */
	int  protocol;
	char host[256];
	int  port;                          /* 0 = the protocol's default */
	char user[128];                     /* empty: anonymous FTP */
	char password[256];                 /* empty: asked for when needed */
	char folder[512];                   /* start folder, empty = login folder */
	char keyfile[256];                  /* SFTP: private key (Amiga path) */
} site_cfg;

typedef struct {
	site_cfg *site;
	int       count;
	int       cap;
} site_list;

const char *proto_name(int protocol);         /* "ftp", "sftp" */
int         proto_from_name(const char *s);   /* -1 when unknown */
int         proto_default_port(int protocol);
int         site_port(const site_cfg *s);     /* port or the default */

void sites_init(site_list *l);
void sites_free(site_list *l);
/* Adds a copy; returns its index or -1 (out of memory). */
int  sites_add(site_list *l, const site_cfg *s);
void sites_remove(site_list *l, int index);
/* Index of the site with that name (case-insensitive), or -1. */
int  sites_find(const site_list *l, const char *name);
int  sites_equal(const site_list *a, const site_list *b);
int  site_cfg_equal(const site_cfg *a, const site_cfg *b);

/* Loads a site file. Returns 0, -ENOENT when there is none, -EINVAL for a
 * broken file (message in err), -ENOMEM. Invalid or duplicate sites are
 * skipped; their number goes to *skipped when not NULL. */
int  sites_load(const char *file, site_list *l, char *err, size_t errlen, int *skipped);
/* Writes the file (through a temp file, then renamed). 0 or -errno. */
int  sites_save(const char *file, const site_list *l);

/* Checks a site name given in the local character set (8-bit). Returns
 * NULL when it is fine, else why not (English text). */
const char *site_name_problem(const char *local_name);

#endif
