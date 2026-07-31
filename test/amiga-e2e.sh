#!/bin/bash
#
# End-to-end test: boot AROS m68k under Amiberry, run rl-target on it, then
# drive it with the host rl-controller.
#
# Nothing here is downloaded except Amiberry itself -- its package ships the
# AROS ROMs, and bsdsocket.library is emulated, so the target's TCP port shows
# up as an ordinary listening socket on this machine. No Workbench install and
# no Kickstart ROM are involved.
#
# Usage: test/amiga-e2e.sh <build-dir> <amiga-build-dir>
set -euo pipefail

BUILD=${1:-build}
AMIGA_BUILD=${2:-build-amiga}
AMIBERRY_VERSION=8.2.2

WORK=$(mktemp -d)
AMIBERRY_PID=""

cleanup() {
	[ -n "$AMIBERRY_PID" ] && kill "$AMIBERRY_PID" 2>/dev/null || true
	rm -rf "$WORK"
}
trap cleanup EXIT

echo "== installing amiberry $AMIBERRY_VERSION =="
curl -sSfL -o "$WORK/amiberry.zip" \
	"https://github.com/BlitterStudio/amiberry/releases/download/v${AMIBERRY_VERSION}/amiberry-ubuntu-24.04-amd64.zip"
unzip -q -o "$WORK/amiberry.zip" -d "$WORK/pkg"
mv "$WORK"/pkg/*.deb "$WORK/amiberry.deb"
sudo apt-get update -qq
sudo apt-get install -y -qq xvfb libgl1-mesa-dri "$WORK/amiberry.deb"

ROMS=/usr/share/amiberry/roms
test -f "$ROMS/aros-rom.bin" || { echo "no AROS ROM in the amiberry package"; exit 1; }

echo "== preparing the emulated disk and the served directory =="
# DH0: is what AROS boots from -- just rl-target and a one-line startup script.
mkdir -p "$WORK/dh0/S"
cp "$AMIGA_BUILD/rl-target" "$WORK/dh0/rl-target"
printf 'SYS:rl-target\n' > "$WORK/dh0/S/Startup-Sequence"

# This is the directory the controller serves; the Amiga sees it as TBLx:.
mkdir -p "$WORK/fsroot/sub"
cp "$AMIGA_BUILD/rl-payload" "$WORK/fsroot/rl-payload"
printf 'hello from the host' > "$WORK/fsroot/hello.txt"
# The one subdirectory: without it nothing on the Amiga side can build a
# multi-component path, take a lock below the root, or walk back out of one.
printf 'nested from the host' > "$WORK/fsroot/sub/nested.txt"

echo "== booting AROS =="
# -a picks a free display number rather than failing on a stale lock file.
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s "-screen 0 800x600x24" \
	amiberry -G \
	-r "$ROMS/aros-rom.bin" -K "$ROMS/aros-ext.bin" \
	-s cpu_model=68020 -s cachesize=8192 \
	-s chipmem_size=4 -s z3mem_size=64 \
	-s bsdsocket_emu=true \
	-s "filesystem2=rw,DH0:DH0:$WORK/dh0,0" \
	> "$WORK/amiberry.log" 2>&1 &
AMIBERRY_PID=$!

# Wait for rl-target to bind. Each probe costs a peer index on the target, so
# the device is not reliably TBL0 -- the payload opens its file by relative path
# and never names the device.
for i in $(seq 1 45); do
	sleep 2
	if (exec 3<>/dev/tcp/127.0.0.1/7001) 2>/dev/null; then
		echo "== target listening after $((i * 2))s =="
		break
	fi
	if ! kill -0 "$AMIBERRY_PID" 2>/dev/null; then
		echo "amiberry died during boot:"; tail -40 "$WORK/amiberry.log"; exit 1
	fi
	if [ "$i" = 45 ]; then
		echo "target never opened port 7001:"; tail -40 "$WORK/amiberry.log"; exit 1
	fi
done

launch() {
	set +e
	# The line on stdin is what the payload reads back through Input(): the
	# controller serves its own stdin as the remote program's input handle.
	OUTPUT=$(printf 'typed at the controller\n' | \
		timeout 120 "$BUILD/rl-controller" -fsroot "$WORK/fsroot" 127.0.0.1 rl-payload "$@" 2>&1)
	STATUS=$?
	set -e
	echo "$OUTPUT"
	echo "== controller exit: $STATUS =="
}

expect_output() {
	case "$OUTPUT" in
		*"$1"*) ;;
		*) echo "FAIL: $2"; exit 1 ;;
	esac
}

echo "== launching the payload on the Amiga =="
launch

if [ "$STATUS" -ne 0 ]; then
	echo "FAIL: controller exited $STATUS (124 means the Amiga side hung)"
	exit 1
fi
expect_output 'read back "hello from the host"' "payload did not read the served file back"
expect_output 'seek read back "from the host"' "payload did not read the file back after a Seek()"
expect_output 'listed hello.txt and rl-payload' "payload did not enumerate the served directory"
expect_output 'read back "nested from the host" from sub/nested.txt' \
	"payload did not read through a multi-component path"
expect_output 'DupLock of sub examines as sub' "payload did not duplicate a lock on the subdirectory"
expect_output 'parent of sub lists the served root' "payload did not walk back out of the subdirectory"
expect_output 'missing file reported object not found' \
	"payload did not get ERROR_OBJECT_NOT_FOUND for a file that is not served"
expect_output 'read the line typed at the controller' "payload did not read the controller's stdin"

# Same launch, but the payload returns 42. Anything else means the remote
# return code did not survive the trip -- including the 0 the first run gave
# us, which on its own proves nothing (#37).
echo "== launching the payload in failure mode =="
launch fail

if [ "$STATUS" -ne 42 ]; then
	echo "FAIL: controller exited $STATUS, wanted the payload's 42 (124 means the Amiga side hung)"
	exit 1
fi

echo "PASS: the payload read, seeked, listed, walked a subdirectory, failed to open a missing file and read stdin over the wire, and both its return codes came back"
