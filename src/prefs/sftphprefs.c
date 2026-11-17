/* sftphprefs - edits the site list of sftp-handler
 * (ENV:SFTPHandler/sites.xml, saved in ENVARC:). Built with clib4: the
 * "Test connection" button uses the handler's own FTP and SFTP code.
 * Public domain. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/listbrowser.h>
#include <proto/layout.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/string.h>
#include <gadgets/integer.h>
#include <gadgets/chooser.h>
#include <gadgets/listbrowser.h>
#include <images/label.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <intuition/gadgetclass.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "charset.h"
#include "config.h"
#include "platform.h"
#include "remote.h"
#include "util.h"
#include "os4.h"
#include "version.h"

static const char __attribute__((used)) verstag[] =
	"\0$VER: sftphprefs " SFH_VERNUM " (" SFH_DATE ")" SFH_BETA_STR;

#define PROGNAME "sftphprefs"
#define ENV_DIR    "ENV:" CFG_DIR
#define ENVARC_DIR "ENVARC:" CFG_DIR

/* from requesters.c */
extern struct IntuitionIFace *IIntuition;
extern struct LayoutIFace *ILayout;

struct ListBrowserIFace *IListBrowser;
struct AslIFace *IAsl;
struct Library *AslBase;
static struct ClassLibrary *WinLib, *LayLib, *BtnLib, *StrLib, *IntLib, *ChoLib,
                           *LbLib, *LblLib;
static Class *WinCls, *LayCls, *BtnCls, *StrCls, *IntCls, *ChoCls, *LbCls, *LblCls;

enum {
	G_LIST = 1, G_ADD, G_DUP, G_REMOVE,
	G_NAME, G_PROTO, G_HOST, G_PORT, G_USER, G_PASS, G_FOLDER, G_KEYFILE, G_KEYPICK,
	G_TEST, G_FORGET, G_SAVE, G_USE, G_CANCEL, G_COUNT
};

enum { M_ABOUT = 1, M_QUIT, M_LASTSAVED, M_RESTORE, M_RESET };

