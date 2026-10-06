#!/usr/bin/env python3
"""Link the actual built ARM64 backend and test generated paging on the device.

Shell UID, isolated /data/local/tmp scratch, no real scans or app lifecycle changes.
Not a camera/whole-app performance benchmark. Pushed executable/scratch are removed.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--serial', required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--archive', type=Path, required=True)
args = parser.parse_args()
adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
compiler = args.sdk / 'ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++'
remote = '/data/local/tmp/scanner-paging-' + uuid.uuid4().hex
with tempfile.TemporaryDirectory(prefix='android-paging-', dir='/tmp/opencode') as temporary:
    binary = Path(temporary) / 'probe'
    subprocess.run([str(compiler), '-std=c++11', '-O2', '-static-libstdc++',
        '-I' + str(ROOT), '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
        str(ROOT / 'tests/capture_io/android_paging_smoke.cc'), str(args.archive), '-llog',
        '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-o', str(binary)],
        check=True, timeout=120)
    subprocess.run(adb + ['shell', 'ls', '-d', '/data/local/tmp'], check=True, timeout=15)
    subprocess.run(adb + ['shell', 'mkdir', remote], check=True, timeout=15)
    try:
        subprocess.run(adb + ['push', str(binary), remote + '/probe'], check=True, timeout=30)
        subprocess.run(adb + ['shell', 'chmod', '700', remote + '/probe'], check=True, timeout=15)
        subprocess.run(adb + ['shell', remote + '/probe', remote], check=True, timeout=240)
    finally:
        subprocess.run(adb + ['shell', 'rm', '-rf', remote], check=True, timeout=20)
