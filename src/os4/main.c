/* sftp-handler - AmigaOS 4 filesystem handler for FTP and SFTP sites,
 * built on filesysbox.library and clib4. Public domain.
 *
 * DOSDriver (Devs/DOSDrivers/SFTP, device SFTP:):
 *   Handler   = L:sftp-handler
 *   StackSize = 262144
 *   Priority  = 5
 *   GlobVec   = -1
 *   Startup   = "VOLUME=\"SFTP Sites\""
 *
 * The sites come from ENV:SFTPHandler/sites.xml (sftphprefs).
 *
 * This process (the handler process DOS starts) does all the work: it owns
 * the clib4 context, the network connections and the cache. A second
 * process runs filesysbox; its callbacks send every call here (fbxglue.c,
 * rpc.h).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <workbench/startup.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "charset.h"
#include "config.h"
#include "platform.h"
#include "remote.h"
#include "sfs.h"
#include "os4.h"
#include "rpc.h"
#include "version.h"

static const char __attribute__((used)) verstag[] = SFH_VERSTAG;

#define SFH_MAGIC  0x53464853UL   /* 'SFHS' */
#define TICK_SECS  5              /* idle work */

/* ------------------------------------------------------------------------
 * Startup
 *
 * clib4's startup code takes the first message on the process port as a
 * Workbench startup message when there is no CLI, and replies it when the
 * program ends. A handler is started by DOS with a startup packet there
 * instead, and filesysbox replies that packet itself. So this function runs
 * first (tools/os4.mk renames it to _start, where AmigaOS starts a program):
 * it takes the packet off the port and puts a harmless Workbench message
 * there for clib4, one without a reply port. It must not use the C library.
 * ------------------------------------------------------------------------ */

struct sfh_start {
	struct WBStartup  wbs;
	struct WBArg      arg;
	struct DosPacket *pkt;
	uint32            magic;
};

/* clib4's _start; renamed to clib4_crt_start after linking (tools/os4.mk) */
extern int _start(STRPTR args, int32 arglen, struct Library *sysbase);

/* the clib4.library the program was built against */
#define CLIB4_VERSION  2
#define CLIB4_REVISION 5

/* clib4's startup code ends the program when clib4.library is too old,
 * and DOS would then wait for the handler's answer for ever (every access
 * to the device hangs). So the version is checked here first; without the
 * C library: Exec, DOS and Intuition only. */
static int clib4_ok(struct ExecIFace *iexec, struct DosPacket *pkt)
{
	struct Library *lib = iexec->OpenLibrary("clib4.library", 0);
	struct Library *ibase, *dbase;
	struct IntuitionIFace *iint;
	struct DOSIFace *idos;
	int ok = lib != NULL && (lib->lib_Version > CLIB4_VERSION ||
	         (lib->lib_Version == CLIB4_VERSION && lib->lib_Revision >= CLIB4_REVISION));

	if (lib != NULL)
		iexec->CloseLibrary(lib);
	if (ok)
		return 1;
	ibase = iexec->OpenLibrary("intuition.library", 50);
	if (ibase != NULL) {
		iint = (struct IntuitionIFace *)iexec->GetInterface(ibase, "main", 1, NULL);
		if (iint != NULL) {
			struct EasyStruct es;

			es.es_StructSize = sizeof(es);
			es.es_Flags = 0;
			es.es_Title = (STRPTR)SFH_VERS;
			es.es_TextFormat = (STRPTR)"sftp-handler needs clib4.library 2.5 or newer.\n"
			                           "OS4Depot: development/library/misc/clib4.lha";
			es.es_GadgetFormat = (STRPTR)"OK";
			es.es_Screen = NULL;
			es.es_TagList = NULL;
			iint->EasyRequestArgs(NULL, &es, NULL, NULL);
			iexec->DropInterface((struct Interface *)iint);
		}
		iexec->CloseLibrary(ibase);
	}
	dbase = iexec->OpenLibrary("dos.library", 50);
	if (dbase != NULL) {
		idos = (struct DOSIFace *)iexec->GetInterface(dbase, "main", 1, NULL);
		if (idos != NULL) {
			idos->ReplyPkt(pkt, DOSFALSE, ERROR_INVALID_RESIDENT_LIBRARY);
			iexec->DropInterface((struct Interface *)idos);
		}
		iexec->CloseLibrary(dbase);
	}
	return 0;
}

