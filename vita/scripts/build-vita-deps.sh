#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build-vita-deps.sh — cross-build mkxp-z hard deps missing from vdpm.
#
#   vita/scripts/build-vita-deps.sh              # fetch + build all (theora uchardet sdl2_sound pixman openal)
#   vita/scripts/build-vita-deps.sh theora       # one dep
#   vita/scripts/build-vita-deps.sh uchardet
#   vita/scripts/build-vita-deps.sh sdl2_sound
#   vita/scripts/build-vita-deps.sh pixman
#   vita/scripts/build-vita-deps.sh openal
#   vita/scripts/build-vita-deps.sh pthread-embedded   # fetch the pinned source only (release source archive)
#   vita/scripts/build-vita-deps.sh vdpm-sources       # fetch the pinned sources and recipes of the vdpm libraries the player links
#   vita/scripts/build-vita-deps.sh wrapper      # (re)write pkg-config wrapper only
#   vita/scripts/build-vita-deps.sh status       # what is installed in the side prefix
#   vita/scripts/build-vita-deps.sh clean        # wipe build/vita-deps (sources + prefix)
#
# Outputs (gitignored):
#   build/vita-deps/prefix/{lib,include,share}   static libs + headers + .pc
#   build/vita-deps/bin/arm-vita-eabi-pkg-config-deps
#       wrapper that prepends prefix/lib/pkgconfig to PKG_CONFIG_LIBDIR
#       so meson's arm-vita-eabi-pkg-config (which clears PKG_CONFIG_PATH
#       and pins the sysroot) can see our .pc files.
#
# Why a side prefix (not sysroot install):
#   - Keeps $VITASDK/arm-vita-eabi a pure vdpm tree (easy to wipe/upgrade).
#   - configure-vita.sh injects -L/-I via a generated cross file and
#     points meson at the wrapper above; nothing is copied into VitaSDK.
#
# Pins (match mkxp-z linux/Makefile where possible):
#   The downloads this script makes are pinned in vita/scripts/dep-pins.json (SHA-256 for
#   tarballs, a full commit for git sources) and refused when they do not match; the other
#   pins are listed in THIRD-PARTY.md. A fetched tarball tree is sealed with the identity of
#   what it was made from (URL, version, tarball digest, every patch and text transformation)
#   and its content digest. It is reused only while that identity equals the current pins
#   (otherwise it is extracted again) and is re-verified before every build and before
#   archiving, so a tree edited or built in place is rejected; nothing is built inside a
#   source tree.
#   theora     libtheora-1.1.1 tarball (pregenerated configure; no automake needed)
#   uchardet   freedesktop/uchardet v0.0.8, checked against its pinned commit
#   SDL_sound  mkxp-z/SDL_sound @ cfb2533eb3bac3700015cbd87cc623bea1467239
#   pthread-embedded  fetched for the source archive only (the VitaSDK libpthread is linked
#              from the toolchain, not built here)
#   vdpm-sources  the package files, upstream sources and vitasdk/packages recipes of the
#              statically linked vdpm libraries (FreeType, SDL2_image, SDL2_ttf, libpng, zlib,
#              bzip2, libwebp, libogg, libvorbis, PhysFS, SDL2): fetched for the source archive
#              only, each pinned by SHA-256 or commit in dep-pins.json
#   pixman     0.42.2 release tarball — the exact Version in the vdpm sysroot's
#              pixman-1.pc (the vdpm recipe itself is not recoverable offline).
#              The sysroot archive is a debug-goal build ("Aggressive Debug"),
#              and without function/data sections the link pulls the whole
#              1.6 MB accessor table even under MKXPZ_SOFTWARE_BITMAPS, which
#              reaches only pixman's region code. This stage rebuilds it
#              -O2 with function/data sections into the side prefix, which
#              every engine pkg-config path searches before the sysroot;
#              ARM SIMD + NEON assembly stay enabled as in the sysroot archive.
#   openal     1.19.1 release tarball + isage's 1.19.1-vita-1 patch — the
#              exact recipe vitasdk's packages repo uses for the vdpm sysroot
#              archive. Rebuilt here for one substitution: upstream SZFMT is
#              "%zu" on non-Windows, and Vita newlib printf neither honours
#              %zu nor consumes its size_t argument, so any OpenAL log line
#              that prints a size crashes on the next conversion (a device data abort in
#              _vfprintf_r). SZFMT becomes "%u"; size_t is 32 bits on arm-vita-eabi. Backends and NEON
#              mirror the sysroot archive: vita/null/wave/loopback, mixer
#              NEON on, SDL2 backend off.
#
# Host-only. Produces arm-vita-eabi static archives; no Vita link or boot.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?VITASDK not found; source vita/scripts/vita-env.sh or install to \$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

