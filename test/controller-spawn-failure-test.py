#!/usr/bin/env python3
#
# Regression test for #34: when the target answers a launch request with
# ERROR_ANSWER, the controller must report it and exit non-zero.
#
# Before the fix ERROR_ANSWER fell through to rl_file_serve(), which replied
# BAD_REQUEST and left the peer connected, so the controller's select loop
# spun forever and the user had to Ctrl-C.
#
# Usage: controller-spawn-failure-test.py <rl-controller>

import os
import re
import socket
import struct
import subprocess
import sys

MSG_ERROR_ANSWER = 0x0
MSG_HANDSHAKE_REQUEST = 0x3
MSG_LAUNCH_EXECUTABLE_REQUEST = 0xe

NETERR_SPAWN_FAILURE = 254


def source_version():
    """The controller drops peers whose protocol version differs, so read the
    one it was built with instead of hardcoding a number that goes stale."""
    header = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          '..', 'src', 'version.h')
    with open(header) as f:
        text = f.read()
    def field(name):
        m = re.search(r'^#define\s+RLAUNCH_PROTO_%s\s+(\d+)' % name, text,
                      re.MULTILINE)
        assert m, 'no RLAUNCH_PROTO_%s in %s' % (name, header)
        return int(m.group(1))
    return field('MAJOR'), field('MINOR')


VER_MAJOR, VER_MINOR = source_version()


def encode(kind, seq, payload):
    """Header is type, flags, big-endian length (header included), sequence."""
    return struct.pack('>BBHI', kind, 0, 8 + len(payload), seq) + payload


def encode_string(s):
    """Length byte, bytes, NUL."""
    b = s.encode('ascii')
    assert len(b) < 256
    return bytes([len(b)]) + b + b'\0'


def recv_message(conn):
    head = b''
    while len(head) < 4:
        chunk = conn.recv(4 - len(head))
        if not chunk:
            raise EOFError('controller closed the connection mid-header')
        head += chunk
    length = struct.unpack('>H', head[2:4])[0]
    body = head
    while len(body) < length:
        chunk = conn.recv(length - len(body))
        if not chunk:
            raise EOFError('controller closed the connection mid-message')
        body += chunk
    return body[0], body


def main(controller):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    proc = subprocess.Popen(
        [controller, '-port', str(port), '127.0.0.1', 'does-not-exist'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        listener.settimeout(15)
        conn, _ = listener.accept()
        conn.settimeout(15)

        kind, _ = recv_message(conn)
        assert kind == MSG_HANDSHAKE_REQUEST, 'expected handshake, got %d' % kind

        # Both ends greet with a handshake *request*; there is no answer kind
        # on the wire for it.
        conn.sendall(encode(MSG_HANDSHAKE_REQUEST, 0,
                            bytes([VER_MAJOR, VER_MINOR]) +
                            encode_string('fake-target') +
                            encode_string('AmigaOS') +
                            encode_string('Kickstart V40') +
                            encode_string('****')))

        kind, body = recv_message(conn)
        assert kind == MSG_LAUNCH_EXECUTABLE_REQUEST, \
            'expected launch request, got %d' % kind
        seq = struct.unpack('>I', body[4:8])[0]

        conn.sendall(encode(MSG_ERROR_ANSWER, seq,
                            struct.pack('>I', NETERR_SPAWN_FAILURE)))

        output = proc.communicate(timeout=15)[0]
    except subprocess.TimeoutExpired:
        proc.kill()
        print('FAIL: controller hung after the spawn failure', file=sys.stderr)
        return 1
    finally:
        proc.poll()
        if proc.returncode is None:
            proc.kill()
        listener.close()

    print(output, end='')

    if proc.returncode == 0:
        print('FAIL: controller exited 0 after a failed spawn', file=sys.stderr)
        return 1
    if 'failed to launch' not in output:
        print('FAIL: controller did not report the launch failure',
              file=sys.stderr)
        return 1

    print('PASS')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('usage: %s <path to rl-controller>' % sys.argv[0],
              file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1]))
