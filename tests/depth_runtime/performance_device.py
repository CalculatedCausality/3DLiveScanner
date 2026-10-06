#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Explicit, bounded synthetic native ABBA; no app, camera, NNAPI or settings access."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path, help='performance.py --android-ndk artifact directory')
parser.add_argument('--adb', required=True, type=Path)
parser.add_argument('--device', required=True)
args = parser.parse_args()
adb = [str(args.adb), '-s', args.device]

def call(*command):
    return subprocess.check_output([*adb, *command], text=True, timeout=20)

assert all((args.build/name).is_file() for name in ['baseline', 'current'])
remote = '/data/local/tmp/'+args.build.name
call('shell', 'ls', '-d', '/data/local/tmp')
call('shell', 'mkdir', remote)  # Fails rather than reusing an existing directory.
records = []
try:
    for name in ['baseline', 'current']:
        call('push', str(args.build/name), remote+'/'+name)
        call('shell', 'chmod', '700', remote+'/'+name)
    expected = None
    for index, name in enumerate(['baseline', 'current', 'current', 'baseline']):
        text = call('shell', remote+'/'+name, 'bench', '2', remote+'/bytes.bin')
        (args.build/(str(index)+'-'+name+'-device.log')).write_text(text)
        print(name, text, flush=True)
        local = args.build/(str(index)+'-'+name+'-device.bin')
        call('pull', remote+'/bytes.bin', str(local))
        actual = local.read_bytes()
        if expected is None:
            expected = actual
        assert actual == expected, 'Native benchmark feature/mask bytes differ'
        records.append(dict((shape+' '+kind, float(ms)) for shape, kind, ms in re.findall(
            r'(\d+x\d+) (dense|holes) preprocessing_component_ms=([0-9.]+)', text)))
    assert expected is not None and len(records[0]) == 8
    print(f'PASS: all {len(expected)} native benchmark feature/mask bytes identical in all four runs; '
          f'SHA256 {hashlib.sha256(expected).hexdigest()}')
    for label in records[0]:
        baseline = (records[0][label]+records[3][label])/2
        current = (records[1][label]+records[2][label])/2
        print(f'{label}: ABBA mean-of-medians baseline={baseline:.6f}ms current={current:.6f}ms '
              f'delta={current-baseline:.6f}ms reduction={100*(1-current/baseline):.2f}% speedup={baseline/current:.3f}x')
finally:
    # Only this invocation's synthetic files; never touches application storage.
    call('shell', 'rm', '-r', remote)
