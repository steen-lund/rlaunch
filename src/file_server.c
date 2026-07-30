#include "config.h"
#include "controller.h"
#include "util.h"
#include "protocol.h"
#include "peer.h"
#include "rlnet.h"

#include <stdio.h>

#if defined(RL_WIN32)
#include <windows.h>
#elif defined(RL_POSIX)
#include <sys/types.h>
#include <sys/stat.h>
#if defined(RL_APPLE)
#include <sys/syslimits.h>
#else
#include <limits.h>
#endif
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif

static int reply_with_error(peer_t *peer, const rl_msg_t *msg, rl_uint32 error_code)
{
	rl_msg_t reply;
	RL_MSG_INIT(reply, RL_MSG_ERROR_ANSWER);
	reply.error_answer.hdr_in_reply_to = msg->handshake_request.hdr_sequence_num;
	reply.error_answer.error_code = error_code;
	peer_transmit_message(peer, &reply);
	return 1;
}

#if defined(RL_POSIX)
rl_proto_neterror_t translate_posix_errno(void)
{
	/* TODO: improve this translation */
	switch (errno)
	{
	case EACCES: return RL_NETERR_ACCESS_DENIED;
	case EISDIR: return RL_NETERR_NOT_A_FILE;
	default: return RL_NETERR_IO_ERROR;
	}
}

#endif


static rl_filehandle_t *get_handle_from_id(rl_controller_t *self, peer_t *peer, rl_uint32 handle_id)
{
	if ((rl_uint32) -1 == handle_id)
	{
		return &self->root_handle;
	}
	else if (RL_FILEHANDLE_VIRTUAL_INPUT == handle_id)
	{
		return &self->vinput_handle;
	}
	else if (RL_FILEHANDLE_VIRTUAL_OUTPUT == handle_id)
	{
		return &self->voutput_handle;
	}
	else if (handle_id >= RL_MAX_FILE_HANDLES)
	{
		return NULL;
	}
	else
	{
		return &self->handles[handle_id];
	}
}

/*
 * Does the path try to climb out of the directory being served? Anything with
 * a ".." component does, as does an absolute path, which would ignore the root
 * entirely. The input arrives from the network, so this is a trust boundary.
 */
static int escapes_root(const char *input)
{
	const char *cursor = input;

	if ('/' == input[0] || '\\' == input[0])
		return 1;

	/* A drive or device prefix ("c:", "TBL0:") is absolute too. */
	if (NULL != rl_strchr(input, ':'))
		return 1;

	for (;;)
	{
		if ('.' == cursor[0] && '.' == cursor[1] &&
			('\0' == cursor[2] || '/' == cursor[2] || '\\' == cursor[2]))
			return 1;

		/* Advance to just past the next separator, if any. */
		while ('\0' != *cursor && '/' != *cursor && '\\' != *cursor)
			++cursor;

		if ('\0' == *cursor)
			return 0;

		++cursor;
	}
}

#if defined(RL_POSIX)
/*
 * The string checks above cannot see a symlink inside the served root that
 * points out of it, so resolve the result and require the served root to still
 * be a prefix. The last component may not exist yet (creating a file), in which
 * case the directory it would land in is what has to be inside the root.
 */
static int inside_root(const char *path, const char *root_path)
{
	char real_root[PATH_MAX];
	char real_path[PATH_MAX];
	size_t root_len;

	if (NULL == realpath(root_path, real_root))
		return 0;

	if (NULL == realpath(path, real_path))
	{
		char parent[PATH_MAX];
		char *slash;

		if (ENOENT != errno || rl_strlen(path) >= sizeof(parent))
			return 0;

		/* Trim components until one resolves; the root always does, so this
		 * terminates. A missing leaf is a lookup that fails later as "not
		 * found" -- what matters here is that the part which does exist is
		 * inside the root. */
		rl_string_copy(sizeof(parent), parent, path);

		for (;;)
		{
			if (NULL == (slash = strrchr(parent, '/')) || parent == slash)
				return 0;

			*slash = '\0';

			if (NULL != realpath(parent, real_path))
				break;

			if (ENOENT != errno)
				return 0;
		}
	}

	root_len = rl_strlen(real_root);

	if (0 != strncmp(real_path, real_root, root_len))
		return 0;

	/* Guard the boundary so "/srv/rootless" doesn't pass as inside "/srv/root".
	 * realpath() always returns an absolute path, so root_len is at least 1 and
	 * a root of "/" already carries the separator. */
	return '\0' == real_path[root_len] || '/' == real_path[root_len] ||
		'/' == real_root[root_len - 1];
}
#endif

