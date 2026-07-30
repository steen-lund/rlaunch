/*
 * Unit tests for amigafs.c, run on the host.
 *
 * amigafs.c compiles unmodified against the real NDK headers with _NO_INLINE
 * (which turns the proto/ headers into plain prototypes), so the code under
 * test is the same code that ships. Only the AmigaOS entry points are faked --
 * see amiga_stubs.c.
 *
 * Each test hands the filesystem a DosPacket, checks the rlnet request it puts
 * on the wire, feeds back an answer, and checks how the packet was replied to.
 *
 * Built -m32 inside the vbcc image; see test/amigafs-host-test.sh.
 */

#include "config.h"
#include "amigafs.h"
#include "protocol.h"
#include "rlnet.h"
#include "util.h"

/* Deliberately not peer.h: under RL_AMIGA it drags in socket_types.h, whose
 * socklen_t typedef fights with the host's. amigafs.h only ever handles the
 * peer as an opaque pointer, so the declaration below is all that is needed. */
struct peer_tag;

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/utest.h"

UTEST_MAIN();

void rl_test_reset_dos_entries(void);

/* util.c's host allocator: -1 never fails, N fails the Nth allocation from
 * now on. The only way into this file's out-of-memory paths. */
extern int rl_test_alloc_fail_in;

#define TEST_REQUIRE(expr)                                                     \
	do {                                                                       \
		if (!(expr)) {                                                         \
			fprintf(stderr, "%s:%d: setup failed: %s\n",                       \
					__FILE__, __LINE__, #expr);                                \
			abort();                                                           \
		}                                                                      \
	} while (0)

/* ------------------------------------------------------------------------
 * Harness
 * ------------------------------------------------------------------------ */

/*
 * amigafs.c reaches the network through exactly one function, so standing in
 * for it is the whole of the peer side. Messages are captured rather than
 * encoded, which keeps peer.c and transport.c -- and the Amiga socket headers
 * they drag in -- out of this build entirely.
 */
#define MAX_CAPTURED 16

static rl_msg_t captured[MAX_CAPTURED];
static int captured_count;
static int captured_taken;

int peer_transmit_message(struct peer_tag *self, const rl_msg_t *msg)
{
	(void)self;
	TEST_REQUIRE(captured_count < MAX_CAPTURED);
	captured[captured_count++] = *msg;
	return 0;
}

typedef struct fs_fixture_tag
{
	rl_amigafs_t fs;
	struct MsgPort reply_port;
} fs_fixture_t;

static void fixture_init(fs_fixture_t *fix)
{
	rl_test_reset_dos_entries();
	captured_count = 0;
	captured_taken = 0;

	rl_memset(fix, 0, sizeof(*fix));

	fix->reply_port.mp_MsgList.lh_Head = (struct Node *)&fix->reply_port.mp_MsgList.lh_Tail;
	fix->reply_port.mp_MsgList.lh_Tail = NULL;
	fix->reply_port.mp_MsgList.lh_TailPred = (struct Node *)&fix->reply_port.mp_MsgList.lh_Head;

	/* A NULL peer is fine: amigafs only dereferences it to print peer->ident,
	 * and only when debug logging is on, which it is not here. */
	TEST_REQUIRE(0 == rl_amigafs_init(&fix->fs, NULL, "TBL0"));
}

static void fixture_destroy(fs_fixture_t *fix)
{
	rl_amigafs_destroy(&fix->fs);
}

/* Take the oldest message the filesystem sent to the controller. */
static int pop_request(fs_fixture_t *fix, rl_msg_t *out)
{
	(void)fix;

	if (captured_taken >= captured_count)
		return -1;

	*out = captured[captured_taken++];
	return 0;
}

/* A DosPacket plus the Message that carries it, laid out the way AmigaDOS
 * does it: the packet hangs off the message's ln_Name. */
typedef struct test_packet_tag
{
	struct StandardPacket sp;
} test_packet_t;

static struct DosPacket *make_packet(fs_fixture_t *fix, test_packet_t *tp, LONG type)
{
	rl_memset(tp, 0, sizeof(*tp));

	tp->sp.sp_Msg.mn_Node.ln_Name = (char *)&tp->sp.sp_Pkt;
	tp->sp.sp_Pkt.dp_Link = &tp->sp.sp_Msg;
	tp->sp.sp_Pkt.dp_Port = &fix->reply_port;
	tp->sp.sp_Pkt.dp_Type = type;
	return &tp->sp.sp_Pkt;
}

/* Hand the packet to the filesystem the way DOS would. */
static void send_packet(fs_fixture_t *fix, test_packet_t *tp)
{
	PutMsg(fix->fs.device_port, &tp->sp.sp_Msg);
	rl_amigafs_process_device_message(&fix->fs);
}

/* Did the filesystem reply to the packet yet? */
static int packet_was_replied(fs_fixture_t *fix)
{
	return NULL != GetMsg(&fix->reply_port);
}

/* Build a BSTR in a caller-supplied buffer and hand back a BPTR to it. */
static BSTR make_bstr(char *storage, const char *text)
{
	size_t length = strlen(text);

	storage[0] = (char)length;
	memcpy(storage + 1, text, length);
	storage[length + 1] = '\0';
	return MKBADDR(storage);
}

/* ------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

UTEST(amigafs, init_registers_and_destroy_removes_the_device)
{
	fs_fixture_t fix;

	fixture_init(&fix);
	ASSERT_TRUE(fix.fs.device_port != NULL);
	ASSERT_TRUE(fix.fs.device_list != NULL);

	/* Anything still holding the device name after teardown will send packets
	 * to a port with no server, which hangs the caller forever. */
	rl_amigafs_destroy(&fix.fs);
	ASSERT_TRUE(NULL == FindDosEntry(NULL, (CONST_STRPTR)"TBL0", ~0u));
}

