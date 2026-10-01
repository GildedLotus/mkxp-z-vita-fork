<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# The launcher: one eboot, three modes

This is the reference for how
a boot decides what it is, what it writes where, and how a game gets back to
the game list.

The launcher front end itself — the window, the list, the pad, the hand-over —
lives in `vita/launcher/vita_launcher.c`. This
document covers the half that decides *whether that code runs at all*.

## Why a mode and not a second program

The player and the launcher are the same `eboot.bin`. Two reasons, and both are
hard constraints rather than preferences:

* **MRI cannot be torn down in process.** `ruby_cleanup()` does not give a
  process back a state where `ruby_setup()` can run again. So "quit to the game
  list" cannot be a return into a menu loop: it has to be a new process, and on
  this device a new process means `sceAppMgrLoadExec` of the same eboot.
* **The package is large.** Almost all of it is Ruby, the fonts and the shader cache. A
  second eboot would double a VPK in order to draw a list.

## The three modes

| mode | when | what runs |
|---|---|---|
| `launcher` | no usable `--game`, and the root config pins nothing | the game list, SDL2 only |
| `game` | `--game <valid path>` | the whole player; on exit, LoadExec back to the launcher |
| `pinned` | no `--game`, and the root config names a `gameFolder` or a `customScript` | the whole player; on exit, back to LiveArea |

`pinned` is a player packaged for one game; the product bubble is a
`launcher`. A pinned player exits to LiveArea.

The decision is taken by `vita_boot_mode_from(argc, argv, root_config_text)`,
which is pure — no file, no log, no process state — and by nothing else. In
order:

1. `launch_args_parse()` says `GAME` → **game**.
2. it says `INVALID` → **launcher**, with the rejected value queued for the
   message screen. A rejected path never falls back to the pin: the user asked
   for a specific game and has to be told they did not get it, not handed a
   different one.
3. the root config has a non-empty `gameFolder` **or** a non-empty
   `customScript` → **pinned**. Either key alone is enough: a `customScript`
   player has no `gameFolder` worth the name, and a deployed game has no
   `customScript`.
4. otherwise → **launcher**.

An unreadable, unparsable or over-large root config counts as "not pinned" and
is reported once the log is open. A broken file must never be able to strand
the device on a black screen with no way back to the list — and a file too big
to read whole (64 KiB) is *not* decided from its prefix, because the half that
did not fit is exactly where a `gameFolder` could have been.

The root config is `app0:/mkxp.json`, read with the engine's own `json5pp`, so
"is this player pinned" is decided by the same parser that will later read the
same file (`src/config.cpp`). JSON5 comments, bare keys and a
UTF-8 BOM are all accepted.

## The argument contract

Both processes are the same eboot, and the kernel supplies its own **empty**
`argv[0]`; whatever the caller passes starts at `argv[1]`. So:

```
launcher → game     sceAppMgrLoadExec("app0:/eboot.bin", {"--game", "<abs path>", NULL}, NULL)
game → launcher     sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL)
```

A LiveArea boot is `argc == 1` with `argv[0] == ""`, which is exactly what the
game's `argv == NULL` direction produces — so coming back and starting fresh
are the same code path. `--game=<path>` is accepted as well as `--game <path>`,
and the first occurrence wins.

A path is only accepted if it is a device-absolute `<dev>:/…` path of at most
255 bytes with no `.` or `..` component and no control bytes
(`vita/launcher/launch_args.c`). That is not decoration: the string crosses a
process boundary and is then handed to `fopen` and `chdir`.

A folder that names its own game launches with a second argument.
Pocket Mirror Classic ships `Pocket Mirror.exe/.ini/.rgss3a`
and no `Game.ini`, so the scan finds no game there by the Game.ini rule.
When a folder holds no Game.ini but exactly one `<name>.ini` plus a partner
whose basename is the same (ASCII case-insensitive) and whose extension is
`.exe`, `.rgssad`, `.rgss2a` or `.rgss3a`, the folder is listed and the
launch hands the name across:

```
launcher → game     {"--game", "<abs path>", "--execName", "<name>", NULL}
```

