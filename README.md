# mkxp-z for PS Vita

A native player for **RPG Maker XP, VX and VX Ace** games on homebrew PS Vita.

This is a fork of [mkxp-z](https://github.com/mkxp-z/mkxp-z) adapted to run on the Vita: the same C++
RGSS runtime and MRI Ruby, rebuilt with VitaSDK on vitaGL, SDL2, OpenAL and PhysFS, plus a game-list
launcher, controller and touch input, and handling for standby and resume. It plays games from their
original, unmodified files; RPG Maker MV and MZ games are not supported (they are not RGSS).

**[Download the latest release](https://github.com/GildedLotus/mkxp-z-vita-fork/releases/latest)** ·
[Player guide](vita/README.md) · [Compatibility](vita/COMPATIBILITY.md) ·
[Configuration](vita/docs/config.md) · [Contributing](vita/CONTRIBUTING.md)

## Quick start

1. Install `mkxp-z.vpk` from the latest release with VitaShell, then start the **mkxp-z** bubble.
2. Copy each game folder (the one containing `Game.ini`) to `ux0:/data/mkxp-z/games/<Name>/` using
   VitaShell's **USB** mode (FTP can mangle Japanese file names).
3. If a game needs its RPG Maker RTP, extract it on a computer and copy it to
   `ux0:/data/mkxp-z/rtp/XP/`, `VX/` or `VXAce/`.
4. For MIDI music (all XP RTP music is MIDI), put a General MIDI `.sf2` SoundFont in
   `ux0:/data/mkxp-z/sf2/`.

Pick a game from the list to play it; hold **Start + Select** for two seconds to quit back to the list.
The [player guide](vita/README.md) covers the folder layout, controls, settings, logs and how to report
a problem.

## Status

Version 1.0.2. Six free games across all three engines reach gameplay on a retail PS Vita, three of
them with save, quit and reload verified. That is not yet a claim that whole games play through to the
end; the per-game results and the port's known limits are in [COMPATIBILITY.md](vita/COMPATIBILITY.md).

## Building from source

Needs a macOS or Linux host with [VitaSDK](https://vitasdk.org). `vita/scripts/build-player.sh` builds
the VPK end to end; prerequisites, pinned dependency revisions and the release build are described in
[vita/README.md](vita/README.md#building).

## Licence and credits

The combined Vita build is distributed under the GNU GPL, version 3; the port's own files are
GPL-3.0-or-later ([LICENSE](LICENSE)). mkxp-z is GPL-2.0-or-later ([COPYING](COPYING), unchanged).
Every bundled component, its licence and its upstream are listed in [THIRD-PARTY.md](THIRD-PARTY.md).
RPG Maker RTPs, games and Sony's libraries are never bundled.

This port stands on [mkxp-z](https://github.com/mkxp-z/mkxp-z) by the mkxp-z contributors, itself a
fork of [mkxp](https://github.com/Ancurio/mkxp) by Amaryllis Kulla (Ancurio), and on
[VitaSDK](https://vitasdk.org), [vitaGL](https://github.com/Rinnegatamante/vitaGL) and the other
projects credited in [vita/README.md](vita/README.md#credits). For the desktop (Windows, Linux, macOS)
builds, use upstream mkxp-z.

Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All rights
reserved.
