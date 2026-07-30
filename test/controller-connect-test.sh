#!/bin/sh
#
# Regression test for #20: a refused connection must be reported as a failed
# connect for that address, and the connect loop must survive it and reach the
# "couldn't connect to any of the addresses" verdict.
#
# Before the fix the non-blocking connect was assumed to have succeeded once
# select() reported the socket ready, so a refused connection ran on into
# peer_init() and the message below was unreachable.
#
# Usage: controller-connect-test.sh <rl-controller> [python3]

set -e

CONTROLLER="$1"
PYTHON="${2:-python3}"

if [ ! -x "$CONTROLLER" ]; then
	echo "usage: $0 <path to rl-controller> [python3]" >&2
	exit 1
fi

# Bind port 0, read back what the kernel picked, then drop it: nothing is
# listening on that port, so loopback refuses the connect immediately instead
# of making the test wait out the 10 second connect timeout.
PORT=$("$PYTHON" -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')

echo "connecting to a closed port $PORT"

OUTPUT=$("$CONTROLLER" -port "$PORT" 127.0.0.1 c:info 2>&1) && STATUS=0 || STATUS=$?
echo "$OUTPUT"
echo "controller exit: $STATUS"

if [ "$STATUS" -eq 0 ]; then
	echo "FAIL: controller exited 0 after failing to connect" >&2
	exit 1
fi

case "$OUTPUT" in
	*"couldn't connect to any of the addresses"*)
		echo "PASS"
		;;
	*)
		echo "FAIL: refused connect was not reported as a failed connect" >&2
		exit 1
		;;
esac
