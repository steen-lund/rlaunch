/*
 * Host-side unit tests for rlaunch.
 *
 * Covers the code that runs on the PC: the file server, the wire protocol
 * codecs and the string/format utilities. amigafs.c is Amiga-only and is not
 * reachable from here.
 *
 * Tests assert *correct* behaviour. Where a test documents a bug that is still
 * open it starts with UTEST_SKIP("#N: ...") so CI stays green; deleting that
 * one line is how you verify a fix.
 */

/* file_server.c includes <windows.h>, which drags in the legacy winsock.h and
 * then collides with the winsock2.h that socket_includes.h needs. Keep the
 * production include order and tell windows.h to leave sockets alone instead. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#endif

/* Deliberately first: this drags in the system endian.h, so protocol.h's inline
 * codecs below are parsed with the system BIG_ENDIAN macro already in scope.
 * inline_codecs_survive_system_endian_h fails if they ever key off it again. */
#include "socket_includes.h"

/* file_server.c ships no header and everything interesting in it is static, so
 * pull the whole translation unit in rather than punching holes in it. rl-test
 * must therefore not also link file_server.c. */
#include "file_server.c"

#include "version.h"
#include "third_party/utest.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

UTEST_MAIN();

/* -------------------------------------------------------------------------
 * Harness
 * ------------------------------------------------------------------------- */

/* Out-of-memory injection, defined in util.c. -1 never fails, N fails the Nth
 * allocation from now on. */
extern int rl_test_alloc_fail_in;

