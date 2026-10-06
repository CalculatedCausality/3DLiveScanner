#!/usr/bin/env python3
"""Alternate frozen/current update-only runs; require complete output identity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--before', type=Path, required=True)
parser.add_argument('--after', type=Path, default=ROOT / 'reconstruction/core.cc')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--frames', type=int, default=8)
parser.add_argument('--repeats', type=int, default=3)
args = parser.parse_args()
assert 1 <= args.frames <= 24 and 1 <= args.repeats <= 7
output = args.output.resolve()
assert output != ROOT and ROOT not in output.parents
output.mkdir(exist_ok=False)
cache = output / 'cache'; cache.mkdir()
def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024*1024), b''):
            h.update(data)
    return h.hexdigest()
sources, binaries = {}, {}
for name, source in [('before', args.before.resolve()), ('after', args.after.resolve())]:
    deps = [source, *source.parent.glob('*.h'), ROOT / 'tests/reconstruction/core_benchmark.cc',
            ROOT / 'tests/reconstruction/meshing_dirty_benchmark.cc']
    sources[name] = {str(p): digest(p) for p in deps}
    binary = output / name
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++11', '-O2', '-g', '-fno-fast-math',
        '-Wall', '-Wextra', '-Werror', '-DMESHING_CORE_SOURCE="' + str(source) + '"',
        '-I' + str(ROOT / 'third_party/glm'), '-I' + str(ROOT / 'third_party/tango_3d_reconstruction/include'),
        str(ROOT / 'tests/reconstruction/meshing_dirty_benchmark.cc'), '-o', str(binary)], check=True)
    binaries[name] = binary
rows = []
for h in (.02, .04):
    for paged in (False, True):
        case = f'{h:.2f}-' + ('paged' if paged else 'ram')
        results = {'before': [], 'after': []}; expected = None
        for repeat in range(args.repeats):
            for name in (('before', 'after') if repeat % 2 == 0 else ('after', 'before')):
                mesh = output / f'{case}-{name}-{repeat}.bin'
                run = subprocess.run([str(binaries[name]), str(mesh), str(h), str(args.frames), str(cache) if paged else '-'],
                                     check=True, capture_output=True, text=True, timeout=300)
                result = json.loads(run.stdout); result['output_sha256'] = digest(mesh)
                if expected is None:
                    expected = result['output_sha256']
                assert expected == result['output_sha256'], (case, name, repeat, 'Dirty/mesh byte difference')
                results[name].append(result)
                if repeat:
                    mesh.unlink()
        summary = {'case': case, 'output_sha256': expected, 'results': results}
        for metric in ('update_ms', 'update_cpu_ms'):
            summary[metric] = {name: statistics.median(sum(r[metric]) for r in results[name]) for name in results}
        summary['writable_cpu_ms'] = {name: statistics.median(r['writable_cpu_ms'] for r in results[name]) for name in results}
        rows.append(summary)
        print(json.dumps({k: v for k, v in summary.items() if k != 'results'}), flush=True)
for hashes in sources.values():
    assert all(digest(Path(p)) == h for p, h in hashes.items())
assert not list(cache.iterdir())
(output / 'results.json').write_text(json.dumps(dict(sources=sources, frames=args.frames, repeats=args.repeats, cases=rows), indent=2) + '\n')
