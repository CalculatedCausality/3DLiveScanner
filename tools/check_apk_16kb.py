#!/usr/bin/env python3
"""Read-only Android ARM64 16 KiB ELF/APK layout gate (Python standard library).

PASS means static layout evidence only, never runtime/device certification.
See tests/page_alignment/README.md for the supported profile and exact rules.
"""

import argparse
from dataclasses import asdict, dataclass, field
import hashlib
import json
from pathlib import Path
import struct
import sys
import zipfile
import zlib


PAGE = 16384
U64 = 1 << 64
PT_LOAD, PT_DYNAMIC, PT_TLS, PT_GNU_RELRO = 1, 2, 7, 0x6474E552
PF_X, PF_W = 1, 2
SHT_STRTAB, SHT_DYNAMIC, SHT_NOBITS = 3, 6, 8
SHF_WRITE, SHF_ALLOC, SHF_TLS, SHF_COMPRESSED = 1, 2, 0x400, 0x800
PROFILE = "android-arm64"
# Bound per-library decompression/memory use; exceeding this is UNKNOWN.
MAX_ELF_BYTES = 512 * 1024 * 1024


class InvalidELF(ValueError):
    pass


class UnsupportedELF(ValueError):
    pass


@dataclass
class Segment:
    index: int
    type: int
    flags: int
    offset: int
    vaddr: int
    paddr: int
    filesz: int
    memsz: int
    align: int

    @property
    def end(self):
        return self.vaddr + self.memsz


@dataclass
class Section:
    index: int
    name_offset: int
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    align: int
    entsize: int
    name: str = ""

    @property
    def end(self):
        return self.addr + self.size


@dataclass
class Result:
    name: str
    sha256: str = ""
    machine: object = None
    packaging: dict = field(default_factory=dict)
    loads: list = field(default_factory=list)
    relro: list = field(default_factory=list)
    needed: list = field(default_factory=list)
    issues: list = field(default_factory=list)
    notes: list = field(default_factory=list)

    def issue(self, code, message, unknown=False):
        self.issues.append({"code": code, "message": message, "unknown": unknown})

    @property
    def status(self):
        if any(not i["unknown"] for i in self.issues):
            return "FAIL"
        return "UNKNOWN" if self.issues else "PASS"

    def to_dict(self):
        return dict(asdict(self), status=self.status)


def require(condition, message):
    if not condition:
        raise InvalidELF(message)


def power_of_two(n):
    return n > 0 and n & (n - 1) == 0


def down(n):
    return n // PAGE * PAGE


def up(n):
    return (n + PAGE - 1) // PAGE * PAGE


def overlaps(a, b, c, d):
    return a < d and c < b


def check_range(offset, size, length, label):
    require(0 <= offset <= length and 0 <= size <= length - offset,
            f"{label}: range offset={offset:#x} size={size:#x} exceeds {length:#x}")


def cstring(data, offset, label):
    require(0 <= offset < len(data), f"{label}: invalid string offset {offset:#x}")
    end = data.find(b"\0", offset)
    require(end >= 0, f"{label}: unterminated string")
    return data[offset:end].decode("utf-8", errors="backslashreplace")