UTEST(amigafs, locate_object_asks_the_controller_for_the_path)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *root;
	char name[64];
	rl_msg_t request;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);

	packet = make_packet(&fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	packet->dp_Arg2 = (LONG)make_bstr(name, "hello.txt");
	packet->dp_Arg3 = ACCESS_READ;
	send_packet(&fix, &tp);

	/* The answer has to come from the controller, so the packet must be held,
	 * not replied to. */
	ASSERT_FALSE(packet_was_replied(&fix));

	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_OPEN_HANDLE_REQUEST, (int)rl_msg_kind_of(&request));
	ASSERT_STREQ("hello.txt", request.open_handle_request.path);

	fixture_destroy(&fix);
}

UTEST(amigafs, locate_object_rejects_a_name_that_does_not_fit)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *root;
	/* MKBADDR() drops the low two bits, so BSTR storage has to be aligned. */
	LONG storage[(257 + sizeof(LONG) - 1) / sizeof(LONG)];
	char name[256];
	rl_msg_t request;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);

	/* The longest name a BSTR can carry, well past the 108-byte path buffer
	 * the filesystem copies it into. */
	memset(name, 'a', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';

	packet = make_packet(&fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	packet->dp_Arg2 = (LONG)make_bstr((char *)storage, name);
	packet->dp_Arg3 = ACCESS_READ;
	send_packet(&fix, &tp);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(0, packet->dp_Res1);
	ASSERT_NE(0, packet->dp_Res2);

	/* Nothing should have been asked of the controller. */
	ASSERT_EQ(-1, pop_request(&fix, &request));

	fixture_destroy(&fix);
}

UTEST(amigafs, a_locate_answer_completes_the_pending_packet)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *root;
	char name[64];
	rl_msg_t request, answer;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);

	packet = make_packet(&fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	packet->dp_Arg2 = (LONG)make_bstr(name, "hello.txt");
	packet->dp_Arg3 = ACCESS_READ;
	send_packet(&fix, &tp);

	ASSERT_EQ(0, pop_request(&fix, &request));

	RL_MSG_INIT(answer, RL_MSG_OPEN_HANDLE_ANSWER);
	answer.open_handle_answer.hdr_in_reply_to = request.open_handle_request.hdr_sequence_num;
	answer.open_handle_answer.handle = 7;
	answer.open_handle_answer.type = RL_NODE_TYPE_FILE;
	answer.open_handle_answer.size = 11;
	rl_amigafs_process_network_message(&fix.fs, &answer);

	ASSERT_TRUE(packet_was_replied(&fix));
	/* LOCATE_OBJECT answers with a BPTR to the new lock, not a boolean. */
	ASSERT_NE(0, packet->dp_Res1);
	ASSERT_EQ(0, packet->dp_Res2);
	ASSERT_TRUE(BADDR(packet->dp_Res1) != NULL);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_failed_root_lock_does_not_free_a_stale_pending_op)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *root;
	char name[64];
	rl_msg_t request;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);

	/* An empty name against the root lock resolves to the root itself, which
	 * is answered locally -- no pending op is ever allocated on this path. */
	packet = make_packet(&fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	packet->dp_Arg2 = (LONG)make_bstr(name, "");
	packet->dp_Arg3 = ACCESS_READ;

	/* The next allocation is the root lock. Failing it takes the error path,
	 * which used to free whatever the uninitialized pending_op pointed at. */
	rl_test_alloc_fail_in = 0;
	send_packet(&fix, &tp);
	rl_test_alloc_fail_in = -1;

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(0, packet->dp_Res1);
	ASSERT_EQ((LONG)ERROR_NO_FREE_STORE, packet->dp_Res2);

	/* Handled locally, so the controller should not have heard about it. */
	ASSERT_EQ(-1, pop_request(&fix, &request));

	fixture_destroy(&fix);
}

