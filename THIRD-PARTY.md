<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Third-party components

The port's own files (everything this branch adds under `vita/`, `src/` and elsewhere, apart from the third-party
material named below) are licensed **GPL-3.0-or-later**: each carries an `SPDX-License-Identifier: GPL-3.0-or-later`
header wherever the file format allows one, and `LICENSE` holds the GNU GPL version 3 text. The combined work (this
repository plus every component below) is distributed under the **GNU GPL, version 3**: SceShaccCgExt, which is
linked into the executable, is GPL-3.0 without an "or later" grant. Every component's licence is compatible with
that, and each is followed as written. Files that keep their upstream licence (the CC0 Ruby preloads, the binutils
linker script, the patches to third-party sources, the SceShaccCgExt replacement file, the GXP binaries and the
`movieYuv.frag` shader, whose source bytes are shader-cache keys) carry no GPL-3.0-or-later header. Full texts are
in `licenses/` and ship in the release VPK at `app0:/licenses/` together with this file and `LICENSE`.
Versions come from this branch's pins and the link line of the vitaGL build (`build.ninja` of
`build/mkxp-z-vitagl`); "linked" means a static archive linked into `eboot.bin`, "bundled" a file inside the VPK.
No shared third-party library is loaded from the device except the user-supplied `libshacccg.suprx` (not shipped).

The **Text** column names the file that holds each component's licence text. `licenses/<file>#<heading>` is the
section of a multi-notice file whose `##########` heading starts with that name. Every text is taken from the
pinned tree or the release tarball of the pinned version, never from memory; where the tree carries no licence
text the entry says so under "Known gaps".

## Elections (where a component offers a choice)

- **FreeType 2.14.3** (linked via SDL2_ttf): FreeType licence (FTL) *or* GPL-2.0-or-later. We elect the **FTL** arm,
  which FreeType's own README states is compatible with GPLv3 (the combined work is GPL-3.0). The FTL asks for the
  credit line below in the documentation; it is in this file, `README.md` and `vita/README.md`. Electing the FTL
  leaves no copyleft obligation on FreeType, so no FreeType source is in the release archive.
- **uchardet 0.0.8**: MPL-1.1 / GPL-2.0-or-later / LGPL-2.1-or-later. We elect **GPL-2.0-or-later**, used under GPL-3.0.
- **OpenAL Soft 1.19.1**: LGPL-2.0-or-later (its COPYING is the LGPL 2.0 text, not 2.1). Elected to GPL-3 through
  LGPL-2 section 3 (a GPL notice may be substituted for the LGPL one).
- **pthread-embedded** (the VitaSDK libpthread, linked): its README says the library as a whole is LGPL 2 or later
  (COPYING.LIB is the 2.1 text); the Vita-specific parts are MIT. Elected to GPL-3 through LGPL-2.1 section 3
  (the section allows a later GPL version).
- **Ruby 3.1.3**: Ruby licence *or* BSD-2-Clause. We elect **BSD-2-Clause** (`licenses/Ruby-BSD-2-Clause.txt`;
  `licenses/Ruby-LEGAL.txt` lists the notices of the parts bundled inside Ruby).
- **mkxp-z**: GPL-2.0-or-later per its README, used under GPL-3.0. Its `assets/LICENSE.mkxp-z-with-https.txt`
  is a GPL-3 notice for the HTTPS build; this build links no OpenSSL.
- **stb_image, stb_vorbis, stb_dxt** (MIT or public domain), **dr_flac, dr_mp3** (public domain or MIT-0), **miniz**
  (public domain): used under their most permissive alternative; both texts ship.

## Engine and graphics stack