/* The ASSERT_* macros only work inside a UTEST body, so helpers use this. */
#define TEST_REQUIRE(expr)                                                     \
	do {                                                                       \
		if (!(expr)) {                                                         \
			fprintf(stderr, "%s:%d: setup failed: %s\n",                       \
					__FILE__, __LINE__, #expr);                                \
			abort();                                                           \
		}                                                                      \
	} while (0)

static int stub_on_message(peer_t *peer, const rl_msg_t *msg)
{
	(void)peer; (void)msg;
	return 0;
}

static int stub_on_connected(peer_t *peer)
{
	(void)peer;
	return 0;
}

/*
 * A peer with no socket. peer_transmit_message() only queues into the
 * transport, so replies can be read straight back out of memory -- no sockets,
 * no select loop, no second process.
 */
static void test_peer_init(peer_t *peer, rl_controller_t *ctl)
{
	static const peer_callbacks_t callbacks = { stub_on_message, stub_on_connected };
	static int sockets_ready = 0;
	struct sockaddr_in addr;

	/* peer_init() formats the address with inet_ntoa(), which returns NULL on
	 * Win32 until WSAStartup() has run. */
	if (!sockets_ready)
	{
		TEST_REQUIRE(0 == rl_init_socket());
		sockets_ready = 1;
	}

	rl_memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	TEST_REQUIRE(0 == peer_init(peer, INVALID_SOCKET, (const struct sockaddr *)&addr,
			&callbacks, PEER_INIT_TARGET, ctl));

	/* Assign directly: peer_set_state() would fire the on_connected callback. */
	peer->state = PEER_CONNECTED;
}

/* Pop the oldest queued reply and decode it. Returns 0 on success. */
static int pop_reply(peer_t *peer, rl_msg_t *out)
{
	rl_transport_t *t = &peer->transport;
	rl_transport_buf_t *buf = t->out_queue;
	int result;

	if (!buf)
		return -1;

	t->out_queue = buf->next;
	if (!t->out_queue)
		t->out_tail = NULL;

	result = rl_decode_msg(buf->buffer, (int)buf->used_size, out);
	rl_transport_free_buffer(t, buf);
	return result;
}

/* Real teardown, so the peer index is released like it is in production. The
 * fd is INVALID_SOCKET; closing that is a no-op that only sets errno. */
static void test_peer_destroy(peer_t *peer)
{
	rl_msg_t drain;
	while (0 == pop_reply(peer, &drain))
		;
	peer_destroy(peer);
}

static void make_temp_dir(char *out, size_t out_size)
{
#if defined(RL_WIN32)
	static int counter = 0;
	char base[MAX_PATH];

	TEST_REQUIRE(0 != GetTempPathA((DWORD)sizeof(base), base));
	/* GetTempPathA leaves a trailing backslash. */
	rl_format_msg(out, out_size, "%srlaunch-test-%d-%d",
			base, (int)GetCurrentProcessId(), counter++);
	TEST_REQUIRE(CreateDirectoryA(out, NULL));
#else
	char template[] = "/tmp/rlaunch-test-XXXXXX";

	TEST_REQUIRE(NULL != mkdtemp(template));
	rl_string_copy(out_size, out, template);
#endif
}

/* A controller serving a scratch directory. */
static void test_controller_init(rl_controller_t *ctl, char *root_out, size_t root_size)
{
	make_temp_dir(root_out, root_size);

	rl_memset(ctl, 0, sizeof(*ctl));
	rl_string_copy(sizeof(ctl->root_handle.native_path), ctl->root_handle.native_path, root_out);
	ctl->root_handle.type = RL_NODE_TYPE_DIRECTORY;
#if defined(RL_WIN32)
	ctl->root_handle.handle = INVALID_HANDLE_VALUE;
#else
	ctl->root_handle.handle = -1;
#endif
}

static void write_file(const char *root, const char *name, const char *contents)
{
	char path[512];
	FILE *fp;

	/* Win32 accepts forward slashes too, so one form works everywhere. */
	rl_format_msg(path, sizeof(path), "%s/%s", root, name);
	fp = fopen(path, "wb");
	TEST_REQUIRE(fp != NULL);
	fwrite(contents, 1, strlen(contents), fp);
	fclose(fp);
}

/* -------------------------------------------------------------------------
 * protocol.c -- string and array codecs
 * ------------------------------------------------------------------------- */

UTEST(protocol, encode_decode_string_roundtrip)
{
	unsigned char buffer[64];
	unsigned char *wcursor = buffer;
	const unsigned char *rcursor = buffer;
	int wsize = (int)sizeof(buffer);
	int rsize;
	const char *decoded = NULL;

	ASSERT_EQ(0, rl_encode_string(&wcursor, &wsize, "hello"));
	rsize = (int)(wcursor - buffer);

	ASSERT_EQ(0, rl_decode_string(&rcursor, &rsize, &decoded));
	ASSERT_STREQ("hello", decoded);
	ASSERT_EQ(0, rsize);
	ASSERT_EQ(wcursor, (unsigned char *)rcursor);
}

UTEST(protocol, decode_string_rejects_length_past_end)
{
	const unsigned char buffer[4] = { 10, 'a', 'b', 0 };
	const unsigned char *cursor = buffer;
	int size = (int)sizeof(buffer);
	const char *decoded = NULL;

	ASSERT_NE(0, rl_decode_string(&cursor, &size, &decoded));
}

UTEST(protocol, decode_string_rejects_payload_without_room_for_terminator)
{
	/* A 3-byte payload needs 5 bytes (length + payload + NUL). Only 4 are
	 * available, so the terminator lives outside the message. */
	const unsigned char buffer[5] = { 3, 'a', 'b', 'c', 0 };
	const unsigned char *cursor = buffer;
	int size = 4;
	const char *decoded = NULL;

	ASSERT_NE(0, rl_decode_string(&cursor, &size, &decoded));
}

UTEST(protocol, decode_array_rejects_length_with_high_bit_set)
{
	/* Regression for the signed-cast bounds check. */
	const unsigned char buffer[8] = { 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 };
	const unsigned char *cursor = buffer;
	int size = (int)sizeof(buffer);
	rl_net_array_t decoded;

	ASSERT_NE(0, rl_decode_array(&cursor, &size, &decoded));
}

UTEST(protocol, messages_encode_in_big_endian_wire_order)
{
	/* The Amiga is big-endian and the wire format follows it. Encode a known
	 * 32-bit field and look for it in the buffer; finding the bytes reversed
	 * means the host is emitting little-endian and no Amiga will understand
	 * it. Uses rl_encode_msg (compiled into common) rather than the inline
	 * helpers so this reflects what actually ships. */
	rl_msg_t msg;
	unsigned char buffer[128];
	size_t used = 0;
	size_t i;
	int found_big = 0, found_little = 0;

	RL_MSG_INIT(msg, RL_MSG_READ_FILE_REQUEST);
	msg.read_file_request.handle = 0x01020304u;

	ASSERT_EQ(0, rl_encode_msg(&msg, buffer, (int)sizeof(buffer), &used));
	ASSERT_GT(used, (size_t)4);

	for (i = 0; i + 4 <= used; ++i)
	{
		if (0 == memcmp(&buffer[i], "\x01\x02\x03\x04", 4))
			found_big = 1;
		if (0 == memcmp(&buffer[i], "\x04\x03\x02\x01", 4))
			found_little = 1;
	}

	ASSERT_TRUE(found_big);
	ASSERT_FALSE(found_little);
}

UTEST(protocol, inline_codecs_survive_system_endian_h)
{
	/* This TU includes socket_includes.h, which drags in glibc's <endian.h>
	 * and its BIG_ENDIAN=4321 macro. The inline helpers must key off the
	 * project's own RL_BIG_ENDIAN, so a system header cannot flip the wire
	 * order in one translation unit and not the next. */
	unsigned char buffer[4];
	unsigned char *cursor = buffer;
	const unsigned char *read_cursor = buffer;
	rl_uint32 decoded = 0;

	rl_encode_int4(&cursor, 0x01020304u);
	ASSERT_EQ(0, memcmp(buffer, "\x01\x02\x03\x04", 4));

	ASSERT_EQ(0, rl_decode_int4(&read_cursor, &decoded));
	ASSERT_EQ(0x01020304u, decoded);
}

/* -------------------------------------------------------------------------
 * util.c -- formatting engine
 * ------------------------------------------------------------------------- */

UTEST(format, basics)
{
	char buffer[64];

	rl_format_msg(buffer, sizeof(buffer), "%s=%d", "answer", 42);
	ASSERT_STREQ("answer=42", buffer);

	rl_format_msg(buffer, sizeof(buffer), "[%5d]", 7);
	ASSERT_STREQ("[    7]", buffer);

	rl_format_msg(buffer, sizeof(buffer), "[%-5d]", 7);
	ASSERT_STREQ("[7    ]", buffer);

	rl_format_msg(buffer, sizeof(buffer), "[%05d]", 7);
	ASSERT_STREQ("[00007]", buffer);

	rl_format_msg(buffer, sizeof(buffer), "100%%");
	ASSERT_STREQ("100%", buffer);
}

UTEST(format, truncates_without_overflowing_the_destination)
{
	char buffer[8];

	rl_format_msg(buffer, sizeof(buffer), "%s", "0123456789abcdef");
	ASSERT_EQ((size_t)7, strlen(buffer));
}

UTEST(format, trailing_percent_is_not_read_past_the_format_string)
{
	char buffer[64];

	rl_format_msg(buffer, sizeof(buffer), "100%");
	ASSERT_STREQ("100%", buffer);

	/* Same for a percent whose flags and width run into the terminator. */
	rl_format_msg(buffer, sizeof(buffer), "hi %-05");
	ASSERT_STREQ("hi %-05", buffer);
}

UTEST(format, hex_does_not_sign_extend)
{
	char buffer[64];

	rl_format_msg(buffer, sizeof(buffer), "%x", 0x80000000u);
	ASSERT_STREQ("80000000", buffer);

	rl_format_msg(buffer, sizeof(buffer), "%x", 0xffffffffu);
	ASSERT_STREQ("ffffffff", buffer);

	rl_format_msg(buffer, sizeof(buffer), "%b", 0x80000000u);
	ASSERT_STREQ("10000000000000000000000000000000", buffer);
}

UTEST(format, handles_the_most_negative_integer)
{
	char buffer[64];

	/* %d fetches an int and format_integer_signed widens it to ssize_t, so on a
	 * 64-bit host this never reaches the most-negative ssize_t. It does on the
	 * 32-bit Amiga build, where the magnitude is now accumulated unsigned. */
	rl_format_msg(buffer, sizeof(buffer), "%d", INT_MIN);
	ASSERT_STREQ("-2147483648", buffer);
}

UTEST(format, string_copy_of_an_exact_fit_is_not_truncation)
{
	char buffer[4];

	ASSERT_EQ(0, rl_string_copy(sizeof(buffer), buffer, "abc"));
	ASSERT_STREQ("abc", buffer);

	/* One character more than fits is still reported as truncation. */
	ASSERT_EQ(-1, rl_string_copy(sizeof(buffer), buffer, "abcd"));
	ASSERT_STREQ("abc", buffer);
}

/* -------------------------------------------------------------------------
 * file_server.c -- path handling
 * ------------------------------------------------------------------------- */

UTEST(fix_path, joins_against_the_served_root)
{
	char dest[260];
	char expected[512];
	char root[256];

	/* A real directory: the POSIX check resolves the root, so a made-up one
	 * would be rejected before the join is even looked at. */
	make_temp_dir(root, sizeof(root));

#ifdef RL_WIN32
	rl_format_msg(expected, sizeof(expected), "%s\\sub\\file.txt", root);
#else
	rl_format_msg(expected, sizeof(expected), "%s/sub/file.txt", root);
#endif

	ASSERT_EQ(0, fix_path(dest, sizeof(dest), "sub/file.txt", root));
	ASSERT_STREQ(expected, dest);
}

UTEST(fix_path, rejects_traversal_outside_the_served_root)
{
	char dest[260];

	ASSERT_NE(0, fix_path(dest, sizeof(dest), "../../etc/passwd", "/srv/root"));
}

UTEST(fix_path, rejects_input_that_does_not_fit)
{
	char dest[64];
	char overlong[300];

	memset(overlong, 'a', sizeof(overlong) - 1);
	overlong[sizeof(overlong) - 1] = '\0';

	ASSERT_NE(0, fix_path(dest, sizeof(dest), overlong, "/srv/root"));
}

#if defined(RL_POSIX)
UTEST(fix_path, rejects_a_symlink_pointing_out_of_the_served_root)
{
	char dest[260];
	char base[256], root[512], sibling[512], link[512];

	/* The escape target is a sibling whose name starts with the root's, so this
	 * also pins the prefix comparison to a directory boundary. */
	make_temp_dir(base, sizeof(base));
	rl_format_msg(root, sizeof(root), "%s/root", base);
	rl_format_msg(sibling, sizeof(sibling), "%s/rootless", base);
	rl_format_msg(link, sizeof(link), "%s/out", root);

	TEST_REQUIRE(0 == mkdir(root, 0777));
	TEST_REQUIRE(0 == mkdir(sibling, 0777));
	TEST_REQUIRE(0 == symlink(sibling, link));

	ASSERT_NE(0, fix_path(dest, sizeof(dest), "out/secret.txt", root));
}
#endif

/* -------------------------------------------------------------------------
 * file_server.c -- request handlers
 * ------------------------------------------------------------------------- */

static void open_request(rl_msg_t *msg, const char *path, int mode)
{
	RL_MSG_INIT(*msg, RL_MSG_OPEN_HANDLE_REQUEST);
	msg->open_handle_request.hdr_sequence_num = 1;
	msg->open_handle_request.path = path;
	msg->open_handle_request.mode = (rl_uint8)mode;
}

UTEST(file_server, opening_a_missing_file_replies_with_not_found)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];

	test_controller_init(&ctl, root, sizeof(root));
	test_peer_init(&peer, &ctl);

	open_request(&request, "nope.txt", RL_OPENFLAG_READ);
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_ERROR_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)RL_NETERR_NOT_FOUND, reply.error_answer.error_code);

	test_peer_destroy(&peer);
}

