#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Canonical host build: dependencies -> pinned MRI -> engine (this tree) -> fonts -> VPK.
# Never deploys. Run one build at a time per checkout (build/ is shared).
# VITASDK, JOBS and BASERUBY may be supplied. Without BASERUBY, a matching
# native Ruby is bootstrapped under build/host-ruby-prefix on first use.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ ${1:-} == --help ]]; then
  echo "Usage: [VITASDK=...] [JOBS=...] [BASERUBY=/path/to/ruby-3.1.3] [VITAGL_TITLE_ID=MKXPZ00xx] [MKXPZ_RELEASE=1] $0"
  echo "Builds build/mkxp-z-vpk-vitagl/mkxp-z.vpk (test TITLE_ID MKXPZ0053 unless VITAGL_TITLE_ID"
  echo "is set); never deploys. MKXPZ_RELEASE=1 (set by vita/scripts/build-release.sh, use that"
  echo "instead) builds the release package under MKXPZ0001 into build/mkxp-z-vpk-release/."
  exit 0
fi
[[ $# == 0 ]] || { echo "build-player: unexpected arguments (use --help)" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?Install VitaSDK first, or set VITASDK}"
export PATH="$VITASDK/bin:$PATH"
RUBY_PIN=4d85560cf65938d7883a323bf553acad1faf5eae
RUBY_URL=https://github.com/mkxp-z/ruby.git
RUBY_SRC="$ROOT/build/ruby-src"
RUBY_BUILD="$ROOT/build/ruby-vita"
RUBY_PREFIX="$ROOT/build/ruby-vita-prefix"
HOST_PREFIX="$ROOT/build/host-ruby-prefix"

die() { echo "build-player: $*" >&2; exit 1; }

# GL backend: vitaGL is the only backend; the pinned vitaGL stack and its SDL2
# are built first. A non-release package is a test build only — the packager
# refuses the product TITLE_ID, so it defaults to the test id MKXPZ0053
# (VITAGL_TITLE_ID overrides).
VITA_GL_BACKEND="${VITA_GL_BACKEND:-vitagl}"
[[ "$VITA_GL_BACKEND" == vitagl ]] || die "VITA_GL_BACKEND must be vitagl"
# MKXPZ_RELEASE=1 is the one exception: the product id
# MKXPZ0001, packed into its own directory so it never overwrites a test VPK.
MKXPZ_RELEASE="${MKXPZ_RELEASE:-0}"
case "$MKXPZ_RELEASE" in
  0|1) ;;
  *) die "MKXPZ_RELEASE must be 0 or 1" ;;
esac
BUILD_NAME=mkxp-z-vitagl
PKG_TITLE_ID="${VITAGL_TITLE_ID:-MKXPZ0053}"
PKG_OUT="$ROOT/build/mkxp-z-vpk-vitagl"
if [[ "$MKXPZ_RELEASE" == 1 ]]; then
  PKG_TITLE_ID=MKXPZ0001
  PKG_OUT="$ROOT/build/mkxp-z-vpk-release"
fi

for tool in git cmake meson ninja python3 curl pkg-config arm-vita-eabi-gcc; do
  command -v "$tool" >/dev/null || die "missing host/toolchain command: $tool"
done

# Builders reset sources and remove outputs. Refuse a symlinked build tree
# instead of cleaning another checkout.
for relative in build build/ruby-src build/ruby-vita build/ruby-vita-prefix \
  build/host-ruby-src build/host-ruby-prefix \
  build/sdl2-src build/vita-deps build/fonts \
  build/mkxp-z-vitagl build/mkxp-z-vpk-vitagl build/mkxp-z-vpk-release build/vitagl-src \
  build/vitagl-build build/vitagl-prefix build/sdl2-vitagl-src \
  build/sdl2-vitagl-build build/sdl2-vitagl-prefix; do
  [[ ! -L "$ROOT/$relative" ]] || die "refusing shared/symlinked build path: $ROOT/$relative"
done

"$ROOT/vita/scripts/build-vitagl.sh"
"$ROOT/vita/scripts/build-sdl2-vitagl.sh"
"$ROOT/vita/scripts/build-vita-deps.sh"
# TinySoundFont (MIDI): pinned, digest-verified headers; a SoundFont is never bundled.
"$ROOT/vita/scripts/fetch-tinysoundfont.sh"

