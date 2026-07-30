/*
 * End-to-end test payload, launched on the Amiga by rl-controller.
 *
 * It runs from the virtual device the target mounts for the connection, so
 * every file access below travels back over the wire and through amigafs.c.
 * Output goes to the virtual output handle and comes back out of the
 * controller's stdout; the return code comes back as the controller's exit
 * code. Between them that is the whole round trip.
 *
 * Deliberately small: it walks the paths a user walks -- open, read, seek,
 * list a directory -- and nothing else. Corner cases belong in the host-side
 * unit tests, which are far cheaper to run than a booted emulator.
 *
 * Given the argument "fail" it returns FAIL_MODE_CODE instead of 0, so the
 * script can prove the remote return code really travels back rather than
 * assuming it because zero came out of a zero-returning program.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/execbase.h>

#ifndef __VBCC__
struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
#else
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
#endif

#define EXPECTED "hello from the host"
/* Reading from here has to skip "hello " and land inside the file. */
#define TAIL_OFFSET 6
#define EXPECTED_TAIL "from the host"
#define FAIL_MODE_CODE 42

static void say(const char *text)
{
	FPuts(Output(), (STRPTR)text);
	/* Flush every line: if a later call hangs, we still see how far we got. */
	Flush(Output());
}

static void saynum(long value)
{
	char digits[16];
	int i = (int)sizeof(digits) - 1;

	digits[i] = '\0';
	if (value < 0)
	{
		say("-");
		value = -value;
	}
	do
	{
		digits[--i] = (char)('0' + (value % 10));
		value /= 10;
	} while (value > 0);

	say(&digits[i]);
}

static int same(const char *a, const char *b)
{
	while (*a && *b)
	{
		if (*a++ != *b++)
			return 0;
	}
	return *a == *b;
}

/*
 * Report a DOS call that failed. The error comes in as an argument rather than
 * being read here: FPuts()/Flush() are DOS calls of their own and overwrite
 * IoErr(), so it has to be picked up before anything is said.
 */
static void say_failed(const char *what, LONG error)
{
	say(what);
	say(" (IoErr=");
	saynum(error);
	say(")\n");
}

/*
 * Open hello.txt through the current directory, read it whole, then seek back
 * into it and read the tail. Returns non-zero if all of that matched.
 */
static int check_file(void)
{
	char buffer[128];
	BPTR file;
	LONG length;
	int result = 0;

	/* Relative, on purpose: the target hands us the served directory as our
	 * current directory, so this is the workflow the README advertises. No
	 * fallback to an explicit TBLx: path -- if this fails the launch is broken
	 * (#29) and the test should say so. */
	if (0 == (file = Open((STRPTR)"hello.txt", MODE_OLDFILE)))
	{
		say_failed("payload: no current dir, relative open failed", IoErr());
		return 0;
	}

	say("payload: opened via current dir\n");

	length = Read(file, buffer, (LONG)sizeof(buffer) - 1);

	if (length < 0)
	{
		say("payload: read failed\n");
		goto done;
	}

	buffer[length] = '\0';

	if (!same(buffer, EXPECTED))
	{
		say("payload: got \"");
		say(buffer);
		say("\", wanted \"" EXPECTED "\"\n");
		goto done;
	}

	say("payload: read back \"" EXPECTED "\"\n");

	/* The read above left us at the end of the file, so this seek moves
	 * backwards -- a forward-only implementation cannot fake it. */
	if (-1 == Seek(file, TAIL_OFFSET, OFFSET_BEGINNING))
	{
		say_failed("payload: seek failed", IoErr());
		goto done;
	}

	length = Read(file, buffer, (LONG)sizeof(buffer) - 1);

	if (length < 0)
	{
		say("payload: read after seek failed\n");
		goto done;
	}

	buffer[length] = '\0';

	if (!same(buffer, EXPECTED_TAIL))
	{
		say("payload: after seek got \"");
		say(buffer);
		say("\", wanted \"" EXPECTED_TAIL "\"\n");
		goto done;
	}

	say("payload: seek read back \"" EXPECTED_TAIL "\"\n");
	result = 1;

done:
	Close(file);
	return result;
}

/*
 * Enumerate the served directory and confirm both files the script puts there
 * come back. Every name is echoed, so a mismatch shows what was served instead
 * of just reporting that something was missing.
 */
static int check_directory(void)
{
	struct FileInfoBlock *fib = NULL;
	BPTR dir;
	int found_file = 0;
	int found_payload = 0;
	int result = 0;

	/* An empty name locks the current directory, which is the served root. */
	if (0 == (dir = Lock((STRPTR)"", SHARED_LOCK)))
	{
		say_failed("payload: cannot lock the current dir", IoErr());
		return 0;
	}

	/* AllocDosObject rather than a local: ExNext() needs the block longword
	 * aligned and DOS owns that guarantee, not the compiler. */
	if (NULL == (fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL)))
	{
		say("payload: no memory for a FileInfoBlock\n");
		goto done;
	}

	if (!Examine(dir, fib))
	{
		say_failed("payload: Examine failed", IoErr());
		goto done;
	}

	while (ExNext(dir, fib))
	{
		say("payload: entry \"");
		say((const char *)fib->fib_FileName);
		say("\"\n");

		if (same((const char *)fib->fib_FileName, "hello.txt"))
			found_file = 1;
		else if (same((const char *)fib->fib_FileName, "rl-payload"))
			found_payload = 1;
	}

	/* The loop above ended on a failing ExNext(), so IoErr() still describes it
	 * -- nothing has been said since. Anything but "that was the last one" is a
	 * broken enumeration rather than the end of one. */
	if (ERROR_NO_MORE_ENTRIES != IoErr())
	{
		say_failed("payload: ExNext failed", IoErr());
		goto done;
	}

	if (!found_file || !found_payload)
	{
		say("payload: listing did not contain both served files\n");
		goto done;
	}

	say("payload: listed hello.txt and rl-payload\n");
	result = 1;

done:
	if (fib)
		FreeDosObject(DOS_FIB, fib);
	UnLock(dir);
	return result;
}

/*
 * Was the payload asked to fail? ReadArgs() rather than main()'s argc/argv:
 * src/target.c reads its own command line the same way, and that keeps this
 * independent of what the startup code makes of a command line SystemTagList()
 * assembled on the other side of the wire.
 */
static int fail_mode_requested(void)
{
	LONG values[1] = { 0 };
	struct RDArgs *args;
	int requested;

	/* Without a CLI to read the command line from, ReadArgs() falls back to
	 * reading Input() -- and there is nobody here to type at it. */
	if (!((struct Process *)FindTask(NULL))->pr_CLI)
		return 0;

	if (NULL == (args = ReadArgs((STRPTR)"FAIL/S", &values[0], NULL)))
		return 0;

	requested = 0 != values[0];
	FreeArgs(args);
	return requested;
}

int main(void)
{
	int fail_mode;
	int result = RETURN_FAIL;

	SysBase = *((struct ExecBase **)4);

	if (NULL == (DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)))
		return RETURN_FAIL;

	/* No one is around to answer "please insert volume ..." requesters, and a
	 * blocked requester looks exactly like a hung filesystem. Fail instead. */
	((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1;

	fail_mode = fail_mode_requested();

	if (check_file() && check_directory())
	{
		result = RETURN_OK;

		if (fail_mode)
		{
			say("payload: returning the failure code as asked\n");
			result = FAIL_MODE_CODE;
		}
	}

	Flush(Output());
	CloseLibrary((struct Library *)DOSBase);
	return result;
}
