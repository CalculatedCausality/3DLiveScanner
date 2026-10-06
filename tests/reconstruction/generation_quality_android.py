#!/usr/bin/env python3
"""Compare generation on Android ARM64 using generated analytic scenes only.

Builds standalone binaries, never installs/restarts the app or reads scans/images.
This measures native fusion/extraction, not camera/render/save FPS. Remote scratch
is unique and removed after the comparison.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--before', type=Path, required=True)
parser.add_argument('--after', type=Path, default=ROOT / 'reconstruction/core.cc')
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--serial', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--resolution', type=float, default=.02)
parser.add_argument('--cpu', type=int, help='Optional allowed device CPU index; does not change governor settings')
parser.add_argument('--workload-case', type=int, choices=(0, 1), help='Use colored/clearing (0) or uncolored (1) moving-view workload instead of analytic shapes')
parser.add_argument('--frames', type=int, default=12)
parser.add_argument('--paged', action='store_true', help='Exercise the paged voxel-access path in a generated workload')
parser.add_argument('--paged-resolution', type=float, choices=(.01, .02, .03, .04, .05),
                    help='Explicit paged workload resolution, including live 5 cm coverage preview')
parser.add_argument('--no-clearing', action='store_true', help='Explicitly disable clearing in the paged workload, matching the stability profile')
parser.add_argument('--compare-output', action='store_true', help='Require identical complete generated mesh dumps for output-preserving experiments')
args = parser.parse_args()
if not .01 <= args.resolution <= .04 or not 1 <= args.frames <= 30 or (args.cpu is not None and not 0 <= args.cpu < 32):
    parser.error('Unsupported resolution/CPU')
if args.paged and args.workload_case is None:
    parser.error('--paged requires --workload-case')
if args.paged_resolution is not None and not args.paged:
    parser.error('--paged-resolution requires --paged')
if args.no_clearing and not args.paged:
    parser.error('--no-clearing requires --paged')
if args.compare_output and args.workload_case is None:
    parser.error('--compare-output requires --workload-case')
output = args.output.resolve()
if output == ROOT or ROOT in output.parents:
    parser.error('Keep generated results outside the repository')
output.mkdir(exist_ok=False)
compiler = args.sdk / 'ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++'
adb = [str(args.sdk / 'platform-tools/adb'), '-s', args.serial]
remote = '/data/local/tmp/generation-quality-' + uuid.uuid4().hex
sources = dict(before=args.before.resolve(), after=args.after.resolve())
hashes = {}
fixture = ROOT / ('tests/reconstruction/generation_quality.cc' if args.workload_case is None
                  else 'tests/reconstruction/generation_paged_workload.cc' if args.paged
                  else 'tests/reconstruction/core_benchmark.cc')
for source in sources.values():
    for path in [source, *source.parent.glob('*.h')]:
        hashes[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
for path in [fixture, ROOT / 'tests/reconstruction/recorded_geometry.h', ROOT / 'tests/reconstruction/core_benchmark.cc']:
    hashes[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
for name, source in sources.items():
    subprocess.run([str(compiler), '-std=c++11', '-O2', '-g', '-fno-fast-math', '-static-libstdc++',
        '-I' + str(ROOT / 'third_party/glm'), '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
        '-I' + str(ROOT / 'tests/reconstruction'), str(source),
        '-I' + str(ROOT / 'reconstruction'),
        str(fixture), '-llog',
        '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-o', str(output / name)],
        check=True, timeout=240)
subprocess.run(adb + ['shell', 'ls', '-d', '/data/local/tmp'], check=True, timeout=15)
subprocess.run(adb + ['shell', 'mkdir', remote], check=True, timeout=15)
results = dict(before=[], after=[])
mesh_hashes = dict(before=[], after=[])
try:
    for name in sources:
        subprocess.run(adb + ['push', str(output / name), remote + '/' + name], check=True, timeout=30)
        subprocess.run(adb + ['shell', 'chmod', '700', remote + '/' + name], check=True, timeout=15)
    for index, name in enumerate(['before', 'after', 'after', 'before']):
        command = ['shell']
        if args.paged:
            command += ['env', 'SCANNER_BENCH_CACHE=' + remote]
            if args.paged_resolution is not None:
                command += ['SCANNER_BENCH_RESOLUTION=' + str(args.paged_resolution)]
            if args.no_clearing:
                command += ['SCANNER_BENCH_CLEARING=0']
        if args.cpu is not None:
            command += ['taskset', format(1 << args.cpu, 'x')]
        command += [remote + '/' + name]
        dump = remote + '/' + name + '.mesh' if args.compare_output else '-'
        command += ([str(args.resolution)] if args.workload_case is None
                    else [dump, str(args.frames), str(args.workload_case)])
        run = subprocess.run(adb + command, check=True, capture_output=True, text=True, timeout=240)
        rows = ([json.loads(line) for line in run.stdout.splitlines() if line.strip()]
                if args.workload_case is None else [json.loads(run.stdout)])
        assert len(rows) == (9 if args.workload_case is None else 1)
        results[name].append(rows)
        if args.compare_output:
            digest = subprocess.check_output(adb + ['shell', 'sha256sum', dump], text=True, timeout=30).split()[0]
            mesh_hashes[name].append(digest)
        (output / f'{index}-{name}.json').write_text(json.dumps(rows, indent=2) + '\n')
        (output / f'{index}-{name}.stderr').write_text(run.stderr)
        print(name, 'completed:', len(rows), 'generated scene/workload records', flush=True)
finally:
    subprocess.run(adb + ['shell', 'rm', '-rf', remote], check=True, timeout=20)
for path, digest in hashes.items():
    assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest, 'Source changed during comparison'
for runs in results.values():
    for a, b in zip(runs[0], runs[1]):
        assert {k:v for k,v in a.items() if not k.endswith('_ms')} == {k:v for k,v in b.items() if not k.endswith('_ms')}
if args.compare_output:
    assert len(set(mesh_hashes['before'] + mesh_hashes['after'])) == 1, 'Target mesh bytes differ'
report = dict(source_hashes=hashes, results=results,
              resolution=args.resolution if args.workload_case is None else None, pinned_cpu=args.cpu,
              workload_case=args.workload_case, workload_frames=args.frames,
               paging_enabled=args.paged,
               paged_resolution_override=args.paged_resolution,
               clearing_override=False if args.no_clearing else None,
              ordered_mesh_hashes=mesh_hashes if args.compare_output else None,
              scope='ARM64 analytic native generator; generated input, not camera/whole-app FPS')
(output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS repeat-invariant geometry; report:', output / 'results.json')
