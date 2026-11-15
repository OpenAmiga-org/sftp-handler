/* nettest - feasibility test for sftp-handler on AmigaOS 4 (clib4).
 *
 *   nettest SFTP host port user password [dir]
 *   nettest FTP  host port user password [dir]
 *
 * SFTP: libssh2 + AmiSSL: handshake, host key, password login, directory
 * listing, read of hello.txt, write/rename/delete of a test file.
 * FTP: plain sockets: login, EPSV/PASV, MLSD/LIST, RETR, STOR, DELE.
 * Public domain. */
#include <proto/exec.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

struct Library           *AmiSSLMasterBase;
struct AmiSSLMasterIFace *IAmiSSLMaster;
struct Library           *AmiSSLBase;
struct AmiSSLIFace       *IAmiSSL;
static struct Library    *SockBase;
static struct Interface  *ISock;

static int amissl_open(void)
{
	SockBase = IExec->OpenLibrary("bsdsocket.library", 4);
	if (SockBase == NULL)
		return -1;
	ISock = IExec->GetInterface(SockBase, "main", 1, NULL);
	AmiSSLMasterBase = IExec->OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
	if (AmiSSLMasterBase == NULL)
		return -2;
	IAmiSSLMaster = (struct AmiSSLMasterIFace *)IExec->GetInterface(AmiSSLMasterBase, "main", 1, NULL);
	if (IAmiSSLMaster == NULL)
		return -2;
	if (IAmiSSLMaster->OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
	                                  AmiSSL_UsesOpenSSLStructs, FALSE,
	                                  AmiSSL_InitAmiSSL, TRUE,
	                                  AmiSSL_GetIAmiSSL, &IAmiSSL,
	                                  AmiSSL_ISocket, ISock,
	                                  AmiSSL_ErrNoPtr, &errno,
	                                  TAG_DONE) != 0)
		return -2;
	return 0;
}

static void amissl_close(void)
{
	if (IAmiSSL != NULL)
		IAmiSSLMaster->CloseAmiSSL();
	if (IAmiSSLMaster != NULL)
		IExec->DropInterface((struct Interface *)IAmiSSLMaster);
	if (AmiSSLMasterBase != NULL)
		IExec->CloseLibrary(AmiSSLMasterBase);
	if (ISock != NULL)
		IExec->DropInterface(ISock);
	if (SockBase != NULL)
		IExec->CloseLibrary(SockBase);
}

static int tcp_connect(const char *host, int port, struct sockaddr_in *peer)
{
	struct sockaddr_in sa;
	struct hostent *he;
	int s;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_aton(host, &sa.sin_addr) == 0) {
		he = gethostbyname(host);
		if (he == NULL) {
			printf("cannot look up %s\n", host);
			return -1;
		}
		memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		printf("socket: %s\n", strerror(errno));
		return -1;
	}
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		printf("connect %s:%d: %s\n", host, port, strerror(errno));
		close(s);
		return -1;
	}
	if (peer != NULL)
		*peer = sa;
	return s;
}

/* ------------------------------------------------------------------------ */