static int fix_path(char *dest, size_t dest_size, const char *input, const char *root_path)
{
	size_t root_len = rl_strlen(root_path);
	size_t input_len = rl_strlen(input);

	if (escapes_root(input))
		return 1;

	/* rl_format_msg() truncates silently, which would resolve a long path to
	 * the wrong file. Refuse instead; +2 covers the separator and the null. */
	if (root_len + input_len + 2 > dest_size)
		return 1;

#ifdef RL_WIN32
	{
		char backslash_path[260];
		size_t i;

		if (input_len >= sizeof(backslash_path))
			return 1;

		/* Stop at the terminator: the input is a decoded string sitting in the
		 * receive buffer, so reading past it is a remote-driven over-read. */
		for (i = 0; i < input_len; ++i)
		{
			char ch = input[i];
			if ('/' == ch)
				ch = '\\';
			backslash_path[i] = ch;
		}
		backslash_path[i] = '\0';

		rl_format_msg(dest, dest_size, "%s\\%s", root_path, backslash_path);
	}
#else
	rl_format_msg(dest, dest_size, "%s/%s", root_path, input);

	if (!inside_root(dest, root_path))
		return 1;
#endif
	return 0;
}

static rl_filehandle_t *make_handle(rl_controller_t *self, const char *path, int mode, rl_uint32 *error_out)
{
	rl_filehandle_t *slot;
	rl_filehandle_t * const slot_end = &self->handles[RL_MAX_FILE_HANDLES];
	char native_path[260];

	/* Fix the path */
	if (0 != fix_path(native_path, sizeof(native_path), path, self->root_handle.native_path))
	{
		*error_out = RL_NETERR_INVALID_VALUE;
		return NULL;
	}

	RL_LOG_DEBUG(("make_handle(\"%s\") => \"%s\"", path, native_path));

	/* Find a free slot.
	 *
	 * $PERF: This is O(n) right now and could be made O(1) if RL_MAX_FILE_HANDLES
	 * is ever greatly increased by using a free list.
	 */

	for (slot=&self->handles[0]; slot != slot_end; ++slot)
	{
		if (!slot->handle)
			break;
	}

	if (slot_end == slot)
	{
		*error_out = RL_NETERR_TOO_MANY_FILES_OPEN;
		return NULL; /* no free slots */
	}

#if defined(RL_WIN32)
	{
		DWORD dwAttributes = 0;

		DWORD dwDesiredAccess = 0;
		DWORD dwShareMode = FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE;
		DWORD dwCreationDisposition = 0;
		DWORD dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL;

		/* When reading, find out what type of path we're looking at (dir/file). */
		if ((mode & RL_OPENFLAG_WRITE) == 0)
		{
			dwAttributes = GetFileAttributes(native_path);
			if (INVALID_FILE_ATTRIBUTES == dwAttributes)
			{
				*error_out = RL_NETERR_NOT_FOUND;
				slot->handle = NULL;
				return NULL;
			}

			if (FILE_ATTRIBUTE_DIRECTORY & dwAttributes)
			{
				slot->type = RL_NODE_TYPE_DIRECTORY;
			}
			else
			{
				slot->type = RL_NODE_TYPE_FILE;
			}
		}
		else
		{
			/* It must be a file. */
			slot->type = RL_NODE_TYPE_FILE;
		}

		if (RL_NODE_TYPE_FILE == slot->type)
		{
			if (mode & RL_OPENFLAG_READ)
				dwDesiredAccess |= GENERIC_READ;
			if (mode & RL_OPENFLAG_WRITE)
				dwDesiredAccess |= GENERIC_WRITE;

			if (mode & RL_OPENFLAG_CREATE)
				dwCreationDisposition = CREATE_ALWAYS;
			else
				dwCreationDisposition = OPEN_EXISTING;

			slot->handle = CreateFileA(
					native_path, dwDesiredAccess, dwShareMode, NULL,
					dwCreationDisposition, dwFlagsAndAttributes, NULL);

			slot->find_handle = NULL;

			if (INVALID_HANDLE_VALUE == (HANDLE) slot->handle)
			{
				DWORD last_err = GetLastError();
				/* TODO: improve this translation */
				switch (last_err)
				{
					case ERROR_ACCESS_DENIED:
						*error_out = RL_NETERR_ACCESS_DENIED;
						break;
					default:
						*error_out = RL_NETERR_IO_ERROR;
						break;
				}

				slot->handle = NULL;
				return NULL;
			}

			/* FIXME: 64-bit support */
			slot->size = GetFileSize(slot->handle, NULL);
		}
		else
		{
			/* Mark the handle as a directory using INVALID_HANDLE_VALUE */
			slot->handle = INVALID_HANDLE_VALUE;
			slot->find_handle = NULL;
			slot->size = 0;
		}
	}
#elif defined(RL_POSIX)
	{
		struct stat st_buf;
		int flags = 0;
		struct stat st;

		/* figure out if the requested path is a file or directory */
		if (0 == (mode & RL_OPENFLAG_WRITE))
		{
			if (0 != stat(native_path, &st))
			{
				*error_out = RL_NETERR_NOT_FOUND;
				slot->handle = 0;
				return NULL;
			}

			if (S_ISDIR(st.st_mode))
				slot->type = RL_NODE_TYPE_DIRECTORY;
			else
				slot->type = RL_NODE_TYPE_FILE;
		}
		else
		{
			slot->type = RL_NODE_TYPE_FILE;
		}

		if (RL_NODE_TYPE_FILE == slot->type)
		{
			if (mode & RL_OPENFLAG_WRITE)
			{
				if (mode & RL_OPENFLAG_READ)
					flags = O_RDWR;
				else
					flags = O_WRONLY;
			}
			else
			{
				if (mode & RL_OPENFLAG_READ)
					flags = O_RDONLY;
				else
				{
					*error_out = RL_NETERR_INVALID_VALUE;
					return NULL;
				}
			}

			if (mode & RL_OPENFLAG_CREATE)
				flags |= O_CREAT;

#if !defined(RL_APPLE)
			/* Has to come after the access mode is picked above, which assigns
			 * over flags rather than or-ing into it. */
			flags |= O_LARGEFILE;
#endif

			slot->handle = open(native_path, flags, 0666);

			if (-1 == slot->handle || 0 != fstat(slot->handle, &st_buf))
			{
				*error_out = translate_posix_errno();

				if (-1 != slot->handle)
				{
					close(slot->handle);
				}

				slot->handle = 0;
				return NULL;
			}

			slot->size = st_buf.st_size;
		}
		else
		{
			/* mark the handle as a directory using -1 */
			slot->handle = -1;
			slot->size = 0;
		}
	}
#else
#error "Implement me."
#endif

	rl_string_copy(sizeof(slot->native_path), slot->native_path, native_path);

	*error_out = RL_NETERR_SUCCESS;
	return slot;
}

