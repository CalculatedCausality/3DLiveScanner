#!/usr/bin/env python3
"""Build extraction-private geometry tests; optional frozen-core A/B baseline."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core-source', type=Path)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--legacy', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='meshing-', dir='/tmp/opencode') as temp:
    binary = Path(temp) / 'meshing_test'
    flags = ['-std=c++11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-DRECONSTRUCTION_CORE_TESTING']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
    if args.core_source:
        flags += ['-DMESHING_CORE_SOURCE="' + str(args.core_source.resolve()) + '"']
    if args.legacy or args.core_source:
        flags += ['-DRECONSTRUCTION_LEGACY_MESH']
    command = [os.environ.get('CXX', 'g++')] + flags + [
        '-I' + str(ROOT / 'third_party/glm'),
        '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
        str(ROOT / 'tests/reconstruction/meshing_test.cc'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, env=dict(os.environ,
        ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
