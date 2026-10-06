#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""NDK28.2 API24 native smoke build and no-NNAPI-link dependency audit (no adb)."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NDK = Path(os.environ.get('ANDROID_NDK_HOME', '/tmp/opencode/android-sdk/ndk/28.2.13676358'))
BIN = NDK/'toolchains/llvm/prebuilt/linux-x86_64/bin'
OUT = Path(tempfile.mkdtemp(prefix='depth-runtime-api24-',dir='/tmp/opencode'))
compiler = BIN/'aarch64-linux-android24-clang++'
flags = ['-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'third_party/glm')]
subprocess.run([str(compiler), *flags, '-c', str(ROOT/'tests/depth_runtime/android_abi_check.cc'),
                '-o', str(OUT/'abi_check.o')],check=True)
exe = OUT/'depth_runtime_smoke'
subprocess.run([str(compiler), *flags, str(ROOT/'common/depth/experimental.cc'),
                str(ROOT/'tests/depth_runtime/native_smoke.cc'), '-static-libstdc++', '-ldl', '-llog',
                '-Wl,-z,max-page-size=16384', '-o', str(exe)],check=True)
dynamic = subprocess.check_output([str(BIN/'llvm-readelf'), '-d', str(exe)],text=True)
symbols = subprocess.check_output([str(BIN/'llvm-nm'), '-u', str(exe)],text=True)
assert 'libneuralnetworks' not in dynamic and 'ANeuralNetworks' not in symbols
print(dynamic)
print('PASS: API24, NDK28.2, NNAPI ABI constants/layout, no DT_NEEDED or undefined NNAPI symbols')
print(exe)