UTEST(amigafs, a_locate_answer_we_cannot_hold_gives_the_handle_back)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *root;
	char name[64];
	rl_msg_t request, answer, close_request;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);

	packet = make_packet(&fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	packet->dp_Arg2 = (LONG)make_bstr(name, "hello.txt");
	packet->dp_Arg3 = ACCESS_READ;
	send_packet(&fix, &tp);

	ASSERT_EQ(0, pop_request(&fix, &request));

	RL_MSG_INIT(answer, RL_MSG_OPEN_HANDLE_ANSWER);
	answer.open_handle_answer.hdr_in_reply_to = request.open_handle_request.hdr_sequence_num;
	answer.open_handle_answer.handle = 7;
	answer.open_handle_answer.type = RL_NODE_TYPE_FILE;
	answer.open_handle_answer.size = 11;

	/* Fail the lock allocation so the answer arrives with nowhere to go. The
	 * server handle is already open at this point. */
	rl_test_alloc_fail_in = 0;
	rl_amigafs_process_network_message(&fix.fs, &answer);
	rl_test_alloc_fail_in = -1;

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(0, packet->dp_Res1);
	ASSERT_EQ((LONG)ERROR_NO_FREE_STORE, packet->dp_Res2);

	/* Without this the handle stays open in the server's fixed table forever. */
	ASSERT_EQ(0, pop_request(&fix, &close_request));
	ASSERT_EQ(RL_MSG_CLOSE_HANDLE_REQUEST, (int)rl_msg_kind_of(&close_request));
	ASSERT_EQ(7u, (unsigned)close_request.close_handle_request.handle);

	fixture_destroy(&fix);
}

/* Walk a LOCATE_OBJECT through to its answer and hand back the lock it made,
 * which is the only way to get a lock carrying a real server handle id. */