DEPS_ROOT="$ROOT/build/vita-deps"
SRC="$DEPS_ROOT/src"
BDIR="$DEPS_ROOT/build"
PREFIX="$DEPS_ROOT/prefix"
BIN="$DEPS_ROOT/bin"
SYSROOT="$VITASDK/arm-vita-eabi"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

DIST="$DEPS_ROOT/dist"
THEORA_DIR="$SRC/theora"
UCHARDET_DIR="$SRC/uchardet"
PTHREAD_DIR="$SRC/pthread-embedded"
SDLSOUND_URL="https://github.com/mkxp-z/SDL_sound"
SDLSOUND_PIN="cfb2533eb3bac3700015cbd87cc623bea1467239"
SDLSOUND_DIR="$SRC/sdl_sound"
PIXMAN_DIR="$SRC/pixman"

die() { echo "error: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null || die "missing host tool: $1"; }

# shellcheck source=/dev/null
. "$ROOT/vita/scripts/dep-pins.sh"
THEORA_VERSION=$(dep_pin theora version)
THEORA_URL=$(dep_pin theora url)
THEORA_SHA256=$(dep_pin theora sha256)
UCHARDET_URL=$(dep_pin uchardet repo)
UCHARDET_TAG=$(dep_pin uchardet tag)
UCHARDET_COMMIT=$(dep_pin uchardet commit)
PIXMAN_VERSION=$(dep_pin pixman version)
PIXMAN_URL=$(dep_pin pixman url)
PIXMAN_SHA256=$(dep_pin pixman sha256)
OPENAL_VERSION=$(dep_pin openal version)
OPENAL_URL=$(dep_pin openal url)
OPENAL_SHA256=$(dep_pin openal sha256)
OPENAL_VITA_PATCH_URL=$(dep_pin openal patchUrl)
OPENAL_VITA_PATCH_SHA256=$(dep_pin openal patchSha256)
OPENAL_DIR="$SRC/openal-soft-openal-soft-$OPENAL_VERSION"
PTHREAD_URL=$(dep_pin pthreadEmbedded repo)
PTHREAD_COMMIT=$(dep_pin pthreadEmbedded commit)
PIXMAN_ASM_SED=$(dep_pin transforms pixmanArmAsmSed)
OPENAL_SZFMT_SED=$(dep_pin transforms openalSzfmtSed)
VDPM_RECIPES_URL=$(dep_pin vdpm recipes repo)
VDPM_RECIPES_COMMIT=$(dep_pin vdpm recipes commit)
VDPM_RECIPES_DIR="$SRC/vitasdk-packages"
LIBPNG_URL=$(dep_pin vdpm packages libpng git repo)
LIBPNG_COMMIT=$(dep_pin vdpm packages libpng git commit)
LIBPNG_DIR="$SRC/libpng"

# verify_git_checkout <dir> <commit>: the managed checkout is at the pinned commit with no tracked edits.
verify_git_checkout() {
  local dir=$1 commit=$2
  [[ "$(git -C "$dir" rev-parse HEAD)" == "$commit" ]] || die "$dir is not at pinned commit $commit; remove it and rebuild"
  git -C "$dir" diff --quiet && git -C "$dir" diff --cached --quiet ||
    die "$dir has tracked edits; remove it and rebuild"
}

# Vita compile flags shared with vita/meson/vita-cross.ini.
VITA_CPU_FLAGS=(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)

# ---------------------------------------------------------------------------
# pkg-config wrapper
#
# arm-vita-eabi-pkg-config (VitaSDK) does:
#   PKG_CONFIG_PATH=  PKG_CONFIG_LIBDIR=$VITASDK/arm-vita-eabi/{lib,share}/pkgconfig
#   pkg-config --define-prefix --static
# so a side-prefix .pc is invisible unless we put it on PKG_CONFIG_LIBDIR.
# --define-prefix recomputes prefix from the .pc location, which is correct
# for both our side prefix and the vdpm sysroot tree.
# ---------------------------------------------------------------------------
write_wrapper() {
  mkdir -p "$BIN"
  local wrap="$BIN/arm-vita-eabi-pkg-config-deps"
  cat > "$wrap" << EOF
#!/usr/bin/env bash
# Generated by vita/scripts/build-vita-deps.sh — do not edit.
# arm-vita-eabi-pkg-config plus the vita-deps side prefix.
# Used by vita/scripts/configure-vita.sh via the generated meson cross file.
export VITASDK="\${VITASDK:-$VITASDK}"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:\$VITASDK/arm-vita-eabi/lib/pkgconfig:\$VITASDK/arm-vita-eabi/share/pkgconfig"
export PKG_CONFIG_PATH=
export PKG_CONFIG_DIR=
export PKG_CONFIG_SYSROOT_DIR=
exec pkg-config --define-variable=VITASDK="\$VITASDK" --define-prefix --static "\$@"
EOF
  chmod +x "$wrap"
  echo "==> wrapper: $wrap"
}

# SDL2_sound: mkxp-z uses find_library('SDL2_sound'), not pkg-config, but we
# still emit a .pc for preflight humans and future meson dependency() use.
write_sdlsound_pc() {
  mkdir -p "$PREFIX/lib/pkgconfig"
  cat > "$PREFIX/lib/pkgconfig/SDL2_sound.pc" << EOF
# Generated by vita/scripts/build-vita-deps.sh
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: SDL2_sound
Description: SDL2_sound (mkxp-z fork) — abstract sound decoding for SDL2
Version: 2.0.1
Requires: sdl2
Libs: -L\${libdir} -lSDL2_sound
Cflags: -I\${includedir} -I\${includedir}/SDL2
EOF
  echo "==> wrote $PREFIX/lib/pkgconfig/SDL2_sound.pc"
}

# mkxp-z does #include <SDL_sound.h> (flat). Upstream installs to include/SDL2/.
flatten_sdlsound_header() {
  if [[ -f "$PREFIX/include/SDL2/SDL_sound.h" ]]; then
    cp -f "$PREFIX/include/SDL2/SDL_sound.h" "$PREFIX/include/SDL_sound.h"
    echo "==> flattened SDL_sound.h -> $PREFIX/include/SDL_sound.h"
  fi
}

fetch_theora() {
  local tar="$DIST/libtheora-$THEORA_VERSION.tar.gz"
  need curl
  fetch_verified "$THEORA_URL" "$THEORA_SHA256" "$tar"
  if tree_reusable "$THEORA_DIR" theora && [[ -x "$THEORA_DIR/configure" ]]; then
    return 0
  fi
  echo "==> unpacking theora $THEORA_VERSION (sha256 verified)"
  mkdir -p "$SRC"
  rm -rf "$SRC/libtheora-$THEORA_VERSION" "$THEORA_DIR"
  tar -C "$SRC" -xzf "$tar"
  mv "$SRC/libtheora-$THEORA_VERSION" "$THEORA_DIR"
  seal_tree "$THEORA_DIR" theora
}

# One -ffile-prefix-map per local root, general prefix first (GCC applies the last match): DWARF and
# __FILE__ carry no build-machine path. Whitespace paths are skipped rather than split into broken flags.
deps_path_maps() {
  local lines="" pair from to
  for pair in "${HOME:-}|/mkxpz-home" "$ROOT|/mkxp-z-vita" "$DEPS_ROOT|/vita-deps"; do
    from=${pair%%|*}
    to=${pair#*|}
    [[ -n "$from" && "$from" != "/" ]] || continue
    case "$from" in *[[:space:]]*) continue ;; esac
    lines="$lines${#from} -ffile-prefix-map=$from=$to
"
  done
  printf '%s' "$lines" | sort -u | sort -n -k1,1 | cut -d' ' -f2-
}

build_theora() {
  need curl
  fetch_theora
  # Out of tree: configure and make never write into the pristine, sealed source
  # (their config.log, config.status and Makefiles carry the builder's paths).
  rm -rf "$BDIR/theora"
  mkdir -p "$BDIR/theora"
  echo "==> configure theora $THEORA_VERSION -> $PREFIX"
  (
    cd "$BDIR/theora"
    CC=arm-vita-eabi-gcc \
    CPPFLAGS="-I$SYSROOT/include $(deps_path_maps | tr '\n' ' ')" \
    LDFLAGS="-L$SYSROOT/lib" \
    "$THEORA_DIR/configure" --host=arm-vita-eabi \
      --prefix="$PREFIX" \
      --disable-shared --enable-static \
      --disable-examples --disable-oggtest --disable-asm --disable-spec \
      --with-ogg="$SYSROOT" \
      > "$BDIR/theora/configure.log" 2>&1 \
      || { tail -40 "$BDIR/theora/configure.log" >&2; die "theora configure failed (log: $BDIR/theora/configure.log)"; }
    make -j"$JOBS" > "$BDIR/theora/make.log" 2>&1 \
      || { tail -40 "$BDIR/theora/make.log" >&2; die "theora make failed (log: $BDIR/theora/make.log)"; }
    make install > "$BDIR/theora/install.log" 2>&1 \
      || { tail -20 "$BDIR/theora/install.log" >&2; die "theora install failed"; }
  )
  verify_tree "$THEORA_DIR" theora
  # Drop theora docs from the prefix (large, unused).
  rm -rf "$PREFIX/share/doc/libtheora-"*
  [[ -f "$PREFIX/lib/libtheora.a" ]] || die "theora: libtheora.a missing after install"
  [[ -f "$PREFIX/lib/pkgconfig/theora.pc" ]] || die "theora: theora.pc missing after install"
  echo "    libtheora.a  theora.pc  theoradec.pc  theoraenc.pc"
}

fetch_uchardet() {
  if [[ ! -d "$UCHARDET_DIR/.git" ]]; then
    echo "==> fetching uchardet $UCHARDET_TAG"
    mkdir -p "$SRC"
    git clone --quiet --depth 1 --branch "$UCHARDET_TAG" "$UCHARDET_URL" "$UCHARDET_DIR"
  fi
  # The tag is movable: the pinned commit is the identity, checked on a fresh clone and on a cached one.
  verify_git_checkout "$UCHARDET_DIR" "$UCHARDET_COMMIT"
}

build_uchardet() {
  need git
  need cmake
  fetch_uchardet
  mkdir -p "$BDIR/uchardet"
  echo "==> cmake uchardet $UCHARDET_TAG -> $PREFIX"
  # CMAKE_POLICY_VERSION_MINIMUM: CMake 4.x rejects cmake_minimum_required(3.1) otherwise.
  # CHECK_SSE2=OFF: host is arm64 macOS; target is ARMv7 — never probe SSE.
  cmake -S "$UCHARDET_DIR" -B "$BDIR/uchardet" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_BINARY=OFF \
    -DCHECK_SSE2=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    > "$BDIR/uchardet/configure.log" 2>&1 \
    || { tail -40 "$BDIR/uchardet/configure.log" >&2; die "uchardet cmake configure failed"; }
  cmake --build "$BDIR/uchardet" -j"$JOBS" > "$BDIR/uchardet/make.log" 2>&1 \
    || { tail -40 "$BDIR/uchardet/make.log" >&2; die "uchardet make failed"; }
  cmake --install "$BDIR/uchardet" > "$BDIR/uchardet/install.log" 2>&1 \
    || die "uchardet install failed"
  [[ -f "$PREFIX/lib/libuchardet.a" ]] || die "uchardet: libuchardet.a missing after install"
  [[ -f "$PREFIX/lib/pkgconfig/uchardet.pc" ]] || die "uchardet: uchardet.pc missing after install"
  verify_git_checkout "$UCHARDET_DIR" "$UCHARDET_COMMIT"
  echo "    libuchardet.a  uchardet.pc  (Cflags -Iinclude/uchardet for <uchardet.h>)"
}

fetch_sdlsound() {
  if [[ ! -d "$SDLSOUND_DIR/.git" ]]; then
    echo "==> fetching SDL_sound (mkxp-z fork, $SDLSOUND_PIN)"
    mkdir -p "$SRC"
    git clone --quiet --no-checkout "$SDLSOUND_URL" "$SDLSOUND_DIR"
  fi
  if ! git -C "$SDLSOUND_DIR" cat-file -e "$SDLSOUND_PIN^{commit}" 2>/dev/null; then
    git -C "$SDLSOUND_DIR" fetch --quiet origin "$SDLSOUND_PIN"
  fi
  # A --no-checkout clone has no index file, which diff --cached reports as
  # every file deleted. Its initial checkout refuses to overwrite untracked
  # files; an index that exists (even emptied) still gets the edit check.
  if [[ -f "$(git -C "$SDLSOUND_DIR" rev-parse --absolute-git-dir)/index" ]]; then
    git -C "$SDLSOUND_DIR" diff --quiet && git -C "$SDLSOUND_DIR" diff --cached --quiet ||
      die "SDL_sound has tracked edits; restore the managed source before building"
  fi
  git -C "$SDLSOUND_DIR" checkout --quiet --detach "$SDLSOUND_PIN"
  [[ "$(git -C "$SDLSOUND_DIR" rev-parse HEAD)" == "$SDLSOUND_PIN" ]] ||
    die "SDL_sound source pin mismatch"
}

build_sdlsound() {
  need git
  need cmake
  fetch_sdlsound
  mkdir -p "$BDIR/sdl2_sound"
  echo "==> cmake SDL2_sound (mkxp-z fork) -> $PREFIX"
  # find_package(SDL2) resolves against the vdpm sysroot via vita.toolchain.cmake.
  # Decoders are bundled (dr_flac, stb_vorbis, dr_mp3) — no external codec libs.
  # MODPLUG/MIDI/SHN/COREAUDIO off: not needed for RGSS audio, saves size.
  cmake -S "$SDLSOUND_DIR" -B "$BDIR/sdl2_sound" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDLSOUND_BUILD_SHARED=OFF \
    -DSDLSOUND_BUILD_STATIC=ON \
    -DSDLSOUND_BUILD_TEST=OFF \
    -DSDLSOUND_DECODER_MODPLUG=OFF \
    -DSDLSOUND_DECODER_MIDI=OFF \
    -DSDLSOUND_DECODER_SHN=OFF \
    -DSDLSOUND_DECODER_COREAUDIO=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    > "$BDIR/sdl2_sound/configure.log" 2>&1 \
    || { tail -40 "$BDIR/sdl2_sound/configure.log" >&2; die "SDL2_sound cmake configure failed"; }
  cmake --build "$BDIR/sdl2_sound" -j"$JOBS" > "$BDIR/sdl2_sound/make.log" 2>&1 \
    || { tail -40 "$BDIR/sdl2_sound/make.log" >&2; die "SDL2_sound make failed"; }
  cmake --install "$BDIR/sdl2_sound" > "$BDIR/sdl2_sound/install.log" 2>&1 \
    || die "SDL2_sound install failed"
  flatten_sdlsound_header
  write_sdlsound_pc
  [[ -f "$PREFIX/lib/libSDL2_sound.a" ]] || die "SDL2_sound: libSDL2_sound.a missing after install"
  echo "    libSDL2_sound.a  SDL2_sound.pc  SDL_sound.h"
}

fetch_pixman() {
  local tar="$DIST/pixman-$PIXMAN_VERSION.tar.gz"
  need curl
  fetch_verified "$PIXMAN_URL" "$PIXMAN_SHA256" "$tar"
  if tree_reusable "$PIXMAN_DIR" pixman && [[ -f "$PIXMAN_DIR/meson.build" ]]; then
    return 0
  fi
  echo "==> unpacking pixman $PIXMAN_VERSION (sha256 verified)"
  mkdir -p "$SRC"
  rm -rf "$SRC/pixman-$PIXMAN_VERSION" "$PIXMAN_DIR"
  tar -C "$SRC" -xzf "$tar"
  mv "$SRC/pixman-$PIXMAN_VERSION" "$PIXMAN_DIR"
  # VitaSDK binutils 2.46 rejects the leading-zero numeric local labels in
  # pixman-arm-simd-asm.S. Upstream commit 865e6ce0 ("pixman: Adjust arm
  # assembly for binutils change", 2024-07-12) strips the leading zeros in
  # exactly this file; apply the same two substitutions to the 0.42.2 tarball.
  # The ref form is anchored on whitespace (BSD sed has no \b) — operand
  # tokens in these files are always space-delimited. The expression is
  # transforms.pixmanArmAsmSed in dep-pins.json, part of the tree's sealed identity.
  sed -i.bak -E "$PIXMAN_ASM_SED" "$PIXMAN_DIR/pixman/pixman-arm-simd-asm.S"
  rm -f "$PIXMAN_DIR/pixman/pixman-arm-simd-asm.S.bak"
  seal_tree "$PIXMAN_DIR" pixman
}

build_pixman() {
  need curl
  need meson
  fetch_pixman
  # The cross file changes between script versions; meson setup cannot
  # reconfigure an existing dir in place, and this build is seconds.
  rm -rf "$BDIR/pixman"
  mkdir -p "$BDIR/pixman"
  # Meson cross file, same shape as vita/meson/vita-cross.ini. 'linux' is a
  # code-path selector, not a claim (see that file's header comment).
  # -O2 rather than meson's 'release' -O3, so optimization is set
  # explicitly. b_ndebug stays false to match the
  # engine's live-assert policy. ARM SIMD and NEON are forced on: the sysroot
  # archive ships both, and a silent auto probe failure under cross would
  # quietly drop them.
  # -g without -ffile-prefix-map puts the real checkout path into the DWARF
  # the engine's unstripped side ELF inherits, which
  # leaks the build machine's paths. Same policy as the other
  # build scripts: map every local root to a neutral name, shortest first so
  # the most general map applies first.
  PIXMAN_MAP_LINES=""
  add_pixman_map() {
    local from=$1 to=$2
    [[ -n "$from" && "$from" != "/" ]] || return 0
    case "$from" in *[[:space:]]*) return 0 ;; esac
    PIXMAN_MAP_LINES="$PIXMAN_MAP_LINES${#from} -ffile-prefix-map=$from=$to
"
  }
  add_pixman_map "${HOME:-}" /mkxpz-home
  add_pixman_map "$ROOT"     /mkxp-z-vita
  add_pixman_map "$DEPS_ROOT" /vita-deps
  PIXMAN_MAPS=""
  while IFS= read -r map_flag; do
    [[ -n "$map_flag" ]] && PIXMAN_MAPS="$PIXMAN_MAPS, '$map_flag'"
  done <<< "$(printf '%s' "$PIXMAN_MAP_LINES" | sort -u | sort -n -k1,1 | cut -d' ' -f2-)"
  cat > "$BDIR/pixman/vita-cross.ini" << EOF
# Generated by vita/scripts/build-vita-deps.sh — do not edit.
[binaries]
c = 'arm-vita-eabi-gcc'
cpp = 'arm-vita-eabi-g++'
ar = 'arm-vita-eabi-ar'
strip = 'arm-vita-eabi-strip'
pkg-config = 'arm-vita-eabi-pkg-config'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a9'
endian = 'little'

[built-in options]
c_args = ['-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-O2', '-g', '-ffunction-sections', '-fdata-sections'$PIXMAN_MAPS]
cpp_args = ['-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-O2', '-g', '-ffunction-sections', '-fdata-sections'$PIXMAN_MAPS]
c_link_args = ['-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard']
cpp_link_args = ['-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard']
EOF
  echo "==> meson pixman $PIXMAN_VERSION -> $PREFIX"
  meson setup "$BDIR/pixman" "$PIXMAN_DIR" \
    --cross-file "$BDIR/pixman/vita-cross.ini" \
    --prefix "$PREFIX" \
    --default-library static \
    --buildtype release \
    -Doptimization=2 \
    -Darm-simd=enabled -Dneon=enabled \
    -Dlibpng=disabled -Dgtk=disabled -Dtests=disabled \
    > "$BDIR/pixman/configure.log" 2>&1 \
    || { tail -40 "$BDIR/pixman/configure.log" >&2; die "pixman meson setup failed (log: $BDIR/pixman/configure.log)"; }
  meson install -C "$BDIR/pixman" > "$BDIR/pixman/install.log" 2>&1 \
    || { tail -40 "$BDIR/pixman/install.log" >&2; die "pixman install failed"; }
  [[ -f "$PREFIX/lib/libpixman-1.a" ]] || die "pixman: libpixman-1.a missing after install"
  [[ -f "$PREFIX/lib/pkgconfig/pixman-1.pc" ]] || die "pixman: pixman-1.pc missing after install"
  verify_tree "$PIXMAN_DIR" pixman
  echo "    libpixman-1.a  pixman-1.pc  (release -O2, function/data sections)"
}

