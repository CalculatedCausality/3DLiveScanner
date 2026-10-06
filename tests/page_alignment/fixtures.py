"""Small synthetic ELF64/AArch64 and ZIP fixtures; no SDK/compiler required.

These are static layout specimens, not executable libraries. Every variant has real
program/section headers and a minimal dynamic table. No vendor ELF is modified.
"""

import io
import struct
import zipfile


def elf_fixture(layout="aligned", align=0x4000, relro=True):
    """aligned: end 0x14000; overlap: writable at 0x11000;
    gap: ARCore-like RELRO-only LOAD ending 0x11000 and next RW at 0x14200;
    prefix: writable bytes before RELRO on its first 16 KiB page.
    """
    rw = 0x10000
    dynamic_offset = rw + 0x100
    nominal_end = 0x14000 if layout in ("aligned", "prefix") else 0x11000
    data_va = nominal_end
    data_offset = data_va
    rw_file_end = data_offset + 0x20
    rw_end = data_va + 0x100
    if layout == "gap":
        data_va, data_offset = 0x14200, 0x10200
        rw_file_end, rw_end = 0x10200, 0x11000
    relro_start = rw if layout != "prefix" else dynamic_offset
    segments = [
        (1, 5, 0, 0, 0, 0x1000, 0x1000, align),
        (1, 6, rw, rw, rw, rw_file_end - rw, rw_end - rw, align),
    ]
    if layout == "gap":
        segments.append((1, 6, data_offset, data_va, data_va, 0x20, 0x100, align))
    segments.append((2, 6, dynamic_offset, dynamic_offset, dynamic_offset, 64, 64, 8))
    if relro:
        segments.append((0x6474E552, 4, relro_start, relro_start, relro_start,
                         min(rw_file_end, nominal_end) - relro_start,
                         nominal_end - relro_start, 1))
    # (name, type, flags, address, offset, size, link, info, align, entsize)
    sections = [
        ("", 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (".text", 1, 6, 0x500, 0x500, 0x100, 0, 0, 16, 0),
        (".dynstr", 3, 2, 0x400, 0x400, 9, 0, 0, 1, 0),
        (".data.rel.ro", 1, 3, rw, rw, 0x100, 0, 0, 8, 0),
        (".dynamic", 6, 3, dynamic_offset, dynamic_offset, 64, 2, 0, 8, 16),
        (".data", 1, 3, data_va, data_offset, 0x20, 0, 0, 8, 0),
        (".bss", 8, 3, data_va + 0x20, data_offset + 0x20, 0xE0, 0, 0, 8, 0),
    ]
    strings = b"\0"
    offsets = {}
    for name in [s[0] for s in sections] + [".shstrtab"]:
        offsets[name] = 0 if name == "" else len(strings)
        if name:
            strings += name.encode() + b"\0"
    str_offset = max(rw_file_end, data_offset + 0x20) + 0x100
    sections.append((".shstrtab", 3, 0, 0, str_offset, len(strings), 0, 0, 1, 0))
    shoff = (str_offset + len(strings) + 7) // 8 * 8
    data = bytearray(shoff + len(sections) * 64)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, ident, 3, 183, 1, 0,
                     64, shoff, 0, 64, 56, len(segments), 64, len(sections), len(sections) - 1)
    for i, p in enumerate(segments):
        struct.pack_into("<IIQQQQQQ", data, 64 + i * 56, *p)
    for i, s in enumerate(sections):
        struct.pack_into("<IIQQQQIIQQ", data, shoff + i * 64, offsets[s[0]], *s[1:])
    data[0x400:0x409] = b"\0libc.so\0"
    for i, pair in enumerate([(1, 1), (5, 0x400), (10, 9), (0, 0)]):
        struct.pack_into("<qQ", data, dynamic_offset + i * 16, *pair)
    data[str_offset:str_offset + len(strings)] = strings
    return bytes(data)


def change_field(data, offset, fmt, value):
    """Corrupt a synthetic fixture field for fail-closed tests."""
    changed = bytearray(data)
    struct.pack_into(fmt, changed, offset, value)
    return bytes(changed)


def section_offset(data, index):
    return struct.unpack_from("<Q", data, 40)[0] + index * 64


def apk_fixture(elf, *, aligned=True, compressed=False, abi="arm64-v8a"):
    memory = io.BytesIO()
    name = f"lib/{abi}/libfixture.so"
    entry = zipfile.ZipInfo(name)
    entry.compress_type = zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED
    if aligned:
        padding = (-30 - len(name.encode())) % 16384
        entry.extra = struct.pack("<HH", 0xCAFE, padding - 4) + bytes(padding - 4)
    with zipfile.ZipFile(memory, "w") as archive:
        archive.writestr(entry, elf)
    return memory.getvalue()
