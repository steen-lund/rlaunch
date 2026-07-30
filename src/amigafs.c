#include "config.h"

#ifndef RL_AMIGA
#error "This is an Amiga source file"
#endif

#include "amigafs.h"
#include "util.h"
#include "rlnet.h"
#include "peer.h"
#include "protocol.h"
#include "version.h"

#define BSTR_LEN(str) (((const rl_uint8 *)str)[0])
#define BSTR_PTR(str) (((const char *)str)+1)

#define BCPL_CAST(ptr_type, expression) ((ptr_type *) (((LONG)expression) << 2))

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/execbase.h>

#define RL_AMIGA_PATH_MAX 108

#define HANDLE_FROM_LOCK(lock) ((rl_client_handle_t*) (lock)->fl_Key)

/* The id every root lock starts out with: it names the server's own root
 * handle, which no client owns and which must never be closed. */
#define RL_ROOT_HANDLE_ID ((rl_uint32) -1)

#define IS_ROOT_HANDLE(handle) (RL_HANDLE_DEVICE == (handle)->type)

static LONG translate_error_code(rl_uint32 error_code);
static LONG failed_res1(const rl_pending_operation_t *op);
static const char* get_packet_type_name(const struct DosPacket* packet);
static void construct_bstr(TEXT *start, size_t max_size, const char *input);

static const char *rl_client_handle_type_name(rl_client_handle_type_t type)
{
	switch (type)
	{
		case RL_HANDLE_FILE: return "file";
		case RL_HANDLE_DIR: return "directory";
		case RL_HANDLE_DEVICE: return "device";
		case RL_HANDLE_VIRTUAL_INPUT: return "virtual input";
		case RL_HANDLE_VIRTUAL_OUTPUT: return "virtual output";
		default:
							   return "<bogus>";
	}
}

/*
 * Given an input path and an optional parent directory, compute the
 * corresponding absolute path on the server.
 *
 * Returns 0 on success, or a DOS error code if the name does not fit. Names
 * are rejected rather than truncated: a truncated path would silently resolve
 * to the wrong file on the server.
 */
static LONG bstr_to_cstr(char *buffer, size_t buffer_size, const void *bstr)
{
	/* A BSTR length byte goes up to 255, so it need not fit the buffer. */
	size_t name_len = BSTR_LEN(bstr);

	if (name_len >= buffer_size)
		return ERROR_LINE_TOO_LONG;

	rl_memcpy(buffer, BSTR_PTR(bstr), name_len);
	buffer[name_len] = '\0';
	return 0;
}

static LONG normalize_object_path(
	rl_amigafs_t *fs,
	char *buffer,
	size_t buffer_size,
	struct FileLock *dir_lock,
	const void *object_name_bstr)
{
	char item_path[RL_AMIGA_PATH_MAX];
	rl_client_handle_t *handle;
	LONG error_code;

	/*
	 * Establish a node to start "locating" from--if we don't have a lock,
	 * start from the root
	 */
	if (!dir_lock)
		handle = &fs->root_handle;
	else
		handle = HANDLE_FROM_LOCK(dir_lock);

	if (0 != (error_code = bstr_to_cstr(item_path, sizeof(item_path), object_name_bstr)))
		return error_code;

	/*
	 * If the name is absolute (starts with a device name followed by a
	 * colon, or a lone colon) we override the start location to be the root.
	 */
	{
		const char *colon_pos = rl_strchr(item_path, ':');
		if (colon_pos)
		{
			rl_memmove(item_path, colon_pos+1, rl_strlen(colon_pos+1)+1);
			handle = &fs->root_handle;
		}
	}

	if (!IS_ROOT_HANDLE(handle))
	{
		/* Joining adds a separator, so check the total before formatting:
		 * rl_format_msg truncates silently. */
		if (rl_strlen(handle->path) + 1 + rl_strlen(item_path) >= buffer_size)
			return ERROR_LINE_TOO_LONG;

		RL_LOG_DEBUG(("normalize: path '%s' relative to parent '%s'", item_path, handle->path));
		rl_format_msg(buffer, buffer_size, "%s/%s", handle->path, item_path);
	}
	else
	{
		if (rl_strlen(item_path) >= buffer_size)
			return ERROR_LINE_TOO_LONG;

		rl_format_msg(buffer, buffer_size, "%s", item_path);
	}

	return 0;
}

static void dump_pending_ops(rl_amigafs_t *fs)
{
	int index = 0;
	rl_pending_operation_t *op;

	if (!(RL_DEBUG & rl_log_bits))
		return;

	rl_log_message("Pending ops against %s: ", fs->peer->ident);

	for (op = fs->pending; op; op = op->next, ++index)
	{
		rl_log_message("%d: [%u: %s] ", index, op->request_seqno, rl_msg_name(op->expected_answer_type));
	}
}

static rl_pending_operation_t *alloc_pending(rl_amigafs_t *self, struct DosPacket *packet, rl_msg_kind_t expected_answer_type, rl_completion_callback_fn_t callback)
{
	rl_pending_operation_t *op;

	op = (rl_pending_operation_t *)
		RL_ALLOC_TYPED_ZERO(rl_pending_operation_t);

	if (!op)
		return NULL;

	op->request_seqno = self->seqno++;
	op->next = self->pending;
	op->input_packet = packet;
	op->expected_answer_type = expected_answer_type;
	op->callback = callback;

	self->pending = op;

	dump_pending_ops(self);
	return op;
}

static void construct_bstr(TEXT *start, size_t max_size, const char *input)
{
	size_t len = rl_strlen(input);

	/* Reserve space for the size byte, and the trailing null termination for
	 * certain DOS BSTRs. size_t throughout: a LONG max_size promoted to
	 * unsigned in the clamp below, so the guard only looked like it worked. */
	const size_t avail = max_size > 2 ? max_size - 2 : 0;

	if (len > avail)
		len = avail;

	*start++ = (TEXT) len;

	while(len--)
	{
		*start++ = *input++;
	}

	*start = '\0';
}

/*
 * Handles keep the whole path so relative lookups can be built from it, but
 * fib_FileName is only the final component: a caller rebuilding a path from a
 * parent plus what Examine() gave it would otherwise repeat the directories.
 */
static const char *final_component(const char *path)
{
	const char *name = path, *cursor;

	for (cursor = path; *cursor; ++cursor)
	{
		if ('/' == *cursor || ':' == *cursor)
			name = cursor + 1;
	}

	return name;
}

static void unlink_pending(rl_amigafs_t *self, rl_pending_operation_t *target)
{
	rl_pending_operation_t *op = self->pending, *previous = NULL;

	RL_LOG_DEBUG(("Unlinking pending operation %u (%s)", target->request_seqno, rl_msg_name(target->expected_answer_type)));

	while (op)
	{
		if (target == op)
		{
			if (previous)
				previous->next = target->next;
			else
				self->pending = target->next;
			break;
		}
		else
		{
			previous = op;
			op = op->next;
		}
	}

	RL_FREE_TYPED(rl_pending_operation_t, target);
	dump_pending_ops(self);
}


/*
 * Mount a volume with the specified device name and map all handler messages
 * to the specified port.
 */
static struct DeviceList *mount_volume(const char *name, struct MsgPort *port)
{
   struct DeviceList *volume;
   struct DosList *dlist;

   if(name == NULL || port == NULL) return NULL;

   while(NULL == (dlist = AttemptLockDosList(LDF_VOLUMES|LDF_WRITE)))
   {
	   /* Can't lock the DOS list.  Wait a second and try again. */
	   Delay(50);
   }

   volume = (struct DeviceList *) FindDosEntry(dlist, (CONST_STRPTR) name, LDF_VOLUMES);

   UnLockDosList(LDF_VOLUMES|LDF_WRITE);

   if(volume || !(volume = (struct DeviceList *)MakeDosEntry((CONST_STRPTR) name, DLT_VOLUME)))
   {
	   return NULL;
   }

   volume->dl_VolumeDate.ds_Days	= 0L;
   volume->dl_VolumeDate.ds_Minute	= 0L;
   volume->dl_VolumeDate.ds_Tick	= 0L;
   volume->dl_Lock					= 0L;
   volume->dl_Task					= port;
   volume->dl_DiskType				= ID_DOS_DISK;

   while(NULL == (dlist = AttemptLockDosList(LDF_VOLUMES|LDF_WRITE)))
   {
	   /* Oops, can't lock DOS list.  Wait 1 second and retry. */
	   Delay(50);
   }

   AddDosEntry((struct DosList *)volume);
   UnLockDosList(LDF_VOLUMES|LDF_WRITE);
   return volume;
}

static int unmount_volume(struct DeviceList *volume)
{
	/* no volume, or locked; can't unmount */
	if(volume == NULL || volume->dl_Lock != 0)
		return 1;

	RemDosEntry((struct DosList *)volume);
	FreeDosEntry((struct DosList *)volume);
	return 0;
}

static int
reply_to_packet(rl_amigafs_t *self, struct DosPacket *packet)
{
	struct MsgPort* reply_port;
	RL_LOG_DEBUG(("OUT: %s Res1=%08x Res2=%08x", get_packet_type_name(packet), packet->dp_Res1, packet->dp_Res2));
	reply_port = packet->dp_Port;
	packet->dp_Port = self->device_port;
	PutMsg(reply_port, packet->dp_Link);
	return 0;
}