fetch_openal() {
  need curl
  need shasum
  need patch
  local tar="$DIST/openal-soft-$OPENAL_VERSION.tar.gz"
  local vita_patch="$DIST/openal-soft-$OPENAL_VERSION-vita-1.patch"
  fetch_verified "$OPENAL_URL" "$OPENAL_SHA256" "$tar"
  fetch_verified "$OPENAL_VITA_PATCH_URL" "$OPENAL_VITA_PATCH_SHA256" "$vita_patch"
  if tree_reusable "$OPENAL_DIR" openal && [[ -f "$OPENAL_DIR/OpenAL32/Include/alMain.h" ]]; then
    return 0
  fi
  echo "==> unpacking openal-soft $OPENAL_VERSION + Vita backend patch (sha256 verified)"
  mkdir -p "$SRC"
  rm -rf "$OPENAL_DIR"
  tar -C "$SRC" -xzf "$tar"
  patch -d "$OPENAL_DIR" -p1 < "$vita_patch" \
    || { echo "patch output above" >&2; die "openal: Vita backend patch failed"; }
  # Vita newlib printf does not honour %zu: it prints the literal letters and
  # does not consume the size_t, so the next conversion reads the wrong slot
  # (device data abort in strlen from _vfprintf_r). alMain.h
  # hardcodes SZFMT "%zu" in the trailing #else of its Windows-only chain, so
  # no -D can select it — substitute in place, same policy as fetch_pixman's
  # binutils sed (transforms.openalSzfmtSed in dep-pins.json, part of the tree's
  # sealed identity). size_t is 32-bit on arm-vita-eabi.
  sed -i.bak "$OPENAL_SZFMT_SED" "$OPENAL_DIR/OpenAL32/Include/alMain.h"
  rm -f "$OPENAL_DIR/OpenAL32/Include/alMain.h.bak"
  grep -q '^#define SZFMT "%u"$' "$OPENAL_DIR/OpenAL32/Include/alMain.h" \
    || die "openal: SZFMT substitution did not land"
  seal_tree "$OPENAL_DIR" openal
}

