# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack the linked engine into the image the FAP installs into flash.

    mkblob.py engine.elf engine.flash engine.bin

engine.flash is `objcopy -O binary` of the ELF: everything from the start of
FLASH in blob.ld (code, constants, initial .got/.data). This adds the 64-byte
fz_image_header_t in front and the relocation list behind it.

It also enforces the one rule that keeps the image position independent with
respect to RAM: nothing in flash may hold an absolute address of a RAM object,
because RAM is only known when the loader runs. Such references come from
non-PIC code (e.g. a libc function with global state) or from const data
pointing at variables, and are reported as errors.
"""
import os
import re
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

IMAGE_ADDRESS = 0x080BD000
HEADER_SIZE = 64
GAP_SIZE = 104 * 1024
RAM_LINK, RAM_LINK_SIZE = 0x10000000, 128 * 1024
MAGIC = 0x4D4F4F44
# the loader refuses an image for another API version: take it from fz_api.h
API_VERSION = int(re.search(r"#define FZ_API_VERSION (\d+)", open(
    os.path.join(os.path.dirname(__file__), "..", "flipper", "fz_api.h")).read()).group(1))

R_ARM_ABS32 = 2
ABSOLUTE = {R_ARM_ABS32, 43, 44, 47, 48}  # ABS32, MOVW/MOVT (ARM and Thumb)


def in_ram(addr):
    return RAM_LINK <= addr < RAM_LINK + RAM_LINK_SIZE


def main(elf_path, flash_path, out_path):
    f = open(elf_path, "rb")
    elf = ELFFile(f)
    symtab = elf.get_section_by_name(".symtab")
    sym = {s.name: s["st_value"] for s in symtab.iter_symbols() if s.name}

    ram_start, data_end, bss_end = sym["__ram_start"], sym["__data_end"], sym["__bss_end"]
    ram_load = sym["__ram_load"]
    got = sym.get("_GLOBAL_OFFSET_TABLE_", ram_start)

    flash_secs = {s.name for s in elf.iter_sections() if s["sh_flags"] & 2 and not in_ram(s["sh_addr"]) and s["sh_addr"]}
    errors, data_relocs = [], []
    for rs in elf.iter_sections():
        if not isinstance(rs, RelocationSection):
            continue
        target = elf.get_section(rs["sh_info"])
        for r in rs.iter_relocations():
            s = symtab.get_symbol(r["r_info_sym"])
            value = s["st_value"]
            if s["st_info"]["type"] == "STT_SECTION":
                value = elf.get_section(s["st_shndx"])["sh_addr"] if isinstance(s["st_shndx"], int) else 0
            if r["r_info_type"] not in ABSOLUTE or not in_ram(value):
                continue
            if target.name in flash_secs:
                errors.append(f"{target.name}+{r['r_offset'] - target['sh_addr']:#x}: absolute reference to RAM symbol '{s.name or target.name}'")
            elif in_ram(target["sh_addr"]) and r["r_info_type"] == R_ARM_ABS32:
                data_relocs.append(r["r_offset"] - ram_start)

    flash = open(flash_path, "rb").read()
    base = IMAGE_ADDRESS + HEADER_SIZE

    def flash_word(addr):
        return struct.unpack_from("<I", flash, addr - base)[0]

    # GOT entries are addresses; the RAM ones move with the RAM block
    got_relocs = []
    gsec = elf.get_section_by_name(".got")
    if gsec is not None and gsec["sh_size"]:
        lma = ram_load + (gsec["sh_addr"] - ram_start)
        for i in range(0, gsec["sh_size"], 4):
            if in_ram(flash_word(lma + i)):
                got_relocs.append(gsec["sh_addr"] - ram_start + i)

    relocs = sorted(set(got_relocs + data_relocs))
    if errors:
        print("RAM references from flash (the image would not be relocatable):")
        for e in errors[:40]:
            print("  " + e)
        print(f"{len(errors)} error(s)")
        sys.exit(1)

    body = flash + b"".join(struct.pack("<I", r) for r in relocs)
    image_size = HEADER_SIZE + len(body)
    if image_size > GAP_SIZE:
        sys.exit(f"image is {image_size} bytes, the flash gap has {GAP_SIZE}")

    header = struct.pack(
        "<16I", MAGIC, API_VERSION, image_size, zlib.crc32(body),
        sym["fz_engine_main"] | 1, ram_load, data_end - ram_start, bss_end - ram_start,
        got - ram_start, base + len(flash), len(relocs), 0, 0, 0, 0, 0)
    open(out_path, "wb").write(header + body)

    print(f"engine image {image_size} B ({image_size / 1024:.1f} of {GAP_SIZE // 1024} KB flash)")
    print(f"RAM block {bss_end - ram_start} B (.got+.data {data_end - ram_start}, GOT at +{got - ram_start}), {len(relocs)} relocations")


if __name__ == "__main__":
    main(*sys.argv[1:4])