def parse_elf(data, result):
    require(len(data) >= 16 and data[:4] == b"\x7fELF", "missing/truncated ELF identification")
    if data[4:6] != b"\x02\x01":
        raise UnsupportedELF("profile requires ELF64 little-endian; ELF class/encoding unsupported")
    require(data[6] == 1, "invalid ELF identification version")
    if data[7] not in (0, 3) or data[8] != 0:
        raise UnsupportedELF("ELF OS ABI/ABI version outside Android ARM64 profile")
    require(len(data) >= 64, "truncated ELF64 header")
    (typ, machine, version, entry, phoff, shoff, flags, ehsize, phentsize,
     phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    result.machine = machine
    if machine != 183 or typ != 3 or flags != 0:
        raise UnsupportedELF(f"requires AArch64 ET_DYN with e_flags=0; machine={machine}, "
                             f"type={typ}, flags={flags:#x}")
    require(version == 1 and ehsize == 64, "invalid ELF version/header size")
    if phnum == 0xFFFF or (shnum == 0 and shoff) or shstrndx == 0xFFFF:
        raise UnsupportedELF("extended ELF table numbering is not supported")
    require(phnum > 0 and phentsize == 56, "missing/invalid program header table")
    require(phoff >= 64 and phoff % 8 == 0, "invalid program header table offset")
    check_range(phoff, phnum * phentsize, len(data), "program headers")
    if shoff == 0 or shnum == 0:
        raise UnsupportedELF("section headers absent: cannot establish writable-section/RELRO safety")
    require(shentsize == 64 and shoff >= 64 and shoff % 8 == 0,
            "invalid section header size/offset")
    check_range(shoff, shnum * shentsize, len(data), "section headers")
    require(not overlaps(phoff, phoff + phnum * 56, shoff, shoff + shnum * 64),
            "program and section header tables overlap")
    require(0 < shstrndx < shnum, "missing/invalid section-name string table index")
    segments = []
    for i in range(phnum):
        p = Segment(i, *struct.unpack_from("<IIQQQQQQ", data, phoff + i * 56))
        if p.type != 0:
            check_range(p.offset, p.filesz, len(data), f"PH[{i}] file bytes")
            require(p.memsz <= U64 - p.vaddr, f"PH[{i}]: virtual address overflow")
            require(p.align in (0, 1) or power_of_two(p.align), f"PH[{i}]: invalid p_align")
            if p.type in (PT_LOAD, PT_DYNAMIC, PT_TLS, PT_GNU_RELRO):
                require(p.filesz <= p.memsz, f"PH[{i}]: p_filesz exceeds p_memsz")
        segments.append(p)
    loads = [p for p in segments if p.type == PT_LOAD]
    require(loads and any(p.memsz for p in loads), "no nonempty PT_LOAD segments")
    require([p.vaddr for p in loads] == sorted(p.vaddr for p in loads), "PT_LOAD headers not in virtual-address order")
    require(any(p.offset == 0 and p.filesz >= phoff + phnum * 56 and p.flags & 4 for p in loads),
            "ELF/program headers not covered by a readable LOAD")
    sections = []
    for i in range(shnum):
        s = Section(i, *struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * 64))
        if i == 0:
            require(data[shoff:shoff + 64] == bytes(64), "invalid ELF null section")
        else:
            require(s.type != 0, f"SH[{i}]: unexpected additional NULL section")
            if s.type != SHT_NOBITS:
                check_range(s.offset, s.size, len(data), f"SH[{i}] file bytes")
            require(s.size <= U64 - s.addr, f"SH[{i}]: virtual address overflow")
            require(s.align in (0, 1) or power_of_two(s.align), f"SH[{i}]: invalid alignment")
            require(s.link < shnum, f"SH[{i}]: invalid linked section")
            if s.entsize:
                require(s.size % s.entsize == 0, f"SH[{i}]: size not divisible by entry size")
        sections.append(s)
    names = sections[shstrndx]
    require(names.type == SHT_STRTAB and names.size > 0, "invalid section-name string table")
    names_data = data[names.offset:names.offset + names.size]
    require(names_data[0] == 0 and names_data[-1] == 0, "invalid section-name string table boundaries")
    for s in sections:
        s.name = cstring(names_data, s.name_offset, f"SH[{s.index}] name")
        if not s.flags & SHF_ALLOC or not s.size:
            continue
        if s.flags & SHF_TLS:
            raise UnsupportedELF(f"{s.name!r}: native TLS layout is not modeled by this profile")
        require(not s.flags & SHF_COMPRESSED, f"{s.name!r}: compressed allocated section")
        require(s.align <= 1 or s.addr % s.align == 0, f"{s.name!r}: misaligned section address")
        containers = [p for p in loads if p.vaddr <= s.addr and s.end <= p.end]
        require(containers, f"{s.name!r}: allocated section outside PT_LOAD memory")
        if s.type != SHT_NOBITS:
            containers = [p for p in containers if s.offset == p.offset + s.addr - p.vaddr
                          and s.offset + s.size <= p.offset + p.filesz]
            require(containers, f"{s.name!r}: section/file mapping disagrees with PT_LOAD")
        if s.flags & SHF_WRITE:
            require(any(p.flags & PF_W for p in containers), f"{s.name!r}: writable section in non-writable LOAD")
    allocated = [s for s in sections if s.flags & SHF_ALLOC and s.size]
    require(allocated, "no allocated sections: insufficient layout evidence")
    ordered = sorted(allocated, key=lambda s: s.addr)
    for a, b in zip(ordered, ordered[1:]):
        require(a.end <= b.addr, f"allocated sections {a.name!r} and {b.name!r} overlap")
    for p in loads:
        if p.memsz:
            require(any(overlaps(p.vaddr, p.end, s.addr, s.end) for s in allocated),
                    f"LOAD[{p.index}]: no allocated sections; insufficient layout evidence")
    return segments, sections


