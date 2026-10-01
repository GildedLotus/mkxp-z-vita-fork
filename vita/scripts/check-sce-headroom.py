#!/usr/bin/env python3
"""Check the SCE-data reserve between RX and RW.

vita-elf-create appends S bytes of SCE module data at ALIGN4(RX_end) of the
linked ELF; the loader maps RW at ALIGN_1MB(RX_end + S) when it does not honour
the preferred address. vita/linker/armvita-1mb-data.ld reserves
__mkxpz_sce_reserve bytes for S. This compares the linked ELF with the velf,
prints S and the margins, and fails when S leaves less than --threshold of the
reserve or when RW is no longer where the loader would put it.

usage: check-sce-headroom.py [--threshold BYTES] LINKED_ELF VELF LDSCRIPT
"""
import argparse
import re
import struct
import sys

MIB = 0x100000
PT_LOAD, PF_X = 1, 1


def load_segments(data):
    """(vaddr, memsz, flags) of each PT_LOAD in a little-endian ELF32 image."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a little-endian ELF32 file")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segments = []
    for i in range(phnum):
        p_type, _, vaddr, _, _, memsz, flags, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == PT_LOAD:
            segments.append((vaddr, memsz, flags))
    return segments


def rx_rw(segments):
    rx = next(s for s in segments if s[2] & PF_X)
    rw = next(s for s in segments if not s[2] & PF_X and s[0] > rx[0])
    return rx, rw


def measure(linked, velf, reserve):
    """Headroom figures from the PT_LOAD lists of the linked ELF and the velf."""
    (rx_base, rx_size, _), (rw, _, _) = rx_rw(linked)
    (vrx_base, vrx_size, _), (vrw, _, _) = rx_rw(velf)
    rx_end, final_end = rx_base + rx_size, vrx_base + vrx_size
    loader_rw = -(-final_end // MIB) * MIB
    return {"sce": final_end - rx_end, "reserve": reserve, "margin": reserve - (final_end - rx_end),
            "rx_end": rx_end, "final_end": final_end, "rw": rw, "velf_rw": vrw, "loader_rw": loader_rw,
            "growth": rw - reserve - rx_end}


def problems(m, threshold):
    out = []
    if m["velf_rw"] != m["rw"]:
        out.append("velf moved RW from 0x%08x to 0x%08x" % (m["rw"], m["velf_rw"]))
    if m["loader_rw"] != m["rw"]:
        out.append("RW 0x%08x != loader ALIGN_1MB(RX end + SCE data) 0x%08x" % (m["rw"], m["loader_rw"]))
    if m["margin"] < threshold:
        out.append("SCE data %d B leaves %d B of the %d B reserve (< %d B): raise __mkxpz_sce_reserve"
                   % (m["sce"], m["margin"], m["reserve"], threshold))
    return out


def reserve_from(ldscript):
    match = re.search(r"__mkxpz_sce_reserve\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;", ldscript)
    if not match:
        raise ValueError("linker script defines no __mkxpz_sce_reserve")
    return int(match.group(1), 0)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--threshold", type=lambda v: int(v, 0), default=16 * 1024)
    parser.add_argument("linked")
    parser.add_argument("velf")
    parser.add_argument("ldscript")
    args = parser.parse_args(argv)
    with open(args.linked, "rb") as a, open(args.velf, "rb") as b, open(args.ldscript) as c:
        m = measure(load_segments(a.read()), load_segments(b.read()), reserve_from(c.read()))
    print("sce-headroom: SCE data %d B, reserve %d B, margin %d B (threshold %d B); RX end 0x%08x"
          " (+SCE 0x%08x), RW 0x%08x; code growth before RW moves to the next MiB: %d B"
          % (m["sce"], m["reserve"], m["margin"], args.threshold, m["rx_end"], m["final_end"],
             m["rw"], m["growth"]))
    errors = problems(m, args.threshold)
    for error in errors:
        print("sce-headroom: FAIL: " + error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