static INLINE rl_uint32 get_filehandle_index(rl_controller_t *self, rl_filehandle_t *handle)
{
	if (&self->root_handle == handle)
	{
		/* Treat the root handle specially, because it's used so frequently. */
		return (rl_uint32) -1;
	}
	else
	{
		/* Just calculate the offset through pointer arithmetic to get an index
		 * into the file handle array. This buys us O(1) lookup when processing
		 * reads and writes.
		 */
		return (rl_uint32) (handle - &self->handles[0]);
	}
}

static int open_handle_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_controller_t * const self = (rl_controller_t *) peer->userdata;
	rl_msg_t answer;
	rl_uint32 error = RL_NETERR_NOT_FOUND;
	rl_filehandle_t *handle;

	/* map the filename to a handle */
	handle = make_handle(self, msg->open_handle_request.path, msg->open_handle_request.mode, &error);

	/* if we didn't get a file handle, return with an error */
	if (!handle)
	{
		return reply_with_error(peer, msg, error);
	}
	else
	{
		/* reply with the handle */
		RL_MSG_INIT(answer, RL_MSG_OPEN_HANDLE_ANSWER);
		answer.open_handle_answer.hdr_in_reply_to = msg->open_handle_request.hdr_sequence_num;
		answer.open_handle_answer.handle = get_filehandle_index(self, handle);
		answer.open_handle_answer.type = (rl_uint8) handle->type;
		answer.open_handle_answer.size = (rl_uint32) handle->size;
		return peer_transmit_message(peer, &answer);
	}
}