def inspect_dynamic(data, segments, sections):
    dynamic = [p for p in segments if p.type == PT_DYNAMIC]
    require(len(dynamic) == 1, "expected exactly one PT_DYNAMIC for Android shared library")
    p = dynamic[0]
    require(p.filesz > 0 and p.filesz % 16 == 0, "invalid PT_DYNAMIC size")
    corresponding = [s for s in sections if s.type == SHT_DYNAMIC and s.flags & SHF_ALLOC]
    require(len(corresponding) == 1, "missing/ambiguous allocated SHT_DYNAMIC")
    s = corresponding[0]
    require((s.offset, s.addr, s.size) == (p.offset, p.vaddr, p.filesz),
            "SHT_DYNAMIC and PT_DYNAMIC disagree")
    values, needed = {}, []
    for off in range(p.offset, p.offset + p.filesz, 16):
        tag, value = struct.unpack_from("<qQ", data, off)
        if tag == 0:
            break
        if tag == 1:
            needed.append(value)
        elif tag in (5, 10):
            require(tag not in values, "duplicate dynamic string table tag")
            values[tag] = value
    else:
        raise InvalidELF("PT_DYNAMIC lacks DT_NULL terminator")
    require(5 in values and 10 in values and values[10] > 0, "missing dynamic string table metadata")
    strings = sections[s.link]
    require(strings.type == SHT_STRTAB and strings.flags & SHF_ALLOC,
            "SHT_DYNAMIC link is not an allocated string table")
    require((strings.addr, strings.size) == (values[5], values[10]),
            "dynamic string table disagrees with section headers")
    raw = data[strings.offset:strings.offset + strings.size]
    require(raw[0] == 0 and raw[-1] == 0, "invalid dynamic string table boundaries")
    return [cstring(raw, n, "DT_NEEDED") for n in needed]