UTEST(file_server, opening_and_reading_a_file_returns_its_contents)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];
	rl_uint32 handle;

	test_controller_init(&ctl, root, sizeof(root));
	write_file(root, "hello.txt", "hello amiga");
	test_peer_init(&peer, &ctl);

	open_request(&request, "hello.txt", RL_OPENFLAG_READ);
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_OPEN_HANDLE_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint8)RL_NODE_TYPE_FILE, reply.open_handle_answer.type);
	ASSERT_EQ((rl_uint32)11, reply.open_handle_answer.size);
	handle = reply.open_handle_answer.handle;

	RL_MSG_INIT(request, RL_MSG_READ_FILE_REQUEST);
	request.read_file_request.hdr_sequence_num = 2;
	request.read_file_request.handle = handle;
	request.read_file_request.offset_lo = 6;
	request.read_file_request.length = 5;
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_READ_FILE_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)5, reply.read_file_answer.data.length);
	ASSERT_EQ(0, memcmp(reply.read_file_answer.data.base, "amiga", 5));

	test_peer_destroy(&peer);
}

UTEST(file_server, read_honours_the_requested_length)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];

	test_controller_init(&ctl, root, sizeof(root));
	write_file(root, "big.txt", "0123456789");
	test_peer_init(&peer, &ctl);

	open_request(&request, "big.txt", RL_OPENFLAG_READ);
	rl_file_serve(&peer, &request);
	ASSERT_EQ(0, pop_reply(&peer, &reply));

	RL_MSG_INIT(request, RL_MSG_READ_FILE_REQUEST);
	request.read_file_request.handle = reply.open_handle_answer.handle;
	request.read_file_request.length = 4;
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_READ_FILE_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)4, reply.read_file_answer.data.length);

	test_peer_destroy(&peer);
}

