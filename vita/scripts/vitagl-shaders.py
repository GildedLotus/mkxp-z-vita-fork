#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Precompiled GXP shader delivery for the vitaGL backend.

  keys     print the boot programs and the vitaGL cache key of each stage
  check    fail unless a cache directory holds exactly the current boot set, every file is a
           well-formed program of its stage, and MANIFEST matches this tree's vitaGL inputs
  capture  copy the boot set out of a pulled device cache (or pull it by FTP), then write MANIFEST
  manifest re-attest MANIFEST for a valid set after a vitaGL change that cannot alter shader output

vitaGL names a cache file <root>/v<MAGIC>/<v|f>/<top byte, %02X>/<%llX>.gxp, where
the hash is XXH3_64 of the shader source exactly as glShaderSource received it. The
engine submits [probe] + "#define GLSLES" + ["#define FRAGMENT_SHADER"] + common.h +
body (src/display/gl/shader.cpp setupShaderSource), so the keys are computable from the
patched mkxp-z tree alone; editing any shader therefore makes the shipped set stale.
Only the boot set with optional shaders off is covered (what MKXPZ_OPTIONAL_SHADERS=0 builds),
plus the launcher's program (kVS/kFS in vita/launcher/launcher_gl.c), which is submitted as one bare string.
The boot-time final-presentation probe (app0:/diagnostics/final-presentation-probe) prepends a define to
SimpleShader's source, so its keys are not in the set: a package that carries the marker needs libshacccg.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

# Importing xxh3 must not leave a __pycache__ in the tree: a release refuses a dirty one.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from xxh3 import xxh3_64  # noqa: E402

SHADER_CACHE_MAGIC = 2  # vitaGL source/shared.h
GXP_MAGIC = b"GXP\x00"
# vitaGL's file layout (custom_shaders.c serialize_shader, bindings on): u32 matrix count, that many u32
# parameter indices, one binds_map (source/shared.h: 12 names x 64 bytes + 12 used flags), the SceGxmProgram.
BINDS_MAP_SIZE = 780
BINDS_FLAGS_AT = 768
GXP_HEADER_SIZE = 0x30
GXP_TYPE_AT = 0x14  # bit 0: 0 vertex, 1 fragment. Read off the 30 device-compiled programs (the SDK type is opaque)
GXP_PARAMS_AT = 0x24  # u32 parameter count; u32 table offset relative to 0x28 follows; 16-byte entries
GXP_MAX_BYTES = 64 * 1024
MANIFEST_NAME = "MANIFEST"
MANIFEST_PINS = ("vitagl", "vitashark", "shacccgext")
BLUR = ("BlurShader::HPass", "BlurShader::VPass")


class ShaderError(Exception):
    pass


def _text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise ShaderError("cannot read %s: %s" % (path, error))


def _define(cpp, name):
    m = re.search(r'static const char %s\[\] = "((?:[^"\\]|\\.)*)";' % name, cpp)
    if not m:
        raise ShaderError("shader.cpp: no %s define" % name)
    return m.group(1).encode().decode("unicode_escape").encode()


def boot_classes(header):
    """ShaderSet members before the first optional-shader block, in declaration order, without the
    GPU-bitmap-only block (#ifndef MKXPZ_SOFTWARE_BITMAPS), which the software-bitmap player never builds."""
    m = re.search(r"struct ShaderSet\s*\{(.*?)\n\};", header, re.S)
    if not m:
        raise ShaderError("shader.h: no ShaderSet")
    names, gpu_bitmaps_only = [], False
    for line in m.group(1).splitlines():
        if line.startswith("#ifndef MKXPZ_NO_OPTIONAL_SHADERS"):
            break
        if line.startswith("#ifndef MKXPZ_SOFTWARE_BITMAPS"):
            gpu_bitmaps_only = True
        elif gpu_bitmaps_only and line.startswith("#endif"):
            gpu_bitmaps_only = False
        member = re.match(r"\s+(\w+)\s+\w+;", line)
        if member and not gpu_bitmaps_only:
            names.extend(BLUR if member.group(1) == "BlurShader" else (member.group(1),))
    return names