| Component | Version / commit | Licence | Upstream | Status | Text |
|---|---|---|---|---|---|
| mkxp-z (RGSS runtime; this tree is the fork's `vita` branch, cut from that commit) | 826929eeb3eb | GPL-2.0-or-later (used under GPL-3.0) | https://github.com/mkxp-z/mkxp-z | linked | `licenses/GPL-2.0.txt` (byte-equal to its `COPYING`) |
| vitaGL | 464876a79cdd | LGPL-3.0-or-later (every source header); bundles ETC/EAC/ATITC codecs, stb_dxt, xxHash, a GLSL preprocessor | https://github.com/Rinnegatamante/vitaGL | linked (static) | `licenses/LGPL-3.0.txt` (byte-equal to its `COPYING.LESSER`), `LICENSE` (its `COPYING`), `licenses/vitaGL-bundled-notices.txt`, `licenses/Apache-2.0.txt` |
| vitaShaRK | df24065e6509 | LGPL-3.0-or-later (source headers); bundles xxHash | https://github.com/Rinnegatamante/vitaShaRK | linked (static) | `licenses/vitaShaRK-LICENSE.txt`, `licenses/vitaGL-bundled-notices.txt` |
| SceShaccCgExt (patched by this branch) | fb0e9d338525 | GPL-3.0 (LICENSE file; its sources carry no headers); its red-black tree containers are replaced by `vita/patches/vitagl/shacccgext-files/src/shacccg_ext_oop.cpp` (the whole file, copied over the pinned one by `build-vitagl.sh`), written for this repository under GPL-3.0 | https://github.com/bythos14/SceShaccCgExt | linked (static) | `licenses/SceShaccCgExt-LICENSE.txt` |
| math-neon | 0faab814782c | MIT (expat) | https://github.com/Rinnegatamante/math-neon | linked (static) | `licenses/math-neon-MIT.txt` |
| taiHEN stubs | 309b3800bcb8 | MIT | https://github.com/yifanlu/taiHen | linked (import stubs) | `licenses/permissive-notices.txt#taiHEN stubs` |
| SDL2 (Vita port, patched here) | 2.32.8 | zlib, plus the notices bundled in its sources | https://github.com/libsdl-org/SDL | linked (static) | `licenses/permissive-notices.txt#SDL2`, `licenses/SDL2-bundled-notices.txt` |
| SDL vitaGL video backend (ported by `vita/patches/sdl2/0001` and `0002`) | Northfear's `vitagl` branch at f99bb638f7eb (SDL 2.24.0), ported onto SDL 2.32.8 | zlib: the fork is a branch of SDL and its files keep the SDL zlib header (Copyright Sam Lantinga) | the SDL fork of GitHub user Northfear, branch `vitagl` | linked (static, part of SDL2) | `licenses/permissive-notices.txt#SDL2` |
| SDL2_image, SDL2_ttf | 2.8.12, 2.24.0 | zlib; SDL2_image bundles stb_image, tiny_jpeg, miniz, nanosvg, qoi | https://github.com/libsdl-org | linked (static) | `licenses/permissive-notices.txt#SDL2`, `licenses/SDL2_image-bundled-notices.txt` |
| libpng | 1.6.58 | PNG Reference Library License | http://www.libpng.org | linked (static) | `licenses/permissive-notices.txt#libpng` |
| zlib | 1.3.2 | zlib | https://zlib.net | linked (static) | `licenses/zlib-1.3.2.txt` |
| bzip2 | 1.0.8 | bzip2 (BSD-style) | https://sourceware.org/bzip2/ | linked (static) | `licenses/permissive-notices.txt#bzip2` |
| libwebp (+sharpyuv) | 1.6.0 | BSD-3-Clause + patent grant | https://chromium.googlesource.com/webm/libwebp | linked (static) | `licenses/permissive-notices.txt#libwebp` |
| FreeType | 2.14.3 | FreeType Licence (FTL) arm elected; BDF/PCF drivers X11-style | https://freetype.org | linked (static) | `licenses/FreeType-2.14.3.txt` |
| pixman | 0.42.2 | MIT | https://cairographics.org/releases/ | linked (static) | `licenses/permissive-notices.txt#pixman` |
| PhysFS | 3.0.2 | zlib | https://github.com/icculus/physfs | linked (static) | `licenses/permissive-notices.txt#PhysFS` |

Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All rights reserved.

The shipped GXP shaders (`app0:/shader_cache/`, `vita/vitagl-shaders/`) are **compiler output of this repo's own
GLSL**, produced by the Sony shader compiler on a Vita; they carry the repo's licence (GPL-3.0) and contain no
third-party shader source (the vitaGL, vitaShaRK and SceShaccCgExt code that builds them at capture time is not
part of their bytes). `libshacccg.suprx` is Sony's and is neither shipped nor linked.

## Audio, video, text

| Component | Version / commit | Licence | Upstream | Status | Text |
|---|---|---|---|---|---|
| OpenAL Soft (+ Vita patch, isage) | 1.19.1 | LGPL-2.0-or-later, elected GPL-3 | https://github.com/kcat/openal-soft | linked (static) | `licenses/LGPL-2.0.txt` (byte-equal to its `COPYING`) |
| SDL_sound (mkxp-z fork) | cfb2533eb3ba | zlib; bundles dr_flac, dr_mp3, stb_vorbis | https://github.com/mkxp-z/SDL_sound | linked (static) | `licenses/permissive-notices.txt#SDL_sound`, `licenses/SDL_sound-bundled-notices.txt` |
| libogg, libvorbis | 1.3.6, 1.3.7 | BSD-3-Clause (Xiph) | https://xiph.org | linked (static) | `licenses/permissive-notices.txt#Xiph libogg`, `licenses/permissive-notices.txt#Xiph libvorbis` |
| libtheora | 1.1.1 | BSD-3-Clause (Xiph) | https://downloads.xiph.org/releases/theora/ | linked (static) | `licenses/permissive-notices.txt#Xiph libtheora` |
| theoraplay | in mkxp-z, `src/theoraplay` | zlib | https://icculus.org/theoraplay/ | bundled in the engine | `licenses/permissive-notices.txt#theoraplay`, `licenses/mkxp-z-vendored-notices.txt` |
| TinySoundFont `tsf.h` (MIT), `tml.h` (zlib) | 853a0a171759 | MIT / zlib | https://github.com/schellingb/TinySoundFont | compiled in (headers) | `licenses/permissive-notices.txt#TinySoundFont` |
| uchardet | 0.0.8 | GPL-2.0-or-later arm elected | https://gitlab.freedesktop.org/uchardet/uchardet | linked (static) | `licenses/uchardet-COPYING.txt` |

## Scripting, vendored code, toolchain

| Component | Version / commit | Licence | Upstream | Status | Text |
|---|---|---|---|---|---|
| Ruby (MRI, mkxp-z fork; patched by `vita/patches/ruby/`) | 3.1.3 @ 4d85560cf659 | BSD-2-Clause arm elected | https://github.com/mkxp-z/ruby | linked (`libruby-static.a`) | `licenses/Ruby-BSD-2-Clause.txt`, `licenses/Ruby-LEGAL.txt` |
| ghc::filesystem | as vendored in mkxp-z | MIT | https://github.com/gulrak/filesystem | compiled into the engine | `licenses/mkxp-z-vendored-notices.txt` |
| libnsgif, LUrlParser, cpp-httplib | as vendored in mkxp-z | MIT | https://www.netsurf-browser.org/projects/libnsgif/, https://github.com/corporateshark/LUrlParser, https://github.com/yhirose/cpp-httplib | compiled into the engine | `licenses/mkxp-z-vendored-notices.txt`, `licenses/permissive-notices.txt#Vendored mkxp-z sources` |
| rapidcsv | as vendored in mkxp-z | BSD-3-Clause | https://github.com/d99kris/rapidcsv | compiled into the engine (header) | `licenses/permissive-notices.txt#rapidcsv` |
| sigslot | as vendored in mkxp-z | MIT | https://github.com/palacaze/sigslot | compiled into the engine (header) | `licenses/permissive-notices.txt#sigslot` |
| json5pp | as vendored in mkxp-z | MIT | https://github.com/kimushu/json5pp | compiled into the engine (header) | `licenses/permissive-notices.txt#json5pp` |
| Game controller DB | `assets/gamecontrollerdb.txt` | zlib | https://github.com/gabomdq/SDL_GameControllerDB | bundled in the engine | `licenses/permissive-notices.txt#SDL2` (same wording, same copyright line) |
| VitaSDK newlib (libc, libm) | 4.1.0 @ 64aa7aa33d4f | BSD-style and other permissive notices per file | https://github.com/vitasdk/newlib | linked | `licenses/newlib-COPYING.NEWLIB.txt` |
| pthread-embedded (libpthread) | 11d2e5722d98 | LGPL-2.1-or-later (parts MIT), elected GPL-3 | https://github.com/vitasdk/pthread-embedded | linked (static) | `licenses/pthread-embedded.txt` |
| libstdc++, libgcc (GCC 15.2.0) | VitaSDK toolchain | GPL-3.0 with the GCC Runtime Library Exception | https://github.com/vitasdk | linked | `licenses/GCC-Runtime-Library-Exception.txt`, `LICENSE` |
| vita-headers import stubs (`libSce*_stub.a`) | ebc8f4f7ac83 | MIT | https://github.com/vitasdk/vita-headers | linked | `licenses/permissive-notices.txt#vita-headers stubs` |

## Fonts (in the VPK, `app0:/fonts/`)

| File | Licence | Text |
|---|---|---|
| `VL-Gothic-Regular.ttf`, `VL-PGothic-Regular.ttf` (VLGothic-20230918, v2.2306) | VL Gothic licence: BSD-style, from M+ FONTS and Sazanami Gothic terms | `licenses/VL-Gothic.txt` (the Japanese originals `LICENSE` and `LICENSE_J.mplus`, their English versions, and `README.sazanami`) |
| `LiberationSans-Regular.ttf` (Liberation 2.00.1, byte-equal to the file in the pinned tarball) | SIL OFL 1.1 | `licenses/OFL-1.1-Liberation.txt` (byte-equal to the `LICENSE` of `liberation-fonts-ttf-2.00.1.tar.gz`) |

Upstream: https://vlgothic.dicey.org/ (mirror https://github.com/daisukesuzuki/VLGothic) and
https://github.com/liberationfonts/liberation-fonts.

## Licence texts (`licenses/`)

`GPL-2.0.txt`, `LGPL-3.0.txt`, `LGPL-2.0.txt`, `OFL-1.1-Liberation.txt`, `VL-Gothic.txt`, `Ruby-BSD-2-Clause.txt`,
`Ruby-LEGAL.txt`, `GCC-Runtime-Library-Exception.txt`, `vitaShaRK-LICENSE.txt`, `SceShaccCgExt-LICENSE.txt`,
`math-neon-MIT.txt`, `zlib-1.3.2.txt`, `Apache-2.0.txt`, `vitaGL-bundled-notices.txt`, `FreeType-2.14.3.txt`, `uchardet-COPYING.txt`, `newlib-COPYING.NEWLIB.txt`,
`pthread-embedded.txt`, `SDL2-bundled-notices.txt`, `SDL2_image-bundled-notices.txt`,
`SDL_sound-bundled-notices.txt`, `mkxp-z-vendored-notices.txt` and `permissive-notices.txt` (Xiph, pixman, SDL,
libpng, bzip2, libwebp, PhysFS, theoraplay, TinySoundFont, rapidcsv, sigslot, json5pp, vita-headers, taiHEN,
the shared MIT wording). The GPL-3.0 text is `LICENSE`. vitaGL's `COPYING` is the same text as `LICENSE`, and its
`COPYING.LESSER` the same as `licenses/LGPL-3.0.txt`. The release build (`vita/scripts/build-release.sh`) checks that the release VPK carries every file in `licenses/`.

## Source for a release

Each release ships with `mkxp-z-source-<version>.tar.xz`, written by `vita/scripts/build-release.sh`
(`vita/scripts/source-archive.py`) and recorded, with its SHA-256 and component list, in `manifest.json`. It holds
this branch and the source of every dependency the build fetches and builds: vitaGL, vitaShaRK, SceShaccCgExt,
math-neon and taiHEN (with its submodules) at the commits in `vita/scripts/vitagl-pins.json`, the mkxp-z Ruby fork
(with the `config.guess` and `config.sub` its build uses), SDL2, SDL_sound, libtheora, uchardet, pixman, OpenAL Soft,
pthread-embedded, the TinySoundFont headers and the font distributions. Every download is pinned in
`vita/scripts/dep-pins.json` (SHA-256 or full commit) and refused on a mismatch; the pins are recorded in `manifest.json`.
Git-based dependencies are pristine trees at the pinned commit; tarball dependencies are the sealed trees the build
extracted (never built in); the changes this branch makes to them are `vita/patches/`, applied by the build scripts.
`vita/scripts/check-corresponding-source.py` fails the release unless every linked GPL or LGPL component in the
tables above has its source in the archive, and the archive builder refuses any member that contains the builder's
home directory, hostname, VitaSDK path or checkout path. With this branch, the archive and `vita/scripts/build-player.sh`, anyone
can rebuild the executable and relink it against a modified LGPL library (vitaGL, vitaShaRK, OpenAL Soft).

pthread-embedded 11d2e5722d98 (LGPL-2.1-or-later) is linked from the VitaSDK toolchain; its pinned source is in the
archive. This repository does not pin the exact VitaSDK build of that binary, so the archive holds the named upstream
commit, not a proof that the VitaSDK binary was built from it.

These components are linked from VitaSDK binary packages (`vdpm`) or the VitaSDK toolchain and are **not** in the
archive; the version and upstream source location of each are in the tables above, and the package recipes are in
https://github.com/vitasdk/packages: FreeType 2.14.3 (FTL arm; https://freetype.org), libstdc++ and libgcc from
GCC 15.2.0 (GPL-3.0 with the runtime exception, which puts no source obligation on a program built by an unmodified
GCC; https://github.com/vitasdk), newlib, and the permissively licensed SDL2_image, SDL2_ttf, libpng, zlib, bzip2,
libwebp, libogg, libvorbis, PhysFS and vita-headers stubs.

## Not shipped

`libshacccg.suprx` (no `module/` tree in the release), and RPG Maker RTPs or game data.
Present in the source trees but absent from the eboot (checked with `nm`): libjpeg-turbo, FLAC, HarfBuzz, libmodplug,
timidity, OpenSSL and the X11/Wayland/hidapi parts of SDL.

## Known gaps

- `sigslot`, `json5pp` and `rapidcsv` carry no licence text in the pinned mkxp-z tree; their texts are the upstream
  repositories' licence files. The vdpm-installed libraries (SDL2_ttf, SDL2_image, libpng, zlib, bzip2, libwebp,
  libogg, libvorbis, FreeType, PhysFS) have no source in this tree: their texts come from the release tarball of
  the same version (the vdpm package DB carries no licence fields), and match the notices in the installed headers.
- vitaGL's `source/utils/preprocessor/` (john-blackburn/preprocessor, GPL-3.0) and `shacccg_paramquery.h`
  (Pigs-In-A-Blanket, LGPL-3.0) carry no licence text in the pinned tree; the licences are the upstream repositories'.
  vitaGL's `debug.c` includes a Linux-kernel 6x10 font table (GPL-2.0 in the kernel); no `font` symbol is in the
  linked ELF, so it is not part of the release binary (checked with `nm`).
- The libnsgif sources say only "MIT"; the MIT wording is the one in `licenses/permissive-notices.txt`.
- The newlib COPYING is the whole file of the pinned VitaSDK revision. Individual newlib sources may carry
  further per-file notices that the file's own list covers.
- FTL/GPL-3 compatibility rests on FreeType's own README statement, which the FTL election relies on.
