#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a matching host MRI 3.1.3 (mkxp-z/ruby fork) for later --with-baseruby.
#
# This is a *native* macOS (or Linux) build. It does not cross-compile for Vita.
# The installed interpreter is the baseruby that build-ruby-vita.sh passes as
# --with-baseruby. Prefix is local, never /usr or /usr/local.
#
# Usage:
#   vita/scripts/build-host-ruby.sh
#   MKXPZ_HOST_RUBY_PREFIX=/path/to/prefix JOBS=8 vita/scripts/build-host-ruby.sh
#
# Defaults:
#   MKXPZ_HOST_RUBY_PREFIX  $HOME/.local/ruby-3.1.3-mkxpz
#   MKXPZ_HOST_RUBY_SRC     <repo>/build/ruby-src          (gitignored; must be inside <repo>/build)
#   pin     mkxp-z/ruby @ 4d85560cf65938d7883a323bf553acad1faf5eae
#           (branch mkxp-z-3.1.3)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

RUBY_REPO="${RUBY_REPO:-https://github.com/mkxp-z/ruby.git}"
RUBY_BRANCH="${RUBY_BRANCH:-mkxp-z-3.1.3}"
RUBY_REF="${RUBY_REF:-4d85560cf65938d7883a323bf553acad1faf5eae}"
SRC="${MKXPZ_HOST_RUBY_SRC:-$ROOT/build/ruby-src}"
PREFIX="${MKXPZ_HOST_RUBY_PREFIX:-$HOME/.local/ruby-3.1.3-mkxpz}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

# Bootstrap ruby used only to generate files *while building* this host ruby.
# Avoid asdf/homebrew rubies (3.2+) as BASERUBY for a 3.1 tree.
if [[ -z "${BOOTSTRAP_RUBY:-}" ]]; then
  if [[ -x /usr/bin/ruby ]]; then
    BOOTSTRAP_RUBY=/usr/bin/ruby
  else
    BOOTSTRAP_RUBY="$(command -v ruby)"
  fi
fi