`--execName` follows the same argument rules as `--game` (paired and `=`
forms, first occurrence wins) and both are stripped before any game script
sees them. The value must be a usable basename — nonempty, at most 127
bytes, no control byte, no separator — because the engine joins it onto the
game folder for the `<name>.ini` and archive opens. The `.ini` that counts is
the one a partner names: a stray `.ini` with no partner — `desktop.ini`, a
settings file the game writes — cannot hide the game's own.
A folder where several `.ini` files have partners (no single name is the
game) is skipped, with a `launcher: scan skipped … several .ini files naming
games` line naming the last such folder. Game.ini folders launch exactly as
before, with no second argument. The decision comes from the directory
listing alone — two readdir passes over the one folder, never a per-entry
stat.

### Scan limits

* **256 games.** With more folders than that, the list holds the first 256 the
  directory scan returned (in the card's own order, sorted only afterwards) and
  the rest are missing, with no on-screen notice: only the `launcher: scan …
  truncated=1` log line says so.
* **8 `.ini` files.** A folder with more than eight `.ini` files is rejected as
  ambiguous.
* **4096 bytes.** A `Game.ini` larger than that drops the game from the list;
  the log names the folder.
* **BOM or leading whitespace.** The launcher's parser skips a UTF-8 BOM and
  trims each line, so a `Game.ini` that starts with either is listed. The
  engine's parser recognises a section only when the line starts with `[`, so
  such a game starts and stops with "No script file has been specified".

## The files

```
app0:/mkxp.json                          the root config; its two keys decide pinned vs launcher
ux0:/data/mkxp-z/logs/launcher.log       a LAUNCHER boot's log
ux0:/data/mkxp-z/logs/mkxp-z.log         a GAME or PINNED boot's log
ux0:/data/mkxp-z/launch-in-progress.txt  the crash breadcrumb
ux0:/data/mkxp-z/last-error.txt          the error hand-over file
ux0:/data/mkxp-z/logs/last-error.prev.txt the previous one, kept for a USB pull
ux0:/data/mkxp-z/logs/launcher-last.txt  the cursor bookmark
```

### Two logs, on purpose

Log rotation is keyed per log path (`vita/glue/vita_glue.c`, `rotate_logs`):
each file keeps three generations of its own. If the launcher wrote the player
log, then starting the launcher after a crash would push the crash log one
generation down every time — and after three glances at the list the evidence
would be gone. So a launcher boot writes `launcher.log` and a game boot writes
`mkxp-z.log`, and `vita_glue_boot()` is handed the path the decision chose.

### The crash breadcrumb

A game the system kills never returns from `main()`. `vita_boot_finish()` never
runs, there is no LoadExec, and the user lands back on LiveArea with no
explanation at all.

So, in **game** mode only, as soon as the log is open:

```
ux0:/data/mkxp-z/launch-in-progress.txt
    game=ux0:/data/mkxp-z/games/<Name> log=ux0:/data/mkxp-z/logs/mkxp-z.log
```

and it is deleted immediately before the hand-over back. A launcher that finds
the file on its next start knows the previous game died: it says so, names the
game, points at the log (still intact), and deletes the file.

This is the **only** crash handling in scope. No `atexit`, no signal handler, no
watchdog thread — a GPU fault or a kernel kill defeats all three, and each
would be one more thing running while the process is being replaced.

Any exit that is not that hand-over produces the same report: closing the app
from LiveArea or losing power is indistinguishable from a crash, so the next
launcher start shows "The last game did not exit cleanly", under the raw kind
`unclean-exit`. Holding Start and Select for two seconds is the way to quit a
game cleanly.

A **pinned** boot neither writes a breadcrumb nor consumes one. The evidence of
a crash belongs to the launcher, and a diagnostic run must not eat it.

### The error hand-over file

A fresh reopen consumes the root report and rotates it into `logs/last-error.prev.txt`; a later distinct boot has no repeated report.

`last-error.txt` has exactly **one** producer: `vitaWriteLastError()` /
`vitaWriteLastErrorTo()` in `src/vita_fatal.cpp`. Everything that reports through it — the bindings' `showExc`, the
three sites in `src/main.cpp`, and `vita_boot_report_error()` — calls that
writer; none of them spells the format out.

The format is:

```
mkxp-z-last-error v1
kind: <script-error|init-error|stuck|unclean-exit|bad-argument>
title: <one line>
bytes: <ten digits>
---
<text, any length>
```

`bytes:` is the exact length of the text after the separator, zero-padded to a
fixed width so the shutdown watchdog can extend a copy of a whole report and
patch the length in place. The launcher shows a report only when the text is
exactly that long; a torn or damaged file is not a report. A report from before
the field existed carries none and is still read when its envelope is whole.
The writer applies the same rule before it rotates: a whole active report moves
to `last-error.txt.bak`, a damaged one to `last-error.txt.corrupt`, so damage
can never displace a valid backup, and a retained `.tmp` is published only when
it declares and matches its length. Any other retained `.tmp` (torn, or a complete
report from a build that declared no length, which may be the newest diagnosis) is
moved to `last-error.txt.corrupt`, never deleted; a newer one takes that slot.

`script-error`, `init-error` and `stuck` are `src/vita_fatal.h`'s.
`unclean-exit` (a breadcrumb was found) and `bad-argument` (a rejected `--game`,
or an unreadable root config) are the boot wrapper's, in
`vita/launcher/vita_boot.h`.

The launcher consumes it by **rename**, to `last-error.prev.txt`, so a successfully
consumed report is shown once and survives for a USB pull
(`vita_launcher_take_last_error()`). If reading fails, the launcher shows an
I/O notice and keeps the pending files. If rotation fails, the diagnostic says
the report remains pending and may appear again next boot; its retained path
is shown and logged. Neither failure is reported as successful consumption.

A report the dying game managed to write always wins over the breadcrumb's: a
game that said "Unable to load scripts from Data/Scripts.rvdata2" diagnosed
itself better than "it did not exit cleanly" ever could, so the breadcrumb is
deleted and the existing report is left alone.

The producer and consumer must agree on the directory, magic, separators and
kinds: `src/vita_fatal.h` defines them for both.

The writer uses bounded libc buffers and no heap allocation: an 8 KiB header buffer, a sanitized
single-line title and a 64 KiB cap on the log excerpt. It writes a temporary file and renames it; on
the Vita an occupied destination may need an unlink first, so there can be a short gap with no
report. The next process tolerates that gap, but a partly written file is never published. The report
is persisted before any font, GL or Ruby-backtrace work that could fail, and a nil or non-array
backtrace or a malformed string falls back to a safe default.

The exception panel sanitizes the class, the message and at most 32 backtrace entries without
modifying Ruby strings. It uses one temporary 960×544 texture, a warmed simple shader and normal
blending, and needs no new FBO, program, VAO or buffer. It restores the pushed GL state on every
exit. It prefers the registered pooled VL Gothic face for CJK and otherwise opens its own built-in
face; a borrowed pooled font is never closed. If the panel fails, the durable log remains and the
panel does not raise again. The RGSS loop is stopped, so dismissal reads raw controller state, waits
for a press and then a release, and sleeps 12 ms between checks while the main thread keeps pumping
SDL; a terminate request ends it early. The autodismiss marker gives tests a five-second exit.
Non-Vita builds leave the panel as a no-op.

The persistent status overlay instead owns one boot-time 512×128 texture and file-static RGBA staging.
A hidden overlay issues no GL work, and unchanged text is not uploaded again. It is drawn on both
normal screen-redraw exits through the shared quad with the already warmed normal blending;
transitions and fades are separate. The texture lasts for the process and is never queued for
retirement after the shutdown drain.

## What each mode does at the end

`vita_boot_finish(rc)` is the tail of `main()`:

* **game**, normal return — delete the breadcrumb, then
  `sceAppMgrLoadExec(argv = NULL)`. By this line mkxp-z's own shutdown has
  already closed the ALC device and released the GL context; only when the call
  fails does anything after it run.
* **game**, `VITA_BOOT_RC_WEDGED` — no hand-over. The RGSS thread never
  acknowledged the terminate request, so it is still inside Ruby and the OpenAL
  mixer thread is still live; replacing the address space underneath them is
  what the crash dumps show. The process exits and the breadcrumb is
  **left on the card**, so the launcher reports it on the next manual start.
* **launcher** — nothing. The front end hands the process over itself, from its
  own loop, with its texture, VBO, program, context and window released first
  (`vita/launcher/vita_loadexec.c`).
* **pinned** — nothing. Back to LiveArea, exactly as before.

An exception that escapes `mkxp_main()` is caught in `main()` and reported
through `vita_boot_report_error()` before the hand-over. An exception leaving
`main()` would be `std::terminate`, and on this device that is a process kill
with an empty log.

The mode is chosen before the player log is opened, so browsing the list preserves the previous
game's evidence. Launcher mode starts neither MRI nor OpenAL. With a wedged RGSS thread there is
neither GL/AL teardown nor LoadExec: the marker stays for the next launch instead. Ruby starts
only once per process, and returning to the launcher always creates a fresh process image.

## The log lines

One launcher → game → launcher round trip, in order:

```
launcher.log   vita-boot: mode=launcher reason=no-args config=ok game='' log='…/launcher.log'
mkxp-z.log     vita-boot: mode=game reason=arg config=absent game='ux0:/…/Blank Dream' log='…/mkxp-z.log'
mkxp-z.log     vita-boot: breadcrumb 'ux0:/data/mkxp-z/launch-in-progress.txt'
mkxp-z.log     vita-config: layers default=ok global=… root=… arg=ok
mkxp-z.log     vita-gpu: fixed render surfaces reserved=3 at 640x480 (rgss3, …)
mkxp-z.log     vita-boot: returning to launcher (rc=0)
launcher.log   vita-boot: mode=launcher …
```

After a crash, the next launcher boot adds:

```
launcher.log   vita-boot: previous run did not exit cleanly (game=… log=…)
launcher.log   launcher: last-error consumed bytes=…
```

`reason=` is one of `no-args`, `arg`, `bad-arg`, `config`, `config-unreadable`;
`config=` is `ok`, `absent`, `unreadable` or `too-large`. Together they say
which of the four rules above fired and what the root config had to do with it.

The `vita-gpu:` line is the render-surface gate. A launcher-started boot has
to report the same numbers as a pinned direct boot of the *same ELF*: the port
reserves its render surfaces at boot, and a hand-over must leave the same
budget.
Earlier builds also logged `vita-gpu: boot headroom surfaces=<n>` after it,
from a canary enabled by `gpu-headroom.enabled`; that canary returns at once on
vitaGL, so the line cannot occur on this build.

## Preflight sidecar

A game folder may carry an optional `vita-preflight.json` next to `Game.ini`.
This port does not ship a tool that writes it; the file is a format for tool
authors. It is a small (at most 2 KiB) advisory summary of a static scan of the
game's scripts and assets, made on a host. Missing, malformed and
future-schema sidecars are never launch failures, and a sidecar is an advisory
snapshot: the launcher does not rehash the game or claim acceptance on the
running build.

The file is one JSON object in which every key below appears exactly once:

| Key | Value |
|---|---|
| `schema_version` | `1` |
| `engine` | `"XP"`, `"VX"` or `"VXAce"` |
| `assessment` | always `"not_tested"` |
| `input_sha256`, `scanner_sha256`, `rulebook_sha256`, `report_sha256` | 64 lowercase hex digits each, identifying the scanned files, the scanner, its rules and its full report |
| `complete` | `true` when the scan covered everything |
| `warnings` | object mapping any of `native`, `threads`, `process`, `save`, `resize`, `midi`, `audio`, `tileset`, `image`, `dynamic`, `incomplete` to a positive reference count (at most 20000) |

`incomplete` must be present exactly when `complete` is `false`.

When a game has a valid sidecar, the launcher shows a player-facing warning page
before handing over, including a summary with no listed warnings. Each warning
says what may fail and what to try. Cross or Circle continues, Square returns to
the same list selection, and Left/Right switches between Warnings and Details.
Both pages say the game is not tested on this build. The confirming press must
be released before another action is accepted; after 1500 ms, fresh button
edges are accepted even if that release was missed, but the timeout never
confirms by itself. Missing or invalid sidecars proceed directly to launch
without an extra screen, wait or confirmation.

`vita/launcher/preflight.{c,h}` supplies the bounded reader and page
navigation. The launcher view uses its existing TrueType faces, list rules,
canvas and texture; no second GPU surface is created.

A failed game-directory scan shows "Scan failed" with a Triangle retry hint,
instead of claiming there are zero games. A genuinely empty or missing games
folder still shows the folder-layout instructions. Rescanning replaces the
error notice once the scan succeeds.

## Where the code is

```
vita/launcher/vita_boot.{h,cpp}   the decision, the breadcrumb, the reporting, the tail of main()
vita/launcher/launch_args.c       the argv contract
vita/launcher/vita_loadexec.c     the hand-over
vita/launcher/vita_launcher.c     the front end
src/main.cpp                      the launcher hooks and the order inside mkxp_main
src/meson.build                   the vita_launcher option and the source list
vita/mkxp-z-vpk/mkxp.json         the unpinned root config the product ships
```
