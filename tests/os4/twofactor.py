#!/home/claude/build/amiga-fleet-venv/bin/python
"""Two-factor logins in the AmigaOS 4 guest, against testserver/otp
(10.0.2.2:2223). Run tests/os4/run_guest.py first: it mounts the build as
SFTP: with RAM:sftph/cfg.

    tests/os4/twofactor.py prep          sites Code, KeyCode, KeyPass + host key + key file
    tests/os4/twofactor.py list SITE     starts List "SFTP:SITE" in the background
    tests/os4/twofactor.py out           what that List printed
    tests/os4/twofactor.py code          the current code (tests/totp.py)

The code requester opens on the guest's screen: type the code there (the
string gadget is active) and press Return.
"""
import asyncio
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import run_guest  # noqa: E402
from totp import totp  # noqa: E402

from amiga_fleet_mcp.config import load_config  # noqa: E402
from amiga_fleet_mcp.fleet import Fleet  # noqa: E402
from amiga_fleet_mcp.transports.mcpd import McpdTransport, McpdChannel  # noqa: E402

ROOT = run_guest.ROOT
KEY = "RAM:sftph/cfg/client_ed25519"
SITES = f"""<?xml version="1.0" encoding="UTF-8"?>
<sftphandler version="1">
  <site name="Docker SFTP" protocol="sftp" host="10.0.2.2" port="2222" user="test" password="test123" folder="data"/>
  <site name="Docker FTP" protocol="ftp" host="10.0.2.2" port="2121" user="test" password="test123"/>
  <site name="Code" protocol="sftp" host="10.0.2.2" port="2223" user="otp" password="otp123" folder="data"/>
  <site name="KeyCode" protocol="sftp" host="10.0.2.2" port="2223" user="keyotp" keyfile="{KEY}" folder="data"/>
  <site name="KeyPass" protocol="sftp" host="10.0.2.2" port="2223" user="keypw" password="keypw123" keyfile="{KEY}" folder="data"/>
</sftphandler>
"""
OUT = "RAM:sftph/2fa.txt"


async def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "prep"
    fleet = Fleet(load_config())
    m = McpdTransport(McpdChannel(enabled=True, endpoint="127.0.0.1:4422"))
    if what == "prep":
        work = os.path.join(ROOT, "build", "guest")
        os.makedirs(work, exist_ok=True)
        key = open(os.path.join(ROOT, "testserver", "keys", "ssh_host_ed25519_key.pub")).read().split()
        files = {
            "sites.xml": SITES,
            "known_hosts": "".join(f"[10.0.2.2]:{p} {key[0]} {key[1]}\n" for p in (2222, 2223)),
        }
        for name, text in files.items():
            with open(os.path.join(work, name), "w") as f:
                f.write(text)
            await run_guest.upload(fleet, os.path.join(work, name), "RAM:sftph/cfg/" + name)
        await run_guest.upload(fleet, os.path.join(ROOT, "testserver", "keys", "client_ed25519"), KEY)
        print("sites, known_hosts and key file in RAM:sftph/cfg")
    elif what == "list":
        site = sys.argv[2]
        rc, out = await run_guest.cmd(m, f'Run >NIL: List "SFTP:{site}" >{OUT}')
        print("started:", rc, out.strip())
    elif what == "out":
        rc, out = await run_guest.cmd(m, f"Type {OUT}")
        print(out.rstrip())
    elif what == "code":
        print(totp())


asyncio.run(main())
