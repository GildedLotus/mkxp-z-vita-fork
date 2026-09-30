#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# package-vpk.sh — package the mkxp-z product VPK.
#
# Pipeline:
#   arm-vita-eabi-gcc → vita-elf-create → vita-make-fself →
#   vita-mksfoex -s TITLE_ID=MKXPZ0001 -d ATTRIBUTE2=12 → vita-pack-vpk
#
# eboot source priority:
#   1. $MKXPZ_ELF                          (explicit override)
#   2. $BUILD_DIR/mkxp-z                   (meson output)
#   3. $BUILD_DIR/src/mkxp-z               (alternate meson layout)
#   4. fail
#
# VPK contents:
#   eboot.bin
#   sce_sys/param.sfo          generated (TITLE_ID, ATTRIBUTE2=12)
#   sce_sys/icon0.png, livearea/contents/   tracked LiveArea art (vita/mkxp-z-vpk/sce_sys/)
#   fonts/                     fallback TTFs from build/fonts (vita/scripts/fetch-fonts.sh)
#                              plus the tracked README.md and fonts.json
#   config/default.json        placeholder (vita/mkxp-z-vpk/config/)
#   preload/                   Ruby preload scripts (vita/mkxp-z-vpk/preload/);
#                              device baseline and optional game composition
#   ruby/                      installed Ruby 3.1 extension wrappers
#   shader_cache/, licenses/   precompiled GXP cache / release only (see MKXPZ_RELEASE below)
#   alsoft.conf                OpenAL-soft device configuration;
#                              must sit at the root, that is the only path
#                              openal-soft looks at inside the package
#   mkxp-vita-package.json     player identity, source provenance and
#                              artifactSha256 (eboot/ELF digests)
#
# Lessons reused from earlier GLES VPK packaging:
#   - source vita/scripts/vita-env.sh for VITASDK resolution
#   - -ffunction-sections -fdata-sections + -Wl,--gc-sections
#   - vita-make-fself without -s (unsafe/self) so ux0:data writes work
#   - vita-make-fself -na: disable ASLR. With ASLR the main RX slid off
#     0x81000000 and the loader then placed RW at ALIGN_1MB(RX_end) instead
#     of the linked address, so absolute .bss symbols (_newlib_heap_*)
#     pointed into unmapped space. -na keeps RX at the preferred address; the
#     custom linker script (vita/linker/armvita-1mb-data.ld) places RW at ALIGN_1MB(RX_end) so
#     either loader policy matches.
#
# Host-only. Does not deploy or boot: copy the VPK to the device and install it
# with VitaShell.
# MKXP_JSON optionally selects a staged root config without editing assets.
#
# Environment overrides (defaults below package the PRODUCT, which is the
# LAUNCHER — an unpinned root config under MKXPZ0001):
#   MKXPZ_ELF, BUILD_DIR, RUBY_PREFIX, TITLE_ID, TITLE, ATTRIBUTE2
#   VITA_GL_BACKEND accepts only vitagl (the default). A non-release package
#                   defaults OUT_DIR to build/mkxp-z-vpk-vitagl and refuses the
#                   product TITLE_ID
#   MKXPZ_RELEASE   1 = the release package (set by
#                   vita/scripts/build-release.sh only): the one way a package
#                   may carry the product TITLE_ID MKXPZ0001. Requires
#                   the tracked shaders, the licence texts (packed as
#                   app0:/licenses) and none of the ALLOW_* escape hatches;
#                   defaults OUT_DIR to build/mkxp-z-vpk-release.
#   VITAGL_SHADERS_DIR  captured GXP cache root (holds v2/), default
#                   vita/vitagl-shaders (tracked); shipped as app0:/shader_cache and checked
#                   against this tree's shader sources (vita/scripts/vitagl-shaders.py).
#                   The set's MANIFEST must match this tree's vitaGL pins and patch series.
#                   ALLOW_MISSING_SHADERS=1 packs whatever is there, with a warning.
#   APP_VER         param.sfo APP_VER string; default derives from the repo
#                   vita/VERSION file ("1.0.0" -> "01.00"); left unset when there
#                   is no readable VERSION
#   MKXP_JSON       root mkxp.json to pack (default vita/mkxp-z-vpk/mkxp.json)
#   OUT_DIR         output directory   (default build/mkxp-z-vpk-vitagl)
#   VPK_NAME        bare VPK file name (default mkxp-z.vpk)
#   EXTRA_VPK_ADD   newline-separated "src=dest" pairs appended to
#                   vita-pack-vpk -a, for packaging variants that add trees.
#                   Sources must exist; dest is VPK-relative.
#   FONTS_PREFIX    fetched fallback fonts (default build/fonts)
#   ALLOW_MISSING_FONTS=1
#                   pack whatever fonts are there instead of failing. For CI
#                   and packaging probes only -- a product VPK built this way
#                   renders Japanese text as tofu and gives the launcher no
#                   face to draw Shift_JIS titles with.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ASSETS="$ROOT/vita/mkxp-z-vpk"
# GL backend: vitaGL is the only backend and needs no driver
# modules (libshacccg.suprx stays user-supplied on device). A non-release
# package is a test build only — never the product TITLE_ID.
VITA_GL_BACKEND="${VITA_GL_BACKEND:-vitagl}"
[[ "$VITA_GL_BACKEND" == vitagl ]] || { echo "package-vpk: VITA_GL_BACKEND must be vitagl" >&2; exit 1; }
MKXPZ_RELEASE="${MKXPZ_RELEASE:-0}"
case "$MKXPZ_RELEASE" in
	0|1) ;;
	*) echo "package-vpk: MKXPZ_RELEASE must be 0 or 1" >&2; exit 1 ;;
