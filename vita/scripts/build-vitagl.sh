#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build-vitagl.sh — cross-build the pinned vitaGL stack for the vitagl backend.
#
#   vita/scripts/build-vitagl.sh              # fetch + build all five
#   vita/scripts/build-vitagl.sh vitagl       # one of: vitagl vitashark shacccgext mathneon taihen
#   vita/scripts/build-vitagl.sh status       # what is installed in the side prefix
#   vita/scripts/build-vitagl.sh clean        # wipe build/vitagl-src, -build and -prefix
#
# Pins live in vita/scripts/vitagl-pins.json (name, repo, commit, licence); every
# fetch is by full commit — branch or tag names are rejected before any
# network traffic. Build fixes for a pin live in vita/patches/vitagl/<name>-*.patch
# and are applied (and re-stripped) by the managed fetch; a file replaced whole
# lives under vita/patches/vitagl/<name>-files/<path in the pinned tree>. Outputs (gitignored)
# never touch the VitaSDK tree:
#   build/vitagl-prefix/{lib,include,share}   archives + headers + pin receipt
#
# Libraries and licences: vitaGL/vitaShaRK LGPL-3.0, SceShaccCgExt GPL-3.0
# (which makes the linked player GPL-3.0; see THIRD-PARTY.md), math-neon and
# taiHEN MIT. taiHEN is built only for its import
# stubs (libtaihen_stub.a); the kernel module itself is never installed.
#
# Flags: VITAGL_KNOBS (NO_SPLASHSCREEN=1 LOG_ERRORS=1 HAVE_SHADER_CACHE=1 HAVE_SHARK_LOG=1
# SINGLE_THREADED_GC=1) are vitaGL Makefile knobs (this pin has no HAVE_GLSL_SUPPORT
# knob; GLSL custom shaders are unconditional). SINGLE_THREADED_GC is required by
# vitagl-0007: the asynchronous collector races the render thread on the purge lists.
# Deterministic archives: every ar invocation passes -D.
#
# Host-only. Produces arm-vita-eabi static archives; no Vita link or boot.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?VITASDK not found; source vita/scripts/vita-env.sh or install to \$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

VITAGL_PIN_FILE="$ROOT/vita/scripts/vitagl-pins.json"
VITAGL_SRC="$ROOT/build/vitagl-src"
VITAGL_BDIR="$ROOT/build/vitagl-build"
VITAGL_PREFIX="$ROOT/build/vitagl-prefix"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

die() { echo "error: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null || die "missing host tool: $1"; }

# One flags line for every build: -I into the side prefix, where the headers
# of earlier libraries in the chain land (math_neon.h, vitashark.h,
# shacccg_ext.h, taihen.h), plus the -ffile-prefix-map policy of
# build-vita-deps.sh: the DWARF the engine later
# inherits must carry no machine paths.
map_flags() {
  local maps="" from to
  for from in "${HOME:-}" "$ROOT"; do
    [[ -z "$from" || "$from" == "/" ]] && continue
    case "$from" in *[[:space:]]*) continue ;; esac
    case "$from" in
      "$HOME") to=/mkxpz-home ;;
      *)       to=/mkxp-z-vita ;;
    esac
    maps="$maps -ffile-prefix-map=$from=$to"
  done
  echo "-I$VITAGL_PREFIX/include$maps"
}
EXTRA_CFLAGS="$(map_flags)"

VGL_AR="arm-vita-eabi-gcc-ar -D"

# vitaGL Makefile knobs. vita/scripts/vitagl-shaders.py records this line in the shader MANIFEST.
VITAGL_KNOBS="NO_SPLASHSCREEN=1 LOG_ERRORS=1 HAVE_SHADER_CACHE=1 HAVE_SHARK_LOG=1 SINGLE_THREADED_GC=1"

