#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build-release.sh — stage the release under build/release/.
#
# Refuses a dirty tree. Re-executes itself under an environment allow-list
# (PATH HOME TMPDIR VITASDK JOBS BASERUBY LANG): nothing else the caller has
# exported reaches the build. Reads vita/VERSION, runs the vitaGL build
# (vita/scripts/build-player.sh with VITA_GL_BACKEND=vitagl and MKXPZ_RELEASE=1,
# the one path the packager lets pack MKXPZ0001, and only with the token this
# script issues; configure stamps the version into the boot log line and the
# launcher header, the packager stamps param.sfo APP_VER), verifies the product
# package, then stages in a fresh directory beside build/release/ and swaps it
# in only when everything, including the source archive, has passed:
#   mkxp-z.vpk             the product launcher VPK (MKXPZ0001, unpinned, vitaGL:
#                          shader_cache/ and licenses/, no module/ tree)
#   eboot.bin              the packed executable, byte-identical to the VPK's
#   manifest.json          SHA-256 of each artifact, git revision, dependency
#                          patch-series hash, version, TITLE_ID, backend,
#                          shader-stage count and shader MANIFEST hash
#   mkxp-z-source-<version>.tar.xz
#                          the corresponding source: this branch plus every
#                          pinned dependency source the build fetched
#                          (vita/scripts/source-archive.py); its digest and
#                          component list are recorded in manifest.json
#   RELEASE-NOTES.md       generated from the vita/COMPATIBILITY.md rows,
#                          < 120 lines
# The unstripped ELF (symbolisation copy for vita-parse-core) is never staged
# there: it is kept in build/release-private/ instead.
# Staging only: this script publishes nothing, pushes nothing, tags nothing and
# creates no branch. Licensing: GPL-3.0 combined work with GPL-3.0-or-later port
# files (LICENSE, THIRD-PARTY.md, licenses/).
set -euo pipefail
# pwd -P: compare like with like — git reports a physical toplevel, and /tmp is
# a symlink on some hosts.
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
PKG="$ROOT/build/mkxp-z-vpk-release"
RELEASE="$ROOT/build/release"
PRIVATE="$ROOT/build/release-private"

die() { echo "build-release: $*" >&2; exit 1; }

