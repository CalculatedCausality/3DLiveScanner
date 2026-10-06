#!/usr/bin/env python3
"""Real vendored codecs + Dataset API, independent large-model exporter suite.

Examples:
  python3 tests/dataset_texturing/run.py --sanitize
  python3 tests/dataset_texturing/run.py --faces 3000000 --frames 3
Build/fixtures are outside the repository; --keep retains them for inspection.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import os
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--sanitize", action="store_true")
p.add_argument("--faces", type=int, default=0)
p.add_argument("--frames", type=int, default=3)
p.add_argument("--keep", action="store_true")
p.add_argument("--reuse-codecs", type=Path, help="Reuse codec objects from a --keep run with identical compiler/sanitizer flags")
args = p.parse_args()
work = Path(tempfile.mkdtemp(prefix="dataset-texturing-", dir="/tmp/opencode"))
print("Work directory:", work, flush=True)
CC = shlex.split(os.environ.get("CC", "cc"))
CXX = shlex.split(os.environ.get("CXX", "c++"))


def run(cmd):
    subprocess.run([str(s) for s in cmd], cwd=ROOT, check=True, timeout=900,
                   env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                            ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))


try:
    jpeg, png = ROOT / "third_party/libjpeg-turbo", ROOT / "third_party/libpng"
    flags = ["-O2", "-g", "-ffunction-sections", "-fdata-sections", "-fno-pie"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    defines = ['-DBUILD="20141110"', "-DC_ARITH_CODING_SUPPORTED=1", "-DD_ARITH_CODING_SUPPORTED=1",
               "-DBITS_IN_JSAMPLE=8", "-DJPEG_LIB_VERSION=62", '-DLIBJPEG_TURBO_VERSION="1.3.90"',
               "-DMEM_SRCDST_SUPPORTED=1", "-DNEED_SYS_TYPES_H=1", "-DSTDC_HEADERS=1", "-DWITH_SIMD=1",
               "-DSIZEOF_SIZE_T=8", "-DINLINE=inline __attribute__((always_inline))"]
    defines += ["-DHAVE_" + s + "=1" for s in ["DLFCN_H", "INTTYPES_H", "LOCALE_H", "MEMCPY", "MEMORY_H",
                "MEMSET", "STDDEF_H", "STDINT_H", "STDLIB_H", "STRINGS_H", "STRING_H", "SYS_STAT_H",
                "SYS_TYPES_H", "UNISTD_H", "UNSIGNED_CHAR", "UNSIGNED_SHORT"]]
    sources = [jpeg / s for s in sorted(set(re.findall(r"src/[a-z0-9_-]+\.c", (jpeg / "Android.mk").read_text())))]
    objects = [work / (s.stem + ".o") for s in sources]

    def build_jpeg(pair):
        s, obj = pair
        run(CC + flags + ["-fno-sanitize=shift"] + defines + ["-I" + str(jpeg / "include"),
            "-I" + str(jpeg / "src"), "-c", s, "-o", obj])

    if args.reuse_codecs:
        objects = [args.reuse_codecs / o.name for o in objects]
    else:
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(build_jpeg, zip(sources, objects)))
    ps = [s for s in sorted(png.glob("png*.c")) if s.name != "pngtest.c"]
    po = [work / (s.stem + ".o") for s in ps]

    def build_png(pair):
        s, obj = pair
        run(CC + flags + ["-I" + str(png / "include"), "-c", s, "-o", obj])

    if args.reuse_codecs:
        po = [args.reuse_codecs / o.name for o in po]
    else:
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(build_png, zip(ps, po)))
    includes = [ROOT / "tests/performance/native/include", ROOT / "common", ROOT / "third_party/glm",
                ROOT / "third_party/tango_3d_reconstruction/include", png / "include", jpeg / "src", ROOT]
    cpp = [ROOT / s for s in ["reconstruction/dataset_texturing.cc", "common/data/image.cc",
                             "common/data/dataset.cc", "tests/dataset_texturing/test.cc"]]
    co = [work / (s.stem + ".o") for s in cpp]
    command = CXX + flags + ["-std=c++11", "-DANDROID", "-DSCANNER_MODERN=1", "-include",
                            ROOT / "tests/performance/native/include/host.h"] + ["-I" + str(s) for s in includes]

    def build_cpp(pair):
        s, obj = pair
        run(command + ["-c", s, "-o", obj])

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(build_cpp, zip(cpp, co)))
    run(CXX + flags + objects + po + co + ["-no-pie", "-Wl,--gc-sections", "-pthread", "-lz", "-lm", "-o", work / "test"])
    run([work / "test", work, args.faces, args.frames])
finally:
    if args.keep:
        print("Retained", work, flush=True)
    else:
        shutil.rmtree(work)
