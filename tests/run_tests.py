#!/usr/bin/env python3
"""Tests the core (through the Linux FUSE build) against the Docker test
servers from testserver/setup.sh. Every result is checked in the servers'
own folders (testserver/ftp-data, testserver/sftp-data), not only through
the mount.

    python3 -I tests/run_tests.py [-k NAME] [--keep]
"""
import errno
import hashlib
import os
import random
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "linux", "sftph-fuse")
WORK = os.environ.get("SFH_TESTDIR", "/tmp/sftph-test")
MNT = os.path.join(WORK, "mnt")
CFG = os.path.join(WORK, "cfg")
TMP = os.path.join(WORK, "tmp")
KEEP = os.path.join(WORK, "kept")
CACHE_MS = 1000
DATA = {
    "FTP": os.path.join(ROOT, "testserver", "ftp-data"),
    "SFTP": os.path.join(ROOT, "testserver", "sftp-data"),
    "OTP": os.path.join(ROOT, "testserver", "otp-data"),
}
# the two-factor server (testserver/otp): its users and the code maker
KEY = os.path.join(ROOT, "testserver", "keys", "client_ed25519")
OTP_SITES = f"""  <site name="Code" protocol="sftp" host="127.0.0.1" port="2223" user="otp" password="{{otp_pw}}" folder="data"/>
  <site name="KeyCode" protocol="sftp" host="127.0.0.1" port="2223" user="keyotp" keyfile="{KEY}" folder="data"/>
  <site name="KeyPass" protocol="sftp" host="127.0.0.1" port="2223" user="keypw" password="keypw123" keyfile="{KEY}" folder="data"/>
"""
TOTP = f"{sys.executable} -I {os.path.join(ROOT, 'tests', 'totp.py')}"

SITES = """<?xml version="1.0" encoding="UTF-8"?>
<sftphandler version="1">
  <site name="Test SFTP" protocol="sftp" host="127.0.0.1" port="2222" user="test" password="{sftp_pw}" folder="data"/>
  <site name="Test FTP" protocol="ftp" host="127.0.0.1" port="2121" user="test" password="{ftp_pw}" folder=""/>
{extra}</sftphandler>
"""


def write_sites(sftp_pw="test123", ftp_pw="test123", extra=""):
    os.makedirs(CFG, exist_ok=True)
    with open(os.path.join(CFG, "sites.xml"), "w") as f:
        f.write(SITES.format(sftp_pw=sftp_pw, ftp_pw=ftp_pw, extra=extra))


class Mount:
    def __init__(self, env=None, args=()):
        self.env = dict(os.environ)
        self.env.pop("SFH_PASSWORD", None)
        self.env.setdefault("SFH_HOSTKEY", "save")
        self.env.update(env or {})
        self.args = list(args)
        self.proc = None

    def __enter__(self):
        subprocess.run(["fusermount3", "-u", "-q", MNT], stderr=subprocess.DEVNULL)
        os.makedirs(MNT, exist_ok=True)
        os.makedirs(TMP, exist_ok=True)
        self.log = open(os.path.join(WORK, "fuse.log"), "a")
        self.proc = subprocess.Popen(
            [BIN, "--config", CFG, "--cache-ms", str(CACHE_MS), "--tempdir", TMP,
             "--keepdir", KEEP, "--verbose"] + self.args + [MNT, "-f"],
            stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log, env=self.env)
        for _ in range(100):
            if os.path.ismount(MNT):
                return self
            time.sleep(0.05)
        raise RuntimeError("mount failed")

    def __exit__(self, *a):
        subprocess.run(["fusermount3", "-u", MNT])
        self.proc.wait(timeout=20)
        self.log.close()


def site_dir(kind):
    return os.path.join(MNT, "Test " + kind)


CUR_SUB = ""   # the folder of the running test, on the server


def srv(kind, *parts):
    return os.path.join(DATA[kind], CUR_SUB, *parts)


def read(p, mode="rb"):
    with open(p, mode) as f:
        return f.read()


def write(p, data):
    with open(p, "wb") as f:
        f.write(data)