# --- Environment allow-list --------------------------------------------------
# The release is what a clean tree produces, never what the caller's last probe
# left behind. Every build driver, the packager and the tools they run read
# overrides from the environment (dependency prefixes, flags, package identity,
# waivers), so the list of names to strip would never be complete: start over
# from an allow-list instead. VITASDK, JOBS and BASERUBY select the toolchain,
# not the package identity.
if [[ ${1:-} != --clean-env ]]; then
  [[ $# == 0 ]] || die "usage: $0 (no arguments)"
  keep=()
  for name in PATH HOME TMPDIR VITASDK JOBS BASERUBY LANG; do
    [[ -z ${!name+x} ]] || keep+=("$name=${!name}")
  done
  dropped=$(env | cut -d= -f1 | /usr/bin/grep -E '^(ALLOW_|APP_VER$|ATTRIBUTE2$|BUILD|CC$|CXX$|CFLAGS$|CXXFLAGS$|CPPFLAGS$|LDFLAGS$|CONFIGURE_PREFIX$|DEPS_|EXTRA_|FONTS_|LIBERATION_|MKXP|MRI_|OUT_DIR$|PKG_CONFIG|REQUIRE_|RUBY_|SDL2_|TITLE|TSF_|VITA|VPK_)' | tr '\n' ' ' || true)
  [[ -z $dropped ]] || echo "build-release: ignoring inherited build overrides: ${dropped% }"
  exec env -i ${keep[@]+"${keep[@]}"} "$BASH" "$0" --clean-env
fi
shift

# --- Refusals, before anything is built or wiped ----------------------------
# rev-parse, not a -d .git test: a worktree's .git is a file.
git -C "$ROOT" rev-parse --show-toplevel >/dev/null 2>&1 ||
  die "$ROOT is not a git checkout"
[[ $(git -C "$ROOT" rev-parse --show-toplevel) == "$ROOT" ]] ||
  die "$ROOT is not the top of a git checkout"
[[ -z $(git -C "$ROOT" status --porcelain) ]] ||
  die "working tree is dirty; release builds run from a committed tree only"
[[ -f "$ROOT/vita/VERSION" ]] || die "missing vita/VERSION"
VERSION=$(tr -d ' \t\r\n' < "$ROOT/vita/VERSION")
[[ $VERSION =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] ||
  die "VERSION must be major.minor.patch digits, got: '$VERSION'"
APP_VER=$(printf '%02d.%02d' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}")

echo "==> release $VERSION (param.sfo APP_VER $APP_VER)"

VPK="$PKG/mkxp-z.vpk"
UNSTRIPPED="$PKG/mkxp-z.elf.unstripped"
EBOOT="$PKG/eboot.bin"

# Everything is staged in a fresh sibling of build/release/ and swapped in at the end; a failure anywhere leaves
# the previous build/release/ exactly as it was. The token lets the build drivers accept MKXPZ_RELEASE=1.
mkdir -p "$ROOT/build"
STAGE=$(mktemp -d "$ROOT/build/.release-staging.XXXXXX")
chmod 755 "$STAGE"
OLD=""
TOKEN_FILE="$ROOT/build/.release-token"
cleanup() {
  rm -f "$TOKEN_FILE"
  case "$STAGE" in "$ROOT"/build/.release-staging.*) rm -rf "$STAGE" ;; esac
  case "$OLD" in "$ROOT"/build/.release-staging.*) rm -rf "$OLD" ;; esac
}
trap cleanup EXIT
release_token=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
(umask 077; printf '%s\n' "$release_token" > "$TOKEN_FILE")

# Freshness witness: what each artifact's mtime was before the build, so the
# verifier can refuse one a failed or skipped packager left stale.
artifact_stamp() {
  python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_mtime_ns if os.path.exists(sys.argv[1]) else "absent")' "$1"
}
vpk_before=$(artifact_stamp "$VPK")
elf_before=$(artifact_stamp "$UNSTRIPPED")
eboot_before=$(artifact_stamp "$EBOOT")

# The release is vitaGL under the product id.
echo "==> vitaGL release build (vita/scripts/build-player.sh, allow-listed environment)"
MKXPZ_RELEASE_TOKEN="$release_token" VITA_GL_BACKEND=vitagl MKXPZ_RELEASE=1 \
  "$ROOT/vita/scripts/build-player.sh"

for artifact in "$VPK" "$UNSTRIPPED" "$EBOOT"; do
  [[ -f $artifact ]] || die "build did not produce $artifact"
done

# --- Verify the product package, then stage everything in one pass ----------
# Everything is written into $STAGE, never into build/release/: a refused release
# must not leave a half-staged build/release/ behind.
echo "==> verifying the product package"
git_revision=$(git -C "$ROOT" rev-parse HEAD)
python3 - "$ROOT" "$VPK" "$UNSTRIPPED" "$EBOOT" "$STAGE" "$VERSION" "$APP_VER" "$git_revision" \
    "$vpk_before" "$elf_before" "$eboot_before" <<'STAGE' || die "release verification failed; nothing staged"
import hashlib
import json
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
vpk_path, elf_path, eboot_path, release, version, app_ver, revision = sys.argv[2:9]
fresh_before = sys.argv[9:12]

# The product package layout, top level. Anything else in the VPK is a
# packaging override that leaked past the build and is refused below.
# The release ships no "module" tree (libshacccg.suprx is user-supplied on
# device) and carries shader_cache/ and licenses/ instead.
PRODUCT_TOPS = {"eboot.bin", "sce_sys", "shader_cache", "licenses", "fonts",
                "config", "preload", "ruby", "mkxp.json",
                "mkxp-vita-package.json", "alsoft.conf"}


def die(message):
    sys.exit("build-release: " + message)


