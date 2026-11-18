#!/home/claude/build/amiga-fleet-venv/bin/python
"""Ciphers and short read replies in the AmigaOS 4 guest, against
testserver/cap (sftph-cap, 10.0.2.2:2224). Public domain.

    tests/os4/ciphers.py

For each cipher (DOSDriver option CIPHERS=) and server cap (the most bytes
the server gives per read reply, 0 = no cap) it mounts the build, copies
test files from SFTP:Cap to RAM: and compares their SHA-256 with the
originals. Needs clib4.library 2.5 (see run_guest.py).
"""
import asyncio
import hashlib
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_guest as rg  # noqa: E402
from amiga_fleet_mcp.tools import fs as fs_tool  # noqa: E402

CAP_DATA = os.path.join(rg.ROOT, "testserver", "cap-data", "data")
CAP_FILE = os.path.join(rg.ROOT, "testserver", "cap-data", "cap", "cap")
SITES = """<?xml version="1.0" encoding="UTF-8"?>
<sftphandler version="1">
  <site name="Cap" protocol="sftp" host="10.0.2.2" port="2224" user="test" password="test123" folder="data"/>
</sftphandler>
"""
RUNS = [("chacha20-poly1305@openssh.com", 0), ("aes128-ctr", 0), ("aes256-ctr", 0),
        ("aes128-gcm@openssh.com", 0), ("aes256-gcm@openssh.com", 0), ("aes128-cbc", 0),
        ("chacha20-poly1305@openssh.com", 20000), ("aes128-ctr", 65536),
        ("aes256-gcm@openssh.com", 100000)]
SIZES = [1, 19999, 20001, 255 * 1024 + 1, 3 * 1024 * 1024 + 7]


async def main():
    fleet = rg.Fleet(rg.load_config())
    m = rg.McpdTransport(rg.McpdChannel(enabled=True, endpoint="127.0.0.1:4422"))
    rnd = random.Random(2735)
    want = {}
    for n in SIZES:
        data = rnd.randbytes(n)
        with open(os.path.join(CAP_DATA, f"g{n}.bin"), "wb") as f:
            f.write(data)
        want[f"g{n}.bin"] = hashlib.sha256(data).hexdigest()
    bad = 0
    try:
        for cipher, cap in RUNS:
            with open(CAP_FILE, "w") as f:
                f.write(str(cap))
            os.environ["SFH_STARTUP"] = "CIPHERS=" + cipher
            work = await rg.setup(fleet, m, True)
            sites = os.path.join(work, "sites-cap.xml")
            with open(sites, "w") as f:
                f.write(SITES)
            await rg.upload(fleet, sites, "RAM:sftph/cfg/sites.xml")
            key = open(os.path.join(rg.ROOT, "testserver", "keys", "ssh_host_ed25519_key.pub")).read().split()
            kh = os.path.join(work, "known_hosts-cap")
            with open(kh, "w") as f:
                f.write(f"[10.0.2.2]:2224 {key[0]} {key[1]}\n")
            await rg.upload(fleet, kh, "RAM:sftph/cfg/known_hosts")
            await rg.cmd(m, "Wait 2")
            for name, h in want.items():
                rc, out = await rg.cmd(m, f'Copy "SFTP:Cap/{name}" RAM:sftph/{name}')
                got = await fs_tool.fs_hash(fleet, rg.TARGET, f"RAM:sftph/{name}", "sha256")
                got = getattr(got, "hash", got)
                ok = rc == 0 and got == h
                bad += not ok
                print(f"{cipher:32} cap {cap:6}  {name:16} {'ok' if ok else 'FAIL rc=%s %s' % (rc, out.strip()[:80])}")
                await rg.cmd(m, f"Delete RAM:sftph/{name} QUIET")
            await rg.cmd(m, "Dismount SFTP:")
    finally:
        with open(CAP_FILE, "w") as f:
            f.write("0")
        for name in want:
            os.unlink(os.path.join(CAP_DATA, name))
        await rg.cmd(m, "Assign LIBS: System:Libs System:MUI/Libs")
    print("ALL OK" if not bad else f"{bad} FAILED")
    return 1 if bad else 0


sys.exit(asyncio.run(main()))
