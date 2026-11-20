#!/home/claude/build/amiga-fleet-venv/bin/python
"""Runs the functional test in the AmigaOS 4 guest (QEMU, MCPd) against the
Docker servers, and checks the copied files by SHA-256.

    tests/os4/run_guest.py [--no-mount]

SFH_HANDLER: another handler build to test; SFH_STARTUP: more DOSDriver
options (for example "CIPHERS=aes128-ctr DEBUG"); SFH_LIBS: more host files
for RAM:sftph/libs, first in LIBS: (for example AmiSSL 6's
amisslmaster.library; AmiSSL never leaves memory, so reboot first).

Uploads build/os4/sftp-handler, a site list for 10.0.2.2 and a DOSDriver
to RAM:sftph, mounts SFTP: (CONFIGDIR=RAM:sftph/cfg), runs
tests/os4/functional.script on both sites. clib4.library 2.5 must be
loaded in the guest (see the project notes).
"""
import asyncio
import hashlib
import os
import sys

from amiga_fleet_mcp.config import load_config
from amiga_fleet_mcp.fleet import Fleet
from amiga_fleet_mcp.tools import fs as fs_tool
from amiga_fleet_mcp.transports.mcpd import McpdTransport, McpdChannel

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TARGET = "os4-amigaone"
SITES = """<?xml version="1.0" encoding="UTF-8"?>
<sftphandler version="1">
  <site name="Docker SFTP" protocol="sftp" host="10.0.2.2" port="2222" user="test" password="test123" folder="data"/>
  <site name="Docker FTP" protocol="ftp" host="10.0.2.2" port="2121" user="test" password="test123"/>
</sftphandler>
"""
DOSDRIVER = """Handler   = RAM:sftph/{handler}
StackSize = 262144
Priority  = 5
GlobVec   = -1
Startup   = "CONFIGDIR=RAM:sftph/cfg{extra}"
"""
CLIB4 = os.path.expanduser("~/build/os4-downloads/clib4-2.5/clib4/clib4.library")
DATA = {"Docker SFTP": "testserver/sftp-data", "Docker FTP": "testserver/ftp-data"}


async def cmd(m, c, timeout=120):
    r = await m.request("exec.cmd", {"command": c, "timeout_ms": timeout * 1000}, timeout_s=timeout + 30)
    return r.get("exit_code"), r.get("output", "")


async def upload(fleet, local, remote):
    r = await fs_tool.fs_upload(fleet, TARGET, local_path=local, remote_path=remote, verify=True)
    assert r.model_dump().get("sha256_verified"), remote


async def setup(fleet, m, mount=True):
    """clib4 2.5 first in LIBS:, the build mounted as SFTP: (RAM:sftph)."""
    work = os.path.join(ROOT, "build", "guest")
    os.makedirs(work, exist_ok=True)
    big = os.path.join(work, "big5.bin")
    os.makedirs(work, exist_ok=True)
    if not os.path.exists(big):
        with open(big, "wb") as f:
            f.write(os.urandom(5 * 1024 * 1024 + 77))
    # a file DOS has loaded stays "in use": every build gets a name of its own
    rev = open(os.path.join(ROOT, "src", "os4", "version.h")).read().split("SFH_REVISION")[1].split()[0]
    handler = f"sftp-handler.{rev}.{os.getpid()}"
    hostkey = open(os.path.join(ROOT, "testserver", "keys", "ssh_host_ed25519_key.pub")).read().split()
    known = f"[10.0.2.2]:2222 {hostkey[0]} {hostkey[1]}\n"
    for name, text in (("sites.xml", SITES), ("SFTP", DOSDRIVER.format(handler=handler, extra=(" " + os.environ["SFH_STARTUP"]) if os.environ.get("SFH_STARTUP") else "")),
                       ("known_hosts", known)):
        with open(os.path.join(work, name), "w") as f:
            f.write(text)
    await cmd(m, "MakeDir RAM:sftph RAM:sftph/cfg RAM:sftph/libs")
    # the guest's own clib4.library is older than the one the build needs
    # (it stays first in LIBS: during the test, else it may be flushed)
    await upload(fleet, CLIB4, "RAM:sftph/libs/clib4.library")
    for lib in os.environ.get("SFH_LIBS", "").split():
        await upload(fleet, lib, "RAM:sftph/libs/" + os.path.basename(lib))
    for c in ("Avail FLUSH", "Assign LIBS: RAM:sftph/libs System:Libs System:MUI/Libs",
              "Version amisslmaster.library", "Version clib4.library"):
        rc, out = await cmd(m, c)
        if c.startswith("Version amissl"):
            print("amisslmaster:", out.strip())
    print("clib4:", out.strip())
    if mount:
        # the old handler ends; DOS loads the new one on the next access
        rc, out = await cmd(m, "Dismount SFTP:")
        print("dismount:", rc, out.strip())
        # SFH_HANDLER: another build to test (default build/os4/sftp-handler)
        binary = os.environ.get("SFH_HANDLER", os.path.join(ROOT, "build", "os4", "sftp-handler"))
        print("handler:", binary)
        await upload(fleet, binary, "RAM:sftph/" + handler)
        await upload(fleet, os.path.join(work, "sites.xml"), "RAM:sftph/cfg/sites.xml")
        await upload(fleet, os.path.join(work, "known_hosts"), "RAM:sftph/cfg/known_hosts")
        await upload(fleet, os.path.join(work, "SFTP"), "RAM:sftph/SFTP")
        rc, out = await cmd(m, "Mount RAM:sftph/SFTP")
        print("mount:", rc, out.strip())
    return work


async def main():
    fleet = Fleet(load_config())
    m = McpdTransport(McpdChannel(enabled=True, endpoint="127.0.0.1:4422"))
    work = await setup(fleet, m, "--no-mount" not in sys.argv)
    big = os.path.join(work, "big5.bin")
    await upload(fleet, big, "RAM:sftph/big5.bin")
    await upload(fleet, os.path.join(ROOT, "tests", "os4", "functional.script"), "RAM:sftph/functional.script")
    want = hashlib.sha256(open(big, "rb").read()).hexdigest()
    ok = True
    for site, data in DATA.items():
        print(f"===== {site}")
        rc, out = await cmd(m, f'Execute RAM:sftph/functional.script "SFTP:{site}"', timeout=300)
        print(out.rstrip())
        srv = os.path.join(ROOT, data, "big5.bin")
        got_srv = hashlib.sha256(open(srv, "rb").read()).hexdigest() if os.path.exists(srv) else None
        got_back = await fs_tool.fs_hash(fleet, TARGET, "RAM:sftph/big5.back", "sha256")
        got_back = got_back.get("hash") if isinstance(got_back, dict) else getattr(got_back, "hash", got_back)
        print(f"upload on server: {'OK' if got_srv == want else 'WRONG'}")
        print(f"download in guest: {'OK' if got_back == want else 'WRONG ' + str(got_back)}")
        ok = ok and got_srv == want and got_back == want
        await cmd(m, f'Delete "SFTP:{site}/big5.bin" RAM:sftph/big5.back QUIET')
    await cmd(m, "Assign LIBS: System:Libs System:MUI/Libs")
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
