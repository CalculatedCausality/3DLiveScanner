#!/usr/bin/env python3
"""Owned ARCore input copy and worker transaction tests; NNAPI boundary is injected."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def definition(signature):
    text=(ROOT/'common/thread/reconstr.cc').read_text();start=text.index(signature);opening=text.index('{',start);depth=0
    for end in range(opening,len(text)):
        depth+=(text[end]=='{')-(text[end]=='}')
        if depth==0:return text[start:end+1]
    raise AssertionError(signature)
with tempfile.TemporaryDirectory(prefix='depth-capture-',dir='/tmp/opencode') as name:
    work=Path(name)
    flags=['-std=c++11','-O1','-g','-DSCANNER_MODERN=1','-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread',
           '-I'+str(ROOT/'common'),'-I'+str(ROOT/'third_party/glm')]
    for test in ('test','worker','visibility'):
        source=ROOT/'tests/depth_capture/test.cc'
        if test=='worker':
            source=work/'worker.cc'
            source.write_text((ROOT/'tests/depth_capture/worker.cc.in').read_text().replace('// PRODUCTION_METHOD',definition('void Reconstruction::ApplyExperimentalDepth()')))
        elif test=='visibility':
            source=ROOT/'tests/depth_capture/visibility.cc'
        binary=work/test
        subprocess.run(['g++',*flags,str(source),'-o',str(binary)],check=True,timeout=120)
        subprocess.run([str(binary),str(work)],check=True,timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
