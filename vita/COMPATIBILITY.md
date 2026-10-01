<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Compatibility

What has been run on a real PS Vita. A row's status changes only after a run on
hardware: an emulator pass never marks a game playable. These are
the results of a small corpus of free RPG Maker games, not a compatibility
guarantee.

Game binaries, RTPs and commercial assets are never part of this repository.
Download each game yourself and copy it to `ux0:/data/mkxp-z/games/<Name>/`
(see [vita/README.md](README.md#3-adding-games)).

Blank Dream, Ao Oni and The Witch's House were re-verified on the vitaGL build
this branch ships, on the same scripted route: steady-state frame times were
identical to the earlier runs and every scene transition was faster. The other
rows come from runs of earlier builds and have not been repeated on this one.

## Status legend

| Status | Meaning |
|---|---|
| `not tested` | Reserved; no run yet. |
| `boots` | Title screen reached; no gameplay verified. |
| `in-game` | New game, walking and the menu work. |
| `playable` | Full loop including battle and save/load. |
| `broken` | Fails with a known reason; see Notes. |
| `blocked` | Cannot be tested (missing RTP, no legal download, dead link); see Notes. |

No row is `playable` yet. Each table has these columns: the game, its RGSS
engine, the exact release that ran, whether it uses custom scripts, the status,
frame times where measured (`scene: average / p95 ms`), the RTP it needs under
`ux0:/data/mkxp-z/rtp/`, and notes.

## Known engine limitations

Limits of the port itself, not of any one game. They apply to every game.

| Limitation | What it means for a game |
|---|---|
| **Font substitution is by family, without per-glyph fallback.** A registered game font wins even when it lacks a character. | Missing glyphs in a game's own font show as blanks instead of falling back to the bundled VL Gothic / Liberation faces. |
| **HTTPLite networking is unavailable.** `HTTPLite.get`, `.post` and `.post_body` raise `NotImplementedError` before starting a request. `HTTPLite::JSON.parse` and `.stringify` remain available. | Games must disable or handle online features such as downloads and remote saves. |
| **MiniFFI cannot load Windows DLLs.** Only a bounded integer/pointer argument ABI is supported. | A `Win32API` call gets a Ruby shim if one exists (a handful of `user32` calls); a game that depends on a real DLL is unsupported. |
| **MIDI requires a SoundFont you supply.** No SoundFont is bundled; without one, MIDI stays silent and the log says why. | XP RTP music is MIDI, so an XP game is quiet until a General MIDI `.sf2` is in `ux0:/data/mkxp-z/sf2/` (or the game folder). The first `.sf2` found is used, searching the game folder, then `sf2/`; `midiSoundFont` names a file or `"off"` ([README](README.md#7-what-works-and-what-does-not)). Limits: an 8 MiB font, 64 voices per synth, 4 MiB of MIDI sound effects. |
| **Only `smoothScaling` 0 (Nearest) and 1 (Bilinear) are available.** The release is built without the optional shaders, so values 2 (Bicubic) and 3 (Lanczos3), and 4 (xBRZ, which needs HTTPS support that the Vita build leaves out), fall back to Bilinear. The same applies to `smoothScalingDown` and the Bitmap scaling keys. | A game or config that asks for a higher mode gets Bilinear, with no error ([config](docs/config.md#keys-that-matter-on-this-device)). |
| **Graphics use a bounded, CPU-authoritative Bitmap backend.** Game resolution is limited to 640×480 and sampled textures to the 4096 GL limit; the render surfaces are reserved at boot. | Large assets and full-screen CPU effects can exceed memory or frame budgets. |
| **Radial blur on the CPU fallback differs measurably from desktop mkxp-z.** On vitaGL the blur runs on the GPU, as upstream does, and the CPU path is used only when the GPU path is unavailable; the GPU path has not been compared. On the CPU path, generated probes exceeded a 2-level channel tolerance (max delta 38 with alpha, 18 opaque). | Visible only as slightly different edge coverage in `Bitmap#radial_blur`. |
| **Scaled `stretch_blt` can differ from the desktop renderer by one source row or column at a boundary.** | The desktop renderer disagrees with itself at those rows (framebuffer-blit path against shader path), so there is no single desktop result to match. |
| **A 20×20 RGSS3 Window has measured border differences on the CPU compose fallback.** On vitaGL window frames compose on the GPU, which has not been compared. On the CPU path eight pixels exceeded the tolerance (max channel delta 6). | Larger windows measured within one channel level. |
| **The Bitmap cache is not a total memory budget.** Its 128 textures / soft 24 MiB exclude CPU pixels, decode scratch, atlases and retired storage. | A large Bitmap may exceed the soft cap, and loading or scene changes can exhaust an allocator even when the kernel free-memory counters look healthy. |
| **Handheld RGSS Y/Z defaults use the right stick left/right.** See [config](docs/config.md#handheld-controller-defaults). | A stored binding file overrides these defaults. The default axis cannot hold Y and Z together; games that need that must be remapped. |
| **A game's saves get no extra protection; they behave as they do on a PC.** The player writes what the game writes; nothing recovers, rotates or resurrects a slot ([save recovery](docs/config.md#save-recovery)). The engine's own settings and fatal-report files keep their generations. | A save interrupted by power loss, a full card or a crash mid-write is lost, as on a PC without a backup. Copy saves off the card yourself. |
| **Saves written through `System.data_directory` go to the game's own folder.** It returns the running game's folder, so a game such as Pokémon Essentials saves next to the game. Key bindings and the `CFG[]=` settings file stay shared in `ux0:/data/mkxp-z/mkxp-z/`. | Up to 1.0.1 it returned the shared folder for every game; saves written there are not moved automatically. Move them into the game's folder by hand ([README](README.md#9-upgrading-and-uninstalling)). No such game has been run on hardware yet. |
| **A BGM start position is dropped while an ME plays, and ignored for the file already playing.** | `Audio.bgm_play` with a position starts from the beginning when the ME ends; asking again for the file and pitch already playing leaves it where it was. Found by reading the source. |
| **With `BGMTrackCount` above 1, music can stay muted after an ME.** | Leave `BGMTrackCount` at its default of 1 unless a game needs several tracks. Found by reading the source. |
| **`load_data` inside an encrypted archive is case-exact, while `System.file_exist?` is not.** | A game that loads `data/Map001.rxdata` from an archive holding `Data/Map001.rxdata` fails although `file_exist?` says yes. Loose files on the card are case-insensitive. Found by reading the source. |
| **`System.user_name` is not meaningful.** The engine reads the `USER` environment variable, which a Vita application does not have. | Do not use it to name saves or profiles. |
| **Circle cannot be assigned in the binding menu.** It cancels a capture; Start and Select are reserved. | Circle stays Cancel (B) unless a game remaps it by script. |
| **Closing the app from LiveArea (or losing power) is reported as an unclean exit.** The player cannot tell a kill from a crash. | The next launcher start shows "The last game did not exit cleanly". Quit with Start + Select held for two seconds instead. |
| **MRI 3.1.3 does not reproduce all Ruby 1.8/1.9 behaviour.** RGSS version selects the bindings; the interpreter stays the same. | Old or custom scripts may need compatibility changes. |
| **Secondary Ruby threads work but are bounded.** Each active Ruby thread costs a kernel thread, an event flag and at least 12 semaphores. | Keep workers few; engine calls from worker threads are not certified. |
| **Fibers exist on one thread only.** Fiber machine stacks are carved out of the registered stack of the thread that starts Ruby, because a Vita thread that makes a syscall with its SP outside that stack is stopped by the kernel. Resuming a `Fiber` on any other Ruby thread raises `FiberError`. | RGSS3 runs its interpreters and message window as Fibers on the rgss thread, which owns the arena. A secondary thread may run ordinary Ruby code; it cannot resume a Fiber. |
| **At most 49 live Fibers.** The arena is 16 MiB of 335,872-byte slots; the 50th concurrently live Fiber raises `FiberError`. Completed Fibers return their slot at once. | Vanilla RGSS3 holds a handful (one per running event interpreter plus the message window). |
| **A Fiber that recurses through C on every level can exhaust its 260 KiB machine stack without raising.** MRI checks the machine stack only on the `rb_funcall` path, and the Vita has no guard page. | Deeply nested `each`/`map` recursion inside a Fiber is a hard stop rather than an exception. |
| **`File.chown` / `File#chown` do not change ownership.** Vita newlib checks that the path exists and reports success. | A zero return does not mean permissions changed. |
| **`File.umask` is a no-op.** The shim always returns zero. | Scripts cannot restrict newly created files through a process umask. |
| **`Process.times` is unavailable.** It raises `Errno::ENOSYS`. | CPU accounting is unavailable; GC profiling time can remain zero. |

## Empty projects (vanilla baseline)

| Game | Engine | Version tested | Custom scripts? | Status | Frame time (avg / p95) | RTP | Notes |
|---|---|---|---|---|---|---|---|
| Empty XP project | XP | — | no | blocked | — | XP | Not run: needs a fresh project from the editor. It would be the RGSS1 baseline: 640×480, 40 fps, 8-way input, tilesets. |
| Empty VX project | VX | — | no | blocked | — | VX | Not run: needs a fresh project from the editor. It would be the RGSS2 baseline: 544×416, 60 fps. |
| Empty VX Ace project | VX Ace | — | no | blocked | — | VXAce | Not run: needs a fresh project from the editor. It would be the RGSS3 baseline: title → new game → walk → menu → battle → save → load. |

## Free games with custom scripts

### XP (RGSS1)

| Game | Engine | Version tested | Custom scripts? | Status | Frame time (avg / p95) | RTP | Notes |
|---|---|---|---|---|---|---|---|
| Ao Oni (noprops, free) | XP | 6.23 | yes | in-game | title: 25.0 / 25.5 ms; map idle: 25.0 / 25.3 ms; menu open: 25.0 / 25.5 ms | XP | New Game → intro → walking → menu → save → quit → fresh-process Continue, on an isolated copy. No smooth-play, audio, battle/chase or soak claim. The free noprops release, not the 2024 remaster. |
| Fausts Alptraum | XP | — | yes | not tested | — | unknown | No freely downloadable build (the official download is Steam-only). Freeware horror by LabORat Studio. |
| Middens | XP | 3.5 | yes | in-game | — | unknown | Title → New Game → opening cutscene (weather, pictures, messages); boot 21.7 s. No save, battle or soak claim. Heavy custom battle/dialogue scripts. |

### VX (RGSS2)

| Game | Engine | Version tested | Custom scripts? | Status | Frame time (avg / p95) | RTP | Notes |
|---|---|---|---|---|---|---|---|
| The Witch's House (original free) | VX | 1.09a | yes | in-game | title: 16.7 / 16.8 ms; map idle: 16.7 / 16.8 ms; menu open: 16.7 / 16.8 ms | none | New Game → walking, menu and dialogue → save → quit → fresh-process Continue, on an isolated copy. The game's `Game.ini` names no RTP and it ran without one. Font substitution limits remain. The Steam MV remaster is not RGSS2. |
| Eternal Eden | VX | — | yes | not tested | — | unknown | No freely downloadable build (the full free release needs a mailing-list sign-up; the itch.io build is a demo). Later commercial remakes exist: use the original free VX release only. |
| Underworld Capital Incident (獄都事変) | VX | 1.05 (JP original) | yes | in-game | — | unknown | The launcher draws its Shift_JIS title; title → New Game → intro narration in kanji and kana; boot 8.0 s. No save, battle or soak claim. An English fan translation exists. |

### VX Ace (RGSS3)

| Game | Engine | Version tested | Custom scripts? | Status | Frame time (avg / p95) | RTP | Notes |
|---|---|---|---|---|---|---|---|
| Blank Dream | VX Ace | 1.05 | yes | in-game | title: 16.7 / 16.8 ms; map idle: 16.7 / 16.8 ms; menu open: 16.7 / 16.8 ms | VXAce | New Game → walking and dialogue → menu → save → quit → relaunch and load. Battle, two-screen traversal, soak and a Windows save round trip are still open. Requires the VX Ace RTP. |
| Pocket Mirror (Classic, 2016) | VX Ace | 1.3 | yes | in-game | — | unknown | Ships `Pocket Mirror.ini` and `Pocket Mirror.rgss3a` and no `Game.ini`; the launcher still lists it. Title → New Game → first map dialogue; boot 13.0 s; movies not reached. No save or soak claim. The later *GoldenerTraum* remaster is RPG Maker MZ, not RGSS3: do not use it. |
| Eternal Senia | VX Ace | — | yes | not tested | — | unknown | No freely downloadable build (Steam-only). Freeware action RPG with custom ARPG scripts, so a heavier Ruby load than a vanilla JRPG. |

## Scope

The corpus is free-to-download RPG Maker XP, VX and VX Ace (RGSS) games.
RPG Maker 2000/2003, MV and MZ, WOLF RPG Editor, Game Maker and commercial
titles are out of scope.

## Updating this table

1. Copy the game folder to `ux0:/data/mkxp-z/games/<Name>/` with VitaShell in
   **USB** mode, not FTP (Japanese filenames).
2. If the game needs an RTP, extract it with `innoextract` and install it under
   `ux0:/data/mkxp-z/rtp/<XP|VX|VXAce>/`; record which one in the RTP column.
3. Run it on hardware. Update Status, Version tested and Notes in the same
   commit and describe the run (the release string, the route, what was and was
   not checked). A status that moves without a run is a guess.
4. An emulator run never promotes a row.