static int close_handle_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_controller_t * const self = (rl_controller_t *) peer->userdata;
	rl_filehandle_t *handle;

	/* Ignore attempts to close virtual input/output */
	if (RL_FILEHANDLE_VIRTUAL_INPUT == msg->close_handle_request.handle ||
		RL_FILEHANDLE_VIRTUAL_OUTPUT == msg->close_handle_request.handle)
		return 0;

	if (NULL == (handle = get_handle_from_id(self, peer, msg->close_handle_request.handle)))
		return reply_with_error(peer, msg, RL_NETERR_INVALID_VALUE);

#if defined(RL_WIN32)
	if (handle->find_handle)
	{
		FindClose(handle->find_handle);
		handle->find_handle = NULL;
	}
	if (INVALID_HANDLE_VALUE != handle->handle && NULL != handle->handle)
		CloseHandle(handle->handle);
	handle->handle = NULL;
#elif defined(RL_POSIX)
	if (handle->dir_handle)
	{
		closedir(handle->dir_handle);
		handle->dir_handle = NULL;
	}
	/* 0 marks a free slot and -1 a directory; anything else is a real
	 * descriptor and has to be given back. */
	if (0 != handle->handle && -1 != handle->handle)
		close(handle->handle);
	handle->handle = 0;
#else
#error "Implement me."
#endif
	return 0;
}

static int find_next_file_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_controller_t * const self = (rl_controller_t *) peer->userdata;
	rl_filehandle_t *handle;
	rl_msg_t answer;

#if defined(RL_WIN32)
	WIN32_FIND_DATAA find_data;
	BOOL reset = msg->find_next_file_request.reset ? TRUE : FALSE;
	BOOL has_file = TRUE;
#elif(defined RL_POSIX)
	struct dirent *dent;
#endif

	if (NULL == (handle = get_handle_from_id(self, peer, msg->find_next_file_request.handle)))
		return reply_with_error(peer, msg, RL_NETERR_INVALID_VALUE);

	if (RL_NODE_TYPE_DIRECTORY != handle->type)
		return reply_with_error(peer, msg, RL_NETERR_NOT_A_DIRECTORY);

#if defined(RL_WIN32)
	do
	{
		if (reset)
		{
			char search_path[MAX_PATH];
			reset = FALSE;

			if (handle->find_handle)
			{
				FindClose(handle->find_handle);
				handle->find_handle = NULL;
			}

			rl_format_msg(search_path, sizeof(search_path), "%s\\*", handle->native_path);

			handle->find_handle = FindFirstFile(search_path, &find_data);
			if (INVALID_HANDLE_VALUE == handle->find_handle)
			{
				handle->find_handle = NULL;
				return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);
			}
		}
		else
		{
			if (!handle->find_handle)
				return reply_with_error(peer, msg, RL_NETERR_INVALID_VALUE);
			else
				has_file = FindNextFile(handle->find_handle, &find_data);
		}
	} while (has_file && '.' == find_data.cFileName[0]);

	RL_MSG_INIT(answer, RL_MSG_FIND_NEXT_FILE_ANSWER);
	answer.find_next_file_answer.hdr_in_reply_to = msg->find_next_file_request.hdr_sequence_num;
	answer.find_next_file_answer.end_of_sequence = has_file ? 0 : 1;

	if (has_file)
	{
		answer.find_next_file_answer.type = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
			RL_NODE_TYPE_DIRECTORY : RL_NODE_TYPE_FILE;
		answer.find_next_file_answer.name = &find_data.cFileName[0];
		answer.find_next_file_answer.size = find_data.nFileSizeLow; /* FIXME: 64-bit file sizes */
	}
	else
	{
		answer.find_next_file_answer.type = RL_NODE_TYPE_DIRECTORY;
		answer.find_next_file_answer.name = "";
		answer.find_next_file_answer.size = 0;
	}