def expect_errno(code, fn, *args):
    try:
        fn(*args)
    except OSError as e:
        assert e.errno == code, f"errno {e.errno} ({os.strerror(e.errno)}), want {code}"
        return
    raise AssertionError(f"no error, want {os.strerror(code)}")


def settle():
    time.sleep(CACHE_MS / 1000 + 0.2)


# --- tests run on both sites ------------------------------------------------

def t_small_file(k, d):
    write(os.path.join(d, "small.txt"), b"hello amiga\n")
    assert read(srv(k, "small.txt")) == b"hello amiga\n"
    assert read(os.path.join(d, "small.txt")) == b"hello amiga\n"
    assert os.path.getsize(os.path.join(d, "small.txt")) == 12


def t_names(k, d):
    for name in ["with space.txt", "åäö ÅÄÖ.txt", "semi;colon", "#hash", "100%"]:
        write(os.path.join(d, name), name.encode())
        assert read(srv(k, name)) == name.encode(), name
    names = os.listdir(d)
    assert "åäö ÅÄÖ.txt" in names


def t_case_insensitive(k, d):
    write(srv(k, "MixedCase.TXT"), b"case")
    settle()
    assert read(os.path.join(d, "mixedcase.txt")) == b"case"
    os.makedirs(srv(k, "CaseDir", "Inner"))
    write(srv(k, "CaseDir", "Inner", "File"), b"deep")
    settle()
    assert read(os.path.join(d, "casedir", "INNER", "file")) == b"deep"
    # writing through another case changes the existing file
    write(os.path.join(d, "MIXEDCASE.txt"), b"changed")
    assert read(srv(k, "MixedCase.TXT")) == b"changed"
    assert not os.path.exists(srv(k, "MIXEDCASE.txt"))


def t_overwrite(k, d):
    p = os.path.join(d, "over.txt")
    write(p, b"a" * 1000)
    write(p, b"short")
    assert read(srv(k, "over.txt")) == b"short"


def t_update_in_place(k, d):
    p = os.path.join(d, "inplace.bin")
    write(p, bytes(range(256)) * 40)
    with open(p, "r+b") as f:
        f.seek(100)
        f.write(b"XYZ")
        f.seek(0, 2)
        f.write(b"END")
    want = bytearray(bytes(range(256)) * 40)
    want[100:103] = b"XYZ"
    want += b"END"
    assert read(srv(k, "inplace.bin")) == bytes(want)
    # IFF style: write, then go back and fix the header
    p = os.path.join(d, "iff.bin")
    with open(p, "wb") as f:
        f.write(b"FORM\0\0\0\0ILBM" + b"x" * 5000)
        f.seek(4)
        f.write(b"\0\0\x13\x8c")
    assert read(srv(k, "iff.bin"))[:12] == b"FORM\0\0\x13\x8cILBM"


def t_empty_file(k, d):
    open(os.path.join(d, "empty"), "wb").close()
    assert os.path.getsize(srv(k, "empty")) == 0
    assert os.path.getsize(os.path.join(d, "empty")) == 0


def t_truncate(k, d):
    p = os.path.join(d, "trunc.txt")
    write(p, b"0123456789")
    os.truncate(p, 0)
    assert read(srv(k, "trunc.txt")) == b""
    write(p, b"0123456789")
    with open(p, "r+b") as f:
        f.truncate(4)
    assert read(srv(k, "trunc.txt")) == b"0123"


def t_large_and_seeks(k, d):
    data = random.Random(1).randbytes(12 * 1024 * 1024 + 123)
    p = os.path.join(d, "large.bin")
    t0 = time.time()
    write(p, data)
    t1 = time.time()
    assert hashlib.sha1(read(srv(k, "large.bin"))).digest() == hashlib.sha1(data).digest()
    got = read(p)
    t2 = time.time()
    assert got == data
    print(f"    12 MB: write {12 / (t1 - t0):.1f} MB/s, read {12 / (t2 - t1):.1f} MB/s")
    with open(p, "rb") as f:
        rnd = random.Random(2)
        for _ in range(30):
            off = rnd.randrange(0, len(data))
            f.seek(off)
            n = rnd.randrange(1, 70000)
            assert f.read(n) == data[off:off + n], off
        f.seek(len(data) - 10)
        assert f.read(100) == data[-10:]
        assert f.read(100) == b""


