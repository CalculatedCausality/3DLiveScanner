#!/usr/bin/env python3
"""Build/run isolated NNAPI probes; retain JSON and logs, clean only our remote dir."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from typing import Any


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path('/tmp/opencode/pixelshare-sdk'))
    parser.add_argument('--ndk-version', default='28.2.13676358')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=15)
    parser.add_argument('--case', action='append', help='CASE:q8 or CASE:f32 (repeatable)')
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 100:
        parser.error('repetitions must be 1..100')
    args.output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name('depth_ops.c')
    binary = args.output / 'depth_ops'
    compiler = args.sdk / 'ndk' / args.ndk_version / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang'
    command = [str(compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-deprecated-declarations', str(source), '-lneuralnetworks', '-lm',
               '-Wl,-z,max-page-size=16384', '-o', str(binary)]
    subprocess.run(command, check=True, timeout=60)
    adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
    remote = '/data/local/tmp/pixel-tpu-' + uuid.uuid4().hex
    cases = args.case or [f'{name}:q8' for name in
                         ('conv', 'depthwise', 'pointwise', 'pool', 'resize', 'concat',
                          'add', 'mul', 'l2', 'decoder', 'decoder_l2')] + [
                         'conv:f32', 'l2:f32', 'div:f32', 'decoder:f32']
    report: dict[str, Any] = dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  compiler_command=command, serial=args.serial, remote=remote,
                  source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), cases=[])

    def save():
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    for prop in ('ro.product.model', 'ro.build.fingerprint', 'ro.build.version.sdk'):
        report[prop] = subprocess.check_output(adb + ['shell', 'getprop', prop], text=True, timeout=15).strip()
    subprocess.run(adb + ['shell', 'ls', '-d', '/data/local/tmp'], check=True, timeout=15)
    subprocess.run(adb + ['shell', 'mkdir', remote], check=True, timeout=15)
    save()
    try:
        subprocess.run(adb + ['push', str(binary), remote + '/depth_ops'], check=True, timeout=30)
        subprocess.run(adb + ['shell', 'chmod', '700', remote + '/depth_ops'], check=True, timeout=15)
        for case in cases:
            name, dtype = case.split(':')
            # Validate before including arguments in adb's remote shell command.
            if name not in ('conv', 'depthwise', 'pointwise', 'pool', 'resize', 'concat',
                            'add', 'mul', 'l2', 'div', 'decoder', 'decoder_l2') or dtype not in ('q8', 'f32'):
                raise ValueError(case)
            dump = dtype == 'q8' and name in ('l2', 'decoder', 'decoder_l2')
            prefix = remote + '/' + name + '-' + dtype
            cmd = adb + ['shell', 'timeout', '90', remote + '/depth_ops', name, dtype, str(args.repetitions)]
            if dump:
                cmd.append(prefix)
            process = subprocess.run(cmd, text=True, capture_output=True, timeout=105)
            (args.output / f'{name}-{dtype}.stdout.jsonl').write_text(process.stdout)
            (args.output / f'{name}-{dtype}.stderr.txt').write_text(process.stderr)
            records = [json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
            if dump:
                for suffix, device in (('tpu', 'google-edgetpu'), ('cpu', 'nnapi-reference')):
                    if any(r['kind'] == 'timing' and r['device'] == device for r in records):
                        subprocess.run(adb + ['pull', prefix + '.' + suffix + '.bin', str(args.output)],
                                       check=True, timeout=30, capture_output=True)
            report['cases'].append(dict(case=case, command=cmd, exit_code=process.returncode, records=records))
            save()
            print(json.dumps(dict(case=case, exit_code=process.returncode,
                                  results=[r for r in records if r['kind'] in
                                           ('support', 'timing', 'comparison', 'output_range', 'compile_error')])), flush=True)
    finally:
        cleanup = subprocess.run(adb + ['shell', 'rm', '-rf', remote], timeout=15)
        report['remote_cleanup_exit_code'] = cleanup.returncode
        save()
    if any(c['exit_code'] != 0 for c in report['cases']) or cleanup.returncode:
        raise SystemExit('Probe failures recorded; inspect results.json and stderr files')


if __name__ == '__main__':
    main()
