# SPDX-License-Identifier: GPL-3.0-or-later
# Shared helpers for the pinned, digest-verified dependency sources (vita/scripts/dep-pins.json).
# Sourced by the build scripts; needs ROOT, and a die() that takes one message.
# shellcheck shell=bash

DEP_PINS="$ROOT/vita/scripts/dep-pins.json"

# dep_pin <entry> <key> [<subkey>]: print one value of dep-pins.json.
dep_pin() {
  python3 -c '
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2:]:
    value = value[key]
print(value)
' "$DEP_PINS" "$@"
}

sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }

# fetch_verified <url> <sha256> <dest>: keep a cached <dest> whose digest matches, otherwise download it and
# refuse anything that does not. A cached file with the wrong digest is replaced, never used.
fetch_verified() {
  local url=$1 want=$2 dest=$3
  if [[ -f $dest && $(sha256_of "$dest") == "$want" ]]; then return 0; fi
  mkdir -p "$(dirname "$dest")"
  rm -f "$dest" "$dest.part"
  curl --proto '=https' --tlsv1.2 -fsSL --retry 3 -o "$dest.part" "$url" || die "download failed: $url"
  [[ $(sha256_of "$dest.part") == "$want" ]] || { rm -f "$dest.part"; die "sha256 mismatch for $url (want $want)"; }
  mv "$dest.part" "$dest"
}

# seal_tree / verify_tree <dir>: record, then re-check, the content digest of a fetched source tree.
seal_tree() { python3 -B "$ROOT/vita/scripts/treedigest.py" seal "$1"; }
verify_tree() { python3 -B "$ROOT/vita/scripts/treedigest.py" verify "$1" || die "source tree $1 failed its integrity check"; }

# fetch_gnu_config <dest-dir> <dist-dir>: config.guess and config.sub at the pinned gcc-mirror commits, digest-checked
# (mode 0755), never "whatever master holds today".
fetch_gnu_config() {
  local dest=$1 dist=$2 name commit want
  mkdir -p "$dest"
  for name in config.guess config.sub; do
    commit=$(dep_pin gnuConfig "$name" commit)
    want=$(dep_pin gnuConfig "$name" sha256)
    fetch_verified "https://raw.githubusercontent.com/gcc-mirror/gcc/$commit/$name" "$want" "$dist/$name"
    if [[ ! -f $dest/$name || $(sha256_of "$dest/$name") != "$want" ]]; then cp -f "$dist/$name" "$dest/$name"; fi
    chmod 755 "$dest/$name"
  done
}

# require_under_build <what> <path>: a path a script empties or resets must lie strictly inside $ROOT/build
# (physical paths, so a symlink cannot lead out of it).
require_under_build() {
  python3 -c '
import os, sys
build = os.path.realpath(os.path.join(sys.argv[1], "build"))
path = os.path.realpath(sys.argv[2])
sys.exit(0 if path.startswith(build + os.sep) else 1)
' "$ROOT" "$2" || die "$1 must be a directory inside $ROOT/build, got: $2"
}