def t_two_readers(k, d):
    a = random.Random(3).randbytes(3 * 1024 * 1024)
    b = random.Random(4).randbytes(3 * 1024 * 1024)
    write(srv(k, "ra.bin"), a)
    write(srv(k, "rb.bin"), b)
    settle()
    with open(os.path.join(d, "ra.bin"), "rb") as fa, open(os.path.join(d, "rb.bin"), "rb") as fb:
        ga, gb = bytearray(), bytearray()
        while True:
            x, y = fa.read(65536), fb.read(65536)
            if not x and not y:
                break
            ga += x
            gb += y
            if len(ga) == 1024 * 1024:
                # a listing in between needs a connection too
                os.listdir(d)
    assert bytes(ga) == a and bytes(gb) == b


def t_copy_on_site(k, d):
    data = random.Random(5).randbytes(2 * 1024 * 1024)
    write(srv(k, "src.bin"), data)
    settle()
    shutil.copyfile(os.path.join(d, "src.bin"), os.path.join(d, "dst.bin"))
    assert read(srv(k, "dst.bin")) == data


def t_mkdir_rmdir(k, d):
    p = os.path.join(d, "newdir")
    os.mkdir(p)
    assert os.path.isdir(srv(k, "newdir"))
    expect_errno(errno.EEXIST, os.mkdir, p)
    write(os.path.join(p, "f"), b"x")
    expect_errno(errno.ENOTEMPTY, os.rmdir, p)
    os.unlink(os.path.join(p, "f"))
    os.rmdir(p)
    assert not os.path.exists(srv(k, "newdir"))


def t_rmdir_sees_server_files(k, d):
    os.mkdir(os.path.join(d, "srvdir"))
    os.listdir(os.path.join(d, "srvdir"))
    write(srv(k, "srvdir", "added"), b"x")
    expect_errno(errno.ENOTEMPTY, os.rmdir, os.path.join(d, "srvdir"))


def t_rename(k, d):
    write(os.path.join(d, "r1.txt"), b"r")
    os.rename(os.path.join(d, "r1.txt"), os.path.join(d, "r2.txt"))
    assert not os.path.exists(srv(k, "r1.txt")) and read(srv(k, "r2.txt")) == b"r"
    os.mkdir(os.path.join(d, "rd"))
    write(os.path.join(d, "rd", "in"), b"in")
    os.rename(os.path.join(d, "rd"), os.path.join(d, "rd2"))
    assert read(srv(k, "rd2", "in")) == b"in"
    assert read(os.path.join(d, "rd2", "in")) == b"in"
    os.rename(os.path.join(d, "r2.txt"), os.path.join(d, "rd2", "r3.txt"))
    assert read(srv(k, "rd2", "r3.txt")) == b"r"
    # case change only
    os.rename(os.path.join(d, "rd2", "r3.txt"), os.path.join(d, "rd2", "R3.TXT"))
    assert os.listdir(srv(k, "rd2")).count("R3.TXT") == 1
    assert "r3.txt" not in os.listdir(srv(k, "rd2"))
    write(os.path.join(d, "taken"), b"t")
    expect_errno(errno.EEXIST, os.rename, os.path.join(d, "rd2", "R3.TXT"), os.path.join(d, "taken"))


def t_unlink(k, d):
    write(os.path.join(d, "del.txt"), b"d")
    os.unlink(os.path.join(d, "del.txt"))
    assert not os.path.exists(srv(k, "del.txt"))
    expect_errno(errno.ENOENT, os.unlink, os.path.join(d, "del.txt"))


def t_mtime(k, d):
    p = os.path.join(d, "dated.txt")
    write(p, b"x")
    os.utime(p, (1500000000, 1500000000))
    if k == "SFTP":
        assert int(os.path.getmtime(srv(k, "dated.txt"))) == 1500000000
        assert int(os.path.getmtime(p)) == 1500000000
    # vsftpd has no MFMT: the date stays, without an error


