#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# configure-vita.sh — meson setup of mkxp-z for arm-vita-eabi, in tree.
#
#   vita/scripts/configure-vita.sh              # meson setup (default)
#   vita/scripts/configure-vita.sh reconfigure  # wipe builddir and re-run
#   vita/scripts/configure-vita.sh status       # print pins/options, no setup
#
# The engine is this checkout: the Vita port lives in src/, binding/, shader/
# and vita/, on top of mkxp-z commit 826929ee (see MKXPZ_PIN). Nothing is
# cloned or patched here.
#
# Feature policy: every optional feature off except MiniFFI (Win32API shim).
# -Dgfx_backend=gles, -Denable-https=false, -Dmri_version=3.1. Steam is off by
# leaving steamworks_path empty. Dynamic FluidSynth is off; a verified
# TSF_PREFIX (TinySoundFont headers) enables the static MIDI path.
#
# Side-prefix deps: theora / uchardet / SDL2_sound / pixman / OpenAL are built
# by vita/scripts/build-vita-deps.sh into build/vita-deps/prefix (gitignored).
# When present, this script injects -I$PREFIX/include and -L$PREFIX/lib via the
# generated cross file so dependency('theora'/'uchardet') and
# find_library('SDL2_sound') resolve.
#
# MRI side prefix: vita/scripts/build-ruby-vita.sh install-prefix populates
# build/ruby-vita-prefix with libruby-static.a, headers (flat config.h for the
# single -I mri_includes) and a corrected ruby-3.1.pc. This script always
# writes a combined pkg-config wrapper (MRI + vita-deps + sysroot on
# PKG_CONFIG_LIBDIR); the stock arm-vita-eabi-pkg-config cannot see side
# prefixes (it clears PKG_CONFIG_PATH and pins the sysroot).
#
# Host-only. Does not claim a Vita link or boot. libruby-static.a may not
# exist yet: meson is expected to stop on that with a clear report until
# vita/scripts/build-ruby-vita.sh build has been run.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE="${1:-setup}"

# GL backend: vitaGL is the only backend. It links the pinned vitaGL stack
# (vita/scripts/build-vitagl.sh + build-sdl2-vitagl.sh) and compiles every TU
# with -DMKXPZ_VITAGL_BACKEND, which the engine's Vita code paths gate on. The
# variable is kept and accepts only vitagl.
VITA_GL_BACKEND="${VITA_GL_BACKEND:-vitagl}"
[[ "$VITA_GL_BACKEND" == vitagl ]] || { echo "error: VITA_GL_BACKEND must be vitagl" >&2; exit 1; }

# shellcheck source=/dev/null
. "$ROOT/vita/scripts/vita-env.sh"
: "${VITASDK:?VitaSDK not found; source vita/scripts/vita-env.sh or install to \$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
export VITASDK

MKXPZ_PIN="826929eeb3ebc4b887c011604919217a790770f4"
MKXPZ_SRC="$ROOT"
# An explicit BUILD_DIR wins.
BUILD_DIR="${BUILD_DIR:-$ROOT/build/mkxp-z-vitagl}"
CROSS_FILE="$ROOT/vita/meson/vita-cross.ini"
STUB_DIR="$BUILD_DIR/stub-libs"

# Release version: the single VERSION file at the repo root is
# the one source for the boot log line and the launcher header. It reaches the
# compiler as a -D on every TU, so a version bump recompiles what prints it.
# No file (probes, older checkouts) or a malformed one builds as "dev"; the
# release script (vita/scripts/build-release.sh) is the strict gate.
MKXPZ_VITA_VERSION=$(tr -d ' \t\r\n' < "$ROOT/vita/VERSION" 2>/dev/null || true)
[[ $MKXPZ_VITA_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || MKXPZ_VITA_VERSION="dev"

# Keep optional shaders off: they hung Shader::init on hardware (BicubicShader),
# and shader-code allocation failures under GPU pool exhaustion are hard errors,
# while the boot shader warm-up already spends that budget. The shipped GXP set
# covers the boot programs only. Enabling them needs a hardware warm-up and
# headroom re-test, not just a link.
MKXPZ_OPTIONAL_SHADERS="${MKXPZ_OPTIONAL_SHADERS:-0}"
case "$MKXPZ_OPTIONAL_SHADERS" in
  0) OPTIONAL_SHADERS=false ;;
  1) OPTIONAL_SHADERS=true ;;
  *) echo "error: MKXPZ_OPTIONAL_SHADERS must be 0 or 1" >&2; exit 1 ;;
esac

# Side-prefix deps from vita/scripts/build-vita-deps.sh.
DEPS_PREFIX="${DEPS_PREFIX:-$ROOT/build/vita-deps/prefix}"
DEPS_PKGCONFIG="${DEPS_PKGCONFIG:-$ROOT/build/vita-deps/bin/arm-vita-eabi-pkg-config-deps}"

# vitaGL side prefixes: build-vitagl.sh installs the GL stack,
# build-sdl2-vitagl.sh the VIDEO_VITA_VGL SDL2 rebuild. When present, that
# sdl2.pc is prepended to PKG_CONFIG_LIBDIR so meson dependency('sdl2')
# resolves against it, not the stock vdpm sdl2 (GXM-only).
SDL2_VITAGL_PREFIX="${SDL2_VITAGL_PREFIX:-$ROOT/build/sdl2-vitagl-prefix}"
VITAGL_PREFIX="${VITAGL_PREFIX:-$ROOT/build/vitagl-prefix}"
# An explicit TSF_PREFIX never falls back, so the header-free build can be proven.
if [[ -z ${TSF_PREFIX+x} ]]; then
  TSF_PREFIX="$ROOT/build/third-party/tinysoundfont"
fi
HAVE_TSF=0
if [[ -f "$TSF_PREFIX/tsf.h" && -f "$TSF_PREFIX/tml.h" ]]; then
  TSF_PREFIX="$TSF_PREFIX" bash "$ROOT/vita/scripts/fetch-tinysoundfont.sh" --check
  HAVE_TSF=1
