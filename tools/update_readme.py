#!/usr/bin/env python3
"""Copy the History of package/SFTPHandler.readme into README.md as its
"Changelog" section (between the changelog markers). Public domain.

    tools/update_readme.py [--check]

--check only reports (exit 1) when README.md is out of date.
make_lha.sh runs it, so every release updates the GitHub page.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
START = "<!-- changelog start: made by tools/update_readme.py from package/SFTPHandler.readme -->"
END = "<!-- changelog end -->"


def history():
    text = open(os.path.join(ROOT, "package", "SFTPHandler.readme"), encoding="utf-8").read()
    m = re.search(r"^History:\n\n(.*?)\n\n(?=\S)", text, re.S | re.M)
    if not m:
        sys.exit("package/SFTPHandler.readme: no History section")
    items = []
    for line in m.group(1).splitlines():
        if line.startswith("- "):
            items.append(line[2:].strip())
        else:
            items[-1] += " " + line.strip()
    out = []
    for it in items:
        m = re.match(r"(\d+\.\d+(?: beta)?) \(([\d-]+)\):\s*(.*)", it)
        if m:
            out.append("- **%s** (%s): %s" % m.groups())
        else:
            out.append("- " + it)
    return "\n".join(out)


def main():
    path = os.path.join(ROOT, "README.md")
    readme = open(path, encoding="utf-8").read()
    block = START + "\n" + history() + "\n" + END
    if START in readme:
        new = re.sub(re.escape(START) + r".*?" + re.escape(END), lambda _: block, readme, flags=re.S)
    else:
        sys.exit("README.md has no changelog markers")
    if new == readme:
        return 0
    if "--check" in sys.argv:
        print("README.md changelog is out of date: run tools/update_readme.py", file=sys.stderr)
        return 1
    open(path, "w", encoding="utf-8").write(new)
    print("README.md changelog updated")
    return 0


sys.exit(main())