def inspect_layout(segments, sections, result):
    loads = [p for p in segments if p.type == PT_LOAD]
    for p in loads:
        result.loads.append(dict(index=p.index, offset=p.offset, vaddr=p.vaddr,
                                 filesz=p.filesz, memsz=p.memsz, flags=p.flags,
                                 p_align=p.align, page_congruence=(p.vaddr - p.offset) % PAGE))
        if p.align < PAGE:
            result.issue("LOAD_ALIGNMENT", f"LOAD[{p.index}] p_align={p.align:#x} < {PAGE:#x}; "
                         "relink this library with a 16 KB-capable toolchain")
        if (p.align > 1 and (p.vaddr - p.offset) % p.align) or (p.vaddr - p.offset) % PAGE:
            result.issue("LOAD_CONGRUENCE", f"LOAD[{p.index}] vaddr={p.vaddr:#x}, offset={p.offset:#x} "
                         f"not congruent for p_align={p.align:#x} / 16 KB; rebuild the library")
        if p.flags & (PF_W | PF_X) == (PF_W | PF_X):
            result.issue("LOAD_WX", f"LOAD[{p.index}] is writable and executable; unsupported Android layout")
    for i, a in enumerate(loads):
        for b in loads[i + 1:]:
            if not a.memsz or not b.memsz:
                continue
            if overlaps(a.vaddr, a.end, b.vaddr, b.end):
                result.issue("LOAD_OVERLAP", f"LOAD[{a.index}] and LOAD[{b.index}] memory ranges overlap")
            elif overlaps(down(a.vaddr), up(a.end), down(b.vaddr), up(b.end)):
                if a.flags != b.flags or a.vaddr - a.offset != b.vaddr - b.offset:
                    result.issue("LOAD_PAGE_OVERLAP", f"LOAD[{a.index}] and LOAD[{b.index}] share a 16 KB page "
                                 "with conflicting permissions/file mappings; relink with 16 KB boundaries")
    relros = [p for p in segments if p.type == PT_GNU_RELRO]
    if not relros:
        result.notes.append("No GNU_RELRO present: no RELRO-rounding hazard assessed; absence is not a mitigation recommendation.")
    if len(relros) > 1:
        raise UnsupportedELF("multiple GNU_RELRO segments require manual review")
    for r in relros:
        require(r.memsz > 0, "empty GNU_RELRO segment")
        containers = [p for p in loads if p.flags & PF_W and p.vaddr <= r.vaddr < p.end
                      and r.end <= up(p.end)]
        require(len(containers) == 1, "GNU_RELRO not contained in one writable LOAD (including page padding)")
        p = containers[0]
        require(r.offset == p.offset + r.vaddr - p.vaddr
                and r.offset + r.filesz <= p.offset + p.filesz,
                "GNU_RELRO file mapping disagrees with containing LOAD")
        start, end = down(r.vaddr), up(r.end)
        evidence = dict(index=r.index, vaddr=r.vaddr, memsz=r.memsz, p_align=r.align,
                        end=r.end, end_residue=r.end % PAGE, rounded_start=start,
                        rounded_end=end, writable_overlaps=[])
        # Both fringes matter. Section names (including '.relro_padding') never
        # grant an exemption; only the declared ranges and flags are used.
        for s in sections:
            if not (s.flags & SHF_ALLOC and s.flags & SHF_WRITE and s.size):
                continue
            for lo, hi in ((start, r.vaddr), (r.end, end)):
                if lo < hi and overlaps(lo, hi, s.addr, s.end):
                    overlap = dict(section=s.name, start=max(lo, s.addr), end=min(hi, s.end))
                    evidence["writable_overlaps"].append(overlap)
                    result.issue("RELRO_WRITABLE_OVERLAP", f"GNU_RELRO[{r.index}] rounds to "
                                 f"[{start:#x}, {end:#x}) and protects writable {s.name!r} "
                                 f"[{overlap['start']:#x}, {overlap['end']:#x}) outside nominal RELRO; "
                                 "obtain a properly relinked vendor library or replace it")
        result.relro.append(evidence)


def audit_elf(data, name="<ELF>"):
    result = Result(name, hashlib.sha256(data).hexdigest())
    try:
        if len(data) > MAX_ELF_BYTES:
            raise UnsupportedELF("ELF exceeds 512 MiB audit limit")
        segments, sections = parse_elf(data, result)
        inspect_layout(segments, sections, result)
        result.needed = inspect_dynamic(data, segments, sections)
    except (InvalidELF, struct.error) as exc:
        result.issue("MALFORMED_ELF", str(exc), unknown=True)
    except UnsupportedELF as exc:
        result.issue("UNSUPPORTED_ELF", str(exc), unknown=True)
    return result


