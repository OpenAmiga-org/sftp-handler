/* sftp-handler - TCP connections with timeouts (BSD sockets: Linux and
 * clib4 on AmigaOS 4). IPv4. Public domain. */
#ifndef SFH_NET_H
#define SFH_NET_H

#include <stddef.h>
#include <stdint.h>

/* Connects to host:port. Returns the socket, or -errno; on failure a short
 * explanation goes to err (may be NULL). The address used is written to
 * addr (4 bytes, network order) when addr is not NULL. */
int  net_connect(const char *host, int port, int timeout_ms, uint8_t addr[4],
                 char *err, size_t errlen);
/* Same, to a numeric address. */
int  net_connect_addr(const uint8_t addr[4], int port, int timeout_ms);
/* Sends everything. 0 or -errno (-ETIMEDOUT, -EPIPE ...). */
int  net_send(int s, const void *buf, size_t len, int timeout_ms);
/* Receives up to len bytes. Returns the count, 0 at the end of the stream,
 * or -errno. */
int  net_recv(int s, void *buf, size_t len, int timeout_ms);
/* Half-closes the sending side (the server sees the end of an upload). */
void net_shutdown_send(int s);
void net_close(int s);
/* "a.b.c.d" */
void net_addr_str(const uint8_t addr[4], char *out, size_t outlen);

#endif