static struct NewMenu menus[] = {
	{ NM_TITLE, (STRPTR)"Project",       NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"About...",      (STRPTR)"?", 0, 0, (APTR)M_ABOUT },
	{ NM_ITEM,  NM_BARLABEL,             NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Quit",          (STRPTR)"Q", 0, 0, (APTR)M_QUIT },
	{ NM_TITLE, (STRPTR)"Edit",          NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Reset to defaults", (STRPTR)"D", 0, 0, (APTR)M_RESET },
	{ NM_ITEM,  (STRPTR)"Last saved",    (STRPTR)"L", 0, 0, (APTR)M_LASTSAVED },
	{ NM_ITEM,  (STRPTR)"Restore",       (STRPTR)"R", 0, 0, (APTR)M_RESTORE },
	{ NM_END,   NULL,                    NULL, 0, 0, NULL }
};

static const char *proto_labels[] = { "FTP", "SFTP", NULL };

static struct ColumnInfo columns[] = {
	{ 30, (STRPTR)"Name", 0 },
	{ 10, (STRPTR)"Protocol", 0 },
	{ 35, (STRPTR)"Server", 0 },
	{ 10, (STRPTR)"Port", 0 },
	{ 15, (STRPTR)"User", 0 },
	{ -1, NULL, 0 }
};

static struct {
	Object        *win;
	struct Window *w;
	Object        *g[G_COUNT];
	struct List    labels;
	site_list      sites;     /* UTF-8, as in the file */
	site_list      start;     /* what was there when the program started */
	int            sel;       /* selected site, -1 = none */
	int            cs;        /* local character set */
	int            amissl;    /* 1 = open, -1 = failed */
} g;

/* --- character sets --------------------------------------------------------- */

static void to_local(const char *utf8, char *out, size_t outlen)
{
	char *l = cs_utf8_to_local(g.cs, utf8);

	snprintf(out, outlen, "%s", l != NULL ? l : utf8);
	free(l);
}

/* for messages: keeps line breaks */
static void to_local_msg(const char *utf8, char *out, size_t outlen)
{
	char *l = cs_utf8_to_message(g.cs, utf8);

	snprintf(out, outlen, "%s", l != NULL ? l : utf8);
	free(l);
}

static void to_utf8(const char *local, char *out, size_t outlen)
{
	char *u = cs_local_to_utf8(g.cs, local);

	snprintf(out, outlen, "%s", u != NULL ? u : local);
	free(u);
}

/* --- messages ---------------------------------------------------------------- */

static int ask(const char *text, const char *buttons)
{
	struct EasyStruct es;

	memset(&es, 0, sizeof(es));
	es.es_StructSize = sizeof(es);
	es.es_Title = (STRPTR)PROGNAME;
	es.es_TextFormat = (STRPTR)"%s";
	es.es_GadgetFormat = (STRPTR)buttons;
	return (int)IIntuition->EasyRequest(g.w, &es, NULL, text);
}

static void tell(const char *text)
{
	ask(text, "OK");
}

static void busy(int on)
{
	if (g.w != NULL)
		IIntuition->SetWindowPointer(g.w, WA_BusyPointer, on, WA_PointerDelay, on, TAG_END);
}

/* --- gadget helpers ------------------------------------------------------------ */

static char  *strbufs[G_COUNT];
static size_t strsizes[G_COUNT];

/* Setting a string gadget to "" left the text of the gadget set before it
 * on display; the empty value is written into its buffer instead. */
static void set_text(int id, const char *text)
{
	struct TagItem tags[2];

	tags[0].ti_Tag = STRINGA_TextVal;
	tags[0].ti_Data = (uint32)(text[0] != '\0' ? text : " ");
	tags[1].ti_Tag = TAG_END;
	IIntuition->SetGadgetAttrsA((struct Gadget *)g.g[id], g.w, NULL, tags);
	if (text[0] == '\0') {
		struct StringInfo *si = (struct StringInfo *)((struct Gadget *)g.g[id])->SpecialInfo;

		if (strbufs[id] != NULL)
			strbufs[id][0] = '\0';
		if (si != NULL) {
			si->NumChars = 0;
			si->BufferPos = 0;
			si->DispPos = 0;
		}
	}
	if (g.w != NULL)
		IIntuition->RefreshGList((struct Gadget *)g.g[id], g.w, NULL, 1);
}

static void set_attrs(int id, uint32 tag, uint32 val)
{
	struct TagItem tags[2];

	tags[0].ti_Tag = tag;
	tags[0].ti_Data = val;
	tags[1].ti_Tag = TAG_END;
	if (IIntuition->SetGadgetAttrsA((struct Gadget *)g.g[id], g.w, NULL, tags) && g.w != NULL)
		IIntuition->RefreshGList((struct Gadget *)g.g[id], g.w, NULL, 1);
}

static const char *get_str(int id)
{
	STRPTR s = NULL;

	IIntuition->GetAttr(STRINGA_TextVal, g.g[id], (uint32 *)&s);
	return s != NULL ? (const char *)s : "";
}

static uint32 get_num(int id, uint32 tag)
{
	uint32 v = 0;

	IIntuition->GetAttr(tag, g.g[id], &v);
	return v;
}

/* --- the list ---------------------------------------------------------------- */

static void list_rebuild(void)
{
	int i;

	set_attrs(G_LIST, LISTBROWSER_Labels, (uint32)~0);
	IListBrowser->FreeListBrowserList(&g.labels);
	for (i = 0; i < g.sites.count; i++) {
		const site_cfg *s = &g.sites.site[i];
		char name[400], host[300], user[200], port[16];
		struct Node *n;

		to_local(s->name, name, sizeof(name));
		to_local(s->host, host, sizeof(host));
		to_local(s->user, user, sizeof(user));
		snprintf(port, sizeof(port), "%d", site_port(s));
		n = IListBrowser->AllocListBrowserNode(5,
			LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, name,
			LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, proto_labels[s->protocol],
			LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, host,
			LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, port,
			LBNA_Column, 4, LBNCA_CopyText, TRUE, LBNCA_Text, s->user[0] != '\0' ? user : "(anonymous)",
			TAG_END);
		if (n != NULL)
			IExec->AddTail(&g.labels, n);
	}
	{
		struct TagItem tags[4];

		tags[0].ti_Tag = LISTBROWSER_Labels;
		tags[0].ti_Data = (uint32)&g.labels;
		tags[1].ti_Tag = LISTBROWSER_Selected;
		tags[1].ti_Data = (uint32)g.sel;
		tags[2].ti_Tag = LISTBROWSER_MakeVisible;
		tags[2].ti_Data = (uint32)(g.sel >= 0 ? g.sel : 0);
		tags[3].ti_Tag = TAG_END;
		if (IIntuition->SetGadgetAttrsA((struct Gadget *)g.g[G_LIST], g.w, NULL, tags) && g.w != NULL)
			IIntuition->RefreshGList((struct Gadget *)g.g[G_LIST], g.w, NULL, 1);
	}
}

/* --- the form ------------------------------------------------------------------ */

static const int form_ids[] = {
	G_NAME, G_PROTO, G_HOST, G_PORT, G_USER, G_PASS, G_FOLDER, G_KEYFILE, G_KEYPICK,
	G_TEST, G_FORGET, G_DUP, G_REMOVE, 0
};

static void form_fill(void)
{
	/* one buffer per field: the gadgets may keep the pointer until they
	 * have copied the text */
	static char name[400], host[800], user[400], pass[800], folder[1600], keyfile[800];
	const site_cfg *s = g.sel >= 0 ? &g.sites.site[g.sel] : NULL;
	int i;

	for (i = 0; form_ids[i] != 0; i++)
		set_attrs(form_ids[i], GA_Disabled, s == NULL);
	if (s == NULL) {
		name[0] = host[0] = user[0] = pass[0] = folder[0] = keyfile[0] = '\0';
	} else {
		to_local(s->name, name, sizeof(name));
		to_local(s->host, host, sizeof(host));
		to_local(s->user, user, sizeof(user));
		to_local(s->password, pass, sizeof(pass));
		to_local(s->folder, folder, sizeof(folder));
		to_local(s->keyfile, keyfile, sizeof(keyfile));
		set_attrs(G_PROTO, CHOOSER_Selected, (uint32)s->protocol);
		set_attrs(G_PORT, INTEGER_Number, (uint32)site_port(s));
	}
	set_text(G_NAME, name);
	set_text(G_HOST, host);
	set_text(G_USER, user);
	set_text(G_PASS, pass);
	set_text(G_FOLDER, folder);
	set_text(G_KEYFILE, keyfile);
	if (s != NULL) {
		set_attrs(G_KEYFILE, GA_Disabled, s->protocol != PROTO_SFTP);
		set_attrs(G_KEYPICK, GA_Disabled, s->protocol != PROTO_SFTP);
		set_attrs(G_FORGET, GA_Disabled, s->protocol != PROTO_SFTP);
	}
}

/* Takes the form into the selected site. A bad name is refused (the old
 * one comes back) and 0 is returned. */
static int form_commit(void)
{
	site_cfg *s;
	char name[sizeof(s->name)];
	const char *lname, *problem;
	int k, proto;

	if (g.sel < 0)
		return 1;
	s = &g.sites.site[g.sel];
	lname = get_str(G_NAME);
	problem = site_name_problem(lname);
	to_utf8(lname, name, sizeof(name));
	if (problem == NULL && strlen(name) >= sizeof(s->name) - 1)
		problem = "The name is too long.";
	if (problem == NULL && (k = sites_find(&g.sites, name)) >= 0 && k != g.sel)
		problem = "Another site has that name.";
	proto = (int)get_num(G_PROTO, CHOOSER_Selected);
	s->protocol = proto == PROTO_SFTP ? PROTO_SFTP : PROTO_FTP;
	to_utf8(get_str(G_HOST), s->host, sizeof(s->host));
	s->port = (int)get_num(G_PORT, INTEGER_Number);
	if (s->port <= 0 || s->port > 65535)
		s->port = proto_default_port(s->protocol);
	to_utf8(get_str(G_USER), s->user, sizeof(s->user));
	to_utf8(get_str(G_PASS), s->password, sizeof(s->password));
	to_utf8(get_str(G_FOLDER), s->folder, sizeof(s->folder));
	to_utf8(get_str(G_KEYFILE), s->keyfile, sizeof(s->keyfile));
	if (problem != NULL) {
		char msg[300], old[400];

		to_local(s->name, old, sizeof(old));
		snprintf(msg, sizeof(msg), "\"%s\" cannot be used as a site name:\n%s", lname, problem);
		tell(msg);
		set_text(G_NAME, old);
		list_rebuild();
		return 0;
	}
	snprintf(s->name, sizeof(s->name), "%s", name);
	list_rebuild();
	return 1;
}

static void select_site(int i)
{
	g.sel = i >= 0 && i < g.sites.count ? i : -1;
	list_rebuild();
	form_fill();
}

/* "New site", "New site 2", ... */
static void unique_name(const char *base, char *out, size_t outlen)
{
	int n;

	snprintf(out, outlen, "%s", base);
	for (n = 2; sites_find(&g.sites, out) >= 0 && n < 1000; n++)
		snprintf(out, outlen, "%.90s %d", base, n);
}

static void add_site(const site_cfg *copy)
{
	site_cfg s;
	char base[200];
	int i;

	if (copy != NULL) {
		s = *copy;
		snprintf(base, sizeof(base), "%.90s copy", copy->name);
	} else {
		memset(&s, 0, sizeof(s));
		s.protocol = PROTO_SFTP;
		s.port = 22;
		snprintf(base, sizeof(base), "New site");
	}
	unique_name(base, s.name, sizeof(s.name));
	i = sites_add(&g.sites, &s);
	if (i < 0)
		return;
	select_site(i);
	if (ILayout != NULL)
		ILayout->ActivateLayoutGadget((struct Gadget *)g.g[0], g.w, NULL, (uint32)g.g[G_NAME]);
}

static void protocol_changed(void)
{
	site_cfg *s;
	int proto = (int)get_num(G_PROTO, CHOOSER_Selected);
	int port = (int)get_num(G_PORT, INTEGER_Number);

	if (g.sel < 0)
		return;
	s = &g.sites.site[g.sel];
	/* the port follows the protocol unless it was set to something else */
	if (port == proto_default_port(s->protocol) || port <= 0)
		set_attrs(G_PORT, INTEGER_Number, (uint32)proto_default_port(proto));
	form_commit();
	form_fill();
}

/* --- files ---------------------------------------------------------------- */

static int load_from(const char *dir, site_list *l, int quiet)
{
	char file[300], err[600], msg[800];
	int rc, skipped = 0;

	snprintf(file, sizeof(file), "%s/%s", dir, CFG_SITES);
	rc = sites_load(file, l, err, sizeof(err), &skipped);
	if (rc == -EINVAL && !quiet) {
		snprintf(msg, sizeof(msg), "The site list cannot be read:\n%s", err);
		tell(msg);
	} else if (rc == 0 && skipped > 0 && !quiet) {
		snprintf(msg, sizeof(msg), "%d sites in %s were left out:\n"
		         "bad or repeated names, or an unknown protocol.", skipped, file);
		tell(msg);
	}
	return rc;
}

static int save_to(const char *dir)
{
	char file[300], msg[500];
	int rc;

	mkdir(dir, 0755);
	snprintf(file, sizeof(file), "%s/%s", dir, CFG_SITES);
	rc = sites_save(file, &g.sites);
	if (rc != 0) {
		snprintf(msg, sizeof(msg), "Cannot save %s:\n%s", file, strerror(-rc));
		tell(msg);
	}
	return rc;
}

/* sites that the handler could not use: say so before saving */
static int check_all(void)
{
	char msg[600], name[400];
	int i;

	for (i = 0; i < g.sites.count; i++) {
		const site_cfg *s = &g.sites.site[i];

		to_local(s->name, name, sizeof(name));
		if (s->host[0] == '\0') {
			snprintf(msg, sizeof(msg), "The site \"%s\" has no server address.", name);
			tell(msg);
			select_site(i);
			return 0;
		}
		if (s->protocol == PROTO_SFTP && s->user[0] == '\0') {
			snprintf(msg, sizeof(msg), "The SFTP site \"%s\" has no user name.", name);
			tell(msg);
			select_site(i);
			return 0;
		}
	}
	return 1;
}

static void replace_sites(const site_list *from)
{
	int i;

	g.sites.count = 0;
	for (i = 0; i < from->count; i++)
		sites_add(&g.sites, &from->site[i]);
	select_site(g.sites.count > 0 ? 0 : -1);
}

/* --- host keys ---------------------------------------------------------------- */

static void forget_hostkey(void)
{
	const site_cfg *s;
	char file[300], tmp[310], spec[300], line[4096], msg[600], host[300];
	FILE *in, *out;
	int removed = 0;

	if (g.sel < 0 || !form_commit())
		return;
	s = &g.sites.site[g.sel];
	if (site_port(s) == 22)
		snprintf(spec, sizeof(spec), "%s", s->host);
	else
		snprintf(spec, sizeof(spec), "[%s]:%d", s->host, site_port(s));
	snprintf(file, sizeof(file), "%s/%s", ENVARC_DIR, CFG_KNOWNHOSTS);
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	to_local(spec, host, sizeof(host));
	in = fopen(file, "r");
	if (in == NULL) {
		snprintf(msg, sizeof(msg), "No host key of %s is saved.", host);
		tell(msg);
		return;
	}
	out = fopen(tmp, "w");
	if (out == NULL) {
		fclose(in);
		tell("Cannot write the host key file.");
		return;
	}
	while (fgets(line, sizeof(line), in) != NULL) {
		/* "name[,name...] keytype key [comment]" */
		size_t n = strcspn(line, " \t");
		char names[2048], kept[2048], *tok, *save = NULL;

		if (line[0] == '#' || n == 0 || n >= sizeof(names)) {
			fputs(line, out);
			continue;
		}
		memcpy(names, line, n);
		names[n] = '\0';
		kept[0] = '\0';
		for (tok = strtok_r(names, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
			if (strcasecmp(tok, spec) == 0) {
				removed++;
				continue;
			}
			if (kept[0] != '\0')
				strcat(kept, ",");
			strcat(kept, tok);
		}
		if (kept[0] != '\0')
			fprintf(out, "%s%s", kept, line + n);
	}
	fclose(in);
	fclose(out);
	if (removed > 0) {
		remove(file);
		rename(tmp, file);
		snprintf(msg, sizeof(msg), "The host key of %s was removed.\n"
		         "The next connection asks about the server's key again.", host);
	} else {
		remove(tmp);
		snprintf(msg, sizeof(msg), "No host key of %s is saved.", host);
	}
	tell(msg);
}

/* --- test connection ---------------------------------------------------------- */

struct count {
	int files;
	int dirs;
};

static int count_entry(void *ud, const char *name, const rstat *st)
{
	struct count *c = ud;

	(void)name;
	if (st->is_dir)
		c->dirs++;
	else
		c->files++;
	return 0;
}

static int64_t question_ms;   /* time spent on questions during a test */

static int prefs_hostkey(void *ud, const char *host, int port, const char *keytype,
                  const char *fingerprint, int mismatch)
{
	const site_cfg *s = ud;
	int64_t t = plat_ms();
	int answer = plat_ask_hostkey(s->name, host, port, keytype, fingerprint, mismatch);

	question_ms += plat_ms() - t;
	return answer;
}

/* a question of the login (a two-factor code), as the handler asks it */
static int prefs_ask(void *ud, const char *name, const char *instruction, const char *prompt,
                     int echo, char *buf, size_t buflen)
{
	const site_cfg *s = ud;
	int64_t t = plat_ms();
	int ok = plat_ask_answer(s->name, s->user, s->host, name, instruction, prompt, echo, NULL,
	                         buf, buflen);

	question_ms += plat_ms() - t;
	return ok;
}

static void test_connection(void)
{
	site_cfg s;
	remote_env env;
	remote *r = NULL;
	struct count c;
	char err[700], msg[1600], local[1000], typed[256], kh[300];
	const char *pass;
	int rc, fail, remember = 0, asked = 0;
	int64_t t0;

	if (g.sel < 0 || !form_commit())
		return;
	s = g.sites.site[g.sel];
	if (s.host[0] == '\0') {
		tell("Enter the server's address first.");
		return;
	}
	if (s.protocol == PROTO_SFTP && s.user[0] == '\0') {
		tell("SFTP needs a user name.");
		return;
	}
	if (s.protocol == PROTO_SFTP && g.amissl == 0) {
		g.amissl = amissl_open() == 0 && sftp_global_init() == 0 ? 1 : -1;
	}
	if (s.protocol == PROTO_SFTP && g.amissl < 0) {
		tell("SFTP needs AmiSSL 5 (5.1 or newer) or AmiSSL 6\n(and a TCP/IP stack).\n"
		     "AmiSSL: OS4Depot library/misc/amissl.lha");
		return;
	}
	pass = s.password;
	typed[0] = '\0';
	if (s.user[0] != '\0' && s.password[0] == '\0' &&
	    !(s.protocol == PROTO_SFTP && s.keyfile[0] != '\0')) {
		if (!plat_ask_password(s.name, s.user, s.host, NULL, NULL, typed, sizeof(typed), &remember))
			return;
		pass = typed;
		asked = 1;
	}
	memset(&env, 0, sizeof(env));
	mkdir(ENVARC_DIR, 0755);
	snprintf(kh, sizeof(kh), "%s/%s", ENVARC_DIR, CFG_KNOWNHOSTS);
	env.known_hosts = kh;
	env.connect_timeout_ms = 15000;
	env.io_timeout_ms = 20000;
	/* the same questions as the handler asks */
	env.hostkey = prefs_hostkey;
	env.ask = prefs_ask;
	env.ud = &s;
	busy(1);
	question_ms = 0;
	t0 = plat_ms();
	if (s.protocol == PROTO_SFTP)
		rc = sftp_connect(&s, pass, &env, &r, err, sizeof(err), &fail);
	else
		rc = ftp_connect(&s, pass, &env, &r, err, sizeof(err), &fail);
	memset(&c, 0, sizeof(c));
	if (rc == 0) {
		rc = r->ops->list(r, "/", count_entry, &c);
		if (rc != 0)
			snprintf(err, sizeof(err), "The folder cannot be listed: %s",
			         r->error[0] != '\0' ? r->error : strerror(-rc));
	}
	busy(0);
	t0 += question_ms;
	if (rc == 0) {
		snprintf(msg, sizeof(msg),
		         "The connection works.\n\n"
		         "Server: %s, port %d (%s)\n"
		         "User: %s\n"
		         "Folder: %s\n"
		         "Contents: %d folders, %d files\n"
		         "Time: %d.%d s",
		         s.host, site_port(&s), proto_labels[s.protocol],
		         s.user[0] != '\0' ? s.user : "anonymous",
		         r->home[0] != '\0' ? r->home : "/", c.dirs, c.files,
		         (int)((plat_ms() - t0) / 1000), (int)((plat_ms() - t0) / 100 % 10));
		if (asked && remember) {
			snprintf(g.sites.site[g.sel].password, sizeof(s.password), "%s", typed);
			form_fill();
		}
	} else {
		snprintf(msg, sizeof(msg), "The connection failed.\n\n%s", err);
	}
	if (r != NULL)
		r->ops->close(r);
	memset(typed, 0, sizeof(typed));
	to_local_msg(msg, local, sizeof(local));
	tell(local);
}

/* --- window ---------------------------------------------------------------- */

static Object *button(int id, const char *text)
{
	return g.g[id] = IIntuition->NewObject(BtnCls, NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		GA_Text, text,
		TAG_END);
}

/* every string gadget gets buffers of its own: the default ones hold only
 * 128 bytes */

static Object *string(int id, int max, int password)
{
	char *b = calloc(3, (size_t)max + 1);

	if (b == NULL)
		return NULL;
	strbufs[id] = b;
	strsizes[id] = 3 * ((size_t)max + 1);
	return g.g[id] = IIntuition->NewObject(StrCls, NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		GA_TabCycle, TRUE,
		STRINGA_MaxChars, max + 1,
		STRINGA_Buffer, b,
		STRINGA_UndoBuffer, b + max + 1,
		STRINGA_WorkBuffer, b + 2 * (max + 1),
		password ? STRINGA_HookType : TAG_IGNORE, SHK_PASSWORD,
		TAG_END);
}

static Object *label(const char *text)
{
	return IIntuition->NewObject(LblCls, NULL, LABEL_Text, text, TAG_END);
}

static int make_window(void)
{
	Object *top;

	g.win = IIntuition->NewObject(WinCls, NULL,
		WA_Title, "sftp-handler sites",
		WA_ScreenTitle, SFH_VERS,
		WA_Activate, TRUE,
		WA_DragBar, TRUE,
		WA_DepthGadget, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_InnerWidth, 640,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_NewMenu, menus,
		WINDOW_Layout, top = g.g[0] = IIntuition->NewObject(LayCls, NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
			LAYOUT_SpaceOuter, TRUE,
			LAYOUT_DeferLayout, TRUE,

			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
				LAYOUT_BevelStyle, BVS_GROUP,
				LAYOUT_Label, "Sites",
				LAYOUT_AddChild, g.g[G_LIST] = IIntuition->NewObject(LbCls, NULL,
					GA_ID, G_LIST,
					GA_RelVerify, TRUE,
					LISTBROWSER_ColumnInfo, columns,
					LISTBROWSER_ColumnTitles, TRUE,
					LISTBROWSER_ShowSelected, TRUE,
					LISTBROWSER_Labels, &g.labels,
					TAG_END),
				CHILD_MinHeight, 140,
				CHILD_MinWidth, 600,
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_EvenSize, TRUE,
					LAYOUT_AddChild, button(G_ADD, "_Add"),
					LAYOUT_AddChild, button(G_DUP, "_Duplicate"),
					LAYOUT_AddChild, button(G_REMOVE, "Re_move"),
					TAG_END),
				CHILD_WeightedHeight, 0,
				TAG_END),

			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
				LAYOUT_BevelStyle, BVS_GROUP,
				LAYOUT_Label, "Site",
				LAYOUT_AddChild, string(G_NAME, SITE_NAME_MAX, 0),
				CHILD_Label, label("_Name"),
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_AddChild, g.g[G_PROTO] = IIntuition->NewObject(ChoCls, NULL,
						GA_ID, G_PROTO,
						GA_RelVerify, TRUE,
						CHOOSER_PopUp, TRUE,
						CHOOSER_LabelArray, proto_labels,
						CHOOSER_Selected, 1,
						TAG_END),
					CHILD_WeightedWidth, 0,
					LAYOUT_AddChild, string(G_HOST, 255, 0),
					CHILD_Label, label("_Server"),
					LAYOUT_AddChild, g.g[G_PORT] = IIntuition->NewObject(IntCls, NULL,
						GA_ID, G_PORT,
						GA_RelVerify, TRUE,
						GA_TabCycle, TRUE,
						INTEGER_Minimum, 1,
						INTEGER_Maximum, 65535,
						INTEGER_Number, 22,
						INTEGER_MaxChars, 5,
						INTEGER_Arrows, FALSE,
						TAG_END),
					CHILD_Label, label("_Port"),
					CHILD_WeightedWidth, 15,
					TAG_END),
				CHILD_Label, label("Pro_tocol"),
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_AddChild, string(G_USER, 127, 0),
					LAYOUT_AddChild, string(G_PASS, 255, 1),
					CHILD_Label, label("Pass_word"),
					TAG_END),
				CHILD_Label, label("_User"),
				LAYOUT_AddChild, string(G_FOLDER, 511, 0),
				CHILD_Label, label("_Folder"),
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_AddChild, string(G_KEYFILE, 255, 0),
					LAYOUT_AddChild, button(G_KEYPICK, "..."),
					CHILD_WeightedWidth, 0,
					TAG_END),
				CHILD_Label, label("_Key file"),
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_EvenSize, TRUE,
					LAYOUT_AddChild, button(G_TEST, "_Test connection"),
					LAYOUT_AddChild, button(G_FORGET, "Forget _host key"),
					TAG_END),
				CHILD_WeightedHeight, 0,
				TAG_END),
			CHILD_WeightedHeight, 0,

			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				LAYOUT_EvenSize, TRUE,
				LAYOUT_AddChild, button(G_SAVE, "Sa_ve"),
				LAYOUT_AddChild, button(G_USE, "U_se"),
				LAYOUT_AddChild, button(G_CANCEL, "_Cancel"),
				TAG_END),
			CHILD_WeightedHeight, 0,
			TAG_END),
		TAG_END);
	if (g.win == NULL)
		return 0;
	(void)top;
	g.w = (struct Window *)IIntuition->IDoMethod(g.win, WM_OPEN, NULL);
	return g.w != NULL;
}

