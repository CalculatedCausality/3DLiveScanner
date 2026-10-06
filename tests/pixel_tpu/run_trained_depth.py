#!/usr/bin/env python3
"""Build/run the fixed DCLN0001 NNAPI graph on two explicit devices; retain evidence."""
import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import uuid


NDK_VERSION = '28.2.13676358'
INPUT_BYTES = 120 * 160 * 3
OUTPUT_BYTES = 120 * 160
MODEL_BYTES = 1880
REMOTE_TIMEOUT = 180


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def bounded_read(path, minimum, maximum, multiple=1):
    if not path.is_file():
        raise ValueError(f'Not a regular file: {path}')
    size = path.stat().st_size
    if not minimum <= size <= maximum or size % multiple:
        raise ValueError(f'Invalid file size {size}: {path}')
    with path.open('rb') as stream:
        data = stream.read(maximum + 1)
    if len(data) != size:
        raise ValueError(f'File changed while reading: {path}')
    return data


def validate_model(data):
    if len(data) != MODEL_BYTES or data[:8] != b'DCLN0001':
        raise ValueError('Model must be exactly 1880 bytes with magic DCLN0001')
    activation = []
    offset = 8
    for _ in range(4):
        scale, zero = struct.unpack_from('<fi', data, offset)
        offset += 8
        if not math.isfinite(scale) or scale <= 0 or not 0 <= zero <= 255:
            raise ValueError('Invalid activation scale/zero')
        activation.append({'scale': scale, 'zero': zero})
    layers = []
    for i, (outputs, inputs) in enumerate(((12, 3), (12, 12), (1, 12))):
        scale, = struct.unpack_from('<f', data, offset)
        if not math.isfinite(scale) or scale <= 0:
            raise ValueError('Invalid weight scale')
        try:
            bias_scale, = struct.unpack('<f', struct.pack('<f', activation[i]['scale'] * scale))
        except OverflowError as error:
            raise ValueError('Bias scale product overflows float32') from error
        if not math.isfinite(bias_scale) or bias_scale <= 0:
            raise ValueError('Invalid float32 bias scale product')
        layers.append({'ohwi': [outputs, 3, 3, inputs], 'weight_scale': scale,
                       'weight_zero': 128, 'bias_scale': bias_scale})
        offset += 4 + outputs * 9 * inputs + outputs * 4
    if offset != len(data):
        raise ValueError('Trailing bytes in model')
    return {'activation': activation, 'layers': layers}


