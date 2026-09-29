<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Contributing

Developed with AI coding assistants under maintainer review.

## Build

A macOS or Unix host with VitaSDK (`VITASDK`, or `$HOME/vitasdk`).
`vita/scripts/build-player.sh` builds the vitaGL player end to end and never
deploys; the steps, pins and outputs are in the [README](README.md#building).
Test builds use a `MKXPZ00xx` title id, never the product id `MKXPZ0001`.
`vita/scripts/build-release.sh` is the release path and refuses a dirty tree.

## Checking a change

This branch carries no test suite; a change is checked by building the VPK and
running it. Claims about the device (frame times, memory, standby behaviour, a
game's state) come from runs on a real PS Vita, not from an emulator or a host
build. A row in [COMPATIBILITY.md](COMPATIBILITY.md) changes only after such a
run, and the commit that changes it describes the run.

## Rules for engine and platform changes

- Reserve render surfaces, shaders and variants, and the default VAO at boot.
  Never recreate gameplay surfaces through resize. Keep Bitmaps
  CPU-authoritative and texture caches bounded.
- Keep syscalls on the thread's registered stack. Ruby fibers stay in the owning
  rgss arena. Never rely on a malloc fallback or on a safe C-stack overflow.
- No `/` or `%` by a non-constant inside a pixel loop: the Cortex-A9 has no
  hardware integer divide.
- `poll()` and `select()` need sceNet and are unavailable in the player.
- Bound kernel-object counts and simultaneous allocation peaks; measure capacity
  before adopting a per-game-object OS resource.
- Preserve non-regenerable files on failure: Vita libc temp+rename does not
  guarantee atomic replacement.
- Test builds never use `TITLE_ID MKXPZ0001`.

## Dependency patches

Fixes to the pinned dependencies (vitaGL and its companions, SDL2, Ruby) are patch
files under `vita/patches/`, applied by the build scripts to a fresh source
checkout at the pinned revision; never edit a downloaded source tree in place.
Keep a patch header short: what it changes and why. Changing a
`vita/patches/vitagl/` file changes the hash recorded in
`vita/vitagl-shaders/MANIFEST`: review the change, then run

```bash
python3 vita/scripts/vitagl-shaders.py manifest --source . --dir vita/vitagl-shaders
```

(see [vita/vitagl-shaders/README.md](vitagl-shaders/README.md)). A change to any
GLSL source, or to the launcher's shader strings, changes the shipped shader
cache keys and needs the cache recaptured on a device.

## Size

Lines are a cost every later reader pays: extend an existing file before adding
one, keep comments to constraints the code cannot show, and never commit build
outputs, logs, fixtures above 50 KB, game data or fonts.
