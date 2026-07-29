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

/* file_server.c ships no header and everything interesting in it is static, so
 * pull the whole translation unit in rather than punching holes in it. rl-test
 * must therefore not also link file_server.c. */
#include "file_server.c"

#include "socket_includes.h"
#include "third_party/utest.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

UTEST_MAIN();

/* -------------------------------------------------------------------------
 * Harness
 * ------------------------------------------------------------------------- */

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

static void test_peer_destroy(peer_t *peer)
{
	rl_msg_t drain;
	while (0 == pop_reply(peer, &drain))
		;
	rl_transport_destroy(&peer->transport);
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

	UTEST_SKIP("#4: rl_decode_string off-by-one reads one byte past the message");

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

	UTEST_SKIP("#24: a bare trailing %% consumes the NUL and reads past the format string");

	rl_format_msg(buffer, sizeof(buffer), "100%");
	ASSERT_STREQ("100%", buffer);
}

UTEST(format, hex_does_not_sign_extend)
{
	char buffer[64];

	UTEST_SKIP("#24: %x fetches a signed int, so values >= 0x80000000 sign-extend");

	rl_format_msg(buffer, sizeof(buffer), "%x", 0x80000000u);
	ASSERT_STREQ("80000000", buffer);
}

UTEST(format, handles_the_most_negative_integer)
{
	char buffer[64];

	/* This passes on a 64-bit host: %d fetches an int and format_integer_signed
	 * widens it to ssize_t, so negating it is safe. The out-of-bounds negation
	 * in issue #24 needs value == SSIZE_MIN, which %d cannot deliver here --
	 * but it is reachable on the 32-bit Amiga build. */
	rl_format_msg(buffer, sizeof(buffer), "%d", INT_MIN);
	ASSERT_STREQ("-2147483648", buffer);
}

UTEST(format, string_copy_of_an_exact_fit_is_not_truncation)
{
	char buffer[4];

	UTEST_SKIP("#24: rl_string_copy reports truncation when the source fits exactly");

	ASSERT_EQ(0, rl_string_copy(sizeof(buffer), buffer, "abc"));
	ASSERT_STREQ("abc", buffer);
}

/* -------------------------------------------------------------------------
 * file_server.c -- path handling
 * ------------------------------------------------------------------------- */

UTEST(fix_path, joins_against_the_served_root)
{
	char dest[260];

	ASSERT_EQ(0, fix_path(dest, sizeof(dest), "sub/file.txt", "/srv/root"));
#ifdef RL_WIN32
	ASSERT_STREQ("/srv/root\\sub\\file.txt", dest);
#else
	ASSERT_STREQ("/srv/root/sub/file.txt", dest);
#endif
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

#if defined(RL_POSIX)
	/* The Win32 branch clamps to request->length correctly. */
	UTEST_SKIP("#22: the POSIX read_file_request ignores request->length and always reads 4096");
#endif

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

#if defined(RL_POSIX)
	/* Win32 marks directories with INVALID_HANDLE_VALUE and rejects them. */
	UTEST_SKIP("#22: POSIX directory handles store -1, fall through to pread() and report IO_ERROR");
#endif

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