def _c_string(text, name):
    m = re.search(r"\b%s\s*=\s*((?:\s*\"(?:[^\"\\\n]|\\.)*\")+)\s*;" % name, text)
    if not m:
        raise ShaderError("launcher_gl.c: no %s string" % name)
    literals = re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(1))
    return "".join(literals).encode().decode("unicode_escape").encode()


def launcher_program(src):
    text = _text(src / "vita/launcher/launcher_gl.c")
    return {"name": "Launcher", "vert": "kVS", "frag": "kFS",
            "v": xxh3_64(_c_string(text, "kVS")), "f": xxh3_64(_c_string(text, "kFS")), "bare": True}


def programs(source):
    src = Path(source)
    cpp, header = _text(src / "src/display/gl/shader.cpp"), _text(src / "src/display/gl/shader.h")
    gles, frag_define = _define(cpp, "glesDefine"), _define(cpp, "fragDefine")
    init = {m.group(3): (m.group(1), m.group(2))
            for m in re.finditer(r"^\tINIT_SHADER\((\w+), (\w+), ([\w:]+)\);", cpp, re.M)}
    common = (src / "shader/common.h").read_bytes()
    out = []
    for cls in boot_classes(header):
        if cls not in init:
            raise ShaderError("shader.cpp: no INIT_SHADER for %s" % cls)
        vert, frag = init[cls]
        stages = {}
        for kind, name, ext, prefix in (("v", vert, "vert", gles),
                                        ("f", frag, "frag", gles + frag_define)):
            body = (src / "shader" / ("%s.%s" % (name, ext))).read_bytes()
            stages[kind] = xxh3_64(prefix + common + body)
        out.append({"name": cls, "vert": vert, "frag": frag, "v": stages["v"], "f": stages["f"]})
    if not out:
        raise ShaderError("no boot programs found")
    out.append(launcher_program(src))
    return out


def stage_name(p, kind):
    return p["vert" if kind == "v" else "frag"] + ("" if p.get("bare") else ".vert" if kind == "v" else ".frag")


def cache_path(root, kind, key):
    return Path(root) / ("v%d" % SHADER_CACHE_MAGIC) / kind / ("%02X" % (key >> 56)) / ("%X.gxp" % key)


def expected(progs):
    return {(kind, p[kind]): "%s %s" % (p["name"], stage_name(p, kind)) for p in progs for kind in "vf"}


def scan(root):
    """Every file under root/v2/<v|f>/<XX>/ as (kind, key or bare name, path)."""
    found = []
    base = Path(root) / ("v%d" % SHADER_CACHE_MAGIC)
    for kind in "vf":
        for path in sorted((base / kind).glob("*/*")) if (base / kind).is_dir() else []:
            m = re.fullmatch(r"([0-9A-F]{1,16})\.gxp", path.name)
            found.append((kind, int(m.group(1), 16) if m else path.name, path))
    return found


def present(root):
    """{(kind, key): path}; a file at its canonical path wins over a misplaced copy of the same key."""
    found = {}
    for kind, key, path in scan(root):
        if (kind, key) not in found or path == cache_path(root, kind, key):
            found[(kind, key)] = path
    return found


def _u32(buf, at):
    return struct.unpack_from("<I", buf, at)[0]