static int test_sftp(const char *host, int port, const char *user,
                     const char *pass, const char *dir)
{
	LIBSSH2_SESSION *ss;
	LIBSSH2_SFTP *sftp;
	LIBSSH2_SFTP_HANDLE *h;
	LIBSSH2_SFTP_ATTRIBUTES at;
	const char *fp;
	char name[512], path[1024], buf[4096];
	int s, rc, i, n;

	rc = amissl_open();
	if (rc != 0) {
		printf("AmiSSL: cannot open (%d)\n", rc);
		return 20;
	}
	if (libssh2_init(0) != 0) {
		printf("libssh2_init failed\n");
		return 20;
	}
	printf("libssh2 %s\n", libssh2_version(0));
	s = tcp_connect(host, port, NULL);
	if (s < 0)
		return 20;
	ss = libssh2_session_init();
	libssh2_session_set_blocking(ss, 1);
	libssh2_session_set_timeout(ss, 20000);
	rc = libssh2_session_handshake(ss, s);
	if (rc != 0) {
		char *msg;
		libssh2_session_last_error(ss, &msg, NULL, 0);
		printf("handshake failed: %d %s\n", rc, msg);
		return 20;
	}
	printf("kex %s, hostkey %s, cipher %s, mac %s\n",
	       libssh2_session_methods(ss, LIBSSH2_METHOD_KEX),
	       libssh2_session_methods(ss, LIBSSH2_METHOD_HOSTKEY),
	       libssh2_session_methods(ss, LIBSSH2_METHOD_CRYPT_CS),
	       libssh2_session_methods(ss, LIBSSH2_METHOD_MAC_CS));
	fp = libssh2_hostkey_hash(ss, LIBSSH2_HOSTKEY_HASH_SHA256);
	printf("host key SHA256:");
	for (i = 0; i < 32; i++)
		printf("%02x", (unsigned char)fp[i]);
	printf("\n");
	printf("auth methods: %s\n", libssh2_userauth_list(ss, user, strlen(user)));
	if (libssh2_userauth_password(ss, user, pass) != 0) {
		char *msg;
		libssh2_session_last_error(ss, &msg, NULL, 0);
		printf("login failed: %s\n", msg);
		return 20;
	}
	printf("logged in\n");
	sftp = libssh2_sftp_init(ss);
	if (sftp == NULL) {
		printf("sftp_init failed\n");
		return 20;
	}
	if (libssh2_sftp_realpath(sftp, ".", path, sizeof(path)) > 0)
		printf("home: %s\n", path);
	h = libssh2_sftp_opendir(sftp, dir);
	if (h == NULL) {
		printf("opendir %s failed: %lu\n", dir, libssh2_sftp_last_error(sftp));
		return 20;
	}
	while ((n = libssh2_sftp_readdir(h, name, sizeof(name), &at)) > 0)
		printf("  %-30s %10llu %s\n", name, (unsigned long long)at.filesize,
		       LIBSSH2_SFTP_S_ISDIR(at.permissions) ? "dir" : "");
	libssh2_sftp_closedir(h);

	snprintf(path, sizeof(path), "%s/hello.txt", dir);
	h = libssh2_sftp_open(sftp, path, LIBSSH2_FXF_READ, 0);
	if (h != NULL) {
		n = libssh2_sftp_read(h, buf, sizeof(buf) - 1);
		if (n >= 0) {
			buf[n] = '\0';
			printf("hello.txt: %s", buf);
		}
		libssh2_sftp_close(h);
	}
	snprintf(path, sizeof(path), "%s/amiga.txt", dir);
	h = libssh2_sftp_open(sftp, path, LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
	if (h == NULL) {
		printf("create failed\n");
		return 20;
	}
	n = snprintf(buf, sizeof(buf), "written by AmigaOS 4\n");
	printf("write: %d\n", (int)libssh2_sftp_write(h, buf, n));
	libssh2_sftp_seek64(h, 8);
	printf("write at 8: %d\n", (int)libssh2_sftp_write(h, "XX", 2));
	libssh2_sftp_close(h);
	snprintf(name, sizeof(name), "%s/amiga2.txt", dir);
	printf("rename: %d\n", libssh2_sftp_rename_ex(sftp, path, strlen(path), name, strlen(name),
	       LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE));
	if (libssh2_sftp_stat(sftp, name, &at) == 0)
		printf("stat: size %llu mtime %lu\n", (unsigned long long)at.filesize, at.mtime);
	snprintf(path, sizeof(path), "%s/sub", dir);
	printf("mkdir: %d rmdir: %d\n", libssh2_sftp_mkdir(sftp, path, 0755), libssh2_sftp_rmdir(sftp, path));
	libssh2_sftp_shutdown(sftp);
	libssh2_session_disconnect(ss, "bye");
	libssh2_session_free(ss);
	close(s);
	libssh2_exit();
	amissl_close();
	printf("SFTP OK\n");
	return 0;
}

/* ------------------------------------------------------------------------ */

static char ftpbuf[4096];
static int ftplen;

/* reads one reply (multi-line too); returns the code */
static int ftp_reply(int s, char *last, size_t lastlen)
{
	char line[1024];
	int code = 0, multi = 0;

	for (;;) {
		char *nl;
		size_t len;

		while ((nl = memchr(ftpbuf, '\n', ftplen)) == NULL) {
			int n = recv(s, ftpbuf + ftplen, sizeof(ftpbuf) - ftplen, 0);
			if (n <= 0)
				return -1;
			ftplen += n;
		}
		len = nl - ftpbuf + 1;
		memcpy(line, ftpbuf, len < sizeof(line) ? len : sizeof(line) - 1);
		line[len < sizeof(line) ? len : sizeof(line) - 1] = '\0';
		memmove(ftpbuf, ftpbuf + len, ftplen - len);
		ftplen -= len;
		printf("< %s", line);
		if (code == 0) {
			code = atoi(line);
			multi = line[3] == '-';
		} else if (atoi(line) == code && line[3] == ' ')
			multi = 0;
		if (!multi) {
			if (last != NULL)
				snprintf(last, lastlen, "%s", line);
			return code;
		}
	}
}

static int ftp_cmd(int s, char *last, size_t lastlen, const char *fmt, const char *arg)
{
	char cmd[1024];
	int n = snprintf(cmd, sizeof(cmd), fmt, arg);

	printf("> %.*s\n", strncmp(cmd, "PASS", 4) == 0 ? 4 : n - 2, cmd);
	if (send(s, cmd, n, 0) != n)
		return -1;
	return ftp_reply(s, last, lastlen);
}

static int ftp_data(int s, const struct sockaddr_in *peer)
{
	char last[1024];
	int port = -1, code;
	char *p;

	code = ftp_cmd(s, last, sizeof(last), "EPSV\r\n", NULL);
	if (code == 229 && (p = strstr(last, "(|||")) != NULL)
		port = atoi(p + 4);
	else {
		int h1, h2, h3, h4, p1, p2;
		code = ftp_cmd(s, last, sizeof(last), "PASV\r\n", NULL);
		p = strchr(last, '(');
		if (code != 227 || p == NULL || sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
			return -1;
		port = p1 * 256 + p2;
	}
	/* the address in the reply is ignored: use the control peer */
	return tcp_connect(inet_ntoa(peer->sin_addr), port, NULL);
}

static void ftp_read_all(int d, int print)
{
	char buf[2048];
	int n;
	long total = 0;

	while ((n = recv(d, buf, sizeof(buf), 0)) > 0) {
		if (print)
			fwrite(buf, 1, n, stdout);
		total += n;
	}
	close(d);
	printf("(%ld bytes)\n", total);
}

static int test_ftp(const char *host, int port, const char *user,
                    const char *pass, const char *dir)
{
	struct sockaddr_in peer;
	char last[1024], path[1024];
	int s, d, code;

	s = tcp_connect(host, port, &peer);
	if (s < 0)
		return 20;
	if (ftp_reply(s, NULL, 0) != 220)
		return 20;
	code = ftp_cmd(s, last, sizeof(last), "USER %s\r\n", user);
	if (code == 331)
		code = ftp_cmd(s, last, sizeof(last), "PASS %s\r\n", pass);
	if (code != 230) {
		printf("login failed\n");
		return 20;
	}
	ftp_cmd(s, last, sizeof(last), "FEAT\r\n", NULL);
	ftp_cmd(s, last, sizeof(last), "TYPE I\r\n", NULL);
	ftp_cmd(s, last, sizeof(last), "PWD\r\n", NULL);

	d = ftp_data(s, &peer);
	if (d < 0)
		return 20;
	code = ftp_cmd(s, last, sizeof(last), "MLSD %s\r\n", dir);
	if (code == 150 || code == 125) {
		ftp_read_all(d, 1);
		ftp_reply(s, NULL, 0);
	} else {
		close(d);
		d = ftp_data(s, &peer);
		code = ftp_cmd(s, last, sizeof(last), "LIST -a %s\r\n", dir);
		if (code == 150 || code == 125) {
			ftp_read_all(d, 1);
			ftp_reply(s, NULL, 0);
		}
	}
	snprintf(path, sizeof(path), "%s/hello.txt", dir);
	d = ftp_data(s, &peer);
	code = ftp_cmd(s, last, sizeof(last), "RETR %s\r\n", path);
	if (code == 150 || code == 125) {
		ftp_read_all(d, 1);
		ftp_reply(s, NULL, 0);
	}
	snprintf(path, sizeof(path), "%s/amiga.txt", dir);
	d = ftp_data(s, &peer);
	code = ftp_cmd(s, last, sizeof(last), "STOR %s\r\n", path);
	if (code == 150 || code == 125) {
		send(d, "written by AmigaOS 4\n", 21, 0);
		close(d);
		ftp_reply(s, NULL, 0);
	}
	ftp_cmd(s, last, sizeof(last), "SIZE %s\r\n", path);
	ftp_cmd(s, last, sizeof(last), "MDTM %s\r\n", path);
	ftp_cmd(s, last, sizeof(last), "DELE %s\r\n", path);
	ftp_cmd(s, last, sizeof(last), "QUIT\r\n", NULL);
	close(s);
	printf("FTP OK\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		printf("usage: nettest SFTP|FTP host port user password [dir]\n");
		return 10;
	}
	if (strcasecmp(argv[1], "SFTP") == 0)
		return test_sftp(argv[2], atoi(argv[3]), argv[4], argv[5], argc > 6 ? argv[6] : ".");
	return test_ftp(argv[2], atoi(argv[3]), argv[4], argv[5], argc > 6 ? argv[6] : ".");
}