fi
echo "==> TinySoundFont: $HAVE_TSF (optional; vita/scripts/fetch-tinysoundfont.sh installs the pinned headers)"
HAVE_SDL2_VITAGL=0
if [[ -f "$SDL2_VITAGL_PREFIX/lib/pkgconfig/sdl2.pc" ]]; then
  HAVE_SDL2_VITAGL=1
fi

# MRI 3.1 static install. Not required to exist for this script
# to *run*; meson will report the missing library if absent.
# build-ruby-vita.sh install-prefix populates:
#   $RUBY_PREFIX/lib/libruby-static.a
#   $RUBY_PREFIX/include/ruby-3.1.0/          (public headers)
#   $RUBY_PREFIX/include/ruby-3.1.0/arm-eabi/ruby/config.h
#   $RUBY_PREFIX/include/ruby-3.1.0/ruby/config.h  (flat copy; see below)
#   $RUBY_PREFIX/lib/pkgconfig/ruby-3.1.pc    (Libs: -lruby-static)
#
# mri_includes is a SINGLE string in mkxp-z (binding/meson.build emits one
# -I). The flat config.h copy is what makes -I$rubyhdrdir alone sufficient
# (upstream issue #187). The dependency('ruby-3.1') path is the alternative
# when mri_includes is left empty; it needs ruby-3.1.pc on PKG_CONFIG_LIBDIR
# (see the wrapper this script generates below).
RUBY_PREFIX="${RUBY_PREFIX:-$ROOT/build/ruby-vita-prefix}"
MRI_INCLUDES="${MRI_INCLUDES:-$RUBY_PREFIX/include/ruby-3.1.0}"
MRI_LIBPATH="${MRI_LIBPATH:-$RUBY_PREFIX/lib}"
MRI_LIBRARY="${MRI_LIBRARY:-ruby-static}"  # finds libruby-static.a

die() { echo "error: $*" >&2; exit 1; }

need() { command -v "$1" >/dev/null || die "missing host tool: $1"; }
need git
need meson
need arm-vita-eabi-gcc
need arm-vita-eabi-g++
need arm-vita-eabi-pkg-config
need xxd

[[ -f "$CROSS_FILE" ]] || die "missing cross file: $CROSS_FILE"

if [[ "$MODE" == "status" ]]; then
  echo "VITA_GL_BACKEND=$VITA_GL_BACKEND"
  echo "VITASDK=$VITASDK"
  echo "version=$MKXPZ_VITA_VERSION"
  echo "MKXPZ_SRC=$MKXPZ_SRC"
  echo "MKXPZ_PIN=$MKXPZ_PIN"
  echo "CROSS_FILE=$CROSS_FILE"
  echo "BUILD_DIR=$BUILD_DIR"
  echo "optional_shaders=$OPTIONAL_SHADERS"
  echo "section_gc=on (MKXPZ_LTO=${MKXPZ_LTO:-0}: b_lto + converter roots when 1)"
  echo "MRI: includes=$MRI_INCLUDES libpath=$MRI_LIBPATH library=lib$MRI_LIBRARY.a"
  echo "MRI pc: $RUBY_PREFIX/lib/pkgconfig/ruby-3.1.pc $( [[ -f $RUBY_PREFIX/lib/pkgconfig/ruby-3.1.pc ]] && echo present || echo absent)"
  echo "DEPS_PREFIX=$DEPS_PREFIX"
  echo "DEPS_PKGCONFIG=$DEPS_PKGCONFIG"
  echo "SDL2_VITAGL_PREFIX=$SDL2_VITAGL_PREFIX (have=$HAVE_SDL2_VITAGL)"
  echo "VITAGL_PREFIX=$VITAGL_PREFIX"
  echo "meson=$(meson --version) gcc=$(arm-vita-eabi-gcc -dumpversion)"
  if [[ -d "$MKXPZ_SRC/.git" ]]; then
    echo "mkxp-z HEAD=$(git -C "$MKXPZ_SRC" rev-parse HEAD)"
  else
    echo "mkxp-z HEAD=(not a git checkout)"
  fi
  exit 0
fi

# A core-only archive links successfully but has dummy Init_ext/Init_enc.
# Fail early instead of shipping another player with unusable requires.
python3 "$ROOT/vita/scripts/ruby-provenance.py" verify --prefix "$RUBY_PREFIX" \
  --archive "$MRI_LIBPATH/lib${MRI_LIBRARY}.a" --includes "$MRI_INCLUDES"
[[ -f "$MRI_LIBPATH/lib${MRI_LIBRARY}.a" ]] || die "build the complete Ruby prefix first: vita/scripts/build-ruby-vita.sh build"
RUBY_MEMBERS=$(arm-vita-eabi-ar t "$MRI_LIBPATH/lib${MRI_LIBRARY}.a")
if [[ "$RUBY_MEMBERS" == *dmyext.o* || "$RUBY_MEMBERS" == *dmyenc.o* ]]; then
  die "Ruby prefix still contains dummy registrars; rebuild vita/scripts/build-ruby-vita.sh build"
fi
[[ "$RUBY_MEMBERS" == *vita_static_extensions.o* && "$RUBY_MEMBERS" == *encinit.o* ]] || die "complete Ruby registrars missing"
[[ -f "$RUBY_PREFIX/lib/ruby/3.1.0/date.rb" ]] || die "Ruby runtime wrappers missing from prefix"

# --- 1. Stub libs for find_library('iconv') / find_library('charset') --------
# newlib's libc.a already exports iconv/iconv_open/iconv_close (verified with
# arm-vita-eabi-nm on this VitaSDK). Upstream src/meson.build does
#   compilers['cpp'].find_library('iconv')
#   compilers['cpp'].find_library('charset')
# on the non-Windows path. We ship a one-symbol dummy archive so -liconv
# resolves; real iconv_* resolve from libc at link time (verified: a program
# that calls iconv_open links). libcharset is linked by upstream meson but
# has no call sites in the sources we grepped.
#
# VitaSDK's gcc overrides LIBRARY_PATH, so meson find_library does NOT see
# env LIBRARY_PATH. Pass -L via a generated second cross file instead.
#
# Side-prefix deps (theora/uchardet/SDL2_sound, vita/scripts/build-vita-deps.sh):
# meson later-cross-file array options REPLACE earlier ones (machinefile.py
# reads all files into one ConfigParser — last key wins), so this generated
# file is self-contained: it repeats the Vita CPU flags from vita-cross.ini
# and adds the deps -I/-L plus the deps-aware pkg-config wrapper.
if [[ "$MODE" == "reconfigure" && -d "$BUILD_DIR" ]]; then
  echo "==> wiping $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$STUB_DIR" "$BUILD_DIR"