pin() {
  python3 -c '
import json, sys
with open(sys.argv[1]) as handle:
    pins = json.load(handle)
for entry in pins:
    if entry.get("name") == sys.argv[2]:
        print(entry[sys.argv[3]])
        break
else:
    sys.exit("no pin named %s in %s" % (sys.argv[2], sys.argv[1]))
' "$VITAGL_PIN_FILE" "$1" "$2"
}

check_pins() {
  [[ -f "$VITAGL_PIN_FILE" ]] || die "missing pin file: $VITAGL_PIN_FILE"
  python3 -c '
import json, re, sys
with open(sys.argv[1]) as handle:
    pins = json.load(handle)
names = [e.get("name") for e in pins]
assert names == ["vitagl", "vitashark", "shacccgext", "mathneon", "taihen"], names
for entry in pins:
    for key in ("name", "repo", "commit", "licence"):
        assert entry.get(key), "%s: missing %s" % (entry.get("name"), key)
    assert re.fullmatch(r"[0-9a-f]{40}", entry["commit"]), entry
' "$VITAGL_PIN_FILE" || die "pin file failed its shape check"
}

# Managed checkout of one pin. Reads the pin file; refuses branch/tag pins and
# tracked-tree edits; verified detach at the exact commit. The per-pin patch
# series in vita/patches/vitagl/ (vitagl-0001 oom-error-contract,
# taihen-0001 modern-sdk-compat) is stripped before the edit guard and
# reapplied after checkout, so a patched tree from a previous run stays
# managed and idempotent.
apply_patch_series() { # name, dir, reverse|apply
  local name=$1 dir=$2 mode=$3 patch patches=()
  for patch in "$ROOT/vita/patches/vitagl/$name"-*.patch; do
    [[ -e "$patch" ]] || return 0
    patches+=("$patch")
  done
  # Strip last-first: a later patch may edit lines an earlier one added.
  if [[ $mode == reverse ]]; then
    local i reversed=()
    for ((i = ${#patches[@]} - 1; i >= 0; i--)); do reversed+=("${patches[i]}"); done
    patches=(${reversed[@]+"${reversed[@]}"})
  fi
  for patch in ${patches[@]+"${patches[@]}"}; do
    case "$mode" in
      reverse)
        if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
          git -C "$dir" apply --reverse --quiet "$patch"
        fi ;;
      apply)
        if git -C "$dir" apply --check "$patch" 2>/dev/null; then
          git -C "$dir" apply "$patch"
        else
          git -C "$dir" apply --reverse --check "$patch" 2>/dev/null ||
            die "$name: patch $(basename "$patch") neither applies nor is applied"
        fi ;;
    esac
  done
}

# Whole-file replacements for a pin: vita/patches/vitagl/<name>-files/<path> is
# copied over <path> in the pinned tree after the patches, and restored from
# the index before the edit guard, so a tree from a previous run stays managed.
# A replaced file must not also be touched by a patch of the same pin.
apply_file_overlay() { # name, dir, restore|apply
  local name=$1 dir=$2 mode=$3 root="$ROOT/vita/patches/vitagl/$1-files" file rel
  [[ -d "$root" ]] || return 0
  while IFS= read -r file; do
    rel=${file#"$root"/}
    case "$mode" in
      restore)
        if [[ -f "$dir/$rel" ]] && ! git -C "$dir" diff --quiet -- "$rel" 2>/dev/null; then
          git -C "$dir" checkout --quiet -- "$rel"
        fi ;;
      apply)
        mkdir -p "$(dirname "$dir/$rel")"
        cp -f "$file" "$dir/$rel" ;;
    esac
  done < <(find "$root" -type f | sort)
}