int32 sfh_entry(STRPTR args, int32 arglen, struct Library *sysbase)
{
	struct ExecIFace *iexec = (struct ExecIFace *)((struct ExecBase *)sysbase)->MainInterface;
	struct Process *me = (struct Process *)iexec->FindTask(NULL);
	struct sfh_start st;

	if (me->pr_CLI == ZERO) {
		struct Message *msg;
		struct DosPacket *pkt;

		iexec->WaitPort(&me->pr_MsgPort);
		msg = iexec->GetMsg(&me->pr_MsgPort);
		pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
		if (pkt != NULL && pkt->dp_Link == msg) {
			if (!clib4_ok(iexec, pkt))
				return RETURN_FAIL;
			/* every field set by hand: no memset() before the C library
			 * is ready */
			st.pkt = pkt;
			st.magic = SFH_MAGIC;
			st.arg.wa_Lock = ZERO;
			st.arg.wa_Name = (STRPTR)"sftp-handler";
			st.wbs.sm_Message.mn_Node.ln_Succ = NULL;
			st.wbs.sm_Message.mn_Node.ln_Pred = NULL;
			st.wbs.sm_Message.mn_Node.ln_Type = NT_MESSAGE;
			st.wbs.sm_Message.mn_Node.ln_Pri = 0;
			st.wbs.sm_Message.mn_Node.ln_Name = NULL;
			st.wbs.sm_Message.mn_ReplyPort = NULL;
			st.wbs.sm_Message.mn_Length = sizeof(struct WBStartup);
			st.wbs.sm_Process = &me->pr_MsgPort;
			st.wbs.sm_Segment = ZERO;
			st.wbs.sm_NumArgs = 1;
			st.wbs.sm_ToolWindow = (STRPTR)"NIL:";
			st.wbs.sm_ArgList = &st.arg;
			msg = &st.wbs.sm_Message;
		}
		iexec->PutMsg(&me->pr_MsgPort, msg);
	}
	return _start(args, arglen, sysbase);
}

/* ------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------ */

static const char template[] =
	"VOLUME/K,CONFIGDIR/K,CACHETIME/K/N,IDLETIME/K/N,TIMEOUT/K/N,"
	"TEMPDIR/K,KEEPDIR/K,NOICONS/S,READONLY/S,CHARSET/K,DEBUG/S,CODEIDLETIME/K/N";

enum {
	A_VOLUME, A_CONFIGDIR, A_CACHETIME, A_IDLETIME, A_TIMEOUT,
	A_TEMPDIR, A_KEEPDIR, A_NOICONS, A_READONLY, A_CHARSET, A_DEBUG, A_CODEIDLETIME, A_COUNT
};

static struct {
	char         device[64];
	char         volume[128];
	char         display[130];
	char         config_dir[256];
	char         save_dir[256];
	char         known_hosts[300];
	char         tempdir[256];
	char         keepdir[256];
	int          readonly;
	int          convert;   /* names arrive in the local character set */
	int          cs;        /* that character set */
	sfs_options  fo;
	sfs         *fs;
	struct MsgPort     *timer_port;
	struct TimeRequest *timer;
	int          timer_open;
} g;

