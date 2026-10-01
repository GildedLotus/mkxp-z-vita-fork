<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Configuration on the Vita

This is the reference for every config file the player reads, in what order,
and which key belongs to whom.

Everything below is Vita-only: the merge paths, protected keys, argument
stripping and defaults are specific to this port.

## The files

```
app0:/config/default.json            device profile, shipped in every VPK
app0:/mkxp.json                      the VPK author's pin: the launcher config,
                                     or a per-game pin
ux0:/data/mkxp-z/config.json         the user's global settings
ux0:/data/mkxp-z/games/<Name>/mkxp.json        the game's own config, as shipped
ux0:/data/mkxp-z/games/<Name>/mkxp-vita.json   the game's Vita-specific config
ux0:/data/mkxp-z/mkxp-z/mkxp.json    user config under customDataPath
```

All of them are JSON5 (comments and trailing commas are legal) and are parsed
as UTF-8. A `_comment` key is tolerated everywhere: it lands in the merged
object, is never read by the engine, and reaches Ruby as part of
`CFG.to_hash` (the Ruby surfaces are `CFG[]`, `CFG[]=`, `CFG.to_hash` and `ARGV`).

The engine's config reader limits each file to **64 KiB (65,536 bytes)**,
including any UTF-8 BOM, comments and whitespace, and **32 nested objects or arrays**, counting the
root object as level 1. The reader checks bytes before appending them and
the parser checks depth before descending. A file exceeding either limit
is logged with `Failed to parse <path>` and the limit error, then ignored;
the other layers retain their usual precedence. These limits also apply to
the root and custom-data configs when layers are off. **That is not the whole
story for a game's own files:** the mandatory preload wrapper
(`vita/mkxp-z-vpk/preload/game_preloads.rb`, see Compatibility preloads)
re-reads `<game>/mkxp.json` and `<game>/mkxp-vita.json` without these limits
and raises on malformed JSON or a value that is not an object, which stops the
game before its scripts run.

## Load order

Layers are **off** by default. They turn on when either

* `argv` carries `--game <path>` or `--game=<path>` — i.e. the launcher started
  this process; or
* `app0:/mkxp.json` sets `"vitaConfigLayers": true`.

With layers off, `Config::read` does exactly what stock mkxp-z does: read
`app0:/mkxp.json`, chdir into `gameFolder`, `readGameINI()`, merge
`<customDataPath>/mkxp.json`. **A player pinned to one game is
therefore unaffected.**

With layers on, lowest priority to highest:

| # | Layer | Notes |
|---|---|---|
| 1 | compiled `ConfDef` defaults | the table at the bottom of this file |
| 2 | `app0:/config/default.json` | the device profile |
| 3 | `ux0:/data/mkxp-z/config.json` | the user's global settings |
| 4 | `<game>/mkxp.json` | **device-owned keys removed** (list below) |
| 5 | `<game>/mkxp-vita.json` | only `gameFolder`, `maxTextureSize` and `enableHires` removed |
| 6 | `app0:/mkxp.json` | the VPK author's pin |
| 7 | `--game <path>` | sets `gameFolder`; `--execName <name>` sets `execName` |

`<game>` is the `--game` path when there is one, otherwise the `gameFolder` of
`app0:/mkxp.json`. If neither gives a folder, layers 4 and 5 are skipped.

All seven are merged **before** the early option reads
(`SET_STRINGOPT(gameFolder …)` through `SET_OPT(defScreenH …)`), because
`gameFolder`, `rgssVersion`, `execName` and `dataPathOrg`/`dataPathApp` are
consumed by the `chdir` and the `readGameINI()` that immediately follow them.
An eighth layer, `<customDataPath>/mkxp.json` (layer 8), is merged after
`readGameINI()` and has the final word; it cannot change `gameFolder` or `rgssVersion`, because both
have already been read.

`customDataPath` is `SDL_GetPrefPath(dataPathOrg, dataPathApp)` — on this
device `ux0:/data/<org>/<app>/`.

### How one key is merged

Each layer is applied with mkxp-z's own `copyObject`, unchanged:

* a key whose type matches the value already in place **replaces** it;
* a key that does not exist yet is **added**;
* a key whose type does *not* match is **rejected** with
  `Invalid variable in configuration: <key>` in the log — the previous value
  survives. `"smoothScaling": true` is not `"smoothScaling": 1`;
* arrays replace wholesale. `RTP`, `fontSub`, `preloadScript` and friends do
  not accumulate across layers — the highest layer that sets one wins all of
  it;
* nested objects are skipped and merged separately. `bindingNames` is the only
  one, and it gets its own `copyObject` per layer, so a layer may override a
  single button name without restating the other seven.

## Device-owned keys

These are dropped from layer 4, the game's own `mkxp.json`, before it is
merged. A game folder is a PC release: its window, renderer, JIT and data-path
choices are wrong here by construction, and its `gameFolder` would send the
player somewhere else entirely.

```
gameFolder          fullscreen              winResizable          defScreenW
defScreenH          vsync                   syncToRefreshrate     enableBlitting
maxTextureSize      enableHires             textureScalingFactor  framebufferScalingFactor
atlasScalingFactor  smoothScalingMipmaps    JITEnable             JITVerboseLevel
JITMaxCache         JITMinCalls             YJITEnable            dataPathOrg
dataPathApp         iconPath                preferMetalRenderer   dumpAtlas
```

