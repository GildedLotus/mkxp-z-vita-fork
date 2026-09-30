#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the corresponding-source archive of a release: this branch plus every dependency source the build used.

usage: source-archive.py ROOT OUT.tar.xz

Git-based dependencies are archived as pristine trees at the commit the build verified (the patches that change them
are in this branch's vita/patches/). Dependencies that come as release tarballs are archived as the sealed tree the
build extracted, already patched where the build patches it; the seal (vita/scripts/treedigest.py) names the input it
was made from (URL, version, tarball digest, digest of every patch and transformation) and proves nobody edited it
or built inside it. The statically linked VitaSDK (vdpm) libraries are archived as the upstream tarballs, patches
and commits their packages were built from, with the vitasdk/packages recipes at the pinned commit; the installed
libraries are checked against the digests pinned in dep-pins.json, and the toolchain revisions of newlib and
pthread-embedded against $VITASDK/version_info.txt. Every input is checked before the output file is opened, every
member is scanned for the builder's home directory, hostname, VitaSDK path and checkout path, and the archive
appears under OUT only when complete: any failure removes the partial file.
Prints one JSON record (file, sha256, size, components, toolchain) on stdout. Members are sorted and carry the
branch's commit time, so the same inputs give the same archive.
"""
import hashlib
import io
import json
import os
import re
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
GENERIC_HOSTNAMES = {"localhost", "localhost.localdomain", "localdomain", "ip6-localhost"}
SKIP_SUFFIXES = (".o", ".a", ".lo", ".la", ".obj", ".so", ".dylib", ".pyc")


def die(message):
    sys.exit("source-archive: " + message)


def git(directory, *args):
    return subprocess.run(["git", "-C", str(directory), *args], check=True, capture_output=True, text=True).stdout.strip()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def vitasdk():
    """The VitaSDK the build used: $VITASDK, else the locations vita-env.sh looks in."""
    for candidate in (os.environ.get("VITASDK"), os.path.expanduser("~/vitasdk"), "/usr/local/vitasdk"):
        if candidate and os.path.isfile(os.path.join(candidate, "bin/arm-vita-eabi-gcc")):
            return candidate
    return os.environ.get("VITASDK", "")


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
    sdk = vitasdk()
    add("the VitaSDK path", sdk)
    add("the VitaSDK path", os.path.realpath(sdk) if sdk else "")
    host = socket.gethostname()
    for value in (host, host.split(".")[0]):
        if value in GENERIC_HOSTNAMES:
            print("source-archive: hostname %r is generic and is not scanned for" % value, file=sys.stderr)
        elif len(value) >= 5:
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
dist = build / "vita-deps/dist"
pins = json.loads((scripts / "dep-pins.json").read_text())
plan = []  # (kind, name, args...) in archive order; built before anything is written
components = []


def plan_git(name, directory, commit, upstream, folder=None, extras=(), clean=False, pinned=False):
    directory = Path(directory)
    if not (directory / ".git").exists():
        die("%s: no git source at %s; run the build first" % (name, directory))
    if subprocess.run(["git", "-C", str(directory), "cat-file", "-e", commit + "^{commit}"]).returncode:
        die("%s: commit %s is not in %s" % (name, commit, directory))
    if pinned and git(directory, "rev-parse", "HEAD") != commit:
        die("%s: the checkout is not at the pinned commit" % name)
    if clean and git(directory, "status", "--porcelain", "--untracked-files=no"):
        die("%s: the checkout has tracked edits; refusing to archive it as commit %s" % (name, commit))
    record = {"name": name, "kind": "git", "revision": commit, "upstream": upstream}
    plan.append(("git", record, directory, folder or "dependencies/" + name, list(extras)))
    return record


def plan_tree(name, identity, directory, upstream):
    """A sealed tarball tree. It must be sealed from exactly the current pins and patches (treedigest.py), and the
    record carries that input identity and the tree's own digest."""
    directory = Path(directory)
    if not directory.is_dir():
        die("%s: no source tree at %s; run the build first" % (name, directory))
    treedigest.verify_source_tree(directory, identity)
    inputs, tree = treedigest.read_seal(directory)
    record = {"name": name, "kind": "tree", "upstream": upstream, "input": inputs, "treeSha256": tree}
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


def toolchain_record():
    """Check the VitaSDK on this machine against the pins, and return what the release records about it.

    The toolchain revisions the VitaSDK records for newlib and pthread-embedded must be the pinned ones, and the
    statically linked vdpm libraries must be byte-for-byte the pinned packages' files: only then are the sources
    archived below the sources of what the executable links."""
    sdk = Path(vitasdk())
    if not (sdk / "bin/arm-vita-eabi-gcc").is_file():
        die("no VitaSDK found (set VITASDK)")
    info_file = sdk / pins["toolchain"]["versionInfoFile"]
    if not info_file.is_file():
        die("%s is missing: cannot bind newlib and pthread-embedded to a revision" % info_file)
    info = {}
    for line in info_file.read_text().splitlines():
        fields = line.split()
        if len(fields) == 2 and re.fullmatch(r"[0-9a-f]{40}", fields[1]):
            info[fields[0]] = fields[1]
    want = {"newlib": pins["toolchain"]["newlib"]["commit"], "pthread-embedded": pins["pthreadEmbedded"]["commit"]}
    for name, commit in want.items():
        if info.get(name) != commit:
            die("the VitaSDK records %s at %s, the pin is %s: the linked library is not the pinned source"
                % (name, info.get(name), commit))
    installed = sdk / "var/lib/pacman/local"
    for package, entry in pins["vdpm"]["packages"].items():
        if not (installed / ("%s-%s" % (package, entry["version"])) / "desc").is_file():
            die("vdpm package %s %s is not the installed version (pinned channel %s)"
                % (package, entry["version"], pins["vdpm"]["channel"]))
        for lib, digest in entry["libs"].items():
            path = sdk / "arm-vita-eabi/lib" / lib
            if not path.is_file() or sha256(path) != digest:
                die("%s does not match the pinned %s %s package" % (path, package, entry["version"]))
        cached = sdk / "var/cache/pacman/pkg" / ("%s-%s-vita.pkg.tar.xz" % (package, entry["version"]))
        if cached.is_file() and sha256(cached) != entry["packageSha256"]:
            die("the cached package %s does not match its pinned digest" % cached.name)
    return {"versionInfo": {name: info[name] for name in sorted(info)}, "newlib": pins["toolchain"]["newlib"],
            "pthreadEmbedded": pins["pthreadEmbedded"], "vdpmChannel": pins["vdpm"]["channel"],
            "vdpmRecipes": pins["vdpm"]["recipes"]}


def check_recipe(package, entry, recipes):
    """The pinned recipe is the one the installed package was built from (its VITABUILD digest is recorded in the
    package's own .BUILDINFO), names this version, and names each pinned source by digest."""
    commit = pins["vdpm"]["recipes"]["commit"]
    blob = subprocess.run(["git", "-C", str(recipes), "show", "%s:%s/VITABUILD" % (commit, package)],
                          check=True, capture_output=True).stdout
    if hashlib.sha256(blob).hexdigest() != entry["vitabuildSha256"]:
        die("%s: the VITABUILD at the pinned recipes commit is not the one the package was built from" % package)
    text = blob.decode()
    ver = re.search(r"^pkgver=['\"]?([^'\"\s]+)", text, re.M)
    rel = re.search(r"^pkgrel=['\"]?([^'\"\s]+)", text, re.M)
    if not ver or not rel or "%s-%s" % (ver.group(1), rel.group(1)) != entry["version"]:
        die("%s: the recipe does not build version %s" % (package, entry["version"]))
    for source in entry["sources"]:
        if source.get("recipePins", True) and source["sha256"] not in text:
            die("%s: the recipe does not name %s by the pinned digest" % (package, source["file"]))


# The branch itself: a tree with edits is not what the release describes.
if git(root, "status", "--porcelain"):
    die("the working tree is dirty; the archive is made from a committed tree")
toolchain = toolchain_record()
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
components.append(plan_git("uchardet", deps / "uchardet", pins["uchardet"]["commit"], pins["uchardet"]["repo"],
                           clean=True, pinned=True))
components.append(plan_git("pthread-embedded", deps / "pthread-embedded", pins["pthreadEmbedded"]["commit"],
                           pins["pthreadEmbedded"]["repo"], clean=True, pinned=True))

components.append(plan_tree("sdl2", "sdl2", build / "sdl2-vitagl-src", "https://github.com/libsdl-org/SDL"))
if pins["vdpm"]["packages"]["sdl2"]["sources"][0]["sha256"] != components[-1]["input"]["tarballSha256"]:
    die("the vdpm sdl2 source pin is not the SDL2 tarball this build patches (dep-pins.json vs sdl2-pin.sh)")
components.append(plan_tree("libtheora", "theora", deps / "theora", pins["theora"]["upstream"]))
components.append(plan_tree("pixman", "pixman", deps / "pixman", pins["pixman"]["upstream"]))
components.append(plan_tree("openal-soft", "openal", deps / ("openal-soft-openal-soft-" + pins["openal"]["version"]),
                            pins["openal"]["upstream"]))

tsf = json.loads((scripts / "fetch-tinysoundfont.json").read_text())
tsf_dir = build / "third-party/tinysoundfont"
tsf_files = dict(tsf["files"])
tsf_files["LICENSE"] = sha256(tsf_dir / "LICENSE") if (tsf_dir / "LICENSE").is_file() else die("tinysoundfont: no LICENSE; run the build first")
components.append(plan_files("tinysoundfont", tsf_dir, tsf_files, "https://github.com/schellingb/TinySoundFont",
                             {"revision": tsf["commit"]}))

fonts = json.loads((root / "vita/mkxp-z-vpk/fonts/fonts.json").read_text())
components.append(plan_files("fonts", build / "fonts", {e["name"]: e["sha256"] for e in fonts["files"] + fonts["documents"]},
                             "https://vlgothic.dicey.org/ and https://github.com/liberationfonts/liberation-fonts"))

# The statically linked vdpm libraries: the recipes at the pinned commit, then each package's sources.
recipes = deps / "vitasdk-packages"
vdpm = pins["vdpm"]
components.append(plan_git("vitasdk-packages", recipes, vdpm["recipes"]["commit"], vdpm["recipes"]["repo"],
                           clean=True, pinned=True))
for package, entry in vdpm["packages"].items():
    check_recipe(package, entry, recipes)
    record = {"version": entry["version"], "vitabuildSha256": entry["vitabuildSha256"],
              "packageSha256": entry["packageSha256"], "libs": entry["libs"]}
    # The SDL2 of the sysroot package is built from the pristine tarball; the tree "sdl2" above is the patched one.
    name = "sdl2-vdpm" if package == "sdl2" else package
    if "git" in entry:
        git_pin = entry["git"]
        components.append(plan_git(name, deps / package, git_pin["commit"], git_pin["repo"], clean=True, pinned=True))
        components[-1]["package"] = record
    if entry["sources"]:
        components.append(plan_files(name, dist / "vdpm" / package, {s["file"]: s["sha256"] for s in entry["sources"]},
                                     ", ".join(s["url"] for s in entry["sources"]), {"package": record}))


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
    """Every member of the sealed tree, symlinks (to directories too) as symlinks; the seal's walk already refused
    a target that is absolute or leaves the tree, and each target is scanned like a name."""
    prefix = "%s/dependencies/%s" % (top, record["name"])
    count = 0
    for rel, path, kind in treedigest.walk_tree(directory):
        if rel == treedigest.SEAL or SKIP_DIRS & set(rel.split("/")) or kind == "file" and rel.endswith(SKIP_SUFFIXES):
            continue
        info = tar.gettarinfo(str(path), arcname=prefix + "/" + rel)
        info.mtime, info.uid, info.gid, info.uname, info.gname = stamp, 0, 0, "", ""
        if kind == "link":
            scan(info.name, info.linkname.encode())
            tar.addfile(info)
        else:
            info.mode = info.mode & 0o755 | 0o644
            payload = path.read_bytes()
            scan(info.name, payload)
            tar.addfile(info, io.BytesIO(payload))
        count += 1
    record["fileCount"] = count


def write_files(tar, record, folder, members):
    for filename, path in members:
        info = tarfile.TarInfo("%s/%s/%s" % (top, folder, filename))
        info.size, info.mtime, info.mode = path.stat().st_size, stamp, 0o644
        payload = path.read_bytes()
        scan(info.name, payload)
        tar.addfile(info, io.BytesIO(payload))


def label(item):
    if item["kind"] == "git":
        return "commit " + item["revision"]
    if item["kind"] == "tree":
        source = item["input"]
        patches = source.get("patchSha256")
        changes = []
        if isinstance(patches, dict):
            changes += ["patch " + name for name in sorted(patches)]
        elif patches:
            changes.append("patch " + source["patchUrl"].rsplit("/", 1)[1])
        changes += ["transformation " + name for name in sorted(source.get("transformSha256", {}))]
        what = "tarball sha256 %s" % source["tarballSha256"]
        if changes:
            what = "patched tree of %s (already applied: %s)" % (what, ", ".join(changes))
        return "%s; tree sha256 %s" % (what, item["treeSha256"])
    return "files, each pinned by sha256: " + ", ".join(sorted(item["files"]))


def record_text():
    lines = ["Corresponding source of this release. Each directory under dependencies/ is the source the build used or,",
             "for the vdpm libraries, the upstream source their packages were built from (recipes: vitasdk-packages).",
             "Patched trees (below) are archived with their patches already applied; branch/vita/patches/ and",
             "branch/vita/scripts/ hold the patches and the steps that produced them: do not apply them a second time.",
             "Other changes this branch makes to a dependency are in branch/vita/patches/, applied by branch/vita/scripts/.", ""]
    for item in components:
        lines.append("%s  %s  %s" % (item["name"], label(item), item["upstream"]))
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
print(json.dumps({"file": out.name, "sha256": digest.hexdigest(), "size": out.stat().st_size, "components": components,
                  "toolchain": toolchain}))