static void strip_quotes(char *s)
{
	size_t n;
	char *p = s;

	while (isspace((unsigned char)*p))
		p++;
	memmove(s, p, strlen(p) + 1);
	n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

/* ------------------------------------------------------------------------
 * Requests from the filesysbox callbacks
 * ------------------------------------------------------------------------ */

struct dirbuf {
	uint8  *data;
	uint32  len;
	uint32  cap;
	uint32  last;   /* offset of the last entry */
	int     failed;
};

static int dir_add(void *ud, const char *uname, const sfs_stat *st)
{
	struct dirbuf *d = ud;
	char *lname = g.convert ? cs_utf8_to_name(g.cs, uname) : NULL;
	const char *name = lname != NULL ? lname : uname;
	uint32 need = (offsetof(struct rpc_dirent, name) + strlen(name) + 1 + 7) & ~7U;
	struct rpc_dirent *e;

	if (d->len + need > d->cap) {
		uint32 ncap = d->cap ? d->cap * 2 : 8192;
		uint8 *n;

		while (ncap < d->len + need)
			ncap *= 2;
		/* Exec memory: the caller frees it with FreeVec() in another task */
		n = IExec->AllocVecTags(ncap, AVT_Type, MEMF_SHARED, TAG_END);
		if (n == NULL) {
			free(lname);
			d->failed = 1;
			return 1;
		}
		if (d->data != NULL) {
			memcpy(n, d->data, d->len);
			IExec->FreeVec(d->data);
		}
		d->data = n;
		d->cap = ncap;
	}
	e = (struct rpc_dirent *)(d->data + d->len);
	e->next = 0;
	e->st = *st;
	strcpy(e->name, name);
	if (d->len > 0)
		((struct rpc_dirent *)(d->data + d->last))->next = d->len;
	d->last = d->len;
	d->len += need;
	free(lname);
	return 0;
}

void rpc_serve(struct rpc *r)
{
	char *conv1 = NULL, *conv2 = NULL;
	int err = 0;

	/* names from filesysbox in the local character set */
	if (g.convert) {
		if (r->path != NULL && (conv1 = cs_local_to_utf8(g.cs, r->path)) != NULL)
			r->path = conv1;
		if (r->path2 != NULL && (conv2 = cs_local_to_utf8(g.cs, r->path2)) != NULL)
			r->path2 = conv2;
	}

	switch (r->op) {
	case OP_GETATTR:
		err = sfs_getattr(g.fs, r->path, &r->st);
		break;
	case OP_FGETATTR:
		err = sfs_fgetattr(g.fs, r->fh, &r->st);
		break;
	case OP_READDIR: {
		struct dirbuf d;

		memset(&d, 0, sizeof(d));
		err = sfs_readdir(g.fs, r->path, dir_add, &d);
		if (err == 0 && d.failed)
			err = -ENOMEM;
		if (err != 0 || d.len == 0) {
			if (d.data != NULL)
				IExec->FreeVec(d.data);
			d.data = NULL;
		}
		r->dir = d.data;
		break;
	}
	case OP_OPEN:
		err = sfs_open(g.fs, r->path, r->mode, &r->fh);
		break;
	case OP_CREATE:
		err = sfs_create(g.fs, r->path, &r->fh);
		break;
	case OP_READ:
		err = sfs_read(g.fs, r->fh, r->buf, r->size, r->off);
		break;
	case OP_WRITE:
		err = sfs_write(g.fs, r->fh, r->cbuf, r->size, r->off);
		break;
	case OP_FLUSH:
		err = sfs_flush(g.fs, r->fh);
		break;
	case OP_RELEASE:
		err = sfs_release(g.fs, r->fh);
		break;
	case OP_TRUNCATE:
		err = sfs_truncate(g.fs, r->path, r->off);
		break;
	case OP_FTRUNCATE:
		err = sfs_ftruncate(g.fs, r->fh, r->off);
		break;
	case OP_MKDIR:
		err = sfs_mkdir(g.fs, r->path);
		break;
	case OP_UNLINK:
		err = sfs_unlink(g.fs, r->path);
		break;
	case OP_RMDIR:
		err = sfs_rmdir(g.fs, r->path);
		break;
	case OP_RENAME:
		err = sfs_rename(g.fs, r->path, r->path2);
		break;
	case OP_UTIMENS:
		err = sfs_utimens(g.fs, r->path, r->mtime);
		break;
	case OP_CHMOD:
		err = sfs_chmod(g.fs, r->path, r->mode);
		break;
	case OP_STATFS:
		r->total = 0;
		r->avail = 0;
		err = sfs_statfs(g.fs, r->path, &r->total, &r->avail);
		break;
	case OP_RESET:
		sfs_reset(g.fs);
		err = 0;
		break;
	default:
		err = -ENOSYS;
		break;
	}
	r->result = err;
	if (plat_loglevel >= LOG_DEBUG)
		plat_log(LOG_DEBUG, "op %d %s -> %d\n", r->op, r->path != NULL ? r->path : "", err);
	/* the caller's strings are back in place for the reply */
	free(conv1);
	free(conv2);
}

/* ------------------------------------------------------------------------
 * Handler
 * ------------------------------------------------------------------------ */

static int read_startup(const char *startup, int32 *args, struct RDArgs **rda_out,
                        char **buf_out)
{
	struct RDArgs *rda;
	char *buf;
	size_t n = strlen(startup);

	*rda_out = NULL;
	*buf_out = NULL;
	buf = malloc(n + 2);
	if (buf == NULL)
		return ERROR_NO_FREE_STORE;
	strcpy(buf, startup);
	strip_quotes(buf);
	strcat(buf, "\n");
	rda = IDOS->AllocDosObject(DOS_RDARGS, NULL);
	if (rda == NULL) {
		free(buf);
		return ERROR_NO_FREE_STORE;
	}
	rda->RDA_Source.CS_Buffer = (STRPTR)buf;
	rda->RDA_Source.CS_Length = strlen(buf);
	rda->RDA_Flags = RDAF_NOPROMPT;
	if (IDOS->ReadArgs(template, args, rda) == NULL) {
		int32 err = IDOS->IoErr();
		IDOS->FreeDosObject(DOS_RDARGS, rda);
		free(buf);
		return err != 0 ? err : ERROR_REQUIRED_ARG_MISSING;
	}
	/* keep buf until FreeArgs(): the results may point into it */
	*rda_out = rda;
	*buf_out = buf;
	return 0;
}

static int timer_open(void)
{
	g.timer_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
	if (g.timer_port == NULL)
		return -1;
	g.timer = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
		ASOIOR_Size, sizeof(struct TimeRequest),
		ASOIOR_ReplyPort, g.timer_port,
		TAG_END);
	if (g.timer == NULL)
		return -1;
	if (IExec->OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)g.timer, 0) != 0)
		return -1;
	g.timer_open = 1;
	return 0;
}

