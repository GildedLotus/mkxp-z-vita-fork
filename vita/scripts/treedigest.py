#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Content digests of the engine tree: the build receipt, the packager check and the measurement header.

  treedigest.py engine ROOT                 print the engine-tree digest
  treedigest.py measure-header ROOT OUT     write the VITA_MEASURE_SOURCE_SHA256 header
  treedigest.py identity NAME               print the input identity (URL, version, tarball and patch digests) of a pinned tree
  treedigest.py seal DIR NAME               record that identity and DIR's content digest in DIR/.mkxpz-tree-sha256
  treedigest.py state DIR NAME              print current, stale (extract again) or edited (refuse) for a sealed tree
  treedigest.py verify DIR NAME             fail unless DIR is current
"""
import hashlib
import json
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
ROOT = Path(__file__).resolve().parents[2]


def die(message):
    sys.exit("treedigest: " + message)


def walk_tree(directory):
    """Every member of a fetched source tree, sorted: (relative name, path, "file" or "link").

    Symlinks, to files and to directories alike, are members in their own right and are never followed. A target
    that is absolute or leads out of the tree is refused: it could name the builder's machine, and it would not
    mean the same thing on the machine that unpacks the archive."""
    directory = Path(directory)
    top = os.path.realpath(directory)
    for base, dirs, files in os.walk(directory):
        dirs.sort()
        members = []
        for name in list(dirs):
            path = Path(base, name)
            if name == ".git":
                dirs.remove(name)
            elif path.is_symlink():
                dirs.remove(name)
                members.append(name)
        members += files
        for name in sorted(members):
            path = Path(base, name)
            rel = path.relative_to(directory).as_posix()
            if path.is_symlink():
                target = os.readlink(path)
                lexical = os.path.normpath(os.path.join(os.path.dirname(rel), target))
                real = os.path.realpath(path)
                if (os.path.isabs(target) or lexical == ".." or lexical.startswith("../")
                        or not (real == top or real.startswith(top + os.sep)) and os.path.lexists(real)):
                    die("%s: symlink %s points to %r, outside the tree" % (directory, rel, target))
                yield rel, path, "link"
            elif path.is_file():
                yield rel, path, "file"
            else:
                die("%s: %s is neither a regular file nor a symlink" % (directory, rel))


def source_tree_digest(directory):
    """Digest of every member of a fetched source tree: names, symlink targets, exec bits, bytes."""
    h = hashlib.sha256()
    for rel, path, kind in walk_tree(directory):
        if rel == SEAL:
            continue
        if kind == "link":
            h.update(b"L" + rel.encode() + b"\0" + os.readlink(path).encode() + b"\0")
        else:
            h.update(b"X" if os.access(path, os.X_OK) else b"F")
            h.update(rel.encode() + b"\0" + path.read_bytes() + b"\0")
    return h.hexdigest()


def sha256_text(text):
    return hashlib.sha256(text.encode()).hexdigest()


def input_identity(name):
    """What a sealed tree must have been made from: the pinned URL, version and tarball digest, and the digest of
    every patch or text transformation the build applies before sealing. The tree is reusable only while this
    equals the current pins."""
    pins = json.loads((ROOT / "vita/scripts/dep-pins.json").read_text())
    if name == "theora":
        pin = pins["theora"]
        return {"name": name, "version": pin["version"], "url": pin["url"], "tarballSha256": pin["sha256"]}
    if name == "pixman":
        pin = pins["pixman"]
        return {"name": name, "version": pin["version"], "url": pin["url"], "tarballSha256": pin["sha256"],
                "transformSha256": {"pixmanArmAsmSed": sha256_text(pins["transforms"]["pixmanArmAsmSed"])}}
    if name == "openal":
        pin = pins["openal"]
        return {"name": name, "version": pin["version"], "url": pin["url"], "tarballSha256": pin["sha256"],
                "patchUrl": pin["patchUrl"], "patchSha256": pin["patchSha256"],
                "transformSha256": {"openalSzfmtSed": sha256_text(pins["transforms"]["openalSzfmtSed"])}}
    if name == "sdl2":
        out = subprocess.run(["bash", "-c", '. "$1"; printf "%s\\n%s\\n%s" "$SDL2_VER" "$SDL2_URL" "$SDL2_SHA256"',
                              "_", str(ROOT / "vita/scripts/sdl2-pin.sh")],
                             check=True, capture_output=True, text=True).stdout.split("\n")
        patches = sorted((ROOT / "vita/patches/sdl2").glob("[0-9][0-9][0-9][0-9]-*.patch"))
        if not patches:
            die("no SDL2 patches under vita/patches/sdl2")
        return {"name": name, "version": out[0], "url": out[1], "tarballSha256": out[2],
                "patchSha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches}}
    die("no input identity for %r" % name)


def read_seal(directory):
    """(input identity, tree digest) of a seal in the current format, or None for a missing or older one."""
    try:
        record = json.loads(Path(directory, SEAL).read_text())
        return record["input"], record["tree"]
    except (OSError, ValueError, KeyError, TypeError):
        return None


def tree_state(directory, name):
    """"current": sealed from exactly the current inputs and unchanged since. "stale": absent, sealed before seals
    carried an input identity, or sealed from other inputs (a pin or patch changed): extract it again. "edited":
    sealed from the current inputs but its content differs (edited, or built in place): refuse it."""
    if not Path(directory).is_dir():
        return "stale"
    seal = read_seal(directory)
    if seal is None or seal[0] != input_identity(name):
        return "stale"
    return "current" if seal[1] == source_tree_digest(directory) else "edited"


def verify_source_tree(directory, name):
    state = tree_state(directory, name)
    if state == "stale":
        die("%s is not a tree sealed from the current %s pins and patches: remove it and rebuild" % (directory, name))
    if state == "edited":
        die("%s differs from what was fetched (edited, or built in tree); remove it and rebuild" % directory)


def main(argv):
    if len(argv) == 3 and argv[1] == "engine":
        print(digest(argv[2], ENGINE))
    elif len(argv) == 4 and argv[1] == "measure-header":
        Path(argv[3]).write_text(
            '#define VITA_MEASURE_SOURCE_SHA256 "%s"\n' % digest(argv[2], ENGINE + GLUE))
    elif len(argv) == 3 and argv[1] == "identity":
        print(json.dumps(input_identity(argv[2]), sort_keys=True))
    elif len(argv) == 4 and argv[1] == "seal":
        Path(argv[2], SEAL).write_text(json.dumps(
            {"input": input_identity(argv[3]), "tree": source_tree_digest(argv[2])}, indent=1, sort_keys=True) + "\n")
    elif len(argv) == 4 and argv[1] == "state":
        print(tree_state(argv[2], argv[3]))
    elif len(argv) == 4 and argv[1] == "verify":
        verify_source_tree(argv[2], argv[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
