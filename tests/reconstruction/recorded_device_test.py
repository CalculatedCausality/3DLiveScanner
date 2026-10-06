#!/usr/bin/env python3
"""Private-acquisition roundtrip using ONLY generated geometry in isolated scratch.

Requires the scanner's existing debug run-as access. Does not install/restart it,
access real captures/images, change preferences or broaden storage permissions.
All device/local test payloads are removed; none become a recorded baseline.
"""
import argparse
import contextlib
import io
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import uuid

sys.dont_write_bytecode = True
import recorded

parser = argparse.ArgumentParser()
parser.add_argument('--adb', type=Path, required=True)
parser.add_argument('--serial', required=True)
args = parser.parse_args()
package = 'com.lvonasek.arcore3dscanner'
adb = [str(args.adb), '-s', args.serial]
remote = 'files/.recorded-probe-' + uuid.uuid4().hex
with tempfile.TemporaryDirectory(prefix='recorded-private-roundtrip-', dir='/tmp/opencode') as temporary:
    work = Path(temporary)
    fixture = work / 'generated'
    original = recorded.synthetic(fixture)
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        for name in recorded.names(original['state']['count']):
            tar.add(fixture / name, arcname=name, recursive=False)
    app_root = subprocess.check_output(adb + ['shell', 'run-as', package, 'pwd'], text=True, timeout=15).strip()
    if not app_root.startswith('/data/') or not app_root.endswith('/' + package):
        raise ValueError('Unexpected app data path')
    try:
        command = ('ls -d files && mkdir ' + shlex.quote(remote)
                   + ' && tar -xf - -C ' + shlex.quote(remote))
        subprocess.run(adb + ['shell', '-T', 'run-as ' + package + ' sh -c ' + shlex.quote(command)],
                       input=archive.getvalue(), check=True, timeout=30)
        with contextlib.redirect_stdout(io.StringIO()):
            recorded.acquire(SimpleNamespace(adb=args.adb, serial=args.serial,
                remote=app_root + '/' + remote, output=work / 'roundtrip', run_as=package,
                expect_count=6, expect_width=48, expect_height=48))
        result = recorded.validate(work / 'roundtrip')
        assert result['fixture_sha256'] == original['fixture_sha256']
        assert result['raw_file_count'] == 13
        print('PASS: generated private-UID geometry acquisition, 13 exact files, no real scan accessed')
    finally:
        subprocess.run(adb + ['shell', 'run-as', package, 'rm', '-rf', remote], check=True, timeout=20)