UTEST(file_server, reading_a_directory_handle_reports_not_a_file)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];

	test_controller_init(&ctl, root, sizeof(root));
	test_peer_init(&peer, &ctl);

	RL_MSG_INIT(request, RL_MSG_READ_FILE_REQUEST);
	request.read_file_request.handle = (rl_uint32)-1; /* the root directory */
	request.read_file_request.length = 16;
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_ERROR_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)RL_NETERR_NOT_A_FILE, reply.error_answer.error_code);

	test_peer_destroy(&peer);
}

UTEST(file_server, reading_a_closed_handle_does_not_fall_through_to_stdin)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];
	rl_uint32 handle;

	test_controller_init(&ctl, root, sizeof(root));
	write_file(root, "hello.txt", "hi");
	test_peer_init(&peer, &ctl);

	open_request(&request, "hello.txt", RL_OPENFLAG_READ);
	rl_file_serve(&peer, &request);
	ASSERT_EQ(0, pop_reply(&peer, &reply));
	handle = reply.open_handle_answer.handle;

	RL_MSG_INIT(request, RL_MSG_CLOSE_HANDLE_REQUEST);
	request.close_handle_request.handle = handle;
	rl_file_serve(&peer, &request);

	RL_MSG_INIT(request, RL_MSG_READ_FILE_REQUEST);
	request.read_file_request.handle = handle;
	request.read_file_request.length = 16;
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_ERROR_ANSWER, (int)rl_msg_kind_of(&reply));

	test_peer_destroy(&peer);
}