def sfo_entries(blob):
    """Entries from a param.sfo: structural read, no SDK tool. String values
    (0x204) and integer values (0x0404, e.g. ATTRIBUTE2) are decoded."""
    try:
        magic, sfo_version, keys, values, count = struct.unpack_from("<4sIIII", blob)
        if magic != b"\0PSF" or sfo_version != 0x101 or not 20 + count * 16 <= keys <= values <= len(blob):
            die("bad param.sfo inside the VPK")
        found = {}
        for index in range(count):
            keyoff, fmt, length, capacity, valueoff = struct.unpack_from("<HHIII", blob, 20 + index * 16)
            end = blob.index(b"\0", keys + keyoff, values)
            name = blob[keys + keyoff:end].decode("ascii", "replace")
            if fmt == 0x204:
                found[name] = blob[values + valueoff:values + valueoff + length].rstrip(b"\0").decode("ascii", "replace")
            elif fmt == 0x0404:
                found[name] = struct.unpack_from("<i", blob, values + valueoff)[0]
    except (ValueError, struct.error):
        die("truncated or corrupt param.sfo inside the VPK")
    return found


with zipfile.ZipFile(vpk_path) as archive:
    names = set(archive.namelist())
    for required in ("eboot.bin", "sce_sys/param.sfo", "mkxp.json", "mkxp-vita-package.json"):
        if required not in names:
            die("VPK lacks %s; not a product package" % required)
    if any(name.startswith("module/") for name in names):
        die("VPK ships a module/ tree; the release carries no driver modules")
    unexpected = sorted({name.split("/", 1)[0] for name in names} - PRODUCT_TOPS)
    if unexpected:
        die("VPK has top-level entries a product never carries: %s" % ", ".join(unexpected))
    sfo = sfo_entries(archive.read("sce_sys/param.sfo"))
    if sfo.get("TITLE_ID") != "MKXPZ0001":
        die("VPK TITLE_ID is %r, product is MKXPZ0001" % sfo.get("TITLE_ID"))
    if sfo.get("APP_VER") != app_ver:
        die("VPK APP_VER is %r, expected %s from VERSION" % (sfo.get("APP_VER"), app_ver))
    if sfo.get("ATTRIBUTE2") != 12:
        die("VPK ATTRIBUTE2 is %r, product needs 12 (extended memory)" % sfo.get("ATTRIBUTE2"))
    config = json.loads(archive.read("mkxp.json"))
    if config.get("gameFolder") or config.get("customScript"):
        die("VPK pins a game (gameFolder/customScript); the product package is the launcher")
    identity = json.loads(archive.read("mkxp-vita-package.json"))
    if identity.get("executableKind") != "player":
        die("VPK executableKind is %r, not a player build" % identity.get("executableKind"))
    if identity.get("sourceDirty"):
        die("package manifest says the packing tree was dirty")
    if identity.get("sourceRevision") != revision:
        die("VPK was packed from %s, but this tree is %s"
            % (identity.get("sourceRevision"), revision))
    receipt = identity.get("build")
    if not isinstance(receipt, dict) or receipt.get("matchesPackagedTree") is not True:
        die("the packaged ELF's build receipt does not match this tree; rebuild")
    if receipt.get("backend") != "vitagl":
        die("VPK build backend is %r, the release is vitagl" % receipt.get("backend"))
    if receipt.get("optionalShaders") is not False:
        die("the ELF was built with optional shaders (%r); the shipped GXP set covers the boot programs only"
            % receipt.get("optionalShaders"))
    if receipt.get("mesonBuildtype") != "release" or receipt.get("lto") is not False:
        die("the ELF was built as buildtype %r, lto %r; the release is buildtype release without LTO"
            % (receipt.get("mesonBuildtype"), receipt.get("lto")))
    if not receipt.get("tinysoundfont") or sorted(receipt.get("dependencies") or {}) != ["sdl2Vitagl", "vitaDeps", "vitagl"]:
        die("the ELF's build receipt lacks its dependency or TinySoundFont digests")
    tracked_shaders = {path.relative_to(root / "vita/vitagl-shaders").as_posix(): path
                       for path in (root / "vita/vitagl-shaders").rglob("*.gxp")}
    packed_shaders = {name[len("shader_cache/"):] for name in names
                      if name.startswith("shader_cache/") and name.endswith(".gxp")}
    if not tracked_shaders or packed_shaders != set(tracked_shaders):
        die("VPK shader_cache differs from the tracked vita/vitagl-shaders set")
    for rel, path in tracked_shaders.items():
        if archive.read("shader_cache/" + rel) != path.read_bytes():
            die("VPK shader_cache/%s differs from the tracked copy" % rel)
    shader_stages = len(packed_shaders)
    licence_files = sorted(path.relative_to(root).as_posix() for path in
                           [root / "LICENSE", root / "THIRD-PARTY.md"]
                           + sorted((root / "licenses").rglob("*")) if path.is_file())
    for rel in licence_files:
        packed = "licenses/" + (rel.split("/", 1)[1] if rel.startswith("licenses/") else rel)
        if packed not in names or archive.read(packed) != (root / rel).read_bytes():
            die("VPK %s is missing or differs from the tree's %s" % (packed, rel))
    fonts = json.loads(archive.read("fonts/fonts.json"))
    absent = [entry["name"] for entry in fonts.get("files", [])
              if entry.get("packaged") and "fonts/" + entry["name"] not in names]
    if absent:
        die("VPK lacks packaged fonts named by fonts.json: %s" % ", ".join(absent))
    packed_eboot = archive.read("eboot.bin")