def manifest(path):
    data = path.read_bytes()
    return {'path': str(path), 'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest()}


def text(value):
    return value.decode('utf-8', errors='replace') if isinstance(value, bytes) else value or ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory outside the workspace')
    parser.add_argument('--sdk', type=Path, default=Path('/tmp/opencode/pixelshare-sdk'))
    parser.add_argument('--repetitions', type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 100:
        parser.error('repetitions must be 1..100')
    source = Path(__file__).resolve().with_name('trained_depth.c')
    workspace = source.parents[2]
    output = args.output.resolve()
    if output == workspace or workspace in output.parents:
        parser.error('--output must be outside the workspace (including symlinks)')
    if output.exists():
        parser.error('--output must not exist')
    output.mkdir(parents=True, exist_ok=False)
    report = {'started_utc': utc_now(), 'contract': 'DCLN0001',
              'quality_claim': 'not_evaluated', 'serial': args.serial,
              'repetitions': args.repetitions, 'remote_timeout_seconds': REMOTE_TIMEOUT,
              'status': 'started', 'files': {}, 'records': []}
    remote = '/data/local/tmp/trained-depth-' + uuid.uuid4().hex
    report['remote'] = remote
    adb = [str(args.sdk.resolve() / 'platform-tools/adb'), '-s', args.serial]
    remote_attempted = False

    def save():
        temporary = output / 'results.json.tmp'
        temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
        temporary.replace(output / 'results.json')

    def command(argv, timeout=30):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f'Command failed ({result.returncode}): {argv!r}\n{result.stderr}\n{result.stdout}')
        return result.stdout

    save()
    try:
        # Snapshot only the two explicitly provided, bounded binary inputs.
        model = bounded_read(args.model.resolve(), MODEL_BYTES, MODEL_BYTES)
        report['model'] = validate_model(model)
        inputs = bounded_read(args.inputs.resolve(), INPUT_BYTES, 64 * INPUT_BYTES, INPUT_BYTES)
        report['frames'] = len(inputs) // INPUT_BYTES
        report['original_inputs'] = {'model': str(args.model.resolve()), 'inputs': str(args.inputs.resolve())}
        (output / 'model.bin').write_bytes(model)
        (output / 'inputs.bin').write_bytes(inputs)
        (output / 'trained_depth.c').write_bytes(source.read_bytes())
        (output / 'run_trained_depth.py').write_bytes(Path(__file__).read_bytes())
        for name in ('model.bin', 'inputs.bin', 'trained_depth.c', 'run_trained_depth.py'):
            report['files'][name] = manifest(output / name)
        compiler = args.sdk.resolve() / 'ndk' / NDK_VERSION / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang'
        binary = output / 'trained_depth'
        compile_command = [str(compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-Wno-deprecated-declarations', str(output / 'trained_depth.c'),
                           '-lneuralnetworks', '-lm', '-Wl,-z,max-page-size=16384', '-o', str(binary)]
        report['compiler_command'] = compile_command
        save()
        compiled = subprocess.run(compile_command, capture_output=True, text=True, timeout=60)
        (output / 'compile.stdout.txt').write_text(compiled.stdout)
        (output / 'compile.stderr.txt').write_text(compiled.stderr)
        report['compile_exit_code'] = compiled.returncode
        if compiled.returncode:
            raise RuntimeError('Native build failed; see compile.stderr.txt')
        report['files']['trained_depth'] = manifest(binary)
        report['compiler_version'] = command([str(compiler), '--version'])
        report['adb_version'] = command([adb[0], 'version'])
        report['device_identity'] = {}
        for prop in ('ro.product.model', 'ro.product.device', 'ro.product.cpu.abi',
                     'ro.build.fingerprint', 'ro.build.version.sdk', 'ro.build.version.security_patch',
                     'ro.hardware', 'ro.soc.model'):
            report['device_identity'][prop] = command(adb + ['shell', 'getprop', prop], 15).strip()
        report['device_identity']['uname'] = command(adb + ['shell', 'uname', '-a'], 15).strip()
        save()
        command(adb + ['shell', 'ls', '-d', '/data/local/tmp'], 15)
        remote_attempted = True
        command(adb + ['shell', 'mkdir', remote], 15)
        for name in ('trained_depth', 'model.bin', 'inputs.bin'):
            command(adb + ['push', str(output / name), remote + '/' + name])
        command(adb + ['shell', 'chmod', '700', remote + '/trained_depth'], 15)
        prefix = remote + '/depth'
        # All remote arguments are fixed names, UUIDs or validated integers.
        native_command = adb + ['shell', 'timeout', '-s', 'KILL', str(REMOTE_TIMEOUT),
                                remote + '/trained_depth', remote + '/model.bin',
                                remote + '/inputs.bin', prefix, str(args.repetitions)]
        report['native_command'] = native_command
        report['status'] = 'running'
        save()
        try:
            process = subprocess.run(native_command, capture_output=True, text=True,
                                     timeout=REMOTE_TIMEOUT + 15)
            stdout, stderr = process.stdout, process.stderr
            report['native_exit_code'] = process.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr = text(error.stdout), text(error.stderr)
            report['native_exit_code'] = None
            report['host_timeout'] = True
        (output / 'native.stdout.jsonl').write_text(stdout)
        (output / 'native.stderr.txt').write_text(stderr)
        # Pull the flushed native report even after unsupported graphs or timeouts.
        pulled = subprocess.run(adb + ['pull', prefix + '.report.jsonl', str(output / 'depth.report.jsonl')],
                                capture_output=True, text=True, timeout=30)
        report['native_report_pull_exit_code'] = pulled.returncode
        report['native_report_pull_stderr'] = pulled.stderr
        record_text = (output / 'depth.report.jsonl').read_text() if pulled.returncode == 0 else stdout
        for line in record_text.splitlines():
            if not line.startswith('{'):
                continue
            try:
                report['records'].append(json.loads(line))
            except json.JSONDecodeError:
                report.setdefault('incomplete_native_records', []).append(line)
        report['drivers'] = [r for r in report['records'] if r.get('kind') == 'device']
        save()
        for suffix in ('tpu.bin', 'cpu.bin'):
            if any(r.get('kind') == 'output' and r.get('suffix') == suffix and r.get('status') == 0
                   for r in report['records']):
                destination = output / ('depth.' + suffix)
                command(adb + ['pull', prefix + '.' + suffix, str(destination)])
                report['files'][destination.name] = manifest(destination)
                if destination.stat().st_size != report['frames'] * OUTPUT_BYTES:
                    raise RuntimeError(f'Wrong output size: {destination}')
        if report.get('native_exit_code') != 0:
            raise RuntimeError('Native run failed or timed out; inspect native report and stderr')
        if pulled.returncode or report.get('incomplete_native_records'):
            raise RuntimeError('Native report missing or incomplete')
        if not any(r.get('kind') == 'complete' and r.get('status') == 0 for r in report['records']):
            raise RuntimeError('Native success record missing')
        if not all('depth.' + suffix in report['files'] for suffix in ('tpu.bin', 'cpu.bin')):
            raise RuntimeError('Complete CPU/TPU outputs missing')
        report['status'] = 'success'
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report['status'] = 'failed'
        report['error'] = str(error)
    except KeyboardInterrupt:
        report['status'] = 'interrupted'
        report['error'] = 'Interrupted; remote watchdog bounds any remaining native execution'
    finally:
        if remote_attempted:
            try:
                cleanup = subprocess.run(adb + ['shell', 'rm', '-rf', remote],
                                         capture_output=True, text=True, timeout=15)
                report['remote_cleanup_exit_code'] = cleanup.returncode
                report['remote_cleanup_stderr'] = cleanup.stderr
                if cleanup.returncode:
                    report['status'] = 'failed'
            except (OSError, subprocess.SubprocessError) as error:
                report['remote_cleanup_error'] = str(error)
                report['status'] = 'failed'
        report['finished_utc'] = utc_now()
        save()
    print(json.dumps({'status': report['status'], 'results': str(output / 'results.json'),
                      'error': report.get('error'),
                      'summary': [r for r in report['records'] if r.get('kind') in ('support', 'timing')
                                  or (r.get('kind') == 'comparison' and r.get('frame') == -1)]}, indent=2))
    return 0 if report['status'] == 'success' else 1


if __name__ == '__main__':
    sys.exit(main())