STUB_C="$STUB_DIR/mkxp_z_vita_stub.c"
if [[ ! -f "$STUB_DIR/libiconv.a" || ! -f "$STUB_DIR/libcharset.a" ]]; then
  echo "==> building iconv/charset stub archives in $STUB_DIR"
  cat > "$STUB_C" << 'EOF'
/* Dummy symbol so -liconv / -lcharset resolve. Real iconv_* live in newlib. */
void mkxp_z_vita_iconv_stub(void) {}
EOF
  STUB_O="$STUB_DIR/mkxp_z_vita_stub.o"
  arm-vita-eabi-gcc -c -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
    -o "$STUB_O" "$STUB_C"
  # D: deterministic member headers, so two configures of the same inputs are
  # byte-identical (the guard-sharing proof compares them).
  arm-vita-eabi-ar rcsD "$STUB_DIR/libiconv.a" "$STUB_O"
  arm-vita-eabi-ar rcsD "$STUB_DIR/libcharset.a" "$STUB_O"
fi

# Vita CPU flags must be repeated here: a later cross file's c_args/cpp_args/
# c_link_args/cpp_link_args replace vita-cross.ini's (see header comment).
VITA_CPU=(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
# -g: keep DWARF on the meson ELF so package-vpk.sh's unstripped
# side copy can symbolize hardware crash dumps (vita-parse-core / addr2line).
# Does not change codegen layout; release -O* stays as meson buildtype sets.
VITA_DEBUG=(-g)
# Function/data sections: every engine object is split so the
# link can drop unreferenced text/data. With MKXPZ_SOFTWARE_BITMAPS only a
# dozen pixman region functions are reachable, but the monolithic pixman
# objects drag the whole 1.6 MB accessor table in; -Wl,--gc-sections below
# keeps only the referenced sections. Verified against the custom linker
# script: initializer/finalizer arrays are KEEP()'d in armvita-1mb-data.ld;
# the converter roots and the receipt section are held by the explicit
# -Wl,--undefined roots in GC_ROOTS below.
VITA_SECTIONS=(-ffunction-sections -fdata-sections)
GC_SECTIONS=(-Wl,--gc-sections)
# GC roots we must name explicitly, applied on every link. sceLibcHeapSize/
# sceUserMainThreadStackSize are read by name from the converted VPK metadata
# (an LTO link drops them outright). mkxpz_build_receipt
# keeps the non-allocated receipt section alive: --gc-sections drops an
# unreferenced custom section, and package-vpk.sh
# reads the receipt back out of the ELF to pair it with the tree.
GC_ROOTS=(-Wl,--undefined=sceLibcHeapSize -Wl,--undefined=sceUserMainThreadStackSize
          -Wl,--undefined=mkxpz_build_receipt)
# LTO trial, opt-in because a device gate is still pending: LTO
# links and converts on the host and saves a further ~208 KiB beyond section
# GC, and the converter roots above stay named on the link line in that mode
# too. Device must confirm boot heaps/stacks and full engine loops before
# this becomes the default.
MKXPZ_LTO="${MKXPZ_LTO:-0}"
case "$MKXPZ_LTO" in
  0) MESON_LTO=() ;;
  1) MESON_LTO=(-Db_lto=true) ;;
  *) echo "error: MKXPZ_LTO must be 0 or 1" >&2; exit 1 ;;
esac

# --- Non-identifying file paths ----------------------------------
# -ffile-prefix-map rewrites the paths GCC bakes into __FILE__ *and* into
# DWARF. Both matter: -D_GLIBCXX_ASSERTIONS puts libstdc++ header paths into
# assertion text in .rodata, which `arm-vita-eabi-strip -g` cannot remove, so
# they ship inside eboot.bin. Use -ffile-prefix-map (= -fdebug-prefix-map plus
# -fmacro-prefix-map); the debug-only form leaves the .rodata strings behind.
# Codegen is unchanged — only path text moves — and the flags themselves do
# not appear in DW_AT_producer, so they leave no trace of the mapping.
#
# ORDERING IS LOAD-BEARING AND COUNTER-INTUITIVE: GCC applies the LAST
# matching map, so the most general prefix must come FIRST and the most
# specific LAST. Otherwise the SDK (which lives under the home directory) is
# swallowed by the home rule and you get /mkxpz-home/vitasdk/... Sorting by
# ascending path length produces exactly that order — a proper prefix is
# always strictly shorter — so entries may be registered in any order below.
# The four leak mechanisms: __FILE__ text, DWARF, libstdc++ assertion text and
# the compiled-in prefix strings of the dependencies.
PREFIX_MAP_LINES=""
add_prefix_map() {
  local from="$1" to="$2" real physical
  [[ -n "$from" && "$from" != "/" ]] || return 0
  physical=""
  [[ -d "$from" ]] && physical="$(cd "$from" && pwd -P)"
  for real in "$from" "$physical"; do
    [[ -n "$real" && "$real" != "/" ]] || continue
    # A path with whitespace cannot survive the shell-word contract these
    # arrays are built on; skip it rather than corrupt c_args.
    case "$real" in *[[:space:]]*) continue ;; esac
    PREFIX_MAP_LINES="$PREFIX_MAP_LINES${#real} -ffile-prefix-map=$real=$to
"
  done
}
add_prefix_map "${HOME:-}"        /mkxpz-home
add_prefix_map "$VITASDK"         /vitasdk
add_prefix_map "$ROOT"            /mkxp-z
add_prefix_map "$BUILD_DIR"       /mkxp-z-build
add_prefix_map "$RUBY_PREFIX"     /ruby-prefix
add_prefix_map "$DEPS_PREFIX"     /vita-deps
add_prefix_map "$SDL2_VITAGL_PREFIX" /sdl2-vitagl
add_prefix_map "$VITAGL_PREFIX"      /vitagl
add_prefix_map "$TSF_PREFIX"      /tinysoundfont
FILE_PREFIX_MAPS=()
while IFS= read -r map_flag; do
  [[ -n "$map_flag" ]] && FILE_PREFIX_MAPS+=("$map_flag")