if [[ -z ${BASERUBY:-} ]]; then
  BASERUBY="$HOST_PREFIX/bin/ruby"
  if [[ ! -x "$BASERUBY" ]] ||
     ! grep -qx "ref=$RUBY_PIN" "$HOST_PREFIX/mkxpz-host-ruby-buildinfo.txt" 2>/dev/null; then
    # Separate host source: build-host-ruby configures in-tree and cleans it.
    MKXPZ_HOST_RUBY_SRC="$ROOT/build/host-ruby-src" MKXPZ_HOST_RUBY_PREFIX="$HOST_PREFIX" \
      RUBY_REPO="$RUBY_URL" RUBY_REF="$RUBY_PIN" RUBY_BRANCH=mkxp-z-3.1.3 \
      "$ROOT/vita/scripts/build-host-ruby.sh"
  fi
fi
[[ -x "$BASERUBY" ]] || die "BASERUBY is not executable: $BASERUBY"
[[ "$("$BASERUBY" -e 'print RUBY_VERSION')" == 3.1.3 ]] || die "BASERUBY must be Ruby 3.1.3"
BASERUBY="$(cd "$(dirname "$BASERUBY")" && pwd)/$(basename "$BASERUBY")"
export BASERUBY

mkdir -p "$ROOT/build"
if [[ ! -d "$RUBY_SRC/.git" ]]; then
  [[ ! -e "$RUBY_SRC" ]] || die "$RUBY_SRC exists but is not a managed git clone"
  git clone --quiet --no-checkout "$RUBY_URL" "$RUBY_SRC"
fi
if ! git -C "$RUBY_SRC" cat-file -e "$RUBY_PIN^{commit}" 2>/dev/null; then
  git -C "$RUBY_SRC" fetch --quiet origin "$RUBY_PIN"
fi
# This ignored checkout is regenerated from vita/patches/ruby, never hand-edited.
git -C "$RUBY_SRC" checkout --quiet --force --detach "$RUBY_PIN"
git -C "$RUBY_SRC" clean -fdx --quiet
[[ "$(git -C "$RUBY_SRC" rev-parse HEAD)" == "$RUBY_PIN" ]] || die "Ruby source pin mismatch"

# A changed patch, compiler or config must not inherit an old archive or objects.
# Dependencies retain their own builder caches; MRI is deliberately rebuilt.
rm -rf "$RUBY_BUILD" "$RUBY_PREFIX"
MKXPZ_RUBY_SRC="$RUBY_SRC" MKXPZ_RUBY_BUILD="$RUBY_BUILD" MKXPZ_RUBY_PREFIX="$RUBY_PREFIX" \
  "$ROOT/vita/scripts/build-ruby-vita.sh" build
RUBY_PREFIX="$RUBY_PREFIX" MRI_INCLUDES="$RUBY_PREFIX/include/ruby-3.1.0" \
  MRI_LIBPATH="$RUBY_PREFIX/lib" MRI_LIBRARY=ruby-static \
  BUILD_DIR="$ROOT/build/$BUILD_NAME" \
  VITA_GL_BACKEND="$VITA_GL_BACKEND" \
  "$ROOT/vita/scripts/configure-vita.sh" reconfigure
# Packaging requires the fonts (Liberation comes from assets/liberation.ttf in
# this tree), so fetch before the long engine compile.
FONTS_PREFIX="$ROOT/build/fonts" "$ROOT/vita/scripts/fetch-fonts.sh"
ninja -C "$ROOT/build/$BUILD_NAME"
ELF="$ROOT/build/$BUILD_NAME/mkxp-z.cortex-a9"
[[ -f "$ELF" ]] || die "product ELF missing: $ELF"
# The package is pinned to the test id (or the product id for a release);
# ${PKG_TITLE_ID:+TITLE_ID=...} cannot do this: an expansion result is a word,
# never an assignment, so the line would run "TITLE_ID=..." as a command.
export TITLE_ID=$PKG_TITLE_ID
RUBY_PREFIX="$RUBY_PREFIX" MKXPZ_ELF="$ELF" FONTS_PREFIX="$ROOT/build/fonts" REQUIRE_BUILD_RECEIPT=1 \
  VITA_GL_BACKEND="$VITA_GL_BACKEND" MKXPZ_RELEASE="$MKXPZ_RELEASE" \
  "$ROOT/vita/scripts/package-vpk.sh"
echo "build-player: built $PKG_OUT/mkxp-z.vpk (backend=$VITA_GL_BACKEND title=$PKG_TITLE_ID)"
