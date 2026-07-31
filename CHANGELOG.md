# Changelog

This project follows semantic versioning.

### v1.1 (2026-07-31)

Security and correctness pass over the whole tree, plus the first automated
tests: host unit tests for the PC-side code and an end-to-end test that boots
an emulated Amiga.

#### Added

- [added] Host unit tests (utest.h) covering the file server, the wire protocol codecs and the string/format utilities, run by ctest on Linux, macOS and Windows.
- [added] Host unit tests for `amigafs.c`, which compiles unmodified for a 32-bit host against the real NDK headers.
- [added] End-to-end test that boots AROS m68k under Amiberry, runs a real `rl-target` and drives it with the host `rl-controller`: executable loading, directory listing, Open/Read/Seek, stdin, subdirectories, a failed open and exit-code propagation.

#### Fixed — remote input handling

- [fixed] A peer could declare an array length with the high bit set and have the file server read gigabytes past the receive buffer.
- [fixed] A read answer's surplus was copied into the client's fixed 4096-byte buffer with an unbounded length taken off the wire, corrupting the heap.
- [fixed] `rl_decode_string` read one byte past the end of the message when validating the terminator, and left the remaining size at -1.
- [fixed] Message headers declaring fewer than 8 bytes stalled the transport permanently; a zero-length `recv()` was reported as a clean disconnect, and `EINTR` tore the connection down.
- [fixed] `fix_path` accepted absolute paths, device prefixes and `..` components from the network, and silently truncated input that did not fit. The Win32 slash conversion read past the end of remote-supplied strings.
- [fixed] Symlinks inside the served directory that pointed outside it were resolved and served; the root is now enforced with `realpath()` (POSIX).
- [fixed] `Lock()` on a name longer than 107 characters smashed the stack in `normalize_object_path`. Over-long names are rejected rather than truncated.
- [fixed] Handles that were closed or never opened resolved to a stale slot, so a closed directory handle still enumerated and a closed file handle could read stdin.
- [fixed] `write_file_request` dereferenced the handle before checking it and answered success for handles that do not exist.

#### Fixed — file server

- [fixed] `read_file_request` ignored the requested length and the high half of the offset: reads past 4 GB wrapped, and every read returned 4096 bytes. Reads of the virtual input handle always failed on POSIX, so remote programs never saw host stdin.
- [fixed] `O_LARGEFILE` was overwritten before it took effect, so files over 2 GB failed to open on 32-bit hosts.
- [fixed] The POSIX `close()` guard was inverted: every file descriptor and directory stream leaked.
- [fixed] Directory listings differed between hosts — UNIX served `.` and `..`, Windows dropped all dotfiles. Both now filter exactly `.` and `..`, and an entry that cannot be `stat()`ed is skipped instead of failing the whole listing.
- [fixed] Directory paths were assembled into a `NAME_MAX` buffer, so long paths truncated and the listing aborted.
- [fixed] Double free on `allocate_lock()`'s error path.

#### Fixed — AmigaOS filesystem handler

- [fixed] `ACTION_DIE` was never answered, leaving the caller waiting forever and orphaning the packet.
- [fixed] `ACTION_PARENT` fabricated a lock on the server's own root handle; unlocking it broke every later root-level operation. It now opens a real handle.
- [fixed] `DupLock()` cloned the server handle id, so unlocking either lock closed the shared handle. Handles are reference counted now.
- [fixed] `ACTION_READ`/`ACTION_WRITE` reported failure as `DOSFALSE`, which readers take for a clean end of file — a failing connection produced silently truncated copies.
- [fixed] `Open()` relative to a current directory other than the root ignored the directory lock and resolved against the volume root.
- [fixed] `FINDINPUT`/`FINDOUTPUT` ran `strchr()`/`strcmp()` over a BSTR that need not be NUL-terminated, and dereferenced a null directory lock while logging.
- [fixed] Wild free on the `LOCATE_OBJECT` error path, and a server handle leaked when the lock allocation failed.
- [fixed] `ExNext()` dereferenced the null lock DOS produces for the volume root.
- [fixed] Every root lock aliased one server handle, so two programs listing the volume consumed each other's entries. Each root lock now gets its own cursor.
- [fixed] `Examine()` returned the whole stored path as `fib_FileName` and left `fib_EntryType` at zero; `Lock()` stored an access mode of zero.
- [fixed] Disconnect left pending operations unanswered and their DOS callers blocked forever, dropped queued packets, deleted the port even when the volume could not be unmounted, and leaked the `rl_amigafs_t`.
- [fixed] An error answer to a fire-and-forget close tore the whole connection down.
- [fixed] Unimplemented actions answered `dp_Res2 = 0`, which names no failure; they now report `ERROR_ACTION_NOT_KNOWN`.

#### Fixed — target and launcher

- [fixed] Under AROS, `SystemTagList()` drops `NP_CurrentDir`, so launched programs came up in `DH0:` instead of the served directory and a relative `Open()` never reached the handler. The launcher now hands over the directory by inheritance, which works on AmigaOS 3.x and AROS alike.
- [fixed] The launch-completion port was read with a bare `GetMsg()`, faulting when two replies coalesced and stranding the second message.
- [fixed] A failed `OpenLibrary()` in the launcher left the parent's message unreplied and the controller waiting forever.
- [fixed] Peer indices were handed out modulo 10 and never released, so the eleventh connection aliased the first — two peers sharing a device name.
- [fixed] Dead peers were freed while still linked into the peer list, giving a use-after-free on the next loop iteration.
- [fixed] The handshake compared `version_major` twice, accepting peers that differ in minor version; and a failed enqueue was overwritten with `PEER_CONNECTED`.
- [fixed] `platform_name` was left uninitialized when `uname()` failed, putting a wild pointer on the wire in the first message of every connection.
- [fixed] A double `close()` on the amiga-init failure path could land on an unrelated live connection.

#### Fixed — controller

- [fixed] A refused connection was treated as a successful connect; `SO_ERROR` now decides. `select()`'s return value was ignored, only the first address from `getaddrinfo()` was ever tried, and success did not leave the loop.
- [fixed] The controller exited 0 on every failure — bad arguments, unresolvable name, nothing listening — so no build could tell whether it ran.
- [fixed] A failed remote spawn made the controller spin until Ctrl-C instead of reporting the error.
- [fixed] A failing `getcwd()` left the served root unspecified; the controller now refuses to start and points at `-fsroot`.

#### Fixed — shared code

- [fixed] `BIG_ENDIAN` collided with glibc's `<endian.h>`, which defines it as 4321 on little-endian machines too. Renamed to `RL_BIG_ENDIAN`; the wire byte order was previously correct only by include-order luck.
- [fixed] A format string ending in a bare `%` ran the parser off the end of the string; `%x`/`%b` sign-extended, and `%d` overflowed on the most negative value.
- [fixed] The log line buffer and cursor were unsynchronized statics shared between the launcher process and the main task.
- [fixed] `rl_string_copy()` reported an exact fit as a truncation.
- [fixed] `rl_configure_socket_blocking()` passed the flags as the `fcntl()` command, with `F_SETFL` missing entirely.

#### Changed

- [changed] Deleted `src/fstest.c`, the standalone handler prototype superseded by `src/amigafs.c`.
- [changed] Seven identical "unsupported action" stubs collapsed into one, and the shared open-handle and failure-reply sequences factored out of the action handlers.
- [changed] The host test build compiles `amigafs.c` with `-Werror`.
