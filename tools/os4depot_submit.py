#!/usr/bin/env python3
"""Submit a new sftp-handler version to OS4Depot, replacing the published
driver/handler/sftphandler.lha. Submissions wait in the maintainer's queue
until approved.

The file id changes with every replace, so it is never stored here: the
script opens the file's readme page, follows its "Replace file" link and
sends to that form (its action and its f_replaces value).

  tools/os4depot_submit.py           show what would be sent (nothing is sent)
  tools/os4depot_submit.py --send    send it (multipart POST, like the web form);
                                     the email comes from the readme (sent hidden)

Run tools/make_lha.sh first: it writes dist/SFTPHandler.lha and build/stage/SFTPHandler.readme.

Only releases from the main branch may be sent. Builds from other branches
are betas (dist/SFTPHandler_beta.lha) and never go to OS4Depot: the script
refuses them.
"""
import html
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ARCHIVE = os.path.join(ROOT, "dist", "SFTPHandler.lha")
README = os.path.join(ROOT, "build", "stage", "SFTPHandler.readme")
SITE = "https://os4depot.net/"
PUBLISHED = "driver/handler/sftphandler.lha"
AGENT = "Mozilla/5.0 (compatible; os4depot-submit)"


def refuse_beta():
    """Betas (any branch but main) must never reach OS4Depot."""
    branch = subprocess.run(["git", "-C", ROOT, "branch", "--show-current"],
                            capture_output=True, text=True).stdout.strip()
    if branch != "main":
        sys.exit(f"Refused: branch {branch!r} is a beta branch; only main releases go to OS4Depot.")
    staged = os.path.join(ROOT, "build", "stage", "SFTPHandler", "L", "sftp-handler")
    for f in (README, staged):
        with open(f, "rb") as fh:
            data = fh.read()
        # what make_lha.sh / build_os4.sh put into beta builds (the History
        # may mention earlier betas)
        if b") beta" in data or b"THIS IS A BETA" in data or re.search(rb"^description: .* - BETA$", data, re.M):
            sys.exit(f"Refused: {f} is from a beta build; run tools/make_lha.sh on main.")


refuse_beta()


def fetch(url):
    return subprocess.run(["curl", "-sS", "--max-time", "60", "-A", AGENT, url],
                          capture_output=True, check=True).stdout.decode("latin-1")


# readme page -> "Replace file" link -> the replace form
page = fetch(SITE + "?function=showfile&file=" + PUBLISHED)
m = re.search(r"<A HREF='([^']*replace=\d+[^']*)'><IMG ALT='Replace file'", page, re.I)
if m is None:
    sys.exit("no 'Replace file' link on the readme page of " + PUBLISHED)
form = fetch(SITE + html.unescape(m.group(1)).lstrip("/"))
action = re.search(r"<FORM[^>]*enctype='multipart/form-data'[^>]*ACTION='([^']*)'", form, re.I)
replaces = re.search(r"NAME='f_replaces' VALUE='([^']*)'", form, re.I)
if action is None or replaces is None or replaces.group(1) != PUBLISHED:
    sys.exit("the replace form is not the one for " + PUBLISHED +
             f" (f_replaces {replaces.group(1) if replaces else None!r})")
URL = SITE + html.unescape(action.group(1)).lstrip("/")

readme = open(README, encoding="latin-1").read()
head, body = readme.split("hend:\n", 1)
body = body.strip() + "\n"
meta = dict(l.split(":", 1) for l in head.splitlines() if ":" in l)
meta = {k.strip(): v.strip() for k, v in meta.items()}

fields = [
    ("f_name", meta["name"]),
    ("f_description", meta["description"]),
    ("f_version", meta["version"]),
    ("f_author", meta["author"]),
    ("f_submitter", meta["submitter"]),
    ("f_email", meta.get("email") or os.environ.get("OS4DEPOT_EMAIL", "")),
    ("f_hideemail", "on"),
    ("f_url", meta.get("url", "")),
    ("f_donateurl", ""),
    ("f_category", meta["category"]),
    ("f_replaces", replaces.group(1)),
    ("f_requirements", meta["requirements"]),
    ("f_license", meta["license"]),
    ("f_distributesubm", "selected"),
    ("f_distribute", "on"),
    ("f_passphrase", ""),
    ("f_minosversion", meta["minosversion"]),
    ("f_texttype", meta["texttype"]),
    ("f_text", body),
    ("f_comments", ""),
    ("f_submit_go", "Submit"),
]

print(f"POST {URL}")
for name, value in fields:
    shown = f"<readme body, {len(value)} chars>" if name == "f_text" else value
    print(f"  {name:17} = {shown!r}")
print(f"  {'f_userfile':17} = @{ARCHIVE} ({os.path.getsize(ARCHIVE)} bytes)")
assert len(meta["description"]) <= 50 and len(meta["name"]) <= 20 and len(meta["requirements"]) <= 200

if "--send" not in sys.argv:
    print("\n(dry run: nothing sent)")
    sys.exit(0)
if not dict(fields)["f_email"]:
    sys.exit("not sent: no email (readme email: field or OS4DEPOT_EMAIL)")

cmd = ["curl", "-sS", "-o", "/dev/stdout", "-w", "\nHTTP %{http_code}\n", "--max-time", "600",
       "-A", "Mozilla/5.0 (compatible; os4depot-submit)"]
for name, value in fields:
    cmd += ["--form-string", f"{name}={value}"]
cmd += ["-F", f"f_userfile=@{ARCHIVE};type=application/x-lha;filename=SFTPHandler.lha", URL]
# the form is ISO-8859-15: send the values as Latin-1 bytes ("Bj\u00f6rn"), not UTF-8
cmd = [c.encode("latin-1") for c in cmd]
out = subprocess.run(cmd, capture_output=True, text=True, encoding="latin-1")
open(os.path.join(ROOT, "build", "os4depot_response.html"), "w",
     encoding="latin-1").write(out.stdout)
print(out.stdout[-300:], out.stderr)
