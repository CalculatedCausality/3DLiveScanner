#!/usr/bin/env python3
"""Pixel-equivalence checks and host-only YUV timings against an optional same-session baseline."""
import argparse
import os
from pathlib import Path
import shlex
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "common/data/image.cc"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--snapshot", type=Path, help="Save the current source before editing; refuses overwrite")
parser.add_argument("--baseline", type=Path)
parser.add_argument("--modern", action="store_true", help="Check standard RGBA/NV21 owned-backend convention")
args = parser.parse_args()
if args.modern and args.baseline:
    parser.error("Modern colour correctness is not a byte-identical legacy benchmark")
if args.snapshot:
    with args.snapshot.open("xb") as output:
        output.write(SOURCE.read_bytes())
    print("Saved unchanged baseline:", args.snapshot)
    raise SystemExit(0)

cxx = shlex.split(os.environ.get("CXX", "c++"))
includes = [ROOT / "tests/performance/native/include", ROOT / "common", ROOT / "third_party/glm",
            ROOT / "third_party/libpng/include", ROOT / "third_party/libjpeg-turbo/src"]
flags = ["-std=c++11", "-ffunction-sections", "-fdata-sections", "-DANDROID", "-include",
         str(ROOT / "tests/performance/native/include/host.h")]
if args.modern:
    flags += ["-DSCANNER_MODERN=1"]
for path in includes:
    flags += ["-I", str(path)]
with tempfile.TemporaryDirectory(prefix="scanner-capture-image-") as temporary:
    work = Path(temporary)
    measurements = {}
    for name, source, sanitizer in [("check", SOURCE, True), ("current", SOURCE, False)] + (
            [("baseline", args.baseline, False)] if args.baseline else []):
        executable = work / name
        mode = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitizer else ["-O2"]
        subprocess.run(cxx + flags + mode + [str(source),
            str(ROOT / "tests/performance/native/codec_init.cc"),
            str(ROOT / "tests/capture_speed/image_yuv_test.cc"), "-Wl,--gc-sections", "-o", str(executable)],
            check=True, timeout=180)
        result = subprocess.run([str(executable)] + ([] if sanitizer else ["bench", name]),
                                check=True, timeout=90, capture_output=True, text=True,
                                env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"))
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="")
        measurements[name] = {match[0]: (float(match[1]), match[2], match[3]) for match in
            re.findall(r"YUV \w+ (\d+x\d+/\d+) median_us=(\S+) hash=(\d+) sample_sum=(\d+)", result.stdout)}
    if args.baseline:
        assert measurements["current"].keys() == measurements["baseline"].keys()
        for case, current in measurements["current"].items():
            baseline = measurements["baseline"][case]
            assert current[1:] == baseline[1:], "Output changed: " + case
            print(f"HOST ONLY {case}: {baseline[0] / current[0]:.2f}x conversion speed; identical output hash")