def t_chmod(k, d):
    p = os.path.join(d, "mode.txt")
    write(p, b"x")
    os.chmod(srv(k, "mode.txt"), 0o644)
    settle()
    # only the owner's r and w bits reach the server
    os.chmod(p, 0o444)
    if k == "SFTP":
        assert os.stat(srv(k, "mode.txt")).st_mode & 0o777 == 0o444
    os.chmod(p, 0o700)
    if k == "SFTP":
        assert os.stat(srv(k, "mode.txt")).st_mode & 0o777 == 0o644


def t_server_changes_seen(k, d):
    os.listdir(d)
    write(srv(k, "late.txt"), b"late")
    settle()
    assert "late.txt" in os.listdir(d)
    assert read(os.path.join(d, "late.txt")) == b"late"


def t_many_files(k, d):
    os.makedirs(srv(k, "many"))
    for i in range(300):
        write(srv(k, "many", f"file{i:03d}"), b"x" * i)
    settle()
    names = os.listdir(os.path.join(d, "many"))
    assert len(names) == 300, len(names)
    assert os.path.getsize(os.path.join(d, "many", "file299")) == 299


def t_reconnect(k, d):
    write(os.path.join(d, "before"), b"b")
    subprocess.run(["docker", "restart", "-t", "1", "sftph-" + k.lower()], check=True,
                   stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    time.sleep(2)
    settle()
    assert read(os.path.join(d, "before")) == b"b"
    write(os.path.join(d, "after"), b"a")
    assert read(srv(k, "after")) == b"a"


def t_write_behind(k, d):
    p = os.path.join(d, "wb.bin")
    data = random.Random(7).randbytes(300000)
    f = open(p, "wb", buffering=0)
    for i in range(0, len(data), 4096):
        f.write(data[i:i + 4096])
    # another open sees everything written so far
    with open(p, "rb") as g:
        assert g.read() == data
    f.close()
    if k == "SFTP":
        # read back through the same handle (r+); FTP has no such handle
        with open(p, "r+b", buffering=0) as h:
            h.seek(1000)
            h.write(b"ABC")
            h.seek(990)
            assert h.read(20) == data[990:1000] + b"ABC" + data[1003:1010]
    # a date set while the file is still open stays after the close
    with open(os.path.join(d, "dated.bin"), "wb", buffering=0) as f:
        f.write(b"x" * 5000)
        os.utime(os.path.join(d, "dated.bin"), (1400000000, 1400000000))
    if k == "SFTP":
        assert int(os.path.getmtime(srv(k, "dated.bin"))) == 1400000000
        assert os.path.getsize(srv(k, "dated.bin")) == 5000


WRITER = """
import sys
f = open(sys.argv[1], "wb", buffering=0)
f.write(b"y" * 100000)          # held by the handler (write-behind)
print("written", flush=True)
sys.stdin.readline()            # the server goes away meanwhile
try:
    f.close()
    print("close ok", flush=True)
except OSError as e:
    print("close error", e.errno, flush=True)
"""


def t_write_behind_lost(k, d):
    if k != "SFTP":
        return
    # a process of its own: a fork in this one (docker restart) would
    # flush the file early through the copy of its descriptor
    w = subprocess.Popen([sys.executable, "-I", "-c", WRITER, os.path.join(d, "lost.bin")],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert w.stdout.readline().strip() == "written"
    subprocess.run(["docker", "restart", "-t", "1", "sftph-sftp"], check=True,
                   stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    time.sleep(2)
    out, _ = w.communicate("go\n", timeout=120)
    time.sleep(0.5)
    log = read(os.path.join(WORK, "fuse.log"), "r")
    # the loss is reported: an error from close() and the requester text
    assert out.startswith("close error") and "may be incomplete" in log, (out, log[-300:])


SITE_TESTS = [t_small_file, t_names, t_case_insensitive, t_overwrite, t_update_in_place,
              t_empty_file, t_truncate, t_large_and_seeks, t_two_readers, t_copy_on_site,
              t_mkdir_rmdir, t_rmdir_sees_server_files, t_rename, t_unlink, t_mtime, t_chmod,
              t_server_changes_seen, t_many_files, t_write_behind, t_write_behind_lost,
              t_reconnect]


# --- tests of the root and the set-up -----------------------------------------

def t_root():
    names = sorted(os.listdir(MNT))
    assert names == ["Test FTP", "Test FTP.info", "Test SFTP", "Test SFTP.info"], names
    assert read(os.path.join(MNT, "Test FTP.info")) == b"ICON" * 10
    expect_errno(errno.EROFS, write, os.path.join(MNT, "new"), b"x")
    expect_errno(errno.EROFS, os.mkdir, os.path.join(MNT, "newdir"))
    expect_errno(errno.EROFS, os.rmdir, os.path.join(MNT, "Test FTP"))
    expect_errno(errno.EROFS, os.rename, os.path.join(MNT, "Test FTP"), os.path.join(MNT, "X"))
    expect_errno(errno.EROFS, os.unlink, os.path.join(MNT, "Test FTP.info"))
    expect_errno(errno.EROFS, os.rename, os.path.join(MNT, "Test FTP", "hello.txt"),
                 os.path.join(MNT, "hello.txt"))
    expect_errno(errno.EXDEV, os.rename, os.path.join(MNT, "Test FTP", "hello.txt"),
                 os.path.join(MNT, "Test SFTP", "hello.txt"))
    # case-insensitive site names
    assert os.path.isdir(os.path.join(MNT, "test ftp"))


def t_config_reload():
    write_sites(extra='  <site name="Extra" protocol="ftp" host="127.0.0.1" port="2121" user="test" password="test123"/>\n')
    time.sleep(1.2)
    assert "Extra" in os.listdir(MNT)
    assert "hello.txt" in os.listdir(os.path.join(MNT, "Extra"))
    write_sites()
    time.sleep(1.2)
    assert "Extra" not in os.listdir(MNT)


def t_password_requester():
    # no password in the file: the requester's answer is used and saved
    write_sites(sftp_pw="", ftp_pw="")
    with Mount(env={"SFH_PASSWORD": "test123", "SFH_REMEMBER": "1"}):
        assert "hello.txt" in os.listdir(site_dir("FTP"))
        assert "hello.txt" in os.listdir(site_dir("SFTP"))
    text = read(os.path.join(CFG, "sites.xml"), "r")
    assert text.count('password="test123"') == 2, text
    # a wrong password and Cancel: "permission denied", no endless asking
    write_sites(sftp_pw="wrong", ftp_pw="wrong")
    with Mount():
        expect_errno(errno.ENODEV, os.listdir, site_dir("FTP"))
        expect_errno(errno.ENODEV, os.listdir, site_dir("SFTP"))
    write_sites()


def t_hostkey():
    kh = os.path.join(CFG, "known_hosts")
    if os.path.exists(kh):
        os.unlink(kh)
    with Mount(env={"SFH_HOSTKEY": "refuse"}):
        expect_errno(errno.ENODEV, os.listdir, site_dir("SFTP"))
    assert not os.path.exists(kh)
    with Mount(env={"SFH_HOSTKEY": "once"}):
        assert "hello.txt" in os.listdir(site_dir("SFTP"))
    assert not os.path.exists(kh)
    with Mount(env={"SFH_HOSTKEY": "save"}):
        assert "hello.txt" in os.listdir(site_dir("SFTP"))
    assert "[127.0.0.1]:2222 ssh-ed25519" in read(kh, "r")
    # a changed key is refused
    with open(kh, "w") as f:
        f.write("[127.0.0.1]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGzR8fkRDZRo8bWyzZJVrHeKmvMHc3oZa1zVa3N9mXkE\n")
    with Mount(env={"SFH_HOSTKEY": "refuse"}):
        expect_errno(errno.ENODEV, os.listdir, site_dir("SFTP"))
    os.unlink(kh)


def t_offline():
    write_sites(extra='  <site name="Nobody" protocol="sftp" host="127.0.0.1" port="2999" user="x" password="y"/>\n')
    with Mount():
        assert os.path.isdir(os.path.join(MNT, "Nobody"))
        expect_errno(errno.ENODEV, os.listdir, os.path.join(MNT, "Nobody"))
    write_sites()


# --- two-factor logins (testserver/otp) ------------------------------------------

def log_pos():
    p = os.path.join(WORK, "fuse.log")
    return os.path.getsize(p) if os.path.exists(p) else 0


def log_since(pos):
    with open(os.path.join(WORK, "fuse.log"), "r", errors="replace") as f:
        f.seek(pos)
        return f.read()


def questions(text, site):
    return [l for l in text.splitlines() if l.startswith(f"[sftph] question requester: {site} ")]


def otp_sites(otp_pw="otp123"):
    write_sites(extra=OTP_SITES.format(otp_pw=otp_pw))


def t_2fa_code():
    # password from the site file, the code from the user
    otp_sites()
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP}):
        d = os.path.join(MNT, "Code")
        assert "hello.txt" in os.listdir(d)
        write(os.path.join(d, "via-code.txt"), b"2fa")
        assert read(os.path.join(DATA["OTP"], "via-code.txt")) == b"2fa"
        assert read(os.path.join(d, "hello.txt")) == b"hello\n"
    q = questions(log_since(pos), "Code")
    assert len(q) == 1, q
    assert "Verification code" in q[0] and "echo=0" in q[0], q


def t_2fa_key_then_code():
    # the key is the first step, the code the second
    otp_sites()
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP}):
        assert "hello.txt" in os.listdir(os.path.join(MNT, "KeyCode"))
    text = log_since(pos)
    assert len(questions(text, "KeyCode")) == 1, text
    assert "login methods keyboard-interactive" in text, text