/*
 * offset_hi carries the top 32 bits of the offset. A read at 4 GB of a tiny
 * file must land past the end and come back empty, not wrap to offset_lo.
 */
UTEST(file_server, read_uses_the_high_half_of_the_offset)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];

	test_controller_init(&ctl, root, sizeof(root));
	write_file(root, "small.txt", "0123456789");
	test_peer_init(&peer, &ctl);

	open_request(&request, "small.txt", RL_OPENFLAG_READ);
	rl_file_serve(&peer, &request);
	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_OPEN_HANDLE_ANSWER, (int)rl_msg_kind_of(&reply));

	RL_MSG_INIT(request, RL_MSG_READ_FILE_REQUEST);
	request.read_file_request.handle = reply.open_handle_answer.handle;
	request.read_file_request.offset_hi = 1;
	request.read_file_request.offset_lo = 0;
	request.read_file_request.length = 4;
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_READ_FILE_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)0, reply.read_file_answer.data.length);

	test_peer_destroy(&peer);
}

#if defined(RL_POSIX)
/*
 * The virtual input handle carries host stdin, descriptor 0 - the same value a
 * free or closed handle slot holds. Reading it has to work anyway, and the
 * offset has to be ignored because a pipe cannot seek. Win32 keeps stdin in a
 * real HANDLE, so the ambiguity is POSIX-only.
 */
