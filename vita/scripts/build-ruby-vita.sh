#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-configure (and optionally build) MRI 3.1.3 for arm-vita-eabi.
#
#   vita/scripts/build-ruby-vita.sh configure   # default: configure only
#   vita/scripts/build-ruby-vita.sh build       # configure + make + prefix install
#   vita/scripts/build-ruby-vita.sh install-prefix
#       # populate $PREFIX only (headers, config.h, libruby-static.a, ruby-3.1.pc)
#
# Pins:
#   source  mkxp-z/ruby @ 4d85560cf65938d7883a323bf553acad1faf5eae
#   baseruby $HOME/.local/ruby-3.1.3-mkxpz/bin/ruby
#
# Release hygiene: MRI is configured with a RELATIVE srcdir, a
# NEUTRAL literal --prefix and -ffile-prefix-map, so the archive this script
# installs carries no local build path. All three are needed — they fix three
# different mechanisms: __FILE__ text, compiled-in prefix strings and DWARF.
#
# Thread model: `--disable-pthread` is IGNORED by Ruby 3.1 configure.
# The real switch is `--with-thread=IMPLEMENTATION`, and
# this Ruby pin (4d85560c) has no thread_none.c, so THREAD_MODEL=pthread
# is what we actually get. vita/patches/ruby/0012 then forces UBF_TIMER_NONE
# so ruby_setup does not spawn the 32 KiB timer pthread that prefetch-
# aborts at PC=0 on hardware. Do not "clean up" --disable-pthread without
# reading that patch first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE="${1:-configure}"

# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?VitaSDK not found; source vita/scripts/vita-env.sh or install to \$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

BASERUBY="${BASERUBY:-$HOME/.local/ruby-3.1.3-mkxpz/bin/ruby}"
# Project-specific names: a generic SRC or PREFIX in the caller's environment must never select
# a directory this script resets or empties. All three must lie inside $ROOT/build.
SRC="${MKXPZ_RUBY_SRC:-$ROOT/build/ruby-src}"
BUILD="${MKXPZ_RUBY_BUILD:-$ROOT/build/ruby-vita}"
PREFIX="${MKXPZ_RUBY_PREFIX:-$ROOT/build/ruby-vita-prefix}"
CONFIG_SITE="$ROOT/vita/ruby/config.site"
export CONFIG_SITE

# Compiled-in prefix. configure bakes --prefix into verconf.h as
# RUBY_EXEC_PREFIX and the default $LOAD_PATH entries; those are plain .rodata
# strings in the shipped eboot and no compiler flag can rewrite them. A neutral
# literal is safe here because this script never runs `make install`:
# install_prefix() below copies into its own $PREFIX and re-roots ${prefix}
# out of the Makefile's includedir, and the on-device load path is set
# explicitly by the engine
# (binding-mri.cpp: rb_ary_clear($:) then push "app0:/ruby").
CONFIGURE_PREFIX="${CONFIGURE_PREFIX:-/mkxp-z}"
export BASERUBY CONFIGURE_PREFIX

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

