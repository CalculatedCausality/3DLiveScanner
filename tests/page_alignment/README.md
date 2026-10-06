# Strict 16 KB native layout audit

`tools/check_apk_16kb.py` is a **read-only, standard-library Python 3.8+** gate
for this app's Android ARM64 native libraries. It neither extracts APK entries
to disk nor invokes an SDK, compiler, ELF editor, Gradle or a connected device.

## Usage

From the repository root:

```sh
python3 -B tools/check_apk_16kb.py path/to/app.apk
python3 -B tools/check_apk_16kb.py --json path/to/app.apk
python3 -B tools/check_apk_16kb.py path/to/lib3dscanner.so path/to/libc++_shared.so
python3 -B tests/page_alignment/run.py
```

The default and only supported `--profile` is `android-arm64`: little-endian
ELF64, AArch64 (`e_machine=183`), `ET_DYN`, `e_flags=0`, Android-compatible OS ABI.
An APK is recognized by its `.apk` suffix. Every `lib/**/*.so` entry is considered;
other ABI directories are **UNKNOWN**, not silently skipped. An APK with no
native entries also fails this native-app profile rather than getting an empty pass.
Standalone ELF input does not establish APK packaging alignment.

Exit status:

- **0:** every library passes the supported **static layout** rules.
- **1:** at least one incompatible, malformed, unsupported, missing or unreadable input.
- **2:** invalid CLI arguments/profile.

Per-library `PASS`, `FAIL`, or `UNKNOWN` output contains the ELF SHA-256, each
LOAD's `p_align`, offsets/virtual addresses, sizes, flags and page congruence,
RELRO end/residue and rounded protection range, writable-section overlaps,
`DT_NEEDED`, ZIP data offset/compression, and actionable diagnostic codes.
Known incompatibilities take status precedence over UNKNOWN findings; **neither
can make the gate succeed**. A nonzero RELRO residue alone is never a failure.

JSON schema version 1 returns `profile`, `page_size`, `static_layout_pass`,
`runtime_validation: "NOT_PERFORMED"`, counts and per-input/per-library evidence.
Numeric fields are integers (human output uses hexadecimal); issue objects have
`code`, `message`, and `unknown`. Output goes to stdout, so callers may retain it
as build evidence. The tool does not write a report itself.

## Exact checks

1. **Parse and cross-check actual ELF headers.** Validate ELF identification,
   header/table sizes and ranges, overflow, program-header alignment values,
   `p_filesz <= p_memsz`, readable mapping of the ELF/program headers, LOAD order,
   section-name strings, section links/alignment/ranges, nonoverlapping allocated
   sections, and correspondence between allocated sections and LOAD file/memory
   ranges. Require section headers and allocated sections; incomplete evidence
   is not a pass. Validate `PT_DYNAMIC`/`SHT_DYNAMIC` consistency, termination,
   string-table mapping and each reported `DT_NEEDED` string.
2. **LOAD:** every segment must declare power-of-two `p_align >= 0x4000`.
   Require `p_vaddr % p_align == p_offset % p_align` and the same congruence
   modulo `0x4000`. Reject writable+executable LOADs and overlapping LOAD memory
   ranges. Reject 16 KB page sharing with conflicting permissions or different
   file-to-virtual-address mappings. A 64 KB-aligned LOAD is acceptable; a 4 KB
   declaration fails even when its particular addresses happen to align to 16 KB.
3. **GNU_RELRO:** record `end = p_vaddr + p_memsz`, `end % 0x4000`, and the
   protection interval `[floor16K(start), ceil16K(end))`. Cross-check its file
   mapping and containing writable LOAD, allowing page-end padding. Intersect
   **both rounded fringes outside nominal RELRO** with nonempty
   `SHF_ALLOC | SHF_WRITE` sections, including `SHT_NOBITS` (`.bss`). Any intersected
   writable bytes fail, including a partly covered section. No section name is
   exempt. Normal relocation sections *inside* nominal RELRO are expected and
   do not fail just because their ELF flags include write permission.
4. **ZIP:** locate data from the **local** ZIP header's filename/extra lengths;
   validate local/central consistency, compression method, sizes/CRC, duplicate
   native entries, and read/decompress through `zipfile` for integrity checking.
   A stored (uncompressed) library must have `data_offset % 0x4000 == 0`.
   Deflated libraries have **no APK direct-mmap alignment requirement**; their
   decompressed ELF is audited identically. Compression does not fix ELF defects.
   Compressed-library installation/extraction policy is explicitly not validated.

### Why padding matters

