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

	if (0 != (file = Open((STRPTR)"hello.txt", MODE_OLDFILE)))
	{
		say("payload: opened via current dir\n");
	}
	else
	{
		/* The target is supposed to hand us the device root as our current
		 * directory, but does not, so fall back to the device we were launched
		 * from. GetProgramName() gives e.g. "TBL1:rl-payload"; the index varies
		 * with how many peers have connected, so it cannot be hardcoded. */
		char path[128];
		int i = 0;

		say("payload: no current dir (IoErr=");
		saynum(IoErr());
		say("), falling back to the launch device\n");

		if (GetProgramName((STRPTR)path, (LONG)sizeof(path) - 16))
		{
			while (path[i] && path[i] != ':')
				++i;

			if (path[i] == ':')
			{
				const char *tail = "hello.txt";
				++i;
				while (*tail)
					path[i++] = *tail++;
				path[i] = '\0';

				if (0 != (file = Open((STRPTR)path, MODE_OLDFILE)))
				{
					say("payload: opened via ");
					say(path);
					say("\n");
				}
			}
		}
	}

	if (0 == file)
	{
		say("payload: cannot open hello.txt by any route\n");
		goto done;
	}

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
