#!/usr/bin/env python3
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='scanner-backoff-', dir='/tmp/opencode') as temporary:
    for name in ['backoff_test', 'paging_policy_test']:
        binary = Path(temporary) / name
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(ROOT / 'common'),
            str(ROOT / ('tests/capture_io/' + name + '.cc')), '-o', str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=15,
                       env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))
