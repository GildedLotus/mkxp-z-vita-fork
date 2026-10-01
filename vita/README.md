<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# mkxp-z on PS Vita

A native player for RPG Maker XP, VX and VX Ace games on homebrew PS Vita,
built by porting [mkxp-z](https://github.com/mkxp-z/mkxp-z). It keeps mkxp-z's
C++ RGSS implementation and MRI Ruby, with SDL2, vitaGL (GLES2 on the Vita's
GXM), OpenAL and PhysFS underneath. This directory holds the platform layer,
the build recipes and the packaging; the engine changes are in `src/`,
`binding/` and `shader/` of this branch.

**Status.** The package is a launcher: install the VPK, copy games to the
memory card, pick one, play. Six free RPG Maker titles (two per engine,
including a Japanese original) have reached in-game on a retail PS Vita, three of
them with save, quit and fresh-process reload loops. Those three were re-verified
on the vitaGL build this branch ships. That establishes neither completed games
nor universal compatibility. Every row, and what has not been checked, is in
[COMPATIBILITY.md](COMPATIBILITY.md).

---

# Playing games

## 1. What you need

| | |
|---|---|
| A PS Vita running homebrew | Only ever run on a **retail PS Vita**. No firmware version is recorded and no PS TV run has been made: treat both as untested. |
| VitaShell | Installs the VPK and, in USB mode, copies game files (see [§6](#6-copying-files-use-vitashell-usb-not-ftp)). |
| libshacccg (optional) | The VPK ships precompiled shader binaries, so a player needs nothing else. Only a shader missing from the shipped set (a modified build) has to be compiled on the device, and that needs Sony's `libshacccg.suprx`, which is not distributed here. |
| Free space on `ux0:` | About 23 MB installed (the VPK is about 12.6 MB and unpacks to about 23 MB). Games and RTPs come on top. |
| A host computer | To build the player and to extract an RTP (macOS or Linux with VitaSDK). |

## 2. Install the player

1. Download `mkxp-z.vpk` from the
   [GitHub Releases page](https://github.com/GildedLotus/mkxp-z-vita-fork/releases).
2. Copy it to the card (VitaShell USB mode is enough) and install it from
   VitaShell.
3. Start the bubble from LiveArea.

It boots into the **launcher**: its root configuration pins no game, so it draws
the list of games found under `ux0:/data/mkxp-z/games`. The first launch creates
`ux0:/data`, `ux0:/data/mkxp-z` and `ux0:/data/mkxp-z/logs`. It does **not**
create `games/` or `rtp/`: make those yourself. To build the VPK from source
instead, see [Building](#building).

## 3. Adding games

One `eboot.bin` decides at boot whether it is a game list or a player, hands
over to the picked game with `sceAppMgrLoadExec`, and comes back to the list
when the game exits ([docs/launcher.md](docs/launcher.md)).

1. Copy the game folder (the one *containing* `Game.ini`) into
   `ux0:/data/mkxp-z/games/<Name>/` with VitaShell in USB mode
   ([§6](#6-copying-files-use-vitashell-usb-not-ftp) is a rule, not a preference).
2. Launch the bubble. The game appears in the list; select it and it starts. Its
   own `Game.ini` sets its RGSS version, and saves land next to the game.
3. A game whose executable is not named `Game` still works: the launcher
   detects it from its own `.ini`/archive pair (Pocket Mirror ships
   `Pocket Mirror.ini` and `Pocket Mirror.rgss3a` and no `Game.ini`).

Launcher controls: Up/Down (D-pad or left stick) move the selection, Cross or
Circle starts the selected game, Triangle rescans the games folder, and L / R
turn the page. A game whose `Game.ini` names an RTP that is not installed under
`ux0:/data/mkxp-z/rtp/` is tagged "RTP missing".

Launcher limits: at most 256 games are listed; folders whose names start with
`.` are ignored; a game whose full path exceeds 255 bytes is skipped; and a
title or folder name too wide for its column is shortened with an ellipsis.

## 4. The folder layout on the card

```text
ux0:/data/mkxp-z/
├── config.json                 your global settings (optional)
├── games/
│   ├── AoOni/                  one folder per game, as the game ships
│   ├── BlankDream/
│   └── Pocket Mirror/          spaces are fine
├── rtp/
│   ├── XP/                     extracted RPG Maker XP RTP
│   ├── VX/                     extracted RPG Maker VX RTP
│   └── VXAce/                  extracted RPG Maker VX Ace RTP
├── sf2/                        General MIDI SoundFont, .sf2 (optional, see §7)
├── fonts/                      extra .ttf faces (optional)
└── logs/                       runtime logs
```

Each `games/<Name>/` is the game folder **as shipped**: `Game.ini`, `Data/` (or
`Game.rgssad` / `.rgss2a` / `.rgss3a`), `Graphics/`, `Audio/`, and its own
`mkxp.json` if it has one. Copy the folder that *contains* `Game.ini`, not its
parent. Things that are easy to get wrong:

- **The RGSS version is read from the `Scripts=` line of `Game.ini`, not from
  `Library=`.** `.rxdata` means XP, `.rvdata` VX and `.rvdata2` VX Ace. The
  launcher falls back to `Library=` only for the engine tag it displays when
  `Scripts=` is missing.
- **Saves are written next to the game**, in the same `games/<Name>/` tree, as
  MRI `Marshal` files: the same format a PC copy writes. That includes games
  that save through `System.data_directory` (Pokémon Essentials), which returns
  the running game's own folder. Nothing recovers, rotates or protects a game's
  save; copy saves off the card yourself. Key bindings and the settings file a
  game writes with `CFG[]=` stay shared in `ux0:/data/mkxp-z/mkxp-z/`.
- **`app0:` is read-only.** It is the contents of the installed VPK (fonts, the
  device config profile, the Ruby standard library).

The player reads a layered set of configuration files, of which
`ux0:/data/mkxp-z/config.json` is the one that belongs to you. It is read for
every game started from the launcher, so it must not name a `gameFolder`, a
`customScript`, an `rgssVersion` or an `RTP`. The full load order and every key
is in [docs/config.md](docs/config.md).

## 5. Installing an RTP

Most RPG Maker games rely on their engine's Run Time Package. RTPs are
Enterbrain-licensed and are **never** bundled: extract your own on the host,
without macOS `._*` metadata files (they nearly double the entry count and slow
the boot). The XP, VX and VX Ace RTPs are three separate Windows installers
(Inno Setup) from <https://www.rpgmakerweb.com/run-time-package>; the download
may be a `.zip` wrapping the installer.

```bash
# macOS: HOMEBREW_NO_AUTO_UPDATE=1 brew install innoextract
innoextract -e RPGVXAce_RTP.exe
```

innoextract writes everything under `app/`, the content root holding `Audio/`,
`Graphics/` and friends. Copy its contents to the matching folder on the card, so
that `ux0:/data/mkxp-z/rtp/VXAce/Graphics/` exists (and likewise `XP/` and `VX/`).
When a game's configuration names no RTP, the player appends the directory that
matches its RGSS version if it exists. A missing RTP directory is logged and
skipped, not fatal.

## 6. Copying files: use VitaShell USB, not FTP

**FTP transfers can mangle Japanese filenames.** Shift_JIS folder and file names
are common in Japanese titles and in stock RPG Maker resources
(`Data/マップ001.rxdata`, `Graphics/キャラクター/…`), and VitaShell's USB mode
preserves them where FTP does not. The failure does not look like a transfer
problem: the game boots and then reports a missing map or graphic, which reads
like an engine bug. FTP is fine for pulling logs off the device afterwards.

`ux0:` is exFAT (case-insensitive, case-preserving), so a game that asks for
`Graphics/Titles/title.png` finds `Title.PNG`.

## 7. What works, and what does not

- **Six games have reached in-game across all three engines** (three of them
  re-verified on the vitaGL build), including a Japanese original whose
  Shift_JIS title the launcher renders; see
  [COMPATIBILITY.md](COMPATIBILITY.md) for the per-game state.
- **Audio** was listen-tested on the device (tones, loop/seek, volume/pitch,
  BGS-over-BGM ducking, sound effects).
- **Saves round-trip with PC mkxp-z**: a save written on the Vita reloads and
  re-saves through the desktop build, structurally identical both ways. A round
  trip through the original Windows RGSS runtime is still open.
- **Standby/resume.** The system posts no notification on suspend, so the engine
  detects it from clock gaps and repairs timers, audio, and the handles it opens
  itself: the log, the stdio streams and its PhysFS read handles (music streams,
  fonts, archive entries). Verified on hardware on this vitaGL build with two
  consecutive standbys: the log, the BGM and BGS streams and stdout recover, and
  a map load and a save both work after waking. The storage tested was an SD2Vita
  adapter (the card mounted as `ux0:` through StorageMgr), which remounts on wake
  and leaves every file handle held across the standby stale (writes fail with
  ENODEV). **Ruby `File` handles a game keeps open across a standby are not
  repaired**; a script that holds one must reopen it after waking. The official
  Sony memory card was not re-tested on this build.
- **Performance.** Title, map and menu scenes hold 16.7 ms (VX, VX Ace) and 25 ms
  (XP) frame budgets on the games profiled so far; the visible costs are scene
  transitions such as loading a save. Script-heavy scenes run slightly slower
  rather than dropping frames, as in RPG Maker itself; `"frameSkip": true`
  (see [config.md](docs/config.md#frame-pacing)) trades that for dropped frames.
  Window frames are composed on the GPU, which shortens the pause when a menu
  or message window opens, and VX and VX Ace build each map's tile atlas there too. The display is double-buffered, so the picture runs
  at most one frame behind the game ([config.md](docs/config.md#display-buffering)).
- **Movies** (`Graphics.play_movie`, VX Ace) play in real time with synced audio
  (Theora/Vorbis `.ogv`). XP and VX have no engine movie call.

Known limits of the port, which apply to every game:

- **MIDI needs a SoundFont you supply.** The MIDI player is built in, but no
  SoundFont is bundled. Put a General MIDI `.sf2` in `ux0:/data/mkxp-z/sf2/`
  (create the folder) or in the game's own folder. The player searches the game
  folder, then `ux0:/data/mkxp-z/sf2/`, then `app0:/sf2/` (the release VPK carries
  none), and uses the first `.sf2` of the first folder that has one; within a
  folder the name that sorts first in byte order wins, so uppercase names come
  before lowercase. The limits are an 8 MiB font and 64 voices per synth.
  `midiSoundFont` (see [config.md](docs/config.md)) names one file instead, or
  `"off"` disables MIDI. Without a SoundFont, MIDI (including XP RTP music) is
  silent and the log says why.
- **`Win32API` is not real here.** A Ruby shim (`win32_wrap.rb`) stops
  `Win32API.new` from raising and implements a handful of `user32` calls; any
  other import raises a `RuntimeError` when the game calls it.
- **Nothing above 640×480.** XP is fixed at 640×480; VX and VX Ace boot at
  544×416 and may resize up to 640×480.
- **Some WAV codings are refused**: 24-bit PCM and IMA-ADPCM. 8/16/32-bit PCM,
  IEEE float, MS-ADPCM, Ogg Vorbis, MP3 and FLAC decode.
- **RPG Maker MV and MZ are not RGSS and are out of scope**, as are 2000 and 2003.

The full list of engine limitations is in [COMPATIBILITY.md](COMPATIBILITY.md).

Default controls, over the Vita pad:

| Vita | RGSS |
|---|---|
| Cross | C (confirm) |
| Circle | B (cancel) |
| Square | A (dash / shift) |
| Triangle | X |
| L / R | L / R |
| D-pad, left stick | directions |
| Right stick left / right | Y / Z (they cannot be held together; untested on a device) |
| Front touch screen | mouse: the first finger moves the pointer and presses the left button (`vitaTouchMouse`, on by default) |

The launcher's own controls are listed in [§3](#3-adding-games). Start (or F1)
opens an in-game binding menu, and Select toggles the FPS counter. **To quit,
hold Start and Select together for two seconds**: the game exits cleanly (back
to the launcher when it was started from there). Closing the app from LiveArea,
or losing power, looks the same as a crash, so the next launcher start shows
"The last game did not exit cleanly".

A stored binding file under `ux0:/data/mkxp-z/mkxp-z/` silently overrides these
defaults. To reset it, open the binding menu (Start), choose Reset defaults,
then Accept; or, with the game closed, delete both `keybindings.mkxp<N>` and
`keybindings.mkxp<N>.bak` (deleting only the first restores the previous
mapping from the `.bak`). In the menu Circle cannot be assigned (it cancels
capture), and Start and Select are reserved.

## 8. Logs, and reporting a problem

Everything the player says goes to a text file on the card:

```text
ux0:/data/mkxp-z/logs/mkxp-z.log     the current run
ux0:/data/mkxp-z/logs/mkxp-z.1.log   the previous run
ux0:/data/mkxp-z/logs/mkxp-z.2.log   …and the two before that
ux0:/data/mkxp-z/logs/mkxp-z.3.log
ux0:/data/mkxp-z/logs/launcher.log   a launcher boot writes here instead
ux0:/data/mkxp-z/logs/last-error.prev.txt   the last error report, once the launcher has shown it
```

The player writes an error report to `ux0:/data/mkxp-z/last-error.txt`; the
next launcher start shows it and moves it to `logs/last-error.prev.txt`, so
that is where to look after you have seen the error on screen.

Every boot rotates the log before opening it, keeping three generations, so a
crash and the relaunch that follows it do not cost the earlier evidence. A
few diagnostics are off unless a marker file exists beside the logs (only its
existence is checked, and it is read once per launch), for example
`ux0:/data/mkxp-z/log-sync.enabled`, which flushes every line as it is written:
turn it on before reproducing a crash so that a process the system kills still
has its last line on disk.

When you report a problem, attach **the text log**. Say which game and which
release of it, which RTP is installed, and what you did immediately before the
failure.

> **Do not attach a `.psp2dmp` crash dump to a public report.** When the system
> kills the player it writes `ux0:/data/psp2core-*.psp2dmp`, a copy of the
> process's memory: whatever the game had loaded is in it, and so is anything
> else the process happened to be holding. The text log is what a maintainer can
> act on.

## 9. Upgrading and uninstalling

Installing a newer `mkxp-z.vpk` over the installed app keeps
`ux0:/data/mkxp-z/`: your games, saves, `config.json`, logs and stored bindings
all live there, outside the app. Deleting the bubble does not remove that folder
either; delete it by hand to remove everything. A stored binding file under
`ux0:/data/mkxp-z/mkxp-z/` overrides the defaults of a newer release too; reset
it as described under the controls above to adopt them.

**Saves in `ux0:/data/mkxp-z/mkxp-z/` (upgrading from 1.0.1 or earlier).** Up
to 1.0.1, `System.data_directory` returned that one shared folder for every
game, so games that save through it (Pokémon Essentials and other
mkxp-z-aware games) wrote their saves there, and two such games overwrote each
other. From 1.0.2 it returns the game's own folder, and those games
look for their saves there. The player does not move old files, because it
cannot tell which game owns them; the log says
`vita-config: legacy shared data in '…' (first: '…')` while any are left. With
VitaShell, move each save file (for Essentials, `Game.rxdata`) from
`ux0:/data/mkxp-z/mkxp-z/` into `ux0:/data/mkxp-z/games/<Name>/` of the game
that wrote it. Leave `keybindings.mkxp*` and `mkxp.json` where they are.

---

# Building

Use a macOS or Unix host with VitaSDK and its port libraries installed
(`vita/scripts/vita-env.sh` finds `$HOME/vitasdk` or `/usr/local/vitasdk`;
otherwise set `VITASDK`). Host tools: a native C/C++ compiler, Git, Make, GNU
Bison 3+, Autoconf, CMake, Meson, Ninja, Python 3, pkg-config, curl, patch,
shasum, tar, unzip and xxd, plus a bootstrap Ruby (`/usr/bin/ruby`, a `ruby` on
`PATH`, or `BOOTSTRAP_RUBY`) for the host-Ruby step. That step replaces `PATH`
with `/opt/homebrew/opt/bison/bin`, `/opt/homebrew/bin` and the system
directories, so Bison 3 and Autoconf must be found there (or pass `BASERUBY`, see
below). Install these VitaSDK packages first with `vdpm install`:
`sdl2` (the stock package; the build links its own vitaGL-backed SDL2 instead),
`sdl2_image`, `sdl2_ttf`, `freetype`, `libpng`, `zlib`, `physfs`, `libogg` and
`libvorbis`. `sdl2_image` and `sdl2_ttf` pull in libjpeg-turbo, libwebp,
bzip2 and harfbuzz. Do not install `sdl2_vitagl`, which conflicts with `sdl2`.
The first build downloads and builds the pinned sources (vitaGL, SDL2, Ruby,
theora, uchardet, SDL_sound, pixman, OpenAL Soft and the other libraries below).
It also downloads the package files and upstream sources of the vdpm libraries above,
pinned by digest, for the release source archive. A test build links whatever vdpm
packages are installed; `vita/scripts/build-release.sh` refuses to write the archive
unless the installed libraries are byte-identical to the files of the packages pinned
in `vita/scripts/dep-pins.json` (vdpm channel 2026.08; each pinned package file is
fetched by digest and its `.BUILDINFO` must name the pinned recipe digest) and
`$VITASDK/version_info.txt` names the pinned newlib, pthread-embedded and vita-headers
revisions.

```bash
vita/scripts/build-player.sh
```

This is the canonical sequence: dependencies → Ruby → configure → compile →
package. It stops on any failed stage and never deploys. Run one build at a
time per checkout: the component builders share `build/`. The first run also
builds a matching host Ruby under `build/host-ruby-prefix/`; to reuse an
existing Ruby 3.1.3 instead:

```bash
BASERUBY=/path/to/ruby-3.1.3/bin/ruby JOBS=8 vita/scripts/build-player.sh
```

| Component | Source revision |
|---|---|
| mkxp-z (the base of this branch) | `826929eeb3ebc4b887c011604919217a790770f4` |
| mkxp-z Ruby 3.1.3 fork | `4d85560cf65938d7883a323bf553acad1faf5eae` |
| mkxp-z SDL_sound fork | `cfb2533eb3bac3700015cbd87cc623bea1467239` |
| SDL2 | `2.32.8` |
| vitaGL | `464876a79cdd00650bb0eb9629b85d81f9908fd8` |
| vitaShaRK | `df24065e65098b2d1ac533760109ad4367573f28` |
| SceShaccCgExt | `fb0e9d338525b067f3679ab33571323336493cca` |
| math-neon | `0faab814782c071ff4015527f1ca955ab1ccc470` |
| taiHEN | `309b3800bcb8ebbd5e4f5e5e920af3da3590b829` |

The vitaGL pins live in `vita/scripts/vitagl-pins.json`; the build fixes for
each pinned dependency are patch files under `vita/patches/{vitagl,sdl2,ruby}`,
applied by the build scripts and never edited in a source clone.
[THIRD-PARTY.md](../THIRD-PARTY.md) lists every component with its licence.

Output: `build/mkxp-z-vpk-vitagl/mkxp-z.vpk`, a **test build** under title id
`MKXPZ0053` (`VITAGL_TITLE_ID` picks another test id), with the unstripped ELF
beside it for crash diagnosis (never publish that file: it names the build
machine's layout). `vita/scripts/build-release.sh` builds the product package
under `MKXPZ0001` in `build/mkxp-z-vpk-release/` and stages it under
`build/release/` together with the corresponding source archive. This is a
repeatable procedure, not a claim of byte-identical output across unpinned SDK
or toolchain installations.

**Title ids.** The product application is `MKXPZ0001`. Every test build takes
its own id, and the packager refuses `MKXPZ0001` unless `MKXPZ_RELEASE=1`
(which `build-release.sh` sets). It also refuses a *pinned* root config under
`MKXPZ0001`. `MKXPZ0001` holds your launcher, its started games and the saves
beside them; a test run must never overwrite it.

**Shaders.** The package ships precompiled vitaGL shader binaries
(`vita/vitagl-shaders/`, loaded from `app0:/shader_cache/`).
`vita/vitagl-shaders/README.md` describes how to regenerate them after a
shader or vitaGL change.

## Layout of this branch

- `src/`, `binding/`, `shader/`, `assets/`: the engine, with the Vita changes.
- `vita/glue`, `vita/swraster`, `vita/overlay`, `vita/textpanel`, `vita/launcher`:
  boot and logging, the CPU raster core, the on-screen overlay and error panel,
  and the game list. They are compiled into the executable by `src/meson.build`.
- `vita/ruby`: the files the Ruby build adds (fiber arena, POSIX shims).
- `vita/mkxp-z-vpk`: what goes into the VPK (config, font manifest, preload scripts).
- `vita/vitagl-shaders`: the precompiled shader cache and its manifest.
- `vita/patches`: fixes to the pinned dependencies.
- `vita/scripts`: build, packaging and release scripts.
- `vita/docs`: [config.md](docs/config.md) and [launcher.md](docs/launcher.md).
- Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

mkxp-z is by the mkxp-z contributors and is a fork of
[mkxp](https://github.com/Ancurio/mkxp) by Amaryllis Kulla (Ancurio). This port
stands on [VitaSDK](https://vitasdk.org), [vitaGL](https://github.com/Rinnegatamante/vitaGL)
and vitaShaRK by Rinnegatamante, SceShaccCgExt by bythos14, taiHEN by yifanlu,
the SDL2 vitaGL video backend from Northfear's SDL branch, OpenAL Soft with
isage's Vita backend, and the libraries listed in [THIRD-PARTY.md](../THIRD-PARTY.md).
VL Gothic (Japanese fallback font) and Liberation Sans (Latin fallback font)
are bundled under their own licences.

## Licence

The Vita port's own files are licensed GPL-3.0-or-later ([LICENSE](../LICENSE) holds the
GPL version 3 text). The combined work is distributed under GPL-3.0, because it links
SceShaccCgExt (GPL-3.0). mkxp-z is GPL-2.0-or-later and is used under GPL-3.0 (its own
`COPYING` is unchanged).
[THIRD-PARTY.md](../THIRD-PARTY.md) lists every bundled component with its version,
licence, upstream and the licence arm elected where one is offered; the texts are
in `licenses/` and ship in the release VPK under `app0:/licenses/`. The shipped
vitaGL shaders are compiler output of this repository's own GLSL. RTPs and
Sony's `libshacccg.suprx` are never bundled.

The LiveArea art (bubble icon, background and start plate under
`vita/mkxp-z-vpk/sce_sys/`) is original to this project, GPL-3.0-or-later; regenerate it
with `vita/mkxp-z-vpk/art/make-livearea.py` (needs Pillow).

Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org).
All rights reserved. (FreeType is used under the FreeType Licence.)
