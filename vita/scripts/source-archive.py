#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the corresponding-source archive of a release: this branch plus every dependency source the build fetched.

usage: source-archive.py ROOT OUT.tar.xz

Git-based dependencies are archived as pristine trees at the commit the build verified (the patches that change them
are in this branch's vita/patches/). Dependencies that come as release tarballs are archived as the sealed tree the
build extracted; the seal (vita/scripts/treedigest.py) proves nobody edited it or built inside it. Every input is
checked before the output file is opened, every member is scanned for the builder's home directory, hostname, VitaSDK
path and checkout path, and the archive appears under OUT only when complete: any failure removes the partial file.
Prints one JSON record (file, sha256, size, components) on stdout. Members are sorted and carry the branch's commit
time, so the same inputs give the same archive.
"""
import hashlib
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tarfile
from pathlib import Path

root = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
build = root / "build"
scripts = root / "vita/scripts"
sys.dont_write_bytecode = True
sys.path.insert(0, str(scripts))
import treedigest  # noqa: E402

SKIP_DIRS = {".git", ".libs", ".deps", "__pycache__"}
SKIP_SUFFIXES = (".o", ".a", ".lo", ".la", ".obj", ".so", ".dylib", ".pyc")


def die(message):
    sys.exit("source-archive: " + message)


def git(directory, *args):
    return subprocess.run(["git", "-C", str(directory), *args], check=True, capture_output=True, text=True).stdout.strip()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def forbidden():
    """label -> bytes that must appear nowhere in the archive (names or contents)."""
    found = {}

    def add(label, value):
        if value and len(value) >= 4 and value != "/":
            found.setdefault(label, set()).add(value.encode())

    for value in (os.path.expanduser("~"), os.environ.get("HOME", "")):
        add("the home directory", value)
        add("the home directory", os.path.realpath(value) if value else "")
    for value in (str(root), os.path.realpath(root)):
        add("the checkout path", value)
    sdk = os.environ.get("VITASDK", "")
    add("the VitaSDK path", sdk)
    add("the VitaSDK path", os.path.realpath(sdk) if sdk else "")
    host = socket.gethostname()
    for value in (host, host.split(".")[0]):
        if len(value) >= 5:
            add("the hostname", value)
        else:
            print("source-archive: hostname %r is shorter than 5 characters and is not scanned for" % value, file=sys.stderr)
    return found


FORBIDDEN = forbidden()


def scan(name, data):
    for label, needles in FORBIDDEN.items():
        for needle in needles:
            if needle in data or needle in name.encode():
                die("member %s contains %s: refusing to publish it" % (name, label))


stamp = int(git(root, "log", "-1", "--format=%ct"))
version = (root / "vita/VERSION").read_text().strip()
top = "mkxp-z-vita-source-%s" % version
deps = build / "vita-deps/src"
pins = json.loads((scripts / "dep-pins.json").read_text())
plan = []  # (kind, name, args...) in archive order; built before anything is written
components = []


def plan_git(name, directory, commit, upstream, folder=None, extras=(), clean=False):
    directory = Path(directory)
    if not (directory / ".git").exists():
        die("%s: no git source at %s; run the build first" % (name, directory))
    if subprocess.run(["git", "-C", str(directory), "cat-file", "-e", commit + "^{commit}"]).returncode:
        die("%s: commit %s is not in %s" % (name, commit, directory))
    if clean and git(directory, "status", "--porcelain", "--untracked-files=no"):
        die("%s: the checkout has tracked edits; refusing to archive it as commit %s" % (name, commit))
    record = {"name": name, "kind": "git", "revision": commit, "upstream": upstream}
    plan.append(("git", record, directory, folder or "dependencies/" + name, list(extras)))
    return record


def plan_tree(name, directory, upstream, pin=None):
    directory = Path(directory)
    if not directory.is_dir():
        die("%s: no source tree at %s; run the build first" % (name, directory))
    treedigest.verify_source_tree(directory)
    record = {"name": name, "kind": "tree", "upstream": upstream}
    if pin:
        record["sha256"] = pin
    plan.append(("tree", record, directory))
    return record


def plan_files(name, directory, files, upstream, extra=None):
    """A fixed set of files whose digests are recorded in a manifest: nothing else in the directory is archived."""
    directory = Path(directory)
    members = []
    for filename, digest in files.items():
        path = directory / filename
        if not path.is_file():
            die("%s: %s is missing from %s; run the build first" % (name, filename, directory))
        if sha256(path) != digest:
            die("%s: %s does not match its pinned digest" % (name, filename))
        members.append((filename, path))
    record = {"name": name, "kind": "files", "files": {f: d for f, d in files.items()}, "upstream": upstream}
    record.update(extra or {})
    plan.append(("files", record, "dependencies/" + name, members))
    return record


# The branch itself: a tree with edits is not what the release describes.
if git(root, "status", "--porcelain"):
    die("the working tree is dirty; the archive is made from a committed tree")
components.append(plan_git("branch", root, git(root, "rev-parse", "HEAD"), "this repository", folder="branch"))
for pin in json.loads((scripts / "vitagl-pins.json").read_text()):
    components.append(plan_git(pin["name"], build / "vitagl-src" / pin["name"], pin["commit"], pin["repo"]))

ruby = build / "ruby-src"
gnu = pins["gnuConfig"]
gnu_files = []
for filename in ("config.guess", "config.sub"):
    path = ruby / "tool" / filename
    if not path.is_file() or sha256(path) != gnu[filename]["sha256"]:
        die("ruby: tool/%s is missing or is not the pinned copy; run the build first" % filename)
    gnu_files.append((filename, path))
ruby_record = plan_git("ruby", ruby, git(ruby, "rev-parse", "HEAD"), "https://github.com/mkxp-z/ruby",
                       extras=[("tool/" + f, p) for f, p in gnu_files])
ruby_record["gnuConfig"] = {f: gnu[f]["sha256"] for f, _ in gnu_files}
components.append(ruby_record)

components.append(plan_git("sdl_sound", deps / "sdl_sound", git(deps / "sdl_sound", "rev-parse", "HEAD"),
                           "https://github.com/mkxp-z/SDL_sound"))
components.append(plan_git("uchardet", deps / "uchardet", pins["uchardet"]["commit"], pins["uchardet"]["repo"], clean=True))
components.append(plan_git("pthread-embedded", deps / "pthread-embedded", pins["pthreadEmbedded"]["commit"],
                           pins["pthreadEmbedded"]["repo"], clean=True))
if git(deps / "uchardet", "rev-parse", "HEAD") != pins["uchardet"]["commit"]:
    die("uchardet: the checkout is not at the pinned commit")
if git(deps / "pthread-embedded", "rev-parse", "HEAD") != pins["pthreadEmbedded"]["commit"]:
    die("pthread-embedded: the checkout is not at the pinned commit")

sdl2_pin = subprocess.run(["bash", "-c", '. "$1"; printf %s "$SDL2_SHA256"', "_", str(scripts / "sdl2-pin.sh")],
                          check=True, capture_output=True, text=True).stdout
components.append(plan_tree("sdl2", build / "sdl2-vitagl-src", "https://github.com/libsdl-org/SDL", sdl2_pin))
components.append(plan_tree("libtheora", deps / "theora", pins["theora"]["upstream"], pins["theora"]["sha256"]))
components.append(plan_tree("pixman", deps / "pixman", pins["pixman"]["upstream"], pins["pixman"]["sha256"]))
components.append(plan_tree("openal-soft", deps / ("openal-soft-openal-soft-" + pins["openal"]["version"]),
                            pins["openal"]["upstream"], pins["openal"]["sha256"]))

tsf = json.loads((scripts / "fetch-tinysoundfont.json").read_text())
tsf_dir = build / "third-party/tinysoundfont"
tsf_files = dict(tsf["files"])
tsf_files["LICENSE"] = sha256(tsf_dir / "LICENSE") if (tsf_dir / "LICENSE").is_file() else die("tinysoundfont: no LICENSE; run the build first")
components.append(plan_files("tinysoundfont", tsf_dir, tsf_files, "https://github.com/schellingb/TinySoundFont",
                             {"revision": tsf["commit"]}))

fonts = json.loads((root / "vita/mkxp-z-vpk/fonts/fonts.json").read_text())
components.append(plan_files("fonts", build / "fonts", {e["name"]: e["sha256"] for e in fonts["files"] + fonts["documents"]},
                             "https://vlgothic.dicey.org/ and https://github.com/liberationfonts/liberation-fonts"))


def add_member(tar, info, data, prefix):
    info.name = prefix + "/" + info.name
    info.mtime, info.uid, info.gid, info.uname, info.gname = stamp, 0, 0, "", ""
    info.mode = info.mode & 0o755 | 0o644
    if info.isreg():
        payload = data.read()
        scan(info.name, payload)
        data = io.BytesIO(payload)
    else:
        scan(info.name, b"")
        if info.issym() or info.islnk():
            scan(info.name, info.linkname.encode())
    tar.addfile(info, data)


def write_git(tar, record, directory, prefix, extras):
    commit = record["revision"]
    proc = subprocess.Popen(["git", "-C", str(directory), "archive", "--format=tar", commit], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=proc.stdout, mode="r|") as source:
        for info in source:
            if info.type == tarfile.XGLTYPE:
                continue
            add_member(tar, info, source.extractfile(info) if info.isreg() else None, prefix)
    if proc.wait():
        die("git archive failed in %s" % directory)
    for line in git(directory, "submodule", "status", "--recursive").splitlines():
        fields = line.lstrip(" +-U").split()
        sub = subprocess.Popen(["git", "-C", str(directory / fields[1]), "archive", "--format=tar", fields[0]],
                               stdout=subprocess.PIPE)
        with tarfile.open(fileobj=sub.stdout, mode="r|") as source:
            for info in source:
                if info.type == tarfile.XGLTYPE:
                    continue
                add_member(tar, info, source.extractfile(info) if info.isreg() else None,
                           prefix + "/" + fields[1])
        if sub.wait():
            die("git archive failed in %s" % (directory / fields[1]))
    for relative, path in extras:
        info = tarfile.TarInfo(relative)
        info.size, info.mode = path.stat().st_size, 0o755
        with open(path, "rb") as handle:
            add_member(tar, info, handle, prefix)


def write_tree(tar, record, directory):
    prefix = "%s/dependencies/%s" % (top, record["name"])
    count = 0
    for base, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for filename in sorted(files):
            path = Path(base, filename)
            if filename.endswith(SKIP_SUFFIXES) or filename == treedigest.SEAL or path.is_symlink() and not path.exists():
                continue
            info = tar.gettarinfo(str(path), arcname=prefix + "/" + path.relative_to(directory).as_posix())
            info.mtime, info.uid, info.gid, info.uname, info.gname = stamp, 0, 0, "", ""
            info.mode = info.mode & 0o755 | 0o644
            scan(info.name, path.read_bytes() if info.isreg() else b"")
            with open(path, "rb") as handle:
                tar.addfile(info, handle if info.isreg() else None)
            count += 1
    record["fileCount"] = count


def write_files(tar, record, folder, members):
    for filename, path in members:
        info = tarfile.TarInfo("%s/%s/%s" % (top, folder, filename))
        info.size, info.mtime, info.mode = path.stat().st_size, stamp, 0o644
        payload = path.read_bytes()
        scan(info.name, payload)
        tar.addfile(info, io.BytesIO(payload))


def record_text():
    lines = ["Corresponding source of this release. Each directory under dependencies/ is the source the build used;",
             "the changes this branch makes to them are in branch/vita/patches/ and applied by branch/vita/scripts/.", ""]
    for item in components:
        pin = item.get("revision") or item.get("sha256") or "pinned files"
        lines.append("%s  %s  %s" % (item["name"], pin, item["upstream"]))
    return ("\n".join(lines) + "\n").encode()


# --- Write: nothing above created any output; a failure below removes the partial file ---------------------------
out.parent.mkdir(parents=True, exist_ok=True)
part = out.with_name(out.name + ".part")
xz = shutil.which("xz")
sink = proc = tar = None
try:
    sink = open(part, "wb") if xz else None
    proc = subprocess.Popen([xz, "-T0", "-6", "-c"], stdin=subprocess.PIPE, stdout=sink) if xz else None
    tar = tarfile.open(fileobj=proc.stdin, mode="w|") if proc else tarfile.open(part, "w:xz", preset=6)
    for entry in plan:
        if entry[0] == "git":
            _, record, directory, folder, extras = entry
            write_git(tar, record, directory, "%s/%s" % (top, folder), extras)
        elif entry[0] == "tree":
            write_tree(tar, entry[1], entry[2])
        else:
            write_files(tar, *entry[1:])
    data = record_text()
    scan("SOURCES.txt", data)
    info = tarfile.TarInfo("%s/SOURCES.txt" % top)
    info.size, info.mtime, info.mode = len(data), stamp, 0o644
    tar.addfile(info, io.BytesIO(data))
    tar.close()
    if proc:
        proc.stdin.close()
        if proc.wait():
            die("xz failed")
        sink.close()
    os.replace(part, out)
except BaseException:
    if proc and proc.poll() is None:
        proc.kill()
    if proc and tar is not None:
        # The stream's finalizer would otherwise try to flush into the killed xz.
        tar.closed = tar.fileobj.closed = True
        try:
            proc.stdin.close()
        except OSError:
            pass
    if sink and not sink.closed:
        sink.close()
    part.unlink(missing_ok=True)
    raise

digest = hashlib.sha256()
with open(out, "rb") as handle:
    for block in iter(lambda: handle.read(1 << 20), b""):
        digest.update(block)
print(json.dumps({"file": out.name, "sha256": digest.hexdigest(), "size": out.stat().st_size, "components": components}))
