/*
 * Host-side stand-ins for the AmigaOS calls amigafs.c makes.
 *
 * The NDK headers are real -- they come from the vbcc image and describe the
 * genuine structures, so the code under test sees the same DosPacket, FileLock
 * and DeviceList layouts it does on a real Amiga. Only the twelve entry points
 * below are faked, and all of them are message-port or DOS-list plumbing.
 *
 * Must be compiled -m32: BPTR arithmetic shifts pointers right by two, which
 * silently truncates a 64-bit address.
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Message ports
 * ------------------------------------------------------------------------ */

struct MsgPort *CreateMsgPort(void)
{
	struct MsgPort *port = (struct MsgPort *)calloc(1, sizeof(struct MsgPort));

	if (!port)
		return NULL;

	/* A signal bit is meaningless here, but the target ORs it into a mask. */
	port->mp_SigBit = 1;
	port->mp_MsgList.lh_Head = (struct Node *)&port->mp_MsgList.lh_Tail;
	port->mp_MsgList.lh_Tail = NULL;
	port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;
	return port;
}

void DeleteMsgPort(struct MsgPort *port)
{
	free(port);
}

void PutMsg(struct MsgPort *port, struct Message *message)
{
	struct Node *node = (struct Node *)message;
	struct Node *tail_pred = port->mp_MsgList.lh_TailPred;

	node->ln_Succ = (struct Node *)&port->mp_MsgList.lh_Tail;
	node->ln_Pred = tail_pred;
	tail_pred->ln_Succ = node;
	port->mp_MsgList.lh_TailPred = node;
}

struct Message *GetMsg(struct MsgPort *port)
{
	struct Node *node = port->mp_MsgList.lh_Head;

	if (!node || !node->ln_Succ)
		return NULL; /* empty list */

	port->mp_MsgList.lh_Head = node->ln_Succ;
	node->ln_Succ->ln_Pred = (struct Node *)&port->mp_MsgList.lh_Head;
	return (struct Message *)node;
}

/* ------------------------------------------------------------------------
 * DOS device list
 *
 * A flat array is plenty: a test mounts one device.
 * ------------------------------------------------------------------------ */

#define MAX_DOS_ENTRIES 8

static struct DosList *dos_entries[MAX_DOS_ENTRIES];

struct DosList *MakeDosEntry(CONST_STRPTR name, LONG type)
{
	struct DosList *entry = (struct DosList *)calloc(1, sizeof(struct DeviceList));
	size_t length = strlen((const char *)name);
	char *bstr = (char *)calloc(1, length + 2);

	if (!entry || !bstr)
	{
		free(entry);
		free(bstr);
		return NULL;
	}

	/* DOS list names are BSTRs: length byte, then the characters. */
	bstr[0] = (char)length;
	memcpy(bstr + 1, name, length);

	entry->dol_Name = MKBADDR(bstr);
	entry->dol_Type = type;
	return entry;
}

LONG FreeDosEntry(struct DosList *entry)
{
	if (entry)
	{
		free(BADDR(entry->dol_Name));
		free(entry);
	}
	return DOSTRUE;
}

LONG AddDosEntry(struct DosList *entry)
{
	int i;

	for (i = 0; i < MAX_DOS_ENTRIES; ++i)
	{
		if (!dos_entries[i])
		{
			dos_entries[i] = entry;
			return DOSTRUE;
		}
	}
	return DOSFALSE;
}

LONG RemDosEntry(struct DosList *entry)
{
	int i;

	for (i = 0; i < MAX_DOS_ENTRIES; ++i)
	{
		if (dos_entries[i] == entry)
		{
			dos_entries[i] = NULL;
			return DOSTRUE;
		}
	}
	return DOSFALSE;
}

struct DosList *FindDosEntry(const struct DosList *start, CONST_STRPTR name, ULONG flags)
{
	int i;

	(void)start;
	(void)flags;

	for (i = 0; i < MAX_DOS_ENTRIES; ++i)
	{
		const char *bstr;

		if (!dos_entries[i])
			continue;

		bstr = (const char *)BADDR(dos_entries[i]->dol_Name);
		if (bstr && (size_t)bstr[0] == strlen((const char *)name) &&
			0 == memcmp(bstr + 1, name, (size_t)bstr[0]))
		{
			return dos_entries[i];
		}
	}
	return NULL;
}

/* Nothing else is running, so the DOS list needs no locking. */
struct DosList *AttemptLockDosList(ULONG flags) { (void)flags; return (struct DosList *)dos_entries; }
void UnLockDosList(ULONG flags) { (void)flags; }

void Delay(ULONG ticks) { (void)ticks; }

/* Reset between tests so a leaked entry cannot leak into the next one. */
void rl_test_reset_dos_entries(void)
{
	memset(dos_entries, 0, sizeof(dos_entries));
}