def gxp_problem(data, kind):
    """Why vitaGL's unserialize_shader (or the stage it is filed under) would refuse this file, else None."""
    if len(data) > GXP_MAX_BYTES:
        return "%d bytes, over the %d byte cap" % (len(data), GXP_MAX_BYTES)
    if len(data) < 4 + BINDS_MAP_SIZE + GXP_HEADER_SIZE:
        return "%d bytes, too short to hold a program" % len(data)
    count = _u32(data, 0)
    start = 4 + 4 * count + BINDS_MAP_SIZE
    if count > (len(data) - 4) // 4 or start + GXP_HEADER_SIZE > len(data):
        return "matrix count %d leaves no room for a program in %d bytes" % (count, len(data))
    prog = data[start:]
    if prog[:4] != GXP_MAGIC:
        return "no GXP magic where vitaGL reads the program (offset %d)" % start
    size = _u32(prog, 8)
    if not GXP_HEADER_SIZE <= size <= len(prog) < size + 4:
        return "program size %d does not match the %d bytes present" % (size, len(prog))
    stage = "f" if prog[GXP_TYPE_AT] & 1 else "v"
    if stage != kind:
        names = {"v": "vertex", "f": "fragment"}
        return "a %s program filed as a %s stage" % (names[stage], names[kind])
    params, table = _u32(prog, GXP_PARAMS_AT), _u32(prog, GXP_PARAMS_AT + 4)
    if GXP_PARAMS_AT + 4 + table + 16 * params > size:
        return "parameter table (%d entries at +%d) lies outside the %d byte program" % (params, table, size)
    for at in range(count):
        if _u32(data, 4 + 4 * at) >= params:
            return "matrix uniform index %d is outside the program's %d parameters" % (_u32(data, 4 + 4 * at), params)
    binds = data[4 + 4 * count:start]
    if any(flag > 1 for flag in binds[BINDS_FLAGS_AT:]) or any(0 not in binds[at:at + 64] for at in range(0, BINDS_FLAGS_AT, 64)):
        return "binding table is not NUL-terminated names and 0/1 flags"
    return None


def problems(progs, root):
    want, out = expected(progs), []
    have = present(root)
    for kind, key, path in scan(root):
        if isinstance(key, int) and path != cache_path(root, kind, key):
            out.append("misplaced %s/%s: vitaGL reads %s" % (kind, path.name, cache_path(root, kind, key).relative_to(root)))
    for (kind, key), label in sorted(want.items(), key=lambda item: item[1]):
        path = have.get((kind, key))
        if path is None:
            out.append("missing %s/%X.gxp (%s)" % (kind, key, label))
            continue
        why = gxp_problem(path.read_bytes(), kind)
        if why:
            out.append("invalid %s/%X.gxp (%s): %s" % (kind, key, label, why))
    for (kind, key), path in sorted(have.items(), key=lambda item: str(item[1])):
        if (kind, key) not in want:
            out.append("stale %s/%s: no current boot shader hashes to it" % (kind, path.name))
    return out


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def manifest_inputs(repo):
    """What the shipped GXP depend on besides the GLSL text: the vitaGL build that translates and caches."""
    repo = Path(repo)
    try:
        pins = {p["name"]: p["commit"] for p in json.loads(_text(repo / "vita/scripts/vitagl-pins.json"))}
        lines = {"shader-cache-magic": str(SHADER_CACHE_MAGIC)}
        lines.update(("pin." + name, pins[name]) for name in MANIFEST_PINS)
    except (ValueError, KeyError, TypeError) as error:
        raise ShaderError("vita/scripts/vitagl-pins.json unusable: %s" % error)
    knobs = re.search(r'^VITAGL_KNOBS="([^"]*)"', _text(repo / "vita/scripts/build-vitagl.sh"), re.M)
    if not knobs:
        raise ShaderError("vita/scripts/build-vitagl.sh: no VITAGL_KNOBS line")
    lines["build-knobs"] = knobs.group(1)
    # SceShaccCgExt's pragma handlers run inside the shader compiler, so its patches and its replaced files are inputs too.
    for pattern in ("vitagl-*.patch", "shacccgext-*.patch"):
        for patch in sorted((repo / "vita/patches/vitagl").glob(pattern)):
            lines["patch." + patch.name] = _sha256(patch.read_bytes())
    overlay = repo / "vita/patches/vitagl/shacccgext-files"
    for path in sorted(p for p in overlay.rglob("*") if p.is_file()):
        lines["file.shacccgext/" + path.relative_to(overlay).as_posix()] = _sha256(path.read_bytes())
    return lines


