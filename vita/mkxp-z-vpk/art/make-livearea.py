#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the LiveArea art: icon0.png (128x128), bg.png (840x500) and
startup.png (280x158), all 8-bit paletted pixel art.

Usage: make-livearea.py OUT_DIR   (writes OUT_DIR/icon0.png, bg.png, startup.png)
Requires Pillow. Not run by the build: the outputs are tracked under
vita/mkxp-z-vpk/sce_sys and reproduce byte-identically with Pillow 12.3.0.
"""
from PIL import Image
import random, sys

S = 4
SKY = [(14, 18, 52), (20, 28, 78), (30, 42, 110), (44, 62, 144), (64, 86, 170)]
STAR = (255, 255, 160)
MOON_L, MOON_D = (236, 232, 200), (200, 196, 168)
MTN_FAR, MTN_NEAR = (40, 48, 96), (28, 36, 72)
GRASS, GRASS_D = (60, 176, 67), (31, 122, 44)
PATH, PATH_D = (196, 164, 108), (150, 118, 72)
STONE, STONE_D, K = (150, 160, 184), (96, 106, 132), (16, 16, 24)
WIN, WIN_L = (248, 208, 64), (255, 240, 150)
TREE, TREE_D, TRUNK = (36, 140, 60), (20, 92, 40), (110, 70, 34)
ROOF, ROOF_D = (170, 40, 56), (120, 20, 36)

def sky(x, y, h):
    n = len(SKY)
    band = min(n - 1, y * n // h)
    edge = (y * n) % h < max(1, h // 40) and band > 0 and (x + y) % 2 == 0
    return SKY[band - 1] if edge else SKY[band]

def put_rect(px, x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            px[x, y] = c

def background(W=210, H=125):
    img = Image.new('RGB', (W, H)); px = img.load()
    horizon = 82
    for y in range(H):
        for x in range(W):
            px[x, y] = sky(x, y, horizon)
    rnd = random.Random(7)
    for _ in range(70):
        x, y = rnd.randrange(W), rnd.randrange(horizon - 20)
        px[x, y] = STAR
    for (x, y) in ((30, 10), (150, 14), (185, 30), (70, 22)):       # twinkles
        for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
            px[x + dx, y + dy] = STAR
    mx, my, mr = 170, 16, 9                                           # moon with craters
    for y in range(my - mr, my + mr + 1):
        for x in range(mx - mr, mx + mr + 1):
            d2 = (x - mx) ** 2 + (y - my) ** 2
            if d2 <= mr * mr:
                px[x, y] = MOON_L if (x - mx + 2) ** 2 + (y - my + 2) ** 2 <= mr * mr else MOON_D
    for cx, cy, r in ((166, 13, 2), (174, 20, 1), (172, 11, 1)):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    px[x, y] = MOON_D
    def ridge(base, amp, seed, col):                                  # stepped mountain ridges
        r = random.Random(seed); h = base; d = -1
        for x in range(W):
            if r.random() < 0.12: d = -d
            h = max(base - amp, min(base, h + d * r.choice((0, 1, 1, 2))))
            for y in range(h, horizon + 2):
                px[x, y] = col
    ridge(72, 28, 3, MTN_FAR)
    ridge(80, 14, 9, MTN_NEAR)
    tuft = random.Random(11)
    for y in range(horizon, H):                                       # grass with scattered tufts
        for x in range(W):
            px[x, y] = GRASS
    for _ in range(140):
        tx, ty = tuft.randrange(1, W - 1), tuft.randrange(horizon + 3, H - 1)
        px[tx - 1, ty] = GRASS_D; px[tx + 1, ty] = GRASS_D; px[tx, ty - 1] = GRASS_D
    for y in range(horizon, horizon + 2):
        for x in range(W):
            px[x, y] = GRASS_D if (x + y) % 2 else GRASS
    # castle on the right hill
    cx0, cy1 = 128, horizon
    put_rect(px, cx0 - 2, cy1 - 14, cx0 + 34, cy1, K)
    put_rect(px, cx0 - 1, cy1 - 13, cx0 + 33, cy1, STONE)
    for t in range(4):                                                # towers
        tx = cx0 - 1 + t * 11
        put_rect(px, tx - 1, cy1 - 25, tx + 6, cy1 - 12, K)
        put_rect(px, tx, cy1 - 24, tx + 5, cy1 - 12, STONE)
        put_rect(px, tx + 4, cy1 - 24, tx + 5, cy1 - 12, STONE_D)
        for k in range(3):                                            # roof steps
            put_rect(px, tx + k - 1, cy1 - 28 - k, tx + 6 - k, cy1 - 25 - k, ROOF if k < 2 else ROOF_D)
        put_rect(px, tx + 2, cy1 - 20, tx + 3, cy1 - 17, WIN)
    for k in range(0, 34, 3):                                         # crenellations
        put_rect(px, cx0 - 1 + k, cy1 - 15, cx0 + 1 + k, cy1 - 13, STONE)
    put_rect(px, cx0 + 13, cy1 - 8, cx0 + 19, cy1, K)                 # gate
    put_rect(px, cx0 + 14, cy1 - 7, cx0 + 18, cy1, (60, 40, 30))
    for wx in (cx0 + 4, cx0 + 25):
        put_rect(px, wx, cy1 - 9, wx + 2, cy1 - 6, WIN_L)
    # winding path from the castle gate to the viewer
    x = cx0 + 16.0
    for y in range(horizon, H):
        w = 2 + (y - horizon) // 5
        x += (-0.9 if y < 100 else 0.5)
        for dx in range(int(x - w), int(x + w)):
            if 0 <= dx < W:
                px[dx, y] = PATH if (dx + y) % 5 else PATH_D
    def tree(tx, ty, s=1):                                            # pine trees
        for row in range(6 * s):
            half = (row // s) // 2 + 1
            for dx in range(-half, half + 1):
                px[tx + dx, ty + row] = TREE if dx < half else TREE_D
        put_rect(px, tx, ty + 6 * s, tx + 1 + (s > 1), ty + 6 * s + 2 * s, TRUNK)
    for tx, ty, s in ((20, horizon - 4, 2), (34, horizon + 2, 2), (9, horizon + 10, 2), (58, horizon + 14, 1),
                      (196, horizon - 2, 1), (184, horizon + 6, 2), (110, horizon + 16, 1)):
        tree(tx, ty, s)
    return img

FONT = {  # 5x5 glyphs
    'S': ["01111", "10000", "01110", "00001", "11110"],
    'T': ["11111", "00100", "00100", "00100", "00100"],
    'A': ["01110", "10001", "11111", "10001", "10001"],
    'R': ["11110", "10001", "11110", "10100", "10011"],
}

def startup(W=70, H=40):
    img = Image.new('RGB', (W, H)); px = img.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = sky(x, y, H)
    # classic RPG menu window
    put_rect(px, 4, 6, W - 4, H - 6, (240, 240, 232))
    put_rect(px, 5, 7, W - 5, H - 7, (150, 160, 200))
    for y in range(8, H - 8):
        put_rect(px, 6, y, W - 6, y + 1, (40, 62, 170) if y < H // 2 else (24, 36, 110))
    text = "START"
    tw = len(text) * 6 - 1
    x0, y0 = (W - tw) // 2 + 3, (H - 5) // 2
    for i, ch in enumerate(text):
        for r, row in enumerate(FONT[ch]):
            for c, bit in enumerate(row):
                if bit == '1':
                    px[x0 + i * 6 + c + 1, y0 + r + 1] = K
                    px[x0 + i * 6 + c, y0 + r] = (255, 255, 255)
    cx = x0 - 7                                                        # cursor arrow
    for r, row in enumerate(["100", "110", "111", "110", "100"]):
        for c, bit in enumerate(row):
            if bit == '1':
                px[cx + c, y0 + r] = (248, 208, 64)
    return img

def save_paletted(img, size, path):
    # The LiveArea installer drops the whole page unless every image is 8-bit
    # paletted with a full 256-entry palette (a 4-bit, 12-colour startup.png failed).
    big = img.resize((img.width * S, img.height * S), Image.NEAREST).crop((0, 0) + size)
    p = big.convert('P', palette=Image.ADAPTIVE, colors=256)
    pal = p.getpalette()[:768]
    p.putpalette(pal + [0] * (768 - len(pal)))
    p.save(path, bits=8)


PAL = {
    'K': (16, 16, 24),     # outline
    'B': (24, 36, 110),    # window blue, dark
    'b': (40, 62, 170),    # window blue, light
    'W': (240, 240, 232),  # window border
    'w': (150, 160, 200),  # border shade
    'g': (60, 176, 67),    # grass
    'G': (31, 122, 44),    # grass shade
    's': (245, 196, 154),  # skin
    'h': (176, 190, 206),  # steel
    'H': (96, 112, 128),   # steel shade
    'r': (208, 40, 56),    # cape
    'R': (138, 16, 32),    # cape shade
    'y': (248, 208, 64),   # gold
    'e': (232, 240, 255),  # blade
    'n': (138, 90, 42),    # leather
    'x': (255, 255, 160),  # sparkle
}

# 32x32. Row strings; '.' = background (filled with the window gradient below).
ART = [
    "................................",
    "................................",
    "................................",
    "................................",
    ".....................Ke.........",
    "....................Kee.........",
    "...................Kee..........",
    "..................Kee...........",
    ".........KKKK....Kee............",
    "........KhhhhK..Kee.............",
    ".......KhHhhhhKKee..............",
    ".......KhhyyhhKey...............",
    ".......KhssssKyyK...............",
    ".......KsKssKsKnK...............",
    ".......KsssssKnK................",
    "......KrKsssKhnK................",
    ".....KrrKhhhhhhK................",
    ".....KrRKhhyhhHK................",
    "....KrrRKhhyhhHK................",
    "....KrRRKhhhhhHK................",
    "....KRRKKnnnnnnK................",
    "....KRK.KhhHKhhK................",
    ".....K..KhhK.KhhK...............",
    "........KHHK.KHHK...............",
    ".......KnnnK.KnnnK..............",
    "......KKKKK...KKKKK.............",
    "..ggggggggggggggggggggggggggg...",
    "..gGggGgggGggggGgggGggggGgGgg...",
    "..GGgGGGgGGGgGgGGGgGGgGGGgGGG...",
    "................................",
    "................................",
    "................................",
]

STARS = {(6, 20): 'x', (24, 7): 'x', (27, 13): 'x', (3, 12): 'x', (22, 21): 'x', (9, 24): 'x'}
MOON = (7, 7, 3)  # centre x, centre y, radius

def icon_background(x, y):
    # Night sky: banded gradient (dithered at band edges), a moon and stars.
    band = min(len(SKY) - 1, y * len(SKY) // 26)
    edge = (y * len(SKY)) % 26 < 2 and band > 0 and (x + y) % 2 == 0
    col = SKY[band - 1] if edge else SKY[band]
    mx, my, mr = MOON
    d2 = (x - mx) ** 2 + (y - my) ** 2
    if d2 <= mr * mr:
        col = (236, 232, 200) if (x - mx + 1) ** 2 + (y - my + 1) ** 2 <= mr * mr or d2 < 4 else (200, 196, 168)
        if (x, y) in ((6, 8), (8, 6)):
            col = (200, 196, 168)
    if (x, y) in STARS:
        col = PAL['x']
    # grassy hill: an arc along the bottom
    hill = 26 - (3 if 8 <= x <= 23 else 2 if 5 <= x <= 26 else 1 if 3 <= x <= 28 else 0)
    if y >= hill:
        col = PAL['g'] if y == hill else PAL['G'] if (x + y) % 3 else PAL['g']
    return col

SHIFT = 4

def render_icon(scale=4):
    assert len(ART) == 32 and all(len(r) == 32 for r in ART), [len(r) for r in ART]
    img = Image.new('RGB', (32, 32))
    for y, row in enumerate(ART):
        for x, c in enumerate(row):
            sx = x - SHIFT
            c = ART[y][sx] if 0 <= sx < 32 else '.'
            if c in 'gG':
                c = '.'
            if c == '.':
                img.putpixel((x, y), icon_background(x, y))
            else:
                img.putpixel((x, y), PAL[c])
    big = img.resize((32 * scale, 32 * scale), Image.NEAREST)
    return big.convert('P', palette=Image.ADAPTIVE, colors=256)


if __name__ == '__main__':
    out = sys.argv[1]
    render_icon().save(f'{out}/icon0.png', optimize=True)
    save_paletted(background(), (840, 500), f'{out}/bg.png')
    save_paletted(startup(), (280, 158), f'{out}/startup.png')
    print('ok')
