/* sftp-handler - requesters (ReAction). Public domain. */
#include "os4.h"

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/layout.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/string.h>
#include <gadgets/button.h>
#include <gadgets/checkbox.h>
#include <images/label.h>
#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>

#include <stdio.h>
#include <string.h>

struct IntuitionIFace *IIntuition;
struct Library *IntuitionBase;
struct LayoutIFace *ILayout;

static struct ClassLibrary *WinLib, *LayLib, *StrLib, *BtnLib,
                           *ChkLib, *LblLib;
static Class *WinCls, *LayCls, *StrCls, *BtnCls,
             *ChkCls, *LblCls;

int req_open_libs(void)
{
	if (IIntuition != NULL)
		return 1;
	IntuitionBase = IExec->OpenLibrary("intuition.library", 53);
	if (IntuitionBase == NULL)
		return 0;
	IIntuition = (struct IntuitionIFace *)IExec->GetInterface(IntuitionBase, "main", 1, NULL);
	if (IIntuition == NULL) {
		IExec->CloseLibrary(IntuitionBase);
		IntuitionBase = NULL;
		return 0;
	}
	WinLib   = IIntuition->OpenClass("window.class", 53, &WinCls);
	LayLib   = IIntuition->OpenClass("gadgets/layout.gadget", 53, &LayCls);
	StrLib   = IIntuition->OpenClass("gadgets/string.gadget", 53, &StrCls);
	BtnLib   = IIntuition->OpenClass("gadgets/button.gadget", 53, &BtnCls);
	ChkLib = IIntuition->OpenClass("gadgets/checkbox.gadget", 53, &ChkCls);
	LblLib    = IIntuition->OpenClass("images/label.image", 53, &LblCls);
	if (LayLib != NULL)
		ILayout = (struct LayoutIFace *)IExec->GetInterface((struct Library *)LayLib,
		                                                     "main", 1, NULL);
	return 1;
}

void req_close_libs(void)
{
	if (IIntuition == NULL)
		return;
	if (ILayout != NULL)
		IExec->DropInterface((struct Interface *)ILayout);
	ILayout = NULL;
	IIntuition->CloseClass(LblLib);
	IIntuition->CloseClass(ChkLib);
	IIntuition->CloseClass(BtnLib);
	IIntuition->CloseClass(StrLib);
	IIntuition->CloseClass(LayLib);
	IIntuition->CloseClass(WinLib);
	IExec->DropInterface((struct Interface *)IIntuition);
	IExec->CloseLibrary(IntuitionBase);
	IIntuition = NULL;
	IntuitionBase = NULL;
}

void req_message(const char *title, const char *text)
{
	struct EasyStruct es;

	if (!req_open_libs())
		return;
	memset(&es, 0, sizeof(es));
	es.es_StructSize = sizeof(es);
	es.es_Title = (STRPTR)title;
	/* the text goes in as an argument, so '%' in it is harmless */
	es.es_TextFormat = (STRPTR)"%s";
	es.es_GadgetFormat = (STRPTR)"OK";
	IIntuition->EasyRequest(NULL, &es, NULL, text);
}

enum { GID_PASSWORD = 1, GID_REMEMBER, GID_OK, GID_CANCEL };

int req_choice(const char *title, const char *text, const char *buttons)
{
	struct EasyStruct es;

	if (!req_open_libs())
		return 0;
	memset(&es, 0, sizeof(es));
	es.es_StructSize = sizeof(es);
	es.es_Title = (STRPTR)title;
	es.es_TextFormat = (STRPTR)"%s";
	es.es_GadgetFormat = (STRPTR)buttons;
	return (int)IIntuition->EasyRequest(NULL, &es, NULL, text);
}

/* A window with a text, a string gadget (hidden: shows dots), with
 * remember != NULL a "Remember" checkbox, and OK/Cancel. */
