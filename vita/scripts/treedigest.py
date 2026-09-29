#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Content digests of the engine tree: the build receipt, the packager check and the measurement header.

  treedigest.py engine ROOT                 print the engine-tree digest
  treedigest.py measure-header ROOT OUT     write the VITA_MEASURE_SOURCE_SHA256 header
"""
import hashlib
import subprocess
import sys
from pathlib import Path

ENGINE = ("src", "binding", "shader", "assets", "meson.build", "meson_options.txt")
GLUE = ("vita/glue",)


def listed(root, paths):
    out = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "-z", "-c", "-o", "--exclude-standard", "--", *paths])
    return sorted({p for p in out.decode().split("\0") if p})


def digest(root, paths):
    """SHA-256 over the working-tree bytes of every listed file, names included."""
    h = hashlib.sha256()
    for rel in listed(root, paths):
        path = Path(root, rel)
        if path.is_file():
            h.update(rel.encode() + b"\0" + path.read_bytes() + b"\0")
    return h.hexdigest()


def main(argv):
    if len(argv) == 3 and argv[1] == "engine":
        print(digest(argv[2], ENGINE))
    elif len(argv) == 4 and argv[1] == "measure-header":
        Path(argv[3]).write_text(
            '#define VITA_MEASURE_SOURCE_SHA256 "%s"\n' % digest(argv[2], ENGINE + GLUE))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