static void timer_start(void)
{
	g.timer->Request.io_Command = TR_ADDREQUEST;
	g.timer->Time.Seconds = TICK_SECS;
	g.timer->Time.Microseconds = 0;
	IExec->SendIO((struct IORequest *)g.timer);
}

static void timer_close(void)
{
	if (g.timer_open) {
		if (!IExec->CheckIO((struct IORequest *)g.timer))
			IExec->AbortIO((struct IORequest *)g.timer);
		IExec->WaitIO((struct IORequest *)g.timer);
		IExec->CloseDevice((struct IORequest *)g.timer);
	}
	g.timer_open = 0;
	if (g.timer != NULL)
		IExec->FreeSysObject(ASOT_IOREQUEST, g.timer);
	g.timer = NULL;
	if (g.timer_port != NULL)
		IExec->FreeSysObject(ASOT_PORT, g.timer_port);
	g.timer_port = NULL;
}

static void serve_requests(void)
{
	uint32 rpcsig = 1UL << rpc_worker_port->mp_SigBit;
	uint32 timersig = g.timer_open ? 1UL << g.timer_port->mp_SigBit : 0;
	int done = 0;

	if (g.timer_open)
		timer_start();
	while (!done) {
		struct rpc *r;
		uint32 sigs = IExec->Wait(rpcsig | timersig);

		if ((sigs & timersig) && IExec->GetMsg(g.timer_port) != NULL) {
			sfs_idle(g.fs);
			timer_start();
		}
		while ((r = (struct rpc *)IExec->GetMsg(rpc_worker_port)) != NULL) {
			if (r->op == OP_FSDONE) {
				r->result = 0;
				done = 1;
			} else {
				rpc_serve(r);
			}
			IExec->ReplyMsg(&r->msg);
		}
	}
}