build_openal() {
  need curl
  need cmake
  fetch_openal
  # cmake cannot change a toolchain/flags in place; this build is under a
  # minute, so start clean like build_pixman.
  rm -rf "$BDIR/openal"
  mkdir -p "$BDIR/openal"
  echo "==> cmake openal-soft $OPENAL_VERSION -> $PREFIX"
  # C flags match the vitasdk package recipe: 1.19.1 turns
  # implicit-function-declaration / int-conversion warnings into errors and
  # the 2021-era vita backend trips both. UTILS/TESTS/EXAMPLES/CONFIG are
  # also forced off for VITA by that patch; CONFIG additionally keeps the
  # alsoftrc sample + HRTF mhr data out of the side prefix (nothing packages
  # share/). NEON required: CMake probes -mfpu=neon itself and the sysroot
  # archive ships mixer_neon, so a silent probe regression must fail loudly.
  cmake -S "$OPENAL_DIR" -B "$BDIR/openal" \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBTYPE=STATIC \
    -DCMAKE_C_FLAGS="-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion" \
    -DALSOFT_REQUIRE_NEON=ON \
    -DALSOFT_BACKEND_SDL2=OFF \
    -DALSOFT_UTILS=OFF -DALSOFT_TESTS=OFF -DALSOFT_EXAMPLES=OFF \
    -DALSOFT_CONFIG=OFF \
    > "$BDIR/openal/configure.log" 2>&1 \
    || { tail -40 "$BDIR/openal/configure.log" >&2; die "openal cmake configure failed (log: $BDIR/openal/configure.log)"; }
  cmake --build "$BDIR/openal" -j"$JOBS" > "$BDIR/openal/make.log" 2>&1 \
    || { tail -40 "$BDIR/openal/make.log" >&2; die "openal make failed"; }
  cmake --install "$BDIR/openal" > "$BDIR/openal/install.log" 2>&1 \
    || { tail -20 "$BDIR/openal/install.log" >&2; die "openal install failed"; }
  [[ -f "$PREFIX/lib/libopenal.a" ]] || die "openal: libopenal.a missing after install"
  [[ -f "$PREFIX/lib/pkgconfig/openal.pc" ]] || die "openal: openal.pc missing after install"
  # The reason this recipe exists: no %zu format may survive in
  # the archive the player links. SZFMT sites are the only %zu in 1.19.1's
  # core sources, and the only other definition (examples/alrecord.c) is not
  # compiled with UTILS/EXAMPLES off. Scan printable strings, not raw bytes:
  # a bare byte grep also hits coincidental code bytes. Capture once, then
  # grep the text: under pipefail a `strings | grep -q` dies with SIGPIPE
  # exactly when the match is found. Adjacent literals concatenate at compile
  # time, so every SZFMT user shows up as one sentence.
  local openal_strings
  openal_strings="$(strings -a "$PREFIX/lib/libopenal.a")"
  if grep -q '%zu' <<< "$openal_strings"; then
    die "openal: %zu format survived in libopenal.a (SZFMT substitution lost?)"
  fi
  grep -q 'Freed %u context property object%s' <<< "$openal_strings" \
    || die "openal: SZFMT %u did not reach the archive strings"
  verify_tree "$OPENAL_DIR" openal
  echo "    libopenal.a  openal.pc  (SZFMT -> %u; vita/null/wave/loopback backends, NEON on)"
}

