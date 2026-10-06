#!/usr/bin/env python3
"""Run a read-only preview-header audit under existing debug-app run-as access."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--serial', required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--capture', required=True)
args = parser.parse_args()
package = 'com.lvonasek.arcore3dscanner'
adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
jdk = Path(os.environ.get('JAVA_HOME', '/tmp/opencode/scanner-jdk17'))
remote = 'cache/preview-audit-' + uuid.uuid4().hex
app_root = subprocess.check_output(adb + ['shell', 'run-as', package, 'pwd'], text=True, timeout=15).strip()
if not app_root.startswith('/data/') or not app_root.endswith('/' + package):
    raise ValueError('Unexpected app root')
with tempfile.TemporaryDirectory(prefix='preview-audit-', dir='/tmp/opencode') as temporary:
    work = Path(temporary)
    classes = work / 'classes'
    classes.mkdir()
    subprocess.run([str(jdk / 'bin/javac'), '--release', '8', '-d', str(classes),
        str(ROOT / 'tests/capture_io/PreviewMemoryAudit.java')], check=True, timeout=60)
    jar = work / 'audit.jar'
    subprocess.run([str(args.sdk / 'build-tools/35.0.0/d8'), '--min-api', '24', '--lib',
        str(args.sdk / 'platforms/android-35/android.jar'), '--output', str(jar)]
        + [str(p) for p in classes.rglob('*.class')], check=True, timeout=60,
        env=dict(os.environ, JAVA_HOME=str(jdk)))
    try:
        prepare = ('ls -d cache && mkdir ' + shlex.quote(remote) + ' && cat >'
                   + shlex.quote(remote + '/audit.jar') + ' && chmod 400 ' + shlex.quote(remote + '/audit.jar'))
        subprocess.run(adb + ['shell', '-T', 'run-as ' + package + ' sh -c ' + shlex.quote(prepare)],
                       input=jar.read_bytes(), check=True, timeout=30)
        command = ('CLASSPATH=' + shlex.quote(app_root + '/' + remote + '/audit.jar')
                   + ' /system/bin/app_process /system/bin '
                   + 'com.lvonasek.arcore3dscanner.diagnostics.PreviewMemoryAudit ' + shlex.quote(args.capture))
        subprocess.run(adb + ['shell', 'run-as ' + package + ' sh -c ' + shlex.quote(command)],
                       check=True, timeout=180)
    finally:
        subprocess.run(adb + ['shell', 'run-as', package, 'rm', '-rf', remote], check=True, timeout=20)