done <<< "$(printf '%s' "$PREFIX_MAP_LINES" | sort -u | sort -n -k1,1 | cut -d' ' -f2-)"
# Never echo the mapped-from paths: they are exactly what must not be quoted.
echo "==> file path maps: ${#FILE_PREFIX_MAPS[@]} -ffile-prefix-map entries"
echo "    (home, SDK, checkout, engine source, build dir, Ruby prefix, deps,"
echo "     SDL2 prefix, vitaGL prefix -> neutral names; general first, specific last)"

HAVE_DEPS=0
DEPS_CFLAGS=""
DEPS_LDFLAGS=""
if [[ -d "$DEPS_PREFIX/lib" && -x "$DEPS_PKGCONFIG" ]]; then
  HAVE_DEPS=1
  DEPS_CFLAGS="-I$DEPS_PREFIX/include"
  DEPS_LDFLAGS="-L$DEPS_PREFIX/lib"
  echo "==> vita-deps side prefix: $DEPS_PREFIX"
  echo "    deps pkg-config:      $DEPS_PKGCONFIG"
else
  echo "==> vita-deps side prefix NOT ready (theora/uchardet/SDL2_sound)"
  echo "    expected prefix:  $DEPS_PREFIX"
  echo "    expected wrapper: $DEPS_PKGCONFIG"
  echo "    build them with:  vita/scripts/build-vita-deps.sh"
fi

# Combined pkg-config wrapper. Stock arm-vita-eabi-pkg-config
# clears PKG_CONFIG_PATH and pins PKG_CONFIG_LIBDIR to the VitaSDK sysroot,
# so a side-prefix ruby-3.1.pc is invisible to meson dependency('ruby-3.1').
# This wrapper prepends the MRI prefix (and vita-deps when present) to
# PKG_CONFIG_LIBDIR. --define-prefix recomputes prefix from each .pc's
# location, so both side prefixes resolve correctly.
MKXPZ_PKGCONFIG="$BUILD_DIR/arm-vita-eabi-pkg-config-mkxpz"
mkdir -p "$BUILD_DIR"
# Prepend the VIDEO_VITA_VGL SDL2 pkgconfig so dependency('sdl2') hits it
# rather than the stock vdpm sdl2 (GXM-only).
SDL2_PC_DIR=""
if [[ "$HAVE_SDL2_VITAGL" -eq 1 ]]; then
  SDL2_PC_DIR="$SDL2_VITAGL_PREFIX/lib/pkgconfig:"
fi
# Emit the wrapper to $1. Kept a self-contained function on purpose: it reads
# only VITASDK / SDL2_PC_DIR / RUBY_PREFIX / DEPS_PREFIX and writes one
# executable file, so it can be lifted out of
# this script verbatim and run against a fixture (or the real) .pc set.
# Same extraction trick as fetch_sdlsound() in vita/scripts/build-vita-deps.sh.
emit_pkgconfig_wrapper() {
  local out=$1
  {
    echo '#!/usr/bin/env bash'
    echo '# Generated by vita/scripts/configure-vita.sh — do not edit.'
    echo '# arm-vita-eabi-pkg-config plus MRI + vita-deps side prefixes.'
    echo "export VITASDK=\"\${VITASDK:-$VITASDK}\""
    echo "export PKG_CONFIG_LIBDIR=\"${SDL2_PC_DIR}$RUBY_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib/pkgconfig:\$VITASDK/arm-vita-eabi/lib/pkgconfig:\$VITASDK/arm-vita-eabi/share/pkgconfig\""
    echo 'export PKG_CONFIG_PATH='
    echo 'export PKG_CONFIG_DIR='
    echo 'export PKG_CONFIG_SYSROOT_DIR='
    # strip every pthread token from LIBRARY output. openal.pc and
    # harfbuzz.pc say "-pthread"; libwebp.pc, libsharpyuv.pc, libwebpdecoder.pc
    # and ruby-3.1.pc say "-lpthread" — and both forms land in the one
    # --start-group of the final link. -pthread is a *driver* flag: the GCC
    # spec file expands it to "--whole-archive -lpthread --no-whole-archive"
    # inside link_gcc_c_sequence, so libpthread.a is pulled member-for-member
    # at the end of the link, while the plain -lpthread in the group pulls the
    # same members on demand — every shared object (vita_osal.o and friends,
    # the only definitions of pte_osInit in the SDK) then appears twice and ld
    # reports ~84 "multiple definition" errors. The driver emits the
    # whole-archive block exactly once no matter how many -pthread tokens it
    # sees, so the fix is: no pthread token survives pkg-config, and the script
    # appends exactly one -pthread to c_link_args/cpp_link_args below.
    # Only --libs* invocations are filtered; --cflags output passes through
    # untouched so -pthread's _REENTRANT still reaches compiles.
    echo 'PC=(pkg-config --define-variable=VITASDK="$VITASDK" --define-prefix --static)'
    echo 'want_libs=0'
    echo 'for arg in "$@"; do'
    echo '  case "$arg" in --libs|--libs-only-*) want_libs=1 ;; esac'
    echo 'done'
    echo '[[ $want_libs -eq 1 ]] || exec "${PC[@]}" "$@"'
    echo 'libs=$("${PC[@]}" "$@") || exit $?'
    echo 'libs=" $libs "'
    echo 'while [[ $libs == *" -pthread "* ]]; do libs=${libs//" -pthread "/" "}; done'
    echo 'while [[ $libs == *" -lpthread "* ]]; do libs=${libs//" -lpthread "/" "}; done'
    # The VIDEO_VITA_VGL sdl2.pc names the STRONG SceShaccCg/SceKernelDmacMgr
    # stubs, whose imports demand those modules at process start; the closure
    # proven on hardware links the weak forms, so rewrite them before the
    # tokens reach the link line.
    echo 'while [[ $libs == *" -lSceShaccCg_stub "* ]]; do libs=${libs//" -lSceShaccCg_stub "/" -lSceShaccCg_stub_weak "}; done'
    echo 'while [[ $libs == *" -lSceKernelDmacMgr_stub "* ]]; do libs=${libs//" -lSceKernelDmacMgr_stub "/" -lSceKernelDmacMgr_stub_weak "}; done'
    echo 'libs=${libs# }'
    echo 'libs=${libs% }'
    echo 'printf "%s\n" "$libs"'
  } > "$out"
  chmod +x "$out"
}
emit_pkgconfig_wrapper "$MKXPZ_PKGCONFIG"
echo "==> pkg-config wrapper: $MKXPZ_PKGCONFIG (library output: no pthread tokens)"
echo "    PKG_CONFIG_LIBDIR:  $SDL2_VITAGL_PREFIX/lib/pkgconfig : $RUBY_PREFIX/lib/pkgconfig : $DEPS_PREFIX/lib/pkgconfig : sysroot"
[[ "$HAVE_SDL2_VITAGL" -eq 1 ]] ||
  echo "    note: vitaGL SDL2 not found at $SDL2_VITAGL_PREFIX (run vita/scripts/build-sdl2-vitagl.sh)"