UTEST(file_server, reading_the_virtual_input_handle_returns_host_stdin)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];
	int pipe_fds[2];
	int saved_stdin;

	test_controller_init(&ctl, root, sizeof(root));

	ctl.vinput_handle.type = RL_NODE_TYPE_FILE;
	rl_string_copy(sizeof(ctl.vinput_handle.native_path),
			ctl.vinput_handle.native_path, "(virtual input)");

	/* An unseekable stdin, so a stray pread() here would fail with ESPIPE. */
	TEST_REQUIRE(0 == pipe(pipe_fds));
	TEST_REQUIRE(10 == write(pipe_fds[1], "from stdin", 10));
	close(pipe_fds[1]);

	saved_stdin = dup(STDIN_FILENO);
	TEST_REQUIRE(-1 != saved_stdin);
	TEST_REQUIRE(-1 != dup2(pipe_fds[0], STDIN_FILENO));
	close(pipe_fds[0]);
	ctl.vinput_handle.handle = STDIN_FILENO;

	test_peer_init(&peer, &ctl);

	RL_MSG_INIT(request, RL_MSG_READ_FILE_REQUEST);
	request.read_file_request.handle = RL_FILEHANDLE_VIRTUAL_INPUT;
	request.read_file_request.offset_lo = 4096; /* unseekable: must be ignored */
	request.read_file_request.length = 16;
	rl_file_serve(&peer, &request);

	TEST_REQUIRE(-1 != dup2(saved_stdin, STDIN_FILENO));
	close(saved_stdin);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_READ_FILE_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)10, reply.read_file_answer.data.length);
	ASSERT_EQ(0, memcmp(reply.read_file_answer.data.base, "from stdin", 10));

	test_peer_destroy(&peer);
}
#endif

UTEST(file_server, closing_a_handle_releases_the_descriptor)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];
#if defined(RL_WIN32)
	HANDLE raw;
	DWORD handle_flags;
#else
	int raw;
#endif

	test_controller_init(&ctl, root, sizeof(root));
	write_file(root, "hello.txt", "hi");
	test_peer_init(&peer, &ctl);

	open_request(&request, "hello.txt", RL_OPENFLAG_READ);
	rl_file_serve(&peer, &request);
	ASSERT_EQ(0, pop_reply(&peer, &reply));
	raw = ctl.handles[reply.open_handle_answer.handle].handle;

	RL_MSG_INIT(request, RL_MSG_CLOSE_HANDLE_REQUEST);
	request.close_handle_request.handle = reply.open_handle_answer.handle;
	rl_file_serve(&peer, &request);

#if defined(RL_WIN32)
	ASSERT_FALSE(GetHandleInformation(raw, &handle_flags));
#else
	ASSERT_EQ(-1, fcntl(raw, F_GETFD));
	ASSERT_EQ(EBADF, errno);
#endif

	test_peer_destroy(&peer);
}

UTEST(file_server, writing_to_an_unknown_handle_replies_with_an_error)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];

	test_controller_init(&ctl, root, sizeof(root));
	test_peer_init(&peer, &ctl);

	RL_MSG_INIT(request, RL_MSG_WRITE_FILE_REQUEST);
	request.write_file_request.handle = RL_MAX_FILE_HANDLES + 1;
	request.write_file_request.data.base = (const rl_uint8 *)"x";
	request.write_file_request.data.length = 1;
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_ERROR_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)RL_NETERR_INVALID_VALUE, reply.error_answer.error_code);

	test_peer_destroy(&peer);
}

UTEST(file_server, an_unhandled_request_replies_with_bad_request)
{
	rl_controller_t ctl;
	peer_t peer;
	rl_msg_t request, reply;
	char root[256];

	test_controller_init(&ctl, root, sizeof(root));
	test_peer_init(&peer, &ctl);

	RL_MSG_INIT(request, RL_MSG_PING_REQUEST);
	rl_file_serve(&peer, &request);

	ASSERT_EQ(0, pop_reply(&peer, &reply));
	ASSERT_EQ(RL_MSG_ERROR_ANSWER, (int)rl_msg_kind_of(&reply));
	ASSERT_EQ((rl_uint32)RL_NETERR_BAD_REQUEST, reply.error_answer.error_code);

	test_peer_destroy(&peer);
}

/* -------------------------------------------------------------------------
 * peer handshake
 * ------------------------------------------------------------------------- */

/*
 * Everything on the handshake path is static in peer.c, and peer.c is linked
 * in (not #included) so it cannot be reached the way file_server.c is. The
 * transport is the seam instead: encode the request straight into the input
 * buffer and let rl_transport_update() run the normal delivery callbacks.
 */