static int ask_window(const char *title, const char *text, int hidden, const char *ok_text,
                      char *buf, size_t buflen, int *remember)
{
	Object *win, *layout, *str, *chk = NULL;
	struct Window *w;
	uint32 sigmask = 0, result;
	uint16 code;
	int done = 0, ok = 0;

	buf[0] = '\0';
	if (remember != NULL)
		*remember = 0;
	if (!req_open_libs() || WinCls == NULL || LayCls == NULL ||
	    StrCls == NULL || BtnCls == NULL || ChkCls == NULL ||
	    LblCls == NULL)
		return 0;

	win = IIntuition->NewObject(WinCls, NULL,
		WA_Title, title,
		WA_Activate, TRUE,
		WA_DragBar, TRUE,
		WA_DepthGadget, TRUE,
		WA_CloseGadget, TRUE,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_Layout, layout = IIntuition->NewObject(LayCls, NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
			LAYOUT_SpaceOuter, TRUE,
			LAYOUT_AddImage, IIntuition->NewObject(LblCls, NULL,
				LABEL_Text, text,
				TAG_END),
			LAYOUT_AddChild, str = IIntuition->NewObject(StrCls, NULL,
				GA_ID, GID_PASSWORD,
				GA_RelVerify, TRUE,
				GA_TabCycle, TRUE,
				STRINGA_MaxChars, 255,
				hidden ? STRINGA_HookType : TAG_IGNORE, SHK_PASSWORD,
				TAG_END),
			CHILD_MinWidth, 300,
			remember != NULL ? LAYOUT_AddChild : TAG_IGNORE,
			remember != NULL ? (chk = IIntuition->NewObject(ChkCls, NULL,
				GA_ID, GID_REMEMBER,
				GA_Text, "_Remember the password (saved with the site)",
				CHECKBOX_Checked, FALSE,
				TAG_END)) : NULL,
			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				LAYOUT_EvenSize, TRUE,
				LAYOUT_AddChild, IIntuition->NewObject(BtnCls, NULL,
					GA_ID, GID_OK,
					GA_Text, ok_text,
					GA_RelVerify, TRUE,
					TAG_END),
				LAYOUT_AddChild, IIntuition->NewObject(BtnCls, NULL,
					GA_ID, GID_CANCEL,
					GA_Text, "C_ancel",
					GA_RelVerify, TRUE,
					TAG_END),
				TAG_END),
			CHILD_WeightedHeight, 0,
			TAG_END),
		TAG_END);
	if (win == NULL)
		return 0;

	w = (struct Window *)IIntuition->IDoMethod(win, WM_OPEN, NULL);
	if (w == NULL) {
		IIntuition->DisposeObject(win);
		return 0;
	}
	if (ILayout != NULL)
		ILayout->ActivateLayoutGadget((struct Gadget *)layout, w, NULL, (uint32)str);
	IIntuition->GetAttr(WINDOW_SigMask, win, &sigmask);

	while (!done) {
		IExec->Wait(sigmask);
		while ((result = IIntuition->IDoMethod(win, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG) {
			switch (result & WMHI_CLASSMASK) {
			case WMHI_CLOSEWINDOW:
				done = 1;
				break;
			case WMHI_GADGETUP:
				switch (result & WMHI_GADGETMASK) {
				case GID_PASSWORD: /* Return in the string gadget */
				case GID_OK:
					ok = 1;
					done = 1;
					break;
				case GID_CANCEL:
					done = 1;
					break;
				}
				break;
			}
		}
	}

	if (ok) {
		STRPTR val = NULL;
		uint32 sel = 0;

		IIntuition->GetAttr(STRINGA_TextVal, str, (uint32 *)&val);
		snprintf(buf, buflen, "%s", val != NULL ? (const char *)val : "");
		if (remember != NULL) {
			IIntuition->GetAttr(GA_Selected, chk, &sel);
			*remember = sel != 0;
		}
	}
	IIntuition->DisposeObject(win);
	return ok;
}

int req_password(const char *title, const char *text, char *buf, size_t buflen,
                 int *remember)
{
	return ask_window(title, text, 1, "_Connect", buf, buflen, remember);
}

int req_answer(const char *title, const char *text, int hidden, char *buf, size_t buflen)
{
	return ask_window(title, text, hidden, "_OK", buf, buflen, NULL);
}
