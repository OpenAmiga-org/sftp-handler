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

int amissl_open(void)
{
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
	/* Ask for the oldest AmiSSL 5 API (AMISSL_V3xx = AmiSSL 5.1, OpenSSL
	 * 3.0.3), not AMISSL_CURRENT_VERSION (the SDK's, 5.27): amisslmaster
	 * then opens the newest AmiSSL 5 that is installed, and an older one
	 * such as 5.25 is not refused. The AmiSSL 5 interface only grows at
	 * the end, and every function libssh2 and this handler call is in the
	 * 5.1 interface at the same place (checked against the 5.1 SDK's
	 * amissl.xml, see README.md). libssh2 only uses the opaque OpenSSL
	 * API, so no structure layouts are shared. */
	if (IAmiSSLMaster->OpenAmiSSLTags(AMISSL_V3xx,
	                                  AmiSSL_UsesOpenSSLStructs, FALSE,
	                                  AmiSSL_InitAmiSSL, TRUE,
	                                  AmiSSL_GetIAmiSSL, &IAmiSSL,
	                                  AmiSSL_ISocket, ISock,
	                                  AmiSSL_ErrNoPtr, &errno,
	                                  TAG_DONE) != 0)
		return -2;
	initialised = 1;
	return 0;
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
