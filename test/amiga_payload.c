/*
 * End-to-end test payload, launched on the Amiga by rl-controller.
 *
 * It runs from the virtual device the target mounts for the connection, so
 * every file access below travels back over the wire and through amigafs.c.
 * Output goes to the virtual output handle and comes back out of the
 * controller's stdout; the return code comes back as the controller's exit
 * code. Between them that is the whole round trip.
 *
 * Deliberately tiny: this is the tracer bullet that proves the emulator rig
 * works, not a filesystem test suite.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/execbase.h>

#ifndef __VBCC__
struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
#else
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
#endif

#define EXPECTED "hello from the host"

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

int main(const char *args)
{
	char buffer[128];
	BPTR file;
	LONG length;
	int result = RETURN_FAIL;

	SysBase = *((struct ExecBase **)4);

	if (NULL == (DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)))
		return RETURN_FAIL;

	/* No one is around to answer "please insert volume ..." requesters, and a
	 * blocked requester looks exactly like a hung filesystem. Fail instead. */
	((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1;

	/* Relative, on purpose: the target hands us the served directory as our
	 * current directory, so this is the workflow the README advertises. No
	 * fallback to an explicit TBLx: path -- if this fails the launch is broken
	 * (#29) and the test should say so. */
	if (0 == (file = Open((STRPTR)"hello.txt", MODE_OLDFILE)))
	{
		/* Read IoErr() before say(): FPuts()/Flush() are DOS calls of their
		 * own and overwrite it, so reading it later reports their result. */
		const LONG open_error = IoErr();

		say("payload: no current dir, relative open failed (IoErr=");
		saynum(open_error);
		say(")\n");
		goto done;
	}

	say("payload: opened via current dir\n");

	length = Read(file, buffer, (LONG)sizeof(buffer) - 1);
	Close(file);

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
	result = RETURN_OK;

done:
	Flush(Output());
	CloseLibrary((struct Library *)DOSBase);
	return result;
}