/*
 * Shared tail for the action handlers that may have a pending operation in
 * flight when they bail out. `res1` differs between them: most report failure
 * as DOSFALSE, but reads and writes return a byte count where -1 is the
 * failure signal (zero would read as a clean end of file).
 */
static void fail_pending(
		rl_amigafs_t *self,
		struct DosPacket *packet,
		rl_pending_operation_t *pending_op,
		LONG res1,
		LONG error_code)
{
	if (pending_op)
		unlink_pending(self, pending_op);

	packet->dp_Res1 = res1;
	packet->dp_Res2 = error_code;
	reply_to_packet(self, packet);
}

static struct FileLock *allocate_lock(
		rl_amigafs_t *fs,
		rl_client_handle_type_t type,
		rl_uint32 handle_id,
		LONG access,
		const char *name,
		rl_uint32 size)
{
	struct FileLock *lock = NULL;
	rl_client_handle_t *handle = NULL;

   	if (NULL == (lock = RL_ALLOC_TYPED_ZERO(struct FileLock)))
		goto error;

   	if (NULL == (handle = RL_ALLOC_TYPED_ZERO(rl_client_handle_t)))
		goto error;

	handle->handle_id = handle_id;
	handle->type = type;
	handle->refcount = 1;
	handle->size_lo = size;
	rl_string_copy(sizeof(handle->path), handle->path, name);

	RL_LOG_DEBUG(("Allocated lock %p for handle id %d (%p) type %s", lock, handle_id, handle, rl_client_handle_type_name(type)));

	lock->fl_Access = access;
	lock->fl_Key = (LONG) handle;
	lock->fl_Task = fs->device_port;
	lock->fl_Volume = MKBADDR(fs->device_list);
	return lock;

error:
	if (lock)
		RL_FREE_TYPED(struct FileLock, lock);
	if (handle)
		RL_FREE_TYPED(rl_client_handle_t, handle);

	return NULL;
}

/*
 * A lock on the volume root. Each one gets a handle of its own rather than
 * sharing a single root handle, because the server tracks the directory cursor
 * per handle: two programs listing the volume through one handle would consume
 * each other's entries. The server handle itself is opened lazily, by the first
 * enumeration that needs one.
 */
struct FileLock* rl_amigafs_alloc_root_lock(rl_amigafs_t *self, long mode)
{
	return allocate_lock(self, RL_HANDLE_DEVICE, RL_ROOT_HANDLE_ID, mode, self->root_handle.path, 0);
}

/* A second lock on an object already locked: both share the one server handle,
 * so only the last one freed may hand the id back. */
static struct FileLock *duplicate_lock(rl_amigafs_t *fs, rl_client_handle_t *handle, LONG access)
{
	struct FileLock *lock;

   	if (NULL == (lock = RL_ALLOC_TYPED_ZERO(struct FileLock)))
		return NULL;

	++handle->refcount;

	RL_LOG_DEBUG(("Allocated lock %p sharing handle id %d (%p), refcount %d",
				lock, handle->handle_id, handle, (int) handle->refcount));

	lock->fl_Access = access;
	lock->fl_Key = (LONG) handle;
	lock->fl_Task = fs->device_port;
	lock->fl_Volume = MKBADDR(fs->device_list);
	return lock;
}

/* Ask the server to open `path` and answer the pending op with the handle. */
static int transmit_open_handle_request(rl_amigafs_t *fs, rl_pending_operation_t *op, const char *path)
{
	rl_msg_t msg;

	RL_MSG_INIT(msg, RL_MSG_OPEN_HANDLE_REQUEST);
	msg.open_handle_request.hdr_sequence_num	= op->request_seqno;
	msg.open_handle_request.path				= path;
	msg.open_handle_request.mode				= RL_OPENFLAG_READ;
	return peer_transmit_message(fs->peer, &msg);
}

/* Hand a server-side handle back; its table of them is a fixed size. */
static void transmit_close_handle(rl_amigafs_t *fs, rl_uint32 handle_id)
{
	rl_msg_t msg;

	RL_MSG_INIT(msg, RL_MSG_CLOSE_HANDLE_REQUEST);
	msg.close_handle_request.hdr_sequence_num = fs->seqno++;
	msg.close_handle_request.handle = handle_id;
	RL_LOG_DEBUG(("transmitting close request for handle %d", handle_id));
	if (0 != peer_transmit_message(fs->peer, &msg))
		RL_LOG_WARNING(("Couldn't transmit close handle request for id %d", handle_id));
}

void rl_amigafs_free_lock(rl_amigafs_t *fs, struct FileLock *lock)
{
	rl_client_handle_t *handle;

	RL_ASSERT(lock);

	handle = HANDLE_FROM_LOCK(lock);

	RL_ASSERT(handle);

	/* Don't close a handle a duplicate of this lock is still using, and don't
	 * ask the server to close its own root handle: a root lock carries that id
	 * until an enumeration opens one of its own. */
	if (0 == --handle->refcount)
	{
		if (RL_ROOT_HANDLE_ID != handle->handle_id)
			transmit_close_handle(fs, handle->handle_id);
		RL_FREE_TYPED(rl_client_handle_t, handle);
	}

	RL_FREE_TYPED(struct FileLock, lock);
}

#define HANDLER_RANGE_1_FIRST (0)
#define HANDLER_RANGE_1_LAST (34)

typedef void (*packet_handler_fn)(rl_amigafs_t *fs, struct DosPacket *packet);

/* Handler functions. NULL in the table below means the action is not
 * implemented at all; action_unsupported means it is known but always
 * rejected. BCPL pointer arguments are adjusted by the individual handlers
 * with BCPL_CAST(). */

