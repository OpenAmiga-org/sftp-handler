/* sftp-handler - requests from the filesysbox callbacks to the worker.
 *
 * filesysbox.library runs the filesystem callbacks in its own process and,
 * on AmigaOS 4, also directly in the tasks of the programs that use the
 * volume. clib4 (and with it libssh2 and the sockets) only works in the
 * handler process, and caller tasks may have small stacks. So the
 * callbacks (fbxglue.c) only pack each call into an Exec message for the
 * handler process (main.c), which does the work and replies.
 *
 * Code on the callback side must use Exec only: no C library calls.
 * Public domain. */
#ifndef SFH_RPC_H
#define SFH_RPC_H

#include <exec/ports.h>
#include <stdint.h>
#include <time.h>

#include "sfs.h"

enum {
	OP_GETATTR = 1,
	OP_FGETATTR,
	OP_READDIR,
	OP_OPEN,
	OP_CREATE,
	OP_READ,
	OP_WRITE,
	OP_FLUSH,
	OP_RELEASE,
	OP_TRUNCATE,
	OP_FTRUNCATE,
	OP_MKDIR,
	OP_UNLINK,
	OP_RMDIR,
	OP_RENAME,
	OP_UTIMENS,
	OP_CHMOD,
	OP_STATFS,
	OP_FSDONE,     /* the filesysbox process has finished */
	OP_RESET       /* volume set up again (DiskChange): may ask again */
};

/* readdir results: entries packed one after another */
struct rpc_dirent {
	uint32     next;   /* offset of the next entry, 0 = last */
	sfs_stat   st;
	char       name[1];
};

struct rpc {
	struct Message msg;
	int            op;
	const char    *path;
	const char    *path2;
	sfs_file      *fh;
	char          *buf;       /* read */
	const char    *cbuf;      /* write */
	uint32         size;
	int64_t        off;
	time_t         mtime;
	int            mode;      /* chmod; open: 1 = may write */
	sfs_stat       st;
	int64_t        total;
	int64_t        avail;
	uint8         *dir;       /* readdir: AllocVec'ed by the worker */
	int            result;    /* 0 / byte count, or -errno */
};

/* fbxglue.c */
extern struct MsgPort *rpc_worker_port;      /* main.c receives here */
/* 1: filesysbox converts names to UTF-8 itself; 0: names arrive in the
 * local character set and the handler converts them */
extern int fbx_utf8_names;
int  fbx_start(struct DosPacket *startup_pkt, const char *device,
               const char *volume, int readonly);
/* waits until the filesysbox process has ended (after OP_FSDONE) */
void fbx_wait(void);
void fbx_stop(void);

/* main.c */
void rpc_serve(struct rpc *r);

#endif
