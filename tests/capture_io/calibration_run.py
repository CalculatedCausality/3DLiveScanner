#!/usr/bin/env python3
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="scanner-calibration-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++11", "-O2", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(ROOT / "common"), "-I" + str(ROOT / "third_party/tango_3d_reconstruction/include"),
        str(ROOT / "tests/capture_io/calibration_test.cc"), "-o", str(binary)], check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=15,
                   env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"))