/* DOS keeps a handler's code in dn_SegList for its next start, and removing
 * the device does not unload it. Instead this process frees its code when
 * it ends, and DOS loads it again for a later start. Returns 1 when the
 * code was taken over. */
static int take_seglist(struct DeviceNode *dn)
{
	struct Process *me = (struct Process *)IExec->FindTask(NULL);
	BPTR seg = IDOS->GetProcSegList(me, GPSLF_SEG);
	int taken = 0;

	IExec->Forbid();
	if (seg != ZERO && dn->dn_SegList == seg && (me->pr_Flags & PRF_FREESEGLIST) == 0) {
		dn->dn_SegList = ZERO;
		me->pr_Flags |= PRF_FREESEGLIST;
		taken = 1;
	}
	IExec->Permit();
	return taken;
}

static int handler_main(struct DosPacket *pkt)
{
	struct DeviceNode *dn = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
	struct RDArgs *rda = NULL;
	char *argbuf = NULL;
	int32 args[A_COUNT];
	const char *startup = "";
	int32 err;
	int seg_taken, noicons;

	seg_taken = take_seglist(dn);
	memset(args, 0, sizeof(args));
	memset(&g, 0, sizeof(g));
	snprintf(g.device, sizeof(g.device), "%s", (const char *)BADDR(dn->dn_Name) + 1);
	if (dn->dn_Startup != ZERO)
		startup = (const char *)BADDR(dn->dn_Startup) + 1;
	dn->dn_Startup = ZERO;

	err = read_startup(startup, args, &rda, &argbuf);
	if (err != 0) {
		char msg[400];
		snprintf(msg, sizeof(msg),
		         "The Startup line of the DOSDriver %s: is not valid.\n\n"
		         "Expected: %s", g.device, template);
		req_message(SFH_VERS, msg);
		goto fail;
	}

	if (args[A_DEBUG])
		plat_loglevel = LOG_DEBUG;
	plat_log(LOG_DEBUG, "code %s\n", seg_taken ? "freed when the handler ends" : "kept by DOS");
	g.cs = cs_from_name(args[A_CHARSET] ? (const char *)args[A_CHARSET] : getenv("CHARSET"));
	g.readonly = args[A_READONLY] != 0;
	noicons = args[A_NOICONS] != 0;
	snprintf(g.volume, sizeof(g.volume), "%s",
	         args[A_VOLUME] ? (const char *)args[A_VOLUME] : "SFTP Sites");
	if (args[A_CONFIGDIR]) {
		/* a drawer of its own: ENV: and ENVARC: copies do not apply */
		snprintf(g.config_dir, sizeof(g.config_dir), "%s", (const char *)args[A_CONFIGDIR]);
		snprintf(g.save_dir, sizeof(g.save_dir), "%s", (const char *)args[A_CONFIGDIR]);
	} else {
		snprintf(g.config_dir, sizeof(g.config_dir), "ENV:%s", CFG_DIR);
		snprintf(g.save_dir, sizeof(g.save_dir), "ENVARC:%s", CFG_DIR);
	}
	snprintf(g.known_hosts, sizeof(g.known_hosts), "%s/%s", g.save_dir, CFG_KNOWNHOSTS);
	snprintf(g.tempdir, sizeof(g.tempdir), "%s", args[A_TEMPDIR] ? (const char *)args[A_TEMPDIR] : "T:");
	snprintf(g.keepdir, sizeof(g.keepdir), "%s", args[A_KEEPDIR] ? (const char *)args[A_KEEPDIR] : g.tempdir);

	memset(&g.fo, 0, sizeof(g.fo));
	g.fo.config_dir = g.config_dir;
	g.fo.save_dir = strcmp(g.save_dir, g.config_dir) != 0 ? g.save_dir : NULL;
	g.fo.known_hosts = g.known_hosts;
	g.fo.icon_file = noicons ? NULL : "ENV:Sys/def_drawer.info";
	g.fo.tempdir = g.tempdir;
	g.fo.keepdir = g.keepdir;
	g.fo.cache_ms = args[A_CACHETIME] ? *(int32 *)args[A_CACHETIME] * 1000 : 5000;
	g.fo.idle_ms = args[A_IDLETIME] ? *(int32 *)args[A_IDLETIME] * 1000 : 300000;
	/* two-factor logins: each new connection needs a new code */
	g.fo.code_idle_ms = args[A_CODEIDLETIME] ? *(int32 *)args[A_CODEIDLETIME] * 1000 : 0;
	g.fo.io_timeout_ms = args[A_TIMEOUT] ? *(int32 *)args[A_TIMEOUT] * 1000 : 60000;
	g.fo.connect_timeout_ms = 20000;
	g.fo.readonly = g.readonly;
	snprintf(g.display, sizeof(g.display), "%s:", g.device);
	g.fo.display = g.display;

	IDOS->FreeArgs(rda);
	IDOS->FreeDosObject(DOS_RDARGS, rda);
	rda = NULL;
	free(argbuf);
	argbuf = NULL;

	if (amissl_open() != 0) {
		req_message(SFH_VERS, "This handler needs AmiSSL 5 or newer (and a TCP/IP stack).\n"
		            "AmiSSL: https://github.com/jens-maus/amissl/releases\n"
		            "or OS4Depot: library/misc/amissl.lha");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}
	if (sftp_global_init() != 0) {
		req_message(SFH_VERS, "The SSH library could not be set up (AmiSSL).");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}
	/* the drawer for host keys and remembered passwords */
	mkdir(g.save_dir, 0700);
	timer_open();

	g.fs = sfs_new(&g.fo);
	if (g.fs == NULL) {
		err = ERROR_NO_FREE_STORE;
		goto fail;
	}
	plat_log(LOG_INFO, "%s: sites from %s\n", g.device, g.config_dir);

	if (fbx_start(pkt, g.device, g.volume, g.readonly) != 0) {
		req_message(SFH_VERS, "This handler needs filesysbox.library 54.8 or newer.");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}
	pkt = NULL; /* the filesysbox process replies it */
	/* The network work happens here. Below the TCP/IP stack's priority so
	 * that waiting for data can never hold the stack up; the filesysbox
	 * process keeps the handler priority. */
	IExec->SetTaskPri(IExec->FindTask(NULL), -1);
	g.convert = !fbx_utf8_names;
	plat_charset = g.cs; /* requester texts are always in the local set */
	plat_log(LOG_INFO, "file names: %s\n", g.convert ?
	         "converted by the handler" : "converted by filesysbox");
	serve_requests();
	fbx_wait(); /* its code goes away with ours */
	err = 0;

fail:
	if (pkt != NULL)
		IDOS->ReplyPkt(pkt, DOSFALSE, err);
	if (rda != NULL) {
		IDOS->FreeArgs(rda);
		IDOS->FreeDosObject(DOS_RDARGS, rda);
	}
	free(argbuf);
	sfs_free(g.fs);
	g.fs = NULL;
	timer_close();
	sftp_global_exit();
	fbx_stop();
	amissl_close();
	req_close_libs();
	return err == 0 ? RETURN_OK : RETURN_FAIL;
}

int main(int argc, char **argv)
{
	struct sfh_start *st = (struct sfh_start *)argv;

	if (argc > 0) {
		printf("%s (%s)\n"
		       "A filesystem handler for FTP and SFTP sites.\n"
		       "It is started by mounting its DOSDriver (SFTP:); the sites are set\n"
		       "up with sftphprefs. See SFTPHandler.guide.\n",
		       SFH_VERS, SFH_DATE);
		return RETURN_OK;
	}
	if (st == NULL || st->magic != SFH_MAGIC)
		return RETURN_FAIL; /* Workbench start */
	signal(SIGINT, SIG_IGN);
	return handler_main(st->pkt);
}