if packed_eboot != Path(eboot_path).read_bytes():
    die("staged eboot.bin differs from the VPK's eboot.bin")
# The boot-log literal is a compile-time concatenation, so its bytes exist in
# .rodata only if the ELF was built from this tree's VERSION.
if ("vita_glue: version " + version).encode() not in Path(elf_path).read_bytes():
    die("unstripped ELF does not carry version %s; rebuild from this tree" % version)
# A packager run that skipped or failed its vita-pack-vpk step must not leave
# a previous VPK here to be staged as this release: every artifact this run
# found before the build had to be rewritten by it.
for path, before in zip((vpk_path, elf_path, eboot_path), fresh_before):
    if before == "absent":
        continue
    if int(before) >= os.stat(path).st_mtime_ns:
        die("%s was not rewritten by this build; refusing a stale artifact"
            % Path(path).name)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# Dependency patch-series hash: sorted relative paths + bytes, the same
# construction the build receipt uses, over the vitaGL, SDL2 and Ruby series.
digest = hashlib.sha256()
series = sorted(p for p in (root / "vita/patches").rglob("*") if p.is_file())
if not series:
    die("no files under vita/patches; not a release tree")
for path in series:
    digest.update(str(path.relative_to(root)).encode() + b"\0" + path.read_bytes() + b"\0")

release = Path(release)
staged = {}
for source, name in ((vpk_path, "mkxp-z.vpk"), (eboot_path, "eboot.bin")):
    target = release / name
    target.write_bytes(Path(source).read_bytes())
    staged[name] = sha(target)
# The GPL and LGPL parts of the VPK come with their source: this branch and every
# pinned dependency source the build fetched (see THIRD-PARTY.md for the binary-only ones).
source_archive = json.loads(subprocess.run(
    [sys.executable, "-B", str(root / "vita/scripts/source-archive.py"), str(root),
     str(release / ("mkxp-z-source-%s.tar.xz" % version))],
    check=True, stdout=subprocess.PIPE, text=True).stdout)
# Every linked GPL/LGPL component of THIRD-PARTY.md must be in the archive that was just written.
subprocess.run([sys.executable, "-B", str(root / "vita/scripts/check-corresponding-source.py"), str(root),
                str(release / source_archive["file"])], check=True)