esac
if [[ -z ${OUT_DIR:-} ]]; then
	OUT="$ROOT/build/mkxp-z-vpk-vitagl"
	[[ $MKXPZ_RELEASE -eq 0 ]] || OUT="$ROOT/build/mkxp-z-vpk-release"
else
	OUT="$OUT_DIR"
fi
VPK_NAME="${VPK_NAME:-mkxp-z.vpk}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/mkxp-z-vitagl}"
RUBY_PREFIX="${RUBY_PREFIX:-$ROOT/build/ruby-vita-prefix}"
RUBY_LIB="$RUBY_PREFIX/lib/ruby/3.1.0"
MKXP_JSON="${MKXP_JSON:-$ASSETS/mkxp.json}"
FONTS_PREFIX="${FONTS_PREFIX:-$ROOT/build/fonts}"
# The dependency prefixes the ELF was linked against (same defaults as configure-vita.sh): the receipt's
# archive digests are compared with what these hold now.
DEPS_PREFIX="${DEPS_PREFIX:-$ROOT/build/vita-deps/prefix}"
SDL2_VITAGL_PREFIX="${SDL2_VITAGL_PREFIX:-$ROOT/build/sdl2-vitagl-prefix}"
VITAGL_PREFIX="${VITAGL_PREFIX:-$ROOT/build/vitagl-prefix}"
if [[ -z ${TSF_PREFIX+x} ]]; then TSF_PREFIX="$ROOT/build/third-party/tinysoundfont"; fi