static struct FileLock *locate_lock_with_mode(fs_fixture_t *fix, const char *path,
		rl_uint32 handle_id, int node_type, LONG mode)
{
	test_packet_t tp;
	struct DosPacket *packet;
	char name[128];
	rl_msg_t request, answer;

	packet = make_packet(fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = 0;
	packet->dp_Arg2 = (LONG)make_bstr(name, path);
	packet->dp_Arg3 = mode;
	send_packet(fix, &tp);

	TEST_REQUIRE(0 == pop_request(fix, &request));

	RL_MSG_INIT(answer, RL_MSG_OPEN_HANDLE_ANSWER);
	answer.open_handle_answer.hdr_in_reply_to = request.open_handle_request.hdr_sequence_num;
	answer.open_handle_answer.handle = handle_id;
	answer.open_handle_answer.type = node_type;
	answer.open_handle_answer.size = 0;
	rl_amigafs_process_network_message(&fix->fs, &answer);

	TEST_REQUIRE(packet_was_replied(fix));
	TEST_REQUIRE(0 != packet->dp_Res1);
	return (struct FileLock *)BADDR(packet->dp_Res1);
}

static struct FileLock *locate_lock(fs_fixture_t *fix, const char *path, rl_uint32 handle_id, int node_type)
{
	return locate_lock_with_mode(fix, path, handle_id, node_type, ACCESS_READ);
}

UTEST(amigafs, parent_asks_the_controller_to_open_the_parent_directory)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	rl_msg_t request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "dir/hello.txt", 7, RL_NODE_TYPE_FILE);

	packet = make_packet(&fix, &tp, ACTION_PARENT);
	packet->dp_Arg1 = (LONG)MKBADDR(lock);
	send_packet(&fix, &tp);

	/* The parent needs a handle of its own, so the packet has to wait. */
	ASSERT_FALSE(packet_was_replied(&fix));

	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_OPEN_HANDLE_REQUEST, (int)rl_msg_kind_of(&request));
	ASSERT_STREQ("dir", request.open_handle_request.path);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_parent_lock_holds_the_handle_the_server_gave_it)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	rl_msg_t request, answer, close_request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "dir/hello.txt", 7, RL_NODE_TYPE_FILE);

	packet = make_packet(&fix, &tp, ACTION_PARENT);
	packet->dp_Arg1 = (LONG)MKBADDR(lock);
	send_packet(&fix, &tp);
	ASSERT_EQ(0, pop_request(&fix, &request));

	RL_MSG_INIT(answer, RL_MSG_OPEN_HANDLE_ANSWER);
	answer.open_handle_answer.hdr_in_reply_to = request.open_handle_request.hdr_sequence_num;
	answer.open_handle_answer.handle = 9;
	answer.open_handle_answer.type = RL_NODE_TYPE_DIRECTORY;
	answer.open_handle_answer.size = 0;
	rl_amigafs_process_network_message(&fix.fs, &answer);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_NE(0, packet->dp_Res1);
	ASSERT_EQ(0, packet->dp_Res2);

	/* Parent() hands back a shared lock, whichever branch built it. */
	ASSERT_EQ((LONG)SHARED_LOCK, ((struct FileLock *)BADDR(packet->dp_Res1))->fl_Access);

	/* Unlocking the parent must close the parent's handle. A fabricated ~0u
	 * would land on the server's root handle and kill the connection. */
	rl_amigafs_free_lock(&fix.fs, (struct FileLock *)BADDR(packet->dp_Res1));
	ASSERT_EQ(0, pop_request(&fix, &close_request));
	ASSERT_EQ(RL_MSG_CLOSE_HANDLE_REQUEST, (int)rl_msg_kind_of(&close_request));
	ASSERT_EQ(9u, (unsigned)close_request.close_handle_request.handle);

	fixture_destroy(&fix);
}

UTEST(amigafs, the_parent_of_a_root_level_file_is_the_root)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	rl_msg_t request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "hello.txt", 7, RL_NODE_TYPE_FILE);

	packet = make_packet(&fix, &tp, ACTION_PARENT);
	packet->dp_Arg1 = (LONG)MKBADDR(lock);
	send_packet(&fix, &tp);

	/* Answered locally -- the root handle needs nothing from the controller. */
	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_NE(0, packet->dp_Res1);
	ASSERT_EQ(0, packet->dp_Res2);
	ASSERT_EQ(-1, pop_request(&fix, &request));

	fixture_destroy(&fix);
}

UTEST(amigafs, the_root_has_no_parent)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *root;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);

	packet = make_packet(&fix, &tp, ACTION_PARENT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	send_packet(&fix, &tp);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ((LONG)DOSFALSE, packet->dp_Res1);
	ASSERT_EQ((LONG)ERROR_OBJECT_NOT_FOUND, packet->dp_Res2);

	fixture_destroy(&fix);
}

