#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# fetch-fonts.sh — install the bundled fallback fonts into a local prefix.
#
# Installs (under build/fonts/):
#   VL-Gothic-Regular.ttf        Japanese fallback, fixed pitch  (packed)
#   VL-PGothic-Regular.ttf       Japanese fallback, proportional (packed)
#   LiberationSans-Regular.ttf   Latin fallback                  (packed)
#   LICENSE*, README*, Changelog upstream VL Gothic texts        (kept, not packed)
#
# Why these three: RPG Maker XP/VX games name Windows fonts
# that cannot be redistributed, and the XP RTP ships no font at all, so the
# bundled set IS what those games render with. Liberation Sans alone has no
# kana and no kanji, so a package carrying only it renders every Japanese game
# as tofu — and the launcher, which picks its list font by probing HIRAGANA A /
# KATAKANA A / CJK ONE rather than by name, would find no face at all.
#
# Font binaries are NEVER committed. This script is the fetch path; both
# sources are pinned and verified against a SHA-256 before anything is
# installed, so a failed or tampered download fails the build instead of
# shipping.
#
#   VL Gothic     one pinned upstream release archive, downloaded over HTTPS,
#                 checked against ARCHIVE_SHA256 below (fail closed).
#   Liberation    staged out of this tree's assets/liberation.ttf
#                 (2.00.1, OFL 1.1). No download: the engine tree already has it.
#
# Idempotent: a prefix that already holds every manifest file with the right
# digest is left alone and nothing is downloaded. A prefix holding the wrong
# bytes (an older pin, a truncated file) is reinstalled.
#
# Environment:
#   FONTS_PREFIX     install directory      (default build/fonts)
#   FONTS_MANIFEST   provenance record      (default vita/mkxp-z-vpk/fonts/fonts.json)
#   FONTS_ARCHIVE    install from this local archive instead of downloading
#   FONTS_SHA256     expected digest of FONTS_ARCHIVE; requires FONTS_ARCHIVE,
#                    so the download path can never run with a weakened digest
#   LIBERATION_TTF   Liberation source file (default assets/liberation.ttf)
#
# Host-only: downloads and copies, builds nothing, touches no device.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX="${FONTS_PREFIX:-$ROOT/build/fonts}"
MANIFEST="${FONTS_MANIFEST:-$ROOT/vita/mkxp-z-vpk/fonts/fonts.json}"
LIBERATION_TTF="${LIBERATION_TTF:-$ROOT/assets/liberation.ttf}"

# The pinned VL Gothic release. Upstream project: https://vlgothic.dicey.org/
# (OSDN, the historical home, no longer resolves). Mirror of the same releases:
# https://github.com/daisukesuzuki/VLGothic
ARCHIVE_NAME="VLGothic-20230918.tar.xz"
ARCHIVE_URL="https://vlgothic.dicey.org/releases/$ARCHIVE_NAME"
ARCHIVE_SHA256="c064b19e72da23a26ef18f336bcfed2cb2f5243cc055f1cfa0b35afc7e850e18"

die() { echo "fetch-fonts: $*" >&2; exit 1; }

command -v python3 >/dev/null || die "missing host command: python3"
[[ -f $MANIFEST ]] || die "missing font manifest: $MANIFEST"

if [[ -n ${FONTS_SHA256:-} && -z ${FONTS_ARCHIVE:-} ]]; then
	die "FONTS_SHA256 needs FONTS_ARCHIVE; the download is pinned to $ARCHIVE_SHA256"
fi

# A font file the manifest does not list is never installed alongside the pinned set: refuse it.
python3 - "$MANIFEST" "$PREFIX" <<'PY' || exit 1
import json
from pathlib import Path
import sys

manifest, prefix = Path(sys.argv[1]), Path(sys.argv[2])
record = json.loads(manifest.read_text(encoding="utf-8"))
known = {entry["name"] for entry in record["files"] + record["documents"]}
extra = []
if prefix.is_dir():
    extra = [p.name for p in sorted(prefix.iterdir())
             if p.suffix.lower() in (".ttf", ".otf", ".ttc", ".otc", ".woff", ".woff2") and p.name not in known]
if extra:
    sys.exit("fetch-fonts: %s holds fonts that fonts.json does not list: %s; remove them" % (prefix, ", ".join(extra)))
PY

# ---------------------------------------------------------------------------
# Already installed?
# ---------------------------------------------------------------------------
# "Present" is not enough: an earlier pin leaves files with the right names and
# the wrong bytes, and those would be packed without complaint.
if python3 - "$MANIFEST" "$PREFIX" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