- Tango-like failure: nominal RELRO ends at `0x11000`; writable `.data`/`.bss`
  start there; 16 KB protection extends to `0x14000`. This fails even when every
  LOAD declares 64 KB alignment.
- ARCore-like safe layout: a RELRO-only LOAD ends at `0x11000`; its next writable
  LOAD starts at `0x14200`. Protection rounds to `0x14000`, touching no writable
  section outside nominal RELRO. The residue `0x1000` is evidence, **not a defect**.
- A zero end residue is insufficient: rounding the *start* down can also protect
  writable bytes before the declared RELRO region.

These rules follow the normal page-rounded protection performed by
[Bionic `_phdr_table_set_gnu_relro_prot`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/linker/linker_phdr.cpp).
[Android's page-size guidance](https://developer.android.com/guide/practices/page-sizes)
also covers LOAD alignment, ZIP packaging and runtime testing. This gate checks
actual writable overlap instead of using the guide's simplified RELRO-residue
heuristic as an unconditional rejection.

## Fixtures and verification

`fixtures.py` generates small, synthetic ELF/ZIP bytes in memory. It never patches
vendor binaries. `run.py` uses `unittest`, temporary files, and subprocesses to
check CLI exit codes and JSON as well as parser/layout results. No binary fixtures
or SDK downloads are needed.

Coverage includes 4 KB LOAD failure, 16/64 KB success, declared-alignment
congruence, conflicting LOAD pages, 64 KB LOAD with RELRO overlap, `.bss` and
partial-section overlap, RELRO start rounding, ARCore-style safe padding,
aligned/misaligned stored ZIPs, deflated good/bad ELFs, local versus central ZIP
padding, duplicate entries, CRC/header corruption, malformed ELF tables/strings,
truncated data, unsupported ABI/machine/class/encoding/TLS and missing inputs.

### Fingerprint-locked historical baseline

```sh
python3 -B tools/check_apk_16kb.py artifacts/3DLiveScanner-capture-io-debug-2026-09-30.apk
# MUST exit 1 (known incompatible).
python3 -B tests/page_alignment/baseline.py
# Or supply that same artifact at another path:
python3 -B tests/page_alignment/baseline.py /path/to/original.apk
```

The separate baseline regression returns 0 **only when expected failures are
reproduced**. It does not invert the audit verdict or approve the APK. It checks:

```text
SHA256 3f7685a3a9a49f262c93bdf13ae32fadb690c32a7895dc0acec056799c2230a0
9 arm64 libraries; audit exit 1; 7 FAIL / 2 PASS / 0 UNKNOWN
4 libraries with 4 KB LOAD alignment
7 libraries with unaligned stored ZIP offsets
3 libraries with writable RELRO fringe overlap (ARCore C, Huawei JNI, Tango)
```

The two GVR libraries pass static layout. Huawei NDK passes ELF layout but fails
the baseline APK's ZIP alignment. Fingerprints, exact residues, diagnostic sets,
and scanner's Tango `DT_NEEDED` dependency are asserted by `baseline.py`.
The original APK is not committed as a fixture or required for unit tests.
Run the normal audit directly on new APKs; the historical baseline test deliberately
rejects any different fingerprint.

## Boundaries and fail-closed states

This is a layout validator, not a complete ELF loader or a runtime compatibility
certificate. It trusts internally consistent section metadata to identify actual
allocated contents and padding; it does not disassemble code, prove metadata
provenance, or infer runtime writes into unnamed gaps.

- Missing sections, extended ELF numbering, native TLS sections, multiple RELRO
  segments, unsupported machines/profiles and native files over 512 MiB return
  UNKNOWN/nonzero. No override promotes these to PASS.
- No RELRO is reported explicitly. It creates no RELRO-rounding hazard by itself,
  but does not establish security hardening; removing RELRO is not recommended.
- No dependency/symbol/API resolution, Java/JNI integration, capture correctness,
  native allocator/page-size assumptions, `dlopen` assets or downloaded libraries
  are validated. Only conventional `lib/**/*.so` APK entries are inventoried.
- No APK signature, binary manifest, extraction policy, AAB-to-APK delivery or
  installation checks are performed. Audit each delivered APK/split separately;
  a native-free split returns UNKNOWN under this app-specific profile.
- A passing static report still requires real **16 KB** runtime/device testing
  without compatibility workarounds. Running on a 4 KB kernel is not that test.
- The tool changes no ELF headers, permissions, RELRO protections, packaging,
  warning settings or compatibility flags. Device/ADB validation belongs to main.
