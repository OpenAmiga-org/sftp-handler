/* tcpsend - sends N MB over plain TCP and reports the speed (guest network
 * test without the handler):  tcpsend HOST PORT MB [SNDBUF]
 * Public domain. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_in sa;
	struct timespec t0, t1;
	static char buf[65536];
	long mb, i;
	int s, snd = 0;
	socklen_t l = sizeof(snd);
	double dt;

	if (argc < 4) {
		printf("usage: tcpsend HOST PORT MB [SNDBUF]\n");
		return 10;
	}
	mb = atol(argv[3]);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(atoi(argv[2]));
	inet_aton(argv[1], &sa.sin_addr);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (argc > 4) {
		snd = atoi(argv[4]);
		setsockopt(s, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
	}
	getsockopt(s, SOL_SOCKET, SO_SNDBUF, &snd, &l);
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		printf("connect failed\n");
		return 20;
	}
	memset(buf, 'x', sizeof(buf));
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < mb * 16; i++) {
		size_t off = 0;
		while (off < sizeof(buf)) {
			ssize_t n = send(s, buf + off, sizeof(buf) - off, 0);
			if (n <= 0) {
				printf("send failed\n");
				return 20;
			}
			off += (size_t)n;
		}
	}
	close(s);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("sent %ld MB in %.2f s = %.2f MB/s (SO_SNDBUF %d)\n", mb, dt, mb / dt, snd);
	return 0;
}
