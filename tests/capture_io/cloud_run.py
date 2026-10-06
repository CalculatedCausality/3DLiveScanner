#!/usr/bin/env python3
"""Run production cloud construction/update/worker/write definitions with SDK fakes.

Host ownership and byte-equivalence checks only; no Android build or device SDK.
"""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--modern", action="store_true", help="test transactional backend rejection policy")
args = parser.parse_args()


def definition(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError("Unterminated definition: " + signature)


template = (HERE / "cloud_test.cc.in").read_text()
for marker, path, signature in [
    ("PCL", "common/tango/retango.cc", "Tango3DR_PointCloud* Retango::PCL("),
    ("UPDATE", "common/tango/scan.cc", "bool TangoScan::Update("),
    ("DISCARD", "common/tango/scan.cc", "void TangoScan::DiscardAdded("),
    ("WORKER", "common/thread/reconstr.cc", "void* ProcessReconstruction("),
    ("WRITE", "common/data/dataset.cc", "bool Dataset::WritePointCloud("),
]:
    template = template.replace("// PRODUCTION_" + marker, definition(path, signature))

# Compile the current public declaration too, rather than a test-only signature.
header = (ROOT / "common/tango/scan.h").read_text()
start = header.index("bool Update(")
template = template.replace("// PRODUCTION_DECLARATION", header[start:header.index(";", start) + 1])

with tempfile.TemporaryDirectory(prefix="scanner-cloud-", dir="/tmp/opencode") as temporary:
    work = Path(temporary)
    (work / "cloud_test.cc").write_text(template)
    command = shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-pthread",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-DSCANNER_MODERN=" + str(int(args.modern)),
        "-I" + str(ROOT / "common"), "-I" + str(ROOT / "third_party/glm"),
        "-I" + str(ROOT),
        "-I" + str(ROOT / "third_party/tango_3d_reconstruction/include"),
        str(work / "cloud_test.cc"), "-o", str(work / "cloud_test"),
    ]
    subprocess.run(command, check=True, timeout=120)
    subprocess.run([str(work / "cloud_test"), str(work / "cloud.pcl")],
                   check=True, timeout=30,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                            UBSAN_OPTIONS="halt_on_error=1"))