static void pick_keyfile(void)
{
	struct FileRequester *fr;
	char path[600], drawer[300];
	const char *file;

	if (IAsl == NULL)
		return;
	snprintf(drawer, sizeof(drawer), "%s", get_str(G_KEYFILE));
	file = drawer[0] != '\0' ? (const char *)IDOS->FilePart(drawer) : "";
	snprintf(path, sizeof(path), "%s", file);
	if (drawer[0] != '\0')
		*(STRPTR)IDOS->PathPart(drawer) = '\0';
	else
		snprintf(drawer, sizeof(drawer), "%s", ENVARC_DIR);
	fr = IAsl->AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, "Private key file (OpenSSH format)",
		ASLFR_InitialDrawer, drawer,
		ASLFR_InitialFile, path,
		TAG_END);
	if (fr == NULL)
		return;
	if (IAsl->AslRequestTags(fr, ASLFR_Window, g.w, TAG_END) &&
	    fr->fr_File != NULL && fr->fr_File[0] != '\0') {
		snprintf(path, sizeof(path), "%s", fr->fr_Drawer != NULL ? (const char *)fr->fr_Drawer : "");
		IDOS->AddPart(path, fr->fr_File, sizeof(path));
		set_text(G_KEYFILE, path);
		form_commit();
	}
	IAsl->FreeAslRequest(fr);
}