UTEST(amigafs, parent_of_a_null_lock_is_rejected)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;

	fixture_init(&fix);

	/* Parent() on the root of a volume that has none hands the handler a NULL
	 * lock; dereferencing it before the check faulted. */
	packet = make_packet(&fix, &tp, ACTION_PARENT);
	packet->dp_Arg1 = 0;
	send_packet(&fix, &tp);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ((LONG)DOSFALSE, packet->dp_Res1);
	ASSERT_EQ((LONG)ERROR_OBJECT_NOT_FOUND, packet->dp_Res2);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_duplicated_lock_keeps_the_handle_until_both_are_freed)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock, *copy;
	rl_msg_t request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "dir/hello.txt", 7, RL_NODE_TYPE_FILE);

	packet = make_packet(&fix, &tp, ACTION_COPY_DIR);
	packet->dp_Arg1 = (LONG)MKBADDR(lock);
	send_packet(&fix, &tp);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_NE(0, packet->dp_Res1);
	copy = (struct FileLock *)BADDR(packet->dp_Res1);
	ASSERT_TRUE(copy != lock);

	/* Both locks name the same server handle, so freeing one of them must not
	 * close it -- the survivor would then read whatever file the server put in
	 * that slot next. */
	rl_amigafs_free_lock(&fix.fs, copy);
	ASSERT_EQ(-1, pop_request(&fix, &request));

	rl_amigafs_free_lock(&fix.fs, lock);
	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_CLOSE_HANDLE_REQUEST, (int)rl_msg_kind_of(&request));
	ASSERT_EQ(7u, (unsigned)request.close_handle_request.handle);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_failed_close_does_not_tear_down_the_connection)
{
	fs_fixture_t fix;
	struct FileLock *lock;
	rl_msg_t request, answer;

	fixture_init(&fix);
	lock = locate_lock(&fix, "hello.txt", 7, RL_NODE_TYPE_FILE);

	rl_amigafs_free_lock(&fix.fs, lock);
	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_CLOSE_HANDLE_REQUEST, (int)rl_msg_kind_of(&request));

	/* The close is fire-and-forget, so its error answer matches no pending
	 * operation. A non-zero return here is PEER_ERROR: the whole volume would
	 * go away over a handle this side has already discarded. */
	RL_MSG_INIT(answer, RL_MSG_ERROR_ANSWER);
	answer.error_answer.hdr_in_reply_to = request.close_handle_request.hdr_sequence_num;
	answer.error_answer.error_code = RL_NETERR_INVALID_VALUE;
	ASSERT_EQ(0, rl_amigafs_process_network_message(&fix.fs, &answer));

	fixture_destroy(&fix);
}

/* Reply to whatever request is outstanding with a server-side error. */
static void fail_pending_request(fs_fixture_t *fix, const rl_msg_t *request)
{
	rl_msg_t answer;

	RL_MSG_INIT(answer, RL_MSG_ERROR_ANSWER);
	answer.error_answer.hdr_in_reply_to = request->read_file_request.hdr_sequence_num;
	answer.error_answer.error_code = RL_NETERR_IO_ERROR;
	rl_amigafs_process_network_message(&fix->fs, &answer);
}

UTEST(amigafs, a_failed_read_is_not_reported_as_end_of_file)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	char buffer[16];
	rl_msg_t request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "hello.txt", 7, RL_NODE_TYPE_FILE);

	packet = make_packet(&fix, &tp, ACTION_READ);
	packet->dp_Arg1 = (LONG)lock;
	packet->dp_Arg2 = (LONG)buffer;
	packet->dp_Arg3 = (LONG)sizeof(buffer);
	send_packet(&fix, &tp);

	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_READ_FILE_REQUEST, (int)rl_msg_kind_of(&request));
	ASSERT_FALSE(packet_was_replied(&fix));

	fail_pending_request(&fix, &request);

	/* dp_Res1 is a byte count for READ: zero is a clean end of file, so a
	 * caller told zero here truncates the copy without ever seeing an error. */
	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(-1, packet->dp_Res1);
	ASSERT_NE(0, packet->dp_Res2);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_failed_write_is_not_reported_as_zero_bytes_written)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	char buffer[16];
	rl_msg_t request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "hello.txt", 7, RL_NODE_TYPE_FILE);

	memset(buffer, 'x', sizeof(buffer));
	packet = make_packet(&fix, &tp, ACTION_WRITE);
	packet->dp_Arg1 = (LONG)lock;
	packet->dp_Arg2 = (LONG)buffer;
	packet->dp_Arg3 = (LONG)sizeof(buffer);
	send_packet(&fix, &tp);

	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_WRITE_FILE_REQUEST, (int)rl_msg_kind_of(&request));

	fail_pending_request(&fix, &request);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(-1, packet->dp_Res1);
	ASSERT_NE(0, packet->dp_Res2);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_read_that_never_reaches_the_wire_fails_the_same_way)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	char buffer[16];
	rl_msg_t request;

	fixture_init(&fix);
	lock = locate_lock(&fix, "hello.txt", 7, RL_NODE_TYPE_FILE);

	packet = make_packet(&fix, &tp, ACTION_READ);
	packet->dp_Arg1 = (LONG)lock;
	packet->dp_Arg2 = (LONG)buffer;
	packet->dp_Arg3 = (LONG)sizeof(buffer);

	/* The pending operation is the next allocation; failing it takes the
	 * handler's local error path rather than any network one. */
	rl_test_alloc_fail_in = 0;
	send_packet(&fix, &tp);
	rl_test_alloc_fail_in = -1;

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(-1, packet->dp_Res1);
	ASSERT_EQ((LONG)ERROR_NO_FREE_STORE, packet->dp_Res2);
	ASSERT_EQ(-1, pop_request(&fix, &request));

	fixture_destroy(&fix);
}

