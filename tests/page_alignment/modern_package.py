#!/usr/bin/env python3
"""Native dependency isolation regressions, separate from the ELF-layout fixtures."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from check_modern_apk import dependency_issues


def item(name, *needed):
    return SimpleNamespace(name="lib/arm64-v8a/" + name, needed=list(needed))


def modern():
    return [item("lib3dscanner.so", "libarcore_sdk_c.so", "libc++_shared.so", "liblog.so"),
            item("libarcore_sdk_c.so", "libc.so"), item("libarcore_sdk_jni.so", "libarcore_sdk_c.so"),
            item("libc++_shared.so", "libc.so", "libm.so", "libdl.so")]


class ModernPackageTests(unittest.TestCase):
    def test_owned_backend_has_no_legacy_dependency(self):
        self.assertEqual(dependency_issues(modern()), [])

    def test_legacy_libraries_fail_even_if_layout_would_pass(self):
        for name in ("libtango_3d_reconstruction.so", "libhuawei_arengine_ndk.so", "libgvr.so", "libgvr_audio.so"):
            self.assertTrue(dependency_issues(modern() + [item(name)]))

    def test_legacy_needed_edge_without_packaged_binary_fails(self):
        libraries = modern()
        libraries[0].needed.append("libtango_3d_reconstruction.so")
        self.assertTrue(dependency_issues(libraries))

    def test_missing_runtime_fails(self):
        self.assertTrue(dependency_issues(modern()[:-1]))

    def test_missing_scanner_or_arcore_fails(self):
        for index in range(3):
            libraries = modern()
            del libraries[index]
            self.assertTrue(dependency_issues(libraries))

    def test_absolute_and_private_dependency_paths_fail(self):
        for name in ("/tmp/libexample.so", "libart.so", "libunknown.so"):
            libraries = modern()
            libraries[0].needed.append(name)
            self.assertTrue(dependency_issues(libraries))


if __name__ == "__main__":
    unittest.main()
