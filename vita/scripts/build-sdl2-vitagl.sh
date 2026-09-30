#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build-sdl2-vitagl.sh — rebuild SDL2 2.32.8 with the ported VIDEO_VITA_VGL
# backend (vita/patches/sdl2/0001-vita-vgl-backend.patch, then the follow-up fixes
# in 0002-…) for the vitaGL backend.
#
# Requires vita/scripts/build-vitagl.sh to have run (libvitaGL.a, libvitashark.a,
# libmathneon.a, libSceShaccCgExt.a, libtaihen_stub.a + headers in the side
# prefix). Installs to build/sdl2-vitagl-prefix; $VITASDK's stock sdl2 is never
# touched.
#
# Idempotent via a receipt: skips only when
# the prefix's receipt records the same SDL version, source, patch digest,
# flags, cmake options, compiler and vitaGL headers/archives as this run, and
# every installed file still matches. An explicit SDL2_VITAGL_SRC tree is used
# as given, patched in place (it is this build's private tree; never point it
# at build/sdl2-src) and recorded by content digest.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?VITASDK not found}"
# shellcheck source=/dev/null
. "$ROOT/vita/scripts/sdl2-pin.sh"

VITAGL="${VITAGL_PREFIX:-$ROOT/build/vitagl-prefix}"
PREFIX="${SDL2_VITAGL_PREFIX:-$ROOT/build/sdl2-vitagl-prefix}"
SRC="${SDL2_VITAGL_SRC:-$ROOT/build/sdl2-vitagl-src}"
BUILD="$ROOT/build/sdl2-vitagl-build"
# The series, applied in name order; the receipt digests all of it.
SDL2_PATCHES=("$ROOT"/vita/patches/sdl2/[0-9][0-9][0-9][0-9]-*.patch)

die() { echo "build-sdl2-vitagl: $*" >&2; exit 1; }

for needed in "$VITAGL/include/vitaGL.h" \
              "$VITAGL/lib/libvitaGL.a" "$VITAGL/lib/libvitashark.a" \
              "$VITAGL/lib/libmathneon.a" "$VITAGL/lib/libSceShaccCgExt.a" \
              "$VITAGL/lib/libtaihen_stub.a"; do
  [[ -f "$needed" ]] || die "missing $needed — run vita/scripts/build-vitagl.sh first"
done
[[ -f "${SDL2_PATCHES[0]}" ]] || die "no patches under $ROOT/vita/patches/sdl2"
sdl2_patches_digest() { cat "${SDL2_PATCHES[@]}" | shasum -a 256 | cut -d' ' -f1; }