def inspect_zip_entry(raw, info):
    """Use the LOCAL header, not central extra length, to locate stored data."""
    raw.seek(info.header_offset)
    header = raw.read(30)
    if len(header) != 30 or header[:4] != b"PK\x03\x04":
        raise ValueError("missing/truncated ZIP local header")
    (_, version, flags, method, time, date, crc, compressed, uncompressed,
     name_length, extra_length) = struct.unpack("<4s5H3I2H", header)
    if method != info.compress_type or flags != info.flag_bits:
        raise ValueError("ZIP local/central compression or flags disagree")
    name = raw.read(name_length)
    expected = info.orig_filename.encode("utf-8" if flags & 0x800 else "cp437")
    if name != expected or "\0" in info.orig_filename:
        raise ValueError("ZIP local/central filename mismatch or NUL filename")
    if len(raw.read(extra_length)) != extra_length:
        raise ValueError("truncated ZIP local extra field")
    if flags & 1:
        raise ValueError("encrypted native ZIP entry unsupported")
    if method not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        raise ValueError(f"APK compression method {method} unsupported")
    if method == zipfile.ZIP_STORED and info.compress_size != info.file_size:
        raise ValueError("stored ZIP entry has inconsistent sizes")
    if not flags & 8:
        if crc != info.CRC:
            raise ValueError("ZIP local/central CRC mismatch")
        if compressed != 0xFFFFFFFF and compressed != info.compress_size:
            raise ValueError("ZIP local/central compressed size mismatch")
        if uncompressed != 0xFFFFFFFF and uncompressed != info.file_size:
            raise ValueError("ZIP local/central uncompressed size mismatch")
    offset = info.header_offset + 30 + name_length + extra_length
    return dict(compression="stored" if method == 0 else "deflated", data_offset=offset,
                data_offset_residue=offset % PAGE, mmap_alignment_required=method == 0)


def audit_apk(path):
    results = []
    try:
        with zipfile.ZipFile(path) as archive, path.open("rb") as raw:
            entries = [i for i in archive.infolist() if i.filename.startswith("lib/")
                       and i.filename.endswith(".so")]
            if not entries:
                r = Result(str(path))
                r.issue("NO_NATIVE_LIBRARIES", "no lib/<abi>/*.so found; ARM64 native-app profile not established", True)
                return [r]
            seen = set()
            for info in sorted(entries, key=lambda i: i.filename):
                r = Result(info.filename)
                try:
                    parts = info.filename.split("/")
                    if len(parts) != 3 or parts[1] != "arm64-v8a" or parts[2] in ("", ".", ".."):
                        raise UnsupportedELF("native entry outside lib/arm64-v8a/<name>.so; no ABI is silently skipped")
                    if info.filename in seen:
                        raise ValueError("duplicate native ZIP entry")
                    seen.add(info.filename)
                    packaging = inspect_zip_entry(raw, info)
                    if info.file_size > MAX_ELF_BYTES:
                        raise UnsupportedELF("native entry exceeds 512 MiB audit limit")
                    data = archive.read(info)
                    r = audit_elf(data, info.filename)
                    r.packaging = packaging
                    if packaging["mmap_alignment_required"] and packaging["data_offset_residue"]:
                        r.issue("APK_ALIGNMENT", f"stored library data offset={packaging['data_offset']:#x}, "
                                f"residue={packaging['data_offset_residue']:#x}; package on 16 KB boundaries "
                                "(AGP >=8.5.1); ZIP alignment does not repair ELF layouts")
                    elif not packaging["mmap_alignment_required"]:
                        r.notes.append("Compressed library: APK direct-mmap alignment is not applicable; ELF rules still apply. "
                                       "Manifest/extraction policy and installation are not validated.")
                except UnsupportedELF as exc:
                    r.issue("UNSUPPORTED_INPUT", str(exc), True)
                except (OSError, ValueError, RuntimeError, NotImplementedError, zipfile.BadZipFile,
                        EOFError, zlib.error) as exc:
                    r.issue("MALFORMED_ZIP_ENTRY", str(exc), True)
                results.append(r)
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        r = Result(str(path))
        r.issue("INVALID_APK", str(exc), True)
        results.append(r)
    return results