STUB_CROSS="$BUILD_DIR/vita-stub-libs.ini"
# Helper: join shell words as meson single-quoted array entries.
meson_array() {
  local first=1 w
  for w in "$@"; do
    [[ -z "$w" ]] && continue
    if [[ $first -eq 1 ]]; then first=0; else printf ', '; fi
    printf "'%s'" "$w"
  done
}
C_ARGS=("${VITA_CPU[@]}" "${VITA_DEBUG[@]}" "${VITA_SECTIONS[@]}" ${FILE_PREFIX_MAPS[@]+"${FILE_PREFIX_MAPS[@]}"})
CPP_ARGS=("${VITA_CPU[@]}" "${VITA_DEBUG[@]}" "${VITA_SECTIONS[@]}" ${FILE_PREFIX_MAPS[@]+"${FILE_PREFIX_MAPS[@]}"})
# One word each: the inner quotes survive meson_array's single-quoting, so the
# compiler sees -DMKXPZ_VITA_VERSION="1.0.0" (a C string literal).
C_ARGS+=("-DMKXPZ_VITA_VERSION=\"$MKXPZ_VITA_VERSION\"")
CPP_ARGS+=("-DMKXPZ_VITA_VERSION=\"$MKXPZ_VITA_VERSION\"")
if [[ "$HAVE_TSF" -eq 1 ]]; then
  CPP_ARGS+=("-DMKXPZ_TSF" "-I$TSF_PREFIX")
fi
C_LINK=("${VITA_CPU[@]}" "-L$STUB_DIR" ${GC_SECTIONS[@]+"${GC_SECTIONS[@]}"} ${GC_ROOTS[@]+"${GC_ROOTS[@]}"})
CPP_LINK=("${VITA_CPU[@]}" "-L$STUB_DIR" ${GC_SECTIONS[@]+"${GC_SECTIONS[@]}"} ${GC_ROOTS[@]+"${GC_ROOTS[@]}"})
# The one pthread token in the whole link. emit_pkgconfig_wrapper
# above removes every -pthread/-lpthread the .pc files contribute; this puts
# exactly one back. -pthread (not -lpthread) on purpose: the driver expands it
# to --whole-archive -lpthread --no-whole-archive, which is the superset of
# on-demand extraction and is what openal.pc actually asks for — pthread-embedded
# has initialisers nothing references by name, and on-demand would drop them.
C_LINK+=("-pthread")
CPP_LINK+=("-pthread")
# Section GC: -ffunction-sections/-fdata-sections on every unit
# (VITA_SECTIONS above) plus -Wl,--gc-sections here. Symbolisation is
# per-build anyway, because package-vpk.sh pairs every eboot.bin with its own
# unstripped ELF by SHA-256, and the converter roots and 175
# startup names survive GC. Crash dumps from pre-GC builds must still
# be resolved against their own side ELF, not a newer one.
#
# Linker script: custom armvita-1mb-data.ld aligns the RW
# segment to 1 MiB so linked .data/.bss addresses match the loader's
# ALIGN_1MB(RX_end) placement. It supersedes __sce_headroom,
# which the loader ignored. Pair with vita-make-fself -na
# (package-vpk.sh) so RX is not ASLR-slid off 0x81000000.
LDSCRIPT="$ROOT/vita/linker/armvita-1mb-data.ld"
[[ -f "$LDSCRIPT" ]] || die "missing linker script: $LDSCRIPT"
C_LINK+=("-Wl,-T,$LDSCRIPT")
CPP_LINK+=("-Wl,-T,$LDSCRIPT")
echo "==> linker script: $LDSCRIPT (1 MiB RW align; no __sce_headroom)"
if [[ "$HAVE_DEPS" -eq 1 ]]; then
  C_ARGS+=("$DEPS_CFLAGS")
  CPP_ARGS+=("$DEPS_CFLAGS")
  C_LINK+=("$DEPS_LDFLAGS")
  CPP_LINK+=("$DEPS_LDFLAGS")
fi
# MRI prefix -L so find_library('ruby-static', dirs: mri_libpath) and any
# bare -lruby-static from the .pc resolve even when the archive sits only
# in the side prefix (meson does not put dependency lib dirs on the link
# line the way -L from a cross file does).
if [[ -f "$MRI_LIBPATH/lib${MRI_LIBRARY}.a" ]]; then
  C_LINK+=("-L$MRI_LIBPATH")
  CPP_LINK+=("-L$MRI_LIBPATH")
fi
# vitaGL: -I so gl-fun.h's <vitaGL.h> resolves (the backend's
# GLES2 header surface; launcher_gl.c includes it the same way); -L for
# libSDL2.a plus the vitaGL chain (sdl2.pc names -lvitaGL -lvitashark
# -lmathneon -lSceShaccCgExt -ltaihen_stub and the module stubs).
# MKXPZ_VITAGL_BACKEND is what gates on.
[[ "$HAVE_SDL2_VITAGL" -eq 1 ]] ||
  die "vitaGL backend needs the VIDEO_VITA_VGL SDL2 at $SDL2_VITAGL_PREFIX (run vita/scripts/build-sdl2-vitagl.sh)"