#elif defined(RL_POSIX)
	/* FIXME: This doesn't filter away '.' and '..' */
	if (msg->find_next_file_request.reset && handle->dir_handle)
	{
		closedir(handle->dir_handle);
		handle->dir_handle = NULL;
	}

	if (!handle->dir_handle)
	{
		if (NULL == (handle->dir_handle = opendir(handle->native_path)))
			return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);
	}

	RL_MSG_INIT(answer, RL_MSG_FIND_NEXT_FILE_ANSWER);
	answer.find_next_file_answer.hdr_in_reply_to =
		msg->find_next_file_request.hdr_sequence_num;

	answer.find_next_file_answer.end_of_sequence = 0;

	// Set errno to zero before calling readdir; errno is not changed at end-of-directory
	errno = 0;

	if (NULL == (dent = readdir(handle->dir_handle)))
	{
		if (0 == errno)
		{
			answer.find_next_file_answer.end_of_sequence = 1;
			answer.find_next_file_answer.type = RL_NODE_TYPE_DIRECTORY;
			answer.find_next_file_answer.name = "";
			answer.find_next_file_answer.size = 0;
		}
		else
			return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);
	}
	else
	{
		char item_path[PATH_MAX];
		rl_strbuf_t path;
		struct stat stat_buf;

		rl_strbuf_init(&path, item_path, sizeof(item_path));
		rl_strbuf_append(&path, handle->native_path);
		rl_strbuf_append(&path, "/");
		rl_strbuf_append(&path, dent->d_name);

		/* FIXME: Maybe we should just ignore the item. */
		if (0 != stat(item_path, &stat_buf))
			return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);

		answer.find_next_file_answer.end_of_sequence = 0;
		answer.find_next_file_answer.type = S_ISDIR(stat_buf.st_mode) ?
			RL_NODE_TYPE_DIRECTORY : RL_NODE_TYPE_FILE;
		answer.find_next_file_answer.name = dent->d_name;
		answer.find_next_file_answer.size = stat_buf.st_size;
	}

#else
#error "Implement me"
#endif

	return peer_transmit_message(peer, &answer);
}

static int read_file_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_controller_t * const self = (rl_controller_t *) peer->userdata;
	rl_msg_t answer;
	rl_filehandle_t *handle;
	rl_uint8 read_buffer[4096];

	const rl_msg_read_file_request_t * const request =
		&msg->read_file_request;

#ifdef RL_WIN32
	LARGE_INTEGER pos;
#endif

	if (NULL == (handle = get_handle_from_id(self, peer, request->handle)))
		return reply_with_error(peer, msg, RL_NETERR_INVALID_VALUE);

#ifdef RL_WIN32
	if (INVALID_HANDLE_VALUE == handle->handle)
		return reply_with_error(peer, msg, RL_NETERR_NOT_A_FILE);

	pos.LowPart = request->offset_lo;
	pos.HighPart = request->offset_hi;

	/* Ignore seeks in standard input */
	if (handle != &self->vinput_handle)
	{
	   	if (!SetFilePointerEx(handle->handle, pos, NULL, FILE_BEGIN))
			return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);
	}

	{
		DWORD size_to_read = sizeof(read_buffer);
		DWORD bytes_read = 0;

		if (size_to_read > request->length)
			size_to_read = request->length;

		if (0 == ReadFile(handle->handle, &read_buffer[0], size_to_read, &bytes_read, NULL))
		{
			RL_LOG_DEBUG(("ReadFile failed w/ Win32 error %d", (int) GetLastError()));
			return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);
		}

		RL_LOG_DEBUG(("read %d bytes at offset %d from %s -> %d bytes read", size_to_read, pos.LowPart, handle->native_path, bytes_read));

		RL_MSG_INIT(answer, RL_MSG_READ_FILE_ANSWER);
		answer.read_file_answer.hdr_in_reply_to = request->hdr_sequence_num;
		answer.read_file_answer.data.base = read_buffer;
		answer.read_file_answer.data.length = bytes_read;
		peer_transmit_message(peer, &answer);
	}