def audit_path(path):
    path = Path(path)
    if path.suffix.lower() == ".apk":
        return audit_apk(path)
    try:
        if path.stat().st_size > MAX_ELF_BYTES:
            raise ValueError("input exceeds 512 MiB native audit limit")
        return [audit_elf(path.read_bytes(), str(path))]
    except (OSError, ValueError) as exc:
        r = Result(str(path))
        r.issue("INPUT_ERROR", str(exc), True)
        return [r]


def print_result(result):
    print(f"[{result.status}] {result.name!r}")
    if result.sha256:
        print(f"  SHA256 {result.sha256}")
    z = result.packaging
    if z:
        requirement = "required" if z["mmap_alignment_required"] else "N/A (compressed/extracted)"
        print(f"  ZIP {z['compression']}: offset={z['data_offset']:#x}, "
              f"mod16K={z['data_offset_residue']:#x}; direct-mmap alignment {requirement}")
    for p in result.loads:
        print(f"  LOAD[{p['index']}] align={p['p_align']:#x} off={p['offset']:#x} "
              f"va={p['vaddr']:#x} filesz={p['filesz']:#x} memsz={p['memsz']:#x} "
              f"flags={p['flags']:#x} congruence16K={p['page_congruence']:#x}")
    for r in result.relro:
        verdict = "WRITABLE OVERLAP" if r["writable_overlaps"] else "no writable section in rounded fringes"
        print(f"  RELRO[{r['index']}] {r['vaddr']:#x}+{r['memsz']:#x}={r['end']:#x} "
              f"mod16K={r['end_residue']:#x} p_align={r['p_align']:#x}; "
              f"protect=[{r['rounded_start']:#x},{r['rounded_end']:#x}): {verdict}")
    if result.needed:
        print("  NEEDED " + ", ".join(repr(n) for n in result.needed))
    for issue in result.issues:
        print(f"  {issue['code']}: {issue['message']}")
    for note in result.notes:
        print(f"  NOTE: {note}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", help="APK files or standalone native ELF libraries; never modified/extracted")
    parser.add_argument("--profile", choices=[PROFILE], default=PROFILE)
    parser.add_argument("--json", action="store_true", help="emit machine-readable static evidence to stdout")
    args = parser.parse_args(argv)
    inputs = [{"path": str(path), "results": audit_path(path)} for path in args.paths]
    results = [r for item in inputs for r in item["results"]]
    counts = {status: sum(r.status == status for r in results) for status in ("PASS", "FAIL", "UNKNOWN")}
    passed = bool(results) and all(r.status == "PASS" for r in results)
    scope = "Static ELF/ZIP layout only. Runtime/device, loader dependencies, APK signatures and manifest policy are NOT validated."
    if args.json:
        print(json.dumps(dict(schema_version=1, profile=PROFILE, page_size=PAGE,
                              static_layout_pass=passed, runtime_validation="NOT_PERFORMED",
                              scope=scope, counts=counts,
                              inputs=[dict(path=i["path"], results=[r.to_dict() for r in i["results"]])
                                      for i in inputs]), indent=2))
    else:
        print(f"16 KB static layout audit ({PROFILE}, page_size={PAGE})")
        for item in inputs:
            print(f"\nInput: {item['path']}")
            for result in item["results"]:
                print_result(result)
        print(f"\nSTATIC {'PASS' if passed else 'NOT VALIDATED'}: "
              f"{counts['PASS']} pass, {counts['FAIL']} fail, {counts['UNKNOWN']} unknown")
        print(scope)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