static void feed_message(peer_t *peer, const rl_msg_t *msg)
{
	rl_iobuf_t *in = &peer->transport.inbuf;
	size_t used = 0;

	TEST_REQUIRE(0 == rl_encode_msg(msg, (rl_uint8 *)in->write_cursor,
			(int)(in->end_address - in->write_cursor), &used));
	in->write_cursor += used;

	rl_transport_update(&peer->transport);
}

static void make_handshake(rl_msg_t *msg, int major, int minor)
{
	rl_msg_handshake_request_t *req = &msg->handshake_request;

	req->hdr_type = RL_MSG_HANDSHAKE_REQUEST;
	req->hdr_flags = 0;
	req->hdr_sequence_num = 0;
	req->version_major = (rl_uint8) major;
	req->version_minor = (rl_uint8) minor;
	req->platform_name = "test";
	req->node_name = "test-node";
	req->platform_version = "1";
	req->password_hash = "****";
}

/* A peer waiting for the handshake it is about to be given. */
static void handshake_peer_init(peer_t *peer, rl_controller_t *ctl)
{
	test_peer_init(peer, ctl);
	peer->state = PEER_WAIT_HANDSHAKE;
}

UTEST(peer_handshake, an_identical_version_connects)
{
	peer_t peer;
	rl_controller_t ctl;
	rl_msg_t handshake;

	rl_memset(&ctl, 0, sizeof(ctl));
	handshake_peer_init(&peer, &ctl);

	make_handshake(&handshake, RLAUNCH_VER_MAJOR, RLAUNCH_VER_MINOR);
	feed_message(&peer, &handshake);

	ASSERT_EQ(PEER_CONNECTED, peer.state);
	/* A target answers with its own handshake, so something must be on the wire. */
	ASSERT_TRUE(NULL != peer.transport.out_queue);

	test_peer_destroy(&peer);
}

UTEST(peer_handshake, a_differing_minor_version_is_rejected)
{
	peer_t peer;
	rl_controller_t ctl;
	rl_msg_t handshake;

	rl_memset(&ctl, 0, sizeof(ctl));
	handshake_peer_init(&peer, &ctl);

	make_handshake(&handshake, RLAUNCH_VER_MAJOR, RLAUNCH_VER_MINOR + 1);
	feed_message(&peer, &handshake);

	ASSERT_EQ(PEER_ERROR, peer.state);

	test_peer_destroy(&peer);
}

UTEST(peer_handshake, a_differing_major_version_is_rejected)
{
	peer_t peer;
	rl_controller_t ctl;
	rl_msg_t handshake;

	rl_memset(&ctl, 0, sizeof(ctl));
	handshake_peer_init(&peer, &ctl);

	make_handshake(&handshake, RLAUNCH_VER_MAJOR + 1, RLAUNCH_VER_MINOR);
	feed_message(&peer, &handshake);

	ASSERT_EQ(PEER_ERROR, peer.state);

	test_peer_destroy(&peer);
}

/*
 * A target answers an accepted handshake with its own. If queueing that answer
 * fails there is nothing on the wire for the controller to wait for, so the
 * peer must stay in error rather than report itself connected.
 */
UTEST(peer_handshake, a_failed_reply_does_not_report_the_peer_connected)
{
	peer_t peer;
	rl_controller_t ctl;
	rl_msg_t handshake;

	rl_memset(&ctl, 0, sizeof(ctl));
	handshake_peer_init(&peer, &ctl);
	ASSERT_EQ(PEER_INIT_TARGET, peer.init_mode);

	make_handshake(&handshake, RLAUNCH_VER_MAJOR, RLAUNCH_VER_MINOR);

	/* Starve the buffer the outgoing handshake is encoded into. This fails the
	 * *next* allocation rather than a named call site, so it only stays aimed at
	 * rl_transport_alloc_buffer() while nothing else on the receive-handshake
	 * path allocates. Decoding does not, today. */
	rl_test_alloc_fail_in = 0;
	feed_message(&peer, &handshake);
	rl_test_alloc_fail_in = -1;

	ASSERT_EQ(PEER_ERROR, peer.state);
	ASSERT_TRUE(NULL == peer.transport.out_queue);

	test_peer_destroy(&peer);
}

/* -------------------------------------------------------------------------
 * transport framing
 * ------------------------------------------------------------------------- */