fetch_repo() {
  local name=$1 dir="$VITAGL_SRC/$1" repo commit
  repo="$(pin "$name" repo)"
  commit="$(pin "$name" commit)"
  [[ "$commit" =~ ^[0-9a-f]{40}$ ]] ||
    die "pin '$name' is not a 40-hex commit (branches and tags are not pins): $commit"
  if [[ ! -d "$dir/.git" ]]; then
    echo "==> fetching $name at ${commit:0:12}"
    mkdir -p "$VITAGL_SRC"
    git clone --quiet --no-checkout "$repo" "$dir"
  fi
  if ! git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
    git -C "$dir" fetch --quiet origin "$commit"
  fi
  apply_patch_series "$name" "$dir" reverse
  apply_file_overlay "$name" "$dir" restore
  if [[ -f "$(git -C "$dir" rev-parse --absolute-git-dir)/index" ]]; then
    git -C "$dir" diff --quiet && git -C "$dir" diff --cached --quiet ||
      die "$name has tracked edits; restore the managed source before building"
  fi
  git -C "$dir" checkout --quiet --detach "$commit"
  [[ "$(git -C "$dir" rev-parse HEAD)" == "$commit" ]] || die "$name source pin mismatch"
  # Submodule pointers (taiHEN: substitute, taihen-parser) are recorded by the
  # pinned commit itself, so this reproduces the pinned state exactly.
  git -C "$dir" submodule update --init --quiet --recursive
  apply_patch_series "$name" "$dir" apply
  apply_file_overlay "$name" "$dir" apply
}

provenance() { # archive, name
  echo "provenance: $1 <- $(pin "$2" repo)@$(pin "$2" commit) ($(pin "$2" licence))"
}

