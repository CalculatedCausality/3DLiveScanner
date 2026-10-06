#!/usr/bin/env python3
"""Real paged-vs-RAM core tests; all cache files are anonymous in a private temp dir."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--sanitize",action="store_true")
parser.add_argument("--core-source",type=Path,default=ROOT / "reconstruction/core.cc",
                    help="Test an isolated/frozen core without editing the production source")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="paging-tests-",dir="/tmp/opencode") as temp:
    work = Path(temp)
    cache = work / "cache"
    cache.mkdir()
    marker = cache / "caller-owned-marker"
    marker.write_bytes(b"do not touch")
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",
                    "-I"+str(ROOT / "reconstruction"),
                    "-I"+str(ROOT / "third_party/tango_3d_reconstruction/include"),
                    "-c",str(ROOT / "tests/reconstruction/paging_header_test.c"),
                    "-o",str(work / "header.o")],check=True)
    flags = ["-std=c++11","-O2","-g","-Wall","-Wextra","-Werror","-DRECONSTRUCTION_CORE_TESTING"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-sanitize-recover=all","-fno-pie","-no-pie"]
    command = [os.environ.get("CXX","g++")] + flags + [
        "-I"+str(ROOT / "reconstruction"),
        "-I"+str(ROOT / "third_party/glm"),
        "-I"+str(ROOT / "third_party/tango_3d_reconstruction/include"),
        str(args.core_source.resolve()),str(ROOT / "tests/reconstruction/paging_test.cc"),
        "-o",str(work / "paging_test")]
    subprocess.run(command,check=True)
    subprocess.run([str(work / "paging_test"),str(cache)],check=True,timeout=360,
                   env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
    assert list(cache.iterdir()) == [marker] and marker.read_bytes() == b"do not touch"
