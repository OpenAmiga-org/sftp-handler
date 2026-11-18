/* cryptobench - how fast this Amiga decrypts and checks SSH data with each
 * cipher sftp-handler can use. Public domain.
 *
 *   cryptobench [SECONDS]
 *
 * chacha20-poly1305 is libssh2's own C code (what OpenSSH servers choose by
 * default); the AES ciphers and HMACs run in AmiSSL. Each test works on 32 KB
 * blocks, about one SSH packet, and prints MB/s. Build (after
 * tools/build_os4.sh, which makes build/os4/amissl_*.o), in the image:
 *   ppc-amigaos-gcc -O2 -mcrt=clib4 -Isrc/core -Isrc/os4
 *     -Ibuild/deps/libssh2-1.11.1/src -Ibuild/deps/libssh2-amissl/include
 *     -Ibuild/deps/amissl-sdk/AmiSSL/Developer/include
 *     -o build/os4/cryptobench tests/os4/cryptobench.c src/os4/amissl.c
 *     build/os4/amissl_*.o -athread=native -Lbuild/deps/libssh2-amissl/lib
 *     -Lbuild/deps/amissl-clib4 -lssh2 -lamisslauto
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>
#include <openssl/params.h>

#include "cipher-chachapoly.h" /* libssh2's src/ */
#include "os4.h"

#define BLK (32 * 1024)

static unsigned char in[BLK + 64], out[BLK + 64], key[64], iv[16];
static double seconds = 2.0;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef void (*work_fn)(void *ud);

static void run(const char *name, work_fn fn, void *ud)
{
	double t0 = now(), t;
	long n = 0;

	do {
		fn(ud);
		n++;
		t = now() - t0;
	} while (t < seconds);
	printf("%-34s %7.1f MB/s\n", name, n * (double)BLK / t / (1024 * 1024));
	fflush(stdout);
}

static void w_memcpy(void *ud)
{
	(void)ud;
	memcpy(out, in, BLK);
}

static void w_chachapoly(void *ud)
{
	static u_int seq;

	/* encrypting does the same work as decrypting: keystream, MAC */
	chachapoly_crypt((struct chachapoly_ctx *)ud, seq++, out, in, BLK, 4, 1);
}

static void w_chacha(void *ud)
{
	chacha_encrypt_bytes((struct chacha_ctx *)ud, in, out, BLK);
}

static void w_poly(void *ud)
{
	(void)ud;
	poly1305_auth(out, in, BLK, key);
}

static void w_evp(void *ud)
{
	EVP_Cipher((EVP_CIPHER_CTX *)ud, out, in, BLK);
}

static void w_mac(void *ud)
{
	EVP_MAC_CTX *m = ud;
	size_t len;

	EVP_MAC_init(m, NULL, 0, NULL);
	EVP_MAC_update(m, in, BLK);
	EVP_MAC_final(m, out, &len, 64);
}

static void cipher(const char *name, const EVP_CIPHER *c)
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

	if (ctx == NULL || EVP_CipherInit(ctx, c, key, iv, 0) != 1) {
		printf("%-34s not available\n", name);
	} else {
		run(name, w_evp, ctx);
	}
	EVP_CIPHER_CTX_free(ctx);
}

static void hmac(const char *name, const char *digest)
{
	EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
	EVP_MAC_CTX *m = mac != NULL ? EVP_MAC_CTX_new(mac) : NULL;
	OSSL_PARAM p[2];

	p[0] = OSSL_PARAM_construct_utf8_string("digest", (char *)digest, 0);
	p[1] = OSSL_PARAM_construct_end();
	if (m == NULL || EVP_MAC_init(m, key, 32, p) != 1)
		printf("%-34s not available\n", name);
	else
		run(name, w_mac, m);
	EVP_MAC_CTX_free(m);
	EVP_MAC_free(mac);
}

int main(int argc, char **argv)
{
	struct chachapoly_ctx cp;
	struct chacha_ctx cc;
	int i;

	if (argc > 1)
		seconds = atof(argv[1]);
	for (i = 0; i < (int)sizeof(in); i++)
		in[i] = (unsigned char)(i * 7);
	for (i = 0; i < (int)sizeof(key); i++)
		key[i] = (unsigned char)(i + 1);

	printf("32 KB blocks, %.0f s per test\n", seconds);
	run("memcpy", w_memcpy, NULL);

	chachapoly_init(&cp, key, 64);
	run("chacha20-poly1305 (libssh2)", w_chachapoly, &cp);
	chacha_keysetup(&cc, key, 256);
	chacha_ivsetup(&cc, iv, NULL);
	run("  of it: chacha20", w_chacha, &cc);
	run("  of it: poly1305", w_poly, NULL);

	if (amissl_open() != 0) {
		printf("AmiSSL could not be opened\n");
		return 10;
	}
	printf("AmiSSL %d\n", amissl_version());
	cipher("aes128-ctr (AmiSSL)", EVP_aes_128_ctr());
	cipher("aes256-ctr (AmiSSL)", EVP_aes_256_ctr());
	cipher("aes128-gcm (AmiSSL)", EVP_aes_128_gcm());
	cipher("aes256-gcm (AmiSSL)", EVP_aes_256_gcm());
	hmac("hmac-sha1 (AmiSSL)", "SHA1");
	hmac("hmac-sha2-256 (AmiSSL)", "SHA2-256");
	hmac("hmac-sha2-512 (AmiSSL)", "SHA2-512");
	amissl_close();
	return 0;
}