[[ -f "$VITAGL_PREFIX/lib/libvitaGL.a" ]] ||
  die "vitaGL backend needs the pinned vitaGL stack at $VITAGL_PREFIX (run vita/scripts/build-vitagl.sh)"
C_ARGS+=("-I$VITAGL_PREFIX/include" "-DMKXPZ_VITAGL_BACKEND=1")
CPP_ARGS+=("-I$VITAGL_PREFIX/include" "-DMKXPZ_VITAGL_BACKEND=1")
C_LINK+=("-L$SDL2_VITAGL_PREFIX/lib" "-L$VITAGL_PREFIX/lib")
CPP_LINK+=("-L$SDL2_VITAGL_PREFIX/lib" "-L$VITAGL_PREFIX/lib")
echo "==> vitaGL backend: SDL2 $SDL2_VITAGL_PREFIX + GL $VITAGL_PREFIX (-DMKXPZ_VITAGL_BACKEND)"
# Vita POSIX compat + ghc/filesystem OS.
# -I$ROOT/vita/include provides ifaddrs.h stub (cpp-httplib) and
# vita_posix_compat.h (symlink/readlink/utimensat/truncate stubs).
# -include force-injects the compat header into every TU.
# -DGHC_OS_DETECTED -DGHC_OS_LINUX make ghc/filesystem.hpp take its
# non-Windows path (Vita defines neither __linux__ nor __APPLE__).
VITA_INC="$ROOT/vita/include"
if [[ -d "$VITA_INC" ]]; then
  C_ARGS+=("-I$VITA_INC" "-include" "$VITA_INC/vita_posix_compat.h" "-DGHC_OS_DETECTED" "-DGHC_OS_LINUX")
  CPP_ARGS+=("-I$VITA_INC" "-include" "$VITA_INC/vita_posix_compat.h" "-DGHC_OS_DETECTED" "-DGHC_OS_LINUX")
  echo "==> Vita include: $VITA_INC (ifaddrs stub + posix compat + ghc OS defines)"
fi
# vita_glue.c is compiled into the executable by src/meson.build. It is NOT put
# on C_LINK/CPP_LINK: meson uses those args for every find_library() test, and
# vita_glue.o needs vitaGL symbols which those tests do not pull in, so it would
# fail SDL2_sound detection.
# Bind deferred measurements to the exact tree: the header carries a digest of
# the engine sources and the glue.
python3 "$ROOT/vita/scripts/treedigest.py" measure-header "$ROOT" "$BUILD_DIR/vita_measure_source.h"
C_ARGS+=("-include$BUILD_DIR/vita_measure_source.h")
# SceSysmem is required by vita_glue_log_free_memory; ScePower is also
# in sdl2.pc but named again here so the glue object always resolves.
# SceRtc is required by vita_glue_rtc_time_ms, the gap detector's
# wall-time clock.
C_LINK+=("-lScePower_stub" "-lSceSysmem_stub" "-lSceGxm_stub" "-lSceKernelModulemgr_stub" "-lSceRtc_stub")
CPP_LINK+=("-lScePower_stub" "-lSceSysmem_stub" "-lSceGxm_stub" "-lSceKernelModulemgr_stub" "-lSceRtc_stub")
echo "==> vita_glue: vitaGL stub closure + ScePower/SceSysmem/SceGxm/SceKernelModulemgr/SceRtc"
# Build receipt: what this configure feeds the link, carried INSIDE
# the ELF as a non-allocated section, so it never reaches eboot.bin's segments
# and cannot be paired with a different executable. package-vpk.sh reads
# it back and rechecks the dependency patches, the engine and vita/ sources and
# the Ruby archive against the tree it packs. Relative names only: the manifest ships in the VPK. The object
# sits on cpp_link_args, which ninja does not track, so a changed receipt drops
# the old ELF to force the relink.
RECEIPT_OBJ="$BUILD_DIR/mkxpz-build-receipt.o"
# The dependency tree digests cover the VIDEO_VITA_VGL SDL2 and the pinned
# vitaGL stack; "backend" lets the packager refuse an ELF built for another
# GL driver.
RECEIPT_DEPS=("sdl2Vitagl=$SDL2_VITAGL_PREFIX/lib" "vitagl=$VITAGL_PREFIX/lib" "vitaDeps=$DEPS_PREFIX/lib")
python3 - "$ROOT" "$BUILD_DIR" "$MKXPZ_PIN" "$MRI_LIBPATH/lib${MRI_LIBRARY}.a" "$OPTIONAL_SHADERS" \
  "$(arm-vita-eabi-gcc -dumpversion)" "$(meson --version)" "$HAVE_TSF:$TSF_PREFIX" \
  "$VITA_GL_BACKEND" "$MKXPZ_LTO" "${RECEIPT_DEPS[@]}" <<'PYRECEIPT'
import hashlib, json, subprocess, sys
from pathlib import Path
root, build = Path(sys.argv[1]), Path(sys.argv[2])
sys.dont_write_bytecode = True
sys.path.insert(0, str(root / "vita/scripts"))
import treedigest
pin, archive, shaders, gcc, meson, tsf, backend, lto = sys.argv[3:11]
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
tree_digest = treedigest.lib_digest
sources = [root / "vita/linker/armvita-1mb-data.ld", root / "vita/meson/vita-cross.ini"]
for folder in ("glue", "swraster", "overlay", "textpanel", "launcher", "include"):
    sources += [p for p in (root / "vita" / folder).rglob("*") if p.suffix in (".c", ".cpp", ".h")
                and not p.name.startswith("test_")]