#elif defined(RL_POSIX)
	/* Directory handles hold -1 and free or closed slots hold 0, which is also
	 * stdin's descriptor - so only the virtual input handle may read from it. */
	if (RL_NODE_TYPE_FILE != handle->type)
		return reply_with_error(peer, msg, RL_NETERR_NOT_A_FILE);

	if (0 == handle->handle && handle != &self->vinput_handle)
		return reply_with_error(peer, msg, RL_NETERR_INVALID_VALUE);

	{
		ssize_t read_size;
		size_t size_to_read = sizeof(read_buffer);

		if (size_to_read > request->length)
			size_to_read = request->length;

		if (handle == &self->vinput_handle)
		{
			/* Standard input is not seekable, so the offset is meaningless. */
			read_size = read(handle->handle, read_buffer, size_to_read);
		}
		else
		{
			const uint64_t offset =
				((uint64_t) request->offset_hi << 32) | request->offset_lo;
			read_size = pread(handle->handle, read_buffer, size_to_read, (off_t) offset);
		}

		if (-1 == read_size)
			return reply_with_error(peer, msg, RL_NETERR_IO_ERROR);

		RL_MSG_INIT(answer, RL_MSG_READ_FILE_ANSWER);
		answer.read_file_answer.hdr_in_reply_to = request->hdr_sequence_num;
		answer.read_file_answer.data.base = read_buffer;
		answer.read_file_answer.data.length = (rl_uint32) read_size;
		peer_transmit_message(peer, &answer);
	}

#else
#error Implement me.
#endif

	return 0;
}

static int write_file_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_msg_t answer;
	rl_controller_t * const self = (rl_controller_t *) peer->userdata;
	rl_filehandle_t *handle;
	const rl_msg_write_file_request_t * request;

	request	= &msg->write_file_request;

	if (NULL == (handle = get_handle_from_id(self, peer, request->handle)))
		return reply_with_error(peer, msg, RL_NETERR_INVALID_VALUE);

	RL_LOG_DEBUG(("write %d bytes against %s", request->data.length, handle->native_path));

	if (handle == &self->voutput_handle)
	{
		fwrite(request->data.base, 1, request->data.length, stdout);
    /* Flush data right away. */
    fflush(stdout);
	}
	else
	{
		RL_LOG_WARNING(("generic file write not implemented"));
	}

	RL_MSG_INIT(answer, RL_MSG_WRITE_FILE_ANSWER);
	answer.write_file_answer.hdr_in_reply_to = request->hdr_sequence_num;
	peer_transmit_message(peer, &answer);

	return 0;
}

int rl_file_serve(peer_t *peer, const rl_msg_t *msg)
{
	switch (rl_msg_kind_of(msg))
	{
		case RL_MSG_READ_FILE_REQUEST:
			read_file_request(peer, msg);
			break;
		case RL_MSG_WRITE_FILE_REQUEST:
			write_file_request(peer, msg);
			break;
		case RL_MSG_OPEN_HANDLE_REQUEST:
			open_handle_request(peer, msg);
			break;
		case RL_MSG_CLOSE_HANDLE_REQUEST:
			close_handle_request(peer, msg);
			break;
		case RL_MSG_FIND_NEXT_FILE_REQUEST:
			find_next_file_request(peer, msg);
			break;
		default:
		{
			rl_msg_t answer;
			RL_LOG_WARNING(("file server can't handle message '%s'", rl_msg_name(rl_msg_kind_of(msg))));
			RL_MSG_INIT(answer, RL_MSG_ERROR_ANSWER);
			answer.error_answer.hdr_in_reply_to = msg->open_handle_request.hdr_sequence_num;
			answer.error_answer.error_code = RL_NETERR_BAD_REQUEST;
			peer_transmit_message(peer, &answer);
			break;
		}
	}

	return 0;
}

