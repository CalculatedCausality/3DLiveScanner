#!/usr/bin/env python3
"""Run the real export backend on the phone, keeping capture images on the phone.

Sources are read-only. Output/binary use unique /data/local/tmp scratch; cleanup
removes only that scratch. No APK install, app restart, settings or permissions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--serial', required=True)
parser.add_argument('--dataset', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--codec-dir', type=Path, required=True)
parser.add_argument('--keep-output', action='store_true')
args = parser.parse_args()
output = args.output.resolve()
if output == ROOT or ROOT in output.parents:
    parser.error('Local report/build output must stay outside the repository')
if not args.dataset.startswith('/') or any(c in args.dataset for c in '\r\n\0'):
    parser.error('An absolute device dataset directory is required')
output.mkdir(exist_ok=False)
sources = [ROOT / name for name in ('reconstruction/dataset_texturing.cc',
           'common/data/dataset.cc', 'common/data/image.cc', 'tests/export_device/probe.cc')]
tracked = sources + sorted((ROOT / 'reconstruction').glob('dataset_textur*.h')) + [
    ROOT / 'reconstruction/texture_geometry.h', ROOT / 'common/data/dataset.h',
    ROOT / 'common/data/image.h', ROOT / 'common/gl/opengl.h', Path(__file__).resolve(),
    args.codec_dir / 'libjpeg-turbo.a', args.codec_dir / 'libpng.a']
hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
compiler = args.sdk / 'ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++'
binary = output / 'export-probe'
includes = ['reconstruction', 'common', 'third_party/glm', 'third_party/libjpeg-turbo/include',
            'third_party/libjpeg-turbo/src',
            'third_party/libpng/include', 'third_party/tango_3d_reconstruction/include']
subprocess.run([str(compiler), '-std=c++11', '-O2', '-g', '-DANDROID', '-DSCANNER_MODERN=1',
    '-ffunction-sections', '-fdata-sections', '-fexceptions', '-static-libstdc++',
    *['-I' + str(ROOT / name) for name in includes], *map(str, sources),
    str(args.codec_dir / 'libjpeg-turbo.a'), str(args.codec_dir / 'libpng.a'),
    '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384',
    '-llog', '-lz', '-landroid', '-o', str(binary)], check=True, timeout=300)
adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
remote = '/data/local/tmp/scanner-export-' + uuid.uuid4().hex
model = args.dataset.rstrip('/') + '/model.obj'
state = args.dataset.rstrip('/') + '/state.txt'

def shell(*command, timeout=30):
    return subprocess.run(adb + ['shell', ' '.join(shlex.quote(s) for s in command)],
                          check=True, capture_output=True, text=True, timeout=timeout)

def fingerprint():
    return shell('sha256sum', model, state, timeout=180).stdout

before = fingerprint()
shell('ls', '-d', '/data/local/tmp')
shell('mkdir', remote)
success = False
try:
    subprocess.run(adb + ['push', str(binary), remote + '/probe'], check=True, timeout=60)
    shell('chmod', '700', remote + '/probe')
    # Bound execution on the device too, so losing the local adb subprocess
    # cannot leave an unbounded export running after scratch cleanup.
    command = ['timeout', '2100', remote + '/probe', args.dataset, model, remote + '/model.obj']
    print('Export running; live progress:', output / 'stderr.txt', flush=True)
    with (output / 'stdout.txt').open('w') as stdout, (output / 'stderr.txt').open('w') as stderr:
        subprocess.run(adb + ['shell', ' '.join(shlex.quote(s) for s in command)],
                       check=True, stdout=stdout, stderr=stderr, text=True, timeout=2160)
    result = json.loads((output / 'stdout.txt').read_text())
    assert result['success'] and result['input_faces'] == result['output_faces']
    assert before == fingerprint(), 'Source geometry or commit state changed during export'
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == h for p, h in hashes.items())
    result.update(source_hashes=hashes, input_fingerprint=before,
                  remote_output=remote + '/model.obj' if args.keep_output else None,
                  images_transferred_to_host=False, source_unchanged=True)
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    success = True
except subprocess.CalledProcessError as error:
    if error.stdout: (output / 'stdout.txt').write_text(error.stdout)
    if error.stderr:
        (output / 'stderr.txt').write_text(error.stderr)
        print(error.stderr)
    raise
finally:
    if args.keep_output and success:
        shell('rm', '-f', remote + '/probe')
    else:
        shell('rm', '-rf', remote, timeout=120)
