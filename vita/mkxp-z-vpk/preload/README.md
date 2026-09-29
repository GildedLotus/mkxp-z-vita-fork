<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Bundled preload scripts (app0:/preload/)

`vita/scripts/package-vpk.sh` packs everything in this directory into
`preload/` inside the VPK. `win32_wrap.rb` is the compatibility entry point.
The Vita filesystem binding always loads `settings_file.rb`, before game
preloads, because the settings writer needs the module; it wraps nothing.

Preload scripts run after the bindings exist and after the game's script
archive has been decoded, but **before** any game script is evaluated
(`binding/binding-mri.cpp`, `runRMXPScripts`). They are read with plain
`fopen` via `readFileSDL` (`src/util/sdl-util.h`), not through PhysFS, so
the entry point uses an absolute `app0:/preload/<name>` path. By the time it
runs, the working directory is the game folder, not `app0:/`.

An exception raised by a preload script leaves `$!` set, and `runRMXPScripts`
returns without evaluating a single game script. Anything added here must be
inert at load time on Vita.

| File | Role | License | Source |
|---|---|---|---|
| `win32_wrap.rb` | Vita Windows-call policy and compatibility entry point | CC0 1.0 (waiver in the file header; Vita edits also CC0) | Based on mkxp-z `scripts/preload/win32_wrap.rb` at pin `826929ee` |
| `settings_file.rb` | Generation handling for the engine's own settings file | Project license | This repository |
| `ruby_classic_wrap.rb` | Small legacy method/constant baseline | CC0 1.0 (waiver in the file header) | Unmodified mkxp-z `scripts/preload/ruby_classic_wrap.rb` at pin `826929ee` |
| `game_preloads.rb` | Game-list composition and duplicate handling | Project license | This repository |

Order: native `settings_file.rb`, classic wrapper, Win32 shim, game preloads,
package additions. The entry point retains its original filename so existing
pinned-game configurations gain composition as well. See
[configuration](../../docs/config.md#compatibility-preloads) for JSON5,
game-relative/device paths, replacement lists, errors, duplicates and the
legacy settings-layer exception. No Ruby JSON extension is required:
`HTTPLite::JSON.parse` is bound even with networking disabled.

`settings_file.rb` defines a module and wraps nothing — not `File.open`,
`Kernel#open`, `File.new`, `IO.binwrite` or `Marshal.dump` — and no title-screen
pass runs, so a game's saves behave exactly as they do on a PC. The only file it
touches is the engine's own settings file: `recover` at binding initialization
and `write_bytes` from `saveUserSettings`, which keep one pinned generation in
`<path>.bak` and never publish a partial file. See
[save recovery](../../docs/config.md#save-recovery).

## Why win32_wrap.rb is shipped

`Win32API` always exists — `binding/miniffi-binding.cpp` binds it to `MiniFFI`
unconditionally — but on Vita there is no Win32 loader, so `SDL_LoadObject`
always fails, `MiniFFI_initialize` finds a NULL entry point and raises a Ruby
`RuntimeError`. Vanilla games do not rescue that:

* Ao Oni (RGSS1) builds `Win32API.new 'user32.dll', 'keybd_event', …` from a
  title-screen command handler, so the title screen dies.
* Witch's House (RGSS2) evaluates
  `Win32API.new('Kernel32', 'SetFileAttributes', 'pi', 'i')` in a class body at
  script-load time, so the game never reaches its title screen at all.

The shim reopens the class without attempting a native load on Vita. Existing
Ruby keyboard/mouse queries remain. Alt+Enter and the four observed Resize.dll
imports keep the Vita window fixed and log the limitation; `resize_get` reports
`REGULAR` (`0`). `SetFileAttributes` logs that attributes are unchanged and
returns failure (`0`). Unsupported imports raise on call by default.
`TOLERATE_ERRORS=true` is an explicit debugging opt-in, not the product policy.

Vendored rather than copied out of the mkxp-z tree at package time so that
packaging needs no engine checkout. The
classic wrapper preserves both CC0 notices, adds aliases and a
Ruby 1.9.2 constructor compatibility rule; it does not emulate Ruby 1.8 syntax,
Strings or reflection. Those differences need a game-specific preload or edit.
