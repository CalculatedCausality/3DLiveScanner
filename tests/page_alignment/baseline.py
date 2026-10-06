#!/usr/bin/env python3
"""Optional integration regression: the fingerprinted original APK MUST fail.

Exit 0 here means expected baseline failures were reproduced, NOT that the APK
is compatible. The production audit CLI must return 1 for this artifact.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SHA256 = "3f7685a3a9a49f262c93bdf13ae32fadb690c32a7895dc0acec056799c2230a0"
# name: LOAD alignment, RELRO end residue, ZIP offset residue
EXPECTED = {
    "lib3dscanner.so": (0x1000, 0, 0x1000),
    "libarcore_sdk_c.so": (0x1000, 0x3000, 0x2000),
    "libarcore_sdk_jni.so": (0x1000, 0, 0x1000),
    "libc++_shared.so": (0x1000, 0, 0x1000),
    "libgvr.so": (0x10000, 0, 0),
    "libgvr_audio.so": (0x10000, 0, 0),
    "libhuawei_arengine_jni.so": (0x10000, 0x3000, 0x1000),
    "libhuawei_arengine_ndk.so": (0x10000, 0, 0x1000),
    "libtango_3d_reconstruction.so": (0x10000, 0x1000, 0x2000),
}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", nargs="?", type=Path,
                        default=ROOT / "artifacts/3DLiveScanner-capture-io-debug-2026-09-30.apk")
    args = parser.parse_args()
    try:
        check(hashlib.sha256(args.apk.read_bytes()).hexdigest() == SHA256,
              "APK fingerprint differs; do not treat a new build as the historical baseline")
        proc = subprocess.run([sys.executable, "-B", str(ROOT / "tools/check_apk_16kb.py"),
                               "--json", str(args.apk)], capture_output=True, text=True, timeout=60)
        check(proc.returncode == 1, f"baseline audit must return 1, got {proc.returncode}: {proc.stderr}")
        report = json.loads(proc.stdout)
        check(report["static_layout_pass"] is False, "baseline unexpectedly passed")
        check(report["counts"] == {"PASS": 2, "FAIL": 7, "UNKNOWN": 0}, "unexpected per-library verdicts")
        results = {Path(r["name"]).name: r for r in report["inputs"][0]["results"]}
        check(set(results) == set(EXPECTED), "baseline library inventory changed")
        for name, (alignment, relro_residue, zip_residue) in EXPECTED.items():
            r = results[name]
            check({p["p_align"] for p in r["loads"]} == {alignment}, f"{name}: LOAD alignment mismatch")
            check([p["end_residue"] for p in r["relro"]] == [relro_residue], f"{name}: RELRO residue mismatch")
            check(r["packaging"]["data_offset_residue"] == zip_residue, f"{name}: ZIP residue mismatch")
            codes = {i["code"] for i in r["issues"]}
            check(("APK_ALIGNMENT" in codes) == bool(zip_residue), f"{name}: ZIP diagnosis mismatch")
            check(("LOAD_ALIGNMENT" in codes) == (alignment < 16384), f"{name}: LOAD diagnosis mismatch")
            overlap_expected = name in {"libarcore_sdk_c.so", "libhuawei_arengine_jni.so",
                                        "libtango_3d_reconstruction.so"}
            check(("RELRO_WRITABLE_OVERLAP" in codes) == overlap_expected, f"{name}: RELRO diagnosis mismatch")
            print(f"{name}: {r['status']}; LOAD={alignment:#x}, RELRO residue={relro_residue:#x}, "
                  f"ZIP residue={zip_residue:#x}; {', '.join(sorted(codes)) or 'static layout only'}")
        check("libtango_3d_reconstruction.so" in results["lib3dscanner.so"]["needed"],
              "baseline Tango DT_NEEDED missing")
        print("Known-failing baseline reproduced: audit exit 1; 7 FAIL / 2 PASS / 0 UNKNOWN. "
              "This regression check is not a compatibility approval; runtime validation NOT PERFORMED.")
        return 0
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
        print(f"Baseline verification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
