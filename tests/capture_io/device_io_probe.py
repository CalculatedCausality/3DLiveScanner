#!/usr/bin/env python3
"""Build/run a synthetic ARM64 codec/filesystem probe using an existing APK build.

Runs as adb shell, not the app UID. Does not measure the complete capture pipeline
and does not open any scan files. Scratch files and the pushed binary are removed.
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
parser.add_argument('--jpeg-archive', type=Path, required=True)
args = parser.parse_args()
adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
compiler = args.sdk / 'ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++'
remote = '/data/local/tmp/scanner-io-probe-' + uuid.uuid4().hex
with tempfile.TemporaryDirectory(prefix='scanner-device-io-', dir='/tmp/opencode') as temporary:
    binary = Path(temporary) / 'probe'
    subprocess.run([str(compiler), '-std=c++11', '-O2', '-static-libstdc++',
                    '-I' + str(ROOT / 'third_party/libjpeg-turbo/src'),
                    '-I' + str(ROOT / 'third_party/libjpeg-turbo/include'),
                    str(ROOT / 'tests/capture_io/device_io_probe.cc'), str(args.jpeg_archive),
                    '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384',
                    '-o', str(binary)], check=True, timeout=120)
    try:
        subprocess.run(adb + ['push', str(binary), remote], check=True, timeout=30)
        subprocess.run(adb + ['shell', 'chmod', '700', remote], check=True, timeout=15)
        # Alternate views twice: inode/page cache, temperature and OS load vary.
        for parent, parallel in [('/data/local/tmp', False), ('/data/local/tmp', True),
                                 ('/storage/emulated/0/Download', False),
                                 ('/storage/emulated/0/Download', True),
                                 ('/storage/emulated/0/Download', True),
                                 ('/storage/emulated/0/Download', False)]:
            print('Synthetic shell-UID filesystem:', parent, 'parallel:', parallel, flush=True)
            subprocess.run(adb + ['shell', remote, parent] + (['parallel'] if parallel else []),
                           check=True, timeout=120)
    finally:
        subprocess.run(adb + ['shell', 'rm', '-f', remote], timeout=15, check=True)