def device_cache_dir(title, root):
    """The writable device cache a build stamped with root's MANIFEST uses (vita_glue.c shader_cache_root)."""
    try:
        return "%s-%s" % (title, _sha256((Path(root) / MANIFEST_NAME).read_bytes())[:16])
    except OSError as error:
        raise ShaderError("--ftp needs the stamped set's MANIFEST in --dir (%s); pull by hand and use --from-dir" % error)


def set_digest(root):
    digest = hashlib.sha256()
    base = Path(root) / ("v%d" % SHADER_CACHE_MAGIC)
    for path in sorted(base.rglob("*.gxp")) if base.is_dir() else []:
        digest.update(path.relative_to(base).as_posix().encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def write_manifest(repo, root):
    lines = dict(manifest_inputs(repo), **{"set-sha256": set_digest(root)})
    text = ("# Provenance of the shipped GXP set (vita/scripts/vitagl-shaders.py manifest): the compiler output was made by\n"
            "# the vitaGL build these inputs describe; `check` fails when this tree's inputs or the files differ.\n"
            + "".join("%s %s\n" % item for item in lines.items()))
    (Path(root) / MANIFEST_NAME).write_text(text, encoding="utf-8")


def manifest_problems(repo, root):
    path = Path(root) / MANIFEST_NAME
    if not path.is_file():
        return ["missing %s: capture the set, or attest it with `vitagl-shaders.py manifest`" % MANIFEST_NAME]
    recorded = dict(line.split(None, 1) for line in path.read_text(encoding="utf-8").splitlines()
                    if line.strip() and not line.startswith("#") and len(line.split(None, 1)) == 2)
    recorded = {key: value.strip() for key, value in recorded.items()}
    current = dict(manifest_inputs(repo), **{"set-sha256": set_digest(root)})
    out = []
    for key in sorted(set(recorded) | set(current)):
        if recorded.get(key) != current.get(key):
            out.append("%s: %s %s, this tree has %s" % (
                MANIFEST_NAME, key, "was captured as " + recorded[key] if key in recorded else "was not recorded",
                current.get(key, "nothing")))
    return out


def ftp_pull(hostport, remote_root, dest):
    from ftplib import FTP, error_perm
    host, _, port = hostport.partition(":")
    ftp = FTP()
    ftp.connect(host, int(port or 1337), timeout=15)
    ftp.login()
    got = 0
    for kind in "vf":
        for top in range(256):
            folder = "%s/v%d/%s/%02X" % (remote_root.rstrip("/"), SHADER_CACHE_MAGIC, kind, top)
            try:
                names = ftp.nlst(folder)
            except error_perm:
                continue
            for name in names:
                leaf = name.rsplit("/", 1)[-1]
                if not leaf.endswith(".gxp"):
                    continue
                target = Path(dest) / ("v%d" % SHADER_CACHE_MAGIC) / kind / ("%02X" % top) / leaf
                target.parent.mkdir(parents=True, exist_ok=True)
                with open(target, "wb") as handle:
                    ftp.retrbinary("RETR %s/%s" % (folder, leaf), handle.write)
                got += 1
    ftp.quit()
    return got


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("keys", "check", "capture", "manifest"):
        p = sub.add_parser(name)
        p.add_argument("--source", required=True, help="engine source tree (the repository root)")
        if name != "keys":
            p.add_argument("--dir", required=True, help="shipped cache root (holds v%d/)" % SHADER_CACHE_MAGIC)
            p.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]),
                           help="checkout whose pins and vita/patches/vitagl the MANIFEST records")
    cap = sub.choices["capture"]
    cap.add_argument("--from-dir", help="a device cache already pulled (holds v%d/)" % SHADER_CACHE_MAGIC)
    cap.add_argument("--ftp", help="host:port of the device FTP server")
    cap.add_argument("--title", help="test title id whose ux0:data/shader_cache/<id>-<set> is pulled (set from --dir's MANIFEST)")
    args = ap.parse_args(argv)
    pulled = None
    try:
        progs = programs(args.source)
        if args.cmd == "keys":
            for p in progs:
                print("%-24s %s %X  %s %X" % (p["name"], stage_name(p, "v"), p["v"], stage_name(p, "f"), p["f"]))
            print("%d programs, %d distinct stages" % (len(progs), len(expected(progs))))
            return 0
        if args.cmd == "capture":
            if bool(args.from_dir) == bool(args.ftp):
                raise ShaderError("capture needs exactly one of --from-dir or --ftp")
            src = args.from_dir
            if args.ftp:
                if not args.title or not re.fullmatch(r"MKXPZ00[0-9A-Z]{2}", args.title) or args.title == "MKXPZ0001":
                    raise ShaderError("--title must be a test title id (MKXPZ00xx, not the product id)")
                src = pulled = tempfile.mkdtemp(prefix="vitagl-pull-")
                remote = "ux0:/data/shader_cache/%s" % device_cache_dir(args.title, args.dir)
                print("pulled %d files" % ftp_pull(args.ftp, remote, src))
            if Path(src).resolve() == Path(args.dir).resolve():
                raise ShaderError("--dir must differ from the pulled cache")
            want, have = expected(progs), present(src)
            stray = [str(v.relative_to(src)) for k, v in have.items() if k not in want]
            miss = ["%s/%X.gxp (%s)" % (k[0], k[1], want[k]) for k in want if k not in have]
            if miss:
                raise ShaderError("capture is missing %d of %d stages:\n  %s\n(run every boot program on a device WITH "
                                  "libshacccg, from an empty cache)" % (len(miss), len(want), "\n  ".join(miss)))
            malformed = ["%s/%X.gxp (%s): %s" % (k[0], k[1], want[k], why) for k in want
                         for why in [gxp_problem(have[k].read_bytes(), k[0])] if why]
            if malformed:
                raise ShaderError("capture holds %d unusable stages:\n  %s" % (len(malformed), "\n  ".join(malformed)))
            shutil.rmtree(Path(args.dir) / ("v%d" % SHADER_CACHE_MAGIC), ignore_errors=True)
            for (kind, key) in want:
                dest = cache_path(args.dir, kind, key)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(have[(kind, key)], dest)
            print("captured %d stages into %s (%d unrelated files in the device cache ignored)"
                  % (len(want), args.dir, len(stray)))
            bad = problems(progs, args.dir)
            if bad:
                raise ShaderError("\n".join(bad))
            write_manifest(args.repo, args.dir)
            return 0
        if args.cmd == "manifest":
            bad = problems(progs, args.dir)
            if bad:
                raise ShaderError("refusing to attest an invalid set:\n  " + "\n  ".join(bad))
            write_manifest(args.repo, args.dir)
            print("wrote %s/%s for the current vitaGL pins and patch series" % (args.dir, MANIFEST_NAME))
            return 0
        bad = problems(progs, args.dir) + manifest_problems(args.repo, args.dir)
        if bad:
            raise ShaderError("%d problem(s) in %s; re-capture after any shader or vitaGL change (vita/scripts/"
                              "capture-vitagl-shaders.sh; `vitagl-shaders.py manifest` only attests a change that cannot "
                              "alter compiled shaders):\n  %s" % (len(bad), args.dir, "\n  ".join(bad)))
        print("ok: %d programs, %d stages match the current shader sources" % (len(progs), len(expected(progs))))
        return 0
    except ShaderError as error:
        print("vitagl-shaders: %s" % error, file=sys.stderr)
        return 1
    finally:
        if pulled:
            shutil.rmtree(pulled, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
