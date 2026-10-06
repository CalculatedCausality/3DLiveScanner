#!/usr/bin/env python3
"""Independent analytic generation measurements; inputs generated, no camera/UI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'reconstruction/core.cc')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--resolution', type=float, default=.02)
parser.add_argument('--repeats', type=int, default=2)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
if not .01 <= args.resolution <= .04 or not 1 <= args.repeats <= 5:
    parser.error('Unsupported fixture resolution/repeat count')
output = args.output.resolve()
if output == ROOT or ROOT in output.parents:
    parser.error('Keep generated results outside the repository')
output.mkdir(exist_ok=False)
source = args.source.resolve()
inputs = [source, *sorted(source.parent.glob('*.h')), ROOT / 'tests/reconstruction/generation_quality.cc',
          ROOT / 'tests/reconstruction/recorded_geometry.h']
hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
binary = output / 'quality'
flags = ['-std=c++11', '-O2', '-g', '-fno-fast-math', '-Wall', '-Wextra', '-Werror']
if args.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
subprocess.run(shlex.split(os.environ.get('CXX', 'g++')) + flags + [
    '-I' + str(ROOT / 'third_party/glm'), '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
    '-I' + str(ROOT / 'tests/reconstruction'), str(source), str(ROOT / 'tests/reconstruction/generation_quality.cc'),
    '-pthread', '-o', str(binary)], check=True, timeout=180)
results = []
for repeat in range(args.repeats):
    process = subprocess.run([str(binary), str(args.resolution)], text=True, capture_output=True,
        check=True, timeout=240, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                                        UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
    rows = [json.loads(line) for line in process.stdout.splitlines() if line.strip()]
    assert len(rows) == 9, rows
    results.append(rows)
    (output / ('run-' + str(repeat) + '.json')).write_text(json.dumps(rows, indent=2) + '\n')
    (output / ('run-' + str(repeat) + '.stderr')).write_text(process.stderr)
for p in inputs:
    assert hashlib.sha256(p.read_bytes()).hexdigest() == hashes[str(p)], 'Source changed during run'
for run in results[1:]:
    for a, b in zip(results[0], run):
        assert {k:v for k,v in a.items() if not k.endswith('_ms')} == {k:v for k,v in b.items() if not k.endswith('_ms')}
report = dict(source_hashes=hashes, flags=flags, resolution=args.resolution, sanitizer=args.sanitize, results=results,
              scope='Analytic synthetic surface/coverage checks; host timings, not phone FPS or real scan accuracy')
(output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(dict(report=str(output / 'results.json'), scenes=len(results[0]), repeats=args.repeats), indent=2))