# Source only: the VitaSDK libpthread that the player links is a toolchain binary; the pinned
# source is fetched so the release source archive can carry it (the release build checks the
# revision against $VITASDK/version_info.txt).
fetch_pthread_embedded() {
  need git
  echo "==> fetching pthread-embedded $PTHREAD_COMMIT"
  fetch_git_pinned "$PTHREAD_DIR" "$PTHREAD_URL" "$PTHREAD_COMMIT"
}

# Source only: the statically linked vdpm libraries are VitaSDK binary packages. Each package
# file and its upstream tarballs and patches are fetched into $DIST/vdpm/<package>/ (each refused
# unless its pinned SHA-256 matches), libpng comes from its pinned commit, and the
# vitasdk/packages recipes at their pinned commit. The release source archive carries the
# sources; it checks each package file's .BUILDINFO against the pinned recipe digest and the
# installed libraries against the digests pinned in dep-pins.json.
fetch_vdpm_sources() {
  need curl
  need git
  local package file url sha
  echo "==> fetching the vdpm library sources (pinned by SHA-256 or commit)"
  while IFS=$'\t' read -r package file url sha; do
    fetch_verified "$url" "$sha" "$DIST/vdpm/$package/$file"
  done < <(vdpm_downloads)
  fetch_git_pinned "$LIBPNG_DIR" "$LIBPNG_URL" "$LIBPNG_COMMIT"
  fetch_git_pinned "$VDPM_RECIPES_DIR" "$VDPM_RECIPES_URL" "$VDPM_RECIPES_COMMIT"
}

