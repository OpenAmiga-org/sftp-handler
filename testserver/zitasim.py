#!/usr/bin/env python3
"""A small FTP server that behaves like ZitaFTP Server (1.36) in the points
that matter for sftp-handler. Plain FTP, reading only. Public domain.

    python3 -I testserver/zitasim.py ROOT [--port 2124] [--host 127.0.0.1]
            [--data-ports 22000:20] [--port-hold 30] [--max-clients 0]
            [--abort-reply 451|226|none|two|two-late] [--user test]
            [--password test123]

Like ZitaFTP: FEAT without MLSD, its reply texts, and a fixed range of data
ports. EPSV/PASV take a free port from the range; a port stays busy for
--port-hold seconds after its data connection closed (as the server's
TIME_WAIT would keep it), and when none is free the reply is
"425 cannot open data connection". More than --max-clients connections get
"421 max clients exceeded (N)". When the client closes a download's data
connection early, the reply is --abort-reply (ZitaFTP has no 426): one
reply, none at all, two ("451 local error", then "226 closing data
connection"), or two with the second one second later (two-late). ZitaFTP
does each of these (seen in a user's ZitaFTP log: often no reply, and after
"Sending file ... failed" a reply too many). ABOR gets "226 closing data
connection".
Every command goes to stderr with a time stamp.
"""
import argparse
import os
import socket
import sys
import threading
import time

LOCK = threading.Lock()
ARGS = None
PORTS = {}        # data port -> time from which it is free again
CLIENTS = [0]
COUNT = {"425": 0, "421": 0, "retr": 0, "list": 0}


def log(cid, text):
    print("%.3f [%d] %s" % (time.time(), cid, text), file=sys.stderr, flush=True)


def take_port():
    now = time.time()
    with LOCK:
        for p in sorted(PORTS):
            if PORTS[p] is not None and PORTS[p] <= now:
                PORTS[p] = None          # in use
                return p
    return None


def give_port(p):
    with LOCK:
        PORTS[p] = time.time() + ARGS.port_hold


