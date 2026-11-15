#!/home/claude/build/amiga-fleet-venv/bin/python
"""Times copies to and from both Docker sites in the AmigaOS 4 guest.

    tests/os4/bench.py [--no-mount] [MB] [BUF]

MB: file size (default 20). BUF: Copy's BUF= value (default: Copy's own).
"""
import asyncio
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_guest as rg  # noqa: E402


async def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mb = int(args[0]) if args else 20
    buf = f" BUF={args[1]}" if len(args) > 1 else ""
    fleet = rg.Fleet(rg.load_config())
    m = rg.McpdTransport(rg.McpdChannel(enabled=True, endpoint="127.0.0.1:4422"))
    work = await rg.setup(fleet, m, "--no-mount" not in sys.argv)
    local = os.path.join(work, f"bench{mb}.bin")
    if not os.path.exists(local):
        with open(local, "wb") as f:
            f.write(os.urandom(mb * 1024 * 1024))
    await rg.upload(fleet, local, f"RAM:sftph/bench{mb}.bin")
    # the first access connects: not part of the timing
    for site in rg.DATA:
        await rg.cmd(m, f'List "SFTP:{site}" QUICK')
    for site in rg.DATA:
        for what, c in (("upload", f'Copy RAM:sftph/bench{mb}.bin "SFTP:{site}/bench.bin"{buf}'),
                        ("download", f'Copy "SFTP:{site}/bench.bin" RAM:sftph/back.bin{buf}')):
            t = time.time()
            rc, out = await rg.cmd(m, c, timeout=900)
            dt = time.time() - t
            print(f"{site:12s} {what:8s} {mb} MB {dt:6.1f} s  {mb / dt:6.2f} MB/s  rc={rc} {out.strip()[:80]}")
        await rg.cmd(m, f'Delete "SFTP:{site}/bench.bin" RAM:sftph/back.bin QUIET')
    t = time.time()
    await rg.cmd(m, f"Copy RAM:sftph/bench{mb}.bin RAM:sftph/ramcopy.bin")
    print(f"RAM to RAM    {mb} MB {time.time() - t:6.1f} s")
    await rg.cmd(m, f"Delete RAM:sftph/ramcopy.bin RAM:sftph/bench{mb}.bin QUIET")
    await rg.cmd(m, "Assign LIBS: System:Libs System:MUI/Libs")


asyncio.run(main())
