#!/usr/bin/env python3
"""Compile/run the real core against the checked-in ABI and GLM. No Android SDK."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--core-source", type=Path, default=ROOT / "reconstruction/core.cc",
                    help="Test a frozen core source without replacing the workspace implementation")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="reconstruction-core-") as work:
    binary = Path(work) / "core_test"
    command = [os.environ.get("CXX", "g++"), "-std=c++11", "-O1", "-g",
               "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-DRECONSTRUCTION_CORE_TESTING",
               "-I" + str(ROOT / "third_party/tango_3d_reconstruction/include"),
               "-I" + str(ROOT / "third_party/glm"),
               str(args.core_source.resolve()),
               str(ROOT / "tests/reconstruction/core_test.cc"), "-o", str(binary)]
    subprocess.run(command, check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    subprocess.run([str(binary)], check=True, env=env)
    resource_binary = Path(work) / "core_resource_test"
    resource_command = command.copy()
    resource_command[-3] = str(ROOT / "tests/reconstruction/core_resource_test.cc")
    resource_command[-1] = str(resource_binary)
    subprocess.run(resource_command, check=True)
    start = time.monotonic()
    resource = subprocess.run([str(resource_binary)], check=True, env=env,
                              capture_output=True, text=True)
    duration = time.monotonic() - start
    names = ("max_update_work", "max_chunks", "max_update_chunks", "max_points_per_frame")
    lines = resource.stderr.splitlines()
    for name in names:
        matching = [line for line in lines if "resource_limit=" + name + " " in line]
        assert 1 <= len(matching) <= 1 + int(duration / 5), (name, duration, lines)
        assert all("frame rejected, volume unchanged" in line for line in matching)
    assert all(any("resource_limit=" + name + " " in line for name in names) for line in lines), lines
    print(resource.stdout, end="")
    print("Named resource diagnostics: %d lines for 32 failures across 8 contexts; throttle passed." % len(lines))