die() { echo "error: $*" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/vita/scripts/dep-pins.sh"
require_under_build MKXPZ_RUBY_SRC "$SRC"
require_under_build MKXPZ_RUBY_BUILD "$BUILD"
require_under_build MKXPZ_RUBY_PREFIX "$PREFIX"
provenance() {
  python3 "$ROOT/vita/scripts/ruby-provenance.py" "$1" \
    --src "$SRC" --build "$BUILD" --prefix "$PREFIX"
}

# ---------------------------------------------------------------------------
# Prefix install
#
# Upstream holes this closes:
#   #185  template/ruby.pc.in has
#           Libs: ${DLDFLAGS} ${LIBRUBYARG_SHARED} ${LIBS}
#         Under --disable-shared, configure.ac sets LIBRUBYARG_SHARED to just
#         $LIBRUBY_RPATHFLAGS (empty -l). meson dependency('ruby-3.1') then
#         cannot link libruby-static.a. We install a corrected ruby-3.1.pc
#         whose Libs is -lruby-static (plus Libs.private for --static).
#   #187  mkxp-z binding/meson.build takes mri_includes as a *single* string
#         and emits exactly one -I. The arch config.h lives under
#         $rubyhdrdir/$arch/ruby/config.h, so a lone -I$rubyhdrdir cannot see
#         it. Install config.h both at the standard arch path (for the .pc
#         Cflags, which has both -I) and flattened at $rubyhdrdir/ruby/ so
#         -Dmri_includes=$rubyhdrdir alone is enough.
#
# Installs a complete libruby-static.a: core, extensions, encodings and their
# real registrars. The raw build/libruby-static.a is a core-only intermediate.
# ---------------------------------------------------------------------------
install_prefix() {
  provenance check-build
  [[ -f "$BUILD/libruby-vita.a" ]] || die "missing complete Ruby archive (run: $0 build)"
  [[ -d "$SRC/include" ]] || die "missing $SRC/include"

  # Physical path: worktrees symlink build/ at the main checkout; the .pc
  # must carry the real prefix so it stays valid after the worktree is gone.
  mkdir -p "$PREFIX"
  PREFIX="$(cd "$PREFIX" && pwd -P)"
  rm -f "$PREFIX/vita-provenance.json"

  # MRI version / arch from the generated Makefile (not hardcoded).
  local ruby_version arch includedir
  ruby_version=$(sed -n 's/^ruby_version = //p' "$BUILD/Makefile" | head -1)
  arch=$(sed -n 's/^arch = //p' "$BUILD/Makefile" | head -1)
  includedir=$(sed -n 's/^includedir = //p' "$BUILD/Makefile" | head -1)
  # includedir is usually ${prefix}/include — resolve against PREFIX. The
  # compiled-in prefix is the neutral $CONFIGURE_PREFIX literal, so
  # an already-expanded includedir lands outside $PREFIX and is re-rooted by
  # the guard below; the ${prefix} form is what MRI 3.1 actually emits.
  includedir="${includedir//\$\{prefix\}/$PREFIX}"
  includedir="${includedir//\$\{exec_prefix\}/$PREFIX}"
  [[ -n "$ruby_version" && -n "$arch" ]] || die "could not read ruby_version/arch from $BUILD/Makefile"
  [[ "$includedir" == "$PREFIX/"* ]] || includedir="$PREFIX/include"

  local hdrdir="$includedir/ruby-$ruby_version"
  local archhdrdir="$hdrdir/$arch"
  local pcdir="$PREFIX/lib/pkgconfig"

  echo "==> install-prefix"
  echo "    PREFIX=$PREFIX"
  echo "    hdrdir=$hdrdir"
  echo "    archhdrdir=$archhdrdir"
  echo "    arch=$arch  ruby_version=$ruby_version"

  rm -rf "$hdrdir"
  mkdir -p "$hdrdir" "$archhdrdir/ruby" "$PREFIX/lib" "$pcdir"

  # Public headers (ruby.h + ruby/*.h). version.h lives in the source tree.
  cp -R "$SRC/include/." "$hdrdir/"

  # Arch config.h. Standard path first (matches rubyarchhdrdir in the .pc).
  local cfg="$BUILD/.ext/include/$arch/ruby/config.h"
  [[ -f "$cfg" ]] || die "missing arch config.h at $cfg"
  cp -f "$cfg" "$archhdrdir/ruby/config.h"
  # Flat copy so a single -I$hdrdir (mkxp-z mri_includes) finds ruby/config.h.
  # The standard tree has no ruby/config.h, so this does not clobber a header.
  cp -f "$cfg" "$hdrdir/ruby/config.h"

  # Static archive into the prefix (mri_libpath / find_library).
  cp -f "$BUILD/libruby-vita.a" "$PREFIX/lib/libruby-static.a"

  # Pure-Ruby companions of the static extensions. The packager copies this
  # directory to app0:/ruby; no host installation paths are used at runtime.
  local runtime="$PREFIX/lib/ruby/$ruby_version"
  mkdir -p "$runtime/digest"
  cp -R "$SRC/ext/date/lib/." "$runtime/"
  cp -R "$SRC/ext/digest/lib/." "$runtime/"
  cp -R "$SRC/ext/digest/sha2/lib/." "$runtime/digest/"
  cp -R "$SRC/ext/objspace/lib/." "$runtime/"

  # Corrected ruby-3.1.pc. Written by hand rather than via `make ruby-3.1.pc`
  # so we control Libs; --define-prefix (VitaSDK pkg-config) recomputes
  # prefix from this file's location, so ${prefix}-relative vars are safe.
  #
  # Libs.private is pulled in by arm-vita-eabi-pkg-config's --static.
  # This runtime uses pthread-embedded and the Zlib extension needs libz.
  local pc="$pcdir/ruby-3.1.pc"
  cat > "$pc" << EOF
# Generated by vita/scripts/build-ruby-vita.sh — do not edit.
# Upstream template/ruby.pc.in Libs uses LIBRUBYARG_SHARED, which is empty
# under --disable-shared. This file points at libruby-static.a instead.
prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
ruby_version=$ruby_version
arch=$arch
rubyhdrdir=\${includedir}/ruby-$ruby_version
rubyarchhdrdir=\${rubyhdrdir}/$arch
RUBY_SO_NAME=ruby
LIBRUBY_A=libruby-static.a

Name: Ruby
Description: Object Oriented Script Language (static, arm-vita-eabi)
Version: \${ruby_version}
URL: https://www.ruby-lang.org
Cflags: -I\${rubyarchhdrdir} -I\${rubyhdrdir}
Libs: -L\${libdir} -lruby-static
Libs.private: -lz -lm -lpthread
Requires:
EOF
  provenance install
  echo "==> wrote $pc"
  echo "==> wrote $PREFIX/lib/libruby-static.a ($(wc -c < "$PREFIX/lib/libruby-static.a") bytes)"
  echo "==> headers: $hdrdir (config.h also at ruby/config.h for single -I)"
}

# install-prefix only needs an existing build tree; skip configure/patches.
if [[ "$MODE" == "install-prefix" ]]; then
  [[ -d "$BUILD" ]] || die "missing build dir $BUILD (run: $0 build first)"
  install_prefix
  echo "done: $MODE"
  exit 0
fi

[[ -x "$BASERUBY" ]] || die "baseruby not found at $BASERUBY (run vita/scripts/build-host-ruby.sh)"
[[ -f "$CONFIG_SITE" ]] || die "missing $CONFIG_SITE"
[[ -d "$SRC" ]] || die "missing ruby source at $SRC (clone mkxp-z/ruby @ 4d85560c)"
[[ "$(git -C "$SRC" rev-parse HEAD)" == 4d85560cf65938d7883a323bf553acad1faf5eae ]] || die "Ruby source does not match the pinned revision"

# config.guess / config.sub are not in the git tree: the pinned gcc-mirror commits of vita/scripts/dep-pins.json,
# digest-checked, never whatever a branch holds today. The release source archive carries them.
fetch_gnu_config "$SRC/tool" "$ROOT/build/vita-deps/dist"

# Apply in-repo MRI patches (never edit the clone in place by hand).
PATCH_DIR="$ROOT/vita/patches/ruby"
if [[ -d "$PATCH_DIR" ]]; then
  echo "==> applying patches from $PATCH_DIR"
  # Revert any previous apply, then apply the series in order. Also drop
  # untracked generated files: an earlier build leaves *.rbinc in the srcdir
  # with the absolute `#line` paths of THAT configure, and make reuses them,
  # which put machine paths back into libruby.
  # Keep the two fetched configure helpers: they are untracked too, and a
  # build without network access cannot fetch them again.
  (cd "$SRC" && { git checkout -- . ; git clean -fdxq -e tool/config.guess -e tool/config.sub ; } 2>/dev/null || true)
  shopt -s nullglob
  for p in "$PATCH_DIR"/*.patch; do
    echo "    $(basename "$p")"
    if ! (cd "$SRC" && patch -p1 --forward --batch < "$p"); then
      echo "error: patch failed: $p" >&2
      exit 1
    fi
  done
  shopt -u nullglob
fi

# autoconf if configure is missing or older than configure.ac
if [[ ! -x "$SRC/configure" ]] || [[ "$SRC/configure.ac" -nt "$SRC/configure" ]]; then
  (cd "$SRC" && autoconf)
fi

mkdir -p "$BUILD"
provenance prepare

# Vita newlib cross flags. -std=gnu99 is mandatory on GCC 15 (MRI 3.1 is not C23).
# NO -fPIC: Vita homebrew is ET_EXEC at a fixed load address; -fPIC emits
# R_ARM_GOTPC/R_ARM_GOT32 relocations that vita-elf-create rejects
# ("Invalid relocation type 25").
# -g: without it MRI archive members carry no DWARF (engine members do), so a
# core landing in Ruby code symbolises by name only. Debug info does not change
# codegen and never ships: the packager strips eboot.bin and keeps the
# unstripped ELF as the symbolisation source.
TARGET_CFLAGS="-std=gnu99 -O2 -g -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"
TARGET_LDFLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"

# --- Non-identifying file paths ----------------------------------
# Belt and braces beside the relative srcdir below: MRI's __FILE__ and assert
# text are .rodata, which `strip -g` cannot remove, and absolute paths also
# arrive through CPPFLAGS/XCFLAGS -I and through the build directory that DWARF
# records as DW_AT_comp_dir. -ffile-prefix-map is -fdebug-prefix-map plus
# -fmacro-prefix-map, so it covers both; codegen is unchanged.
#
# ORDERING IS LOAD-BEARING: GCC applies the LAST matching map, so the most
# general prefix goes FIRST and the most specific LAST. Sorting by ascending
# path length yields that order, because a proper prefix is strictly shorter.
# TARGET_CFLAGS is used unquoted, so a path holding whitespace is skipped
# instead of splitting into broken flags.
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
add_prefix_map "$SRC"      /ruby-src
add_prefix_map "$BUILD"    /ruby-build
add_prefix_map "$PREFIX"   /ruby-prefix
FILE_PREFIX_MAPS="$(printf '%s' "$PREFIX_MAP_LINES" | sort -u | sort -n -k1,1 \
  | cut -d' ' -f2- | tr '\n' ' ')"
# Never echo the mapped-from paths: they are exactly what must not be quoted.
echo "==> file path maps: $(printf '%s' "$FILE_PREFIX_MAPS" | wc -w | tr -d ' ')" \
     "-ffile-prefix-map entries (home, SDK, checkout, source, build, prefix)"
TARGET_CFLAGS="$TARGET_CFLAGS $FILE_PREFIX_MAPS"

# Out-ext list (socket needs /usr/include on the host).
# `-test-` alone does NOT silence the tree: extmk.rb matches out-ext entries with
# File.fnmatch against each extconf dir, and `fnmatch("-test-", "-test-/bignum")`
# is false (no wildcard, so children don't match). `-*` matches the whole
# ext/-test-/* tree (and any other internal `-*` dirs) and keeps configure-ext
# from running 60+ pointless extconf passes. EXTSTATIC=static also drops `-*`
# from the build list, but the out-ext is what stops the configure noise.
OUT_EXT="openssl,readline,fiddle,pty,syslog,socket,io/console,dbm,gdbm,etc,win32ole,win32,-*"

echo "==> configure"
echo "    SRC=$SRC"
echo "    BUILD=$BUILD"
echo "    BASERUBY=$BASERUBY"
echo "    VITASDK=$VITASDK"
echo "    CONFIG_SITE=$CONFIG_SITE"

cd "$BUILD"
# Relative srcdir. tool/mk_builtin_loader.rb writes the *text*
# `#line N "<srcdir>/gc.rb"` into the generated *.rbinc as a C string constant,
# so it is data, not a debug record: no -f*-prefix-map can reach it and
# `strip -g` cannot remove it. Those literals are simply whatever srcdir the
# generator was handed, so configuring out-of-tree with a relative srcdir makes
# them relative ("../ruby-src/gc.rb") and takes MRI's own __FILE__/assert paths
# with them. Computed against $BUILD because configure runs from there.
command -v python3 >/dev/null || die "python3 is required to compute the relative srcdir"
SRC_REL="$(python3 -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
  "$SRC" "$BUILD")"
[[ -x "$SRC_REL/configure" ]] || die "relative srcdir does not resolve from $BUILD"
echo "    srcdir=$SRC_REL (relative: keeps .rbinc #line literals path-free)"
echo "    --prefix=$CONFIGURE_PREFIX (neutral literal; files are installed into \$PREFIX)"
# Cache overrides must also reach an existing build (notably condattr's clock).
if [[ ! -f Makefile ]] || [[ "$MODE" == "reconfigure" ]] || [[ "$CONFIG_SITE" -nt config.status ]] || [[ "$MODE" == "configure" && ! -f config.status ]]; then
  "$SRC_REL/configure" \
    --host=arm-vita-eabi \
    --build="$("$SRC/tool/config.guess")" \
    --prefix="$CONFIGURE_PREFIX" \
    --with-baseruby="$BASERUBY" \
    --disable-shared \
    --disable-install-doc \
    --disable-install-rdoc \
    --disable-jit-support \
    --disable-yjit \
    --disable-rubygems \
    --without-gmp \
    --with-out-ext="$OUT_EXT" \
    --with-static-linked-ext \
    --with-coroutine=arm32 \
    --enable-install-static-library \
    --disable-pthread \
    CC=arm-vita-eabi-gcc \
    CXX=arm-vita-eabi-g++ \
    AR=arm-vita-eabi-ar \
    RANLIB=arm-vita-eabi-ranlib \
    STRIP=arm-vita-eabi-strip \
    CFLAGS="$TARGET_CFLAGS" \
    CXXFLAGS="$TARGET_CFLAGS" \
    LDFLAGS="$TARGET_LDFLAGS" \
    CPPFLAGS="-I$VITASDK/arm-vita-eabi/include" \
    LIBS="-L$VITASDK/arm-vita-eabi/lib"
fi

# Reapplying patches regenerates configure even when this build was already
# configured. Settle make's automatic reconfigure before editing its outputs.
echo "==> materialise Ruby makefiles"
make -j"$JOBS" config.status Makefile GNUmakefile

VITA_SHIM_FLAGS="-I$ROOT/vita/ruby/include -include $ROOT/vita/ruby/vita_mman.h"
verify_vita_makefile() {
  grep '^XCFLAGS = ' "$BUILD/Makefile" | grep -F -- "$VITA_SHIM_FLAGS" >/dev/null \
    || die "missing required Vita shim flags in $BUILD/Makefile"
}
locate_vita_config_h() {
  RUBY_ARCH=$(sed -n 's/^arch = //p' "$BUILD/Makefile" | head -1)
  [[ -n "$RUBY_ARCH" ]] || die "could not read arch from $BUILD/Makefile"
  CFG_H="$BUILD/.ext/include/$RUBY_ARCH/ruby/config.h"
  [[ -f "$CFG_H" ]] || die "missing arch config.h at $CFG_H"
}
verify_vita_config_h() {
  locate_vita_config_h
  local macros
  macros=$(arm-vita-eabi-gcc -E -dM -x c "$CFG_H") \
    || die "could not preprocess $CFG_H"
  if printf '%s\n' "$macros" | grep -E \
    '^#define HAVE_(PIPE2|QSORT_R|GNU_QSORT_R|BSD_QSORT_R|SIGACTION|SIGPROCMASK)([[:space:]]|$)' >/dev/null; then
    die "missing required Vita config.h overrides in $CFG_H"
  fi
}

# Inject the newlib mman shim only for compilation, not for configure probes.
if [[ -f "$BUILD/Makefile" ]]; then
  if ! grep '^XCFLAGS = ' "$BUILD/Makefile" | grep -F -- "$VITA_SHIM_FLAGS" >/dev/null; then
    sed -i.bak 's|^XCFLAGS = |XCFLAGS = '"$VITA_SHIM_FLAGS"' |' "$BUILD/Makefile"
  fi
  # Newlib declares some functions configure believes in; force them off in
  # the generated config.h so MRI takes its portable fallbacks.
  locate_vita_config_h
  if ! grep -q 'VITA_FORCE_NO' "$CFG_H"; then
    cat >> "$CFG_H" << 'EOF'

/* VITA_FORCE_NO: newlib lies about these; disable configure's yes. */
#ifdef HAVE_PIPE2
#undef HAVE_PIPE2
#endif
#ifdef HAVE_QSORT_R
#undef HAVE_QSORT_R
#endif
#ifdef HAVE_GNU_QSORT_R
#undef HAVE_GNU_QSORT_R
#endif
#ifdef HAVE_BSD_QSORT_R
#undef HAVE_BSD_QSORT_R
#endif
#ifdef HAVE_SIGACTION
#undef HAVE_SIGACTION
#endif
#ifdef HAVE_SIGPROCMASK
#undef HAVE_SIGPROCMASK
#endif
EOF
  fi
fi
verify_vita_makefile
verify_vita_config_h

# Record configuration only after the injected flags and overrides pass.
provenance configured

if [[ "$MODE" == "build" ]]; then
  # Compile Vita POSIX shims for the complete archive assembled after make.
  SHIM_SRC="$ROOT/vita/ruby/vita_posix_shims.c"
  if [[ -f "$SHIM_SRC" ]]; then
    cp -f "$SHIM_SRC" .
    arm-vita-eabi-gcc $TARGET_CFLAGS -c vita_posix_shims.c -o vita_posix_shims.o
  fi
  # Fiber machine stacks live inside the Ruby thread's own registered stack
  # (vita/patches/ruby/0015 calls into this module from cont.c and
  # thread_pthread.c). Its own file so it can be built on the host
  # with a fake thread-info provider.
  # Both files are copied in and compiled with relative paths, like the shims
  # above, so nothing of this checkout's layout reaches .rodata.
  ARENA_SRC="$ROOT/vita/ruby/vita_fiber_arena.c"
  ARENA_HDR="$ROOT/vita/ruby/include/vita_fiber_arena.h"
  [[ -f "$ARENA_SRC" ]] || die "missing $ARENA_SRC (vita/patches/ruby/0015 needs it)"
  [[ -f "$ARENA_HDR" ]] || die "missing $ARENA_HDR (vita/patches/ruby/0015 needs it)"
  cp -f "$ARENA_SRC" "$ARENA_HDR" .
  arm-vita-eabi-gcc $TARGET_CFLAGS -I. -c vita_fiber_arena.c -o vita_fiber_arena.o
  echo "==> make -j$JOBS libruby-static.a"
  make -j"$JOBS" libruby-static.a
  verify_vita_makefile
  verify_vita_config_h
  if [[ -f libruby-static.a ]]; then
    echo "==> libruby-static.a: $(pwd)/libruby-static.a ($(wc -c < libruby-static.a) bytes)"
  else
    die "libruby-static.a was not produced"
  fi

  # --- Static extensions -----------------------------------------
  # Never run bare `make`/`make all` for extensions: mkmf tries to host-ld a
  # .so for each one and the cross link fails. Drive each required ext through
  # its own `static` target, which emits <name>.a instead.
  #
  # Required list (from the engine and its preloads):
  #   zlib        binding/binding-mri.cpp: require('zlib') unless Zlib defined
  #   stringio
  #   digest/* ; Digest::MD5/SHA1/SHA2 lazily require the sub-ext
  #   strscan
  #   date
  #   objspace    scripts/preload/kgl2_wrap.rb: require 'objspace' (KGL2 games)
  REQUIRED_EXTS=(
    zlib
    stringio
    strscan
    date
    digest
    digest/md5
    digest/sha1
    digest/sha2
    digest/rmd160
    digest/bubblebabble
    objspace
  )

  # Generate ext/*/Makefile (and exts.mk) without building every .so.
  # `configure-ext` depends on $(LIBRUBY), so libruby-static.a must exist first.
  echo "==> make configure-ext (generates ext Makefiles; skips host-ld .so)"
  make configure-ext

  EXT_FAIL=0
  build_static_ext() {
    local name=$1
    local mk="ext/$name/Makefile"
    if [[ ! -f "$mk" ]]; then
      echo "warning: $mk missing (extconf skipped or failed); cannot build $name" >&2
      EXT_FAIL=1
      return 1
    fi
    if grep -q '\*\*\*DUMMY MAKEFILE\*\*\*' "$mk" 2>/dev/null; then
      echo "warning: ext/$name has a dummy Makefile (extconf failed); see ext/$name/mkmf.log" >&2
      EXT_FAIL=1
      return 1
    fi
    echo "==> make -C ext/$name static"
    # Extensions and encodings must be non-PIC at their original build,
    # not replaced by a harness-only subset of objects at link time.
    make -C "ext/$name" clean
    if ! make -C "ext/$name" -j"$JOBS" CCDLFLAGS= static; then
      echo "error: make -C ext/$name static failed" >&2
      EXT_FAIL=1
      return 1
    fi
  }

  for e in "${REQUIRED_EXTS[@]}"; do
    build_static_ext "$e" || true
  done

  # enc/ + enc/trans → enc/libenc.a, enc/libtrans.a (ENCSTATIC=static).
  # `libencs` is the documented target; `make -C enc` runs `all` which also
  # tries to produce .so for every encoding.
  echo "==> make libencs (libenc.a + libtrans.a)"
  # Make does not track flag changes. Drop prior encoding objects so an old
  # PIC build cannot be silently reused after upgrading this build script.
  rm -f "$BUILD"/enc/*.o "$BUILD"/enc/*.a "$BUILD"/enc/trans/*.o
  if ! make -j"$JOBS" CCDLFLAGS= libencs; then
    echo "error: make libencs failed" >&2
    EXT_FAIL=1
  fi
  verify_vita_makefile
  verify_vita_config_h

  echo "==> static extension artifacts"
  find ext enc -name '*.a' 2>/dev/null | sort || true
  if [[ "$EXT_FAIL" -ne 0 ]]; then
    die "required static extensions or encodings failed; refusing incomplete prefix"
  fi

  # Build the real registrars, then combine the core and required archives.
  # Removing dmyenc/dmyext is essential: otherwise the archive index may
  # resolve Init_enc/Init_ext to no-op/dynamic-load fallbacks first.
  arm-vita-eabi-gcc $TARGET_CFLAGS -I"$SRC/include" \
    -I"$BUILD/.ext/include/$RUBY_ARCH" -c "$BUILD/enc/encinit.c" \
    -o "$BUILD/enc/encinit.o"
  arm-vita-eabi-gcc $TARGET_CFLAGS -c "$ROOT/vita/ruby/static_extensions.c" \
    -o "$BUILD/vita_static_extensions.o"
  ARCHIVES=(libruby-static.a ext/zlib/zlib.a ext/stringio/stringio.a
    ext/strscan/strscan.a ext/date/date_core.a ext/digest/digest.a
    ext/digest/md5/md5.a ext/digest/sha1/sha1.a ext/digest/sha2/sha2.a
    ext/digest/rmd160/rmd160.a ext/digest/bubblebabble/bubblebabble.a
    ext/objspace/objspace.a enc/libenc.a enc/libtrans.a)
  for archive in "${ARCHIVES[@]}"; do
    [[ -f "$archive" ]] || die "missing required archive $archive"
    # Read the complete output (no grep -q SIGPIPE under pipefail).
    if arm-vita-eabi-readelf -r "$archive" | grep 'R_ARM_.*GOT' >/dev/null; then
      die "PIC relocations in $archive; rebuild with CCDLFLAGS= before installing"
    fi
    # MRI ADDLIB copies member headers verbatim, even with ar -DM; ranlib -D
    # only fixes the index. Normalize every input's headers before combining.
    arm-vita-eabi-objcopy -D "$archive"
  done
  # VitaSDK defaults to timestamped indexes/MRI archives; keep both outputs
  # byte-identical across configure-then-build and build-only invocations.
  {
    echo 'CREATE libruby-vita.a'
    for archive in "${ARCHIVES[@]}"; do echo "ADDLIB $archive"; done
    echo 'DELETE dmyext.o'
    echo 'DELETE dmyenc.o'
    echo 'ADDMOD enc/encinit.o'
    echo 'ADDMOD vita_static_extensions.o'
    # Add shims after all make targets: configure-ext can rebuild the core.
    echo 'ADDMOD vita_posix_shims.o'
    echo 'ADDMOD vita_fiber_arena.o'
    echo SAVE
    echo END
  } | arm-vita-eabi-ar -DM
  arm-vita-eabi-ranlib -D libruby-vita.a

  # Populate $PREFIX (headers + config.h + libruby-static.a + ruby-3.1.pc)
  # so meson find_library / dependency('ruby-3.1') can resolve MRI.
  cd "$BUILD"
  provenance seal
  install_prefix
fi

echo "done: $MODE (see $BUILD/config.log on failure)"
