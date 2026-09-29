# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure-Python XXH3_64bits (seed 0, default secret), the hash vitaGL keys its shader cache with.

Verified against the pinned vitaGL utils/xxhash_utils.h.
"""

M64 = (1 << 64) - 1
P32_1, P32_2, P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
P64_1, P64_2, P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
P64_4, P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
MX1, MX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25

SECRET = bytes.fromhex(
    "b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f"
    "cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c"
    "3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8"
    "a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364"
    "eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e"
    "2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e")


def _r32(b, i):
    return int.from_bytes(b[i:i + 4], "little")


def _r64(b, i):
    return int.from_bytes(b[i:i + 8], "little")


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _fold(a, b):
    p = a * b
    return (p ^ (p >> 64)) & M64


def _avalanche(h):
    h ^= h >> 37
    h = (h * MX1) & M64
    return h ^ (h >> 32)


def _avalanche64(h):
    h ^= h >> 33
    h = (h * P64_2) & M64
    h ^= h >> 29
    h = (h * P64_3) & M64
    return h ^ (h >> 32)


def _mix16(d, i, s):
    return _fold(_r64(d, i) ^ _r64(SECRET, s), _r64(d, i + 8) ^ _r64(SECRET, s + 8))


def _long(d):
    n = len(d)
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]

    def accumulate(off, soff):
        for i in range(8):
            v = _r64(d, off + 8 * i)
            k = v ^ _r64(SECRET, soff + 8 * i)
            acc[i ^ 1] = (acc[i ^ 1] + v) & M64
            acc[i] = (acc[i] + (k & 0xFFFFFFFF) * (k >> 32)) & M64

    blocks = (n - 1) // 1024
    for b in range(blocks):
        for s in range(16):
            accumulate(b * 1024 + s * 64, s * 8)
        for i in range(8):
            a = acc[i]
            a ^= a >> 47
            a ^= _r64(SECRET, 128 + 8 * i)
            acc[i] = (a * P32_1) & M64
    for s in range(((n - 1) - 1024 * blocks) // 64):
        accumulate(blocks * 1024 + s * 64, s * 8)
    accumulate(n - 64, 192 - 64 - 7)
    r = (n * P64_1) & M64
    for i in range(4):
        r = (r + _fold(acc[2 * i] ^ _r64(SECRET, 11 + 16 * i),
                       acc[2 * i + 1] ^ _r64(SECRET, 11 + 16 * i + 8))) & M64
    return _avalanche(r)


def xxh3_64(d):
    n = len(d)
    if n == 0:
        return _avalanche64(_r64(SECRET, 56) ^ _r64(SECRET, 64))
    if n <= 3:
        c = (d[0] << 16) | (d[n >> 1] << 24) | d[n - 1] | (n << 8)
        return _avalanche64(c ^ ((_r32(SECRET, 0) ^ _r32(SECRET, 4)) & M64))
    if n <= 8:
        k = ((_r32(d, n - 4) + (_r32(d, 0) << 32)) & M64) ^ (_r64(SECRET, 8) ^ _r64(SECRET, 16))
        k ^= _rotl(k, 49) ^ _rotl(k, 24)
        k = (k * MX2) & M64
        k ^= (k >> 35) + n
        k = (k * MX2) & M64
        return k ^ (k >> 28)
    if n <= 16:
        lo = _r64(d, 0) ^ (_r64(SECRET, 24) ^ _r64(SECRET, 32))
        hi = _r64(d, n - 8) ^ (_r64(SECRET, 40) ^ _r64(SECRET, 48))
        acc = (n + int.from_bytes(lo.to_bytes(8, "little"), "big") + hi + _fold(lo, hi)) & M64
        return _avalanche(acc)
    if n <= 128:
        acc = (n * P64_1) & M64
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(d, 48, 96) + _mix16(d, n - 64, 112)
                acc += _mix16(d, 32, 64) + _mix16(d, n - 48, 80)
            acc += _mix16(d, 16, 32) + _mix16(d, n - 32, 48)
        acc += _mix16(d, 0, 0) + _mix16(d, n - 16, 16)
        return _avalanche(acc & M64)
    if n <= 240:
        acc = (n * P64_1) & M64
        for i in range(8):
            acc += _mix16(d, 16 * i, 16 * i)
        acc = _avalanche(acc & M64)
        for i in range(8, n // 16):
            acc += _mix16(d, 16 * i, 16 * (i - 8) + 3)
        acc += _mix16(d, n - 16, 136 - 17)
        return _avalanche(acc & M64)
    return _long(d)
