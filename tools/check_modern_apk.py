#!/usr/bin/env python3
"""Modern scanner package gate: static 16 KiB layout plus native dependency isolation."""
import argparse
from pathlib import Path
import sys

from check_apk_16kb import audit_path, print_result

# Public native platform libraries available by this app's minSdk 24.
PLATFORM = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
    "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
    "libOpenSLES.so", "libmediandk.so", "libjnigraphics.so", "libvulkan.so",
    "libcamera2ndk.so", "libstdc++.so",
}
REQUIRED = {"lib3dscanner.so", "libarcore_sdk_c.so", "libarcore_sdk_jni.so"}


def legacy(name):
    return name.startswith(("libtango_", "libhuawei_", "libgvr"))


def dependency_issues(results):
    issues = []
    names = {Path(result.name).name for result in results}
    for missing in sorted(REQUIRED - names):
        issues.append("Missing required modern library: " + missing)
    for result in results:
        name = Path(result.name).name
        if legacy(name):
            issues.append("Legacy library is packaged: " + name)
        for needed in result.needed:
            if legacy(needed):
                issues.append(name + " still requires legacy library " + needed)
            elif needed not in names and needed not in PLATFORM:
                issues.append(name + " has unresolved/non-public native dependency " + needed)
    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    args = parser.parse_args()
    if args.apk.suffix.lower() != ".apk":
        parser.error("The modern package gate requires a complete APK")
    results = audit_path(args.apk)
    for result in results:
        print_result(result)
    problems = dependency_issues(results)
    for problem in problems:
        print("PACKAGE: " + problem)
    passed = bool(results) and all(result.status == "PASS" for result in results) and not problems
    print("MODERN STATIC PACKAGE " + ("PASS" if passed else "NOT VALIDATED"))
    print("Signature, Java feature gating, dynamic dlopen and device behavior still require separate validation.")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
