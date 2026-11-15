/* sftp-handler - TCP connections with timeouts. Public domain. */
#include "net.h"
#include "platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int wait_fd(int s, int for_write, int timeout_ms)
{
	fd_set fds;
	struct timeval tv;
	int n;

	for (;;) {
		FD_ZERO(&fds);
		FD_SET(s, &fds);
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		n = select(s + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
		if (n > 0)
			return 0;
		if (n == 0)
			return -ETIMEDOUT;
		if (errno != EINTR)
			return -errno;
	}
}

static int set_nonblock(int s, int on)
{
	int fl = fcntl(s, F_GETFL, 0);

	if (fl < 0)
		return -1;
	return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

void net_addr_str(const uint8_t addr[4], char *out, size_t outlen)
{
	snprintf(out, outlen, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
}

int net_connect_addr(const uint8_t addr[4], int port, int timeout_ms)
{
	struct sockaddr_in sa;
	int s, err, one = 1;
	socklen_t elen = sizeof(err);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	memcpy(&sa.sin_addr, addr, 4);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -errno;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	if (set_nonblock(s, 1) != 0) {
		/* no non-blocking connect: plain connect */
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
			err = -errno;
			close(s);
			return err;
		}
		return s;
	}
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
			err = -errno;
			close(s);
			return err;
		}
		err = wait_fd(s, 1, timeout_ms);
		if (err != 0) {
			close(s);
			return err;
		}
		err = 0;
		if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
			err = errno;
		if (err != 0) {
			close(s);
			return -err;
		}
	}
	set_nonblock(s, 0);
	return s;
}

int net_connect(const char *host, int port, int timeout_ms, uint8_t addr_out[4],
                char *err, size_t errlen)
{
	struct in_addr in;
	uint8_t addr[4];
	int s;

	if (inet_aton(host, &in) == 0) {
		struct hostent *he = gethostbyname(host);

		if (he == NULL || he->h_addrtype != AF_INET || he->h_addr_list[0] == NULL) {
			if (err != NULL)
				snprintf(err, errlen, "The server name %s was not found.", host);
			return -EHOSTUNREACH;
		}
		memcpy(addr, he->h_addr_list[0], 4);
	} else {
		memcpy(addr, &in, 4);
	}
	s = net_connect_addr(addr, port, timeout_ms);
	if (s < 0) {
		if (err != NULL) {
			char a[20];

			net_addr_str(addr, a, sizeof(a));
			if (s == -ETIMEDOUT)
				snprintf(err, errlen, "No answer from %s (%s) port %d.", host, a, port);
			else if (s == -ECONNREFUSED)
				snprintf(err, errlen, "%s (%s) refused the connection on port %d.", host, a, port);
			else
				snprintf(err, errlen, "Cannot connect to %s (%s) port %d: %s.", host, a, port,
				         strerror(-s));
		}
		return s;
	}
	if (addr_out != NULL)
		memcpy(addr_out, addr, 4);
	return s;
}

int net_send(int s, const void *buf, size_t len, int timeout_ms)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n;
		int err = wait_fd(s, 1, timeout_ms);

		if (err != 0)
			return err;
		n = send(s, p, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return errno == ECONNRESET ? -EPIPE : -errno;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int net_recv(int s, void *buf, size_t len, int timeout_ms)
{
	for (;;) {
		ssize_t n;
		int err = wait_fd(s, 0, timeout_ms);

		if (err != 0)
			return err;
		n = recv(s, buf, len, 0);
		if (n >= 0)
			return (int)n;
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			return -errno;
	}
}

void net_shutdown_send(int s)
{
	shutdown(s, SHUT_WR);
}

void net_close(int s)
{
	if (s >= 0)
		close(s);
}