status() {
  echo "DEPS_ROOT=$DEPS_ROOT"
  echo "PREFIX=$PREFIX"
  echo "SYSROOT=$SYSROOT"
  local p
  for p in \
    "lib/libtheora.a" \
    "lib/libuchardet.a" \
    "lib/libSDL2_sound.a" \
    "lib/libpixman-1.a" \
    "lib/libopenal.a" \
    "lib/pkgconfig/theora.pc" \
    "lib/pkgconfig/uchardet.pc" \
    "lib/pkgconfig/SDL2_sound.pc" \
    "lib/pkgconfig/pixman-1.pc" \
    "lib/pkgconfig/openal.pc" \
    "include/theora/theoradec.h" \
    "include/uchardet/uchardet.h" \
    "include/SDL_sound.h" \
    "include/pixman-1/pixman.h" \
    "include/AL/alc.h"
  do
    if [[ -f "$PREFIX/$p" ]]; then
      echo "  ok      $p"
    else
      echo "  MISSING $p"
    fi
  done
  if [[ -x "$BIN/arm-vita-eabi-pkg-config-deps" ]]; then
    echo "  ok      bin/arm-vita-eabi-pkg-config-deps"
    if command -v pkg-config >/dev/null; then
      echo "  probe   theora  $($BIN/arm-vita-eabi-pkg-config-deps --modversion theora 2>/dev/null || echo '?')"
      echo "  probe   uchardet $($BIN/arm-vita-eabi-pkg-config-deps --modversion uchardet 2>/dev/null || echo '?')"
      echo "  probe   openal  $($BIN/arm-vita-eabi-pkg-config-deps --modversion openal 2>/dev/null || echo '?')"
    fi
  else
    echo "  MISSING bin/arm-vita-eabi-pkg-config-deps (run: vita/scripts/build-vita-deps.sh wrapper)"
  fi
}

