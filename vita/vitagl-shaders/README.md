<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# vitaGL precompiled shaders

`v2/{v,f}/XX/<HEX>.gxp`: the vitaGL shader-cache binaries (30 stages, 24 programs, 38,012 bytes) that the
vitaGL backend loads from `app0:/shader_cache/` at boot. They are compiler output: the Sony shader compiler
(`libshacccg.suprx`) built them on a Vita from this tree's GLSL (`shader/`). Cache keys are XXH3_64
hashes of the shader source, so a changed source is a different file.

Regenerate after any shader source change (stale files are never loaded, and a missing stage compiles at
runtime, which needs `libshacccg.suprx` on the device):

- Capture: `vita/scripts/capture-vitagl-shaders.sh --ftp HOST[:PORT] --title MKXPZ00xx` after a device boot with
  libshacccg installed (or `--from-dir <pulled cache root holding v2/>`). It writes here by default.
- Host check: `python3 -B vita/scripts/vitagl-shaders.py check --source . --dir vita/vitagl-shaders`.
  It parses every file as vitaGL reads it (matrix table, binding table, GXP program of the right stage,
  sizes, canonical path), and compares `MANIFEST` with this tree.

`MANIFEST` records what the compiler output depends on besides the GLSL text: the vitaGL, vitaShaRK and
SceShaccCgExt pins, the vitaGL build knobs, the hash of each `vita/patches/vitagl/vitagl-*.patch` and of the SceShaccCgExt files under `vita/patches/vitagl/shacccgext-files/`, and
the hash of the set. Any change to those fails the check, so a re-pin or a translator/cache patch forces a recapture.
After a vitaGL patch that cannot change compiled shaders (an allocation fix, say), review it and run
`vita/scripts/vitagl-shaders.py manifest --source . --dir vita/vitagl-shaders`; that only attests, and its diff
shows what changed. `capture` writes `MANIFEST` itself.

The boot-time final-presentation probe (`app0:/diagnostics/final-presentation-probe`) changes SimpleShader's
source, hence its key: a package with the marker is not covered by this set and needs libshacccg on the device
(the packager asks for `ALLOW_MISSING_SHADERS=1`). An ELF built with `MKXPZ_OPTIONAL_SHADERS=1` is refused by
its build receipt.

The packager takes this directory by default (`VITAGL_SHADERS_DIR`); `ALLOW_MISSING_SHADERS=1` is for capture
builds only and is refused by the release.