# Apply the series, idempotently: a patch later in the series rewrites lines of
# the earlier ones, so find the newest patch that reverse-applies (the tree
# carries it and everything before it), then apply the rest in order. Fail
# loudly on a tree that matches neither. git apply run inside a repository
# resolves patch paths against that repository and silently matches nothing;
# cap discovery at $SRC's parent so the disposable tarball tree (a repo itself
# never) is treated standalone.
apply_backend_patch() {
  local ceiling="$(dirname "$SRC")" applied=0 i
  [[ -z ${GIT_CEILING_DIRECTORIES:-} ]] || ceiling="$ceiling:$GIT_CEILING_DIRECTORIES"
  for ((i = ${#SDL2_PATCHES[@]} - 1; i >= 0; i--)); do
    if GIT_CEILING_DIRECTORIES="$ceiling" git -C "$SRC" apply --reverse --check "${SDL2_PATCHES[i]}" 2>/dev/null; then
      applied=$((i + 1))
      break
    fi
  done
  for ((i = applied; i < ${#SDL2_PATCHES[@]}; i++)); do
    GIT_CEILING_DIRECTORIES="$ceiling" git -C "$SRC" apply --check "${SDL2_PATCHES[i]}" 2>/dev/null \
      || die "$SRC does not match the pin plus patches before $(basename "${SDL2_PATCHES[i]}") — restore a vanilla tree"
    echo "==> apply $(basename "${SDL2_PATCHES[i]}") -> $SRC"
    GIT_CEILING_DIRECTORIES="$ceiling" git -C "$SRC" apply "${SDL2_PATCHES[i]}"
  done
}

# Non-identifying file paths: the ordering is load-bearing (most general map
# first; sort by ascending path length).
PREFIX_MAP_LINES=""
add_prefix_map() {
  local from="$1" to="$2" real physical
  [[ -n "$from" && "$from" != "/" ]] || return 0
  physical=""
  [[ -d "$from" ]] && physical="$(cd "$from" && pwd -P)"
  for real in "$from" "$physical"; do
    [[ -n "$real" && "$real" != "/" ]] || continue
    case "$real" in *[[:space:]]*) continue ;; esac
    PREFIX_MAP_LINES="$PREFIX_MAP_LINES${#real} -ffile-prefix-map=$real=$to
"
  done
}
add_prefix_map "${HOME:-}" /mkxpz-home
add_prefix_map "$VITASDK"  /vitasdk
add_prefix_map "$ROOT"     /mkxp-z-vita
add_prefix_map "$SRC"      /sdl2-src
add_prefix_map "$BUILD"    /sdl2-build
add_prefix_map "$PREFIX"   /sdl2-vitagl
add_prefix_map "$VITAGL"   /vitagl
FILE_PREFIX_MAPS="$(printf '%s' "$PREFIX_MAP_LINES" | sort -u | sort -n -k1,1 \
  | cut -d' ' -f2- | tr '\n' ' ')"
C_FLAGS="-std=gnu11 -I$VITAGL/include"
C_FLAGS="$C_FLAGS -Wno-error=implicit-function-declaration"
C_FLAGS="$C_FLAGS -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"
C_FLAGS="$C_FLAGS ${FILE_PREFIX_MAPS% }"

CMAKE_OPTIONS=(
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DCMAKE_BUILD_TYPE=Release
  -DSDL_TEST=OFF
  -DSDL_GCC_ATOMICS=ON
  -DVIDEO_VITA_PIB=OFF
  -DVIDEO_VITA_PVR=OFF
  -DVIDEO_VITA_VGL=ON
)

RECEIPT="$PREFIX/mkxpz-sdl2-vitagl-receipt.json"
tree_digest() {
  python3 - "$1" <<'PY'
import hashlib, sys
from pathlib import Path
root, digest = Path(sys.argv[1]), hashlib.sha256()
for p in sorted(root.rglob("*")):
    if p.is_file() and ".git" not in p.relative_to(root).parts:
        digest.update(str(p.relative_to(root)).encode() + b"\0" + p.read_bytes() + b"\0")
print(digest.hexdigest())
PY
}
if [[ -n ${SDL2_VITAGL_SRC:-} ]]; then
  [[ -f "$SRC/CMakeLists.txt" ]] || die "SDL2_VITAGL_SRC=$SRC holds no SDL source tree"
  apply_backend_patch
  SOURCE_ID="tree:$(tree_digest "$SRC")"
else
  SOURCE_ID="tarball:$SDL2_SHA256"
fi
# receipt <check|write>: compare against, or record, identity plus payload.
receipt() {
  python3 - "$1" "$PREFIX" "$RECEIPT" "$VITAGL" "$SDL2_VER" "$SOURCE_ID" \
    "$(sdl2_patches_digest)" "$C_FLAGS" \
    "$("$VITASDK/bin/arm-vita-eabi-gcc" --version | head -1)" \
    "$(cmake --version | head -1)" "${CMAKE_OPTIONS[@]}" <<'PY'
import hashlib, json, sys
from pathlib import Path
mode, prefix, receipt = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
vitagl_digest = hashlib.sha256()
vitagl = Path(sys.argv[4])
for p in sorted([*(vitagl / "include").rglob("*"), *(vitagl / "lib").rglob("*")]):
    if p.is_file():
        vitagl_digest.update(str(p.relative_to(vitagl)).encode() + b"\0" + p.read_bytes() + b"\0")
identity = {"sdlVersion": sys.argv[5], "source": sys.argv[6], "patch": sys.argv[7],
            "cFlags": sys.argv[8], "compiler": sys.argv[9], "cmake": sys.argv[10],
            "cmakeOptions": sys.argv[11:], "vitaglHeadersAndArchives": vitagl_digest.hexdigest()}
files = {str(p.relative_to(prefix)): sha(p) for p in sorted(prefix.rglob("*"))
         if p.is_file() and p != receipt} if prefix.is_dir() else {}
record = {"identity": identity, "files": files}
if mode == "write":
    receipt.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    sys.exit(0)
try:
    sys.exit(0 if json.loads(receipt.read_text()) == record else 1)
except (OSError, ValueError):
    sys.exit(1)
PY
}
if receipt check; then
  echo "build-sdl2-vitagl: already built at $PREFIX (receipt identity and payload match)"
  exit 0
fi

if [[ -z ${SDL2_VITAGL_SRC:-} ]]; then
  echo "==> fetch SDL2-$SDL2_VER (verified) -> $SRC"
  sdl2_fetch_src "$SRC" die
  apply_backend_patch
  # The release source archive takes this tree as it is: seal it now, verify it after the build.
  python3 -B "$ROOT/vita/scripts/treedigest.py" seal "$SRC"
fi

echo "==> cmake (VIDEO_VITA_VGL=ON) -> $PREFIX"
rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD"
# SDL_GCC_ATOMICS=ON: without it SDL_spinlock.c falls back to ARM inline
# `strexeq`, which GCC 15 Thumb rejects. Never share $BUILD between parallel
# jobs.
cmake -S "$SRC" -B "$BUILD" "${CMAKE_OPTIONS[@]}" -DCMAKE_C_FLAGS="$C_FLAGS"

echo "==> make"
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake --install "$BUILD"

# A lib-prefixed stub name (-llibSceFoo) in the .pc would ask ld for an archive
# that does not exist: assert instead of trusting.
pc="$PREFIX/lib/pkgconfig/sdl2.pc"
[[ -f "$pc" ]] || die "no pkg-config file at $pc"
for file in "$pc" "$PREFIX/bin/sdl2-config" "$PREFIX"/lib/cmake/SDL2/*Targets*.cmake; do
  [[ -f "$file" ]] || continue
  leftover=$(LC_ALL=C grep -o -- '-llib[A-Za-z0-9_.+-]*' "$file" | sort -u | tr '\n' ' ' || true)
  [[ -z "$leftover" ]] || die "$file asks ld for ${leftover% }"
done
for lib in vitaGL vitashark mathneon; do
  grep -q -- "-l$lib" "$pc" || die "$pc no longer links -l$lib"
done

if [[ -z ${SDL2_VITAGL_SRC:-} ]]; then
  python3 -B "$ROOT/vita/scripts/treedigest.py" verify "$SRC" || die "the SDL2 source tree changed during the build (built in tree?)"
fi
receipt write
receipt check || die "installed payload does not match its receipt"
grep 'VITA_VGL' "$PREFIX/include/SDL2/SDL_config.h"
echo "build-sdl2-vitagl: installed $PREFIX"
