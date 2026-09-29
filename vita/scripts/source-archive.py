#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the corresponding-source archive of a release: this branch plus every dependency source the build fetched.

usage: source-archive.py ROOT OUT.tar.xz

Git-based dependencies are archived as pristine trees at the commit the build verified (the patches that change them
are in this branch's vita/patches/). Dependencies that come as release tarballs are archived as the extracted tree the
build used. Prints one JSON record (file, sha256, size, components) on stdout. Members are sorted and carry the
branch's commit time, so the same inputs give the same archive.
"""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

root = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
build = root / "build"
SKIP_DIRS = {".git", ".libs", ".deps", "__pycache__"}
SKIP_SUFFIXES = (".o", ".a", ".lo", ".la", ".obj", ".so", ".dylib", ".pyc")


def die(message):
    sys.exit("source-archive: " + message)


def git(directory, *args):
    return subprocess.run(["git", "-C", str(directory), *args], check=True, capture_output=True, text=True).stdout.strip()


stamp = int(git(root, "log", "-1", "--format=%ct"))
version = (root / "vita/VERSION").read_text().strip()
top = "mkxp-z-vita-source-%s" % version
components = []


def add_member(tar, info, data, prefix):
    info.name = prefix + "/" + info.name
    info.mtime, info.uid, info.gid, info.uname, info.gname = stamp, 0, 0, "", ""
    info.mode = info.mode & 0o755 | 0o644
    tar.addfile(info, data)


def add_git_archive(tar, directory, commit, prefix):
    proc = subprocess.Popen(["git", "-C", str(directory), "archive", "--format=tar", commit], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=proc.stdout, mode="r|") as source:
        for info in source:
            if info.type == tarfile.XGLTYPE:
                continue
            add_member(tar, info, source.extractfile(info) if info.isreg() else None, prefix)
    if proc.wait():
        die("git archive failed in %s" % directory)


def add_git_component(tar, name, directory, commit, upstream, folder=None):
    directory = Path(directory)
    if not (directory / ".git").exists():
        die("%s: no git source at %s; run the build first" % (name, directory))
    prefix = "%s/%s" % (top, folder or "dependencies/" + name)
    add_git_archive(tar, directory, commit, prefix)
    for line in git(directory, "submodule", "status", "--recursive").splitlines():
        fields = line.lstrip(" +-U").split()
        add_git_archive(tar, directory / fields[1], fields[0], prefix + "/" + fields[1])
    components.append({"name": name, "kind": "git", "revision": commit, "upstream": upstream})


def add_tree_component(tar, name, directory, upstream):
    directory = Path(directory)
    if not directory.is_dir():
        die("%s: no source tree at %s; run the build first" % (name, directory))
    prefix = "%s/dependencies/%s" % (top, name)
    count = 0
    for base, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for filename in sorted(files):
            path = Path(base, filename)
            if filename.endswith(SKIP_SUFFIXES) or path.is_symlink() and not path.exists():
                continue
            info = tar.gettarinfo(str(path), arcname=prefix + "/" + path.relative_to(directory).as_posix())
            info.mtime, info.uid, info.gid, info.uname, info.gname = stamp, 0, 0, "", ""
            info.mode = info.mode & 0o755 | 0o644
            with open(path, "rb") as handle:
                tar.addfile(info, handle if info.isreg() else None)
            count += 1
    components.append({"name": name, "kind": "tree", "files": count, "upstream": upstream})


def record_text():
    lines = ["Corresponding source of this release. Each directory under dependencies/ is the source the build used;",
             "the changes this branch makes to them are in branch/vita/patches/ and applied by branch/vita/scripts/.", ""]
    for item in components:
        lines.append("%s  %s  %s" % (item["name"], item.get("revision") or "extracted tree", item["upstream"]))
    return ("\n".join(lines) + "\n").encode()


out.parent.mkdir(parents=True, exist_ok=True)
xz = shutil.which("xz")
sink = open(out, "wb") if xz else None
proc = subprocess.Popen([xz, "-T0", "-6", "-c"], stdin=subprocess.PIPE, stdout=sink) if xz else None
tar = tarfile.open(fileobj=proc.stdin, mode="w|") if proc else tarfile.open(out, "w:xz", preset=6)

add_git_component(tar, "branch", root, git(root, "rev-parse", "HEAD"), "this repository", folder="branch")
for pin in json.loads((root / "vita/scripts/vitagl-pins.json").read_text()):
    add_git_component(tar, pin["name"], build / "vitagl-src" / pin["name"], pin["commit"], pin["repo"])
ruby = build / "ruby-src"
add_git_component(tar, "ruby", ruby, git(ruby, "rev-parse", "HEAD"), "https://github.com/mkxp-z/ruby")
deps = build / "vita-deps/src"
add_git_component(tar, "sdl_sound", deps / "sdl_sound", git(deps / "sdl_sound", "rev-parse", "HEAD"),
                  "https://github.com/mkxp-z/SDL_sound")
add_git_component(tar, "uchardet", deps / "uchardet", git(deps / "uchardet", "rev-parse", "HEAD"),
                  "https://gitlab.freedesktop.org/uchardet/uchardet")
add_tree_component(tar, "sdl2", build / "sdl2-vitagl-src", "https://github.com/libsdl-org/SDL")
add_tree_component(tar, "libtheora", deps / "theora", "https://downloads.xiph.org/releases/theora/")
add_tree_component(tar, "pixman", deps / "pixman", "https://cairographics.org/releases/")
openal = sorted(deps.glob("openal-soft-*"))
if not openal:
    die("openal: no source tree under %s; run the build first" % deps)
add_tree_component(tar, "openal-soft", openal[0], "https://github.com/kcat/openal-soft")
if (build / "third-party/tinysoundfont").is_dir():
    add_tree_component(tar, "tinysoundfont", build / "third-party/tinysoundfont", "https://github.com/schellingb/TinySoundFont")
add_tree_component(tar, "fonts", build / "fonts", "https://vlgothic.dicey.org/ and https://github.com/liberationfonts/liberation-fonts")

data = record_text()
info = tarfile.TarInfo("%s/SOURCES.txt" % top)
info.size, info.mtime, info.mode = len(data), stamp, 0o644
tar.addfile(info, io.BytesIO(data))
tar.close()
if proc:
    proc.stdin.close()
    if proc.wait():
        die("xz failed")
    sink.close()

digest = hashlib.sha256()
with open(out, "rb") as handle:
    for block in iter(lambda: handle.read(1 << 20), b""):
        digest.update(block)
print(json.dumps({"file": out.name, "sha256": digest.hexdigest(), "size": out.stat().st_size, "components": components}))