# Re-archive without member timestamps: vita-libs-gen's generated
# Makefile offers no -D hook, so the taiHEN stubs are repacked here. Weak-stub
# members end in .wo, strong ones in .o; every member is repacked.
determinize_archive() {
  local archive=$1 tmp
  tmp="$(mktemp -d)"
  (
    cd "$tmp"
    arm-vita-eabi-ar -x "$archive"
    rm -f "$archive"
    arm-vita-eabi-ar -D -rcs "$archive" ./*
  )
  rm -rf "$tmp"
}

# taiHEN: the kernel module is a dependency of its own stub generation;
# cmake --install drops libtaihen_stub{,_weak}.a and taihen.h into the side
# prefix. The .skprx target is never built.
build_taihen() {
  need git; need cmake; need make
  fetch_repo taihen
  # GCC 14 made uintptr_t->pointer int-conversion an error; the pinned
  # taiHEN user wrappers pass 32-bit user addresses by value. The module is
  # never run, so the class is downgraded to a warning instead of casting
  # dozens of call sites in the patch series.
  cmake -S "$VITAGL_SRC/taihen" -B "$VITAGL_BDIR/taihen" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$VITAGL_PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_FLAGS="-Wno-error=int-conversion" \
    > "$VITAGL_BDIR/taihen-config.log" 2>&1 \
    || { tail -40 "$VITAGL_BDIR/taihen-config.log" >&2; die "taihen cmake configure failed"; }
  cmake --build "$VITAGL_BDIR/taihen" --target taihen-stubs -j"$JOBS" \
    > "$VITAGL_BDIR/taihen-build.log" 2>&1 \
    || { tail -40 "$VITAGL_BDIR/taihen-build.log" >&2; die "taihen stub build failed"; }
  cmake --install "$VITAGL_BDIR/taihen" > "$VITAGL_BDIR/taihen-install.log" 2>&1 \
    || { tail -20 "$VITAGL_BDIR/taihen-install.log" >&2; die "taihen install failed"; }
  local lib
  for lib in "$VITAGL_PREFIX"/lib/libtaihen*.a; do
    determinize_archive "$lib"
  done
  provenance libtaihen_stub.a taihen
}

build_mathneon() {
  need git; need make
  fetch_repo mathneon
  # CFLAGS is duplicated verbatim from the pinned Makefile (a command-line
  # CFLAGS would suppress the Makefile's own assignments) plus EXTRA_CFLAGS.
  make -C "$VITAGL_SRC/mathneon" -j"$JOBS" AR="$VGL_AR" \
    CFLAGS="-g -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize $EXTRA_CFLAGS"
  mkdir -p "$VITAGL_PREFIX/lib" "$VITAGL_PREFIX/include"
  cp -f "$VITAGL_SRC/mathneon/libmathneon.a" "$VITAGL_PREFIX/lib/"
  cp -f "$VITAGL_SRC/mathneon/source/math_neon.h" "$VITAGL_PREFIX/include/"
  provenance libmathneon.a mathneon
}

build_shacccgext() {
  need git; need cmake
  fetch_repo shacccgext
  rm -rf "$VITAGL_BDIR/shacccgext" # cmake cannot change flags in place
  # -std=gnu11: GCC 15 defaults to C23, where () in a function-pointer type
  # means (void), which breaks taihen.h's TAI_CONTINUE for hooked calls with
  # arguments. This pin predates that; its own C++ side pins gnu++11.
  cmake -S "$VITAGL_SRC/shacccgext" -B "$VITAGL_BDIR/shacccgext" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$VITAGL_PREFIX" \
    -DCMAKE_C_FLAGS="$EXTRA_CFLAGS -std=gnu11" -DCMAKE_CXX_FLAGS="$EXTRA_CFLAGS" \
    -DCMAKE_C_ARCHIVE_CREATE="<CMAKE_AR> -D qc <TARGET> <LINK_FLAGS> <OBJECTS>" \
    -DCMAKE_CXX_ARCHIVE_CREATE="<CMAKE_AR> -D qc <TARGET> <LINK_FLAGS> <OBJECTS>" \
    -DCMAKE_C_ARCHIVE_FINISH="<CMAKE_RANLIB> -D <TARGET>" \
    -DCMAKE_CXX_ARCHIVE_FINISH="<CMAKE_RANLIB> -D <TARGET>" \
    > "$VITAGL_BDIR/shacccgext-config.log" 2>&1 \
    || { tail -40 "$VITAGL_BDIR/shacccgext-config.log" >&2; die "shacccgext cmake configure failed"; }
  cmake --build "$VITAGL_BDIR/shacccgext" -j"$JOBS" \
    > "$VITAGL_BDIR/shacccgext-build.log" 2>&1 \
    || { tail -40 "$VITAGL_BDIR/shacccgext-build.log" >&2; die "shacccgext build failed"; }
  cmake --install "$VITAGL_BDIR/shacccgext" > "$VITAGL_BDIR/shacccgext-install.log" 2>&1 \
    || { tail -20 "$VITAGL_BDIR/shacccgext-install.log" >&2; die "shacccgext install failed"; }
  provenance libSceShaccCgExt.a shacccgext
}

build_vitashark() {
  need git; need make
  fetch_repo vitashark
  make -C "$VITAGL_SRC/vitashark" -j"$JOBS" AR="$VGL_AR" \
    CFLAGS="-g -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize $EXTRA_CFLAGS"
  mkdir -p "$VITAGL_PREFIX/lib" "$VITAGL_PREFIX/include"
  cp -f "$VITAGL_SRC/vitashark/libvitashark.a" "$VITAGL_PREFIX/lib/"
  cp -f "$VITAGL_SRC/vitashark/source/vitashark.h" "$VITAGL_PREFIX/include/"
  provenance libvitashark.a vitashark
}

# vitaGL's Makefile tracks neither headers nor the command line, so a changed pin, patch (a header-only
# patch touches no .c), knob, path-map flag or compiler leaves objects built from the old inputs in the
# archive. The fingerprint of those inputs is stamped after a good build; a mismatch wipes every object.
vitagl_wipe_stale_objects() {
  local stamp file="$VITAGL_BDIR/vitagl.stamp" patch name
  stamp="$({
    # vitaGL includes the headers of the libraries built before it (math_neon.h, vitashark.h, ...).
    for name in vitagl vitashark shacccgext mathneon; do pin "$name" commit; done
    echo "$VITAGL_KNOBS"
    echo "$EXTRA_CFLAGS"
    arm-vita-eabi-gcc -dumpfullversion 2>/dev/null || echo unknown-compiler
    for patch in "$ROOT"/vita/patches/vitagl/vitagl-*.patch; do
      [[ -e "$patch" ]] || continue
      basename "$patch"
      cat "$patch"
    done
  } | python3 -c 'import hashlib, sys; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())')"
  if [[ "$(cat "$file" 2>/dev/null || true)" != "$stamp" ]]; then
    echo "==> vitaGL build inputs changed: rebuilding every object"
    rm -f "$file"
    find "$VITAGL_SRC/vitagl" \( -name '*.o' -o -name 'libvitaGL.a' -o -name 'libvitaGL.elf' \) -exec rm -f {} + 2>/dev/null || true
  fi
  VITAGL_STAMP="$stamp"
}

build_vitagl() {
  need git; need make
  fetch_repo vitagl
  vitagl_wipe_stale_objects
  # The Makefile knobs stay upstream; the side-prefix include and path maps
  # ride on CC/CXX because a command-line CFLAGS would kill the knob appends.
  # VGL_GIT_HASH still resolves from the detached pin via the Makefile's
  # $(shell git rev-parse).
  make -C "$VITAGL_SRC/vitagl" -j"$JOBS" \
    $VITAGL_KNOBS \
    AR="$VGL_AR" \
    CC="arm-vita-eabi-gcc $EXTRA_CFLAGS" \
    CXX="arm-vita-eabi-g++ $EXTRA_CFLAGS"
  mkdir -p "$VITAGL_PREFIX/lib" "$VITAGL_PREFIX/include"
  cp -f "$VITAGL_SRC/vitagl/libvitaGL.a" "$VITAGL_PREFIX/lib/"
  cp -f "$VITAGL_SRC/vitagl/source/vitaGL.h" "$VITAGL_PREFIX/include/"
  printf '%s\n' "$VITAGL_STAMP" > "$VITAGL_BDIR/vitagl.stamp"
  provenance libvitaGL.a vitagl
}

status() {
  echo "PREFIX=$VITAGL_PREFIX"
  local p
  for p in libvitaGL.a libvitashark.a libSceShaccCgExt.a libmathneon.a libtaihen_stub.a; do
    if [[ -f "$VITAGL_PREFIX/lib/$p" ]]; then
      echo "  ok      lib/$p  $(wc -c < "$VITAGL_PREFIX/lib/$p" | tr -d ' ') bytes"
    else
      echo "  MISSING lib/$p"
    fi
  done
  for p in vitaGL.h vitashark.h shacccg_ext.h math_neon.h taihen.h; do
    if [[ -f "$VITAGL_PREFIX/include/$p" ]]; then
      echo "  ok      include/$p"
    else
      echo "  MISSING include/$p"
    fi
  done
}

main() {
  need arm-vita-eabi-gcc
  check_pins
  mkdir -p "$VITAGL_BDIR" "$VITAGL_PREFIX/lib" "$VITAGL_PREFIX/include" "$VITAGL_PREFIX/share"
  build_taihen
  build_mathneon
  build_shacccgext
  build_vitashark
  build_vitagl
  cp -f "$VITAGL_PIN_FILE" "$VITAGL_PREFIX/share/vitagl-pins.json"
  echo
  status
}

MODE="${1:-all}"
case "$MODE" in
  all) main ;;
  vitagl|vitashark|shacccgext|mathneon|taihen)
    need arm-vita-eabi-gcc
    check_pins
    mkdir -p "$VITAGL_BDIR" "$VITAGL_PREFIX/lib" "$VITAGL_PREFIX/include"
    "build_$MODE"
    ;;
  status) status ;;
  clean) echo "==> removing vitaGL build trees"; rm -rf "$VITAGL_SRC" "$VITAGL_BDIR" "$VITAGL_PREFIX" ;;
  *) die "unknown mode: $MODE (all|vitagl|vitashark|shacccgext|mathneon|taihen|status|clean)" ;;
esac