manifest = {
    "schemaVersion": 1,
    "version": version,
    "appVer": app_ver,
    "titleId": sfo["TITLE_ID"],
    "backend": "vitagl",
    "shaderStages": shader_stages,
    "shaderManifestSha256": sha(root / "vita/vitagl-shaders/MANIFEST"),
    "licenseFiles": len(licence_files),
    "gitRevision": revision,
    "dependencyPatchesSha256": digest.hexdigest(),
    "sourceArchive": source_archive,
    "artifactSha256": staged,
}
(release / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("==> staged %s" % release)
STAGE

# --- Release notes: generated, never hand-edited in build/ ------------------
echo "==> generating RELEASE-NOTES.md"
python3 - "$ROOT" "$STAGE/RELEASE-NOTES.md" "$VERSION" "$git_revision" <<'NOTES'
import re
import sys
from pathlib import Path

root, out, version, revision = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
compat = (root / "vita/COMPATIBILITY.md").read_text(encoding="utf-8")


def cells(row):
    return [cell.strip() for cell in row.strip().strip("|").split("|")]


def table_rows(section):
    body = section.split("\n\n", 1)[1] if "\n\n" in section else ""
    return [cells(line) for line in body.splitlines()
            if line.startswith("| ") and not set(line) <= set("|- ")]


lines = [
    "# mkxp-z for PS Vita %s — release notes (draft)" % version,
    "",
    "Generated by vita/scripts/build-release.sh from vita/COMPATIBILITY.md.",
    "Revision `%s`, TITLE_ID MKXPZ0001, vitaGL backend." % revision,
    "Statuses come from hardware runs only: an emulator or host pass never",
    "marks a row playable.",
    "",
    "## Game compatibility (vita/COMPATIBILITY.md)",
    "",
    "| Game | Engine | Version tested | Status |",
    "|---|---|---|---|",
]
for heading in ("### XP (RGSS1)", "### VX (RGSS2)", "### VX Ace (RGSS3)"):
    start = compat.index(heading)
    next_at = compat.find("\n### ", start + 1)
    section = compat[start:next_at if next_at > 0 else len(compat)]
    for row in table_rows(section):
        if len(row) >= 5 and row[0] != "Game":
            lines.append("| %s | %s | %s | %s |" % tuple(row[column] for column in (0, 1, 2, 4)))

tail = ["", "## Known engine limitations (leads; full list in vita/COMPATIBILITY.md)", ""]
known = compat.index("## Known engine limitations")
for row in table_rows(compat[known:compat.index("\n## ", known + 1)]):
    lead = re.findall(r"\*\*(.+?)\*\*", row[0])
    if lead:
        tail.append("- " + lead[0].rstrip("."))

tail += [
    "",
    "## Licensing",
    "",
    "The port's own files are GPL-3.0-or-later; the combined work is GPL-3.0",
    "(LICENSE). THIRD-PARTY.md lists every bundled component with its version,",
    "licence and upstream; the texts ship in the VPK under app0:/licenses/. The",
    "source of this release (this branch and every pinned dependency it builds) is",
    "the mkxp-z-source-%s.tar.xz beside the VPK. The shipped GXP shaders are" % version,
    "compiler output of this repo's GLSL. The VPK carries no driver modules and no",
    "libshacccg (supplied by the user on the device). Game RTPs are never bundled.",
    "",
]

lines = lines + tail
if len(lines) >= 120:
    sys.exit("build-release: RELEASE-NOTES.md draft exceeds 120 lines")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("==> %s (%d lines)" % (out, len(lines)))
NOTES

# --- Publish: swap the finished staging directory in --------------------------
# Nothing above touched build/release/. The previous one is renamed aside and put back if the swap fails, so
# it is never left half-replaced; stale files from an older release cannot survive the swap.
mkdir -p "$PRIVATE"
cp -f "$UNSTRIPPED" "$PRIVATE/mkxp-z-$VERSION.elf.unstripped"
if [[ -e $RELEASE ]]; then
  OLD="$STAGE.old"
  mv "$RELEASE" "$OLD"
fi
if ! mv "$STAGE" "$RELEASE"; then
  [[ -z $OLD ]] || mv "$OLD" "$RELEASE"
  die "could not move the staged release into place"
fi
STAGE=""

echo
echo "release $VERSION staged under build/release/ (symbolisation ELF: build/release-private/)"
ls -l "$RELEASE"
