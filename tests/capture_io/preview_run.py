#!/usr/bin/env python3
"""Preview serialization/flush regressions and real host write-syscall counts."""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "common/data/dataset.cc"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--snapshot", type=Path)
parser.add_argument("--baseline", type=Path)
args = parser.parse_args()
if args.snapshot:
    with args.snapshot.open("xb") as output:
        output.write(SOURCE.read_bytes())
    print("Saved unchanged baseline:", args.snapshot)
    raise SystemExit(0)

compiler = shlex.split(os.environ.get("CXX", "c++"))
flags = ["-std=c++11", "-O2", "-ffunction-sections", "-fdata-sections"]
for include in ("tests/geometry/include", "common", "third_party/glm", "third_party/tango_3d_reconstruction/include"):
    flags += ["-I", str(ROOT / include)]
with tempfile.TemporaryDirectory(prefix="scanner-preview-io-") as temporary:
    work = Path(temporary)
    counts = {}
    for name, source in [("current", SOURCE)] + ([("baseline", args.baseline)] if args.baseline else []):
        binary = work / name
        subprocess.run(compiler + flags + [str(source), str(ROOT / "tests/capture_io/preview_test.cc"),
                       "-Wl,--gc-sections", "-o", str(binary)], check=True, timeout=180)
        fixture = work / (name + "-data")
        fixture.mkdir()
        subprocess.run([str(binary), str(fixture)], check=True, timeout=30)
        if shutil.which("strace"):
            trace = work / (name + ".trace")
            subprocess.run(["strace", "-qq", "-e", "trace=write,fsync", "-o", str(trace),
                            str(binary), str(fixture), "once"], check=True, timeout=30)
            text = trace.read_text()
            writes = sum(int(fd) > 2 for fd in re.findall(r"^write\((\d+),", text, re.M))
            syncs = len(re.findall(r"^fsync\(", text, re.M))
            counts[name] = writes
            assert syncs == 1, "Preview durability barrier changed"
            print(f"{name}: {writes} file write syscalls, {syncs} fsync for the same preview")
    if len(counts) == 2:
        assert counts["current"] < counts["baseline"], counts
        print("PASS: byte-compatible previews with fewer host writes and the same fsync barrier")
    sanitized = work / "sanitized"
    subprocess.run(compiler + flags + ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   str(SOURCE), str(ROOT / "tests/capture_io/preview_test.cc"), "-Wl,--gc-sections", "-o", str(sanitized)],
                   check=True, timeout=180)
    fixture = work / "sanitized-data"
    fixture.mkdir()
    subprocess.run([str(sanitized), str(fixture)], check=True, timeout=30,
                   env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"))
