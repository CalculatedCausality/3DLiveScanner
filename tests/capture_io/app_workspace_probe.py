#!/usr/bin/env python3
"""Run generated I/O/selection/publication checks under the debug app's UID.

Does not install/restart the app, load scan images, change preferences, or touch
the active dataset. Requires run-as permission on a debug build. No privilege
settings are changed. Generated dex and isolated synthetic directories are removed.
"""
import argparse
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--serial', required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--library', required=True)
parser.add_argument('--package', default='com.lvonasek.arcore3dscanner')
parser.add_argument('--serial-writes', action='store_true', help='Use serial persistence to match the frozen native baseline')
args = parser.parse_args()
package = args.package
assert re.fullmatch(r'[A-Za-z0-9_.]+',package), 'Invalid application ID'
adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
home = Path(os.environ.get('JAVA_HOME', '/tmp/opencode/scanner-jdk17'))
android = args.sdk / 'platforms/android-35/android.jar'
probe_id = uuid.uuid4().hex
remote = 'cache/workspace-probe-' + probe_id
public_dex = '/data/local/tmp/workspace-probe-' + probe_id + '.jar'


def shell(command, **kwargs):
    return subprocess.run(adb + ['shell', '-T', command], check=True, timeout=180, **kwargs)


app_root = subprocess.check_output(adb + ['shell', 'run-as', package, 'pwd'], text=True, timeout=15).strip()
if not app_root.startswith('/data/') or not app_root.endswith('/' + package):
    raise SystemExit('Unexpected app data path; refusing to create probe')
with tempfile.TemporaryDirectory(prefix='app-workspace-probe-', dir='/tmp/opencode') as temporary:
    work = Path(temporary)
    classes = work / 'classes'
    classes.mkdir()
    sources = [ROOT / path for path in [
        'common/utils/com/lvonasek/utils/IO.java',
        'scanner/app/src/main/java/com/lvonasek/arcore3dscanner/ui/CaptureWorkspace.java',
        'tests/capture_io/AppWorkspaceProbe.java']]
    subprocess.run([str(home / 'bin/javac'), '--release', '8', '-classpath', str(android),
                    '-d', str(classes)] + [str(path) for path in sources], check=True, timeout=60)
    jar = work / 'probe.jar'
    subprocess.run([str(args.sdk / 'build-tools/35.0.0/d8'), '--min-api', '24', '--lib', str(android),
                    '--output', str(jar)] + [str(path) for path in classes.rglob('*.class')],
                   check=True, timeout=60, env=dict(os.environ, JAVA_HOME=str(home)))
    try:
        prepare = ('ls -d files cache && mkdir ' + shlex.quote(remote)
                   + ' && cat >' + shlex.quote(remote + '/probe.jar')
                   + ' && chmod 400 ' + shlex.quote(remote + '/probe.jar'))
        shell('run-as ' + package + ' sh -c ' + shlex.quote(prepare), input=jar.read_bytes())
        command = ('CLASSPATH=' + shlex.quote(app_root + '/' + remote + '/probe.jar')
                   + ' /system/bin/app_process /system/bin '
                   + 'com.lvonasek.arcore3dscanner.ui.AppWorkspaceProbe '
                    + shlex.quote(app_root + '/files') + ' ' + shlex.quote(args.library) + ' ' + probe_id
                    + (' serial' if args.serial_writes else ''))
        shell('run-as ' + package + ' sh -c ' + shlex.quote(command))
        # run-as has the app UID but may not inherit its shared-storage mount /
        # package attribution. A separate shell-UID comparison is clearly labelled.
        shell('ls -d /data/local/tmp')
        subprocess.run(adb + ['push', str(jar), public_dex], check=True, timeout=30)
        shell('chmod 400 ' + shlex.quote(public_dex))
        command = ('CLASSPATH=' + shlex.quote(public_dex)
                   + ' /system/bin/app_process /system/bin '
                   + 'com.lvonasek.arcore3dscanner.ui.AppWorkspaceProbe /data/local/tmp '
                    + shlex.quote(args.library) + ' ' + probe_id + (' serial' if args.serial_writes else ''))
        shell(command)
    finally:
        shell('run-as ' + package + ' rm -rf ' + shlex.quote(remote)
              + ' ' + shlex.quote('files/.workspace-probe-' + probe_id))
        shell('rm -rf ' + shlex.quote(public_dex) + ' '
              + shlex.quote('/data/local/tmp/.workspace-probe-' + probe_id) + ' '
              + shlex.quote(args.library + '/.workspace-probe-' + probe_id))
