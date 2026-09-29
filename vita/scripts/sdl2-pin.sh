# SPDX-License-Identifier: GPL-3.0-or-later
# Shared SDL2 source pin for the Vita SDL build (build-sdl2-vitagl.sh): one
# version, one digest, one fetch path. The build script owns its own prefix,
# receipt and patch application.
# shellcheck shell=bash
SDL2_VER=2.32.8
SDL2_SHA256=0ca83e9c9b31e18288c7ec811108e58bac1f1bb5ec6577ad386830eac51c787e
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VER}/SDL2-${SDL2_VER}.tar.gz"

# sdl2_fetch_src <src-dir> <die-fn>: unless <src-dir> already holds a tree,
# download the pinned tarball, verify its digest and extract it there. Any
# existing tree is replaced, so callers must re-apply their patches after
# this (build-sdl2-vitagl.sh does).
sdl2_fetch_src() {
  local src="$1" die="$2" tmp
  tmp=$(mktemp -d) || "$die" "mktemp failed"
  if ! curl -fsSL -o "$tmp/SDL2-$SDL2_VER.tar.gz" "$SDL2_URL"; then
    rm -rf "$tmp"
    "$die" "SDL2 download failed"
  fi
  if [[ "$(shasum -a 256 "$tmp/SDL2-$SDL2_VER.tar.gz" | cut -d' ' -f1)" != "$SDL2_SHA256" ]]; then
    rm -rf "$tmp"
    "$die" "SDL2-$SDL2_VER.tar.gz digest mismatch"
  fi
  tar xzf "$tmp/SDL2-$SDL2_VER.tar.gz" -C "$tmp"
  rm -rf "$src"
  mkdir -p "$(dirname "$src")"
  mv "$tmp/SDL2-$SDL2_VER" "$src"
  rm -rf "$tmp"
}