TITLE_ID="${TITLE_ID:-MKXPZ0001}"
TITLE="${TITLE:-mkxp-z}"
ATTRIBUTE2="${ATTRIBUTE2:-12}"
# APP_VER: vita/VERSION stamps the SFO's dotted release string, zero-padded to
# the Vita's two-digit fields. An explicit $APP_VER wins; a missing or
# malformed VERSION packs without an APP_VER entry. build-release.sh is the
# strict gate; this stays lenient so test packages cannot fail on it.
if [[ -z ${APP_VER:-} && -f "$ROOT/vita/VERSION" ]]; then
	derived=$(tr -d ' \t\r\n' < "$ROOT/vita/VERSION")
	if [[ $derived =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
		APP_VER=$(printf '%02d.%02d' "$((10#${BASH_REMATCH[1]}))" "$((10#${BASH_REMATCH[2]}))")
	fi
fi

die() { echo "package-vpk: $*" >&2; exit 1; }

if [[ $VPK_NAME == */* || -z $VPK_NAME ]]; then
	die "VPK_NAME must be a bare file name, got: $VPK_NAME"
fi

# ---------------------------------------------------------------------------
# Resolve eboot source
# ---------------------------------------------------------------------------
ELF_IN=""
if [[ -n ${MKXPZ_ELF:-} ]]; then
	[[ -f $MKXPZ_ELF ]] || die "MKXPZ_ELF=$MKXPZ_ELF does not exist"
	ELF_IN="$MKXPZ_ELF"
elif [[ -x "$BUILD_DIR/mkxp-z" ]]; then
	ELF_IN="$BUILD_DIR/mkxp-z"
elif [[ -x "$BUILD_DIR/mkxp-z.cortex-a9" ]]; then
	# meson names the exe '<project>.<cpu>' when host_system=linux and
	# appimage=false (vita/meson/vita-cross.ini). Without this branch the
	# packager silently fell back to the stub.
	ELF_IN="$BUILD_DIR/mkxp-z.cortex-a9"
elif [[ -x "$BUILD_DIR/src/mkxp-z" ]]; then
	ELF_IN="$BUILD_DIR/src/mkxp-z"
else
	die "real player ELF missing; build mkxp-z first or set MKXPZ_ELF"
fi

# Same rule for every non-release package: the product bubble must never
# receive one. No escape flag: the id is the caller's one-line fix.
if [[ $TITLE_ID == "MKXPZ0001" && $MKXPZ_RELEASE -eq 0 ]]; then
	die "a package without MKXPZ_RELEASE=1 is a test build only: set TITLE_ID=MKXPZ00xx (MKXPZ0001 is the product id; vita/scripts/build-release.sh is the release path)"
fi

# The release package: the product id, complete and unwaived, from a clean tree, started by build-release.sh.
# Every ALLOW_* switch weakens a check the release depends on.
if [[ $MKXPZ_RELEASE -eq 1 ]]; then
	# shellcheck source=/dev/null
	. "$ROOT/vita/scripts/release-gate.sh"
	release_gate
	[[ $TITLE_ID == "MKXPZ0001" ]] || die "MKXPZ_RELEASE=1 is the product package: TITLE_ID must be MKXPZ0001, got $TITLE_ID"
	for escape in ALLOW_MISSING_SHADERS ALLOW_MISSING_FONTS ALLOW_STALE_ELF ALLOW_PINNED_PRODUCT_ID; do
		[[ ${!escape:-0} != 1 ]] || die "MKXPZ_RELEASE=1 refuses $escape=1"
	done
	[[ -z ${EXTRA_VPK_ADD:-} ]] || die "MKXPZ_RELEASE=1 packs no extra trees (EXTRA_VPK_ADD is set)"
	for licence_file in LICENSE THIRD-PARTY.md; do
		[[ -f "$ROOT/$licence_file" ]] || die "release needs $ROOT/$licence_file"
	done
	[[ -d "$ROOT/licenses" ]] || die "release needs the licence texts in $ROOT/licenses"
fi

[[ -d "$RUBY_LIB" ]] || die "Ruby standard library missing: $RUBY_LIB; run vita/scripts/build-ruby-vita.sh first"
for ruby_wrapper in date.rb digest.rb digest/loader.rb objspace.rb; do
	[[ -f "$RUBY_LIB/$ruby_wrapper" ]] || die "Ruby install incomplete: missing $RUBY_LIB/$ruby_wrapper; rebuild Ruby"
done
[[ -f "$MKXP_JSON" ]] || die "missing root configuration: $MKXP_JSON"

# Check inputs before invoking any SDK tools or removing a
# previous package. Missing player/Ruby artifacts must be an actionable failure.
# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?VITASDK not found}"
export PATH="$VITASDK/bin:$PATH"
need() {
	command -v "$1" >/dev/null || die "missing toolchain binary: $1 (VITASDK=$VITASDK)"
}
need arm-vita-eabi-gcc
need arm-vita-eabi-strip
need vita-elf-create
need vita-make-fself
need vita-mksfoex
need vita-pack-vpk
need python3
[[ -d $ASSETS ]] || die "missing assets dir: $ASSETS"
[[ -f "$ASSETS/config/default.json" ]] || die "missing $ASSETS/config/default.json"
[[ -d "$ASSETS/fonts" ]] || die "missing $ASSETS/fonts"
[[ -f "$ASSETS/fonts/fonts.json" ]] || die "missing $ASSETS/fonts/fonts.json"
[[ -d "$ASSETS/preload" ]] || die "missing $ASSETS/preload"
[[ -f "$ASSETS/alsoft.conf" ]] || die "missing $ASSETS/alsoft.conf"
# The player names its own mandatory preloads as absolute app0: literals
# (binding/filesystem-binding.cpp fileIntBindingInit), and loads them with
# rb_load during binding init, before any protect tag exists: a missing one is
# a LoadError that longjmps with nowhere to land and data-aborts the rgss
# thread with nothing in the log. Packaging a payload from a newer tree than a
# prebuilt --elf is exactly how that happens (a payload shipping
# settings_file.rb next to an ELF that still asked for save_guard.rb). Gate
# the pair.
python3 - "$ELF_IN" "$ASSETS/preload" <<'ELFGATE' || die "player ELF and preload payload disagree; rebuild the ELF from this tree or package the matching preloads"
import re
import sys
from pathlib import Path

elf, preload = Path(sys.argv[1]), Path(sys.argv[2])
wanted = sorted({name.decode("ascii") for name in re.findall(
    rb"app0:/preload/([A-Za-z0-9_.-]{1,64}\.rb)", elf.read_bytes())})
if not wanted:
    sys.exit("%s names no app0:/preload/*.rb: not a player ELF from this port" % elf)
missing = [name for name in wanted if not (preload / name).is_file()]
if missing:
    shipped = sorted(path.name for path in preload.glob("*.rb"))
    sys.exit("player ELF loads %s, absent from %s (which ships %s)"
             % (", ".join(missing), preload, ", ".join(shipped) or "no .rb file"))
print("==> preload gate: ELF requires %s; all present" % ", ".join(wanted))
ELFGATE
# Build receipt: configure-vita.sh links its inputs into the
# ELF as section .mkxpz.build_receipt. The dependency patches, the engine and
# vita/ sources and the Ruby archive it names must be the ones in this tree, or the package would pair this tree's
# provenance and Ruby payload with an executable built from something else.
# ALLOW_STALE_ELF=1 packs anyway and records the mismatch; REQUIRE_BUILD_RECEIPT=1
# (vita/scripts/build-player.sh) refuses an ELF without a receipt.
mkdir -p "$OUT"
BUILD_RECEIPT="$OUT/mkxpz-build-receipt.json"
rm -f "$BUILD_RECEIPT"
python3 - "$ELF_IN" "$ROOT" "$RUBY_PREFIX/lib/libruby-static.a" "$BUILD_RECEIPT" \
	"${ALLOW_STALE_ELF:-0}" "${REQUIRE_BUILD_RECEIPT:-0}" "$VITA_GL_BACKEND" \
	"$SDL2_VITAGL_PREFIX" "$VITAGL_PREFIX" "$DEPS_PREFIX" "$TSF_PREFIX" <<'RECEIPT' || die "player ELF build receipt rejected"
import hashlib
import json
import struct
import sys
from pathlib import Path

elf, root, archive, output = map(Path, sys.argv[1:5])
allow_stale, require = sys.argv[5] == "1", sys.argv[6] == "1"
backend = sys.argv[7]
sdl2_prefix, vitagl_prefix, deps_prefix, tsf_prefix = map(Path, sys.argv[8:12])
blob = elf.read_bytes()
receipt = None
if blob[:6] == b"\x7fELF\x01\x01":
    shoff, = struct.unpack_from("<I", blob, 0x20)
    size, count, names = struct.unpack_from("<HHH", blob, 0x2E)
    sections = [struct.unpack_from("<IIIIII", blob, shoff + i * size) for i in range(count)]
    table = sections[names]
    for name, kind, _, _, offset, length in sections:
        label = blob[table[4] + name:blob.index(b"\0", table[4] + name)]
        if label == b".mkxpz.build_receipt":
            receipt = json.loads(blob[offset:offset + length])
if receipt is None:
    # Shader delivery is decided by what the ELF was built with (optionalShaders).
    sys.exit("%s carries no build receipt: configure it with vita/scripts/configure-vita.sh and relink" % elf)
# The GL driver is baked in at compile time. Not governed by
# ALLOW_STALE_ELF — this is a different build, not a stale one.
if receipt.get("backend") != backend:
    sys.exit("ELF was built for the %s backend, not %s: reconfigure with "
             "VITA_GL_BACKEND=%s and relink" % (receipt.get("backend"), backend, backend))
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
# The shipped GXP set covers the boot programs only. An ELF built with the optional shaders links
# Bicubic/Lanczos3/... at boot, which have no cache file and no compiler on a player's device: the
# receipt, not the packaging shell, says what this ELF was built with.
if receipt.get("optionalShaders") is not False:
    sys.exit("ELF was built with optional shaders (receipt optionalShaders=%r): the shipped GXP set does not cover "
             "them; reconfigure with MKXPZ_OPTIONAL_SHADERS=0 and relink" % receipt.get("optionalShaders"))
sys.dont_write_bytecode = True
sys.path.insert(0, str(root / "vita/scripts"))
import treedigest
current = {str(p.relative_to(root)): sha(p) for p in (root / "vita/patches").rglob("*") if p.is_file()}
stale = ["patch " + n for n in sorted(set(current) | set(receipt["patches"]))
         if current.get(n) != receipt["patches"].get(n)]
stale += [n for n, digest in sorted(receipt["sources"].items()) if sha(root / n) != digest]
if treedigest.digest(root, treedigest.ENGINE) != receipt.get("engineSourcesSha256"):
    stale.append("engine sources (src, binding, shader, assets, meson files)")
if sha(archive) != receipt["rubyArchiveSha256"]:
    stale.append("Ruby archive (packed ruby/ is from a different build)")
# The static archives the ELF linked must be the ones the managed prefixes hold now, and the TinySoundFont
# headers likewise: a rebuilt or swapped dependency is a different executable.
for name, prefix in (("sdl2Vitagl", sdl2_prefix), ("vitagl", vitagl_prefix), ("vitaDeps", deps_prefix)):
    if treedigest.lib_digest((prefix / "lib").glob("*.a")) != receipt["dependencies"].get(name):
        stale.append("dependency archives (%s)" % name)
tsf_files = [tsf_prefix / "tsf.h", tsf_prefix / "tml.h"]
tsf_now = treedigest.lib_digest(tsf_files) if all(p.is_file() for p in tsf_files) else None
if tsf_now != receipt.get("tinysoundfont"):
    stale.append("TinySoundFont headers")
receipt["matchesPackagedTree"] = not stale
output.write_text(json.dumps(receipt, sort_keys=True) + "\n")
if stale:
    message = "ELF was built from other inputs: " + ", ".join(stale[:8]) + (" ..." if len(stale) > 8 else "")
    if not allow_stale:
        sys.exit(message + "\n  reconfigure and relink, or set ALLOW_STALE_ELF=1 to pack it labelled stale")
    print("package-vpk: WARNING: " + message, file=sys.stderr)
print("==> build receipt: ELF built at %s%s; matches packaged tree: %s" % (
    receipt["sourceRevision"][:12], " (dirty)" if receipt["sourceDirty"] else "", not stale))
RECEIPT
# Stage the root pin without modifying the author's input. Stock-game packages
# always enter through the device shim; author preloads run after game preloads.
# The product TITLE_ID must not ship a PINNED config, and a test build never
# uses the product id.
mkdir -p "$OUT"
python3 - "$MKXP_JSON" "$ASSETS/preload" "$OUT/mkxp-packaged.json" \
	"$TITLE_ID" "${ALLOW_PINNED_PRODUCT_ID:-0}" <<'PY'
import json
from pathlib import Path
import posixpath
import re
import sys

config, preload, output = map(Path, sys.argv[1:4])
title_id, allow_pinned = sys.argv[4:6]
prefix = "app0:/preload/"
try:
    names = ("settings_file.rb", "ruby_classic_wrap.rb", "win32_wrap.rb", "game_preloads.rb")
    for name in names:
        if not (preload / name).is_file():
            raise ValueError("missing mandatory preload: " + name)
    raw = config.read_bytes()
    root = json.loads(raw)
    if not isinstance(root, dict):
        raise ValueError("root must be a JSON object")
    if not isinstance(root.get("customScript", ""), str):
        raise ValueError("customScript must be a string")
    if root.get("gameFolder") or root.get("customScript"):
        if title_id == "MKXPZ0001" and allow_pinned != "1":
            raise ValueError(
                "a pinned root config (gameFolder/customScript set) is not allowed "
                "under the product TITLE_ID MKXPZ0001: a pinned player takes a test id "
                "(TITLE_ID=MKXPZ00xx), or ALLOW_PINNED_PRODUCT_ID=1 packs it anyway")
    original = root.copy()
    scripts = root.get("preloadScript", [])
    extras = root.get("vitaPackagePreloads", [])
    if not isinstance(scripts, list) or not isinstance(extras, list):
        raise ValueError("preloadScript and vitaPackagePreloads must be lists")
    scripts = scripts + extras
    for entry in scripts:
        if not isinstance(entry, str) or not entry or re.search(r"[\x00-\x1f\x7f]", entry):
            raise ValueError("preload entries must be nonempty path strings")
        entry = entry.replace("\\", "/")
        if entry.startswith("/") or (":" in entry and not re.match(r"[A-Za-z][A-Za-z0-9]*:/", entry)):
            raise ValueError("preloads need a game-relative or device:/ path")
        if ":/" in entry:
            device, tail = entry.split(":/", 1)
            entry = device + ":/" + posixpath.normpath("/" + tail).lstrip("/")
        if entry.startswith(prefix):
            name = entry[len(prefix):]
            if not name or "/" in name or not (preload / name).is_file():
                raise ValueError("preloadScript entry %r has no bundled file" % entry)
    if not root.get("customScript"):
        mandatory = {prefix + name for name in names}
        root["preloadScript"] = [prefix + "win32_wrap.rb"]
        root["vitaPackagePreloads"] = list(dict.fromkeys(s for s in scripts if s not in mandatory))
        root["enableSettings"] = True
    output.write_bytes(raw if root == original else (json.dumps(root, indent=2) + "\n").encode("utf-8"))
except (OSError, ValueError) as error:
    sys.exit(f"package-vpk: invalid root configuration {config}: {error}")
PY
MKXP_JSON="$OUT/mkxp-packaged.json"
# A committed binary is exactly what removed: the tracked
# Liberation Sans was 1.07.4 under the Liberation Fonts licence while this
# script called it OFL. Refuse to grow one back by accident, and refuse it
# unconditionally -- ALLOW_MISSING_FONTS is an escape from "not fetched yet",
# never from "do not commit fonts".
for tracked_font in "$ASSETS/fonts"/*.ttf; do
	[[ -e $tracked_font ]] || continue
	die "font binary must not be tracked: $tracked_font (fetch it into $FONTS_PREFIX with vita/scripts/fetch-fonts.sh)"
done

# The fetched set is exactly what fonts.json names: a font file the manifest does not list is refused (never
# packed unverified, whatever ALLOW_MISSING_FONTS says), and every packaged font must match its recorded digest.
# Only manifest entries are ever staged below.
python3 - "$ASSETS/fonts/fonts.json" "$FONTS_PREFIX" <<'FONTGATE' || exit 1
import hashlib
import json
from pathlib import Path
import sys

manifest, fetched = Path(sys.argv[1]), Path(sys.argv[2])
record = json.loads(manifest.read_text(encoding="utf-8"))
known = {entry["name"] for entry in record["files"] + record["documents"]}
problems = []
if fetched.is_dir():
    for path in sorted(fetched.iterdir()):
        if path.suffix.lower() in (".ttf", ".otf", ".ttc", ".otc", ".woff", ".woff2") and path.name not in known:
            problems.append("%s is not listed in fonts.json" % path.name)
    for entry in record["files"]:
        path = fetched / entry["name"]
        if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            problems.append("%s does not match its fonts.json digest" % entry["name"])
if problems:
    sys.exit("package-vpk: refusing the fonts in %s: %s\n"
             "  remove the extra files or re-run vita/scripts/fetch-fonts.sh" % (fetched, "; ".join(problems)))
FONTGATE

# What the fetched set must satisfy: every font fonts.json marks packaged, and
# between them the codepoints the launcher probes for. Liberation Sans has no
# kana and no kanji, so "the fonts directory is not empty" is not a check --
# a package that ships Liberation alone renders every Japanese game as tofu
# and leaves the launcher with no face for Shift_JIS titles.
if ! python3 - "$ASSETS/fonts/fonts.json" "$FONTS_PREFIX" <<'PY'
import json
from pathlib import Path
import struct
import sys


def subtables(blob):
    """(platform, encoding, offset) of every cmap subtable."""
    tag, count = struct.unpack_from(">IH", blob, 0)
    if tag not in (0x00010000, 0x74727565):
        raise ValueError("not a TrueType file (sfnt tag %#010x)" % tag)
    for index in range(count):
        name, _, offset, _ = struct.unpack_from(">4sIII", blob, 12 + 16 * index)
        if name == b"cmap":
            break
    else:
        raise ValueError("no cmap table")
    _, subs = struct.unpack_from(">HH", blob, offset)
    for index in range(subs):
        platform, encoding, relative = struct.unpack_from(">HHI", blob, offset + 4 + 8 * index)
        yield platform, encoding, offset + relative


def covers(blob, codepoints):
    """Every codepoint that some Unicode cmap subtable maps to a glyph."""
    found = set()
    for platform, encoding, base in subtables(blob):
        if (platform, encoding) not in ((3, 1), (3, 10), (0, 3), (0, 4), (0, 6)):
            continue
        fmt = struct.unpack_from(">H", blob, base)[0]
        if fmt == 4:
            segments = struct.unpack_from(">H", blob, base + 6)[0] // 2
            ends = struct.unpack_from(">%dH" % segments, blob, base + 14)
            starts = struct.unpack_from(">%dH" % segments, blob, base + 16 + 2 * segments)
            deltas = struct.unpack_from(">%dh" % segments, blob, base + 16 + 4 * segments)
            range_at = base + 16 + 6 * segments
            offsets = struct.unpack_from(">%dH" % segments, blob, range_at)
            for index in range(segments):
                for char in codepoints:
                    if not starts[index] <= char <= ends[index]:
                        continue
                    if offsets[index] == 0:
                        glyph = (char + deltas[index]) & 0xFFFF
                    else:
                        at = range_at + 2 * index + offsets[index] + 2 * (char - starts[index])
                        glyph = struct.unpack_from(">H", blob, at)[0]
                        if glyph:
                            glyph = (glyph + deltas[index]) & 0xFFFF
                    if glyph:
                        found.add(char)
        elif fmt == 12:
            groups = struct.unpack_from(">I", blob, base + 12)[0]
            for index in range(groups):
                start, end, glyph = struct.unpack_from(">III", blob, base + 16 + 12 * index)
                if glyph:
                    found.update(char for char in codepoints if start <= char <= end)
    return found


manifest, fetched = Path(sys.argv[1]), Path(sys.argv[2])
record = json.loads(manifest.read_text(encoding="utf-8"))
required = [int(text[2:], 16) for text in record["requiredCodepoints"]]
problems = []
covered = set()
for entry in record["files"]:
    if not entry.get("packaged"):
        continue
    font = fetched / entry["name"]
    if not font.is_file():
        problems.append("missing %s" % entry["name"])
        continue
    try:
        covered |= covers(font.read_bytes(), required)
    except (ValueError, struct.error) as error:
        problems.append("%s is not a usable font: %s" % (entry["name"], error))
absent = ["U+%04X" % char for char in required if char not in covered]
if absent and not problems:
    problems.append("no packaged font covers %s" % ", ".join(absent))
if problems:
    sys.exit("package-vpk: app0:/fonts is not shippable: %s\n"
             "  run vita/scripts/fetch-fonts.sh (it installs them into build/fonts),\n"
             "  or set ALLOW_MISSING_FONTS=1 for a CI or packaging-only build."
             % "; ".join(problems))
PY
then
	[[ ${ALLOW_MISSING_FONTS:-0} == 1 ]] || exit 1
	echo "package-vpk: WARNING: ALLOW_MISSING_FONTS=1 — packing app0:/fonts as it stands." >&2
	echo "                    This VPK is not shippable: Japanese text renders as tofu." >&2
fi

# Snapshot the incoming ELF before wiping $OUT — MKXPZ_ELF may legitimately
# point at a previous build/mkxp-z-vpk/mkxp-z.elf, and rm -f would delete
# the source we are about to copy. The snapshot carries DWARF naming
# build-machine paths, so it is released on every exit path, not only the
# success one that consumes it below.
ELF_SNAP=""
# Explicit template: argument-less BSD mktemp ignores $TMPDIR and creates
# in the darwin per-user temp dir instead, so the snapshot's home would
# differ per host.
ELF_SNAP=$(mktemp "${TMPDIR:-/tmp}/mkxpz-elf.XXXXXXXX")
trap '[[ -z $ELF_SNAP ]] || rm -f "$ELF_SNAP"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp -f "$ELF_IN" "$ELF_SNAP"

mkdir -p "$OUT"
rm -rf "$OUT/fonts"
rm -f "$OUT"/mkxp-z.{elf,velf,vpk} "$OUT"/eboot.bin "$OUT"/param.sfo "$OUT/$VPK_NAME"

# ---------------------------------------------------------------------------
# Stage app0:/fonts
# ---------------------------------------------------------------------------
# The fonts are fetched, never committed (vita/scripts/fetch-fonts.sh), so the packed
# tree is assembled here: the tracked README.md and fonts.json, plus each font
# fonts.json marks packaged. Checked above, before anything was removed. The upstream
# licence texts stay in $FONTS_PREFIX and are deliberately not packed --
# licensing is v1 work (vita/mkxp-z-vpk/fonts/README.md says where they live).
mkdir -p "$OUT/fonts"
for font_asset in "$ASSETS/fonts"/*; do
	[[ -f $font_asset ]] || continue
	cp -f "$font_asset" "$OUT/fonts/"
done
FONTS_FOUND=0
PACKAGED_FONTS=$(python3 -c '
import json, sys
for entry in json.load(open(sys.argv[1], encoding="utf-8"))["files"]:
    if entry.get("packaged"):
        print(entry["name"])
' "$ASSETS/fonts/fonts.json")
while IFS= read -r font_name; do
	[[ -n $font_name && -f "$FONTS_PREFIX/$font_name" ]] || continue
	cp -f "$FONTS_PREFIX/$font_name" "$OUT/fonts/"
	FONTS_FOUND=$((FONTS_FOUND + 1))
done <<<"$PACKAGED_FONTS"

SHADER_STAGE=0
# Players have no shader compiler: ship the precompiled GXP cache.
SHADERS_DIR="${VITAGL_SHADERS_DIR:-$ROOT/vita/vitagl-shaders}"
if ! python3 -B "$ROOT/vita/scripts/vitagl-shaders.py" check --source "$ROOT" --dir "$SHADERS_DIR"; then
	[[ ${ALLOW_MISSING_SHADERS:-0} == 1 ]] || die "shipped shader cache is missing or stale (vita/scripts/capture-vitagl-shaders.sh); ALLOW_MISSING_SHADERS=1 packs it anyway"
	echo "WARNING: packing an incomplete shader cache (ALLOW_MISSING_SHADERS=1)" >&2
fi
# The final-presentation probe marker prepends a define to SimpleShader's source: another cache key,
# absent from the shipped set. Only a device with libshacccg can compile that variant.
while IFS= read -r probe_spec; do
	[[ -n $probe_spec ]] || continue
	probe_src=${probe_spec%%=*}
	[[ -e $probe_src && -n $(find "$probe_src" -name final-presentation-probe -print -quit) ]] || continue
	[[ ${ALLOW_MISSING_SHADERS:-0} == 1 ]] || die "EXTRA_VPK_ADD carries final-presentation-probe: the shipped GXP set does not cover that shader variant (needs libshacccg on the device); ALLOW_MISSING_SHADERS=1 packs it anyway"
	echo "WARNING: final-presentation-probe shader variant has no shipped GXP (needs libshacccg)" >&2
done <<<"${EXTRA_VPK_ADD:-}"
rm -rf "$OUT/shader_cache"
mkdir -p "$OUT/shader_cache"
[[ ! -d $SHADERS_DIR/v2 ]] || cp -Rf "$SHADERS_DIR/v2" "$OUT/shader_cache/"
[[ -d $OUT/shader_cache/v2 ]] || mkdir -p "$OUT/shader_cache/v2"
SHADER_STAGE=1

# Licence texts and third-party notices travel with the release.
LICENSE_STAGE=0
if [[ $MKXPZ_RELEASE -eq 1 ]]; then
	rm -rf "$OUT/licenses"
	mkdir -p "$OUT/licenses"
	cp -Rf "$ROOT/licenses/." "$OUT/licenses/"
	cp -f "$ROOT/LICENSE" "$ROOT/THIRD-PARTY.md" "$OUT/licenses/"
	LICENSE_STAGE=1
fi

echo "VITASDK=$VITASDK"
echo "gcc=$(arm-vita-eabi-gcc -dumpversion)"
echo "out=$OUT"
echo "TITLE_ID=$TITLE_ID ATTRIBUTE2=$ATTRIBUTE2"
echo "module/ none (vitaGL needs no driver modules; libshacccg.suprx user-supplied on device)"
echo "fonts=$FONTS_FOUND fetched from $FONTS_PREFIX"

echo "eboot=REAL ($ELF_IN)"
# Already-linked meson output: keep DWARF on a side copy, strip for pack.
cp -f "$ELF_SNAP" "$OUT/mkxp-z.elf"
cp -f "$ELF_SNAP" "$OUT/mkxp-z.elf.unstripped"
rm -f "$ELF_SNAP"
ELF_SNAP=""

# Stripping policy — deliberate, do not "simplify":
#   $OUT/mkxp-z.elf.unstripped  full DWARF, stays beside the VPK, NEVER packed.
#                               It is the only symbolisation source for a
#                               hardware .psp2dmp (no NT_GNU_BUILD_ID note), so
#                               meson must keep -g and must not gain strip=true.
#   $OUT/mkxp-z.elf             the packed copy; -g removes the DWARF paths.
# `strip -g` is necessary but NOT sufficient for privacy: __FILE__ and MRI's
# generated #line literals live in .rodata and survive it. Those are handled
# upstream of here by the prefix maps / relative srcdir / neutral Ruby prefix
# in configure-vita.sh, build-ruby-vita.sh and build-sdl2-vitagl.sh.
# The build receipt stays on the unstripped copy only: vita-elf-create keeps
# unloaded sections, and eboot.bin bytes must not vary with the receipt.
echo "==> arm-vita-eabi-strip -g"
arm-vita-eabi-strip -g --remove-section=.mkxpz.build_receipt "$OUT/mkxp-z.elf"

echo "==> vita-elf-create"
vita-elf-create "$OUT/mkxp-z.elf" "$OUT/mkxp-z.velf"

echo "==> vita-make-fself (unsafe + -na disable ASLR; needed to write ux0:data)"
vita-make-fself -na "$OUT/mkxp-z.velf" "$OUT/eboot.bin"

echo "==> vita-mksfoex TITLE_ID=$TITLE_ID ATTRIBUTE2=$ATTRIBUTE2${APP_VER:+ APP_VER=$APP_VER}"
APP_VER_ARGS=()
if [[ -n ${APP_VER:-} ]]; then
	APP_VER_ARGS+=(-s "APP_VER=$APP_VER")
fi
vita-mksfoex -s TITLE_ID="$TITLE_ID" -d "ATTRIBUTE2=$ATTRIBUTE2" \
	${APP_VER_ARGS[@]+"${APP_VER_ARGS[@]}"} \
	"$TITLE" "$OUT/param.sfo"

echo "==> vita-pack-vpk"
# sourceRevision/sourceDirty describe the checkout that PACKED this VPK
# (untracked files count as dirty); "build" is the ELF's own link-time receipt.
SOURCE_REVISION=$(git -C "$ROOT" rev-parse HEAD)
SOURCE_DIRTY=0
if [[ -n $(git -C "$ROOT" status --porcelain) ]]; then SOURCE_DIRTY=1; fi
python3 - "$OUT/mkxp-vita-package.json" "$SOURCE_REVISION" "$SOURCE_DIRTY" "$RUBY_PREFIX" "$OUT" "$BUILD_RECEIPT" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

output, revision, dirty, ruby_prefix, out_dir, receipt = sys.argv[1:]
manifest = {"schemaVersion": 1, "executableKind": "player",
            "sourceRevision": revision, "sourceDirty": dirty == "1",
            "rubyApiVersion": "3.1.0"}
archive = Path(ruby_prefix) / "lib/libruby-static.a"
if archive.is_file():
    manifest["rubyArchiveSha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
if Path(receipt).is_file():
    manifest["build"] = json.loads(Path(receipt).read_text())
# Symbolisation provenance. The ELF carries no NT_GNU_BUILD_ID, so a
# .psp2dmp can only be resolved against the byte-identical mkxp-z.elf.unstripped
# that is deliberately kept OUT of the VPK. Recording all three digests is what
# lets a later dump be matched to the right (private) symbol file. File names
# only -- never a path, because this manifest ships inside the VPK.
digests = {}
for name in ["eboot.bin", "mkxp-z.elf", "mkxp-z.elf.unstripped"]:
    candidate = Path(out_dir) / name
    if candidate.is_file():
        digests[name] = hashlib.sha256(candidate.read_bytes()).hexdigest()
if digests:
    manifest["artifactSha256"] = digests
Path(output).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY
# -s writes sce_sys/param.sfo. fonts/ and config/ land at app0:/.
# mkxp.json at VPK root is the first config mkxp-z reads (CWD = app0:/).
# The default (vita/mkxp-z-vpk/mkxp.json) pins NOTHING, which makes the
# product bubble the launcher; a pinned player needs an explicit MKXP_JSON
# under a test TITLE_ID.
VPK_ARGS=(
	-s "$OUT/param.sfo"
	-b "$OUT/eboot.bin"
	-a "$OUT/fonts=fonts"
	-a "$ASSETS/config=config"
	-a "$ASSETS/preload=preload"
	-a "$OUT/mkxp-vita-package.json=mkxp-vita-package.json"
)
if [[ $SHADER_STAGE -eq 1 ]]; then
	VPK_ARGS+=(-a "$OUT/shader_cache=shader_cache")
fi
if [[ $LICENSE_STAGE -eq 1 ]]; then
	VPK_ARGS+=(-a "$OUT/licenses=licenses")
fi
VPK_ARGS+=(-a "$RUBY_LIB=ruby")
# LiveArea art: every VPK, test or product, carries the icon and
# the LiveArea pages. Every image must be 8-bit paletted at the documented
# size: the installer silently drops the LiveArea page for a 4-bit startup.png
# (seen on hardware), so refuse anything else here.
python3 - "$ASSETS/sce_sys" <<'PY'
import struct
import sys
from pathlib import Path

root = Path(sys.argv[1])
wanted = {"icon0.png": (128, 128), "livearea/contents/bg.png": (840, 500),
          "livearea/contents/startup.png": (280, 158)}
for name, size in wanted.items():
    path = root / name
    blob = path.read_bytes() if path.is_file() else b""
    if blob[:8] != b"\x89PNG\r\n\x1a\n" or blob[12:16] != b"IHDR":
        sys.exit("package-vpk: LiveArea art %s is missing or not a PNG" % name)
    width, height, depth, colour = struct.unpack(">IIBB", blob[16:26])
    if (width, height) != size or colour != 3 or depth != 8:
        sys.exit("package-vpk: %s must be %dx%d 8-bit paletted, got %dx%d depth %d colour type %d"
                 % (name, size[0], size[1], width, height, depth, colour))
if not (root / "livearea/contents/template.xml").is_file():
    sys.exit("package-vpk: LiveArea template.xml is missing")
PY
VPK_ARGS+=(
	-a "$ASSETS/sce_sys/icon0.png=sce_sys/icon0.png"
	-a "$ASSETS/sce_sys/livearea/contents=sce_sys/livearea/contents"
)
VPK_ARGS+=(-a "$MKXP_JSON=mkxp.json")
echo "    packing root mkxp.json from $MKXP_JSON"
# alsoft.conf must land at the VPK ROOT: openal-soft opens exactly
# app0:/alsoft.conf and then ux0:/data/openal/alsoft.conf, first one wins.
# Anywhere else in the package is a file nothing reads.
VPK_ARGS+=(-a "$ASSETS/alsoft.conf=alsoft.conf")
echo "    packing root alsoft.conf from $ASSETS/alsoft.conf"
# Packaging variants add whole trees here. Empty by
# default, so the product package is unchanged.
if [[ -n ${EXTRA_VPK_ADD:-} ]]; then
	while IFS= read -r add_spec; do
		[[ -n $add_spec ]] || continue
		add_src=${add_spec%%=*}
		add_dst=${add_spec#*=}
		# vita-pack-vpk splits -a on the first '='; keep exactly one so the
		# spec here and the tool's parse cannot disagree.
		if [[ $add_spec != *=* || -z $add_src || -z $add_dst || $add_dst == *=* ]]; then
			die "EXTRA_VPK_ADD entry must be src=dest with one '=', got: $add_spec"
		fi
		[[ -e $add_src ]] || die "EXTRA_VPK_ADD source missing: $add_src"
		VPK_ARGS+=(-a "$add_src=$add_dst")
		echo "    packing extra $add_dst from $add_src"
	done <<<"$EXTRA_VPK_ADD"
fi
vita-pack-vpk "${VPK_ARGS[@]}" "$OUT/$VPK_NAME"

echo
echo "built $OUT/$VPK_NAME"
echo
echo "--- product packaging contract ---"
echo "TITLE_ID     $TITLE_ID   (product id; test builds use other MKXPZ00xx)"
echo "ATTRIBUTE2   $ATTRIBUTE2   (extended memory: ~230 MiB user vs ~121)"
echo "APP_VER      ${APP_VER:-none}   (param.sfo release string; vita/VERSION)"
echo "module/      none — the pinned vitaGL stack is linked into the player;"
echo "             shaders load from shader_cache/ (precompiled GXP, no compiler needed);"
echo "             an uncached program still needs the user-supplied libshacccg.suprx."
echo "fonts/       fetched fallback faces from $FONTS_PREFIX (vita/scripts/fetch-fonts.sh),"
echo "             with README.md and fonts.json from vita/mkxp-z-vpk/fonts/:"
ls "$OUT/fonts" | sed 's/^/             /'
echo "             VL Gothic / VL PGothic 20230918: VL Gothic licence (BSD style,"
echo "             M+ FONTS + Sazanami); Liberation Sans 2.00.1: OFL 1.1, staged"
echo "             from this tree's assets. fonts.json records every"
echo "             digest, licence and source; the font licence texts ship in"
echo "             licenses/ with the release package (MKXPZ_RELEASE=1)."
echo "             NOTE: PhysFS mounts the game folder, not app0:."
echo "             Bundled fonts are fallbacks; put TTFs in the game's Fonts/."
echo "config/      default.json → app0:/config/default.json (schema placeholder)"
echo "preload/     Ruby preload scripts from vita/mkxp-z-vpk/preload/:"
ls "$ASSETS/preload" | sed 's/^/             /'
echo "             loose Ruby files; the device entry point composes game preloads"
echo "mkxp.json    app0:/mkxp.json — first config mkxp-z reads. Unpinned by default:"
echo "             empty gameFolder/customScript make the bubble the LAUNCHER;"
echo "             pack a pinned player with MKXP_JSON + a test TITLE_ID."
echo "alsoft.conf  app0:/alsoft.conf — OpenAL-soft device configuration."
echo "             openal-soft opens app0:/alsoft.conf, then"
echo "             ux0:/data/openal/alsoft.conf, and stops at the first that"
echo "             opens — so this file shadows the ux0 one. Retune by editing"
echo "             it in place at ux0:app/$TITLE_ID/alsoft.conf; no rebuild."
if [[ $LICENSE_STAGE -eq 1 ]]; then
	echo "licenses/    app0:/licenses — LICENSE (GPL-3.0), THIRD-PARTY.md and the component"
	echo "             licence texts"
fi
echo "sce_sys/     param.sfo, icon0.png and livearea/contents (bg, startup, template.xml)"
echo "manifest     executable identity and source revision; deployment requires executableKind=player"
echo "             artifactSha256 matches a crash dump to its symbols"
echo "ruby/        Ruby 3.1 extension wrappers from installed prefix"
echo "PRIVATE      $(basename "$OUT")/mkxp-z.elf.unstripped is NOT in the VPK. Keep it to"
echo "             symbolise .psp2dmp crash dumps; do not publish it."
echo
ls -l "$OUT/$VPK_NAME" "$OUT/eboot.bin" "$OUT/param.sfo"
unzip -l "$OUT/$VPK_NAME"
