<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Bundled fallback fonts (app0:/fonts/)

**No font binary is committed here.** `vita/scripts/fetch-fonts.sh` installs them
into `build/fonts/` (git-ignored) and `vita/scripts/package-vpk.sh` packs
`build/fonts/*.ttf`, this README and `fonts.json` into `fonts/` inside the VPK.
Packaging fails if the fetch has not run; `ALLOW_MISSING_FONTS=1` overrides
that for CI and packaging probes and produces a VPK that is not shippable.

mkxp resolves `Font.name` against the game's `Fonts/` directory
and falls back to what is bundled here. `SharedStatePrivate::init` mounts `ux0:/data/mkxp-z/fonts` and
then this directory **at `Fonts`**, after the game folder and the RTPs, so
every face here joins the same registry a game's own `Fonts/` fills and a
game's own copy still wins every name clash. The engine also carries a
fallback table that points the Windows family names RPG Maker projects ask
for at the three faces below. Dropping a `.ttf` into `ux0:/data/mkxp-z/fonts` on the card
adds a face without repacking the VPK.

| File | Version | Role | Licence | Source |
|---|---|---|---|---|
| `VL-Gothic-Regular.ttf` | 2.2306 | Japanese fallback, fixed pitch | VL Gothic Font Family licence (3-clause BSD style; M+ FONTS + Sazanami Gothic + Project Vine) | `VLGothic-20230918.tar.xz` from <https://vlgothic.dicey.org/> |
| `VL-PGothic-Regular.ttf` | 2.2306 | Japanese fallback, proportional | same | same archive |
| `LiberationSans-Regular.ttf` | 2.00.1 | Latin fallback, metric-compatible with Arial | SIL Open Font License 1.1 | pinned mkxp-z checkout, `assets/liberation.ttf` |
| `fonts.json` | — | provenance record: digest, licence and source of each file | — | tracked in this directory |

`fonts.json` is the machine-readable version of that table and is what both
scripts check against, so a stale or tampered font is a failed fetch rather
than a surprise on the device.

## Why three faces, and why not just Liberation

XP and VX games name Windows fonts (`MS PGothic`, `MS Gothic`, `Arial`) that
cannot be redistributed, and the **XP RTP ships no font at all**, so whatever
is bundled here is literally what those games render with.

Liberation Sans is the **2.00.1** file the pinned mkxp-z checkout already
carries (OFL 1.1, 2294 codepoints), staged rather than committed. It has no
kana or kanji, so it is the Latin fallback only.

Liberation alone is still not enough, and that is a hard constraint rather
than a preference: the launcher chooses the face it draws game titles with by
probing glyphs, not by name, because Shift_JIS titles are common. `fonts.json`
records the three it probes for as `requiredCodepoints` — U+3042 HIRAGANA A,
U+30A2 KATAKANA A, U+4E00 CJK ONE — and packaging refuses to build a VPK whose
fonts do not between them cover all three. VL Gothic covers 16119 codepoints
and VL PGothic 16720, including all of those.

The two VL files cost 7.85 MiB on the card and about 4.9 MiB of VPK download
(they deflate well). That is the price of Japanese text rendering at all, and
the alternative — copying a font out of an RTP — is an Enterbrain
redistribution we will not make.

## Licence texts

`vita/scripts/fetch-fonts.sh` also installs the upstream VL Gothic `LICENSE`,
`LICENSE.en`, `LICENSE_E.mplus`, `LICENSE_J.mplus`, `README*` and `Changelog`
next to the fonts in `build/fonts/`. They are not packed from there. The release
VPK carries the font licence texts under `app0:/licenses/` (`VL-Gothic.txt`,
`OFL-1.1-Liberation.txt`), with `LICENSE` and `THIRD-PARTY.md`.
`VL-Gothic.txt` holds all five VL Gothic texts, the Japanese originals (`LICENSE`,
`LICENSE_J.mplus`) included, taken from the pinned archive; `OFL-1.1-Liberation.txt` is byte-equal
to the `LICENSE` of the Liberation 2.00.1 tarball.

Liberation 2.00.1 declares OFL 1.1 in its own `name` table (id 13, with
<https://scripts.sil.org/OFL> as id 14); the mkxp-z checkout carries no
separate copy of the licence text, so the script fetches one.

## Adding a font

Add it to `fonts.json` with its digest, licence and source, teach
`vita/scripts/fetch-fonts.sh` where to get it, and check the redistribution terms
first. Never commit the binary, and never copy a font out of an
RTP.