UTEST(amigafs, a_failed_lock_still_answers_with_dosfalse)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	char name[64];
	rl_msg_t request;

	fixture_init(&fix);

	packet = make_packet(&fix, &tp, ACTION_LOCATE_OBJECT);
	packet->dp_Arg1 = 0;
	packet->dp_Arg2 = (LONG)make_bstr(name, "hello.txt");
	packet->dp_Arg3 = ACCESS_READ;
	send_packet(&fix, &tp);

	ASSERT_EQ(0, pop_request(&fix, &request));
	fail_pending_request(&fix, &request);

	/* The boolean and BPTR-returning actions keep zero as their failure
	 * value; only the byte-count ones changed. */
	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ(0, packet->dp_Res1);
	ASSERT_NE(0, packet->dp_Res2);

	fixture_destroy(&fix);
}

/* A FileInfoBlock the handlers can be handed: MKBADDR() drops the low two bits,
 * so it has to be aligned. */
typedef union fib_storage_tag
{
	struct FileInfoBlock fib;
	LONG alignment;
} fib_storage_t;

UTEST(amigafs, examine_names_only_the_final_path_component)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	struct FileLock *lock;
	fib_storage_t storage;

	fixture_init(&fix);
	lock = locate_lock(&fix, "dir/hello.txt", 7, RL_NODE_TYPE_FILE);

	memset(&storage, 0, sizeof(storage));
	packet = make_packet(&fix, &tp, ACTION_EXAMINE_OBJECT);
	packet->dp_Arg1 = (LONG)MKBADDR(lock);
	packet->dp_Arg2 = (LONG)MKBADDR(&storage.fib);
	send_packet(&fix, &tp);

	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ((LONG)DOSTRUE, packet->dp_Res1);

	/* A caller that rebuilds a path from the parent plus this name gets
	 * DEV:dir/dir/hello.txt when the whole path is handed back here. */
	ASSERT_EQ(9, (int)(unsigned char)storage.fib.fib_FileName[0]);
	ASSERT_STREQ("hello.txt", (const char *)storage.fib.fib_FileName + 1);

	/* ExNext() fills both in, so a caller reading fib_EntryType off an
	 * Examine() must not see the zero a memset left behind. */
	ASSERT_EQ(-1L, storage.fib.fib_DirEntryType);
	ASSERT_EQ(storage.fib.fib_DirEntryType, storage.fib.fib_EntryType);

	fixture_destroy(&fix);
}

UTEST(amigafs, examine_next_on_a_null_lock_is_answered)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;
	fib_storage_t storage;
	rl_msg_t request;

	fixture_init(&fix);

	/* DOS hands the handler a NULL lock for the volume root; reading a handle
	 * out of it faulted. */
	memset(&storage, 0, sizeof(storage));
	packet = make_packet(&fix, &tp, ACTION_EXAMINE_NEXT);
	packet->dp_Arg1 = 0;
	packet->dp_Arg2 = (LONG)MKBADDR(&storage.fib);
	send_packet(&fix, &tp);

	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_FIND_NEXT_FILE_REQUEST, (int)rl_msg_kind_of(&request));

	fixture_destroy(&fix);
}

/* Start an enumeration on a root lock and answer the handle open it needs,
 * returning the id the following FIND_NEXT_FILE_REQUEST carried. */
