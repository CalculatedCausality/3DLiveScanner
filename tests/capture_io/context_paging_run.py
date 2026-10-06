#!/usr/bin/env python3
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'common/tango/scan.cc').read_text()
start = source.index('Tango3DR_ReconstructionContext CreateContext(')
opening = source.index('{', start)
depth = 0
definition = None
for end in range(opening, len(source)):
    depth += (source[end] == '{') - (source[end] == '}')
    if depth == 0:
        definition = source[start:end+1]
        break
assert definition is not None, 'Unterminated CreateContext definition'
template = (ROOT / 'tests/capture_io/context_paging_test.cc.in').read_text()
with tempfile.TemporaryDirectory(prefix='scanner-context-paging-', dir='/tmp/opencode') as temporary:
    work = Path(temporary)
    test = work / 'test.cc'
    test.write_text(template.replace('// PRODUCTION_CREATE_CONTEXT', definition))
    for modern in (0, 1):
        binary = work / ('test-' + str(modern))
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-DSCANNER_MODERN=' + str(modern), '-I' + str(ROOT), '-I' + str(ROOT / 'common'),
            '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
            str(test), '-o', str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=15,
                       env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))
