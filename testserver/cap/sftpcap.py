#!/usr/bin/python3
"""SFTP subsystem for the sftph-cap test server: runs OpenSSH's sftp-server
and shortens every SSH_FXP_READ request to at most CAP bytes, so the server
answers with short replies, as servers do that limit the read size. CAP comes
from /srv/cap/cap (0 or missing = unchanged). Public domain."""
import os
import struct
import subprocess
import sys
import threading

SSH_FXP_READ = 5


def cap():
    try:
        with open("/srv/cap/cap") as f:
            return int(f.read().strip() or 0)
    except (OSError, ValueError):
        return 0


def read_exact(f, n):
    data = b""
    while len(data) < n:
        b = f.read(n - len(data))
        if not b:
            return None
        data += b
    return data


def server_to_client(src, dst):
    while True:
        b = src.read1(65536) if hasattr(src, "read1") else src.read(65536)
        if not b:
            break
        dst.write(b)
        dst.flush()


def main():
    limit = cap()
    srv = subprocess.Popen(["/usr/lib/openssh/sftp-server"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    out = threading.Thread(target=server_to_client, args=(srv.stdout, sys.stdout.buffer), daemon=True)
    out.start()
    inp = sys.stdin.buffer
    while True:
        head = read_exact(inp, 4)
        if head is None:
            break
        (length,) = struct.unpack(">I", head)
        body = read_exact(inp, length)
        if body is None:
            break
        if limit and body[0] == SSH_FXP_READ:
            (hlen,) = struct.unpack(">I", body[5:9])
            at = 9 + hlen + 8
            (want,) = struct.unpack(">I", body[at:at + 4])
            if want > limit:
                body = body[:at] + struct.pack(">I", limit) + body[at + 4:]
        srv.stdin.write(head + body)
        srv.stdin.flush()
    srv.stdin.close()
    out.join()
    srv.wait()


if __name__ == "__main__":
    main()