static rl_uint32 begin_root_enumeration(fs_fixture_t *fix, struct FileLock *root,
		test_packet_t *tp, fib_storage_t *storage, rl_uint32 handle_id)
{
	struct DosPacket *packet;
	rl_msg_t request, answer;

	memset(storage, 0, sizeof(*storage));
	packet = make_packet(fix, tp, ACTION_EXAMINE_NEXT);
	packet->dp_Arg1 = (LONG)MKBADDR(root);
	packet->dp_Arg2 = (LONG)MKBADDR(&storage->fib);
	send_packet(fix, tp);

	TEST_REQUIRE(0 == pop_request(fix, &request));
	TEST_REQUIRE(RL_MSG_OPEN_HANDLE_REQUEST == rl_msg_kind_of(&request));

	RL_MSG_INIT(answer, RL_MSG_OPEN_HANDLE_ANSWER);
	answer.open_handle_answer.hdr_in_reply_to = request.open_handle_request.hdr_sequence_num;
	answer.open_handle_answer.handle = handle_id;
	answer.open_handle_answer.type = RL_NODE_TYPE_DIRECTORY;
	answer.open_handle_answer.size = 0;
	rl_amigafs_process_network_message(&fix->fs, &answer);

	TEST_REQUIRE(0 == pop_request(fix, &request));
	TEST_REQUIRE(RL_MSG_FIND_NEXT_FILE_REQUEST == rl_msg_kind_of(&request));
	return request.find_next_file_request.handle;
}

UTEST(amigafs, two_root_locks_enumerate_through_handles_of_their_own)
{
	fs_fixture_t fix;
	test_packet_t first_tp, second_tp;
	fib_storage_t first_fib, second_fib;
	struct FileLock *first, *second;

	fixture_init(&fix);
	first = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	second = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(first != NULL && second != NULL);

	/* The server keeps the directory cursor per handle, so two programs
	 * listing the volume through one handle silently skip each other's
	 * entries. */
	ASSERT_EQ(3u, (unsigned)begin_root_enumeration(&fix, first, &first_tp, &first_fib, 3));
	ASSERT_EQ(4u, (unsigned)begin_root_enumeration(&fix, second, &second_tp, &second_fib, 4));

	fixture_destroy(&fix);
}

UTEST(amigafs, an_enumerated_root_lock_gives_its_handle_back)
{
	fs_fixture_t fix;
	test_packet_t tp;
	fib_storage_t storage;
	struct FileLock *root;
	rl_msg_t request;

	fixture_init(&fix);
	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);

	/* Before it enumerates, a root lock names the server's own root handle,
	 * which closing would take the connection's root away. */
	rl_amigafs_free_lock(&fix.fs, root);
	ASSERT_EQ(-1, pop_request(&fix, &request));

	root = rl_amigafs_alloc_root_lock(&fix.fs, SHARED_LOCK);
	ASSERT_TRUE(root != NULL);
	ASSERT_EQ(5u, (unsigned)begin_root_enumeration(&fix, root, &tp, &storage, 5));

	rl_amigafs_free_lock(&fix.fs, root);
	ASSERT_EQ(0, pop_request(&fix, &request));
	ASSERT_EQ(RL_MSG_CLOSE_HANDLE_REQUEST, (int)rl_msg_kind_of(&request));
	ASSERT_EQ(5u, (unsigned)request.close_handle_request.handle);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_lock_keeps_the_access_mode_it_was_asked_for)
{
	fs_fixture_t fix;
	struct FileLock *shared, *exclusive;

	fixture_init(&fix);

	shared = locate_lock(&fix, "hello.txt", 7, RL_NODE_TYPE_FILE);
	ASSERT_EQ((LONG)SHARED_LOCK, shared->fl_Access);

	/* Zero is neither SHARED_LOCK nor EXCLUSIVE_LOCK; anything reading
	 * fl_Access sees a value DOS has no meaning for. */
	exclusive = locate_lock_with_mode(&fix, "hello.txt", 8, RL_NODE_TYPE_FILE, ACCESS_WRITE);
	ASSERT_EQ((LONG)EXCLUSIVE_LOCK, exclusive->fl_Access);

	fixture_destroy(&fix);
}

UTEST(amigafs, a_die_packet_is_answered)
{
	fs_fixture_t fix;
	test_packet_t tp;
	struct DosPacket *packet;

	fixture_init(&fix);
	packet = make_packet(&fix, &tp, ACTION_DIE);
	send_packet(&fix, &tp);

	/* Whoever sent it blocks on its reply port, so refusing still has to be
	 * said out loud. */
	ASSERT_TRUE(packet_was_replied(&fix));
	ASSERT_EQ((LONG)DOSFALSE, packet->dp_Res1);
	ASSERT_EQ((LONG)ERROR_OBJECT_IN_USE, packet->dp_Res2);

	fixture_destroy(&fix);
}