/*
 * A header declaring a length below the 8-byte minimum used to come straight
 * back out of peer_peek_incoming(). Zero read as "not enough data yet", so the
 * transport consumed nothing and re-peeked the same four bytes on every update
 * -- a permanent stall with no ping to break it during the handshake.
 */
UTEST(transport_framing, an_undersized_declared_length_is_an_error)
{
	static const rl_uint8 header[8] =
	{
		RL_MSG_PING_REQUEST, 0, /* hdr_type, hdr_flags */
		0, 0,                   /* hdr_length -- bogus */
		0, 0, 0, 0              /* hdr_sequence_num */
	};

	peer_t peer;
	rl_controller_t ctl;
	rl_iobuf_t *in;

	rl_memset(&ctl, 0, sizeof(ctl));
	handshake_peer_init(&peer, &ctl);

	in = &peer.transport.inbuf;
	rl_memcpy(in->write_cursor, header, sizeof(header));
	in->write_cursor += sizeof(header);

	ASSERT_EQ(RL_TRANSPORT_ERROR, rl_transport_update(&peer.transport));

	test_peer_destroy(&peer);
}

/* -------------------------------------------------------------------------
 * peer index
 * ------------------------------------------------------------------------- */

UTEST(peer_index, live_peers_never_share_an_index)
{
	static const peer_callbacks_t callbacks = { stub_on_message, stub_on_connected };
	peer_t peers[10];
	peer_t overflow, reused;
	rl_controller_t ctl;
	struct sockaddr_in addr;
	unsigned int seen = 0;
	int freed, i;

	rl_memset(&ctl, 0, sizeof(ctl));
	rl_memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	for (i = 0; i < 10; ++i)
	{
		test_peer_init(&peers[i], &ctl);
		ASSERT_TRUE(peers[i].peer_index >= 0 && peers[i].peer_index < 10);
		ASSERT_TRUE(0 == (seen & (1u << peers[i].peer_index)));
		seen |= 1u << peers[i].peer_index;
	}

	/* All ten slots are live, so an eleventh connection has to be refused
	 * rather than handed a duplicate index. */
	ASSERT_NE(0, peer_init(&overflow, INVALID_SOCKET, (const struct sockaddr *)&addr,
			&callbacks, PEER_INIT_TARGET, &ctl));

	/* Disconnecting returns the slot to the pool. */
	freed = peers[3].peer_index;
	test_peer_destroy(&peers[3]);

	test_peer_init(&reused, &ctl);
	ASSERT_EQ(freed, reused.peer_index);
	test_peer_destroy(&reused);

	for (i = 0; i < 10; ++i)
	{
		if (3 != i)
			test_peer_destroy(&peers[i]);
	}
}

/* -------------------------------------------------------------------------
 * peer list
 * ------------------------------------------------------------------------- */

/* Only the next pointers matter here, so skip peer_init() and its socket. */
UTEST(peer_list, removes_a_node_from_the_middle)
{
	peer_t a, b, c;
	peer_t *head = &a;

	a.next = &b;
	b.next = &c;
	c.next = NULL;

	peer_list_remove(&head, &b);

	ASSERT_TRUE(head == &a);
	ASSERT_TRUE(a.next == &c);
	ASSERT_TRUE(c.next == NULL);
	ASSERT_TRUE(b.next == NULL);
}

UTEST(peer_list, removes_the_head_and_the_tail)
{
	peer_t a, b, c;
	peer_t *head = &a;

	a.next = &b;
	b.next = &c;
	c.next = NULL;

	peer_list_remove(&head, &a);
	ASSERT_TRUE(head == &b);

	peer_list_remove(&head, &c);
	ASSERT_TRUE(head == &b);
	ASSERT_TRUE(b.next == NULL);

	peer_list_remove(&head, &b);
	ASSERT_TRUE(head == NULL);
}

UTEST(peer_list, removing_a_node_that_is_not_linked_is_a_no_op)
{
	peer_t a, b, stray;
	peer_t *head = &a;

	a.next = &b;
	b.next = NULL;
	stray.next = NULL;

	peer_list_remove(&head, &stray);

	ASSERT_TRUE(head == &a);
	ASSERT_TRUE(a.next == &b);
	ASSERT_TRUE(b.next == NULL);
}
