#!/usr/bin/env python3
"""Observed partial-cell geometry and unknown-space exclusion under sanitizers."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,default=ROOT/'reconstruction/core.cc')
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='partial-cells-',dir='/tmp/opencode') as temp:
    work=Path(temp);cache=work/'cache';cache.mkdir()
    command=['g++','-std=c++11','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
             '-DMESHING_CORE_SOURCE="'+str(args.source.resolve())+'"',
             '-I'+str(ROOT/'third_party/glm'),'-I'+str(ROOT/'third_party/tango_3d_reconstruction/include'),
             str(ROOT/'tests/reconstruction/partial_cells_test.cc'),'-o',str(work/'test')]
    subprocess.run(command,check=True,timeout=120)
    subprocess.run([str(work/'test'),str(cache)],check=True,timeout=60,
                   env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    assert not list(cache.iterdir())
