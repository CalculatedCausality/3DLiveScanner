#!/usr/bin/env python3
"""Host ASan/UBSan fixtures, real core/File3d/Mesh/Image and bundled JPEG/PNG codecs.

No Android SDK, Gradle, device, proprietary library, codec stub or OBJ stub used.
Only GL/log platform headers are supplied by the existing host test harness.
"""
from concurrent.futures import ThreadPoolExecutor
import argparse
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = shlex.split(os.environ.get("CC", "cc"))
CXX = shlex.split(os.environ.get("CXX", "c++"))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--dataset", type=Path, help="Also validate/replay a generated dataset fixture")
args = parser.parse_args()


def run(command):
    subprocess.run([str(x) for x in command], check=True, cwd=ROOT, timeout=240)


with tempfile.TemporaryDirectory(prefix="texturing-", dir="/tmp/opencode") as tmp:
    work = Path(tmp)
    jpeg = ROOT / "third_party/libjpeg-turbo"
    png = ROOT / "third_party/libpng"
    mk = (jpeg / "Android.mk").read_text()
    sources = sorted(set(re.findall(r"src/[a-z0-9_-]+\.c", mk)))
    # These configuration flags mirror the vendored build (scalar SIMD fallback).
    defines = ["-DBUILD=\"20141110\"", "-DC_ARITH_CODING_SUPPORTED=1", "-DD_ARITH_CODING_SUPPORTED=1",
               "-DBITS_IN_JSAMPLE=8", "-DJPEG_LIB_VERSION=62", "-DLIBJPEG_TURBO_VERSION=\"1.3.90\"",
               "-DMEM_SRCDST_SUPPORTED=1", "-DNEED_SYS_TYPES_H=1", "-DSTDC_HEADERS=1", "-DWITH_SIMD=1",
               "-DSIZEOF_SIZE_T=8", "-DINLINE=inline __attribute__((always_inline))"]
    defines += ["-DHAVE_" + x + "=1" for x in ["DLFCN_H", "INTTYPES_H", "LOCALE_H", "MEMCPY", "MEMORY_H",
                "MEMSET", "STDDEF_H", "STDINT_H", "STDLIB_H", "STRINGS_H", "STRING_H", "SYS_STAT_H",
                "SYS_TYPES_H", "UNISTD_H", "UNSIGNED_CHAR", "UNSIGNED_SHORT"]]
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie",
             "-ffunction-sections", "-fdata-sections"]
    objects = [work / (Path(s).stem + ".o") for s in sources]

    def build_jpeg(pair):
        source, obj = pair
        # This legacy JPEG version sign-extends Huffman values using negative
        # signed left shifts (jdhuff.c:577). Keep ASan and other UBSan checks;
        # exclude only shift instrumentation in vendored JPEG C sources.
        run(CC + flags + ["-fno-sanitize=shift"] + defines + ["-I" + str(jpeg / "include"), "-I" + str(jpeg / "src"),
                                   "-c", jpeg / source, "-o", obj])

    print("Building real vendored JPEG and PNG codecs", flush=True)
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(build_jpeg, zip(sources, objects)))
    png_sources = sorted(png.glob("png*.c"))
    png_sources = [s for s in png_sources if s.name != "pngtest.c"]
    png_objects = [work / (s.stem + ".o") for s in png_sources]

    def build_png(pair):
        source, obj = pair
        run(CC + flags + ["-I" + str(png / "include"), "-c", source, "-o", obj])

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(build_png, zip(png_sources, png_objects)))
    includes = [ROOT / "tests/performance/native/include", ROOT / "common", ROOT / "third_party/glm",
                ROOT / "third_party/tango_3d_reconstruction/include", png / "include", jpeg / "src"]
    command = CXX + flags + ["-std=c++11", "-DANDROID", "-DSCANNER_MODERN=1", "-DRECONSTRUCTION_CORE_TESTING", "-include",
                            str(ROOT / "tests/performance/native/include/host.h")]
    command += ["-I" + str(p) for p in includes]
    cpp_sources = [ROOT / s for s in ["reconstruction/core.cc", "reconstruction/texturing.cc", "common/data/file3d.cc",
                   "common/data/mesh.cc", "common/data/image.cc", "tests/reconstruction/texturing_test.cc"]]
    if args.dataset:
        cpp_sources += [ROOT / "common/data/dataset.cc", ROOT / "tests/reconstruction/dataset_contract_test.cc"]
    cpp_objects = [work / (s.stem + ".o") for s in cpp_sources]

    def build_cpp(pair):
        source, obj = pair
        run(command + ["-c", source, "-o", obj])

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(build_cpp, zip(cpp_sources, cpp_objects)))
    common_objects = [p for p in cpp_objects if p.stem not in ("texturing_test", "dataset_contract_test")]
    run(CXX + flags + common_objects + [work / "texturing_test.o"] + objects + png_objects +
        ["-no-pie", "-Wl,--gc-sections", "-pthread", "-lz", "-lm", "-o", work / "test"])
    subprocess.run([str(work / "test"), str(work)], check=True, cwd=ROOT, timeout=180,
                    env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                             ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
    if args.dataset:
        run(CXX + flags + common_objects + [work / "dataset_contract_test.o"] + objects + png_objects +
            ["-no-pie", "-Wl,--gc-sections", "-pthread", "-lz", "-lm", "-o", work / "dataset-test"])
        subprocess.run([str(work / "dataset-test"), str(args.dataset.resolve())], check=True, cwd=ROOT, timeout=180,
                       env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                                ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"))