git = lambda *args: subprocess.check_output(["git", "-C", str(root), *args], text=True)
have_tsf, tsf_prefix = tsf.split(":", 1)
receipt = {
    "schemaVersion": 1, "mkxpzPin": pin, "backend": backend,
    "sourceRevision": git("rev-parse", "HEAD").strip(),
    "sourceDirty": bool(git("status", "--porcelain")),
    "patches": {str(p.relative_to(root)): sha(p) for p in sorted(p for p in (root / "vita/patches").rglob("*") if p.is_file())},
    "engineSourcesSha256": treedigest.digest(root, treedigest.ENGINE),
    "sources": {str(p.relative_to(root)): sha(p) for p in sorted(sources)},
    "rubyArchiveSha256": sha(Path(archive)),
    "dependencies": {name: tree_digest(Path(lib).glob("*.a")) for name, lib in (a.split("=", 1) for a in sys.argv[11:])},
    "tinysoundfont": tree_digest([Path(tsf_prefix, n) for n in ("tsf.h", "tml.h")]) if have_tsf == "1" else None,
    "toolchain": {"gcc": gcc, "meson": meson}, "optionalShaders": shaders == "true",
    "mesonBuildtype": "release", "lto": lto == "1",
}
blob = json.dumps(receipt, sort_keys=True, separators=(",", ":")).encode()
target = build / "mkxpz-build-receipt.json"
if target.is_file() and target.read_bytes() != blob:
    for name in ("mkxp-z", "mkxp-z.cortex-a9"):
        (build / name).unlink(missing_ok=True)
target.write_bytes(blob)
rows = [",".join(str(b) for b in blob[i:i + 32]) for i in range(0, len(blob), 32)]
# The .globl symbol is the GC root: -Wl,--undefined=mkxpz_build_receipt keeps
# this otherwise unreferenced section from being collected.
(build / "mkxpz-build-receipt.s").write_text(
    '\t.section .mkxpz.build_receipt,"",%progbits\n'
    '\t.globl mkxpz_build_receipt\n'
    'mkxpz_build_receipt:\n'
    + "".join("\t.byte " + r + "\n" for r in rows))
PYRECEIPT
arm-vita-eabi-gcc "${VITA_CPU[@]}" -c -o "$RECEIPT_OBJ" "$BUILD_DIR/mkxpz-build-receipt.s" || die "could not assemble the build receipt"
CPP_LINK+=("$RECEIPT_OBJ")
echo "==> build receipt: $RECEIPT_OBJ (section .mkxpz.build_receipt, not loaded)"
{
  cat << EOF
# Generated by vita/scripts/configure-vita.sh — do not edit.
# Injects:
#   -L for iconv/charset stub archives
#   -I/-L for build/vita-deps/prefix (theora, uchardet, SDL2_sound) when built
#   -L for build/ruby-vita-prefix when libruby-static.a is installed
#   -I/-L for the vitaGL stack and its VIDEO_VITA_VGL SDL2
#   -ffile-prefix-map for every local root, so no build path reaches .rodata
#     or DWARF (general prefix first, specific last — GCC last match)
#   pkg-config wrapper that searches the vitaGL SDL2 + MRI + vita-deps side prefixes
# Later cross-file array options REPLACE vita/meson/vita-cross.ini, so the
# Vita CPU flags are repeated here.
[binaries]
pkg-config = '$MKXPZ_PKGCONFIG'

[built-in options]
EOF
  printf 'c_args = [%s]\n'       "$(meson_array "${C_ARGS[@]}")"
  printf 'cpp_args = [%s]\n'     "$(meson_array "${CPP_ARGS[@]}")"
  printf 'c_link_args = [%s]\n'  "$(meson_array "${C_LINK[@]}")"
  printf 'cpp_link_args = [%s]\n' "$(meson_array "${CPP_LINK[@]}")"
} > "$STUB_CROSS"
echo "==> stub cross file: $STUB_CROSS"

# --- 4. Preflight: report every hard dep, not just the first meson stop ------
# Hard deps from src/meson.build + binding/meson.build at this pin.
# Use the combined wrapper so preflight matches what meson will see.
PC="$MKXPZ_PKGCONFIG"
preflight_pkg() {
  local name=$1
  if $PC --exists "$name" 2>/dev/null; then
    echo "  ok      $name $($PC --modversion "$name" 2>/dev/null)"
  else
    echo "  MISSING $name"
  fi
}
preflight_lib() {
  local name=$1 note=$2
  local search=()
  search+=(-L"$STUB_DIR")
  [[ "$HAVE_DEPS" -eq 1 ]] && search+=(-L"$DEPS_PREFIX/lib")
  if arm-vita-eabi-g++ "${search[@]}" \
     -print-file-name="lib${name}.a" 2>/dev/null \
     | grep -q '^/'; then
    echo "  ok      lib${name}.a  ($note)"
  elif [[ -f "$STUB_DIR/lib${name}.a" ]]; then
    echo "  stub    lib${name}.a  ($note)"
  elif [[ -f "$DEPS_PREFIX/lib/lib${name}.a" ]]; then
    echo "  deps    lib${name}.a  ($note; $DEPS_PREFIX/lib)"
  else
    echo "  MISSING lib${name}.a  ($note)"
  fi
}

echo "==> preflight hard deps (vdpm pkg-config + find_library + vita-deps)"
preflight_pkg physfs
preflight_pkg openal
preflight_pkg theora
preflight_pkg vorbisfile
preflight_pkg vorbis
preflight_pkg ogg
preflight_pkg SDL2
preflight_lib SDL2_sound "find_library; mkxp-z fork via vita/scripts/build-vita-deps.sh"
preflight_pkg SDL2_ttf
preflight_pkg freetype2
preflight_pkg pixman-1
preflight_pkg libpng
preflight_pkg zlib
preflight_pkg uchardet
preflight_pkg SDL2_image
preflight_lib bz2 "vdpm via sdl2_image"
preflight_lib iconv "newlib libc provides iconv_*; stub satisfies find_library"
preflight_lib charset "linked by upstream meson; no call sites found"
if [[ -f "$MRI_LIBPATH/lib${MRI_LIBRARY}.a" ]]; then
  echo "  ok      lib${MRI_LIBRARY}.a  (MRI $MRI_INCLUDES)"
else
  echo "  MISSING lib${MRI_LIBRARY}.a  (planned: $MRI_LIBPATH — run vita/scripts/build-ruby-vita.sh install-prefix)"