manifest, prefix = Path(sys.argv[1]), Path(sys.argv[2])
record = json.loads(manifest.read_text(encoding="utf-8"))
for entry in record["files"] + record["documents"]:
    candidate = prefix / entry["name"]
    if not candidate.is_file():
        sys.exit(1)
    if hashlib.sha256(candidate.read_bytes()).hexdigest() != entry["sha256"]:
        sys.exit(1)
sys.exit(0)
PY
then
	echo "fetch-fonts: already present at $PREFIX"
	ls -la "$PREFIX"
	exit 0
fi

# ---------------------------------------------------------------------------
# Obtain the VL Gothic archive
# ---------------------------------------------------------------------------
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

EXPECT_SHA256="$ARCHIVE_SHA256"
if [[ -n ${FONTS_ARCHIVE:-} ]]; then
	[[ -f $FONTS_ARCHIVE ]] || die "FONTS_ARCHIVE=$FONTS_ARCHIVE does not exist"
	ARCHIVE="$FONTS_ARCHIVE"
	echo "==> VL Gothic from $ARCHIVE (local, no download)"
	if [[ -n ${FONTS_SHA256:-} ]]; then
		EXPECT_SHA256="$FONTS_SHA256"
		echo "    WARNING: FONTS_SHA256 overrides the pinned digest; this is a"
		echo "             test and mirror hook, never a release path."
	fi
else
	ARCHIVE="$TMP/$ARCHIVE_NAME"
	echo "==> $ARCHIVE_URL"
	command -v curl >/dev/null || die "missing host command: curl"
	curl -sSL --fail -o "$ARCHIVE" "$ARCHIVE_URL" ||
		die "download failed: $ARCHIVE_URL (set FONTS_ARCHIVE=<path> to install from a local copy)"
fi

# ---------------------------------------------------------------------------
# Verify and install
# ---------------------------------------------------------------------------
# One Python pass so extraction, verification and installation cannot disagree:
# the archive digest is checked first, every member is extracted in memory (no
# path from the tarball ever reaches the filesystem), each file is matched
# against the manifest, and only then is anything written to the prefix.
python3 - "$MANIFEST" "$PREFIX" "$ARCHIVE" "$EXPECT_SHA256" "$LIBERATION_TTF" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
import tarfile

manifest, prefix, archive, expected, liberation = sys.argv[1:]
manifest, prefix, archive = Path(manifest), Path(prefix), Path(archive)
record = json.loads(manifest.read_text(encoding="utf-8"))
entries = record["files"] + record["documents"]


def fail(message):
    sys.exit("fetch-fonts: " + message)


blob = archive.read_bytes()
digest = hashlib.sha256(blob).hexdigest()
if digest != expected:
    fail("archive digest mismatch for %s\n  expected %s\n  got      %s"
         % (archive.name, expected, digest))

# The member layout is part of the pin, so an archive that hashes correctly but
# holds a different tree is still refused rather than silently half-installed.
wanted = {entry["member"]: entry for entry in entries if entry["from"] == "archive"}
staged = {}
try:
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if member.isdir():
                continue
            if not member.isfile():
                fail("archive holds a non-regular member: %s" % member.name)
            if member.name not in wanted:
                fail("archive member is not in %s: %s" % (manifest.name, member.name))
            handle = tar.extractfile(member)
            staged[member.name] = handle.read() if handle else b""
except tarfile.TarError as error:
    fail("cannot read %s: %s" % (archive.name, error))
missing = sorted(set(wanted) - set(staged))
if missing:
    fail("archive is missing %s" % ", ".join(missing))

source = Path(liberation)
if not source.is_file():
    fail("Liberation source missing: %s\n"
         "  it is staged from this tree, not downloaded;\n"
         "  restore assets/liberation.ttf or set LIBERATION_TTF"
         % source)
for entry in entries:
    if entry["from"] == "mkxp-z":
        staged[entry["member"]] = source.read_bytes()

for entry in entries:
    data = staged[entry["member"]]
    digest = hashlib.sha256(data).hexdigest()
    if digest != entry["sha256"] or len(data) != entry["bytes"]:
        fail("%s does not match %s\n  expected %s (%d bytes)\n  got      %s (%d bytes)"
             % (entry["member"], manifest.name, entry["sha256"], entry["bytes"],
                digest, len(data)))

prefix.mkdir(parents=True, exist_ok=True)
for entry in entries:
    target = prefix / entry["name"]
    # Write beside the target and rename, so an interrupted run cannot leave a
    # half-written font that the next run's digest check would have to catch.
    temporary = prefix / ("." + entry["name"] + ".part")
    temporary.write_bytes(staged[entry["member"]])
    temporary.replace(target)
    print("    %-26s %8d  %s" % (entry["name"], entry["bytes"], entry["sha256"][:16]))
PY

echo "fetch-fonts: installed into $PREFIX"
ls -la "$PREFIX"
