# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by build-player.sh and package-vpk.sh: the product package (MKXPZ_RELEASE=1) comes only from a
# clean committed tree, and only through vita/scripts/build-release.sh. Needs ROOT and die().
# shellcheck shell=bash

RELEASE_TOKEN_FILE="$ROOT/build/.release-token"

release_gate() {
  local porcelain
  porcelain=$(git -C "$ROOT" status --porcelain) || die "MKXPZ_RELEASE=1 needs a git checkout at $ROOT"
  [[ -z $porcelain ]] || die "MKXPZ_RELEASE=1 refuses a dirty tree (git status --porcelain is not empty); commit or discard the changes"
  [[ -n ${MKXPZ_RELEASE_TOKEN:-} && -f $RELEASE_TOKEN_FILE && "$(cat "$RELEASE_TOKEN_FILE")" == "$MKXPZ_RELEASE_TOKEN" ]] ||
    die "MKXPZ_RELEASE=1 is set only by vita/scripts/build-release.sh; run that script for a release package"
}