class Client:
    def __init__(self, sock, cid):
        self.sock = sock
        self.cid = cid
        self.f = sock.makefile("rb")
        self.cwd = "/"
        self.user = None
        self.logged_in = False
        self.listener = None
        self.port = None
        self.rest = 0

    def reply(self, text):
        log(self.cid, "< " + text.replace("\r\n", " | "))
        self.sock.sendall((text + "\r\n").encode("utf-8", "replace"))

    def real(self, path):
        if not path.startswith("/"):
            path = self.cwd.rstrip("/") + "/" + path
        parts = []
        for p in path.split("/"):
            if p in ("", "."):
                continue
            if p == "..":
                if parts:
                    parts.pop()
                continue
            parts.append(p)
        return "/" + "/".join(parts), os.path.join(ARGS.root, *parts)

    def drop_listener(self):
        if self.listener is not None:
            self.listener.close()
            self.listener = None
            give_port(self.port)
            self.port = None

    def passive(self, extended):
        self.drop_listener()
        p = take_port()
        if p is None:
            with LOCK:
                COUNT["425"] += 1
            self.reply("425 cannot open data connection")
            return
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind((ARGS.host, p))
        ls.listen(1)
        ls.settimeout(10)
        self.listener, self.port = ls, p
        if extended:
            self.reply("229 Entering Extended Passive Mode (|||%d|)" % p)
        else:
            h = ARGS.host.split(".")
            self.reply("227 Entering Passive Mode (%s,%d,%d)" % (",".join(h), p // 256, p % 256))

    def data(self):
        """the client's data connection, or None (reply sent)"""
        if self.listener is None:
            self.reply("425 cannot open data connection")
            return None
        try:
            d, _ = self.listener.accept()
        except OSError:
            self.drop_listener()
            self.reply("425 cannot open data connection")
            return None
        self.listener.close()
        self.listener = None
        return d

    def end_data(self, d):
        d.close()
        give_port(self.port)
        self.port = None

    def do_list(self, arg):
        names = [a for a in arg.split() if not a.startswith("-")]
        vpath, real = self.real(names[0] if names else ".")
        if not os.path.isdir(real):
            self.drop_listener()
            self.reply("550 action not taken")
            return
        self.reply("150 okay")
        d = self.data()
        if d is None:
            return
        with LOCK:
            COUNT["list"] += 1
        lines = []
        for n in sorted(os.listdir(real)):
            st = os.stat(os.path.join(real, n))
            kind = "d" if os.path.isdir(os.path.join(real, n)) else "-"
            lines.append("%srw-r--r-- 1 user group %d %s %s" % (
                kind, st.st_size, time.strftime("%b %d %H:%M", time.localtime(st.st_mtime)), n))
        try:
            d.sendall(("\r\n".join(lines) + "\r\n").encode("utf-8"))
        except OSError:
            pass
        self.end_data(d)
        self.reply("226 closing data connection")

    def do_retr(self, arg):
        vpath, real = self.real(arg)
        rest, self.rest = self.rest, 0
        if not os.path.isfile(real):
            self.drop_listener()
            self.reply("550 action not taken")
            return
        self.reply("150 okay")
        d = self.data()
        if d is None:
            return
        with LOCK:
            COUNT["retr"] += 1
        cut = False
        with open(real, "rb") as f:
            f.seek(rest)
            try:
                while True:
                    b = f.read(65536)
                    if not b:
                        break
                    d.sendall(b)
            except OSError:
                cut = True
        self.end_data(d)
        if not cut:
            self.reply("226 closing data connection")
        elif ARGS.abort_reply in ("451", "two", "two-late"):
            self.reply("451 local error")
            if ARGS.abort_reply == "two-late":
                time.sleep(1)
            if ARGS.abort_reply != "451":
                self.reply("226 closing data connection")
        elif ARGS.abort_reply == "226":
            self.reply("226 closing data connection")

    def run(self):
        self.reply("220 Welcome")
        while True:
            line = self.f.readline()
            if not line:
                break
            line = line.decode("utf-8", "replace").rstrip("\r\n")
            log(self.cid, "> " + ("PASS ****" if line.upper().startswith("PASS") else line))
            verb, _, arg = line.partition(" ")
            verb = verb.upper()
            if verb == "USER":
                self.user = arg
                self.reply("331 password required")
            elif verb == "PASS":
                if self.user == ARGS.user and arg == ARGS.password:
                    self.logged_in = True
                    self.reply("230 logged in")
                else:
                    self.reply("530 bad username and/or password")
            elif verb == "QUIT":
                self.reply("221 closing")
                break
            elif not self.logged_in:
                self.reply("530 bad username and/or password")
            elif verb == "FEAT":
                self.reply("211-\r\n AUTH TLS\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n TVFS\r\n UTF8\r\n EPSV\r\n211  END")
            elif verb in ("TYPE", "NOOP", "MODE", "STRU"):
                self.reply("200 okay")
            elif verb == "SYST":
                self.reply("215  UNIX Type: L8")
            elif verb == "PWD":
                self.reply('257 "%s"' % self.cwd)
            elif verb in ("CWD", "CDUP"):
                vpath, real = self.real(arg if verb == "CWD" else "..")
                if os.path.isdir(real):
                    self.cwd = vpath
                    self.reply("250 completed")
                else:
                    self.reply("550 action not taken")
            elif verb in ("EPSV", "PASV"):
                self.passive(verb == "EPSV")
            elif verb in ("LIST", "NLST"):
                self.do_list(arg)
            elif verb == "RETR":
                self.do_retr(arg)
            elif verb == "REST":
                self.rest = int(arg)
                self.reply("350 okay")
            elif verb == "SIZE":
                vpath, real = self.real(arg)
                if os.path.isfile(real):
                    self.reply("213  %d" % os.path.getsize(real))
                else:
                    self.reply("550 action not taken")
            elif verb == "MDTM":
                vpath, real = self.real(arg)
                if os.path.exists(real):
                    self.reply("213 " + time.strftime("%Y%m%d%H%M%S", time.gmtime(os.path.getmtime(real))))
                else:
                    self.reply("550 action not taken")
            elif verb == "ABOR":
                self.reply("226 closing data connection")
            else:
                self.reply("500 unrecognized command")
        self.drop_listener()


def serve(sock, cid):
    try:
        with LOCK:
            CLIENTS[0] += 1
            over = ARGS.max_clients and CLIENTS[0] > ARGS.max_clients
            if over:
                COUNT["421"] += 1
        if over:
            sock.sendall(b"421  max clients exceeded (%d)\r\n" % ARGS.max_clients)
            log(cid, "< 421 max clients exceeded")
            return
        Client(sock, cid).run()
    except OSError as e:
        log(cid, "connection error %s" % e)
    finally:
        with LOCK:
            CLIENTS[0] -= 1
        sock.close()
        log(cid, "closed; totals %s" % COUNT)


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2124)
    ap.add_argument("--data-ports", default="22000:20")
    ap.add_argument("--port-hold", type=float, default=30)
    ap.add_argument("--max-clients", type=int, default=0)
    ap.add_argument("--abort-reply", choices=("451", "226", "none", "two", "two-late"), default="451")
    ap.add_argument("--user", default="test")
    ap.add_argument("--password", default="test123")
    ARGS = ap.parse_args()
    first, count = (int(x) for x in ARGS.data_ports.split(":"))
    for p in range(first, first + count):
        PORTS[p] = 0
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ARGS.host, ARGS.port))
    srv.listen(16)
    log(0, "listening on %s:%d, data ports %d-%d, hold %.0f s, max clients %d, abort reply %s" % (
        ARGS.host, ARGS.port, first, first + count - 1, ARGS.port_hold, ARGS.max_clients, ARGS.abort_reply))
    cid = 0
    while True:
        s, _ = srv.accept()
        cid += 1
        threading.Thread(target=serve, args=(s, cid), daemon=True).start()


if __name__ == "__main__":
    main()