die() { echo "error: $*" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/vita/scripts/dep-pins.sh"
# The source directory is reset (clean -fdx) or deleted below.
require_under_build MKXPZ_HOST_RUBY_SRC "$SRC"

case "$PREFIX" in
  /usr|/usr/local|/usr/local/*|/opt/homebrew|/opt/homebrew/*)
    die "refusing system prefix '$PREFIX' (set PREFIX to a user path)"
    ;;
esac

# Keg-only bison 3.x is required: macOS /usr/bin/bison is 2.3 and cannot
# process MRI 3.1 parse.y. autoconf 2.72 is already on this machine.
# Drop asdf/rbenv shims so #!/usr/bin/env ruby in tool/ is not 3.3.
PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

command -v autoconf >/dev/null || die "autoconf not found"
command -v bison >/dev/null || die "bison not found (need GNU bison 3; brew install bison)"
bison_ver="$(bison --version | head -n1)"
bison_major="$(printf '%s\n' "$bison_ver" | awk '{
  for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+/) { split($i, a, "."); print a[1]; exit }
}')"
[[ "${bison_major:-0}" -ge 3 ]] || die "need GNU bison 3+, got: $bison_ver"

unset RUBYOPT RUBYLIB GEM_HOME GEM_PATH BUNDLE_GEMFILE BUNDLE_PATH || true

mkdir -p "$ROOT/build"

if [[ -d "$SRC/.git" ]]; then
  git -C "$SRC" fetch --quiet origin "$RUBY_BRANCH" || git -C "$SRC" fetch --quiet origin
  git -C "$SRC" checkout --quiet --detach "$RUBY_REF"
  git -C "$SRC" reset --hard --quiet "$RUBY_REF"
  git -C "$SRC" clean -fdx --quiet
else
  rm -rf "$SRC"
  git clone --quiet --branch "$RUBY_BRANCH" --single-branch "$RUBY_REPO" "$SRC"
  git -C "$SRC" checkout --quiet --detach "$RUBY_REF"
fi

actual="$(git -C "$SRC" rev-parse HEAD)"
[[ "$actual" == "$RUBY_REF" ]] || die "source HEAD $actual != pin $RUBY_REF"

# Strip the macOS preload assignment before autoreconf. autoconf 2.72
# accepted it here, but keep the same guard so a
# future autoconf does not treat the default as a preprocessor macro.
if [[ "$(uname -s)" == Darwin ]]; then
  sed -i.bak '/: ${PRELOADENV=DYLD_INSERT_LIBRARIES}/d' "$SRC/configure.ac"
  rm -f "$SRC/configure.ac.bak"
fi

cd "$SRC"
autoconf
[[ -f configure ]] || die "autoconf did not produce ./configure"

# AC_CONFIG_AUX_DIR(tool) looks for these *before* configure.ac's downloader
# runs, so put them in place up front: the pinned, digest-checked copies.
fetch_gnu_config "$SRC/tool" "$ROOT/build/vita-deps/dist"

# Static-friendly enough to serve as baseruby: static libruby is still
# installed, extensions are folded in. --enable-shared is required on
# Darwin with Apple clang/ld 21 (--disable-shared dies at the second
# ruby link with "ld: -bundle_loader missing <path>").
# --enable-bundled-libyaml is consumed by ext/psych/extconf.rb so the
# binary does not depend on Homebrew libyaml.
# socket is omitted: MRI 3.1 extconf opens /usr/include/netinet6/in6.h,
# which this SDK does not ship. Not needed for baseruby.
CONFIGURE_ARGS=(
  --prefix="$PREFIX"
  --enable-install-static-library
  --enable-shared
  --with-static-linked-ext
  --disable-install-doc
  --disable-rubygems
  --disable-jit-support
  --without-gmp
  --enable-bundled-libyaml
  --with-out-ext=openssl,readline,dbm,gdbm,fiddle,win32ole,win32,socket
  --with-baseruby="$BOOTSTRAP_RUBY"
)

# 3.1.x is not valid C23. Harmless on Apple clang, required for GCC 15+.
export CFLAGS="-std=gnu99 -O2 ${CFLAGS:-}"

echo "==> configure ${CONFIGURE_ARGS[*]}"
echo "==> CFLAGS=$CFLAGS"
echo "==> BASERUBY=$BOOTSTRAP_RUBY ($("$BOOTSTRAP_RUBY" -v))"
echo "==> prefix=$PREFIX"

./configure "${CONFIGURE_ARGS[@]}"

make -j"$JOBS"
make install

[[ -x "$PREFIX/bin/ruby" ]] || die "missing $PREFIX/bin/ruby"
got_ver="$("$PREFIX/bin/ruby" -e 'print RUBY_VERSION')"
[[ "$got_ver" == 3.1.3 ]] || die "expected RUBY_VERSION 3.1.3, got $got_ver"

info="$PREFIX/mkxpz-host-ruby-buildinfo.txt"
{
  echo "prefix=$PREFIX"
  echo "source=$RUBY_REPO"
  echo "branch=$RUBY_BRANCH"
  echo "ref=$RUBY_REF"
  echo "actual=$(git -C "$SRC" rev-parse HEAD)"
  echo "configure=${CONFIGURE_ARGS[*]}"
  echo "CFLAGS=$CFLAGS"
  echo "bootstrap_ruby=$BOOTSTRAP_RUBY"
  echo "bootstrap_ruby_version=$("$BOOTSTRAP_RUBY" -v)"
  echo "bison=$bison_ver"
  echo "host=$(uname -s) $(uname -m)"
  echo "built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "ruby_v=$("$PREFIX/bin/ruby" -v)"
  echo "RUBY_VERSION=$("$PREFIX/bin/ruby" -e 'print RUBY_VERSION')"
  echo "RUBY_PLATFORM=$("$PREFIX/bin/ruby" -e 'print RUBY_PLATFORM')"
} > "$info"

echo
echo "==> installed"
echo "    $PREFIX/bin/ruby"
"$PREFIX/bin/ruby" -v
"$PREFIX/bin/ruby" -e 'puts "RUBY_VERSION=#{RUBY_VERSION} platform=#{RUBY_PLATFORM}"'
echo "    buildinfo: $info"
echo
echo "Later (Vita cross): ./configure --with-baseruby=$PREFIX/bin/ruby ..."