def t_2fa_key_then_password():
    # two steps without a code: nothing is asked
    otp_sites()
    pos = log_pos()
    with Mount():
        assert "hello.txt" in os.listdir(os.path.join(MNT, "KeyPass"))
    text = log_since(pos)
    assert not questions(text, "KeyPass"), text
    assert "login methods password" in text, text


def t_2fa_password_asked_first():
    # no password saved: the password requester, then the code
    otp_sites(otp_pw="")
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP, "SFH_PASSWORD": "otp123"}):
        assert "hello.txt" in os.listdir(os.path.join(MNT, "Code"))
    text = log_since(pos)
    assert "password requester: Code" in text, text
    assert len(questions(text, "Code")) == 1, text


def t_2fa_slow_answer():
    # typing the code takes longer than the network timeout: still works
    otp_sites()
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": f"sleep 4; {TOTP}"}, args=["--timeout-ms", "2000"]):
        assert "hello.txt" in os.listdir(os.path.join(MNT, "KeyCode"))
    assert len(questions(log_since(pos), "KeyCode")) == 1


def t_2fa_wrong_code():
    # asked three times, the problem shown again; then reported, no more asking
    otp_sites()
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": "echo 000000"}):
        expect_errno(errno.ENODEV, os.listdir, os.path.join(MNT, "KeyCode"))
        expect_errno(errno.ENODEV, os.listdir, os.path.join(MNT, "KeyCode"))
    text = log_since(pos)
    q = questions(text, "KeyCode")
    assert len(q) == 3, q
    assert "did not accept the code." in q[1] and "did not accept the code." in q[2], q
    assert "*** " in text and "use the newest one" in text, text