static void action_die				(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_current_volume	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_locate_object	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_free_lock		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_copy_dir			(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_examine_object	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_examine_next		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_disk_info		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_info				(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_parent			(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_unsupported		(rl_amigafs_t *fs, struct DosPacket *packet);

static void action_set_protect		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_flush			(rl_amigafs_t *fs, struct DosPacket *packet);

static void action_findinput		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_findoutput		(rl_amigafs_t *fs, struct DosPacket *packet);

static const packet_handler_fn packet_handlers_range_1[(HANDLER_RANGE_1_LAST - HANDLER_RANGE_1_FIRST) + 1] =
{
   NULL,					/*  0 - ACTION_NIL			 */
   NULL,					/*  1 - Unknown				 */
   NULL,					/*  2 - ACTION_GET_BLOCK	 */
   NULL,					/*  3 - Unknown				 */
   NULL,					/*  4 - ACTION_SET_MAP		 */
   action_die,				/*  5 - ACTION_DIE			 */
   NULL,					/*  6 - ACTION_EVENT		 */
   action_current_volume,	/*  7 - ACTION_CURRENT_VOLUME*/
   action_locate_object,	/*  8 - ACTION_LOCATE_OBJECT */
   action_unsupported,		/*  9 - ACTION_RENAME_DISK	 */
   NULL,					/* 10 - Unknown				 */
   NULL,					/* 11 - Unknown				 */
   NULL,					/* 12 - Unknown				 */
   NULL,					/* 13 - Unknown				 */
   NULL,					/* 14 - Unknown				 */
   action_free_lock,		/* 15 - ACTION_FREE_LOCK	 */
   action_unsupported,		/* 16 - ACTION_DELETE_OBJECT */
   action_unsupported,		/* 17 - ACTION_RENAME_OBJECT */
   NULL,					/* 18 - ACTION_MORE_CACHE	 */
   action_copy_dir,			/* 19 - ACTION_COPY_DIR		 */
   NULL,					/* 20 - ACTION_WAIT_CHAR	 */
   action_set_protect,		/* 21 - ACTION_SET_PROTECT	 */
   action_unsupported,		/* 22 - ACTION_CREATE_DIR	 */
   action_examine_object,	/* 23 - ACTION_EXAMINE_OBJECT*/
   action_examine_next,		/* 24 - ACTION_EXAMINE_NEXT	 */
   action_disk_info,		/* 25 - ACTION_DISK_INFO	 */
   action_info,				/* 26 - ACTION_INFO			 */
   action_flush,			/* 27 - ACTION_FLUSH		 */
   action_unsupported,		/* 28 - ACTION_SET_COMMENT	 */
   action_parent,			/* 29 - ACTION_PARENT		 */
   NULL,					/* 30 - ACTION_TIMER		 */
   action_unsupported,		/* 31 - ACTION_INHIBIT		 */
   NULL,					/* 32 - ACTION_DISK_TYPE	 */
   NULL,					/* 33 - ACTION_DISK_CHANGE	 */
   action_unsupported		/* 34 - ACTION_SET_FILE_DATE */
};

static void action_is_filesystem(rl_amigafs_t *fs, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_is_filesystem"));
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}


/*
 *	ACTION_FINDINPUT	Open(..., MODE_OLDFILE)
 *
 *	ARG1:	BPTR -	FileHandle to fill in
 *	ARG2:	LOCK -	Lock to directory that ARG3 is relative to
 *	ARG3:	BSTR -	Name of file to be opened (relative to ARG2)
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void complete_findinput(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);

static void action_findinput(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock * const dir_lock =
		BCPL_CAST(struct FileLock, packet->dp_Arg2);

	const void *filename_bstr = BCPL_CAST(const void, packet->dp_Arg3);
	char filename[RL_AMIGA_PATH_MAX];
	char full_path[RL_AMIGA_PATH_MAX];
	const char *filename_cstr = filename;

	rl_pending_operation_t *pending_op = NULL;
	LONG error_code = 0;

    RL_LOG_DEBUG(("FINDINPUT: directory=\"%d\", name=\"%Q\"",
				dir_lock ? HANDLE_FROM_LOCK(dir_lock)->handle_id : -1, packet->dp_Arg3));

	/* A BSTR is not NUL-terminated; the virtual-channel test below needs a
	 * C string. The server path is built from the BSTR by
	 * normalize_object_path(). */
	if (0 != (error_code = bstr_to_cstr(filename, sizeof(filename), filename_bstr)))
		goto error;

	/* Skip leading DEVICE: header that is sometimes present */
	{
		const char* colon;
		if (NULL != (colon = rl_strchr(filename_cstr, ':')))
		{
			filename_cstr = colon+1;
			RL_LOG_DEBUG(("FINDINPUT: dropping device prefix"));
		}
	}

	/* See if this is a request to open the virtual input channel */
	if (0 == rl_strcmp(filename_cstr, RLAUNCH_VIRTUAL_INPUT_FILE))
	{
		struct FileLock *file_lock;
		struct FileHandle * const fh = BCPL_CAST(struct FileHandle, packet->dp_Arg1);
		file_lock = allocate_lock(fs, RL_HANDLE_VIRTUAL_INPUT, RL_FILEHANDLE_VIRTUAL_INPUT, SHARED_LOCK, filename_cstr, 0);

		if (!file_lock)
		{
			error_code = ERROR_NO_FREE_STORE;
			goto error;
		}

		RL_LOG_DEBUG(("FINDINPUT: opening virtual input channel"));
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
		fh->fh_Type = fs->device_port;
		fh->fh_Arg1 = (LONG) file_lock;
		reply_to_packet(fs, packet);
		return;
	}

	/* The name is relative to the directory lock, so resolve it against that
	 * lock before asking the server for it. */
	if (0 != (error_code = normalize_object_path(fs, full_path, sizeof(full_path), dir_lock, filename_bstr)))
		goto error;

	/* Construct a pending open for the file. */
	pending_op = alloc_pending(fs, packet, RL_MSG_OPEN_HANDLE_ANSWER, complete_findinput);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	if (0 != transmit_open_handle_request(fs, pending_op, full_path))
	{
		error_code = ERROR_NOT_A_DOS_DISK;
		goto error;
	}

	return;

error:
	fail_pending(fs, packet, pending_op, DOSFALSE, error_code);
}

static void complete_findinput(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct DosPacket * const packet = op->input_packet;
	struct FileHandle * const fh = BCPL_CAST(struct FileHandle, op->input_packet->dp_Arg1);
	struct FileLock * const dir_lock = BCPL_CAST(struct FileLock, packet->dp_Arg2);
	const void *filename_bstr = BCPL_CAST(const void, packet->dp_Arg3);
	char full_path[RL_AMIGA_PATH_MAX];

	/* cannot fail here -- action_findinput normalized the same
	 * inputs before putting the request on the wire. */
	if (0 != normalize_object_path(fs, full_path, sizeof(full_path), dir_lock, filename_bstr))
	{
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_LINE_TOO_LONG;
		transmit_close_handle(fs, msg->open_handle_answer.handle);
		reply_to_packet(fs, packet);
		unlink_pending(fs, op);
		return;
	}

	/* Make sure the client is getting a lock on a file. */
	if (RL_NODE_TYPE_FILE != msg->open_handle_answer.type)
	{
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
	}
	else
	{
		struct FileLock *file_lock;

		file_lock = allocate_lock(fs,
				RL_HANDLE_FILE,
				msg->open_handle_answer.handle,
				SHARED_LOCK,
				full_path,
				msg->open_handle_answer.size);

		if (!file_lock)
		{
			packet->dp_Res1 = DOSFALSE;
			packet->dp_Res2 = ERROR_NO_FREE_STORE;
		}
		else
		{
			packet->dp_Res1 = DOSTRUE;
			packet->dp_Res2 = 0;
			fh->fh_Type = fs->device_port;
			fh->fh_Arg1 = (LONG) file_lock;
		}
	}

	/* If we failed, clean up the server-side handle. */
	if (DOSFALSE == packet->dp_Res1)
		transmit_close_handle(fs, msg->open_handle_answer.handle);

	reply_to_packet(fs, packet);
	unlink_pending(fs, op);
}

/*
 *	ACTION_FINDOUTPUT	Open(..., MODE_NEWFILE)
 *
 *	ARG1:	BPTR -	FileHandle to fill in
 *	ARG2:	LOCK -	Lock to directory that ARG3 is relative to
 *	ARG3:	BSTR -	Name of file to be opened (relative to ARG2)
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void action_findoutput(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileHandle * const fh =
		BCPL_CAST(struct FileHandle, packet->dp_Arg1);

	struct FileLock * const dir_lock =
		BCPL_CAST(struct FileLock, packet->dp_Arg2);

	struct FileLock *file_lock;

	const void *filename_bstr = BCPL_CAST(const void, packet->dp_Arg3);
	char filename[RL_AMIGA_PATH_MAX];
	const char *filename_cstr = filename;

	LONG error_code = 0;

    RL_LOG_DEBUG(("FINDOUTPUT: directory=\"%d\", name=\"%Q\"",
				dir_lock ? HANDLE_FROM_LOCK(dir_lock)->handle_id : -1, packet->dp_Arg3));

	/* A BSTR is not NUL-terminated; the comparison below needs a C string. */
	if (0 != (error_code = bstr_to_cstr(filename, sizeof(filename), filename_bstr)))
		goto error;

	/* Skip leading DEVICE: header that is sometimes present */
	{
		const char* colon;
		if (NULL != (colon = rl_strchr(filename_cstr, ':')))
		{
			filename_cstr = colon+1;
			RL_LOG_DEBUG(("FINDOUTPUT: dropping device prefix"));
		}
	}

	/* We only support writing to the virtual output file. */
	if (0 != rl_strcmp(filename_cstr, RLAUNCH_VIRTUAL_OUTPUT_FILE))
	{
		RL_LOG_DEBUG(("FINDOUTPUT: attempt to write to file beside virtual output file"));
		error_code = ERROR_WRITE_PROTECTED;
		goto error;
	}

	file_lock = allocate_lock(fs, RL_HANDLE_VIRTUAL_OUTPUT, RL_FILEHANDLE_VIRTUAL_OUTPUT, EXCLUSIVE_LOCK, filename_cstr, 0);
	if (!file_lock)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	fh->fh_Type = fs->device_port;
	fh->fh_Arg1 = (LONG) file_lock;

	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
	return;

error:
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = error_code;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_EXAMINE_OBJECT	Examine(...)
 *
 *	ARG1:	LOCK -	Lock on object to examine
 *	ARG2:	BPTR -	FileInfoBlock to fill in
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void action_examine_object(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	struct FileInfoBlock *fib = BCPL_CAST(struct FileInfoBlock, packet->dp_Arg2);
	rl_client_handle_t *handle = NULL;

	RL_LOG_DEBUG(("EXAMINE_OBJECT Lock=%p fib=%p", lock, fib));

	if (lock)
		handle = HANDLE_FROM_LOCK(lock);

	if (!handle)
		handle = &fs->root_handle;

	if (IS_ROOT_HANDLE(handle))
	{
		rl_memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_EntryType = fib->fib_DirEntryType = ST_ROOT;
		{
			/* dl_Name is a BSTR whose length byte goes up to 255; fib_FileName
			 * holds a size byte, the name and a terminator. */
			const void *volume_name = BADDR(fs->device_list->dl_Name);
			size_t volume_len = BSTR_LEN(volume_name);

			if (volume_len > sizeof(fib->fib_FileName) - 2)
				volume_len = sizeof(fib->fib_FileName) - 2;

			fib->fib_FileName[0] = (char) volume_len;
			rl_memcpy(fib->fib_FileName + 1, BSTR_PTR(volume_name), volume_len);
			fib->fib_FileName[volume_len + 1] = '\0';
		}
		fib->fib_Protection = 0;
		fib->fib_Size = 0;
		fib->fib_NumBlocks = 0;
		fib->fib_Date = fs->device_list->dl_VolumeDate;
		construct_bstr(fib->fib_Comment, sizeof(fib->fib_Comment), "This is a remote launch device");
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		rl_memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_DirEntryType = RL_HANDLE_FILE == handle->type ? -1 : 1;
		fib->fib_EntryType = fib->fib_DirEntryType;
		construct_bstr(fib->fib_FileName, sizeof(fib->fib_FileName), final_component(handle->path));
		fib->fib_Size = handle->size_lo;
		fib->fib_NumBlocks = handle->size_lo;
		fib->fib_Comment[0] = '\0';
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}

	reply_to_packet(fs, packet);
}

/*
 *	ACTION_EXAMINE_NEXT	ExNext(...)
 *
 *	ARG1:	LOCK -	Lock on directory being examined
 *	ARG2:	BPTR -	FileInfoBlock to fill in
 *
 *	RES1:	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	Failure code if RES1 = DOSFALSE
 */
static void complete_examine_next(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);
static void complete_root_enum_open(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);

static void action_examine_next(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	/* DOS produces a NULL lock for the volume root, the same case
	 * action_examine_object() handles. */
	rl_client_handle_t *handle = lock ? HANDLE_FROM_LOCK(lock) : &fs->root_handle;
	rl_msg_t msg;
	rl_pending_operation_t *pending_op = NULL;
	LONG error_code;

	/* A root lock still naming the server's root handle would enumerate through
	 * the cursor every other root lock uses. Open a handle for this lock first
	 * and let its completion restart the enumeration. */
	if (lock && RL_ROOT_HANDLE_ID == handle->handle_id)
	{
		pending_op = alloc_pending(fs, packet, RL_MSG_OPEN_HANDLE_ANSWER, complete_root_enum_open);
		if (!pending_op)
		{
			error_code = ERROR_NO_FREE_STORE;
			goto error;
		}

		if (0 != transmit_open_handle_request(fs, pending_op, ""))
		{
			error_code = ERROR_NOT_A_DOS_DISK;
			goto error;
		}

		return;
	}

	pending_op = alloc_pending(fs, packet, RL_MSG_FIND_NEXT_FILE_ANSWER, complete_examine_next);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	RL_MSG_INIT(msg, RL_MSG_FIND_NEXT_FILE_REQUEST);
	msg.find_next_file_request.hdr_sequence_num	= pending_op->request_seqno;
	msg.find_next_file_request.handle = handle->handle_id;
	msg.find_next_file_request.reset =
		(handle->flags & RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS) ? 0 : 1;

	if (0 != peer_transmit_message(fs->peer, &msg))
	{
		error_code = ERROR_DEVICE_NOT_MOUNTED;
		goto error;
	}

	handle->flags |= RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS;
	return;

error:
	fail_pending(fs, packet, pending_op, DOSFALSE, error_code);
}

static void complete_examine_next(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct DosPacket * const packet = op->input_packet;
	struct FileLock * const lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	struct FileInfoBlock * const fib = BCPL_CAST(struct FileInfoBlock, packet->dp_Arg2);
	rl_client_handle_t *handle = lock ? HANDLE_FROM_LOCK(lock) : &fs->root_handle;
	const rl_msg_find_next_file_answer_t * const answer = &msg->find_next_file_answer;
	
	if (answer->end_of_sequence)
	{
		handle->flags &= ~(RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS);
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_NO_MORE_ENTRIES;
	}
	else
	{
		rl_memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_DirEntryType = RL_NODE_TYPE_DIRECTORY == answer->type ? 1 : -1;
		fib->fib_EntryType = fib->fib_DirEntryType; /* FIXME: Is this right? */
		construct_bstr(fib->fib_FileName, sizeof(fib->fib_FileName), answer->name);
		/* Set protection bits for regular files. These set bits in the
		 * protection mask indicate forbidden actions, not caps. Really weird.
		 * */
		fib->fib_Protection = FIBF_WRITE | FIBF_DELETE /* simulate R/O FS */;
		fib->fib_Size = answer->size;
		fib->fib_NumBlocks = answer->size;
		fib->fib_Comment[0] = '\0';
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}

	reply_to_packet(fs, packet);
	unlink_pending(fs, op);
}

/* The root lock now has a server handle of its own; run the enumeration the
 * packet came in for through it. */
static void complete_root_enum_open(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct DosPacket * const packet = op->input_packet;
	struct FileLock * const lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);

	unlink_pending(fs, op);

	if (RL_NODE_TYPE_DIRECTORY != msg->open_handle_answer.type)
	{
		transmit_close_handle(fs, msg->open_handle_answer.handle);
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
		reply_to_packet(fs, packet);
		return;
	}

	HANDLE_FROM_LOCK(lock)->handle_id = msg->open_handle_answer.handle;
	action_examine_next(fs, packet);
}

/* Helper function to populate a InfoData struct from the specified fs. */
static void fill_in_infodata(rl_amigafs_t *fs, struct InfoData *info)
{
	RL_LOG_DEBUG(("Populating infodata @ %p - %p", info, ((LONG)info) + sizeof(*info)));

	info->id_NumSoftErrors = 0;
	info->id_UnitNumber = 1;
	info->id_DiskState = ID_VALIDATED;
	info->id_NumBlocks = 1000;
	info->id_NumBlocksUsed = 500;
	info->id_BytesPerBlock = 1;
	info->id_DiskType = ID_FFS_DISK;
	info->id_VolumeNode = MKBADDR(fs->device_list);
	info->id_InUse = 0;

	RL_LOG_DEBUG(("id_VolumeNode as BPTR = %08x", info->id_VolumeNode));
}

/*	ACTION_DISK_INFO	Info(...)
 *	ARG1:	BPTR -	Pointer to an InfoData structure to fill in
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 */
static void action_disk_info(rl_amigafs_t *fs, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("ACTION_DISK_INFO"));
	fill_in_infodata(fs, BCPL_CAST(struct InfoData, packet->dp_Arg1));
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_INFO	<sendpkt only>
 *
 *	ARG1:	LOCK -	Lock on volume
 *	ARG2:	BPTR -	Pointer to an InfoData structure to fill in
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 */
static void action_info(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);

	RL_LOG_DEBUG(("ACTION_INFO %p lock_id=%d", lock, lock ? HANDLE_FROM_LOCK(lock)->handle_id : -1));

	if (NULL == lock || lock->fl_Volume != MKBADDR(fs->device_list))
	{
		RL_LOG_DEBUG(("--> failed, invalid lock"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_INVALID_LOCK;
	}
	else if (IS_ROOT_HANDLE(HANDLE_FROM_LOCK(lock)))
	{
		fill_in_infodata(fs, BCPL_CAST(struct InfoData, packet->dp_Arg2));
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}

	reply_to_packet(fs, packet);
}

static void action_flush(rl_amigafs_t *fs, struct DosPacket *packet)
{
	/* sure thing */
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_LOCATE_OBJECT	Lock(...)
 *
 *	ARG1:	LOCK -	Lock on directory to which ARG2 is relative
 *	ARG2:	BSTR -	Name (possibly with a path) of object to lock
 *	ARG3:	LONG -	Mode: ACCESS_READ/SHARED_LOCK or
 *			ACCESS_WRITE/EXCLUSIVE_LOCK
 *
 *	RES1:	LOCK -	Lock on requested object or 0 to indicate failure
 *	RES2:	CODE -	Failure code if RES1 = 0
 */
static void complete_locate_object(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);

static void action_locate_object(rl_amigafs_t *fs, struct DosPacket* packet)
{
	LONG error_code = ERROR_OBJECT_NOT_FOUND;
	struct FileLock *dir_lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	const void* object_name_bstr = BCPL_CAST(const void, packet->dp_Arg2);
	const LONG mode = packet->dp_Arg3;
	struct FileLock *result_lock = NULL;
	char full_path[RL_AMIGA_PATH_MAX];
	/* The root-lock path below can reach the error label before any pending
	 * op exists, and the label frees whatever this points at. */
	rl_pending_operation_t *pending_op = NULL;

    RL_LOG_DEBUG(("LOCATE_OBJECT: directory=\"%d\", name=\"%Q\" mode=%d (%s)",
				dir_lock ? HANDLE_FROM_LOCK(dir_lock)->handle_id : -1,
				packet->dp_Arg2,
				(int) mode,
				mode == -1 ? "SHARED_LOCK/ACCESS_READ" : "EXCLUSIVE_LOCK/ACCESS_WRITE"));

	/* Clean up and normalize the path string. Bail out before any pending op
	 * exists, so the error path below has nothing to unwind. */
	if (0 != (error_code = normalize_object_path(fs, full_path, sizeof(full_path), dir_lock, object_name_bstr)))
	{
		packet->dp_Res1 = 0;
		packet->dp_Res2 = error_code;
		reply_to_packet(fs, packet);
		return;
	}
	RL_LOG_DEBUG(("Normalized lookup path: '%s'", full_path));

	/* If the client really wanted the root node, we can return that immediately. */
	if (0 == rl_strlen(full_path))
	{
		result_lock = rl_amigafs_alloc_root_lock(fs, mode);
		if (!result_lock)
		{
			error_code = ERROR_NO_FREE_STORE;
			goto error;
		}
		RL_LOG_DEBUG(("Returning lock: %p for handle id %d", result_lock, HANDLE_FROM_LOCK(result_lock)->handle_id));
		packet->dp_Res1 = MKBADDR(result_lock);
		packet->dp_Res2 = 0;
		reply_to_packet(fs, packet);
		return;
	}
	
	/* Construct a pending handle open request for the object */
	pending_op = alloc_pending(fs, packet, RL_MSG_OPEN_HANDLE_ANSWER, complete_locate_object);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	if (0 != transmit_open_handle_request(fs, pending_op, full_path))
	{
		error_code = ERROR_NOT_A_DOS_DISK;
		goto error;
	}

	return;

error:
	fail_pending(fs, packet, pending_op, DOSFALSE, error_code);
}

/*
 * Turn the handle the server just opened into a lock on `path` and answer the
 * packet the operation was started for. Shared by every action that replies
 * with a lock built from an open_handle_answer.
 */
static void complete_lock_from_answer(
		rl_amigafs_t *fs,
		rl_pending_operation_t *op,
		const rl_msg_t *msg,
		const char *path,
		LONG access)
{
	struct DosPacket * const packet = op->input_packet;
	const rl_client_handle_type_t type =
		RL_NODE_TYPE_DIRECTORY == msg->open_handle_answer.type ? RL_HANDLE_DIR : RL_HANDLE_FILE;
	struct FileLock *lock =
		allocate_lock(fs, type, msg->open_handle_answer.handle, access, path, msg->open_handle_answer.size);

	if (lock)
	{
		packet->dp_Res1 = MKBADDR(lock);
		packet->dp_Res2 = 0;
	}
	else
	{
		packet->dp_Res1 = 0;
		packet->dp_Res2 = ERROR_NO_FREE_STORE;

		/* Nobody is left holding the handle the server just opened for us, and
		 * its table of them is fixed size -- hand it back. */
		transmit_close_handle(fs, msg->open_handle_answer.handle);
	}

	reply_to_packet(fs, packet);
	unlink_pending(fs, op);
}

static void complete_locate_object(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct DosPacket * const packet = op->input_packet;
	struct FileLock *dir_lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	const void* object_name_bstr = BCPL_CAST(const void, packet->dp_Arg2);
	char full_path[RL_AMIGA_PATH_MAX];

	/* cannot fail here -- action_locate_object normalized the same
	 * inputs before putting the request on the wire. */
	if (0 != normalize_object_path(fs, full_path, sizeof(full_path), dir_lock, object_name_bstr))
	{
		packet->dp_Res1 = 0;
		packet->dp_Res2 = ERROR_LINE_TOO_LONG;
		reply_to_packet(fs, packet);
		unlink_pending(fs, op);
		return;
	}

	/* Hand back the mode the caller asked for. Anything but ACCESS_WRITE is a
	 * shared lock; fl_Access has no third value. */
	complete_lock_from_answer(fs, op, msg, full_path,
			EXCLUSIVE_LOCK == packet->dp_Arg3 ? EXCLUSIVE_LOCK : SHARED_LOCK);
}


/*
 *	ACTION_FREE_LOCK	UnLock(...)
 *	ARG1:	LOCK -	Lock to free
 *	RES1:	BOOL -	DOSTRUE
 */
static void action_free_lock(rl_amigafs_t *fs, struct DosPacket* packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);

	if (lock)
	{
		RL_LOG_DEBUG(("ACTION_FREE_LOCK: lock: %p (handle: %d) Node=%x", lock, HANDLE_FROM_LOCK(lock)->handle_id, (int)lock->fl_Key));
		rl_amigafs_free_lock(fs, lock);
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		RL_LOG_DEBUG(("ACTION_FREE_LOCK w/ null lock?!"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_END	Close()
 *
 *	ARG1:	ARG1 -	Lock (fh_Arg1 field of the file handle we're closing)
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */ 

static void action_end(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock * const lock = (struct FileLock *) packet->dp_Arg1;

	/* TODO: Should make the free be sync w/ the server so freeing has a bottom
	 * half continuation with the close_handle_answer. Otherwise we might run
	 * out of handles if opening/closing quicker on the client than on the
	 * server. */
	rl_amigafs_free_lock(fs, lock);

	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_SET_PROTECT
 *
 *	ARG1:	ARG1 -	Unused
 *	ARG2:	LOCK -	Lock to which ARG3 is relative to
 *	ARG3:	BSTR -	bstring of the object name
 *	ARG4:   LONG -  Protection 32-bits
 *
 *	RES1:	BOOL -	DOSTRUE/DOSFALSE
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void action_set_protect(rl_amigafs_t *fs, struct DosPacket *packet)
{
	RL_LOG_DEBUG(("SET_PROTECT Lock=%p Name=\"%Q\" Bits=%d",
				packet->dp_Arg2,
				packet->dp_Arg3,
				(int) packet->dp_Arg4));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = ERROR_WRITE_PROTECTED;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_COPY_DIR		DupLock(...)
 *
 *	ARG1:	LOCK -	Lock to duplicate
 *
 *	RES1:	LOCK -	Duplicated lock or 0 to indicate failure
 *	RES2:	CODE -	Failure code if RES1 = 0
 */
static void action_copy_dir(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	if (lock)
	{
		struct FileLock *copy;
		rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);

		if (RL_HANDLE_DEVICE == handle->type)
			copy = rl_amigafs_alloc_root_lock(fs, SHARED_LOCK);
		else
			copy = duplicate_lock(fs, handle, SHARED_LOCK);

		packet->dp_Res1 = MKBADDR(copy);

		if (!packet->dp_Res1)
		{
			packet->dp_Res2 = ERROR_NO_FREE_STORE;
		}
		else
		{
			packet->dp_Res2 = 0;
			RL_LOG_DEBUG(("Resulting lock is %p", copy));
		}
	}
	else
	{
		RL_LOG_WARNING(("ACTION_COPY_DIR with null lock?!"));
		packet->dp_Res1 = 0;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_PARENT	Parent(...)
 *
 *	ARG1:	LOCK -	Lock on object to get the parent of
 *
 *	RES1:	LOCK -	Parent lock
 *	RES2:	Failure code if RES1 = 0
 */

/*
 * Copy a handle's path with the last component dropped. Returns zero when
 * there is no component to drop, i.e. the parent is the volume root.
 */
static int parent_path_of(char *buffer, size_t buffer_size, const rl_client_handle_t *handle)
{
	int len;

	rl_string_copy(buffer_size, buffer, handle->path);

	len = (int) rl_strlen(buffer);
	while (--len >= 0)
	{
		if (buffer[len] == '/')
		{
			buffer[len] = '\0';
			break;
		}
	}

	return len > 0;
}

static void complete_parent(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);

static void action_parent(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	rl_client_handle_t *handle = lock ? HANDLE_FROM_LOCK(lock) : NULL;
	struct FileLock *result_lock;
	rl_pending_operation_t *pending_op = NULL;
	LONG error_code;
	char parent_path[RL_AMIGA_PATH_MAX];

	RL_LOG_DEBUG(("ACTION_PARENT for lock %p (%d)", lock, handle ? (int) handle->handle_id : -1));

	if (NULL == handle || IS_ROOT_HANDLE(handle))
	{
		RL_LOG_DEBUG(("[The root handle (or null handle) doesn't have a parent]"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
		reply_to_packet(fs, packet);
		return;
	}

	/* No slashes? Assume it's a file in the root directory and return the root. */
	if (!parent_path_of(parent_path, sizeof(parent_path), handle))
	{
		RL_LOG_DEBUG(("Returning root lock as parent of %s", handle->path));
		result_lock = rl_amigafs_alloc_root_lock(fs, SHARED_LOCK);
		if (result_lock)
		{
			packet->dp_Res1 = MKBADDR(result_lock);
			packet->dp_Res2 = 0;
		}
		else
		{
			packet->dp_Res1 = DOSFALSE;
			packet->dp_Res2 = ERROR_NO_FREE_STORE;
		}
		reply_to_packet(fs, packet);
		return;
	}

	/* The parent needs a server handle of its own: a fabricated id would name
	 * the server's root, so enumerating it would list the wrong directory and
	 * unlocking it would close the root out from under the connection. */
	RL_LOG_DEBUG(("parent handle from '%s' to '%s'", handle->path, parent_path));

	pending_op = alloc_pending(fs, packet, RL_MSG_OPEN_HANDLE_ANSWER, complete_parent);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	if (0 != transmit_open_handle_request(fs, pending_op, parent_path))
	{
		error_code = ERROR_NOT_A_DOS_DISK;
		goto error;
	}

	return;

error:
	fail_pending(fs, packet, pending_op, DOSFALSE, error_code);
}

static void complete_parent(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct FileLock * const lock = BCPL_CAST(struct FileLock, op->input_packet->dp_Arg1);
	char parent_path[RL_AMIGA_PATH_MAX];

	/* The lock the caller asked the parent of is still theirs, so recomputing
	 * the path is cheaper than carrying it across the wire and back. */
	parent_path_of(parent_path, sizeof(parent_path), HANDLE_FROM_LOCK(lock));

	complete_lock_from_answer(fs, op, msg, parent_path, SHARED_LOCK);
}

/*
 *	ACTION_READ	Read(...)
 *
 *	ARG1:	ARG1 -	fh_Arg1 field of opened FileHandle
 *	ARG2:	APTR -	Buffer to put data into
 *	ARG3:	LONG -	Number of bytes to read
 *
 *	RES1:	LONG -	Number of bytes read. 0 indicates EOF. -1 indicates ERROR
 *	RES2:	CODE -	Failure code if RES1 = -1
 */
static void
complete_read(rl_amigafs_t *self, rl_pending_operation_t *op, const rl_msg_t *msg);

static int
transmit_read_request(peer_t *peer, rl_client_handle_t *handle, rl_pending_operation_t *op, rl_uint32 count);

static int
buffer_overlap(rl_client_handle_t *handle, struct DosPacket *packet, rl_uint32* offset, rl_uint32* count);

static void
action_read(rl_amigafs_t *self, struct DosPacket *packet)
{
	LONG error_code = ERROR_SEEK_ERROR; /* TODO: What to use for real read errors? */
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	rl_uint32 bytes_remaining = (rl_uint32) packet->dp_Arg3;

	rl_pending_operation_t *pending_op;

	RL_LOG_DEBUG(("action_read \"%s\", %d bytes", handle->path, (int) packet->dp_Arg3));

	/* See if we can satisfy some of the request from the read buffer. */
	{
		rl_uint32 offset, count;
		if (buffer_overlap(handle, packet, &offset, &count))
		{
			/* We can return (some) buffered data. */
			handle->offset_lo += count;
			bytes_remaining -= count;
			rl_memcpy((char*) packet->dp_Arg2, &handle->buffer[offset], count);

			/* Early out for request served entirely from buffer. */
			if (0 == bytes_remaining)
			{
				RL_LOG_DEBUG(("early out servicing %u bytes from buffer", count));
				packet->dp_Res1 = count;
				packet->dp_Res2 = 0;
				reply_to_packet(self, packet);
				return;
			}
		}
	}

	/* We have to round-trip to the server for more buffer data.
	 * Populate a pending op and queue it waiting for the network reply.
	 */
	pending_op = alloc_pending(self, packet, RL_MSG_READ_FILE_ANSWER, complete_read);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	{
		rl_uint32 bytes_read = (rl_uint32) packet->dp_Arg3 - bytes_remaining;
		pending_op->detail.read.destination = (char*) packet->dp_Arg2 + bytes_read;
	}

	if (0 != transmit_read_request(self->peer, handle, pending_op, bytes_remaining))
		goto error;

	return;

error:
	fail_pending(self, packet, pending_op, -1, error_code);
}

static int
transmit_read_request(peer_t *peer, rl_client_handle_t *handle, rl_pending_operation_t *op, rl_uint32 count)
{
	rl_msg_t msg;
	RL_MSG_INIT(msg, RL_MSG_READ_FILE_REQUEST);
	msg.read_file_request.hdr_sequence_num	= op->request_seqno;
	msg.read_file_request.handle			= handle->handle_id;
	msg.read_file_request.offset_hi			= handle->offset_hi;
	msg.read_file_request.offset_lo			= handle->offset_lo;
	/* TODO: split into multiple packets on request side or in answer w/ continuations? */
	msg.read_file_request.length			= RL_MAX_MACRO(count, sizeof(handle->buffer));

	return peer_transmit_message(peer, &msg);
}

static int
buffer_overlap(rl_client_handle_t *handle, struct DosPacket *packet, rl_uint32* offset, rl_uint32* count)
{
	rl_uint32 lo = handle->buffer_start;
	rl_uint32 hi = handle->buffer_start + handle->buffer_len;
	rl_uint32 out_offset;
	rl_uint32 avail = 0;
	rl_uint32 bytes_to_read = (rl_uint32) packet->dp_Arg3;

	/* See if the buffer lies after the cursor */
	if (handle->offset_lo < lo)
		return 0;

	/* See if the buffer lies before the cursor */
	if (hi <= handle->offset_lo)
		return 0;

	/* There is overlap with the start position inside the buffer. */
	avail = hi - handle->offset_lo;
	*offset = out_offset = (handle->offset_lo - lo);
	*count = RL_MIN_MACRO(avail, bytes_to_read);
	return 1;
}

static rl_uint32
readop_bytes_read(rl_pending_operation_t *op, struct DosPacket *packet)
{
	/* Calculate how much we read through pointer subtraction: dp_Arg2 is
	 * the start of the caller-supplied buffer. */
	return (rl_uint8*) op->detail.read.destination - (rl_uint8*) packet->dp_Arg2;
}

static void
complete_read(rl_amigafs_t *self, rl_pending_operation_t *op, const rl_msg_t *msg) 
{
	register struct DosPacket * const packet = op->input_packet;
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	const rl_uint32 amount_read = msg->read_file_answer.data.length;
	rl_uint32 amount_left = (rl_uint32) packet->dp_Arg3 - readop_bytes_read(op, packet);
	rl_uint32 slice_amount;

	/* Move data from the packet's transfer buffer into the destination.
	 *
	 * The read can have been much greater than requested, so only copy the
	 * externally expected size. For example, when requesting a single byte a
	 * full buffer will be requested. One byte should go to the external
	 * buffer, and the rest should end up in the buffer space to be used for
	 * future reads. */
	slice_amount = RL_MIN_MACRO(amount_left, amount_read);

	/* We always ask for at least sizeof(handle->buffer) bytes, so a conforming
	 * server can never leave us more surplus than the buffer holds. Anything
	 * larger is a protocol violation; fail the read instead of overflowing. */
	if (amount_read - slice_amount > sizeof(handle->buffer))
	{
		RL_LOG_WARNING(("read answer of %u bytes overflows the %u byte handle buffer",
					amount_read, (rl_uint32) sizeof(handle->buffer)));
		packet->dp_Res1 = -1;
		packet->dp_Res2 = ERROR_SEEK_ERROR;
		reply_to_packet(self, packet);
		unlink_pending(self, op);
		return;
	}

	rl_memcpy(op->detail.read.destination, msg->read_file_answer.data.base, slice_amount);

	/* Update the handle's virtual file position. TODO: 64-bit filepos. */
	handle->offset_lo += slice_amount;

	/* Move the byte cursor within the pending operation. Note that the op
	 * structure is reused between all reads required to fulfil a non-buffered
	 * read. */
	op->detail.read.destination += slice_amount;

	/* If we're done (all bytes read, or EOF), reply to the ACTION_READ and
	 * unlink the message. */
	if (packet->dp_Arg3 == readop_bytes_read(op, packet) || 0 == amount_read)
	{
		/* Any extra data we managed to read goes into the buffer. */
		handle->buffer_start = handle->offset_lo;
		handle->buffer_len = amount_read - slice_amount;
		rl_memcpy(handle->buffer, msg->read_file_answer.data.base + slice_amount, handle->buffer_len);
		RL_LOG_DEBUG(("Buffered %u bytes from offset %u", handle->buffer_len, handle->buffer_start));

		packet->dp_Res1 = readop_bytes_read(op, packet);
		packet->dp_Res2 = 0;
		RL_LOG_DEBUG(("Returning DOS result %d", packet->dp_Res1));
		reply_to_packet(self, packet);
		unlink_pending(self, op);
	}
	/* Otherwise, continue by leaving the read pending and issue more requests
	 * until we have filled the externally supplied buffer. AmigaDOS allows
	 * handlers to return partial reads, but in practice a lot of code is
	 * written that doesn't properly loop around Read(), so we have to do it
	 * for them or it will fail.
	 */
	else
	{
		/* Just grab the next sequence number and requeue the same operation */
		op->request_seqno = self->seqno++;

		if (0 != transmit_read_request(self->peer, handle, op, packet->dp_Arg3 - readop_bytes_read(op, packet)))
		{
			packet->dp_Res1 = -1;
			packet->dp_Res2 = ERROR_SEEK_ERROR;
			reply_to_packet(self, packet);
			unlink_pending(self, op);
		}
	}
}

/*
 *	ACTION_WRITE Write(...)
 *
 *	ARG1:	ARG1 -	fh_Arg1 field of opened FileHandle
 *	ARG2:	APTR -	Buffer to write data from
 *	ARG3:	LONG -	Number of bytes to write
 *
 *	RES1:	LONG -	Number of bytes written. -1 indicates ERROR
 *	RES2:	CODE -	Failure code if RES1 = -1
 */

static void
complete_write(rl_amigafs_t *self, rl_pending_operation_t *op, const rl_msg_t *msg);

static int
transmit_write_request(peer_t *peer, rl_client_handle_t *handle, rl_pending_operation_t *op, char* data, rl_uint32 count);

static void
action_write(rl_amigafs_t *self, struct DosPacket *packet)
{
	LONG error_code = ERROR_SEEK_ERROR; /* TODO: What to use for real read errors? */
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	rl_pending_operation_t *pending_op;

	RL_LOG_DEBUG(("action_write \"%s\", %d bytes from %p", handle->path, (int) packet->dp_Arg3, packet->dp_Arg2));

	if (!(pending_op = alloc_pending(self, packet, RL_MSG_WRITE_FILE_ANSWER, complete_write)))
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	if (0 != transmit_write_request(self->peer, handle, pending_op, (char*) packet->dp_Arg2, RL_MIN_MACRO(packet->dp_Arg3, 4096)))
		goto error;

	return;

error:
	fail_pending(self, packet, pending_op, -1, error_code);
}

static void
complete_write(rl_amigafs_t *self, rl_pending_operation_t *op, const rl_msg_t *msg) 
{
	struct DosPacket *packet;
	char *curr_ptr, *end_ptr;
	rl_client_handle_t *handle;
   
	packet = op->input_packet;
	handle = HANDLE_FROM_LOCK((struct FileLock *) packet->dp_Arg1);
	curr_ptr = op->detail.write.source + op->detail.write.length;
	end_ptr = (char*)packet->dp_Arg2 + packet->dp_Arg3;

	RL_LOG_DEBUG(("Completing write of %u bytes", op->detail.write.length));

	/* If there's more data to write, just keep sending. */
	if (curr_ptr < end_ptr)
	{
		/* Just grab the next sequence number and requeue the same operation */
		op->request_seqno = self->seqno++;

		if (0 != transmit_write_request(self->peer, handle, op, curr_ptr, RL_MIN_MACRO(end_ptr - curr_ptr, 4096)))
		{
			packet->dp_Res1 = curr_ptr - (char*)packet->dp_Arg2;
			packet->dp_Res2 = ERROR_SEEK_ERROR;
			reply_to_packet(self, packet);
			unlink_pending(self, op);
		}
	}
	else
	{
		packet->dp_Res1 = packet->dp_Arg3;
		packet->dp_Res2 = 0;
		reply_to_packet(self, packet);
		unlink_pending(self, op);
	}
}

static int
transmit_write_request(peer_t *peer, rl_client_handle_t *handle, rl_pending_operation_t *op, char* data, rl_uint32 count)
{
	rl_msg_t msg;
	RL_MSG_INIT(msg, RL_MSG_WRITE_FILE_REQUEST);
	msg.write_file_request.hdr_sequence_num	= op->request_seqno;
	msg.write_file_request.handle			= handle->handle_id;
	msg.write_file_request.data.base		= (const rl_uint8 *) data;
	msg.write_file_request.data.length		= count;

	op->detail.write.source = data;
	op->detail.write.length = count;

	return peer_transmit_message(peer, &msg);
}

/*
 *	ACTION_SEEK	Seek(...)
 *
 *	ARG1:	ARG1 -	fh_Arg1 field of opened FileHandle
 *	ARG2:	LONG -	Position or offset
 *	ARG3:	LONG -	Seek mode
 *
 *	RES1:	LONG -	Position before Seek() took place
 *	RES2:	CODE -	Failure code if RES1 = -1
 */
static void action_seek(rl_amigafs_t *self, struct DosPacket *packet)
{
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	LONG old_pos;
	LONG seek_amount;

	RL_LOG_DEBUG(("action_seek \"%s\", %d bytes rel %d", handle->path, (int) packet->dp_Arg2, (int) packet->dp_Arg3));

	old_pos = (LONG) handle->offset_lo;
	seek_amount = packet->dp_Arg2;

	switch (packet->dp_Arg3)
	{
	case OFFSET_BEGINNING:
		handle->offset_lo = seek_amount;
		break;
	case OFFSET_CURRENT:
		handle->offset_lo += seek_amount;
		break;
	case OFFSET_END:
		handle->offset_lo = ((LONG) handle->size_lo) + seek_amount;
		break;
	}

	/* assume 32-bit files */
	if (handle->offset_lo > handle->size_lo)
		handle->offset_lo = handle->size_lo;

	packet->dp_Res1 = old_pos;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

static const char* get_packet_type_name(const struct DosPacket* packet)
{
	switch (packet->dp_Type)
	{
	case ACTION_STARTUP: return "ACTION_STARTUP";
	case ACTION_GET_BLOCK: return "ACTION_GET_BLOCK";
	case ACTION_SET_MAP: return "ACTION_SET_MAP";
	case ACTION_DIE: return "ACTION_DIE";
	case ACTION_EVENT: return "ACTION_EVENT";
	case ACTION_CURRENT_VOLUME: return "ACTION_CURRENT_VOLUME";
	case ACTION_LOCATE_OBJECT: return "ACTION_LOCATE_OBJECT";
	case ACTION_RENAME_DISK: return "ACTION_RENAME_DISK";
	case ACTION_WRITE: return "ACTION_WRITE";
	case ACTION_READ: return "ACTION_READ";
	case ACTION_FREE_LOCK: return "ACTION_FREE_LOCK";
	case ACTION_DELETE_OBJECT: return "ACTION_DELETE_OBJECT";
	case ACTION_RENAME_OBJECT: return "ACTION_RENAME_OBJECT";
	case ACTION_MORE_CACHE: return "ACTION_MORE_CACHE";
	case ACTION_COPY_DIR: return "ACTION_COPY_DIR";
	case ACTION_WAIT_CHAR: return "ACTION_WAIT_CHAR";
	case ACTION_SET_PROTECT: return "ACTION_SET_PROTECT";
	case ACTION_CREATE_DIR: return "ACTION_CREATE_DIR";
	case ACTION_EXAMINE_OBJECT: return "ACTION_EXAMINE_OBJECT";
	case ACTION_EXAMINE_NEXT: return "ACTION_EXAMINE_NEXT";
	case ACTION_DISK_INFO: return "ACTION_DISK_INFO";
	case ACTION_INFO: return "ACTION_INFO";
	case ACTION_FLUSH: return "ACTION_FLUSH";
	case ACTION_SET_COMMENT: return "ACTION_SET_COMMENT";
	case ACTION_PARENT: return "ACTION_PARENT";
	case ACTION_TIMER: return "ACTION_TIMER";
	case ACTION_INHIBIT: return "ACTION_INHIBIT";
	case ACTION_DISK_TYPE: return "ACTION_DISK_TYPE";
	case ACTION_DISK_CHANGE: return "ACTION_DISK_CHANGE";
	case ACTION_SET_DATE: return "ACTION_SET_DATE";
	case ACTION_SCREEN_MODE: return "ACTION_SCREEN_MODE";
	case ACTION_READ_RETURN: return "ACTION_READ_RETURN";
	case ACTION_WRITE_RETURN: return "ACTION_WRITE_RETURN";
	case ACTION_SEEK: return "ACTION_SEEK";
	case ACTION_FINDUPDATE: return "ACTION_FINDUPDATE";
	case ACTION_FINDINPUT: return "ACTION_FINDINPUT";
	case ACTION_FINDOUTPUT: return "ACTION_FINDOUTPUT";
	case ACTION_END: return "ACTION_END";
	case ACTION_SET_FILE_SIZE: return "ACTION_SET_FILE_SIZE";
	case ACTION_WRITE_PROTECT: return "ACTION_WRITE_PROTECT";
	case ACTION_SAME_LOCK: return "ACTION_SAME_LOCK";
	case ACTION_CHANGE_SIGNAL: return "ACTION_CHANGE_SIGNAL";
	case ACTION_FORMAT: return "ACTION_FORMAT";
	case ACTION_MAKE_LINK: return "ACTION_MAKE_LINK";
	case ACTION_READ_LINK: return "ACTION_READ_LINK";
	case ACTION_FH_FROM_LOCK: return "ACTION_FH_FROM_LOCK";
	case ACTION_IS_FILESYSTEM: return "ACTION_IS_FILESYSTEM";
	case ACTION_CHANGE_MODE: return "ACTION_CHANGE_MODE";
	case ACTION_COPY_DIR_FH: return "ACTION_COPY_DIR_FH";
	case ACTION_PARENT_FH: return "ACTION_PARENT_FH";
	case ACTION_EXAMINE_ALL: return "ACTION_EXAMINE_ALL";
	case ACTION_EXAMINE_FH: return "ACTION_EXAMINE_FH";
	case ACTION_LOCK_RECORD: return "ACTION_LOCK_RECORD";
	case ACTION_FREE_RECORD: return "ACTION_FREE_RECORD";
	case ACTION_ADD_NOTIFY: return "ACTION_ADD_NOTIFY";
	case ACTION_REMOVE_NOTIFY: return "ACTION_REMOVE_NOTIFY";
	case ACTION_EXAMINE_ALL_END: return "ACTION_EXAMINE_ALL_END";
	case ACTION_SET_OWNER: return "ACTION_SET_OWNER";
	case ACTION_SERIALIZE_DISK: return "ACTION_SERIALIZE_DISK";
	default: return "<unknown>";
	}
}

static void process_fs_packet(rl_amigafs_t *self, struct DosPacket* packet)
{
	packet_handler_fn handler = NULL;
	register LONG packet_type = packet->dp_Type;

	RL_LOG_DEBUG(("IN: type=%s Arg1=%08x Arg2=%08x Arg3=%08x Arg4=%08x Arg5=%08x",
				get_packet_type_name(packet),
				packet->dp_Arg1,
				packet->dp_Arg2,
				packet->dp_Arg3,
				packet->dp_Arg4,
				packet->dp_Arg5));

	/* handle the most critical cases first */
	if (ACTION_READ == packet_type)
	{
		handler = action_read;
	}
	else if (ACTION_WRITE == packet_type)
	{
		handler = action_write;
	}
	/* handle common packets in the continous low range via a lookup table */
	else if (packet_type >= HANDLER_RANGE_1_FIRST && packet_type <= HANDLER_RANGE_1_LAST)
	{
		handler = packet_handlers_range_1[packet_type];
	}
	/* handle later extension packets with a switch--why didn't they order them
	 * continually?! */
	else
	{
		switch (packet_type)
		{
		case ACTION_IS_FILESYSTEM:
			handler = action_is_filesystem;
			break;

		case ACTION_FINDINPUT:
			handler = action_findinput;
			break;

		case ACTION_FINDOUTPUT:
			handler = action_findoutput;
			break;

		case ACTION_SEEK:
			handler = action_seek;
			break;

		case ACTION_END:
			handler = action_end;
			break;
		default:
			break;
		}
	}

	if (!handler)
	{
		RL_LOG_WARNING(("don't know how to handle %s", get_packet_type_name(packet)));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
		reply_to_packet(self, packet);
		return;
	}
	
	(*handler)(self, packet);
}

int rl_amigafs_init(rl_amigafs_t *self, peer_t *peer, const char *device_name)
{
	RL_LOG_DEBUG(("rl_amigafs_init %p w/ device_name=\"%s\"", self, device_name));

	rl_memset(self, 0, sizeof(rl_amigafs_t));

	self->peer = peer;
	self->root_handle.type = RL_HANDLE_DEVICE;
	self->root_handle.handle_id = RL_ROOT_HANDLE_ID;
	rl_string_copy(sizeof(self->root_handle.path), self->root_handle.path, device_name);

	if (NULL == (self->device_port = CreateMsgPort()))
	{
		RL_LOG_DEBUG(("creating message port failed"));
		goto cleanup;
	}

	if (NULL == (self->device_list = mount_volume(device_name, self->device_port)))
	{
		RL_LOG_DEBUG(("creating volume failed"));
		goto cleanup;
	}

	RL_LOG_DEBUG(("%s: volume mounted", peer ? peer->ident : ""));
	return 0;

cleanup:
	rl_amigafs_destroy(self);
	return 1;
}

/*
 * Fail a packet that was never dispatched, so there is no pending operation to
 * take the failure convention from. Read and write packets signal failure with
 * -1 in dp_Res1; everything else uses DOSFALSE.
 */
static void fail_packet_on_teardown(rl_amigafs_t *self, struct DosPacket *packet)
{
	switch (packet->dp_Type)
	{
		case ACTION_READ:
		case ACTION_WRITE:
			packet->dp_Res1 = -1;
			break;
		default:
			packet->dp_Res1 = DOSFALSE;
			break;
	}

	packet->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
	reply_to_packet(self, packet);
}

int rl_amigafs_destroy(rl_amigafs_t *self)
{
	rl_pending_operation_t *op;
	struct Message *msg;

	RL_LOG_DEBUG(("rl_amigafs_destroy %p", self));

	/* Every pending operation holds a DOS packet whose sender is blocked
	 * waiting for an answer the server will never send now. */
	op = self->pending;
	while (op)
	{
		rl_pending_operation_t *next = op->next;
		op->input_packet->dp_Res1 = failed_res1(op);
		op->input_packet->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
		reply_to_packet(self, op->input_packet);
		RL_FREE_TYPED(rl_pending_operation_t, op);
		op = next;
	}
	self->pending = NULL;

	/* Same for anything still queued at the port: deleting it would drop
	 * those packets and hang their senders. */
	if (self->device_port)
	{
		while (NULL != (msg = GetMsg(self->device_port)))
			fail_packet_on_teardown(self, (struct DosPacket *) msg->mn_Node.ln_Name);
	}

	/* A locked volume still points its dl_Task at our port, so neither the
	 * DOS entry nor the port can go away yet. Report failure so the caller
	 * knows not to free us either. */
	if (self->device_list && 0 != unmount_volume(self->device_list))
	{
		RL_LOG_WARNING(("%s: volume still locked; leaving it mounted",
					self->peer ? self->peer->ident : ""));
		return 1;
	}

	self->device_list = NULL;

	if (self->device_port)
	{
		DeleteMsgPort(self->device_port);
		self->device_port = NULL;
	}

	self->peer = 0;
	return 0;
}

int rl_amigafs_process_device_message(rl_amigafs_t *self)
{
	struct Message* msg;
	while (NULL != (msg = GetMsg(self->device_port)))
	{
		struct DosPacket *packet = (struct DosPacket *) msg->mn_Node.ln_Name;
		process_fs_packet(self, packet);
	}
	return 0;
}

static rl_pending_operation_t *
find_pending_op(rl_amigafs_t *self, const rl_msg_t *msg) 
{
	const rl_uint32 seqno = msg->handshake_request.hdr_sequence_num;
	rl_pending_operation_t *op = self->pending;

	while (op)
	{
		if (seqno == op->request_seqno)
			return op;
		else
			op = op->next;
	}

	return NULL;

}

static LONG translate_error_code(rl_uint32 error_code)
{
	switch (error_code)
	{
		case RL_NETERR_SUCCESS:
			return 0;

		case RL_NETERR_ACCESS_DENIED:
			return ERROR_OBJECT_IN_USE;

		case RL_NETERR_NOT_FOUND:
			return ERROR_OBJECT_NOT_FOUND;

		case RL_NETERR_NOT_A_FILE:
		case RL_NETERR_NOT_A_DIRECTORY:
			return ERROR_OBJECT_WRONG_TYPE;

		case RL_NETERR_IO_ERROR:
			/* TODO: This is probably going to break. */
			return ERROR_DISK_NOT_VALIDATED;

		case RL_NETERR_INVALID_VALUE:
			/* TODO: This is probably going to break. */
			return ERROR_OBJECT_WRONG_TYPE;

		default:
			return ERROR_DEVICE_NOT_MOUNTED;
	}
}

/*
 * The value dp_Res1 takes when a pending operation fails. READ and WRITE
 * answer with a byte count, where zero means "nothing transferred" -- end of
 * file to a reader -- so those two signal failure with -1 instead.
 */
static LONG failed_res1(const rl_pending_operation_t *op)
{
	switch (op->expected_answer_type)
	{
		case RL_MSG_READ_FILE_ANSWER:
		case RL_MSG_WRITE_FILE_ANSWER:
			return -1;

		default:
			return DOSFALSE;
	}
}

int rl_amigafs_process_network_message(rl_amigafs_t *self, const rl_msg_t *msg)
{
	const rl_msg_kind_t msg_kind = rl_msg_kind_of(msg);
	int status = 0;
	rl_pending_operation_t *pending_op = NULL;
	
	/* If there isn't any pending operation for this message, throw it away. */
	if (NULL == (pending_op = find_pending_op(self, msg)))
	{
		/* Close requests are fire-and-forget: they burn a sequence number but
		 * register no pending operation, so the server's error answer to one
		 * arrives unmatched. That failure is ignorable by definition--the
		 * handle is already gone on this side--and dropping the connection
		 * over it would take every other open file and lock with it. */
		if (RL_MSG_ERROR_ANSWER == msg_kind)
		{
			RL_LOG_WARNING(("Ignoring unmatched error answer for seq no %u (error %u)",
						msg->error_answer.hdr_in_reply_to,
						(unsigned int) msg->error_answer.error_code));
			return 0;
		}

		RL_LOG_DEBUG(("Couldn't find pending operation for message %s w/ seq no %u",
					rl_msg_name(msg_kind), msg->handshake_request.hdr_sequence_num));
		dump_pending_ops(self);
		return 1;
	}

	if (msg_kind == pending_op->expected_answer_type)
	{
		(*pending_op->callback)(self, pending_op, msg);
	}
	else if(msg_kind == RL_MSG_ERROR_ANSWER)
	{
		pending_op->input_packet->dp_Res1 = failed_res1(pending_op);
		pending_op->input_packet->dp_Res2 = translate_error_code(msg->error_answer.error_code);
		reply_to_packet(self, pending_op->input_packet);
		unlink_pending(self, pending_op);
	}
	else
	{
		RL_LOG_DEBUG(("mismatched answer for sequence #%u: got %s but expected %s",
					pending_op->request_seqno,
					rl_msg_name(msg_kind),
					rl_msg_name(pending_op->expected_answer_type)));
		pending_op->input_packet->dp_Res1 = failed_res1(pending_op);
		pending_op->input_packet->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
		reply_to_packet(self, pending_op->input_packet);
		unlink_pending(self, pending_op);
		status = 1; /* terminate this connection */
	}

	return status;
}

/*
   ACTION_DIE

Purpose: ask the handler to shut itself down
dp_Res1 - BOOL (DOSTRUE if the handler agreed to go away)
dp_Res2 - CODE (failure code if dp_Res1 = DOSFALSE)
*/
static void action_die(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_die"));
	/* Refuse: the connection is torn down by the peer loop, not from here.
	 * Flagging the filesystem for teardown and replying DOSTRUE is what a
	 * shutdown driven from the Amiga side would need. */
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = ERROR_OBJECT_IN_USE;
	reply_to_packet(self, packet);
}

/*
   ACTION_CURRENT_VOLUME

Purpose: identify the volume belonging to a FileHandle
Implements: used by AmigaDOS function ErrorReport(REPORT_STREAM)
dp_Type - ACTION_CURRENT_VOLUME (7)
dp_Arg1 - fh->fh_Argl
dp_Res1 - BPTR to struct DeviceList
dp_Res2 - ULONG (Exec unit number)
*/
static void action_current_volume(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_current_volume"));
	packet->dp_Res1 = MKBADDR(self->device_list);
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
	return;
}

/* Every action this filesystem knows about but does not implement: reject it
 * and name it from the dispatch table rather than a per-action literal. */
static void action_unsupported(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("%s: unsupported", get_packet_type_name(packet)));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}
