#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Content digests of the engine tree: the build receipt, the packager check and the measurement header.

  treedigest.py engine ROOT                 print the engine-tree digest
  treedigest.py measure-header ROOT OUT     write the VITA_MEASURE_SOURCE_SHA256 header
  treedigest.py seal DIR                    record the content digest of a fetched source tree in DIR/.mkxpz-tree-sha256
  treedigest.py verify DIR                  fail unless DIR still matches its recorded digest (any edit, addition or removal)
"""
import hashlib
import os
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


def lib_digest(files):
    """SHA-256 over file names (not paths) and bytes of the given files, sorted by path."""
    h = hashlib.sha256()
    for path in sorted(files):
        h.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    return h.hexdigest()


SEAL = ".mkxpz-tree-sha256"


def source_tree_digest(directory):
    """Digest of every file (and symlink target) under a fetched source tree: names, exec bit, bytes."""
    h = hashlib.sha256()
    directory = Path(directory)
    for base, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if d != ".git")
        for name in sorted(files):
            path = Path(base, name)
            rel = path.relative_to(directory).as_posix()
            if rel == SEAL:
                continue
            if path.is_symlink():
                h.update(b"L" + rel.encode() + b"\0" + os.readlink(path).encode() + b"\0")
            else:
                h.update(b"X" if os.access(path, os.X_OK) else b"F")
                h.update(rel.encode() + b"\0" + path.read_bytes() + b"\0")
    return h.hexdigest()


def verify_source_tree(directory):
    seal = Path(directory, SEAL)
    if not seal.is_file():
        sys.exit("treedigest: %s has no %s: it was not fetched by this build; remove it and rebuild" % (directory, SEAL))
    if seal.read_text().strip() != source_tree_digest(directory):
        sys.exit("treedigest: %s differs from what was fetched (edited, or built in tree); remove it and rebuild" % directory)


def main(argv):
    if len(argv) == 3 and argv[1] == "engine":
        print(digest(argv[2], ENGINE))
    elif len(argv) == 4 and argv[1] == "measure-header":
        Path(argv[3]).write_text(
            '#define VITA_MEASURE_SOURCE_SHA256 "%s"\n' % digest(argv[2], ENGINE + GLUE))
    elif len(argv) == 3 and argv[1] == "seal":
        Path(argv[2], SEAL).write_text(source_tree_digest(argv[2]) + "\n")
    elif len(argv) == 3 and argv[1] == "verify":
        verify_source_tree(argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
