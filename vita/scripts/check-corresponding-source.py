#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail unless every linked or compiled-in GPL/LGPL component in THIRD-PARTY.md has its source in the release archive.

usage: check-corresponding-source.py ROOT SOURCE-ARCHIVE.tar.xz

A row of a THIRD-PARTY.md component table counts when its Status column says linked, compiled or bundled and its
Licence column names the GPL or LGPL (an elected permissive arm, such as FreeType's FTL, is written without them).
Each such row must be listed in COVERAGE below and the archive directory it names must hold files. The one exemption,
the GCC runtime libraries, carries its reason. A row the script does not know is a failure: add the component's source
to the archive (vita/scripts/source-archive.py) and list it here, or elect a permissive arm and say so in THIRD-PARTY.md.
"""
import re
import sys
import tarfile
from pathlib import Path

root, archive = Path(sys.argv[1]), Path(sys.argv[2])

# Row name (start of the Component cell) -> archive component directory under dependencies/, "branch" for this tree.
COVERAGE = {
    "mkxp-z": "branch",
    "vitaGL": "vitagl",
    "vitaShaRK": "vitashark",
    "SceShaccCgExt": "shacccgext",
    "OpenAL Soft": "openal-soft",
    "uchardet": "uchardet",
    "pthread-embedded": "pthread-embedded",
}
EXEMPT = {
    "libstdc++, libgcc": "GPL-3.0 with the GCC Runtime Library Exception: linking it into a program built by an "
                         "unmodified GCC imposes no source obligation on the program",
}
COPYLEFT = re.compile(r"(?<![A-Za-z])L?GPL")


def die(message):
    sys.exit("check-corresponding-source: " + message)


def cells(line):
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


rows = []
header = None
for line in (root / "THIRD-PARTY.md").read_text(encoding="utf-8").splitlines():
    if not line.startswith("|"):
        header = None
        continue
    fields = cells(line)
    if fields[0] == "Component":
        header = fields
    elif header and len(header) >= 6 and len(fields) == len(header) and not set(line) <= set("|- "):
        rows.append(dict(zip(header, fields)))
if not rows:
    die("no component rows found in THIRD-PARTY.md")

wanted = {}
for row in rows:
    name = row["Component"].replace("`", "")
    if not re.search(r"linked|compiled|bundled", row["Status"]) or not COPYLEFT.search(row["Licence"]):
        continue
    key = next((k for k in list(COVERAGE) + list(EXEMPT) if name.startswith(k)), None)
    if key is None:
        die("%r is linked and %s, but has no source in the release archive (not listed in check-corresponding-source.py)"
            % (name, row["Licence"].split(";")[0]))
    if key in COVERAGE:
        wanted[key] = COVERAGE[key]

counts = {}
with tarfile.open(archive, "r:xz") as tar:
    for member in tar:
        if not member.isfile():
            continue
        parts = member.name.split("/")
        if len(parts) > 2 and parts[1] == "branch":
            counts["branch"] = counts.get("branch", 0) + 1
        elif len(parts) > 3 and parts[1] == "dependencies":
            counts[parts[2]] = counts.get(parts[2], 0) + 1
missing = sorted(key for key, folder in wanted.items() if not counts.get(folder))
if missing:
    die("the archive holds no source for: %s" % ", ".join(missing))
print("check-corresponding-source: %d copyleft components covered (%s); exempt: %s" % (
    len(wanted), ", ".join(sorted(wanted)), ", ".join(sorted(EXEMPT))))