def t_2fa_wrong_password():
    # the password is refused before the code is asked: the password requester comes
    otp_sites(otp_pw="wrong")
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP}):
        expect_errno(errno.ENODEV, os.listdir, os.path.join(MNT, "Code"))
    text = log_since(pos)
    assert not questions(text, "Code"), text
    assert "password requester: Code" in text, text


def t_2fa_cancel():
    # Cancel: offline until the site is reset, no second question
    otp_sites()
    pos = log_pos()
    with Mount():
        expect_errno(errno.ENODEV, os.listdir, os.path.join(MNT, "KeyCode"))
        expect_errno(errno.ENODEV, os.listdir, os.path.join(MNT, "KeyCode"))
    text = log_since(pos)
    assert len(questions(text, "KeyCode")) == 1, text
    assert "*** " not in text, text


def t_2fa_kept_open():
    # an idle two-factor connection stays (--idle-ms is for the others) ...
    otp_sites()
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP}, args=["--idle-ms", "1000"]):
        d = os.path.join(MNT, "KeyCode")
        assert "hello.txt" in os.listdir(d)
        assert "hello.txt" in os.listdir(site_dir("SFTP"))
        time.sleep(6)
        os.listdir(MNT)
        time.sleep(1)
        assert "hello.txt" in os.listdir(d)
    text = log_since(pos)
    assert len(questions(text, "KeyCode")) == 1, text
    assert "Test SFTP: disconnected" in text, text
    # ... unless --code-idle-ms says otherwise; the new question says why
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP}, args=["--code-idle-ms", "1000"]):
        d = os.path.join(MNT, "KeyCode")
        assert "hello.txt" in os.listdir(d)
        time.sleep(6)
        os.listdir(MNT)
        time.sleep(1)
        assert "hello.txt" in os.listdir(d)
    q = questions(log_since(pos), "KeyCode")
    assert len(q) == 2, q
    assert "connection to the server was closed" in q[1], q