A game that genuinely has something to say about one of them says it in
`<game>/mkxp-vita.json` (layer 5), from which only `gameFolder`, `maxTextureSize` and
`enableHires` are removed (they size window bases and GL surfaces).
`enableHires` set by any remaining layer is logged
(`vita-config: enableHires is not supported on this platform; ignored`) and
forced off: the software Bitmap backend cannot build a high-resolution Bitmap.
Everything else a game ships — `RTP`, `preloadScript`, `patches`, `fontSub`,
`windowTitle`, `SESourceCount` … — is honoured from layer 4 as-is, except where
a higher layer sets the same key. In launcher (product) builds the root
`app0:/mkxp.json` sets `customScript: ""` and `rgssVersion: 0` and is layer 6,
above the game's files, so a game's own `customScript` and `rgssVersion` are
overridden there (`Game.ini` still decides the RGSS version).

## Compatibility preloads

The launcher and stock-game packager pin `app0:/preload/win32_wrap.rb` as the
entry point. It composes scripts in this order:

1. `settings_file.rb`, already loaded by the native filesystem binding.
2. The bundled `ruby_classic_wrap.rb`, then the Vita Windows-call shim.
3. The `preloadScript` array in the game's `mkxp.json`, or the array in its
   `mkxp-vita.json` when present. The latter replaces the whole optional list;
   `[]` disables game preloads. This applies to pinned games as well as the launcher.
4. Package-author additions, preserved as `vitaPackagePreloads` by the packager.

These game files are read with the engine's JSON5 parser, including UTF-8 BOM
handling. Missing files are ignored; malformed objects, lists or entries raise
an error. They are read from the actual game working directory. Relative
script paths resolve there too, including package-author additions; absolute
`app0:/`, `ux0:/` and `uma0:/` paths retain their root; a preload under any other
`name:/` prefix, upper-case spellings such as `UX0:/` included, is refused with
an error at boot. Backslashes become slashes, and repeated separators, `.` and
`..` are normalized. Paths are case-preserving; symlink aliases are not deduplicated. Preloads must be loose
files: Ruby `load` does not read an encrypted RGSS archive or search RTPs.

The first normalized occurrence runs once. References to the four bundled
preload files are skipped in optional lists, so they cannot reorder or reload
the mandatory scripts. All paths are validated before the first optional
script runs. A missing file or an exception stops the chain and game startup;
scripts after the failure do not run. Game preloads can customize compatibility
behavior after the baseline is installed.

For example, a game can ship `{"preloadScript":["compat/fixes.rb"]}` in
`mkxp.json`. A Vita-specific replacement list goes in `mkxp-vita.json`.
The packager preserves its input config, stages a composed root config and
pins `enableSettings: true` for stock-game and launcher packages. `customScript`
diagnostics keep their explicit settings gate and preload list; the engine does
not run game preloads in that mode.

Only these game lists and package additions participate in composition.
Device/global `preloadScript` lists are not accumulated. Layer 8 runs after the
root pin in native code: do not put `preloadScript` there, since it can bypass
the entry point. Game-level config cannot override the packaged
`enableSettings: true` pin. Layer 8 can set it false to disable both Start and
F1 menu entry. Explicit `System.show_settings` calls remain independent of this
gate, and no layer can restore the desktop second window.

### Old Ruby and Windows limits

The CC0 classic wrapper at the pinned upstream revision supplies `Hash#index`,
`Object#type`/`id`, `TRUE`/`FALSE`/`NIL`, legacy `nil.id`/`true.id`, and a permissive
`BasicObject#initialize` for Ruby 1.9.2 callers. It is enabled for XP, VX and Ace.
Its waiver ships in the file. It does not change MRI 3.1's parser, byte-versus-
character String indexing, encoding rules, keyword arguments, method/reflection
return types, or object-ID scheme. Scripts relying on those older semantics
need game-specific changes; `rgssVersion` is not universal old-Ruby emulation.