static void about(void)
{
	tell(SFH_VERS " (" SFH_DATE ")\n\n"
	     "sftphprefs keeps the list of FTP and SFTP sites that\n"
	     "sftp-handler shows as folders (in ENV:SFTPHandler).\n\n"
	     "Public domain. Uses libssh2 (BSD licence) and AmiSSL.");
}

/* returns 1 when the program should end */
static int handle_menu(uint16 code)
{
	while (code != MENUNULL) {
		struct MenuItem *item = IIntuition->ItemAddress(g.w->MenuStrip, code);
		site_list l;

		if (item == NULL)
			break;
		switch ((uint32)GTMENUITEM_USERDATA(item)) {
		case M_ABOUT:
			about();
			break;
		case M_QUIT:
			return 1;
		case M_RESET:
			g.sites.count = 0;
			select_site(-1);
			break;
		case M_LASTSAVED:
			sites_init(&l);
			if (load_from(ENVARC_DIR, &l, 0) == 0 || l.count == 0)
				replace_sites(&l);
			sites_free(&l);
			break;
		case M_RESTORE:
			replace_sites(&g.start);
			break;
		}
		code = item->NextSelect;
	}
	return 0;
}

static void event_loop(void)
{
	uint32 sigmask = 0, result;
	uint16 code;
	int done = 0;

	IIntuition->GetAttr(WINDOW_SigMask, g.win, &sigmask);
	while (!done) {
		uint32 sigs = IExec->Wait(sigmask | SIGBREAKF_CTRL_C);

		if (sigs & SIGBREAKF_CTRL_C)
			break;
		while ((result = IIntuition->IDoMethod(g.win, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG) {
			switch (result & WMHI_CLASSMASK) {
			case WMHI_CLOSEWINDOW:
				done = 1;
				break;
			case WMHI_MENUPICK:
				if (handle_menu(code))
					done = 1;
				break;
			case WMHI_GADGETUP:
				switch (result & WMHI_GADGETMASK) {
				case G_LIST: {
					int i = (int)get_num(G_LIST, LISTBROWSER_Selected);

					if (i != g.sel) {
						form_commit();
						select_site(i);
					}
					break;
				}
				case G_ADD:
					if (form_commit())
						add_site(NULL);
					break;
				case G_DUP:
					if (g.sel >= 0 && form_commit())
						add_site(&g.sites.site[g.sel]);
					break;
				case G_REMOVE:
					if (g.sel >= 0) {
						int i = g.sel;

						sites_remove(&g.sites, i);
						select_site(i < g.sites.count ? i : g.sites.count - 1);
					}
					break;
				case G_PROTO:
					protocol_changed();
					break;
				case G_NAME: case G_HOST: case G_PORT: case G_USER:
				case G_PASS: case G_FOLDER: case G_KEYFILE:
					form_commit();
					break;
				case G_KEYPICK:
					pick_keyfile();
					break;
				case G_TEST:
					test_connection();
					break;
				case G_FORGET:
					forget_hostkey();
					break;
				case G_SAVE:
					if (form_commit() && check_all() &&
					    save_to(ENVARC_DIR) == 0 && save_to(ENV_DIR) == 0)
						done = 1;
					break;
				case G_USE:
					if (form_commit() && check_all() && save_to(ENV_DIR) == 0)
						done = 1;
					break;
				case G_CANCEL:
					done = 1;
					break;
				}
				break;
			}
		}
	}
}

static int open_libs(void)
{
	if (!req_open_libs())
		return 0;
	WinLib = IIntuition->OpenClass("window.class", 53, &WinCls);
	LayLib = IIntuition->OpenClass("gadgets/layout.gadget", 53, &LayCls);
	BtnLib = IIntuition->OpenClass("gadgets/button.gadget", 53, &BtnCls);
	StrLib = IIntuition->OpenClass("gadgets/string.gadget", 53, &StrCls);
	IntLib = IIntuition->OpenClass("gadgets/integer.gadget", 53, &IntCls);
	ChoLib = IIntuition->OpenClass("gadgets/chooser.gadget", 53, &ChoCls);
	LbLib  = IIntuition->OpenClass("gadgets/listbrowser.gadget", 53, &LbCls);
	LblLib = IIntuition->OpenClass("images/label.image", 53, &LblCls);
	if (LbLib != NULL)
		IListBrowser = (struct ListBrowserIFace *)IExec->GetInterface((struct Library *)LbLib,
		                                                               "main", 1, NULL);
	AslBase = IExec->OpenLibrary("asl.library", 53);
	if (AslBase != NULL)
		IAsl = (struct AslIFace *)IExec->GetInterface(AslBase, "main", 1, NULL);
	return WinCls != NULL && LayCls != NULL && BtnCls != NULL && StrCls != NULL &&
	       IntCls != NULL && ChoCls != NULL && LbCls != NULL && LblCls != NULL &&
	       IListBrowser != NULL;
}

static void close_libs(void)
{
	if (IAsl != NULL)
		IExec->DropInterface((struct Interface *)IAsl);
	if (AslBase != NULL)
		IExec->CloseLibrary(AslBase);
	if (IListBrowser != NULL)
		IExec->DropInterface((struct Interface *)IListBrowser);
	if (IIntuition != NULL) {
		IIntuition->CloseClass(LblLib);
		IIntuition->CloseClass(LbLib);
		IIntuition->CloseClass(ChoLib);
		IIntuition->CloseClass(IntLib);
		IIntuition->CloseClass(StrLib);
		IIntuition->CloseClass(BtnLib);
		IIntuition->CloseClass(LayLib);
		IIntuition->CloseClass(WinLib);
	}
	req_close_libs();
}

int main(void)
{
	int rc = RETURN_OK;

	memset(&g, 0, sizeof(g));
	g.sel = -1;
	IExec->NewList(&g.labels);
	g.cs = cs_from_name(getenv("CHARSET"));
	plat_charset = g.cs;
	sites_init(&g.sites);
	sites_init(&g.start);
	if (!open_libs()) {
		printf(PROGNAME ": cannot open the ReAction classes it needs.\n");
		close_libs();
		return RETURN_FAIL;
	}
	if (load_from(ENV_DIR, &g.sites, 1) == -ENOENT)
		load_from(ENVARC_DIR, &g.sites, 1);
	{
		int i;

		for (i = 0; i < g.sites.count; i++)
			sites_add(&g.start, &g.sites.site[i]);
	}
	if (make_window()) {
		select_site(g.sites.count > 0 ? 0 : -1);
		/* complaints about the file only now that there is a window */
		{
			site_list l;

			sites_init(&l);
			if (load_from(ENV_DIR, &l, 0) == -ENOENT)
				load_from(ENVARC_DIR, &l, 0);
			sites_free(&l);
		}
		event_loop();
	} else {
		printf(PROGNAME ": cannot open its window.\n");
		rc = RETURN_FAIL;
	}
	if (g.win != NULL)
		IIntuition->DisposeObject(g.win);
	{
		int i;

		for (i = 0; i < G_COUNT; i++) {
			if (strbufs[i] != NULL)
				memset(strbufs[i], 0, strsizes[i]);   /* passwords */
			free(strbufs[i]);
		}
	}
	IListBrowser->FreeListBrowserList(&g.labels);
	memset(g.sites.site, 0, g.sites.site != NULL ? (size_t)g.sites.cap * sizeof(site_cfg) : 0);
	sites_free(&g.sites);
	sites_free(&g.start);
	if (g.amissl > 0) {
		sftp_global_exit();
		amissl_close();
	}
	close_libs();
	return rc;
}