def t_2fa_server_restart():
    # the connection breaks: the next use asks for a new code
    otp_sites()
    pos = log_pos()
    with Mount(env={"SFH_ANSWER_CMD": TOTP}):
        d = os.path.join(MNT, "Code")
        assert "hello.txt" in os.listdir(d)
        subprocess.run(["docker", "restart", "-t", "1", "sftph-otp"], check=True,
                       stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        time.sleep(2)
        settle()
        write(os.path.join(d, "after-restart"), b"r")
        assert read(os.path.join(DATA["OTP"], "after-restart")) == b"r"
    q = questions(log_since(pos), "Code")
    assert len(q) == 2, q
    assert "connection to the server was closed" in q[1], q


# --- an FTP server like ZitaFTP Server (testserver/zitasim.py) --------------

ZITA = os.path.join(ROOT, "testserver", "zitasim.py")
ZITA_DATA = os.path.join(WORK, "zita-data")
ZITA_SITE = '  <site name="Zita" protocol="ftp" host="127.0.0.1" port="2124" user="test" password="test123"/>\n'


class Zita:
    """testserver/zitasim.py on port 2124 with ZITA_DATA as its files"""

    def __init__(self, *args):
        self.args = list(args)

    def __enter__(self):
        os.makedirs(ZITA_DATA, exist_ok=True)
        self.log = open(os.path.join(WORK, "zitasim.log"), "a")
        self.proc = subprocess.Popen([sys.executable, "-I", ZITA, ZITA_DATA, "--port", "2124"] + self.args,
                                     stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", 2124), timeout=1).close()
                break
            except OSError:
                time.sleep(0.05)
        write_sites(extra=ZITA_SITE)
        return self

    def __exit__(self, *a):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.log.close()
        write_sites()
        shutil.rmtree(ZITA_DATA, ignore_errors=True)


def zita_files(n, size=5000):
    os.makedirs(os.path.join(ZITA_DATA, "many"), exist_ok=True)
    for i in range(n):
        write(os.path.join(ZITA_DATA, "many", f"file{i:03d}.lha"), bytes([i % 256]) * size)


def t_ftp_data_ports_busy():
    # Workbench reads the start of every file of a folder it opens: one
    # data connection each. When the server's data ports are used up it
    # answers 425; the handler waits for a free one instead of failing
    # (it was "No disk present" on AmigaOS)
    zita_files(12)
    pos = log_pos()
    with Zita("--data-ports", "22000:3", "--port-hold", "1.5"), Mount():
        d = os.path.join(MNT, "Zita", "many")
        for n in sorted(os.listdir(d)):
            with open(os.path.join(d, n), "rb") as f:
                assert len(f.read(480)) == 480, n
        assert read(os.path.join(d, "file011.lha")) == bytes([11]) * 5000
    assert "no data connection free (425), waiting" in log_since(pos)


def t_ftp_data_ports_gone():
    # a server that stays without data ports: "busy" after the I/O timeout,
    # not an I/O error
    zita_files(2)
    with Zita("--data-ports", "22000:2", "--port-hold", "600"), Mount(args=["--timeout-ms", "3000"]):
        d = os.path.join(MNT, "Zita", "many")
        names = sorted(os.listdir(d))   # the two listings take both ports
        t = time.time()
        expect_errno(errno.EBUSY, read, os.path.join(d, names[0]))
        assert 2 < time.time() - t < 10, time.time() - t


def t_ftp_max_clients():
    # "421 max clients exceeded": fewer connections; three files read at
    # the same time still come out whole
    zita_files(3, 300000)
    with Zita("--max-clients", "2"), Mount():
        d = os.path.join(MNT, "Zita", "many")
        fs = [open(os.path.join(d, f"file{i:03d}.lha"), "rb") for i in range(3)]
        got = [b"", b"", b""]
        while any(len(g) < 300000 for g in got):
            for i in range(3):
                got[i] += fs[i].read(65536)
        for f in fs:
            f.close()
        for i in range(3):
            assert got[i] == bytes([i]) * 300000, i


ZITA_TESTS = [t_ftp_data_ports_busy, t_ftp_data_ports_gone, t_ftp_max_clients]


OTP_TESTS = [t_2fa_code, t_2fa_key_then_code, t_2fa_key_then_password,
             t_2fa_password_asked_first, t_2fa_slow_answer, t_2fa_wrong_code, t_2fa_wrong_password,
             t_2fa_cancel, t_2fa_kept_open, t_2fa_server_restart]


def t_readonly():
    with Mount(args=["--readonly"]):
        expect_errno(errno.EROFS, write, os.path.join(site_dir("FTP"), "ro"), b"x")
        expect_errno(errno.EROFS, os.mkdir, os.path.join(site_dir("SFTP"), "ro"))
        assert read(os.path.join(site_dir("FTP"), "hello.txt")) == b"hello\n"


def clean_data():
    for k, p in DATA.items():
        for n in os.listdir(p):
            q = os.path.join(p, n)
            if os.path.isdir(q) and not os.path.islink(q):
                shutil.rmtree(q)
            else:
                os.unlink(q)
        write(os.path.join(p, "hello.txt"), b"hello\n")


def main():
    only = None
    if "-k" in sys.argv:
        only = sys.argv[sys.argv.index("-k") + 1]
    os.makedirs(WORK, exist_ok=True)
    icon = os.path.join(WORK, "icon.info")
    write(icon, b"ICON" * 10)
    write_sites()
    global CUR_SUB
    passed = failed = 0
    clean_data()
    for k in ("FTP", "SFTP"):
        with Mount(args=["--icon", icon]):
            for t in SITE_TESTS:
                name = f"{t.__name__}[{k}]"
                if only and only not in name:
                    continue
                CUR_SUB = ""
                d = os.path.join(site_dir(k), "t_" + t.__name__)
                os.makedirs(srv(k, "t_" + t.__name__), exist_ok=True)
                CUR_SUB = "t_" + t.__name__
                settle()
                try:
                    t(k, d)
                    print(f"ok   {name}")
                    passed += 1
                except Exception as e:
                    print(f"FAIL {name}: {e!r}")
                    failed += 1
    CUR_SUB = ""
    for t in (t_root, t_config_reload):
        if only and only not in t.__name__:
            continue
        with Mount(args=["--icon", icon]):
            try:
                t()
                print(f"ok   {t.__name__}")
                passed += 1
            except Exception as e:
                print(f"FAIL {t.__name__}: {e!r}")
                failed += 1
    for t in [t_password_requester, t_hostkey, t_offline, t_readonly] + ZITA_TESTS + OTP_TESTS:
        if only and only not in t.__name__:
            continue
        try:
            t()
            print(f"ok   {t.__name__}")
            passed += 1
        except Exception as e:
            print(f"FAIL {t.__name__}: {e!r}")
            failed += 1
        write_sites()
    if "--keep" not in sys.argv:
        clean_data()
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
