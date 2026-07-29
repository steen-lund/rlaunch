#!/bin/bash
#
# Build and run the amigafs unit tests on the host.
#
# amigafs.c is Amiga-only source, but it compiles unmodified for a 32-bit host
# against the real NDK headers: _NO_INLINE turns the proto/ headers into plain
# prototypes, and amiga_stubs.c supplies the dozen AmigaOS entry points it
# calls. 32-bit is required -- BPTR arithmetic shifts addresses right by two.
#
# Runs inside the vbcc image because that is where the NDK headers live; the
# image is the same one the Amiga build already needs.
set -euo pipefail

IMAGE=rlaunch-vbcc
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GEN=${1:-build-amiga/generated}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "$IMAGE image missing -- configure the amiga build first:"
	echo "  cmake -B build-amiga -DCMAKE_TOOLCHAIN_FILE=cmake/amiga-vbcc.cmake"
	exit 1
fi

test -f "$ROOT/$GEN/rlnet.c" || { echo "generated rlnet.c missing; build the amiga target first"; exit 1; }

docker run --rm -u "$(id -u):$(id -g)" \
	-v "$ROOT:$ROOT" -w "$ROOT" "$IMAGE" bash -eu -c "
OUT=\$(mktemp -d)
# __USE_NEW_TIMEVAL__ is the NDK's own switch for code that also uses the POSIX
# struct timeval; without it dos/dosextens.h and the host headers collide.
AMIGA_FLAGS='-m32 -g -O1 -Wall -D__AMIGA__ -D_NO_INLINE -D__USE_NEW_TIMEVAL__ -I/opt/vbcc/NDK3.2/Include_H'

# The code under test and the harness need the Amiga headers.
for src in src/amigafs.c test/amigafs_test.c test/amiga_stubs.c; do
	gcc \$AMIGA_FLAGS -Isrc -I$GEN -Itest -c \"\$src\" -o \"\$OUT/\$(basename \$src .c).o\"
done

# The portable pieces build as ordinary host code. util.h's Amiga branch only
# selects equivalent fixed-width typedefs, so the layouts match.
for src in src/util.c src/protocol.c $GEN/rlnet.c; do
	gcc -m32 -g -O1 -Isrc -I$GEN -c \"\$src\" -o \"\$OUT/\$(basename \$src .c).o\"
done

gcc -m32 -o \"\$OUT/rl-amigafs-test\" \"\$OUT\"/*.o
\"\$OUT/rl-amigafs-test\"
"
