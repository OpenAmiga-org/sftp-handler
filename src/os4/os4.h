/* sftp-handler - AmigaOS 4 parts. Public domain. */
#ifndef SFH_OS4_H
#define SFH_OS4_H

#include <stddef.h>

/* requesters.c (texts in the local character set) */
int  req_open_libs(void);
void req_close_libs(void);
/* Information requester; text may contain any characters. */
void req_message(const char *title, const char *text);
/* Asks for a password. Returns 1 for OK, 0 for Cancel. */
int  req_password(const char *title, const char *text, char *buf, size_t buflen,
                  int *remember);
/* Returns the number of the button pressed: buttons "A|B|C" give 1, 2, 0. */
int  req_choice(const char *title, const char *text, const char *buttons);

/* amissl.c: 0, -1 (no bsdsocket.library) or -2 (no usable AmiSSL) */
int  amissl_open(void);
void amissl_close(void);

/* platform_os4.c */
extern int plat_charset;   /* messages are converted to it, -1 = no */

#endif
