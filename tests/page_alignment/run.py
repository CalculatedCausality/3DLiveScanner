#!/usr/bin/env python3
"""Run deterministic stdlib-only layout, malformed-input and CLI tests."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

from fixtures import apk_fixture, change_field, elf_fixture, section_offset

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/check_apk_16kb.py"
spec = importlib.util.spec_from_file_location("check_apk_16kb", TOOL)
assert spec is not None and spec.loader is not None
audit = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = audit
spec.loader.exec_module(audit)


class ElfTests(unittest.TestCase):
    def codes(self, result):
        return {issue["code"] for issue in result.issues}

    def test_16k_aligned(self):
        result = audit.audit_elf(elf_fixture())
        self.assertEqual(result.status, "PASS", result.issues)
        self.assertEqual(result.needed, ["libc.so"])
        self.assertEqual(result.relro[0]["end_residue"], 0)

    def test_4k_load_fails_even_when_addresses_are_16k_aligned(self):
        result = audit.audit_elf(elf_fixture(align=0x1000))
        self.assertEqual(result.status, "FAIL")
        self.assertIn("LOAD_ALIGNMENT", self.codes(result))

    def test_64k_load_safe(self):
        result = audit.audit_elf(elf_fixture(align=0x10000))
        self.assertEqual(result.status, "PASS", result.issues)

    def test_64k_load_does_not_rescue_relro_overlap(self):
        result = audit.audit_elf(elf_fixture("overlap", align=0x10000))
        self.assertEqual(result.status, "FAIL", result.issues)
        self.assertEqual(self.codes(result), {"RELRO_WRITABLE_OVERLAP"})
        self.assertEqual(result.relro[0]["end_residue"], 0x1000)
        self.assertEqual({o["section"] for o in result.relro[0]["writable_overlaps"]}, {".data", ".bss"})

    def test_arcore_like_gap_is_safe_despite_nonzero_residue(self):
        result = audit.audit_elf(elf_fixture("gap"))
        self.assertEqual(result.status, "PASS", result.issues)
        self.assertEqual(result.relro[0]["end_residue"], 0x1000)
        self.assertEqual(result.relro[0]["rounded_end"], 0x14000)
        self.assertEqual(result.relro[0]["writable_overlaps"], [])

    def test_relro_prefix_overlap_fails_even_with_aligned_end(self):
        result = audit.audit_elf(elf_fixture("prefix"))
        self.assertEqual(result.status, "FAIL", result.issues)
        self.assertEqual(result.relro[0]["end_residue"], 0)
        self.assertIn("RELRO_WRITABLE_OVERLAP", self.codes(result))

    def test_partial_writable_section_at_relro_end_fails(self):
        data = elf_fixture("overlap")
        # Extend nominal RELRO 16 bytes into .data; its remaining 16 still matter.
        data = change_field(data, 64 + 3 * 56 + 40, "<Q", 0x1010)
        result = audit.audit_elf(data)
        self.assertEqual(result.status, "FAIL", result.issues)
        self.assertEqual(result.relro[0]["writable_overlaps"][0]["start"], 0x11010)

    def test_missing_relro_is_reported_not_recommended(self):
        result = audit.audit_elf(elf_fixture(relro=False))
        self.assertEqual(result.status, "PASS", result.issues)
        self.assertTrue(any("No GNU_RELRO" in note for note in result.notes))

    def test_load_congruence_checks_declared_alignment_too(self):
        # Gap delta 0x4000 satisfies 16K, not a claimed 64K alignment.
        result = audit.audit_elf(elf_fixture("gap", align=0x10000))
        self.assertEqual(result.status, "FAIL", result.issues)
        self.assertIn("LOAD_CONGRUENCE", self.codes(result))

    def test_load_page_permission_collision_fails(self):
        data = elf_fixture()
        # Fill RX up to RW start; then shift RW segment+sections by one 4K page
        # but keep file offsets in sync, leaving conflicting mappings in one page.
        data = change_field(data, 64 + 32, "<Q", 0xF000)
        data = change_field(data, 64 + 40, "<Q", 0xF000)
        for i in (1, 2, 3):
            va = struct.unpack_from("<Q", data, 64 + i * 56 + 16)[0]
            data = change_field(data, 64 + i * 56 + 16, "<Q", va - 0x1000)
        for i in (3, 4, 5, 6):
            so = section_offset(data, i)
            va = struct.unpack_from("<Q", data, so + 16)[0]
            data = change_field(data, so + 16, "<Q", va - 0x1000)
        result = audit.audit_elf(data)
        self.assertIn("LOAD_PAGE_OVERLAP", self.codes(result))

    def test_malformed_headers_fail_closed(self):
        good = elf_fixture()
        cases = {
            "magic": b"not ELF",
            "short_header": good[:48],
            "truncated_tables": good[:-1],
            "phoff_outside": change_field(good, 32, "<Q", 1 << 60),
            "phentsize": change_field(good, 54, "<H", 1),
            "shentsize": change_field(good, 58, "<H", 1),
            "shstr_index": change_field(good, 62, "<H", 400),
            "filesz_too_big": change_field(good, 64 + 32, "<Q", 1 << 60),
            "memsz_smaller": change_field(good, 64 + 40, "<Q", 8),
            "bad_align": change_field(good, 64 + 48, "<Q", 0x5000),
            "virtual_overflow": change_field(good, 64 + 16, "<Q", (1 << 64) - 8),
            "bad_section_name": change_field(good, section_offset(good, 1), "<I", 99999),
            "bad_section_link": change_field(good, section_offset(good, 4) + 40, "<I", 99999),
            "bad_section_mapping": change_field(good, section_offset(good, 5) + 24, "<Q", 0x100),
            "bad_needed": change_field(good, 0x10108, "<Q", 99999),
            "missing_dynamic_null": change_field(good, 0x10130, "<q", 42),
        }
        for name, data in cases.items():
            with self.subTest(name=name):
                result = audit.audit_elf(data)
                self.assertNotEqual(result.status, "PASS")
                self.assertIn("MALFORMED_ELF", self.codes(result))

    def test_unsupported_profiles_fail_closed(self):
        good = elf_fixture()
        cases = [change_field(good, 18, "<H", 62), change_field(good, 4, "<B", 1),
                 change_field(good, 5, "<B", 2), change_field(good, 16, "<H", 2),
                 change_field(good, 40, "<Q", 0), change_field(good, 56, "<H", 0xFFFF),
                 change_field(good, section_offset(good, 6) + 8, "<Q", 0x403)]
        for data in cases:
            with self.subTest(header=data[:64]):
                result = audit.audit_elf(data)
                self.assertEqual(result.status, "UNKNOWN", result.issues)
                self.assertIn("UNSUPPORTED_ELF", self.codes(result))

    def test_truncation_and_random_header_noise_never_throw_or_pass(self):
        data = elf_fixture()
        rng = random.Random(16000)
        for length in [0, 1, 15, 16, 63, 64, 100, 4096, len(data) - 1]:
            self.assertNotEqual(audit.audit_elf(data[:length]).status, "PASS")
        for _ in range(64):
            self.assertNotEqual(audit.audit_elf(bytes(rng.getrandbits(8) for _ in range(128))).status, "PASS")


class ApkAndCliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="scanner-page-alignment-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "fixture.apk"

    def check(self, data):
        self.path.write_bytes(data)
        before = hashlib.sha256(data).digest()
        results = audit.audit_path(self.path)
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).digest(), before)
        return results

    def test_stored_aligned_pass(self):
        result, = self.check(apk_fixture(elf_fixture()))
        self.assertEqual(result.status, "PASS", result.issues)
        self.assertEqual(result.packaging["data_offset"], 0x4000)

    def test_misaligned_zip_fails_independently_of_good_elf(self):
        result, = self.check(apk_fixture(elf_fixture(), aligned=False))
        self.assertEqual(result.status, "FAIL")
        self.assertEqual([i["code"] for i in result.issues], ["APK_ALIGNMENT"])

    def test_compressed_misaligned_has_no_zip_alignment_requirement(self):
        result, = self.check(apk_fixture(elf_fixture(), aligned=False, compressed=True))
        self.assertEqual(result.status, "PASS", result.issues)
        self.assertFalse(result.packaging["mmap_alignment_required"])
        self.assertNotEqual(result.packaging["data_offset_residue"], 0)
        self.assertTrue(any("not applicable" in n for n in result.notes))

    def test_compression_does_not_rescue_bad_elf(self):
        result, = self.check(apk_fixture(elf_fixture(align=0x1000), compressed=True))
        self.assertEqual(result.status, "FAIL")
        self.assertTrue(any(i["code"] == "LOAD_ALIGNMENT" for i in result.issues))
        self.assertFalse(any(i["code"] == "APK_ALIGNMENT" for i in result.issues))

    def test_other_abi_and_machine_are_not_silently_skipped(self):
        result, = self.check(apk_fixture(elf_fixture(), abi="x86_64"))
        self.assertEqual(result.status, "UNKNOWN")
        wrong_machine = change_field(elf_fixture(), 18, "<H", 62)
        result, = self.check(apk_fixture(wrong_machine))
        self.assertEqual(result.status, "UNKNOWN")

    def test_bad_zip_and_empty_archive_fail_closed(self):
        result, = self.check(b"bad archive")
        self.assertEqual(result.status, "UNKNOWN")
        memory = io.BytesIO()
        with zipfile.ZipFile(memory, "w"):
            pass
        result, = self.check(memory.getvalue())
        self.assertEqual(result.status, "UNKNOWN")

    def test_crc_and_local_header_corruption_fail_closed(self):
        data = apk_fixture(elf_fixture())
        for changed in [change_field(data, 0, "<I", 0), change_field(data, 0x4500, "<B", 0xFF),
                        change_field(data, 8, "<H", 8), change_field(data, 28, "<H", 0)]:
            with self.subTest(prefix=changed[:30]):
                result, = self.check(changed)
                self.assertNotEqual(result.status, "PASS")

    def test_local_padding_is_not_inferred_from_central_extra(self):
        data = apk_fixture(elf_fixture())
        # Valid ZIP: local padding retained, central extra removed. The central
        # directory offset remains the same; only its size/entry length changes.
        central = data.index(b"PK\x01\x02")
        name_len, extra_len = struct.unpack_from("<HH", data, central + 28)
        extra_start = central + 46 + name_len
        data = data[:extra_start] + data[extra_start + extra_len:]
        data = change_field(data, central + 30, "<H", 0)
        end = data.rindex(b"PK\x05\x06")
        directory_size = struct.unpack_from("<I", data, end + 12)[0]
        data = change_field(data, end + 12, "<I", directory_size - extra_len)
        result, = self.check(data)
        self.assertEqual(result.status, "PASS", result.issues)
        self.assertEqual(result.packaging["data_offset"], 0x4000)

    def test_missing_input_is_unknown_not_pass(self):
        result, = audit.audit_path(self.path)
        self.assertEqual(result.status, "UNKNOWN")

    def test_standalone_elf_cli_does_not_claim_apk_validation(self):
        path = self.path.with_suffix(".so")
        path.write_bytes(elf_fixture("gap"))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = audit.main(["--json", str(path)])
        self.assertEqual(status, 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["inputs"][0]["results"][0]["packaging"], {})
        self.assertEqual(report["runtime_validation"], "NOT_PERFORMED")

    def test_duplicate_library_fails_closed(self):
        memory = io.BytesIO()
        import warnings
        with warnings.catch_warnings(), zipfile.ZipFile(memory, "w") as archive:
            warnings.simplefilter("ignore", UserWarning)
            for _ in range(2):
                archive.writestr("lib/arm64-v8a/libfixture.so", elf_fixture())
        results = self.check(memory.getvalue())
        self.assertTrue(any(i["code"] == "MALFORMED_ZIP_ENTRY" for r in results for i in r.issues))

    def test_cli_json_exit_codes_and_runtime_boundary(self):
        for specimen, expected in [(apk_fixture(elf_fixture()), 0),
                                   (apk_fixture(elf_fixture(), aligned=False), 1), (b"invalid", 1)]:
            self.path.write_bytes(specimen)
            proc = subprocess.run([sys.executable, "-B", str(TOOL), "--json", str(self.path)],
                                  capture_output=True, text=True, timeout=15)
            self.assertEqual(proc.returncode, expected, proc.stderr)
            report = json.loads(proc.stdout)
            self.assertEqual(report["static_layout_pass"], expected == 0)
            self.assertEqual(report["runtime_validation"], "NOT_PERFORMED")
        proc = subprocess.run([sys.executable, "-B", str(TOOL), "--profile", "unknown", str(self.path)],
                              capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 2)

    def test_human_output_is_actionable(self):
        self.path.write_bytes(apk_fixture(elf_fixture("overlap"), aligned=False))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = audit.main([str(self.path)])
        self.assertEqual(status, 1)
        for text in ["p_align=", "RELRO_WRITABLE_OVERLAP", "APK_ALIGNMENT", "NEEDED", "NOT validated"]:
            self.assertIn(text, output.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