fi
# ruby-3.1.pc: the dependency('ruby-3.1') path (mri_includes left empty).
# Libs must mention the static archive — upstream template does not.
if [[ -f "$RUBY_PREFIX/lib/pkgconfig/ruby-3.1.pc" ]]; then
  if grep -q 'lruby-static' "$RUBY_PREFIX/lib/pkgconfig/ruby-3.1.pc"; then
    if $PC --exists ruby-3.1 2>/dev/null; then
      echo "  ok      ruby-3.1.pc  ($($PC --modversion ruby-3.1 2>/dev/null); $($PC --libs ruby-3.1 2>/dev/null))"
    else
      echo "  MISSING ruby-3.1  (file exists at $RUBY_PREFIX/lib/pkgconfig but wrapper cannot see it)"
    fi
  else
    echo "  stale   ruby-3.1.pc  (no -lruby-static in Libs; re-run vita/scripts/build-ruby-vita.sh install-prefix)"
  fi
else
  echo "  MISSING ruby-3.1.pc  (planned: $RUBY_PREFIX/lib/pkgconfig — run vita/scripts/build-ruby-vita.sh install-prefix)"
fi
# Flat config.h for single -I mri_includes (upstream issue #187).
if [[ -f "$MRI_INCLUDES/ruby/config.h" ]]; then
  echo "  ok      $MRI_INCLUDES/ruby/config.h  (flat arch config; single -I mri_includes)"
elif [[ -f "$MRI_INCLUDES/arm-eabi/ruby/config.h" ]]; then
  echo "  note    arch config.h only under arm-eabi/; mri_includes alone will miss it (need install-prefix)"
else
  echo "  MISSING config.h under $MRI_INCLUDES  (run vita/scripts/build-ruby-vita.sh install-prefix)"
fi
echo "  note    fluidsynth skipped (-Dshared_fluid=false); Discord is not a meson option at this pin"
if [[ "$HAVE_DEPS" -eq 1 ]]; then
  echo "  note    theora/uchardet/SDL2_sound from $DEPS_PREFIX (vita/scripts/build-vita-deps.sh)"
else
  echo "  note    theora/uchardet/SDL2_sound NOT built — run vita/scripts/build-vita-deps.sh"
fi

# --- 5. Meson setup ----------------------------------------------------------
# Exact option set for
#   gfx_backend=gles  ; -DGLES2_HEADER, no gl.pc needed
#   enable-https=false   non-goal; drops OpenSSL
#   mri_version=3.1
#   use_miniffi=true     default; the one optional feature we keep
#   shared_fluid=false   no dynamic fluidsynth; TSF_PREFIX enables the static shim
#   cjk_fallback_font=false
#   cxx11_experimental=false  use bundled ghc/filesystem
#   workdir_current=false
#   static_executable=true    -static-libgcc/-static-libstdc++, AL_LIBTYPE_STATIC
#   appimage=false
#   steamworks_path='' / steam_appid=''  Steam compiled out
#   force32=false
#   mri_includes/libpath/library  manual MRI; bypasses dependency('ruby-3.1').
#       install-prefix also drops ruby-3.1.pc so the dependency() path works
#       when mri_includes is left empty.
#   software_bitmaps=true: CPU-authoritative
#       Bitmap backend. Every Bitmap keeps its pixels in client memory, mutation
#       runs through vita/swraster, and the GL texture is a lazily uploaded
#       sample-only cache — so a Bitmap never owns a GPU-resident render
#       surface.
#   vita_launcher=true: one eboot, three modes.
#       With no --game and no pinned root config the player IS the launcher —
#       the game list drawn with SDL2 alone, no Config, no audio, no Ruby — and
#       picking a game is sceAppMgrLoadExec of this same eboot. A player whose
#       root config names a gameFolder or a customScript (a player packaged for one
#       game) is unaffected and still exits to LiveArea.
MESON_OPTS=(
  --cross-file "$CROSS_FILE"
  --cross-file "$STUB_CROSS"
  --buildtype release
  ${MESON_LTO[@]+"${MESON_LTO[@]}"}
  -Dgfx_backend=gles
  -Denable-https=false
  -Doptional_shaders="$OPTIONAL_SHADERS"
  -Dsoftware_bitmaps=true
  -Dvita_launcher=true
  -Dmri_version=3.1
  -Dmri_includes="$MRI_INCLUDES"
  -Dmri_libpath="$MRI_LIBPATH"
  -Dmri_library="$MRI_LIBRARY"
  -Duse_miniffi=true
  -Dshared_fluid=false
  -Dcjk_fallback_font=false
  -Dcxx11_experimental=false
  -Dworkdir_current=false
  -Dstatic_executable=true
  -Dappimage=false
  -Dappimagekit_path=
  -Dsteamworks_path=
  -Dsteam_appid=
  -Dsteamshim_debug=false
  -Dforce32=false
)

echo "==> meson setup"
echo "    src=$MKXPZ_SRC"
echo "    build=$BUILD_DIR"
echo "    cross=$CROSS_FILE + $STUB_CROSS"
echo "    VITASDK=$VITASDK"
echo "    meson options:"
for o in "${MESON_OPTS[@]}"; do
  case "$o" in
    --cross-file|-D*) echo "      $o" ;;
  esac
done

set +e
meson setup "$BUILD_DIR" "$MKXPZ_SRC" "${MESON_OPTS[@]}"
rc=$?
set -e

echo
if [[ $rc -eq 0 ]]; then
  echo "meson setup: SUCCESS"
  echo "  next: ninja -C $BUILD_DIR   (link will need libruby-static + remaining deps)"
else
  echo "meson setup: FAILED (exit $rc)"
  echo "  Expected until the MISSING preflight lines above are resolved"
  echo "  (vita/scripts/build-ruby-vita.sh install-prefix for MRI;"
  echo "   vita/scripts/build-vita-deps.sh if theora/uchardet/SDL2_sound missing)."
  echo "  Meson stops at the first missing dependency — see ERROR and preflight."
  if [[ -f "$BUILD_DIR/meson-logs/meson-log.txt" ]]; then
    echo "  log: $BUILD_DIR/meson-logs/meson-log.txt"
  fi
fi
exit $rc
