#!/usr/bin/env python3
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='scanner-frame-writer-') as temporary:
    binary = Path(temporary) / 'test'
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++11', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(ROOT / 'common'),
        str(ROOT / 'tests/capture_io/frame_writer_test.cc'), '-o', str(binary)], check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=20,
                   env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))