MODE="${1:-all}"
case "$MODE" in
  all)
    need arm-vita-eabi-gcc
    build_theora
    build_uchardet
    build_sdlsound
    build_pixman
    build_openal
    fetch_pthread_embedded
    fetch_vdpm_sources
    write_wrapper
    echo
    status
    ;;
  theora)
    need arm-vita-eabi-gcc
    build_theora
    write_wrapper
    ;;
  uchardet)
    need arm-vita-eabi-gcc
    build_uchardet
    write_wrapper
    ;;
  sdl2_sound|sdlsound|SDL2_sound)
    need arm-vita-eabi-gcc
    build_sdlsound
    write_wrapper
    ;;
  pixman)
    need arm-vita-eabi-gcc
    build_pixman
    write_wrapper
    ;;
  openal)
    need arm-vita-eabi-gcc
    build_openal
    write_wrapper
    ;;
  pthread-embedded)
    fetch_pthread_embedded
    ;;
  vdpm-sources)
    fetch_vdpm_sources
    ;;
  wrapper)
    write_wrapper
    ;;
  status)
    status
    ;;
  clean)
    echo "==> removing $DEPS_ROOT"
    rm -rf "$DEPS_ROOT"
    ;;
  *)
    die "unknown mode: $MODE (all|theora|uchardet|sdl2_sound|pixman|openal|pthread-embedded|vdpm-sources|wrapper|status|clean)"
    ;;
esac