| Corpus import / feature | Vita policy |
|---|---|
| `user32.dll!keybd_event` Alt+Enter (Ao Oni) | Logged no-op; the single window stays fixed. Other keyboard injection is unsupported. |
| `Resize.dll!resize_init` (Witch's House) | Logged no-op; no Windows window or render surface is resized. |
| `Resize.dll!resize_toggle` | Logged no-op; F5 window scaling is unavailable and needs no handheld binding. |
| `Resize.dll!resize_get` | Returns the actual fixed policy, `REGULAR` (`0`). |
| `Resize.dll!resize_set` | Logged no-op; requested desktop scale is ignored, and `resize_get` remains `0`. |
| `Kernel32!SetFileAttributes` (`pi` → `i`) | Logs unsupported attributes and returns failure (`0`); no hidden/read-only protection is claimed. The Witch's House constructs this at Section122:447 and ignores the result when requesting NORMAL (`0x80`) or HIDDEN (`0x02`) on its save directory. |
| Any other unimplemented import | Raises on call by default; construction does not load a DLL on Vita. |

The attribute refusal follows the documented [zero-on-failure contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileattributesa);
it does not emulate Windows attributes or provide `GetLastError`.

The shim's existing keyboard/mouse query implementations remain a limited
portable subset. Windows libraries and native extensions are not supplied.
Ao Oni's optional website command requires a thread and Windows shell: it is
unsupported. A game-specific preload should disable or explain that menu item;
thread support alone cannot provide the browser action.

## Automatic RTP

After the `RTP` list has been read, the player appends
`ux0:/data/mkxp-z/rtp/XP`, `.../VX` or `.../VXAce` for `rgssVersion` 1, 2 or 3
— but only when **all** of the following hold:

* the merged `RTP` list is empty;
* `customScript` is empty (a script player has no `Game.ini` and no RTP);
* `"vitaAutoRTP"` is not `false`;
* that directory actually exists on the card.

Set `"vitaAutoRTP": false` in any layer to turn it off, or give an explicit
`RTP` array to replace it. Supplying the list is all this does — what the
engine then mounts is decided by the resource-path code.

The filesystem also resolves explicit RTP entries relative to the actual game working directory.
The sentinel `none` is case-insensitive and suppresses implicit RTP; missing directories are
nonfatal and custom-script runs do not infer an RTP. Font mounts append only the `Fonts` namespace,
after game/archive/RTP resources; a bundled root mount would incorrectly shadow game files.

## Reading a config file on this device

`readConfFile` does **not** run `Encoding::convertString` here. `iconv` on
VitaSDK newlib has no CES tables, so the moment `uchardet`
guesses anything other than ASCII or UTF-8 the conversion throws and stock
discards the entire file with a single `Debug` line. Config files on this
device are UTF-8 by construction, so the reader strips a UTF-8 byte-order mark
— the one thing json5pp cannot parse — and hands the bytes over unchanged.

For the native reader, a file that is missing, unreadable, unparsable, or that
parses to something that is not an object contributes nothing and does not stop
the boot. A game's own `mkxp.json` and `mkxp-vita.json` are the exception: the
preload wrapper reads them again and raises on malformed JSON or a non-object
(Compatibility preloads), so a broken one stops the game before its scripts run.

## What the log says

Two lines, through `vita_glue_trace`, in `ux0:/data/mkxp-z/logs/`:

```
vita-config: layers default=ok global=absent game=ok game-vita=absent root=ok arg=ok
```

One status per layer: `ok` (read and merged), `absent` (no such file, or no
`--game`), `error` (unreadable, unparsable, not an object — or, for `arg`, a
`--game` whose path this player must not use). Emitted only when the layers
run.

```
vita-config: effective gameFolder='ux0:/data/mkxp-z/games/Blank Dream' rgssVersion=3 rtp=[ux0:/data/mkxp-z/rtp/VXAce] smoothScaling=1 fixedAspect=1 integer=0/1 frameSkip=0 fixedFramerate=0
```

What the engine actually ended up with, after every layer and both clamps.
Always emitted. `integer=` is `integerScalingActive`/`integerScalingLastMile`.
`frameSkip=0 fixedFramerate=0` is the device default; see **Frame pacing**
below for what the two of them do.

A game boot also logs where `System.data_directory` points, and, when the
shared folder holds files that are neither bindings nor the settings file,
a pointer at saves written by 1.0.1 and earlier:

```
vita-config: data directory='ux0:/data/mkxp-z/games/Blank Dream'
vita-config: legacy shared data in 'ux0:/data/mkxp-z/mkxp-z' (first: 'Game.rxdata'); move a game's files into its own folder
```

Nothing is moved, copied or deleted; see the saves note under *Keys that
matter on this device*.

## `--game`

```
sceAppMgrLoadExec("app0:/eboot.bin", {"--game", "<abs path>", NULL}, NULL)
```

The kernel supplies its own empty `argv[0]`, so the scan starts at `argv[1]`
and never inspects `argv[0]`. Both `--game <path>` and `--game=<path>` are
accepted, anywhere in the vector (mkxp-z reads `argv[1]` as `debug`/`test`/
`btest`, so the flag is frequently not first), and the first occurrence wins
whether or not its path is usable. A `NULL` element ends the vector.

A path is used only if it is a device-absolute `<dev>:/…` of at most 255 bytes
with no `.` or `..` component and no control byte — the same rule as
`launch_path_is_valid` in `vita/launcher/launch_args.c`. That is not
decoration: the string crosses a process boundary and then reaches `chdir` and
`fopen`.

`--game` and its value are stripped out of `launchArgs`, so a game script never
sees the launcher's own flag in `ARGV`.

The player deliberately does **not** link the launcher core library for this:
ten lines are not worth a build dependency from the engine onto the launcher.
The two copies of the argument handling must agree: **if one side changes,
check both.**

### `--execName`

The launcher's second flag names a game whose executable is not `Game`:
Pocket Mirror Classic ships
`Pocket Mirror.exe/.ini/.rgss3a` and no `Game.ini`, so the engine must open
`Pocket Mirror.ini`, not `Game.ini`.

```
sceAppMgrLoadExec("app0:/eboot.bin",
                  {"--game", "<abs path>", "--execName", "<name>", NULL}, NULL)
```

`--execName <name>` and `--execName=<name>` follow the same scan rules as
`--game` (first occurrence wins, `NULL` ends the vector) and both forms are
stripped from `launchArgs` with it. The value is a basename, not a path:
nonempty, at most 255 bytes, no control byte and no `/` or `\`, because
`readGameINI()` opens `execName + ".ini"` and SharedState opens
`execName + <archive ext>` inside the game folder. The launcher is stricter
than the engine: it lists only games whose executable name is at most 127 bytes, so a
longer one never reaches this flag from the list. An unusable name is never
half-applied: the flag is ignored and the layers' `execName` stands.

It merges as part of layer 7, above the game's own `mkxp.json` — the name
the launcher verified against the card's own listing outranks the game's
config. The layers trace names it when it was given; without the flag the
line is byte-identical to the shape at the top of this file:

```
vita-config: layers default=ok global=absent game=ok game-vita=absent root=ok arg=ok execName='Pocket Mirror'
```

Which folder earns a name is the launcher scan's decision, made from the
directory listing alone (`vita/launcher/game_scan.c`; see [launcher.md](launcher.md)).

## Writing `ux0:/data/mkxp-z/config.json`

Plain JSON5, flat keys, any subset. Two examples.

Integer 1× centred instead of the smooth stretch:

```json
{
  "smoothScaling": 0,
  "integerScalingActive": true
}
```

The integer-scale surface is reserved at boot only when the config enables
integer scaling. Without it, a game script's `Graphics.integer_scaling = true`
is refused: the attribute stays `false` and the log says once
`vita-gpu: integer scaling needs integerScaling in config at boot`.

Show the frame counter and keep the log quiet:

```json
{
  "displayFPS": true,
  "printFPS": false
}
```

Do not put `gameFolder`, `customScript`, `rgssVersion` or `RTP` in this file:
it is read for every game on the card.

## Save recovery

Vita treats save contents as opaque bytes. Filenames and Marshal streams stay
PC-compatible, including custom layouts with extra records.

**A game's own saves get exactly PC semantics: the player writes what the game
writes.** `File`, `Kernel`, `IO` and `Marshal` keep MRI's own methods, nothing
is installed over them, and no configuration key changes that. Native
`save_data` is the stock write — open, `Marshal.dump`, close — plus the close
stock leaks when `_dump` raises; native `load_data` is the stock read plus the
same guaranteed close. No `.tmp`, `.bak`, `.txn` or `.corrupt` file is created
for, consumed from or recovered into a game's slot. A deliberate `File.delete`,
a zero-byte save and a game's own `.bak` name mean what they mean on a PC.
Files with those names beside a slot (`Save*.txn`, `Save*.bak.keep`,
`Save*.bak.legacy`, `Save*.bak.tmp`, `Save*.corrupt`, `Save*.corrupt.N`) are
ordinary files to the player: nothing reads, consumes or cleans them.
**Nothing recovers a save for you: keep an external copy of anything you care
about.** Every save call shape gives the same result as on plain MRI 3.1.3.

The engine's own files are ours and keep one generation: the settings file at
`userConfPath` (`<customDataPath>/mkxp.json`, normally
`ux0:/data/mkxp-z/mkxp-z/mkxp.json`; written to `.tmp`, flushed, synced and closed before the rename
publishes it, recovered at binding initialization by
`vita/mkxp-z-vpk/preload/settings_file.rb`) and the fatal report (`.bak` until
the launcher consumes it). Those are regenerable-by-us bookkeeping, not a
game's save. The settings file is rotated: every successful
publish moves the previous active file to `.bak`, so recovery restores the last
published generation, not the first one ever written — the same rule the
binding menu below applies to its keybindings file. One validity rule (a
nonempty regular file within the 64 KiB config bound that parses to a JSON
object) drives the boot-time generation selection, the `CFG[]` reader and
recovery, so a corrupt active file selects a valid `.bak`, and the write path
never starts from the corrupt one. Publication follows the `vita_publish`
commit order: only a valid active file moves to `.bak`, then the finished
`.tmp` is renamed onto the free name. An invalid active file is quarantined as
`mkxp.json.corrupt`; the next damaged file is kept as `mkxp.json.corrupt.1`
and every later one replaces that second slot, so a fresh install holds two
copies at most. It never replaces a good backup. A write whose bytes the reader would refuse (over 64 KiB,
or not a JSON object) is checked after the sync and raises `IOError` before
anything is rotated, so a large `CFG[]=` value leaves the last valid settings
active. The accepted cost is that newlib's
non-atomic rename can drop the backup itself, never the active file.
Files named `mkxp.json.bak.keep` or `mkxp.json.bak.legacy*` beside the settings
file are ordinary files: nothing reads, writes or cleans them.

The player deliberately does not intercept a game's own writes: an interception
layer can lose saves (case aliases, garbage-collected writers, recovery
resurrecting a save the game deleted).

## Vita binding menu

Start and F1 open the binding menu when `enableSettings` is true. Explicit
`System.show_settings` calls request the same overlay regardless of that setting.
`enableSettings: false` suppresses Start as well as F1. Product packages still pin
the setting true, while diagnostics can disable both user entry points.
Requests during freeze, transition, movie playback or a skipped frame wait for
the next normal frame.
The scene stays resident and gameplay pauses. Reset and Start+Select quit remain
responsive. The menu uses the boot-reserved overlay, without another window.

Up/Down chooses a row, Left/Right one of four slots, Cross captures a binding,
Triangle clears it, Circle cancels, and Start accepts. Scroll down for Reset
defaults, Cancel and Accept. Capture waits for release, then uses the configured
stick deadzone for direction rows and triggers and the action gate (see
Handheld controller defaults) for stick input on other rows. Circle cancels
capture; Start and Select cannot be bound.
Mapped RGSS navigation works alongside raw d-pad and face buttons, so even an
empty binding set can reach Reset defaults and Accept. Duplicate actions show a
warning. Cancel discards the draft; Reset defaults takes effect only on Accept.

Bindings use the existing `customDataPath/keybindings.mkxp1`, `mkxp2` or `mkxp3`
file for each RGSS version (normally under `ux0:/data/mkxp-z/mkxp-z/`). They are
shared by games using that path/version. The menu does not write game or device
JSON settings.

On Vita, Accept writes `.tmp`, checks write/flush/sync/close and reads it back,
then rotates a valid current file to `.bak` before publishing the replacement.
It reads the published file back too. A missing/invalid current file loads from
`.bak` without consuming it; an invalid current is moved to `.corrupt` on the
next write, preserving the valid backup. Format and descriptor bytes remain
unchanged. An empty binding set is valid. Save failure keeps the menu open and
does not apply the draft to gameplay; retry or Cancel remains available.
As with save recovery, process-interruption recovery assumes one writer; card
and power-loss durability still need device testing.

## Key reference

Types are what `copyObject` enforces. `int` and `float` are both "number" to
it, so `1` where a float is wanted is fine; `true` where a number is wanted is
not. Defaults are mkxp-z's compiled `ConfDef` at pin `826929ee`; the
"app0:/config/default.json" column is what the shipped device profile sets.

| Key | Type | ConfDef | device profile |
|---|---|---|---|
| `rgssVersion` | int | `0` | — |
| `debugMode` | bool | `false` | — |
| `displayFPS` | bool | `false` | — |
| `printFPS` | bool | `false` | — |
| `winResizable` | bool | `true` | — |
| `fullscreen` | bool | `false` | `true` |
| `fixedAspectRatio` | bool | `true` | `true` |
| `smoothScaling` | int | `0` | `1` |
| `smoothScalingDown` | int | `0` | — |
| `bitmapSmoothScaling` | int | `0` | — |
| `bitmapSmoothScalingDown` | int | `0` | — |
| `smoothScalingMipmaps` | bool | `false` | — |
| `bicubicSharpness` | int | `100` | — |
| `enableHires` | bool | `false` | — |
| `textureScalingFactor` | float | `1.0` | — |
| `framebufferScalingFactor` | float | `1.0` | — |
| `atlasScalingFactor` | float | `1.0` | — |
| `vsync` | bool | `false` | `true` |
| `defScreenW` | int | `0` | `960` |
| `defScreenH` | int | `0` | `544` |
| `windowTitle` | string | `""` | — |
| `fixedFramerate` | int | `0` | — |
| `frameSkip` | bool | `false` | — |
| `syncToRefreshrate` | bool | `false` | — |
| `solidFonts` | array of string | `[]` | — |
| `preferMetalRenderer` † | bool | `false` | — |
| `subImageFix` | bool | `false` | — |
| `enableBlitting` | bool | `true` | — |
| `integerScalingActive` | bool | `false` | `false` |
| `integerScalingLastMile` | bool | `true` | `false` |
| `maxTextureSize` | int | `0` | — |
| `gameFolder` | string | `""` | — |
| `anyAltToggleFS` † | bool | `false` | — |
| `enableReset` † | bool | `true` | — |
| `enableSettings` | bool | `true` | — |
| `allowSymlinks` | bool | `true` | — |
| `dataPathOrg` | string | `""` | `"mkxp-z"` |
| `dataPathApp` | string | `""` | `"mkxp-z"` |
| `iconPath` | string | `""` | — |
| `execName` | string | `"Game"` | — |
| `midiSoundFont` | string | `""` | — |
| `midiChorus` † | bool | `false` | — |
| `midiReverb` † | bool | `false` | — |
| `SESourceCount` | int | `6` | — |
| `BGMTrackCount` | int | `1` | — |
| `customScript` | string | `""` | — |
| `pathCache` | bool | `true` | — |
| `useScriptNames` | bool | `true` | — |
| `preloadScript` | array of string | `[]` | — |
| `postloadScript` | array of string | `[]` | — |
| `RTP` | array of string | `[]` | — |
| `patches` | array of string | `[]` | — |
| `fontSub` | array of string | `[]` | `[]` (the player's built-in fallback table applies) |
| `fontScale` | float | `0.0` | — |
| `fontKerning` | bool | `true` | `true` |
| `fontHinting` | int | `3` | — |
| `fontHeightReporting` | int | `0` | — |
| `fontOutlineCrop` | bool | `true` | — |
| `rubyLoadpath` | array of string | `[]` | — |
| `JITEnable` † | bool | `false` | — |
| `JITVerboseLevel` † | int | `0` | — |
| `JITMaxCache` † | int | `100` | — |
| `JITMinCalls` † | int | `10000` | — |
| `YJITEnable` † | bool | `false` | — |
| `dumpAtlas` † | bool | `false` | — |
| `bindingNames` † | object of 8 strings | `a`…`r` → `"A"`…`"R"` | — |
| `vitaConfigLayers` | bool | `false` | — (set it in `app0:/mkxp.json`) |
| `vitaAutoRTP` | bool | `true` | — |
| `vitaTouchMouse` | bool | `true` | — |
| `controllerDeadzone` | float | `0.3` (Vita only; clamped to 0.05–0.95, see below) | — |
| `vitaGamesRoot` | string | — (not a ConfDef key; the launcher reads it from `app0:/mkxp.json` only) | — |
| `vitaglRamPoolMiB` / `vitaglCdramPoolMiB` / `vitaglPhycontPoolMiB` | int | `0` | — (vitaGL builds only; see below) |

† **No effect on this build.** The JIT keys: Ruby is built with
`--disable-jit-support --disable-yjit`, so it only warns. `bindingNames`: the
Vita binding menu has fixed labels; only the desktop settings window reads
them. `midiChorus`, `midiReverb`: the TinySoundFont synth has neither effect.
`preferMetalRenderer`, `dumpAtlas`: desktop-only. `anyAltToggleFS`,
`enableReset`: only a keyboard can trigger what they gate (Alt+Enter, F12).

`xbrzScalingFactor` exists only in an `MKXPZ_SSL` build and is not compiled
here.

### Keys that matter on this device

* **`fontSub` entries: one rule to know and one to follow** (the built-in
  table skips any family `fontSub` already defines, and an entry here still
  WINS). Case is handled for you: every entry is lower-cased before it is
  registered, so `Arial>Liberation Sans` works the same as
  `arial>liberation sans`. The rule to follow: a substitution applies ONLY when nothing is registered under `From`,
  so an installed family always wins, and a `To` that is itself unregistered
  is skipped rather than obeyed. Never list a family an RTP supplies:
  `VL Gothic>Liberation Sans` once hid the VX Ace RTP's own
  VL-Gothic-Regular.ttf and every Ace game logged `Primary font not found:
  VL Gothic` and drew Japanese text as tofu.
* **`enableSettings` gates Start and F1 for the Vita overlay menu.** Product
  packages pin it `true` in `app0:/mkxp.json` (layer 6), above both game-level
  configs. Layer 8 can override it to disable both entry points.
  Explicit `System.show_settings` calls remain available regardless. The menu
  uses the existing game window.
* **`defScreenW`/`defScreenH` are the window**, not the game's internal
  resolution (`src/main.cpp` `SDL_CreateWindow`, `graphics.cpp` `winSize`).
  `960x544` is the panel. The RGSS screen size still comes from the RGSS
  version.
* **`dataPathOrg`/`dataPathApp` must both be `"mkxp-z"`.** They give the key
  bindings and the settings file (`CFG[]=`, `mkxp.json`) one shared place for
  every game, `ux0:/data/mkxp-z/mkxp-z/`, apart from `ux0:/data/mkxp-z/`, which
  holds the logs, the marker files and `config.json` itself. With the stock
  defaults (`org` `"."`, `app` = the `Game.ini` title) `SDL_GetPrefPath` would
  return a different folder per title outside `mkxp-z/`, and, for a game with
  no usable title (the fallback title is `mkxp-z`), `ux0:/data/mkxp-z/` itself,
  where stock would read `ux0:/data/mkxp-z/mkxp.json` as its layer-8 user config
  in the middle of the layer stack's own data.
* **Saves are per game, not under `customDataPath`.** `System.data_directory`
  returns the running game's own folder (for example
  `ux0:/data/mkxp-z/games/<Name>`), so a game that saves through it (Pokémon
  Essentials writes `System.data_directory + "/Game.rxdata"`) keeps its saves
  next to the game like stock games; with no game (the launcher) it returns
  `customDataPath`. Key bindings and the `CFG[]=` settings file stay in
  `ux0:/data/mkxp-z/mkxp-z/`. Up to 1.0.1 it returned that shared folder
  for every game, so a save such a game wrote there
  (`ux0:/data/mkxp-z/mkxp-z/Game.rxdata`) is not moved automatically, because
  the player cannot tell which game owns it. The player logs `vita-config:
  legacy shared data in '<dir>' (first: '<name>')`; move the file into the
  game's own folder by hand.
* **Bindings are a file, not a config key.** `loadBindings` reads
  `<customDataPath>/keybindings.mkxp<rgssVersion>` and only falls back to the
  built-in defaults when that file is absent — a stored file silently overrides
  `bindingNames` and everything the defaults set. To reset it, open the
  binding menu (Start), choose Reset defaults and Accept; or, with the game
  closed, delete both the file **and** its `.bak`: the reader falls back to
  the `.bak` when the file is missing or invalid, so deleting the file alone
  restores the previous mapping.
* **`smoothScaling` is an integer**: `0` Nearest, `1` Bilinear, `2` Bicubic,
  `3` Lanczos3, `4` xBRZ. **The release package offers only `0` and `1`.** It is
  built without the optional shaders (the Bicubic and Lanczos3 programs), and
  xBRZ exists only in a build with HTTPS support (`MKXPZ_SSL`), which the Vita
  build turns off. On such a build every value of `2` or higher is treated as `1` (Bilinear), for
  `smoothScaling`, `smoothScalingDown`, `bitmapSmoothScaling` and
  `bitmapSmoothScalingDown` alike; the log says so once
  (`vita-gfx: smoothScaling N is not built`) for the first two, and nothing for
  the Bitmap keys, which choose the sampling of scaled or rotated sprites.
* **`midiSoundFont`** is either a file name, or `"off"` to disable MIDI. A name is
  opened as written, so it is relative to the game folder or an absolute device
  path such as `ux0:/data/mkxp-z/sf2/GM.sf2`. The default, an empty string,
  searches for a `.sf2` in the game folder, then `ux0:/data/mkxp-z/sf2/`, then
  `app0:/sf2/`: the first folder that has one wins, and within it the file whose
  name sorts first in byte order. No SoundFont is bundled; without one MIDI is
  silent. A font larger than 8 MiB is refused, and a synth plays at most 64 voices.

### Handheld controller defaults

| Handheld control | RGSS action |
|---|---|
| D-pad / left stick | directions |
| Cross / Circle | C (confirm) / B (cancel) |
| Square / Triangle | A (dash) / X |
| L / R | L / R |
| Right stick left / right | Y / Z |
| Start / Select | reserved for settings / frame-time overlay |
| Start + Select held | reserved for quit |

Vita defaults use right-stick left/right instead of the unavailable L3/R3 clicks. Movement, face buttons, shoulders and touch do not produce Y/Z.
Stick-to-button actions use the greater of `controllerDeadzone` and `0.30` in
SDL axis units, so lowering the movement deadzone does not lower the action gate.
Once pressed, the action stays held until the stick returns to 75% of that gate
or below (or reverses direction); jitter near the press gate cannot retrigger it.
These cached per-binding gates also apply to saved stick-to-button mappings;
direction bindings, trigger axes, raw-axis queries and binding-file contents are
unchanged. The binding menu's capture uses this action gate for stick input
when the row is anything but a direction; direction rows and triggers use the
movement gate described below.
Y/Z cannot be held together on this default axis. Desktop defaults still use clicks.
The handheld mapping has not been tested on a device.

A stored binding file replaces the RGSS rows above. Reset it from the binding
menu (Reset defaults, then Accept), or remove both it and its `.bak`, to use
these defaults. `bindingNames` only changes labels, not the controls. To isolate an existing format-3 binding file per game,
set `"dataPathOrg": "mkxp-z", "dataPathApp": "MyGame"` in that game's
`mkxp-vita.json` (a game's own `mkxp.json` cannot: layer 4 removes both keys),
then put `keybindings.mkxp1`, `.mkxp2` or `.mkxp3` (for XP, VX or Ace) in
`ux0:/data/mkxp-z/MyGame/`. That folder also becomes the game's settings-file
folder (`CFG[]=`); `System.data_directory` is unaffected and stays the game's
own folder. Use a binding file saved by mkxp-z's
binding editor with the desired controller mapping; do not enable the desktop
settings window on Vita. Keep both path names nonempty. This overrides the common
path described above only when config layers are enabled and the packaged
`app0:/mkxp.json` does not pin `dataPathOrg`/`dataPathApp` at higher priority;
a root config that pins both must be adjusted when
packaging a player intended to use per-game binding directories.

### Front touch as mouse

`vitaTouchMouse` is Vita-only and defaults to `true`. The first front-panel finger
drives `Input.mouse_x`, `Input.mouse_y` and `Input.press?(:MOUSELEFT)` through
SDL's mouse events. The rear touchpad and additional held fingers do not drive
the mouse; lifting the first finger does not transfer control to another held finger.
Coordinates use the game's resolution and current letterboxed rectangle. Touches
in the bars report coordinates outside the game and cannot press a button.
Dragging into a bar releases the button; moving back inside does not press again
until a new touch. Releasing outside also clears the button.

Set `"vitaTouchMouse": false` in the game's `mkxp-vita.json` to disable this mapping
(requires `vitaConfigLayers`, with the usual layer precedence). Games must call
`Input.update` to sample mouse input. Finger alignment and rear-panel
inactivity have not been checked on hardware.

### Controller deadzone

`controllerDeadzone` is Vita-only: default `0.30`, clamped to `[0.05, 0.95]` of SDL's signed
16-bit axis range. SDL's Vita response curve is nonlinear; `0.30` means about 37% physical
travel at the pinned SDL revision, not 30%. `0.5` restores the stock gate `0x4000`.
The threshold is resolved once, when bindings are applied, and cached per axis binding. The binding
menu's capture uses the same threshold for direction rows and triggers; stick
capture for any other row uses the action gate above. The binding file format is unchanged, and stored bindings
still win over defaults.

### Frame pacing

Two keys that mkxp-z reads and `src/display/graphics.cpp` acts on.

* **`frameSkip` is `false`**, as in stock mkxp-z. When `Graphics.update` finds
  the frame limiter more than a whole frame behind the ideal timestep, it
  either drops the redraw (`frameSkip` on) or throws the accumulated debt away
  and draws the late frame anyway (`frameSkip` off). Rendering runs on the GPU,
  so a frame that overruns is almost always script work, and skipping the draw
  does not shorten it: a script-heavy map plays smoothly and slightly slower
  with the skip off, but choppily with it on. RGSS itself never skips frames.

  Set `"frameSkip": true` in any layer (a game's own `mkxp.json` may too) to
  keep `Graphics.frame_count`, `Input` repeat timing and every `Graphics.update`
  loop at the rate the game asked for, dropping frames instead of slowing down.
  The skip is bounded: at most 5 consecutive updates are skipped (at least one
  in six is drawn) and the accumulated debt is capped at 5 frames, so a long
  load cannot cause a burst of skipped frames afterwards.

* **`fixedFramerate` pins the limiter.** `0` (the default) means "follow
  `Graphics.frame_rate`", which is what the game asks for — 60 for VX Ace, 40
  for XP by default. A positive value overrides it and `Graphics.frame_rate =`
  no longer moves the limiter; a negative value disables the limiter entirely.
  A game that cannot hold its own rate can be asked for less:

  ```json
  { "fixedFramerate": 30 }
  ```

  in `ux0:/data/mkxp-z/config.json` (per device) or `<game>/mkxp-vita.json`
  (per game).

  This is a *different* trade from `frameSkip`, not a stronger version of it.
  RGSS counts animation timing in frames, so halving the limiter halves the
  game's clock too: a 60 fps game pinned to 30 draws every frame and plays at
  half speed, where the same game left at `0` with `frameSkip` on keeps
  near real-time speed and drops frames it cannot draw. Reach for
  `fixedFramerate` when steady, unskipped motion matters more than pace —
  otherwise leave it at `0`.

* **`syncToRefreshrate` disables both.** It sets `fpsLimiter.disabled`, and a
  disabled limiter never reports a frame skip, so `frameSkip` has nothing to
  act on and `fixedFramerate` is ignored. Leave it `false` (the default, and
  what the shipped device profile leaves alone).

The boot log settles which of these was in force: see the `vita-config:
effective … frameSkip= fixedFramerate=` line above.

### vitaGL pool sizes

`vitaglRamPoolMiB`, `vitaglCdramPoolMiB` and `vitaglPhycontPoolMiB` (whole MiB,
`0` = default 56 / 64 / 4) become the `SDL_VITA_VGL_*_POOL` hints before
`SDL_CreateWindow`, where SDL's VGL backend calls `vglInitWithCustomSizes`. A
request outside 16..200 / 16..112 / 4..26 MiB, or larger than the boot free
memory of its kernel pool, is logged as `vita-vgl-pool: <pool> request=… rejected`
and replaced by the default. SDL also honours an `SDL_VITA_VGL_*_POOL`
environment variable ahead of the config, but nothing in the player sets one,
so on an installed build the config keys are the only choice.

The defaults come from device measurements: vglInit takes 36.6 MiB of RAM and
use never moved afterwards, and CDRAM peaked at 36.7 MiB in a 30-minute play
session; PhyCont was never touched. The newlib heap is 160 MiB, which leaves
about 97 MiB of user memory free during play. When every pool is full, vitaGL
allocates from newlib itself, so that heap is also the GPU's last resort. The
launcher draws one textured quad and asks for RAM 40 / CDRAM 24 / PhyCont 4 MiB;
it reads no config, but the environment variables still override it.

Every boot logs `vita-boot: vgl_init tag=<game|launcher> window_us=<n> ram=…
cdram=… phycont=… cdlg=…`: the time `SDL_CreateWindow` spent (vglInit and its
pool memory included) and the pool totals vitaGL actually got (0 for a pool
that failed).

With the marker file `ux0:/data/mkxp-z/memory-ledger.enabled` present, the log
also gets `vita-vgl-pool: tag=… scene=… ram=… cdram=… phycont=… cdlg=…
vtx=last/peak/slice` lines (`free/total` bytes per pool) after GL init, after
the shader phase and with each memory sample. `vtx` is the circular vertex pool
watermark: bytes the last presented frame reserved, the most any frame has
reserved since boot, and one per-frame slice (the 32 MiB pool divided by the
display buffer count). A peak above the slice means frames spilled into ad-hoc
allocations; size the pool from the peak of a long run (slice >= peak plus
headroom, times the buffer count).

### Display buffering

The display is double-buffered: 2 buffers and at most 1 pending flip. vitaGL's
own default of 3 buffers and 2 pending flips lets a 60 fps game settle two
frames ahead of the panel, which adds about 16-33 ms of input latency. The
SDL2 backend reads an `SDL_VITA_VGL_TRIPLE_BUFFER` hint that would restore
triple buffering, but there is no config key and nothing on an installed build
sets it, so the player is always double-buffered. Frame pacing (above) is
unchanged by this.
