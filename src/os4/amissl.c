/* sftp-handler - opens AmiSSL for libssh2's cryptography. AmiSSL is set up per
 * task; all network work happens in the handler process (main.c), so this
 * is called there. Public domain. */
#include <proto/exec.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

#include <errno.h>

#include "os4.h"

/* used by the AmiSSL stubs that libssh2 calls */
struct Library            *AmiSSLMasterBase;
struct AmiSSLMasterIFace  *IAmiSSLMaster;
struct Library            *AmiSSLBase;
struct AmiSSLIFace        *IAmiSSL;

static struct Library     *SockBase;
static struct Interface   *ISock;
static int                 initialised;

/* tools/gen_amissl_glue.py: the stubs call amissl_fn[], filled here with
 * the functions of the opened interface. AmiSSL 5 (OpenSSL 3) and AmiSSL 6
 * (OpenSSL 4) have the same functions with the same prototypes (checked by
 * tools/check_amissl_api.py) but at other places in their interfaces. */
extern void *amissl_fn[];
extern const int amissl_nfn;
extern const unsigned int amissl_off_v3[], amissl_off_v4[];
extern const int amissl_v4xx;

static int amissl_api; /* 5 or 6 */

static int open_api(int api)
{
	return IAmiSSLMaster->OpenAmiSSLTags(api,
	                                     AmiSSL_UsesOpenSSLStructs, FALSE,
	                                     AmiSSL_InitAmiSSL, TRUE,
	                                     AmiSSL_GetIAmiSSL, &IAmiSSL,
	                                     AmiSSL_ISocket, ISock,
	                                     AmiSSL_ErrNoPtr, &errno,
	                                     TAG_DONE);
}

int amissl_open(void)
{
	const unsigned int *off;
	int i;

	SockBase = IExec->OpenLibrary("bsdsocket.library", 4);
	if (SockBase == NULL)
		return -1;
	ISock = IExec->GetInterface(SockBase, "main", 1, NULL);
	if (ISock == NULL)
		return -1;
	AmiSSLMasterBase = IExec->OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
	if (AmiSSLMasterBase == NULL)
		return -2;
	IAmiSSLMaster = (struct AmiSSLMasterIFace *)IExec->GetInterface(AmiSSLMasterBase, "main", 1, NULL);
	if (IAmiSSLMaster == NULL)
		return -2;
	/* AmiSSL 6 when it is there (amisslmaster 6 comes with it), else the
	 * oldest AmiSSL 5 API (AMISSL_V3xx = AmiSSL 5.1, OpenSSL 3.0.3), not the
	 * SDK's AMISSL_CURRENT_VERSION (5.27): amisslmaster then opens the
	 * newest AmiSSL 5 installed, and an older one such as 5.25 is not
	 * refused. Each AmiSSL interface only grows at the end; every function
	 * used is in AmiSSL 5.1 at the same place (tools/check_amissl_api.py).
	 * libssh2 only uses the opaque OpenSSL API, so no structure layouts
	 * are shared. */
	amissl_api = 0;
	if (AmiSSLMasterBase->lib_Version >= 6 && open_api(amissl_v4xx) == 0)
		amissl_api = 6;
	else if (open_api(AMISSL_V3xx) == 0)
		amissl_api = 5;
	else
		return -2;
	initialised = 1;
	off = amissl_api == 6 ? amissl_off_v4 : amissl_off_v3;
	for (i = 0; i < amissl_nfn; i++)
		amissl_fn[i] = *(void **)((char *)IAmiSSL + off[i]);
	return 0;
}

/* 5 or 6 while AmiSSL is open, else 0 */
int amissl_version(void)
{
	return initialised ? amissl_api : 0;
}

void amissl_close(void)
{
	if (initialised)
		IAmiSSLMaster->CloseAmiSSL(); /* also cleans up and drops IAmiSSL */
	initialised = 0;
	IAmiSSL = NULL;
	AmiSSLBase = NULL;
	if (IAmiSSLMaster != NULL)
		IExec->DropInterface((struct Interface *)IAmiSSLMaster);
	IAmiSSLMaster = NULL;
	if (AmiSSLMasterBase != NULL)
		IExec->CloseLibrary(AmiSSLMasterBase);
	AmiSSLMasterBase = NULL;
	if (ISock != NULL)
		IExec->DropInterface(ISock);
	ISock = NULL;
	if (SockBase != NULL)
		IExec->CloseLibrary(SockBase);
	SockBase = NULL;
}
