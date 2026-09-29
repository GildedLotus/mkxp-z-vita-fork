#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Capture the precompiled GXP boot shaders after a device boot WITH libshacccg.
#   capture-vitagl-shaders.sh --ftp HOST[:PORT] --title MKXPZ00xx   pull ux0:/data/shader_cache/<title>
#   capture-vitagl-shaders.sh --from-dir <pulled cache root holding v2/>
# Output: $VITAGL_SHADERS_DIR (default vita/vitagl-shaders, the tracked set the release ships). Extra arguments go to
# vita/scripts/vitagl-shaders.py capture; the shader sources come from this tree. Capture refuses
# malformed or wrong-stage files before touching the set, then writes MANIFEST (the vitaGL pins and patch series the
# capturing build must have been built from: capture from a package built by THIS tree).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 -B "$ROOT/vita/scripts/vitagl-shaders.py" capture \
	--source "$ROOT" \
	--dir "${VITAGL_SHADERS_DIR:-$ROOT/vita/vitagl-shaders}" "$@"
