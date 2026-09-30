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

# Sealed source trees (vita/scripts/treedigest.py): a tree carries the identity of what it was made from (URL, version,
# tarball digest, every patch and text transformation) and its content digest.
# seal_tree <dir> <name> records both. verify_tree <dir> <name> dies unless the tree is that of the current pins, unchanged.
# tree_reusable <dir> <name> is 0 for such a tree and 1 when it is absent, unsealed or sealed from other inputs (extract
# it again); a tree of the current inputs whose content changed (edited or built in place) is refused.
seal_tree() { python3 -B "$ROOT/vita/scripts/treedigest.py" seal "$1" "$2" || die "could not seal source tree $1"; }
verify_tree() { python3 -B "$ROOT/vita/scripts/treedigest.py" verify "$1" "$2" || die "source tree $1 failed its integrity check"; }
tree_reusable() {
  local state
  state=$(python3 -B "$ROOT/vita/scripts/treedigest.py" state "$1" "$2") || die "could not check source tree $1"
  case "$state" in
    current) return 0 ;;
    stale) return 1 ;;
    *) die "source tree $1 differs from what was fetched (edited, or built in tree); remove it and rebuild" ;;
  esac
}

# fetch_git_pinned <dir> <url> <commit>: a managed clone checked out, detached, at exactly <commit>.
fetch_git_pinned() {
  local dir=$1 url=$2 commit=$3
  if [[ ! -d "$dir/.git" ]]; then
    [[ ! -e $dir ]] || die "$dir exists but is not a managed git clone"
    mkdir -p "$(dirname "$dir")"
    git clone --quiet --no-checkout "$url" "$dir"
  fi
  if ! git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
    git -C "$dir" fetch --quiet origin "$commit"
  fi
  git -C "$dir" checkout --quiet --force --detach "$commit"
  [[ "$(git -C "$dir" rev-parse HEAD)" == "$commit" ]] || die "$dir is not at pinned commit $commit; remove it and rebuild"
  git -C "$dir" diff --quiet && git -C "$dir" diff --cached --quiet || die "$dir has tracked edits; remove it and rebuild"
}

# vdpm_downloads: one line per pinned VitaSDK package source: package, file, url, sha256 (tab separated).
vdpm_downloads() {
  python3 -c '
import json, sys
for package, entry in json.load(open(sys.argv[1]))["vdpm"]["packages"].items():
    for source in entry["sources"]:
        print("\t".join((package, source["file"], source["url"], source["sha256"])))
' "$DEP_PINS"
}

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
